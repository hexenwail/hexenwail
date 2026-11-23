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
#ifndef USE_GLES

#include "gl_shader.h"
#include "gl_uniforms.h"
#include "gl_pipeline.h"
#include "gl_vbo.h"
#include "gl_matrix.h"
#include "gl_postprocess.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"	/* R_GPU (): Hi-Z stays off under -gpu (#340 P5) */
#endif

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
/* binding 2 (PVS bitvector) streams from the frame ring via GL_Upload —
 * no dedicated SSBO; see R_DispatchWorldCull. uhexen2-o35n. */
static GLuint	cull_indirect_buf;	/* binding 3: gpu_indirect_cmd_t[] */
static GLuint	cull_src_ibo;		/* binding 4: source indices (read-only copy) */
static GLuint	cull_dst_ibo;		/* binding 5: dest indices (compute writes, draw reads) */
static GLuint	cull_dedup_ssbo;	/* binding 6: per-surface framecount for dedup */
static int	cull_total_indices;	/* total index count across all buckets */

/* Hi-Z occlusion culling — uhexen2-xd87.
 * Previous-frame depth pyramid (latency-1) drives a per-AABB rejection
 * inside cull_mark, before the dedup atomic.  The pyramid lives in a
 * R32F mip chain copied/reduced from the scene depth texture at end of
 * the 3D pass; the cull dispatch samples it via binding 7.
 */
cvar_t	gl_hiz_cull = {"gl_hiz_cull", "1", CVAR_ARCHIVE};
/* Hi-Z cull-rate stats (uhexen2-cyu0).  N = print every N frames; 0 = off.
 * Enables 7 atomicAdd counters inside cull_mark + glGetBufferSubData
 * readback.  Used to validate the "≥10% post-frustum cull rate" gate
 * before flipping gl_hiz_cull default to 1.  Per-frame readback stalls
 * the CPU — throttle with N >= 30 for normal use. */
cvar_t	gl_hiz_stats = {"gl_hiz_stats", "0", CVAR_NONE};
static GLuint	cull_stats_ssbo;	/* binding 7: stats[8] uints */
static int	hiz_stats_frame_counter;

static GLuint	hiz_pyramid_tex;	/* binding 7 sampler in cull_mark */
static GLuint	hiz_copy_prog;		/* copy scene depth -> mip 0 */
static GLuint	hiz_reduce_prog;	/* min/max 2x2 reduction per mip */
static int	hiz_width;		/* mip 0 size in pixels */
static int	hiz_height;
static int	hiz_mip_count;
static float	hiz_prev_mvp[16];	/* world MVP captured at end of last 3D pass */
static qboolean	hiz_prev_mvp_valid;

/* Standalone depth resolve (uhexen2-9912): when the postprocess pipeline
 * is inactive (no FXAA/HDR/etc.) we still need a sampler-readable depth
 * to seed the Hi-Z pyramid.  Maintain a private depth texture + FBO and
 * blit-resolve the default framebuffer's depth into it after the 3D pass.
 * Allocated lazily — zero overhead when gl_hiz_cull is off. */
static GLuint	hiz_resolve_fbo;
static GLuint	hiz_resolve_depth_tex;
static int	hiz_resolve_w;
static int	hiz_resolve_h;

static GLuint	cull_clear_prog;	/* clear_indirect compute shader */
static GLuint	cull_mark_prog;		/* cull_mark compute shader */

static int	cull_num_surfs;		/* total world surfaces */
static int	cull_num_marksurfs;	/* total marksurface entries */
static int	cull_num_buckets;	/* texture buckets (= numtextures) */
static int	cull_num_leaves;	/* total BSP leaves */

static int	*cull_bucket_map;	/* surface index -> texture bucket */

static qboolean	cull_initialized;

/* ------------------------------------------------------------------ */
/* Compute shader sources                                              */
/* ------------------------------------------------------------------ */

/* The cull family lives in engine/shaders/ (#450): cull_clear_comp.glsl (was
 * clear_indirect_src), cull_mark_comp.glsl and hiz_reduce_comp.glsl, built as
 * RPROG_CULL_CLEAR, RPROG_CULL_MARK and RPROG_HIZ_REDUCE (gl_shader.c's
 * rprog_defs).  All three include cull.inc, the one CullParams declaration,
 * whose C mirror is cull_params_t below.  Like hiz_copy_comp they carry no
 * #version; the desktop tier header, TIER_CORE, is a separate glShaderSource
 * chunk.  The GL binding points named in cull_mark_comp.glsl (SSBOs 0-7, the
 * pyramid on unit 1) are the ones R_DispatchWorldCull binds. */

