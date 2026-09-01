/* md3mesh.c -- Quake III .md3 loader
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * uhexen2-2ah9.  This tree carried the GPU half of MD3 support since
 * 2026-05-12 -- the PV_MD3 8-byte vertex format, its SSBO upload in
 * gl_mesh.c and the decode in the instanced vertex shader -- and nothing
 * that parsed a file, so the whole of it was unreached scaffolding.  This
 * is the parser.
 *
 * WHAT AN MD3 IS, AND WHAT AN aliashdr_t WANTS.  An .md3 is a set of
 * SURFACES, each with its own vertex array, its own triangle list indexing
 * that array, one (s,t) per vertex, and one vertex array per animation
 * frame.  An aliashdr_t is single-mesh and draws from a Quake GL COMMAND
 * LIST: a stream of (count, count x (float s, float t)) runs, with no vertex
 * indices in it at all.  GL_MakeAliasGPUMesh walks that stream and numbers
 * the vertices as it goes, so hdr->posedata has to be in exactly that order
 * -- see GL_MakeAliasModelDisplayLists, which reorders the .mdl's poses
 * through vertexorder[] for the same reason.
 *
 * So the surfaces are concatenated and every triangle is emitted as its own
 * 3-vertex run.  That costs vertex sharing: a mesh with N triangles gets 3N
 * pose vertices rather than its own vertex count.  Stripifying would recover
 * some of it, and the file has the connectivity to do it with, but a wrong
 * strip is a silently wrong mesh and this is the first code here to read an
 * .md3 at all -- so: correct first, and the size is reported under
 * `developer 1` for whoever wants to revisit it.
 */

#include "quakedef.h"
#include "gl_model.h"
#include "img_load.h"
#include "gl_shader.h"	/* gl_solid_white_texture, the missing-skin fallback */

#include <stdlib.h>
#include <string.h>

/* Hexen II's own limits, not Quake III's: MD3_MAX_* in gl_model.h are the
 * format's, and a file inside them can still be far past what an aliashdr_t
 * or the alias draw path will carry. */
#define MD3_LOAD_MAX_TRIS	(MD3_MAX_TRIANGLES * 4)

static void MD3_LoadSkin (aliashdr_t *hdr, const char *modelname, const char *shader);

/*
=================
MD3_ReadVertex

One on-disk md3Vertex_t into one in-memory md3Vertex_t.

The two are the same eight bytes and on a little-endian host this is a copy,
which is what the SSBO upload in gl_mesh.c assumes -- it hands hdr->posedata
straight to glBufferData and the shader reads the words back as the GPU sees
them.  Going through LittleShort anyway keeps the CPU draw path (which decodes
these itself) right on a big-endian host, where the GPU path was already
wrong for every pose format here.

The normal is a little-endian short, low byte longitude, high byte latitude
(Quake III tr_surface.c LerpMeshVertexes: lat = normal >> 8, lng = normal &
0xff).  Stored in that order, so normal[0] is longitude and normal[1] is
latitude, which is what both decoders here expect.
=================
*/
static void MD3_ReadVertex (const md3Vertex_t *in, md3Vertex_t *out)
{
	unsigned short	packed;
	int		i;

	for (i = 0; i < 3; i++)
		out->xyz[i] = (short) LittleShort (in->xyz[i]);

	packed = (unsigned short) (in->normal[0] | (in->normal[1] << 8));
	out->normal[0] = (unsigned char) (packed & 0xff);		/* longitude */
	out->normal[1] = (unsigned char) ((packed >> 8) & 0xff);	/* latitude */
}

/*
=================
MD3_DecodeNormal

The CPU-side twin of MD3_DecodeNormal in the instanced vertex shader.  Both
have to agree or a model lights differently depending on r_alias_gpu.
=================
*/
void MD3_DecodeNormal (const md3Vertex_t *v, vec3_t out)
{
	float	lng = (float) v->normal[0] * (float)(M_PI / 128.0);
	float	lat = (float) v->normal[1] * (float)(M_PI / 128.0);
	float	sinlng = sin (lng);

	out[0] = sinlng * cos (lat);
	out[1] = sinlng * sin (lat);
	out[2] = cos (lng);
}

