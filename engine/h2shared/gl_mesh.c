/*
 * gl_mesh.c -- triangle model functions
 * Copyright (C) 1996-1997  Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "gl_shader.h"
#include "gl_vbo.h"

void GL_MakeAliasGPUMesh (aliashdr_t *hdr);

/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

/* Sized from MAXALIASTRIS, which the loaders enforce, so no model can overrun
 * them.  They were 8192 / 128 / 1024 with no bounds checks: a 2048-triangle
 * model that strips badly emits 7 command ints per triangle (14337 > 8192),
 * and any strip or fan past 126 triangles overran stripverts[128].  GitHub
 * #174 (e). */
static int		used[MAXALIASTRIS];

// the command list holds counts and s/t values that are valid for
// every frame: per strip a count and two s/t ints per vertex, worst case a
// strip per triangle (1 + 2*3), plus the terminator
static int		commands[MAXALIASTRIS * 7 + 1];
static int		numcommands;

// all frames will have their vertexes rearranged and expanded
// so they are in the order expected by the command list
static int		vertexorder[MAXALIASTRIS * 3];
static int		numorder;

static int		stripverts[MAX_ALIAS_STRIP_VERTS];
static int		striptris[MAXALIASTRIS];
static int		stripstverts[MAX_ALIAS_STRIP_VERTS];
static int		stripcount;

/*
================
StripLength
================
*/
static int StripLength (int starttri, int startv)
{
	int			m1, m2;
	int			st1, st2;
	int			j;
	mtriangle_t	*last, *check;
	int			k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv)%3];
	stripstverts[0] = last->stindex[(startv)%3];

	stripverts[1] = last->vertindex[(startv+1)%3];
	stripstverts[1] = last->stindex[(startv+1)%3];

	stripverts[2] = last->vertindex[(startv+2)%3];
	stripstverts[2] = last->stindex[(startv+2)%3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv+2)%3];
	st1 = last->stindex[(startv+2)%3];
	m2 = last->vertindex[(startv+1)%3];
	st2 = last->stindex[(startv+1)%3];

	// look for a matching triangle
nexttri:
	for (j = starttri+1, check = &triangles[starttri+1]; j < pheader->numtris; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k = 0; k < 3; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->stindex[k] != st1)
				continue;
			if (check->vertindex[ (k+1)%3 ] != m2)
				continue;
			if (check->stindex[ (k+1)%3 ] != st2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			if (stripcount & 1)
			{
				m2 = check->vertindex[ (k+2)%3 ];
				st2 = check->stindex[ (k+2)%3 ];
			}
			else
			{
				m1 = check->vertindex[ (k+2)%3 ];
				st1 = check->stindex[ (k+2)%3 ];
			}

			stripverts[stripcount+2] = check->vertindex[ (k+2)%3 ];
			stripstverts[stripcount+2] = check->stindex[ (k+2)%3 ];
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}

done:
	// clear the temp used flags
	for (j = starttri+1; j < pheader->numtris; j++)
	{
		if (used[j] == 2)
			used[j] = 0;
	}

	return stripcount;
}

/*
===========
FanLength
===========
*/
static int FanLength (int starttri, int startv)
{
	int		m1, m2;
	int		st1, st2;
	int		j;
	mtriangle_t	*last, *check;
	int		k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv)%3];
	stripstverts[0] = last->stindex[(startv)%3];

	stripverts[1] = last->vertindex[(startv+1)%3];
	stripstverts[1] = last->stindex[(startv+1)%3];

	stripverts[2] = last->vertindex[(startv+2)%3];
	stripstverts[2] = last->stindex[(startv+2)%3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv+0)%3];
	st1 = last->stindex[(startv+2)%3];
	m2 = last->vertindex[(startv+2)%3];
	st2 = last->stindex[(startv+1)%3];

	// look for a matching triangle
nexttri:
	for (j = starttri+1, check = &triangles[starttri+1]; j < pheader->numtris; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k = 0; k < 3; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->stindex[k] != st1)
				continue;
			if (check->vertindex[ (k+1)%3 ] != m2)
				continue;
			if (check->stindex[ (k+1)%3 ] != st2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			m2 = check->vertindex[ (k+2)%3 ];
			st2 = check->stindex[ (k+2)%3 ];

			stripverts[stripcount+2] = m2;
			stripstverts[stripcount+2] = st2;
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}