/* ------------------------------------------------------------------ */
/* Hi-Z compute shaders                                                */
/* ------------------------------------------------------------------ */

/* hiz_copy lives in engine/shaders/hiz_copy_comp.glsl, built as
 * RPROG_HIZ_COPY.  Its #version is supplied separately, as a second
 * glShaderSource chunk, so the file itself stays tier-agnostic. */

/* Uniform-block binding point for HizParams.  GLSL ES 3.00 cannot put
 * layout(binding=) on a block, so the shader does not name this and
 * glUniformBlockBinding assigns it here; keep the two in step by hand.
 * Independent of the SSBO binding points above -- different target, different
 * index space. */
#define HIZ_PARAMS_UBO_BINDING	0

/* Uniform-block binding point for CullParams.  Same arrangement as
 * HizParams above: the shader does not name the point, the C side assigns
 * it once per program at compile time.  Binding 4 is the next free point
 * after 0 (HizParams), 1/2 (the shared vert/frag blocks) and 3 (SsaoParams);
 * see the binding comments in gl_shader.h and gl_ssao.c. */
#define CULL_PARAMS_UBO_BINDING	4

/* Mirrors the std140 HizParams block.  ivec4 rather than ivec2 so the padding
 * is explicit in both languages. */
typedef struct {
	GLint	size[4];	/* xy = scene extent, zw unused */
} hiz_params_t;

/* Mirrors the std140 CullParams block.  All members vec4/mat4/ivec4, so
 * the std140 offsets are exact by construction, pinned by
 * COMPILE_TIME_ASSERT below and by tools/test_cullparams_block.py. */
typedef struct {
	GLint	counts0[4];	/* x framecount, y num_marksurfs, z num_buckets, w max_dst_indices */
	GLint	counts1[4];	/* x hiz_mip_count, y hiz_enable, z reverse_z, w stats_enable */
	float	vieworg[4];	/* xyz view origin, w unused */
	float	frustum[4][4];	/* frustum planes (normal.xyz, -dist) */
	float	prevmvp[16];	/* previous-frame MVP for Hi-Z reprojection */
	float	hizsize[4];	/* xy Hi-Z pyramid size, zw unused */
	GLint	reduce[4];	/* x src_w, y src_h, z dst_w, w dst_h */
	GLint	reduce2[4];	/* x src_lod, y reverse_z, zw unused */
} cull_params_t;		/* 224 bytes */

COMPILE_TIME_ASSERT(cullparams_size, sizeof(cull_params_t) == 224);


/* ------------------------------------------------------------------ */
/* Build GPU data at map load                                          */
/* ------------------------------------------------------------------ */

/* Bind the CullParams block of a compute program to its binding point,
 * once, at compile time; the binding dies with the program, so vid_restart
 * is covered by construction.  Also asks the driver where it actually put
 * the block and complains rather than rendering garbage if it disagrees
 * with the C mirror (the RU_CheckBlockSize practice in gl_uniforms.c). */