/*
=================
MD3_LoadMesh

Parse `buffer` and return a hunk-allocated aliashdr_t, or NULL.  `out_mins` /
`out_maxs` receive the union of every frame's own bounding box, which is what
the format states and is strictly better than re-deriving it from the decoded
vertices: the file's boxes cover the frames the caller has not asked for yet.
=================
*/
aliashdr_t *MD3_LoadMesh (const char *name, const byte *buffer, int size,
			  vec3_t out_mins, vec3_t out_maxs)
{
	const md3Header_t	*hdr;
	const md3Surface_t	*surf;
	/* A copy, not a pointer into the file.  Every fixed-width name field in
	 * an .md3 is written by an exporter and read by us: nothing guarantees a
	 * NUL inside it, and q_strlcpy walks the tail of an over-long source to
	 * compute its return value, so handing it an unterminated char[64] reads
	 * off the end of the mapping. */
	char			shadername[sizeof(((md3Shader_t *)0)->name) + 1];
	aliashdr_t		*ahdr = NULL;
	int			numframes, numsurfaces;
	int			ofs_surfaces, ofs_frames;
	int			totaltris = 0, totalverts = 0;
	int			poseverts, numcommands;
	int			header_size, commands_off, posedata_off, hunksize;
	int			*cmds;
	md3Vertex_t		*poses;
	vec3_t			mins, maxs;
	int			i, f, s;

	shadername[0] = '\0';

	if (size < (int) sizeof(md3Header_t))
	{
		Con_DPrintf ("MD3_LoadMesh: %s is too small to be an .md3\n", name);
		return NULL;
	}

	hdr = (const md3Header_t *) buffer;
	if (LittleLong (hdr->ident) != MD3_IDENT)
	{
		Con_DPrintf ("MD3_LoadMesh: %s is not an .md3\n", name);
		return NULL;
	}
	if (LittleLong (hdr->version) != MD3_VERSION)
	{
		Con_Printf ("MD3_LoadMesh: %s is version %d, expected %d\n",
			    name, LittleLong (hdr->version), MD3_VERSION);
		return NULL;
	}

	numframes    = LittleLong (hdr->num_frames);
	numsurfaces  = LittleLong (hdr->num_surfaces);
	ofs_frames   = LittleLong (hdr->ofs_frames);
	ofs_surfaces = LittleLong (hdr->ofs_surfaces);

	if (numframes < 1 || numframes > MD3_MAX_FRAMES ||
	    numsurfaces < 1 || numsurfaces > MD3_MAX_SURFACES)
	{
		Con_Printf ("MD3_LoadMesh: %s has %d frames and %d surfaces, out of range\n",
			    name, numframes, numsurfaces);
		return NULL;
	}
	if (ofs_frames < 0 || ofs_surfaces < 0 ||
	    ofs_frames + numframes * (int) sizeof(md3Frame_t) > size ||
	    ofs_surfaces >= size)
	{
		Con_Printf ("MD3_LoadMesh: %s has offsets outside the file\n", name);
		return NULL;
	}

	/*
	 * Pass one: validate every surface and total up what the command list
	 * and the pose array will need.  Nothing is written until the whole
	 * file has been checked, so a truncated or hostile .md3 costs a
	 * message rather than a hunk allocation sized from its own bad numbers.
	 */
	surf = (const md3Surface_t *) (buffer + ofs_surfaces);
	for (s = 0; s < numsurfaces; s++)
	{
		int	sofs = (const byte *) surf - buffer;
		int	sframes, sverts, stris, snext;
		int	ofs_tris, ofs_st, ofs_verts, ofs_shaders, nshaders;

		if (sofs + (int) sizeof(md3Surface_t) > size)
		{
			Con_Printf ("MD3_LoadMesh: %s surface %d runs past the end\n", name, s);
			return NULL;
		}
		if (LittleLong (surf->ident) != MD3_IDENT)
		{
			Con_Printf ("MD3_LoadMesh: %s surface %d has a bad ident\n", name, s);
			return NULL;
		}

		sframes  = LittleLong (surf->num_frames);
		sverts   = LittleLong (surf->num_verts);
		stris    = LittleLong (surf->num_triangles);
		snext    = LittleLong (surf->ofs_end);
		ofs_tris = LittleLong (surf->ofs_triangles);
		ofs_st   = LittleLong (surf->ofs_st);
		ofs_verts = LittleLong (surf->ofs_verts);
		ofs_shaders = LittleLong (surf->ofs_shaders);
		nshaders = LittleLong (surf->num_shaders);

		/* Per-surface frame counts that disagree with the header's are
		 * the one inconsistency worth refusing outright: the pose array
		 * is indexed frame-major across all surfaces, so a short surface
		 * would have its later frames read from the next surface's. */
		if (sframes != numframes)
		{
			Con_Printf ("MD3_LoadMesh: %s surface %d has %d frames, header says %d\n",
				    name, s, sframes, numframes);
			return NULL;
		}
		if (sverts < 1 || sverts > MD3_MAX_VERTS ||
		    stris < 1 || stris > MD3_MAX_TRIANGLES)
		{
			Con_Printf ("MD3_LoadMesh: %s surface %d has %d verts and %d tris, out of range\n",
				    name, s, sverts, stris);
			return NULL;
		}
		if (snext < (int) sizeof(md3Surface_t) || sofs + snext > size ||
		    sofs + ofs_tris + stris * (int) sizeof(md3Triangle_t) > size ||
		    sofs + ofs_st + sverts * (int) sizeof(md3St_t) > size ||
		    sofs + ofs_verts + numframes * sverts * (int) sizeof(md3Vertex_t) > size)
		{
			Con_Printf ("MD3_LoadMesh: %s surface %d has offsets outside the file\n", name, s);
			return NULL;
		}

		/* First surface with a shader names the skin.  Multi-material
		 * models lose their later materials -- an aliashdr_t draws one
		 * skin per model and there is nowhere to put a second. */
		if (!shadername[0] && nshaders > 0 &&
		    sofs + ofs_shaders + (int) sizeof(md3Shader_t) <= size)
		{
			const md3Shader_t *sh = (const md3Shader_t *) ((const byte *) surf + ofs_shaders);

			memcpy (shadername, sh->name, sizeof(sh->name));
			shadername[sizeof(sh->name)] = '\0';
		}

		totalverts += sverts;
		totaltris  += stris;
		surf = (const md3Surface_t *) ((const byte *) surf + snext);
	}

	if (totaltris > MD3_LOAD_MAX_TRIS)
	{
		Con_Printf ("MD3_LoadMesh: %s has %d triangles, more than this engine carries (%d)\n",
			    name, totaltris, MD3_LOAD_MAX_TRIS);
		return NULL;
	}

	poseverts   = totaltris * 3;
	numcommands = totaltris * 7 + 1;	/* per tri: count + 3 x (s,t); then the terminator */

	/* 65535 is the index buffer's ceiling: GL_MakeAliasGPUMesh emits
	 * unsigned short indices and numbers vertices in command-list order,
	 * so it is poseverts and not the file's vertex count that has to fit. */
	if (poseverts > 65535)
	{
		Con_Printf ("MD3_LoadMesh: %s needs %d pose vertices, past the 65535 the index buffer holds\n",
			    name, poseverts);
		return NULL;
	}

	header_size  = sizeof(aliashdr_t) + (numframes - 1) * (int) sizeof(maliasframedesc_t);
	commands_off = header_size;
	posedata_off = commands_off + numcommands * (int) sizeof(int);
	hunksize     = posedata_off + numframes * poseverts * (int) sizeof(md3Vertex_t);

	ahdr = (aliashdr_t *) Hunk_Alloc (hunksize);
	if (!ahdr)
		return NULL;

	memset (ahdr, 0, header_size);
	ahdr->ident        = ALIAS_IDENT;
	ahdr->version      = ALIAS_VERSION_H2;
	ahdr->numverts     = poseverts;
	ahdr->numtris      = totaltris;
	ahdr->numframes    = numframes;
	ahdr->numposes     = numframes;
	ahdr->poseverts    = poseverts;
	ahdr->poseverttype = PV_MD3;
	ahdr->commands     = commands_off;
	ahdr->posedata     = posedata_off;
	ahdr->synctype     = ST_SYNC;

	/* MD3 positions decode straight to model units (xyz / 64), so the
	 * scale/scale_origin the draw path folds into every alias model's
	 * matrix has to be the identity.  memset leaves an all-zero scale,
	 * which would collapse the mesh to a point. */
	VectorSet (ahdr->scale, 1.0f, 1.0f, 1.0f);
	VectorSet (ahdr->scale_origin, 0.0f, 0.0f, 0.0f);

	/* One MD3 frame per aliashdr_t frame, one pose each, so a frame number
	 * from QuakeC indexes this exactly as it would a .mdl and
	 * R_AliasResolveLerp's single-pose blending applies unchanged. */
	for (f = 0; f < numframes; f++)
	{
		const md3Frame_t *fr = (const md3Frame_t *) (buffer + ofs_frames) + f;

		ahdr->frames[f].firstpose = f;
		ahdr->frames[f].numposes  = 1;
		ahdr->frames[f].interval  = 0.1f;	/* .mdl default; QuakeC drives the rate */
		ahdr->frames[f].frame     = f;
		/* Bounded copy for the same reason shadername is one: md3Frame_t's
		 * name is a fixed char[16] with no promise of a NUL in it. */
		memcpy (ahdr->frames[f].name, fr->name,
			q_min (sizeof(ahdr->frames[f].name) - 1, sizeof(fr->name)));
		ahdr->frames[f].name[sizeof(ahdr->frames[f].name) - 1] = '\0';
		if (!ahdr->frames[f].name[0])
			q_snprintf (ahdr->frames[f].name, sizeof(ahdr->frames[f].name), "frame%d", f);
	}

	cmds  = (int *) ((byte *) ahdr + commands_off);
	poses = (md3Vertex_t *) ((byte *) ahdr + posedata_off);

	/*
	 * Pass two: emit.  The vertex counter `i` runs across all surfaces and
	 * is the index the command list implies, so the pose array is filled at
	 * the same cursor -- that identity is the whole contract with
	 * GL_MakeAliasGPUMesh.
	 */
	i = 0;
	surf = (const md3Surface_t *) (buffer + ofs_surfaces);
	for (s = 0; s < numsurfaces; s++)
	{
		const byte		*sbase = (const byte *) surf;
		int			sverts = LittleLong (surf->num_verts);
		int			stris  = LittleLong (surf->num_triangles);
		const md3Triangle_t	*tris = (const md3Triangle_t *) (sbase + LittleLong (surf->ofs_triangles));
		const md3St_t		*st   = (const md3St_t *)       (sbase + LittleLong (surf->ofs_st));
		const md3Vertex_t	*vtx  = (const md3Vertex_t *)   (sbase + LittleLong (surf->ofs_verts));
		int			t, c;

		for (t = 0; t < stris; t++)
		{
			*cmds++ = 3;	/* positive: a three-vertex strip, i.e. one triangle */

			for (c = 0; c < 3; c++)
			{
				int	vi = LittleLong (tris[t].indexes[c]);
				float	sc, tc;

				/* An out-of-range index would read another
				 * surface's vertices, or off the end.  Clamp
				 * rather than refuse: the rest of the model is
				 * still worth drawing, and a degenerate triangle
				 * is a far smaller lie than a wrong one. */
				if (vi < 0 || vi >= sverts)
					vi = 0;

				sc = LittleFloat (st[vi].s);
				tc = LittleFloat (st[vi].t);
				memcpy (cmds++, &sc, 4);
				memcpy (cmds++, &tc, 4);

				for (f = 0; f < numframes; f++)
					MD3_ReadVertex (&vtx[f * sverts + vi],
							&poses[f * poseverts + i]);
				i++;
			}
		}

		surf = (const md3Surface_t *) (sbase + LittleLong (surf->ofs_end));
	}
	*cmds++ = 0;	/* end of command list */

	/* Union of the frames' own boxes.  MD3 stores these in model units
	 * already, so they need no scaling. */
	{
		const md3Frame_t *fr = (const md3Frame_t *) (buffer + ofs_frames);

		for (i = 0; i < 3; i++)
		{
			mins[i] = LittleFloat (fr[0].bounds[0][i]);
			maxs[i] = LittleFloat (fr[0].bounds[1][i]);
		}
		for (f = 1; f < numframes; f++)
		{
			for (i = 0; i < 3; i++)
			{
				float lo = LittleFloat (fr[f].bounds[0][i]);
				float hi = LittleFloat (fr[f].bounds[1][i]);
				if (lo < mins[i]) mins[i] = lo;
				if (hi > maxs[i]) maxs[i] = hi;
			}
		}
	}
	if (out_mins && out_maxs)
	{
		VectorCopy (mins, out_mins);
		VectorCopy (maxs, out_maxs);
	}

	MD3_LoadSkin (ahdr, name, shadername);

	Con_DPrintf ("MD3_LoadMesh: loaded %s (%d surfaces, %d verts, %d tris, %d frames, "
		     "%d KB of poses)\n",
		     name, numsurfaces, totalverts, totaltris, numframes,
		     (numframes * poseverts * (int) sizeof(md3Vertex_t)) >> 10);
	return ahdr;
}