done:
	// clear the temp used flags
	for (j = starttri+1; j < pheader->numtris; j++)
	{
		if (used[j] == 2)
			used[j] = 0;
	}

	return stripcount;
}


/*
================
BuildTris

Generate a list of trifans or strips
for the model, which holds for all frames
================
*/
static void BuildTris (void)
{
	int		i, j, k;
	int		startv;
	float	s, t;
	int		len, bestlen, besttype;
	static int	bestverts[MAX_ALIAS_STRIP_VERTS];	/* GitHub #174 (e) */
	static int	besttris[MAXALIASTRIS];
	static int	beststverts[MAX_ALIAS_STRIP_VERTS];
	int		type;

	//
	// build tristrips
	//
	numorder = 0;
	numcommands = 0;
	memset (used, 0, sizeof(used));
	for (i = 0; i < pheader->numtris; i++)
	{
		// pick an unused triangle and start the trifan
		if (used[i])
			continue;

		bestlen = 0;
		besttype = 0;
		for (type = 0 ; type < 2 ; type++)
		{
			for (startv = 0; startv < 3; startv++)
			{
				if (type == 1)
					len = StripLength (i, startv);
				else
					len = FanLength (i, startv);
				if (len > bestlen)
				{
					besttype = type;
					bestlen = len;
					for (j = 0; j < bestlen+2; j++)
					{
						beststverts[j] = stripstverts[j];
						bestverts[j] = stripverts[j];
					}
					for (j = 0; j < bestlen; j++)
						besttris[j] = striptris[j];
				}
			}
		}

		// mark the tris on the best strip as used
		for (j = 0; j < bestlen; j++)
			used[besttris[j]] = 1;

		if (besttype == 1)
			commands[numcommands++] = (bestlen+2);
		else
			commands[numcommands++] = -(bestlen+2);

		for (j = 0; j < bestlen+2; j++)
		{
			int		tmp;

			// emit a vertex into the reorder buffer
			k = bestverts[j];
			vertexorder[numorder++] = k;

			k = beststverts[j];

			// emit s/t coords into the commands stream
			s = stverts[k].s;
			t = stverts[k].t;

			if (!triangles[besttris[0]].facesfront && stverts[k].onseam)
				s += pheader->skinwidth / 2;	// on back side
			s = (s + 0.5) / pheader->skinwidth;
			t = (t + 0.5) / pheader->skinheight;

		//	*(float *)&commands[numcommands++] = s;
		//	*(float *)&commands[numcommands++] = t;
			// NOTE: 4 == sizeof(int)
			//	   == sizeof(float)
			memcpy (&tmp, &s, 4);
			commands[numcommands++] = tmp;
			memcpy (&tmp, &t, 4);
			commands[numcommands++] = tmp;
		}
	}

	commands[numcommands++] = 0;		// end of list marker
	DEBUG_Printf ("%3i tri %3i vert %3i cmd\n", pheader->numtris, numorder, numcommands);
}


/*
================
GL_MakeAliasModelDisplayLists
================
*/
void GL_MakeAliasModelDisplayLists (qmodel_t *m, aliashdr_t *hdr)
{
	int		i, j;
	int		*cmds;
	trivertx_t	*verts;

	DEBUG_Printf ("meshing %s...\n", m->name);
	BuildTris ();		// trifans or lists

	hdr->poseverts = numorder;

	cmds = (int *) Hunk_AllocName (numcommands * 4, "cmds");
	hdr->commands = (byte *)cmds - (byte *)hdr;
	memcpy (cmds, commands, numcommands * 4);

	verts = (trivertx_t *) Hunk_AllocName (hdr->numposes * hdr->poseverts * sizeof(trivertx_t), "verts");
	hdr->posedata = (byte *)verts - (byte *)hdr;
	for (i = 0; i < hdr->numposes; i++)
	{
		for (j = 0; j < numorder; j++)
			*verts++ = poseverts[i][vertexorder[j]];
	}

	/* Build GPU-resident data for AZDO rendering */
	GL_MakeAliasGPUMesh(hdr);
}