static void WC_BindCullParamsBlock (GLuint program)
{
	GLuint	idx;
	GLint	size = 0;

	if (!program || !glGetUniformBlockIndex_fp || !glUniformBlockBinding_fp)
		return;
	idx = glGetUniformBlockIndex_fp(program, "CullParams");
	if (idx == GL_INVALID_INDEX)
		return;
	if (glGetActiveUniformBlockiv_fp)
	{
		glGetActiveUniformBlockiv_fp(program, idx, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
		if (size != (GLint)sizeof(cull_params_t))
			Con_Printf("WARNING: uniform block CullParams is %d bytes on this driver, "
				   "C mirror is %d -- uniforms will be wrong\n",
				   (int)size, (int)sizeof(cull_params_t));
	}
	glUniformBlockBinding_fp(program, idx, CULL_PARAMS_UBO_BINDING);
}

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
#ifdef USE_SDL_GPU
	/* Compute, P5 of #340, and its buffers and its indirect draw from
	 * world_vao are raw GL the null table swallows.  The null table's
	 * glDispatchCompute is not NULL, so the test above passes under -gpu:
	 * say no here, and DrawTextureChains draws the whole world. */
	if (R_GPU ())
		return;
#endif

	R_FreeWorldCull();

	/* Compile compute shaders */
	cull_clear_prog = R_Program_Create(RPROG_CULL_CLEAR);
	cull_mark_prog = R_Program_Create(RPROG_CULL_MARK);
	if (!cull_clear_prog || !cull_mark_prog)
	{
		Con_Printf("GPU world culling: compute shaders failed, using CPU path\n");
		R_FreeWorldCull();
		return;
	}

	/* Bind the CullParams block on both programs (binding 4); the
	 * binding dies with the program, so vid_restart is covered by
	 * construction.  WC_BindCullParamsBlock also size-checks the block
	 * against the C mirror. */
	WC_BindCullParamsBlock(cull_clear_prog);
	WC_BindCullParamsBlock(cull_mark_prog);

	cull_num_surfs = m->numsurfaces;
	cull_num_leaves = m->numleafs;
	cull_num_buckets = m->numtextures;

	/* Build per-surface GPU data */
	gpu_surfs = (gpu_surface_t *) malloc(cull_num_surfs * sizeof(gpu_surface_t));
	cull_bucket_map = (int *) malloc(cull_num_surfs * sizeof(int));
	if (!gpu_surfs || !cull_bucket_map)
	{
		Con_Printf("GPU world culling: malloc failed for surface data\n");
		free(gpu_surfs); gpu_surfs = NULL;
		R_FreeWorldCull();
		return;
	}
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
		if (surf->texinfo->texture && m->textures)
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
		/* Propagate surface flags; mark lightmap-less surfaces as
		 * DRAWTILED so the compute shader skips them (they'd sample
		 * black from the atlas since they have no lightmap data). */
		gpu_surfs[i].flags = surf->flags;
		if (!surf->samples)
			gpu_surfs[i].flags |= SURF_DRAWTILED;
		/* The mirror's surfaces too, whatever r_mirroralpha: the chain
		 * pass draws them, as a wall at 1 and through R_MirrorChain
		 * below it, and a cull draw would put them opaque over the
		 * reflection (#517).  R_NewMap sets mirrortexturenum first. */
		if (mirrortexturenum >= 0 && mirrortexturenum < m->numtextures &&
		    surf->texinfo->texture == m->textures[mirrortexturenum])
			gpu_surfs[i].flags |= SURF_DRAWTILED;

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
	if (!gpu_marks)
	{
		Con_Printf("GPU world culling: malloc failed for marksurface data\n");
		R_FreeWorldCull();
		return;
	}
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

	/* PVS buffer is streamed per-frame via GL_Upload (uhexen2-o35n) —
	 * no static allocation needed. */

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

	/* Source index SSBO: read-only copy of the world IBO for compute to read
	 * from.  Upload R_BuildWorldVBO's retained CPU array verbatim — do NOT
	 * re-derive the indices here.  A local rebuild used to fan over
	 * surf->polys->numverts, but R_BuildWorldVBO fans over surf->numedges,
	 * and BuildSurfaceDisplayList strips co-linear points from the poly when
	 * gl_keeptjunctions is 0.  When the two disagree, cull_src_ibo no longer
	 * lines up with the surf->vbo_firstindex offsets cull_mark reads with,
	 * and the tail of the buffer is left uninitialised.  (uhexen2-vh5w) */
	{
		extern int world_num_indices;
		extern unsigned int *world_index_data;

		if (world_index_data && world_num_indices > 0)
		{
			glGenBuffers_fp(1, &cull_src_ibo);
			glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_src_ibo);
			glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
					world_num_indices * sizeof(unsigned int),
					world_index_data, GL_STATIC_DRAW);
		}
		else
		{
			Con_Printf("GPU world culling: no world index data, disabling\n");
			R_FreeWorldCull();
			return;
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

	/* Stats buffer: 8 uints, zeroed each dispatch when stats enabled */
	{
		unsigned int zeros[8] = {0};
		glGenBuffers_fp(1, &cull_stats_ssbo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				sizeof(zeros), zeros, GL_DYNAMIC_DRAW);
	}

	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	cull_initialized = true;

	/* Hi-Z reprojection lives across the map change only when the prior
	 * map's last-frame MVP is meaningful here — it isn't, so reset. */
	R_BuildHiZ();

	Con_SafePrintf("GPU world cull: %d surfs, %d marksurfs, %d buckets, %d max indices\n",
		       cull_num_surfs, cull_num_marksurfs, cull_num_buckets, cull_total_indices);
}

