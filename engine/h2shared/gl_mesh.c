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

static void GL_MakeAliasGPUMesh (aliashdr_t *hdr);

/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

static int		used[8192];

// the command list holds counts and s/t values that are valid for
// every frame
static int		commands[8192];
static int		numcommands;

// all frames will have their vertexes rearranged and expanded
// so they are in the order expected by the command list
static int		vertexorder[8192];
static int		numorder;

static int		stripverts[128];
static int		striptris[128];
static int		stripstverts[128];
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
	int		bestverts[1024];
	int		besttris[1024];
	int		beststverts[1024];
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

/* Map an aliashdr_t to its GPU mesh.  Searches by pointer-derived key. */
static int alias_gpu_keys[MAX_ALIAS_MODELS]; /* hdr address hash for lookup */

alias_gpu_mesh_t *GL_GetAliasGPUMesh (aliashdr_t *hdr)
{
	int i, key = (int)((size_t)hdr & 0x7fffffff);
	for (i = 0; i < num_alias_gpu_meshes; i++)
	{
		if (alias_gpu_keys[i] == key && alias_gpu_meshes[i].valid)
			return &alias_gpu_meshes[i];
	}
	return NULL;
}

void GL_FreeAliasGPUMeshes (void)
{
	int i;
	for (i = 0; i < num_alias_gpu_meshes; i++)
	{
		alias_gpu_mesh_t *gm = &alias_gpu_meshes[i];
		if (gm->vao)       { glDeleteVertexArrays_fp(1, &gm->vao); }
		if (gm->vbo_tc)    { glDeleteBuffers_fp(1, &gm->vbo_tc); }
		if (gm->ibo)       { glDeleteBuffers_fp(1, &gm->ibo); }
		if (gm->ssbo_pose) { glDeleteBuffers_fp(1, &gm->ssbo_pose); }
		if (gm->tex_pose)  { glDeleteTextures_fp(1, &gm->tex_pose); }
	}
	memset(alias_gpu_meshes, 0, sizeof(alias_gpu_meshes));
	memset(alias_gpu_keys, 0, sizeof(alias_gpu_keys));
	num_alias_gpu_meshes = 0;
}

/*
================
GL_MakeAliasGPUMesh

Triangulate the command list, extract static texcoords, and upload
all pose vertex data to an SSBO.  Called from GL_MakeAliasModelDisplayLists.
================
*/
static void GL_MakeAliasGPUMesh (aliashdr_t *hdr)
{
	int		*order, count;
	int		num_tris, vi, vert_idx;
	unsigned short	*indices;
	float		*texcoords;
	unsigned int	*pose_packed;
	trivertx_t	*pv;
	alias_gpu_mesh_t *gm;
	int		mark, i, j;

	if (num_alias_gpu_meshes >= MAX_ALIAS_MODELS)
		return;

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
		return;

	mark = Hunk_LowMark();
	indices = (unsigned short *) Hunk_AllocName(num_tris * 3 * sizeof(unsigned short), "gpuidx");
	texcoords = (float *) Hunk_AllocName(hdr->poseverts * 2 * sizeof(float), "gputc");

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

	/* Pack all pose vertex data: trivertx_t → uint (v[0] | v[1]<<8 | v[2]<<16 | ni<<24) */
	pose_packed = (unsigned int *) Hunk_AllocName(
		hdr->numposes * hdr->poseverts * sizeof(unsigned int), "gpupose");
	pv = (trivertx_t *)((byte *)hdr + hdr->posedata);
	for (i = 0; i < hdr->numposes * hdr->poseverts; i++)
	{
		pose_packed[i] = (unsigned int)pv[i].v[0]
			       | ((unsigned int)pv[i].v[1] << 8)
			       | ((unsigned int)pv[i].v[2] << 16)
			       | ((unsigned int)pv[i].lightnormalindex << 24);
	}

	/* Create GPU objects */
	gm = &alias_gpu_meshes[num_alias_gpu_meshes];
	memset(gm, 0, sizeof(*gm));

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

#ifndef __EMSCRIPTEN__
	/* Pose SSBO (GL 4.3 path) */
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

	gm->num_indices = vi;
	gm->poseverts = hdr->poseverts;
	gm->numposes = hdr->numposes;
	gm->valid = true;

	alias_gpu_keys[num_alias_gpu_meshes] = (int)((size_t)hdr & 0x7fffffff);
	num_alias_gpu_meshes++;

	Hunk_FreeToLowMark(mark);
}