/* ------------------------------------------------------------------ */
/* GPU-resident alias model data (AZDO Phase 1)                        */
/* ------------------------------------------------------------------ */

alias_gpu_mesh_t alias_gpu_meshes[MAX_ALIAS_MODELS];
int num_alias_gpu_meshes;

/* The table's generation.  A header's gpu_mesh is only trusted when its
 * gpu_mesh_gen matches; GL_FreeAliasGPUMeshes bumps this, which retires every
 * cached header's slot at once.  Starts at 1 so a zero-filled header (the
 * hunk zeroes what it hands out) is never mistaken for a current one. */
static int alias_gpu_generation = 1;

/* Map an aliashdr_t to its GPU mesh, through the slot its header carries.
 * The mesh used to be found by comparing header ADDRESSES against the one
 * GL_MakeAliasGPUMesh saw -- which is the loader's hunk copy, freed before
 * the first draw.  Every model then missed, built a second mesh from the draw
 * path, and a later header landing on a dead address was handed another
 * model's mesh: its index count and pose size, read against this model's
 * draw -- the "orphaned header" GitHub #134 caught in gdb. */
alias_gpu_mesh_t *GL_GetAliasGPUMesh (aliashdr_t *hdr)
{
	if (hdr->gpu_mesh_gen != alias_gpu_generation)
	{
		/* No mesh in this generation: build one (after Mod_ClearAll or a
		 * mesh-affecting cvar freed the table, the model stays cached). */
		GL_MakeAliasGPUMesh(hdr);
	}
	if (hdr->gpu_mesh_gen == alias_gpu_generation &&
	    hdr->gpu_mesh >= 0 && hdr->gpu_mesh < num_alias_gpu_meshes &&
	    alias_gpu_meshes[hdr->gpu_mesh].valid)
		return &alias_gpu_meshes[hdr->gpu_mesh];
	return NULL;
}

static void GL_FreeAliasGPUMesh (alias_gpu_mesh_t *gm)
{
	if (gm->vao)           { glDeleteVertexArrays_fp(1, &gm->vao); }
	if (gm->vbo_verts)     { glDeleteBuffers_fp(1, &gm->vbo_verts); }
	if (gm->vbo_tc)        { glDeleteBuffers_fp(1, &gm->vbo_tc); }
	if (gm->ibo)           { glDeleteBuffers_fp(1, &gm->ibo); }
	/* SSBOs sit in the indexed-binding cache; forget the name before
	 * GL recycles it into a different buffer.  uhexen2 #203. */
	if (gm->ssbo_pose)     { GL_ForgetBuffer(gm->ssbo_pose);     glDeleteBuffers_fp(1, &gm->ssbo_pose); }
	if (gm->ssbo_pose_md3) { GL_ForgetBuffer(gm->ssbo_pose_md3); glDeleteBuffers_fp(1, &gm->ssbo_pose_md3); }
	if (gm->ssbo_bones)    { GL_ForgetBuffer(gm->ssbo_bones);    glDeleteBuffers_fp(1, &gm->ssbo_bones); }
	if (gm->tex_pose)      { glDeleteTextures_fp(1, &gm->tex_pose); }
	memset(gm, 0, sizeof(*gm));
}

void GL_FreeAliasGPUMeshes (void)
{
	int i;
	for (i = 0; i < num_alias_gpu_meshes; i++)
		GL_FreeAliasGPUMesh(&alias_gpu_meshes[i]);
	num_alias_gpu_meshes = 0;
	alias_gpu_generation++;
}

/* The GL context is going away (vid_restart).  Its names die with it, and the
 * new context hands the same low numbers out again, so nothing is deleted:
 * deleting later would hit whatever the new context gave those names to.
 * The table is emptied and the generation bumped, so every header -- cached
 * ones reloaded by Mod_ReloadTextures, and hunk-resident MD3/MD5/IQM ones it
 * skips -- builds a fresh mesh in the new context.  GitHub #387. */
void GL_AliasGPUMeshes_ContextLost (void)
{
	memset(alias_gpu_meshes, 0, sizeof(alias_gpu_meshes));
	num_alias_gpu_meshes = 0;
	alias_gpu_generation++;
}