void R_FreeWorldCull (void)
{
	if (cull_surf_ssbo) { GL_ForgetBuffer(cull_surf_ssbo); glDeleteBuffers_fp(1, &cull_surf_ssbo); cull_surf_ssbo = 0; }
	if (cull_marksurf_ssbo) { GL_ForgetBuffer(cull_marksurf_ssbo); glDeleteBuffers_fp(1, &cull_marksurf_ssbo); cull_marksurf_ssbo = 0; }
	if (cull_indirect_buf) { GL_ForgetBuffer(cull_indirect_buf); glDeleteBuffers_fp(1, &cull_indirect_buf); cull_indirect_buf = 0; }
	if (cull_src_ibo) { GL_ForgetBuffer(cull_src_ibo); glDeleteBuffers_fp(1, &cull_src_ibo); cull_src_ibo = 0; }
	if (cull_dst_ibo) { GL_ForgetBuffer(cull_dst_ibo); glDeleteBuffers_fp(1, &cull_dst_ibo); cull_dst_ibo = 0; }
	if (cull_dedup_ssbo) { GL_ForgetBuffer(cull_dedup_ssbo); glDeleteBuffers_fp(1, &cull_dedup_ssbo); cull_dedup_ssbo = 0; }
	if (cull_stats_ssbo) { GL_ForgetBuffer(cull_stats_ssbo); glDeleteBuffers_fp(1, &cull_stats_ssbo); cull_stats_ssbo = 0; }
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (cull_clear_prog) { glDeleteProgram_fp(cull_clear_prog); cull_clear_prog = 0; }
	if (cull_mark_prog) { glDeleteProgram_fp(cull_mark_prog); cull_mark_prog = 0; }
	if (cull_bucket_map) { free(cull_bucket_map); cull_bucket_map = NULL; }
	R_FreeHiZ();
	cull_initialized = false;
	cull_total_indices = 0;
}

/* ------------------------------------------------------------------ */
/* Hi-Z occlusion culling — uhexen2-xd87                               */
/* ------------------------------------------------------------------ */

extern qboolean	gl_clipcontrol_able;

static int R_HiZ_MipCount (int w, int h)
{
	int d = (w > h) ? w : h;
	int n = 1;
	while (d > 1) { d >>= 1; n++; }
	return n;
}

static qboolean R_HiZ_Available (void)
{
#ifdef USE_SDL_GPU
	/* Hi-Z is compute, P5 of #340: off under -gpu until it is routed. */
	if (R_GPU ())
		return false;
#endif
	return (gl_hiz_cull.integer != 0)
	    && (glDispatchCompute_fp != NULL)
	    && (glTexStorage2D_fp    != NULL)
	    && (glBindImageTexture_fp != NULL)
	    && (glMemoryBarrier_fp   != NULL);
}

static qboolean R_HiZ_EnsureResources (int scene_w, int scene_h)
{
	if (scene_w <= 0 || scene_h <= 0)
		return false;

	if (hiz_pyramid_tex && hiz_width == scene_w && hiz_height == scene_h)
		return true;

	if (hiz_pyramid_tex)
	{
		R_DeleteTextures (1, &hiz_pyramid_tex);
		hiz_pyramid_tex = 0;
	}

	hiz_width = scene_w;
	hiz_height = scene_h;
	hiz_mip_count = R_HiZ_MipCount(scene_w, scene_h);

	R_GenTextures(1, &hiz_pyramid_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, hiz_pyramid_tex);
	R_TexStorage2D(GL_TEXTURE_2D, hiz_pyramid_tex, hiz_mip_count, GL_R32F, scene_w, scene_h);
	R_TexSetFilter(GL_TEXTURE_2D, hiz_pyramid_tex, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, hiz_pyramid_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	if (!hiz_copy_prog)
	{
		hiz_copy_prog = R_Program_Create(RPROG_HIZ_COPY);
		/* Safe to skip: an unassigned block already defaults to binding
		 * point 0, which is the point we ask for. */
		if (hiz_copy_prog && glGetUniformBlockIndex_fp &&
		    glUniformBlockBinding_fp)
		{
			GLuint blk = glGetUniformBlockIndex_fp(hiz_copy_prog,
							       "HizParams");
			if (blk != GL_INVALID_INDEX)
				glUniformBlockBinding_fp(hiz_copy_prog, blk,
							 HIZ_PARAMS_UBO_BINDING);
		}
	}
	if (!hiz_reduce_prog)
	{
		hiz_reduce_prog = R_Program_Create(RPROG_HIZ_REDUCE);
		WC_BindCullParamsBlock(hiz_reduce_prog);
	}

	if (!hiz_copy_prog || !hiz_reduce_prog)
	{
		Con_Printf("Hi-Z: compute shader failed, disabling\n");
		R_FreeHiZ();
		return false;
	}

	Con_DPrintf("Hi-Z: %dx%d pyramid, %d mips\n",
		    hiz_width, hiz_height, hiz_mip_count);
	return true;
}

void R_BuildHiZ (void)
{
	/* Allocation deferred to R_BuildHiZForNextFrame so we don't burn
	 * GPU memory when gl_hiz_cull is off. */
	memset(hiz_prev_mvp, 0, sizeof(hiz_prev_mvp));
	hiz_prev_mvp_valid = false;
}

void R_FreeHiZ (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (hiz_pyramid_tex)   { R_DeleteTextures (1, &hiz_pyramid_tex); hiz_pyramid_tex = 0; }
	if (hiz_copy_prog)     { glDeleteProgram_fp(hiz_copy_prog);        hiz_copy_prog = 0; }
	if (hiz_reduce_prog)   { glDeleteProgram_fp(hiz_reduce_prog);      hiz_reduce_prog = 0; }
	if (hiz_resolve_fbo)       { glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);   hiz_resolve_fbo = 0; }
	if (hiz_resolve_depth_tex) { R_DeleteTextures (1, &hiz_resolve_depth_tex); hiz_resolve_depth_tex = 0; }
	hiz_resolve_w = hiz_resolve_h = 0;
	hiz_width = hiz_height = hiz_mip_count = 0;
	hiz_prev_mvp_valid = false;
}

