/* gl_worldcull.c -- GPU compute world surface culling (AZDO Phase 3)
 *
 * Replaces CPU R_RecursiveWorldNode visibility marking with GPU compute:
 * - Upload decompressed PVS bitvector per frame
 * - Compute shader performs PVS + backface + frustum cull per surface
 * - Surviving surfaces written to indirect draw buffer
 * - One glMultiDrawElementsIndirect per texture bucket
 *
 * Requires GL 4.3 (compute shaders, SSBOs, indirect draw).
 * Falls back to CPU path when unavailable.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"

#ifdef GLQUAKE
#ifndef __EMSCRIPTEN__

#include "gl_shader.h"
#include "gl_vbo.h"
#include "gl_matrix.h"

/* ------------------------------------------------------------------ */
/* Data structures matching GLSL std430 layout                         */
/* ------------------------------------------------------------------ */

/* Per-surface GPU data (built at map load) */
typedef struct {
	float	plane[4];	/* normal.xyz + dist */
	float	mins[4];	/* bbox min.xyz + pad */
	float	maxs[4];	/* bbox max.xyz + pad */
	int	tex_bucket;	/* texture index for indirect cmd */
	int	firstindex;	/* IBO offset (in indices) */
	int	numindices;	/* triangle count * 3 */
	int	flags;		/* SURF_* flags */
} gpu_surface_t;		/* 64 bytes */

/* Per marksurface entry: which leaf references which surface */
typedef struct {
	int	leaf_idx;	/* leaf number (for PVS lookup) */
	int	surf_idx;	/* surface number (index into gpu_surfaces) */
} gpu_marksurf_t;		/* 8 bytes */

/* GL indirect draw command (matches DrawElementsIndirectCommand) */
typedef struct {
	unsigned int	count;		/* number of indices */
	unsigned int	instanceCount;	/* always 1 */
	unsigned int	firstIndex;	/* byte offset into IBO / sizeof(uint) */
	unsigned int	baseVertex;	/* always 0 */
	unsigned int	baseInstance;	/* always 0 */
} gpu_indirect_cmd_t;		/* 20 bytes */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static GLuint	cull_surf_ssbo;		/* binding 0: gpu_surface_t[] */
static GLuint	cull_marksurf_ssbo;	/* binding 1: gpu_marksurf_t[] */
static GLuint	cull_vis_ssbo;		/* binding 2: PVS bitvector (uint[]) */
static GLuint	cull_indirect_buf;	/* binding 3: gpu_indirect_cmd_t[] */
static GLuint	cull_src_ibo;		/* binding 4: source indices (read-only copy) */
static GLuint	cull_dst_ibo;		/* binding 5: dest indices (compute writes, draw reads) */
static GLuint	cull_dedup_ssbo;	/* binding 6: per-surface framecount for dedup */
static int	cull_total_indices;	/* total index count across all buckets */

/* (Hi-Z occlusion culling planned as future work — uhexen2-xd87) */

static GLuint	cull_clear_prog;	/* clear_indirect compute shader */
static GLuint	cull_mark_prog;		/* cull_mark compute shader */

static int	cull_num_surfs;		/* total world surfaces */
static int	cull_num_marksurfs;	/* total marksurface entries */
static int	cull_num_buckets;	/* texture buckets (= numtextures) */
static int	cull_num_leaves;	/* total BSP leaves */

static int	*cull_bucket_map;	/* surface index -> texture bucket */

static qboolean	cull_initialized;

/* uniform locations */
static GLint	cull_mark_u_vieworg;
static GLint	cull_mark_u_frustum;
static GLint	cull_mark_u_framecount;
static GLint	cull_mark_u_num_marksurfs;

static GLint	cull_clear_u_num_buckets;

/* ------------------------------------------------------------------ */
/* Compute shader sources                                              */
/* ------------------------------------------------------------------ */

static const char clear_indirect_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 64) in;\n"
	"\n"
	"struct IndirectCmd {\n"
	"    uint count;\n"
	"    uint instanceCount;\n"
	"    uint firstIndex;\n"
	"    uint baseVertex;\n"
	"    uint baseInstance;\n"
	"};\n"
	"\n"
	"layout(std430, binding = 3) buffer IndirectBuffer {\n"
	"    IndirectCmd cmds[];\n"
	"};\n"
	"\n"
	"uniform int u_num_buckets;\n"
	"\n"
	"void main() {\n"
	"    uint id = gl_GlobalInvocationID.x;\n"
	"    if (id >= uint(u_num_buckets)) return;\n"
	"    cmds[id].count = 0u;\n"
	"    cmds[id].instanceCount = 1u;\n"
	"}\n";