/* A model's previous mesh, released when the model reloads after the cache
 * evicted it: the evicted header is gone, so nothing can reach that slot any
 * more, and without this every eviction leaked one until the next
 * generation.  A slot from an older generation is already gone.  GitHub #387. */
void GL_ReleaseAliasGPUMesh (int slot, int gen)
{
	if (gen != alias_gpu_generation || slot < 0 || slot >= num_alias_gpu_meshes)
		return;
	GL_FreeAliasGPUMesh (&alias_gpu_meshes[slot]);	/* memsets: now empty */
}

/* The slot a new mesh goes into: an emptied one if there is one, else the
 * next past the end.  -1 when the table is full.  GitHub #387. */
static int GL_AllocAliasGPUSlot (void)
{
	int	i;

	for (i = 0; i < num_alias_gpu_meshes; i++)
		if (!alias_gpu_meshes[i].valid && !alias_gpu_meshes[i].vao)
			return i;
	return (num_alias_gpu_meshes < MAX_ALIAS_MODELS) ? num_alias_gpu_meshes : -1;
}

/* A refused mesh: free what was made for it, and mark the header so the draw
 * path does not rebuild (and leak) it every frame. */
static void GL_RefuseAliasGPUMesh (aliashdr_t *hdr, alias_gpu_mesh_t *gm)
{
	GL_FreeAliasGPUMesh(gm);
	hdr->gpu_mesh = -1;
	hdr->gpu_mesh_gen = alias_gpu_generation;
}

/* Scratch for GL_MakeAliasGPUMesh, from the C heap rather than the hunk.
 * The builder also runs from the draw path (GL_GetAliasGPUMesh, after
 * Mod_ClearAll or vid_restart retired the table while models stayed cached),
 * and a hunk allocation there can call Cache_FreeLow -- evicting, and so
 * freeing, the cache block that holds the very header being read.  GitHub
 * #387.  Zero-filled, as Hunk_AllocName was. */
#define MESH_SCRATCH_MAX	4
static void	*mesh_scratch[MESH_SCRATCH_MAX];
static int	mesh_scratch_count;

static void *GL_MeshScratch (size_t size)
{
	void *p;

	if (mesh_scratch_count >= MESH_SCRATCH_MAX)
		Sys_Error ("GL_MeshScratch: more than %d scratch blocks", MESH_SCRATCH_MAX);
	p = calloc (1, size ? size : 1);
	if (!p)
		Sys_Error ("GL_MeshScratch: failed on %lu bytes", (unsigned long) size);
	mesh_scratch[mesh_scratch_count++] = p;
	return p;
}

static void GL_MeshScratchFree (void)
{
	while (mesh_scratch_count > 0)
		free (mesh_scratch[--mesh_scratch_count]);
}