/*
=================
MD3_LoadSkin

MD5_LoadSkin in md5mesh.c is the sibling of this and deliberately not shared:
an .md5mesh names its material and nothing else, while an .md3 has a second
place to look -- the model's own path, which is where a converted model's
texture usually sits when the shader field carries a Quake III material name
this engine has no shader system to resolve.
=================
*/
static void MD3_StripExtension (char *s)
{
	char	*dot = strrchr (s, '.');
	char	*slash = strrchr (s, '/');

	/* Only a real extension: a dot in a directory name is not one, and
	 * IMG_LoadExternalTexture appends its own candidates -- leaving one on
	 * asks for "skin.tga.tga" and gets no hits. */
	if (dot && (!slash || dot > slash))
		*dot = '\0';
}

static void MD3_LoadSkin (aliashdr_t *hdr, const char *modelname, const char *shader)
{
	char		path[MAX_QPATH];
	byte		*rgba = NULL;
	int		w = 0, h = 0, i;
	qboolean	has_alpha = false;
	GLuint		tex;

	if (shader && shader[0])
	{
		q_strlcpy (path, shader, sizeof(path));
		MD3_StripExtension (path);
		rgba = IMG_LoadExternalTexture (path, &w, &h, &has_alpha);
	}

	if (!rgba)
	{
		q_strlcpy (path, modelname, sizeof(path));
		MD3_StripExtension (path);
		rgba = IMG_LoadExternalTexture (path, &w, &h, &has_alpha);
	}

	if (rgba)
	{
		tex = GL_LoadTexture (path, rgba, w, h,
				      TEX_MIPMAP | TEX_RGBA | (has_alpha ? TEX_ALPHA : 0));
		free (rgba);
	}
	else
	{
		/* The white sentinel rather than numskins 0: the draw path binds
		 * gl_texturenum[skin][anim] unconditionally and a zero there is
		 * texture object 0, which a core profile leaves undefined -- so a
		 * missing skin would read as black or as garbage instead of as an
		 * obviously untextured model. */
		Con_DPrintf ("MD3: no skin image for %s (shader \"%s\")\n",
			     modelname, shader ? shader : "");
		tex = gl_solid_white_texture;
	}

	if (!tex)
		return;

	/* The same texture in all four animation slots: the draw path indexes
	 * gl_texturenum[skin][(int)(cl.time*10)&3] unconditionally, so leaving
	 * 1..3 at zero would strobe the model untextured three frames in four. */
	hdr->numskins = 1;
	for (i = 0; i < 4; i++)
		hdr->gl_texturenum[0][i] = tex;
}