/* uhexen2-9912: standalone depth resolve.  When the postprocess pipeline
 * isn't active (no FXAA/HDR/gamma override/etc.), the scene is drawn
 * straight into the default framebuffer and there is no sampler-readable
 * depth texture for Hi-Z to seed from.  Maintain a private depth texture
 * and blit-resolve the default fb's depth into it.  Returns the texture
 * or 0 on failure. */
static GLuint R_HiZ_ResolveStandaloneDepth (int scene_w, int scene_h)
{
	GLenum status;

	if (scene_w <= 0 || scene_h <= 0)
		return 0;

	if (hiz_resolve_depth_tex && hiz_resolve_fbo &&
	    hiz_resolve_w == scene_w && hiz_resolve_h == scene_h)
	{
		/* existing texture matches — just refresh contents */
		goto blit;
	}

	if (hiz_resolve_depth_tex)
	{
		R_DeleteTextures (1, &hiz_resolve_depth_tex);
		hiz_resolve_depth_tex = 0;
	}
	if (hiz_resolve_fbo)
	{
		glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);
		hiz_resolve_fbo = 0;
	}

	R_GenTextures(1, &hiz_resolve_depth_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, hiz_resolve_depth_tex);
	R_TexImage2D(GL_TEXTURE_2D, hiz_resolve_depth_tex, 0, GL_DEPTH_COMPONENT24,
			scene_w, scene_h,
			GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	R_TexSetFilter(GL_TEXTURE_2D, hiz_resolve_depth_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, hiz_resolve_depth_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	glGenFramebuffers_fp(1, &hiz_resolve_fbo);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	glFramebufferTexture2D_fp(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				  GL_TEXTURE_2D, hiz_resolve_depth_tex, 0);
	/* Depth-only FBO needs DRAW_BUFFER = NONE for completeness on strict
	 * drivers.  READ_BUFFER state is irrelevant since we only ever bind
	 * this fbo as the DRAW target of glBlitFramebuffer. */
	{
		GLenum none = GL_NONE;
		glDrawBuffers_fp(1, &none);
	}

	status = glCheckFramebufferStatus_fp(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf("Hi-Z standalone resolve FBO incomplete (0x%x)\n", status);
		glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);
		R_DeleteTextures (1, &hiz_resolve_depth_tex);
		hiz_resolve_fbo = 0;
		hiz_resolve_depth_tex = 0;
		return 0;
	}

	hiz_resolve_w = scene_w;
	hiz_resolve_h = scene_h;

blit:
	/* Resolve default framebuffer depth -> private depth texture.  Default
	 * fb is the source (id 0), our private fbo is the destination.  Blit
	 * is a no-op for non-MSAA → non-MSAA except that it copies the data;
	 * GL_NEAREST is the only valid filter for depth blits. */
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	glBlitFramebuffer_fp(0, 0, scene_w, scene_h,
			     0, 0, scene_w, scene_h,
			     GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, 0);

	return hiz_resolve_depth_tex;
}