static const char cull_mark_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 64) in;\n"
	"\n"
	"struct Surface {\n"
	"    vec4 plane;\n"
	"    vec4 mins;\n"
	"    vec4 maxs;\n"
	"    int tex_bucket;\n"
	"    int firstindex;\n"
	"    int numindices;\n"
	"    int flags;\n"
	"};\n"
	"\n"
	"struct MarkSurf {\n"
	"    int leaf_idx;\n"
	"    int surf_idx;\n"
	"};\n"
	"\n"
	"struct IndirectCmd {\n"
	"    uint count;\n"
	"    uint instanceCount;\n"
	"    uint firstIndex;\n"
	"    uint baseVertex;\n"
	"    uint baseInstance;\n"
	"};\n"
	"\n"
	"layout(std430, binding = 0) readonly buffer SurfaceBuffer {\n"
	"    Surface surfaces[];\n"
	"};\n"
	"layout(std430, binding = 1) readonly buffer MarkSurfBuffer {\n"
	"    MarkSurf marksurfs[];\n"
	"};\n"
	"layout(std430, binding = 2) readonly buffer VisBuffer {\n"
	"    uint vis[];\n"
	"};\n"
	"layout(std430, binding = 3) buffer IndirectBuffer {\n"
	"    IndirectCmd cmds[];\n"
	"};\n"
	"layout(std430, binding = 4) readonly buffer SrcIndexBuffer {\n"
	"    uint src_indices[];\n"
	"};\n"
	"layout(std430, binding = 5) writeonly buffer DstIndexBuffer {\n"
	"    uint dst_indices[];\n"
	"};\n"
	"layout(std430, binding = 6) buffer DedupBuffer {\n"
	"    int dedup[];\n"  /* per-surface framecount for dedup */
	"};\n"
	"\n"
	"uniform vec3 u_vieworg;\n"
	"uniform vec4 u_frustum[4];\n"
	"uniform int u_framecount;\n"
	"uniform int u_num_marksurfs;\n"
	"\n"
	"void main() {\n"
	"    uint id = gl_GlobalInvocationID.x;\n"
	"    if (id >= uint(u_num_marksurfs)) return;\n"
	"\n"
	"    MarkSurf ms = marksurfs[id];\n"
	"    int leaf = ms.leaf_idx;\n"
	"    int si = ms.surf_idx;\n"
	"\n"
	"    /* PVS test */\n"
	"    if ((vis[leaf >> 5] & (1u << (leaf & 31))) == 0u)\n"
	"        return;\n"
	"\n"
	"    Surface s = surfaces[si];\n"
	"\n"
	"    /* Skip sky, liquid, fence, underwater */\n"
	"    if ((s.flags & 0x35) != 0) return;\n"
	"\n"
	"    /* Skip surfaces with no VBO data */\n"
	"    if (s.numindices <= 0) return;\n"
	"\n"
	"    /* Backface cull — disabled to avoid popping at grazing angles.\n"
	"     * GPU rasterizer handles backfaces via winding order. */\n"
	"    /* float d = dot(s.plane.xyz, u_vieworg) - s.plane.w;\n"
	"    bool planeback = (s.flags & 2) != 0;\n"
	"    if (planeback && d > 0.01) return;\n"
	"    if (!planeback && d < -0.01) return; */\n"
	"\n"
	"    /* Frustum cull (4 planes) */\n"
	"    for (int p = 0; p < 4; p++) {\n"
	"        vec3 n = u_frustum[p].xyz;\n"
	"        float fd = u_frustum[p].w;\n"
	"        vec3 pvert = vec3(\n"
	"            n.x > 0.0 ? s.maxs.x : s.mins.x,\n"
	"            n.y > 0.0 ? s.maxs.y : s.mins.y,\n"
	"            n.z > 0.0 ? s.maxs.z : s.mins.z\n"
	"        );\n"
	"        if (dot(n, pvert) + fd < 0.0) return;\n"
	"    }\n"
	"\n"
	"    /* Dedup: same surface can appear in multiple leaves.\n"
	"     * atomicExchange the framecount — if it was already this\n"
	"     * frame's value, another thread already emitted it. */\n"
	"    int old = atomicExchange(dedup[si], u_framecount);\n"
	"    if (old == u_framecount) return;\n"
	"\n"
	"    /* Claim index slots in the texture bucket's indirect command */\n"
	"    uint slot = atomicAdd(cmds[s.tex_bucket].count, uint(s.numindices));\n"
	"    uint dst_base = cmds[s.tex_bucket].firstIndex + slot;\n"
	"\n"
	"    /* Copy indices from source to destination */\n"
	"    for (int i = 0; i < s.numindices; i++)\n"
	"        dst_indices[dst_base + uint(i)] = src_indices[s.firstindex + i];\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Compile helper                                                      */