/*
================
GL_MakeAliasGPUMesh

Triangulate the command list, extract static texcoords, and upload
all pose vertex data to an SSBO.  Called from GL_MakeAliasModelDisplayLists.
================
*/
void GL_MakeAliasGPUMesh (aliashdr_t *hdr)
{
	int		*order, count;
	int		num_tris, vi, vert_idx;
	unsigned short	*indices;
	float		*texcoords;
	unsigned int	*pose_packed;
	trivertx_t	*pv;
	iqmvert_t	*iqmv;
	alias_gpu_mesh_t *gm;
	int		i, j, slot;

	slot = GL_AllocAliasGPUSlot ();
	if (slot < 0)
	{
		hdr->gpu_mesh = -1;		/* full: refused for this generation */
		hdr->gpu_mesh_gen = alias_gpu_generation;
		return;
	}

	/* For skeletal (PV_IQM) models, create simple vertex order */
	if (hdr->poseverttype == PV_IQM)
	{
		num_tris = hdr->numtris;
		indices = (unsigned short *) GL_MeshScratch(num_tris * 3 * sizeof(unsigned short));
		texcoords = (float *) GL_MeshScratch(hdr->poseverts * 2 * sizeof(float));
		iqmv = (iqmvert_t *)((byte *)hdr + hdr->posedata);

		/* Extract texture coordinates */
		for (i = 0; i < hdr->poseverts; i++)
		{
			texcoords[i*2 + 0] = iqmv[i].st[0];
			texcoords[i*2 + 1] = iqmv[i].st[1];
		}

		/* Build IBO from the persisted triangle indices (uhexen2-byu7+ssct). */
		{
			const unsigned short *src = (const unsigned short *)((byte *)hdr + hdr->triangledata);
			for (i = 0; i < num_tris * 3; i++)
				indices[i] = src[i];
		}
		vi = num_tris * 3;

		goto iqm_gpu_setup;
	}

	/* First pass: count triangles from command list */
	order = (int *)((byte *)hdr + hdr->commands);
	num_tris = 0;
	while (1)
	{
		count = *order++;
		if (!count)
			break;
		if (count < 0)
			count = -count;
		order += count * 2; /* skip s/t pairs */
		num_tris += count - 2;
	}

	if (num_tris <= 0)
	{
		hdr->gpu_mesh = -1;		/* nothing to draw: refused, see above */
		hdr->gpu_mesh_gen = alias_gpu_generation;
		GL_MeshScratchFree();	/* none held on this path; kept for any reorder */
		return;
	}

	indices = (unsigned short *) GL_MeshScratch(num_tris * 3 * sizeof(unsigned short));
	texcoords = (float *) GL_MeshScratch(hdr->poseverts * 2 * sizeof(float));

	/* Second pass: triangulate and extract texcoords */
	order = (int *)((byte *)hdr + hdr->commands);
	vert_idx = 0;
	vi = 0; /* index into indices array */

	while (1)
	{
		qboolean is_fan;
		int first_vert, prev_vert;

		count = *order++;
		if (!count)
			break;
		if (count < 0)
		{
			count = -count;
			is_fan = true;
		}
		else
			is_fan = false;

		first_vert = vert_idx;
		for (i = 0; i < count; i++)
		{
			texcoords[(vert_idx + i) * 2 + 0] = ((float *)order)[0];
			texcoords[(vert_idx + i) * 2 + 1] = ((float *)order)[1];
			order += 2;
		}

		/* Convert to triangles */
		if (is_fan)
		{
			for (i = 2; i < count; i++)
			{
				indices[vi++] = (unsigned short)first_vert;
				indices[vi++] = (unsigned short)(first_vert + i - 1);
				indices[vi++] = (unsigned short)(first_vert + i);
			}
		}
		else
		{
			for (i = 2; i < count; i++)
			{
				if (i & 1)
				{
					indices[vi++] = (unsigned short)(first_vert + i);
					indices[vi++] = (unsigned short)(first_vert + i - 1);
					indices[vi++] = (unsigned short)(first_vert + i - 2);
				}
				else
				{
					indices[vi++] = (unsigned short)(first_vert + i - 2);
					indices[vi++] = (unsigned short)(first_vert + i - 1);
					indices[vi++] = (unsigned short)(first_vert + i);
				}
			}
		}

		vert_idx += count;
	}

	/* Pack all pose vertex data: format depends on poseverttype */
	if (hdr->poseverttype == PV_MD3)
	{
		/* MD3: 8-byte format (uint16_t xyz[3] + uint8_t normal[2]) — no packing needed */
		/* TODO: Phase 3 will handle MD3 vertex upload to separate SSBO */
		pose_packed = NULL;
	}
	else
	{
		/* PV_QUAKE1: trivertx_t → uint (v[0] | v[1]<<8 | v[2]<<16 | ni<<24) */
		pose_packed = (unsigned int *) GL_MeshScratch(hdr->numposes * hdr->poseverts * sizeof(unsigned int));
		pv = (trivertx_t *)((byte *)hdr + hdr->posedata);
		for (i = 0; i < hdr->numposes * hdr->poseverts; i++)
		{
			pose_packed[i] = (unsigned int)pv[i].v[0]
				       | ((unsigned int)pv[i].v[1] << 8)
				       | ((unsigned int)pv[i].v[2] << 16)
				       | ((unsigned int)pv[i].lightnormalindex << 24);
		}
	}

	/* Create GPU objects */
	gm = &alias_gpu_meshes[slot];
	memset(gm, 0, sizeof(*gm));
	gm->poseverttype = hdr->poseverttype;

	glGenVertexArrays_fp(1, &gm->vao);
	glBindVertexArray_fp(gm->vao);

	/* Texcoord VBO — bound to ATTR_TEXCOORD (location 1) */
	glGenBuffers_fp(1, &gm->vbo_tc);
	glBindBuffer_fp(GL_ARRAY_BUFFER, gm->vbo_tc);
	glBufferData_fp(GL_ARRAY_BUFFER, hdr->poseverts * 2 * sizeof(float),
			texcoords, GL_STATIC_DRAW);
	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribPointer_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	/* Index buffer */
	glGenBuffers_fp(1, &gm->ibo);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, gm->ibo);
	glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER, vi * sizeof(unsigned short),
			indices, GL_STATIC_DRAW);

	glBindVertexArray_fp(0);

	/* Pose data upload: format-dependent path */
	if (hdr->poseverttype == PV_IQM)
	{
iqm_gpu_setup:
		/* Skeletal setup: upload iqmvert_t data + bone pose matrices */
		gm = &alias_gpu_meshes[slot];
		memset(gm, 0, sizeof(*gm));
		gm->poseverttype = PV_IQM;
		gm->numbones = hdr->numbones;

		glGenVertexArrays_fp(1, &gm->vao);
		glBindVertexArray_fp(gm->vao);

		/* Upload vertex data (iqmvert_t: 32 bytes per vertex) */
#ifndef USE_GLES
		glGenBuffers_fp(1, &gm->vbo_verts);
		glBindBuffer_fp(GL_ARRAY_BUFFER, gm->vbo_verts);
		glBufferData_fp(GL_ARRAY_BUFFER, hdr->poseverts * sizeof(iqmvert_t),
				(byte *)hdr + hdr->posedata, GL_STATIC_DRAW);

		/* Vertex attributes for skeletal format */
		glEnableVertexAttribArray_fp(0); /* position */
		glVertexAttribPointer_fp(0, 3, GL_FLOAT, GL_FALSE, sizeof(iqmvert_t), (void *)0);

		glEnableVertexAttribArray_fp(1); /* normal */
		glVertexAttribPointer_fp(1, 4, GL_BYTE, GL_TRUE, sizeof(iqmvert_t), (void *)12);

		glEnableVertexAttribArray_fp(2); /* texcoord */
		glVertexAttribPointer_fp(2, 2, GL_FLOAT, GL_FALSE, sizeof(iqmvert_t), (void *)16);

		glEnableVertexAttribArray_fp(3); /* bone weights */
		glVertexAttribPointer_fp(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(iqmvert_t), (void *)24);

		glEnableVertexAttribArray_fp(4); /* bone indices */
		/* uvec4 in the skeletal shader: must use the integer path, or the
		 * indices arrive int-to-float converted and select the wrong bones. */
		glVertexAttribIPointer_fp(4, 4, GL_UNSIGNED_BYTE, sizeof(iqmvert_t), (void *)28);

		/* Index buffer */
		glGenBuffers_fp(1, &gm->ibo);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, gm->ibo);
		glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER, vi * sizeof(unsigned short),
				indices, GL_STATIC_DRAW);

		glBindVertexArray_fp(0);

		/* Upload bone pose matrices as SSBO */
		glGenBuffers_fp(1, &gm->ssbo_bones);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, gm->ssbo_bones);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				hdr->numposes * hdr->numbones * sizeof(bonepose_t),
				(byte *)hdr + hdr->boneposedata, GL_DYNAMIC_DRAW);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