void R_BuildHiZForNextFrame (void)
{
	GLuint	scene_depth_tex;
	int	scene_w, scene_h;
	int	mip;

	if (!R_HiZ_Available())
		return;

	scene_depth_tex = GL_PostProcess_GetSceneDepthTex();
	if (scene_depth_tex)
	{
		/* Postprocess is active with a sampler-readable depth (non-MSAA
		 * pp path).  Use the scene size it actually rendered at — may
		 * be scaled down by r_scale < 1. */
		if (!GL_PostProcess_GetSceneSize(&scene_w, &scene_h))
			return;
	}
	else if (!GL_PostProcess_Active())
	{
		/* uhexen2-9912: postprocess inactive (no FXAA/HDR/gamma/etc.) —
		 * scene went straight to the default framebuffer.  Resolve its
		 * depth to our private texture so the pyramid build still runs.
		 * (When pp IS active but has no depth texture — MSAA pp path —
		 * the scene is in pp_fbo, not the default fb; reading default
		 * fb would give stale data, so skip silently.) */
		extern int glwidth, glheight;
		scene_w = glwidth;
		scene_h = glheight;
		scene_depth_tex = R_HiZ_ResolveStandaloneDepth(scene_w, scene_h);
		if (!scene_depth_tex)
			return;
	}
	else
	{
		/* pp_active && no depth texture — MSAA postprocess path.  No
		 * sampler-readable depth available; cull-mark will run without
		 * Hi-Z this frame. */
		return;
	}

	if (!R_HiZ_EnsureResources(scene_w, scene_h))
		return;

	/* Copy scene depth -> mip 0 */
	R_UseProgram (hiz_copy_prog);
	R_BindTextureSlot (0, scene_depth_tex);
	R_BindImageTexture (0, hiz_pyramid_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
	{
		/* Pushed unconditionally every dispatch.  u_size used to be a
		 * loose uniform, which is per-PROGRAM state and so survived
		 * between frames on its own; a block member is global and any
		 * future writer of this binding point would silently win. */
		hiz_params_t params;
		GLuint ubo;
		GLintptr ofs;

		params.size[0] = scene_w;
		params.size[1] = scene_h;
		params.size[2] = 0;
		params.size[3] = 0;
		GL_Upload(GL_UNIFORM_BUFFER, &params, sizeof(params), &ubo, &ofs);
		GL_BindBufferRange(GL_UNIFORM_BUFFER, HIZ_PARAMS_UBO_BINDING,
				   ubo, ofs, sizeof(params));
	}
	glDispatchCompute_fp((scene_w + 7) / 8, (scene_h + 7) / 8, 1);
	glMemoryBarrier_fp(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* Reduce mip N -> mip N+1 */
	R_UseProgram (hiz_reduce_prog);
	R_BindTextureSlot (0, hiz_pyramid_tex);
	{
		/* Per-mip CullParams upload (the HizParams pattern above):
		 * zero-filled so the members this program never reads are
		 * zero, never stale ring data. */
		int src_w = scene_w, src_h = scene_h;
		for (mip = 1; mip < hiz_mip_count; mip++)
		{
			int dst_w = src_w > 1 ? src_w >> 1 : 1;
			int dst_h = src_h > 1 ? src_h >> 1 : 1;
			cull_params_t params;
			GLuint ubo;
			GLintptr ofs;

			R_BindImageTexture (0, hiz_pyramid_tex, mip, GL_FALSE, 0,
					    GL_WRITE_ONLY, GL_R32F);
			memset(&params, 0, sizeof(params));
			params.reduce[0] = src_w;
			params.reduce[1] = src_h;
			params.reduce[2] = dst_w;
			params.reduce[3] = dst_h;
			params.reduce2[0] = mip - 1;
			params.reduce2[1] = gl_clipcontrol_able ? 1 : 0;
			GL_Upload(GL_UNIFORM_BUFFER, &params, sizeof(params), &ubo, &ofs);
			GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
					   ubo, ofs, sizeof(params));
			glDispatchCompute_fp((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
			glMemoryBarrier_fp(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
			src_w = dst_w;
			src_h = dst_h;
		}
	}

	R_UseProgram (0);
	R_BindTextureSlot (0, 0);
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
	/* Hoisted for the CullParams upload below: the Hi-Z gate, the stats
	 * gate and the frustum planes are each computed in their own block
	 * but written into the mirror in one place, before the dispatch. */
	qboolean	hiz_on, stats_on;
	float		frust[16];	/* 4 vec4s */

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

	/* Stream PVS into this frame's host ring slot (uhexen2-o35n).
	 * Persistent-mapped when available, falls back to BufferSubData. */
	{
		GLuint		vis_buf;
		GLintptr	vis_ofs;
		GLsizeiptr	vis_bytes = vis_size * sizeof(unsigned int);
		GL_Upload(GL_SHADER_STORAGE_BUFFER, vis_bits, vis_bytes,
			  &vis_buf, &vis_ofs);
		GL_BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2,
				   vis_buf, vis_ofs, vis_bytes);
	}

	/* Pass 1: clear indirect buffer */
	R_UseProgram (cull_clear_prog);
	{
		/* CullParams, zero-filled (the HizParams pattern): only
		 * u_ccounts0.z is read by this program. */
		cull_params_t params;
		GLuint ubo;
		GLintptr ofs;

		memset(&params, 0, sizeof(params));
		params.counts0[2] = cull_num_buckets;
		GL_Upload(GL_UNIFORM_BUFFER, &params, sizeof(params), &ubo, &ofs);
		GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
				   ubo, ofs, sizeof(params));
	}
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	glDispatchCompute_fp((cull_num_buckets + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_SHADER_STORAGE_BARRIER_BIT);

	/* Pass 2: cull + mark surfaces */
	R_UseProgram (cull_mark_prog);

	/* Hi-Z occlusion (uhexen2-xd87).  Sampler binding 1 = pyramid; the
	 * enable flag is also gated by gl_hiz_cull/prev_mvp_valid CPU-side
	 * so shaders early-out cheaply when not in use. */
	hiz_on = gl_hiz_cull.integer && hiz_pyramid_tex && hiz_prev_mvp_valid;
	if (hiz_on)
		R_BindTextureSlot (1, hiz_pyramid_tex);

	/* Frustum planes (negate: shader uses dot(n,p)+d, engine uses
	 * dot(n,p)-dist). */
	{
		extern mplane_t frustum[4];
		for (i = 0; i < 4; i++)
		{
			frust[i*4+0] = frustum[i].normal[0];
			frust[i*4+1] = frustum[i].normal[1];
			frust[i*4+2] = frustum[i].normal[2];
			frust[i*4+3] = -frustum[i].dist;
		}
	}

	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cull_surf_ssbo);
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, cull_marksurf_ssbo);
	/* binding 2 already bound via GL_BindBufferRange above (PVS upload). */
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, cull_src_ibo);
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, cull_dst_ibo);
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, cull_dedup_ssbo);

	/* Stats counters (uhexen2-cyu0).  Bind always; gating happens via
	 * u_ccounts1.w in the shader so we don't pay for atomicAdds when
	 * gl_hiz_stats is 0.  When enabled, zero the buffer before dispatch
	 * so the readback sees only this frame's contributions. */
	stats_on = (gl_hiz_stats.integer > 0) && cull_stats_ssbo;
	if (stats_on)
	{
		unsigned int zeros[8] = {0};
		GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
		glBufferSubData_fp(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
	}
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, cull_stats_ssbo);

	/* CullParams: one per-dispatch upload, zero-filled so the members
	 * this program never reads are zero, never stale ring data (the
	 * HizParams pattern). */
	{
		cull_params_t params;
		GLuint ubo;
		GLintptr ofs;

		memset(&params, 0, sizeof(params));
		params.counts0[0] = framecount;
		params.counts0[1] = cull_num_marksurfs;
		params.counts0[2] = cull_num_buckets;
		params.counts0[3] = cull_total_indices;
		params.counts1[0] = hiz_mip_count;
		params.counts1[1] = hiz_on ? 1 : 0;
		params.counts1[2] = gl_clipcontrol_able ? 1 : 0;
		params.counts1[3] = stats_on ? 1 : 0;
		params.vieworg[0] = r_origin[0];
		params.vieworg[1] = r_origin[1];
		params.vieworg[2] = r_origin[2];
		memcpy(params.frustum, frust, sizeof(params.frustum));
		memcpy(params.prevmvp, hiz_prev_mvp, sizeof(params.prevmvp));
		params.hizsize[0] = (float)hiz_width;
		params.hizsize[1] = (float)hiz_height;
		GL_Upload(GL_UNIFORM_BUFFER, &params, sizeof(params), &ubo, &ofs);
		GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
				   ubo, ofs, sizeof(params));
	}

	glDispatchCompute_fp((cull_num_marksurfs + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_COMMAND_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT |
			   GL_SHADER_STORAGE_BARRIER_BIT);

	/* Gated stats readback: only every gl_hiz_stats frames, so the
	 * implied glGetBufferSubData stall amortizes.  Print to console;
	 * caller decides whether to leave running or set 0 after reading. */
	if (gl_hiz_stats.integer > 0 && cull_stats_ssbo && glGetBufferSubData_fp)
	{
		hiz_stats_frame_counter++;
		if (hiz_stats_frame_counter >= gl_hiz_stats.integer)
		{
			unsigned int counts[8] = {0};
			unsigned int post;
			hiz_stats_frame_counter = 0;
			GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
			glGetBufferSubData_fp(GL_SHADER_STORAGE_BUFFER, 0,
					      sizeof(counts), counts);
			post = counts[4] + counts[5] + counts[6];
			Con_Printf("hiz_stats: total=%u pvs=%u flags=%u frust=%u "
				   "hiz=%u accepted=%u dedup=%u\n",
				   counts[0], counts[1], counts[2], counts[3],
				   counts[4], counts[5], counts[6]);
			if (post > 0)
				Con_Printf("  hiz cull rate = %.1f%% of post-frustum "
					   "(%u / %u; gate %.1f%%)\n",
					   100.0 * counts[4] / (double)post,
					   counts[4], post, 10.0);
			else
				Con_Printf("  hiz cull rate = (no post-frustum surfaces)\n");
		}
	}

	R_UseProgram (0);
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

	if (!cull_initialized || !cull_dst_ibo || cull_total_indices <= 0)
		return;

	/* This path has no lightmap of its own — it binds the atlas below as the
	 * world shader's lightmap sampler, and world_vao's UVs are only in atlas
	 * space when the atlas is on.  Drawing without it renders the whole
	 * opaque world unlit, since an unbound sampler2D reads black.  The
	 * caller already declines to set gpu_cull_active in that case; this is
	 * the invariant stated where it is actually relied upon.  uhexen2-qt7s. */
	if (!lm_atlas_texture)
		return;

	/* Bind world VAO but replace its IBO with the compute-written dest IBO */
	glBindVertexArray_fp(world_vao);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, cull_dst_ibo);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

	/* Set up world shader.  MDI cull path emits opaque world only — fence
	 * surfaces flow through R_DrawBrushInstanced's fence sub-pass — so use
	 * the early_fragment_tests variant for Hi-Z.  uhexen2-5c6r. */
	{
		glprogram_t *prog = &gl_shader_world_opaque;
		R_UseProgram (prog->program);
		GL_GetMVP(mvp);
		GL_GetModelview(mv);
		/* sworld_opaque_frag's whole set from FragParams and WorldFrag
		 * (#450 W2), the fog / alpha / overbright this path used to upload
		 * by hand among it; then sworld_vert's pair from VertParams (W1).
		 * The texture binds between the flush and the indirect draws write
		 * no block. */
		R_SetWorldFrag (r_fog_density, r_fog_color, 0.01f, 1.0f,
				R_WorldOverbright (), 0.0f, 0.0f);
		R_SetMVP (mvp);
		R_SetModelView (mv);
		R_FlushUniforms ();
	}

	/* Bind lightmap atlas on unit 1, default fb null at unit 2 */
	{
		const GLuint	units12[2] = { lm_atlas_texture, gl_null_fb_texture };
		R_BindTextures (1, 2, units12);
	}

	/* Draw each texture bucket via indirect commands */
	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);

	for (i = 0; i < cull_num_buckets; i++)
	{
		texture_t *t = (cl.worldmodel->textures) ? cl.worldmodel->textures[i] : NULL;
		if (!t)
			continue;

		/* Bind diffuse texture + matching fullbright mask (uhexen2-sjvf) */
		R_BindTextureSlot (0, t->gl_texturenum);
		R_BindTextureSlot (2, t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture);

		/* Issue indirect draw for this bucket */
		glDrawElementsIndirect_fp(GL_TRIANGLES, GL_UNSIGNED_INT,
					  (void *)((size_t)i * sizeof(gpu_indirect_cmd_t)));
	}

	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, 0);

	/* Restore original world IBO in the VAO so later code that
	 * binds world_vao gets the correct element buffer */
	{
		extern GLuint world_ibo;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, world_ibo);
	}

	glBindVertexArray_fp(0);
	R_UseProgram (0);

	/* Capture this frame's world MVP for next-frame Hi-Z reprojection.
	 * The pyramid will be built at end-of-3D (after every depth write
	 * has landed), and the cull dispatch one frame from now consults
	 * this matrix when reprojecting AABBs onto the pyramid. */
	memcpy(hiz_prev_mvp, mvp, sizeof(hiz_prev_mvp));
	hiz_prev_mvp_valid = true;
}


#else	/* USE_GLES — GPU culling needs compute shaders and SSBOs, which
	 * WebGL2 lacks, so the whole module is compiled out.  Only the
	 * shutdown hook is reachable from live code (gl_vidsdl.c); stub it so
	 * VID_Shutdown links without an ifdef at the call site, matching the
	 * gl_buffer.c treatment. */

void R_FreeWorldCull (void) {}

#endif /* !USE_GLES */
#endif /* GLQUAKE */