/* ------------------------------------------------------------------ */

static GLuint R_CompileComputeProgram (const char *source, const char *name)
{
	GLuint shader, prog;
	GLint status;
	char log[1024];

	shader = glCreateShader_fp(GL_COMPUTE_SHADER);
	glShaderSource_fp(shader, 1, &source, NULL);
	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("Compute shader '%s' compile error: %s\n", name, log);
		glDeleteShader_fp(shader);
		return 0;
	}

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, shader);
	glLinkProgram_fp(prog);
	glDeleteShader_fp(shader);

	glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
	if (!status)
	{
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_Printf("Compute shader '%s' link error: %s\n", name, log);
		glDeleteProgram_fp(prog);
		return 0;
	}

	return prog;
}

/* ------------------------------------------------------------------ */
/* Build GPU data at map load                                          */
/* ------------------------------------------------------------------ */

void R_BuildWorldCull (void)
{
	qmodel_t	*m = cl.worldmodel;
	msurface_t	*surf;
	mleaf_t		*leaf;
	gpu_surface_t	*gpu_surfs;
	gpu_marksurf_t	*gpu_marks;
	int		i, j, ms_count;

	if (!m || !glDispatchCompute_fp)
		return;

	R_FreeWorldCull();

	/* Compile compute shaders */
	cull_clear_prog = R_CompileComputeProgram(clear_indirect_src, "clear_indirect");
	cull_mark_prog = R_CompileComputeProgram(cull_mark_src, "cull_mark");
	if (!cull_clear_prog || !cull_mark_prog)
	{
		Con_Printf("GPU world culling: compute shaders failed, using CPU path\n");
		R_FreeWorldCull();
		return;
	}

	/* Get uniform locations */
	cull_clear_u_num_buckets = glGetUniformLocation_fp(cull_clear_prog, "u_num_buckets");
	cull_mark_u_vieworg = glGetUniformLocation_fp(cull_mark_prog, "u_vieworg");
	cull_mark_u_frustum = glGetUniformLocation_fp(cull_mark_prog, "u_frustum");
	cull_mark_u_framecount = glGetUniformLocation_fp(cull_mark_prog, "u_framecount");
	cull_mark_u_num_marksurfs = glGetUniformLocation_fp(cull_mark_prog, "u_num_marksurfs");

	cull_num_surfs = m->numsurfaces;
	cull_num_leaves = m->numleafs;
	cull_num_buckets = m->numtextures;

	/* Build per-surface GPU data */
	gpu_surfs = (gpu_surface_t *) malloc(cull_num_surfs * sizeof(gpu_surface_t));
	cull_bucket_map = (int *) malloc(cull_num_surfs * sizeof(int));
	for (i = 0; i < cull_num_surfs; i++)
	{
		surf = &m->surfaces[i];
		gpu_surfs[i].plane[0] = surf->plane->normal[0];
		gpu_surfs[i].plane[1] = surf->plane->normal[1];
		gpu_surfs[i].plane[2] = surf->plane->normal[2];
		gpu_surfs[i].plane[3] = surf->plane->dist;

		/* Compute surface AABB from polygon */
		if (surf->polys && surf->polys->numverts > 0)
		{
			float *v = surf->polys->verts[0];
			float bmin[3] = { v[0], v[1], v[2] };
			float bmax[3] = { v[0], v[1], v[2] };
			for (j = 1; j < surf->polys->numverts; j++)
			{
				v = surf->polys->verts[j];
				if (v[0] < bmin[0]) bmin[0] = v[0];
				if (v[1] < bmin[1]) bmin[1] = v[1];
				if (v[2] < bmin[2]) bmin[2] = v[2];
				if (v[0] > bmax[0]) bmax[0] = v[0];
				if (v[1] > bmax[1]) bmax[1] = v[1];
				if (v[2] > bmax[2]) bmax[2] = v[2];
			}
			gpu_surfs[i].mins[0] = bmin[0];
			gpu_surfs[i].mins[1] = bmin[1];
			gpu_surfs[i].mins[2] = bmin[2];
			gpu_surfs[i].maxs[0] = bmax[0];
			gpu_surfs[i].maxs[1] = bmax[1];
			gpu_surfs[i].maxs[2] = bmax[2];
		}
		gpu_surfs[i].mins[3] = 0;
		gpu_surfs[i].maxs[3] = 0;

		/* Texture bucket = find texture index in model's texture array */
		gpu_surfs[i].tex_bucket = 0;
		if (surf->texinfo->texture)
		{
			for (j = 0; j < m->numtextures; j++)
			{
				if (m->textures[j] == surf->texinfo->texture)
				{
					gpu_surfs[i].tex_bucket = j;
					break;
				}
			}
		}

		gpu_surfs[i].firstindex = surf->vbo_firstindex;
		gpu_surfs[i].numindices = surf->vbo_numtris * 3;
		gpu_surfs[i].flags = surf->flags;

		cull_bucket_map[i] = gpu_surfs[i].tex_bucket;
	}

	/* Upload surface SSBO */
	glGenBuffers_fp(1, &cull_surf_ssbo);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_surf_ssbo);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
			cull_num_surfs * sizeof(gpu_surface_t),
			gpu_surfs, GL_STATIC_DRAW);
	free(gpu_surfs);

	/* Build marksurface entries: flatten (leaf, surface) pairs */
	ms_count = 0;
	for (i = 1; i < cull_num_leaves; i++)
		ms_count += m->leafs[i].nummarksurfaces;

	gpu_marks = (gpu_marksurf_t *) malloc(ms_count * sizeof(gpu_marksurf_t));
	ms_count = 0;
	for (i = 1; i < cull_num_leaves; i++)
	{
		leaf = &m->leafs[i];
		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			int surf_idx = (int)(leaf->firstmarksurface[j] - m->surfaces);
			gpu_marks[ms_count].leaf_idx = i;
			gpu_marks[ms_count].surf_idx = surf_idx;
			ms_count++;
		}
	}
	cull_num_marksurfs = ms_count;

	glGenBuffers_fp(1, &cull_marksurf_ssbo);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_marksurf_ssbo);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
			cull_num_marksurfs * sizeof(gpu_marksurf_t),
			gpu_marks, GL_STATIC_DRAW);
	free(gpu_marks);

	/* PVS buffer (uploaded per-frame) */
	{
		int vis_size = (cull_num_leaves + 31) / 32;
		glGenBuffers_fp(1, &cull_vis_ssbo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_vis_ssbo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				vis_size * sizeof(unsigned int),
				NULL, GL_DYNAMIC_DRAW);
	}

	/* Count max indices per texture bucket for firstIndex allocation */
	{
		int *bucket_max = (int *) calloc(cull_num_buckets, sizeof(int));
		gpu_indirect_cmd_t *cmds;
		int offset;

		for (i = 0; i < cull_num_surfs; i++)
		{
			surf = &m->surfaces[i];
			if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_UNDERWATER))
				continue;
			if (surf->vbo_numtris <= 0)
				continue;
			int bucket = cull_bucket_map[i];
			if (bucket >= 0 && bucket < cull_num_buckets)
				bucket_max[bucket] += surf->vbo_numtris * 3;
		}

		/* Allocate contiguous regions in the dest IBO per bucket */
		cmds = (gpu_indirect_cmd_t *) calloc(cull_num_buckets, sizeof(gpu_indirect_cmd_t));
		offset = 0;
		for (i = 0; i < cull_num_buckets; i++)
		{
			cmds[i].count = 0;		/* filled by compute each frame */
			cmds[i].instanceCount = 1;
			cmds[i].firstIndex = offset;	/* fixed region start */
			cmds[i].baseVertex = 0;
			cmds[i].baseInstance = 0;
			offset += bucket_max[i];
		}
		cull_total_indices = offset;
		free(bucket_max);

		glGenBuffers_fp(1, &cull_indirect_buf);
		glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);
		glBufferData_fp(GL_DRAW_INDIRECT_BUFFER,
				cull_num_buckets * sizeof(gpu_indirect_cmd_t),
				cmds, GL_DYNAMIC_DRAW);
		free(cmds);
	}

	/* Source index SSBO: read-only copy of world IBO for compute to read from.
	 * We read the data back from the existing world IBO (built by R_BuildWorldVBO). */
	{
		extern GLuint world_ibo;
		extern int world_num_indices;
		unsigned int *idx_data;

		if (world_ibo && world_num_indices > 0)
		{
			idx_data = (unsigned int *) malloc(world_num_indices * sizeof(unsigned int));
			/* Rebuild index data from surface polys (same as R_BuildWorldVBO) */
			{
				int idx_pos = 0;
				for (i = 0; i < m->numsurfaces; i++)
				{
					glpoly_t *p;
					surf = &m->surfaces[i];
					p = surf->polys;
					if (!p || p->numverts < 3)
						continue;
					if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
						continue;
					for (j = 2; j < p->numverts; j++)
					{
						idx_data[idx_pos++] = surf->vbo_firstvert;
						idx_data[idx_pos++] = surf->vbo_firstvert + j - 1;
						idx_data[idx_pos++] = surf->vbo_firstvert + j;
					}
				}
			}

			glGenBuffers_fp(1, &cull_src_ibo);
			glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_src_ibo);
			glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
					world_num_indices * sizeof(unsigned int),
					idx_data, GL_STATIC_DRAW);
			free(idx_data);
		}
	}

	/* Dest index buffer: written by compute, read as GL_ELEMENT_ARRAY_BUFFER.
	 * Sized to hold the worst case (all surfaces visible). */
	if (cull_total_indices > 0)
	{
		glGenBuffers_fp(1, &cull_dst_ibo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_dst_ibo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				cull_total_indices * sizeof(unsigned int),
				NULL, GL_DYNAMIC_DRAW);
	}

	/* Dedup buffer: one int per surface, atomicExchange'd with framecount */
	{
		int *zeros = (int *) calloc(cull_num_surfs, sizeof(int));
		glGenBuffers_fp(1, &cull_dedup_ssbo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_dedup_ssbo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				cull_num_surfs * sizeof(int),
				zeros, GL_DYNAMIC_DRAW);
		free(zeros);
	}

	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	cull_initialized = true;

	Con_SafePrintf("GPU world cull: %d surfs, %d marksurfs, %d buckets, %d max indices\n",
		       cull_num_surfs, cull_num_marksurfs, cull_num_buckets, cull_total_indices);
}