#endif

	/* Refuse a mesh with no pose data.  Everything downstream indexes the
	 * pose SSBO as pose*poseverts + vertex, so numposes or poseverts of 0
	 * means a zero-byte buffer that every vertex then reads past.  Nothing
	 * else catches it: glBufferData with size 0 still returns a live buffer
	 * name, so the "if (!pose_ssbo) continue" guard in R_DrawAliasInstanced
	 * sees a perfectly good buffer and draws.  The read is out of bounds and
	 * therefore undefined -- llvmpipe absorbs it, the NVIDIA driver
	 * segfaults inside its own vertex fetch with a stack naming only the
	 * draw call.
	 *
	 * Checked here rather than on entry because hdr->poseverts is filled in
	 * by the strip builder above, not by the model loader.  Leaving valid
	 * clear covers every consumer at once: GL_GetAliasGPUMesh only hands
	 * back meshes with valid set, so the model draws as nothing instead of
	 * taking the process down.  uhexen2-5krx */
		if (hdr->numposes <= 0 || hdr->poseverts <= 0)
		{
			Con_DPrintf ("GL_MakeAliasGPUMesh: no pose data (numposes=%d poseverts=%d), model will not be drawn\n",
						hdr->numposes, hdr->poseverts);
			GL_RefuseAliasGPUMesh(hdr, gm);
			GL_MeshScratchFree();
			return;
		}

		gm->num_indices = vi;
		gm->poseverts = hdr->poseverts;
		gm->numposes = hdr->numposes;
		gm->valid = true;

		hdr->gpu_mesh = slot;
		hdr->gpu_mesh_gen = alias_gpu_generation;
		if (slot == num_alias_gpu_meshes)
			num_alias_gpu_meshes++;

		if (hdr->poseverttype == PV_IQM)
		{
			GL_MeshScratchFree();
			return;
		}
	}
	else if (hdr->poseverttype == PV_MD3)
	{
		/* TODO: Phase 3 MD3 SSBO upload
		 * Upload md3Vertex_t data to separate SSBO (8 bytes per vertex)
		 * MD3 data is in hdr->posedata, packs as short[3] + ubyte[2]
		 */
#ifndef USE_GLES
		glGenBuffers_fp(1, &gm->ssbo_pose_md3);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, gm->ssbo_pose_md3);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				hdr->numposes * hdr->poseverts * sizeof(md3Vertex_t),
				(byte *)hdr + hdr->posedata, GL_STATIC_DRAW);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