void R_FreeWorldCull (void)
{
	if (cull_surf_ssbo) { glDeleteBuffers_fp(1, &cull_surf_ssbo); cull_surf_ssbo = 0; }
	if (cull_marksurf_ssbo) { glDeleteBuffers_fp(1, &cull_marksurf_ssbo); cull_marksurf_ssbo = 0; }
	if (cull_vis_ssbo) { glDeleteBuffers_fp(1, &cull_vis_ssbo); cull_vis_ssbo = 0; }
	if (cull_indirect_buf) { glDeleteBuffers_fp(1, &cull_indirect_buf); cull_indirect_buf = 0; }
	if (cull_src_ibo) { glDeleteBuffers_fp(1, &cull_src_ibo); cull_src_ibo = 0; }
	if (cull_dst_ibo) { glDeleteBuffers_fp(1, &cull_dst_ibo); cull_dst_ibo = 0; }
	if (cull_dedup_ssbo) { glDeleteBuffers_fp(1, &cull_dedup_ssbo); cull_dedup_ssbo = 0; }
	if (cull_clear_prog) { glDeleteProgram_fp(cull_clear_prog); cull_clear_prog = 0; }
	if (cull_mark_prog) { glDeleteProgram_fp(cull_mark_prog); cull_mark_prog = 0; }
	if (cull_bucket_map) { free(cull_bucket_map); cull_bucket_map = NULL; }
	cull_initialized = false;
	cull_total_indices = 0;
}

qboolean R_WorldCullAvailable (void)
{
	return cull_initialized;
}

/* ------------------------------------------------------------------ */
/* Per-frame dispatch                                                  */
/* ------------------------------------------------------------------ */

void R_DispatchWorldCull (void)
{
	byte		*pvs;
	unsigned int	*vis_bits;
	int		vis_size, i;
	static int	framecount;
	extern vec3_t	r_origin;

	if (!cull_initialized)
		return;

	framecount++;

	/* Decompress PVS for current viewleaf */
	pvs = Mod_LeafPVS(r_viewleaf, cl.worldmodel);
	vis_size = (cull_num_leaves + 31) / 32;
	vis_bits = (unsigned int *) alloca(vis_size * sizeof(unsigned int));
	memset(vis_bits, 0, vis_size * sizeof(unsigned int));

	/* Convert byte PVS to bit array */
	for (i = 0; i < cull_num_leaves - 1; i++)
	{
		if (pvs[i >> 3] & (1 << (i & 7)))
			vis_bits[(i + 1) >> 5] |= (1u << ((i + 1) & 31));
	}

	/* Upload PVS */
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_vis_ssbo);
	glBufferSubData_fp(GL_SHADER_STORAGE_BUFFER, 0,
			   vis_size * sizeof(unsigned int), vis_bits);

	/* Pass 1: clear indirect buffer */
	glUseProgram_fp(cull_clear_prog);
	glUniform1i_fp(cull_clear_u_num_buckets, cull_num_buckets);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	glDispatchCompute_fp((cull_num_buckets + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_SHADER_STORAGE_BARRIER_BIT);

	/* Pass 2: cull + mark surfaces */
	glUseProgram_fp(cull_mark_prog);
	glUniform3f_fp(cull_mark_u_vieworg, r_origin[0], r_origin[1], r_origin[2]);
	glUniform1i_fp(cull_mark_u_framecount, framecount);
	glUniform1i_fp(cull_mark_u_num_marksurfs, cull_num_marksurfs);

	/* Upload frustum planes */
	{
		extern mplane_t frustum[4];
		float frust[16]; /* 4 vec4s */
		for (i = 0; i < 4; i++)
		{
			frust[i*4+0] = frustum[i].normal[0];
			frust[i*4+1] = frustum[i].normal[1];
			frust[i*4+2] = frustum[i].normal[2];
			frust[i*4+3] = -frustum[i].dist;  /* negate: shader uses dot(n,p)+d, engine uses dot(n,p)-dist */
		}
		glUniform4fv_fp(cull_mark_u_frustum, 4, frust);
	}

	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 0, cull_surf_ssbo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, cull_marksurf_ssbo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 2, cull_vis_ssbo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 4, cull_src_ibo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 5, cull_dst_ibo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 6, cull_dedup_ssbo);

	glDispatchCompute_fp((cull_num_marksurfs + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_COMMAND_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT |
			   GL_SHADER_STORAGE_BARRIER_BIT);

	glUseProgram_fp(0);
}