#endif
	}
	else
	{
		/* PV_QUAKE1: standard path */
#ifndef USE_GLES
		glGenBuffers_fp(1, &gm->ssbo_pose);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, gm->ssbo_pose);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				hdr->numposes * hdr->poseverts * sizeof(unsigned int),
				pose_packed, GL_STATIC_DRAW);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
#endif

		/* Pose texture (ES 3.0 compatible) — R32UI, width=poseverts, height=numposes.
		 * Each texel packs one trivertx_t as: v[0] | v[1]<<8 | v[2]<<16 | ni<<24 */
		glGenTextures_fp(1, &gm->tex_pose);
		glBindTexture_fp(GL_TEXTURE_2D, gm->tex_pose);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_R32UI,
				hdr->poseverts, hdr->numposes, 0,
				GL_RED_INTEGER, GL_UNSIGNED_INT, pose_packed);
		glBindTexture_fp(GL_TEXTURE_2D, 0);
	}

	/* Refuse a mesh with no pose data.  Everything downstream indexes the
	 * pose SSBO as pose*poseverts + vertex, so numposes or poseverts of 0
	 * means a zero-byte buffer that every vertex then reads past.  Nothing
	 * else catches it: glBufferData with size 0 still returns a live buffer
	 * name, so the "if (!pose_ssbo) continue" guard in R_DrawAliasInstanced
	 * sees a perfectly good buffer and draws.  The read is out of bounds and
	 * therefore undefined -- llvmpipe absorbs it, the NVIDIA driver
	 * segfaults inside its own vertex fetch with a stack naming only the
	 * draw call.
	 *
	 * Checked here rather than on entry because hdr->poseverts is filled in
	 * by the strip builder above, not by the model loader.  Leaving valid
	 * clear covers every consumer at once: GL_GetAliasGPUMesh only hands
	 * back meshes with valid set, so the model draws as nothing instead of
	 * taking the process down.  uhexen2-5krx */
	if (hdr->numposes <= 0 || hdr->poseverts <= 0)
	{
		Con_DPrintf ("GL_MakeAliasGPUMesh: no pose data (numposes=%d poseverts=%d), model will not be drawn\n",
					hdr->numposes, hdr->poseverts);
		GL_RefuseAliasGPUMesh(hdr, gm);
		GL_MeshScratchFree();
		return;
	}

	gm->num_indices = vi;
	gm->poseverts = hdr->poseverts;
	gm->numposes = hdr->numposes;
	gm->valid = true;

	hdr->gpu_mesh = slot;
	hdr->gpu_mesh_gen = alias_gpu_generation;
	if (slot == num_alias_gpu_meshes)
		num_alias_gpu_meshes++;

	GL_MeshScratchFree();
}