/* ------------------------------------------------------------------ */
/* Draw with indirect commands after compute cull                      */
/* ------------------------------------------------------------------ */

void R_DrawWorldCulled (void)
{
	int		i;
	float		mvp[16], mv[16];
	extern GLuint	world_vao;
	extern float	r_fog_density;
	extern float	r_fog_color[3];
	extern GLuint	lm_atlas_texture;
	extern entity_t	r_worldentity;

	if (!cull_initialized || !cull_dst_ibo || cull_total_indices <= 0)
		return;

	/* Bind world VAO but replace its IBO with the compute-written dest IBO */
	glBindVertexArray_fp(world_vao);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, cull_dst_ibo);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

	/* Set up world shader */
	glUseProgram_fp(gl_shader_world.program);
	GL_GetMVP(mvp);
	GL_GetModelview(mv);
	if (gl_shader_world.u_mvp >= 0)
		glUniformMatrix4fv_fp(gl_shader_world.u_mvp, 1, GL_FALSE, mvp);
	if (gl_shader_world.u_modelview >= 0)
		glUniformMatrix4fv_fp(gl_shader_world.u_modelview, 1, GL_FALSE, mv);
	if (gl_shader_world.u_fog_density >= 0)
		glUniform1f_fp(gl_shader_world.u_fog_density, r_fog_density);
	if (gl_shader_world.u_fog_color >= 0)
		glUniform3f_fp(gl_shader_world.u_fog_color,
			       r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (gl_shader_world.u_alpha_threshold >= 0)
		glUniform1f_fp(gl_shader_world.u_alpha_threshold, 0.01f);

	/* Bind lightmap atlas on unit 1 */
	glActiveTextureARB_fp(GL_TEXTURE1_ARB);
	glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	/* Draw each texture bucket via indirect commands */
	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);

	for (i = 0; i < cull_num_buckets; i++)
	{
		texture_t *t = cl.worldmodel->textures[i];
		if (!t)
			continue;

		/* Bind diffuse texture */
		GL_Bind(t->gl_texturenum);

		/* Issue indirect draw for this bucket */
		glDrawElementsIndirect_fp(GL_TRIANGLES, GL_UNSIGNED_INT,
					  (void *)((size_t)i * sizeof(gpu_indirect_cmd_t)));
	}

	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, 0);
	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
}


#endif /* !__EMSCRIPTEN__ */
#endif /* GLQUAKE */
