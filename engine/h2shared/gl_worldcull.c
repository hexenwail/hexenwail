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
#include "gl_fbo.h"	/* the Hi-Z depth resolve, through the seam (#340 P5a-1) */
#include "gl_compute.h"	/* dispatch, barriers, readback, program deletes (#340 P5a-1) */
#ifdef USE_SDL_GPU
#include "rg_device.h"	/* R_GPU (), the CullParams mirror, the stats readback (#340 P5c) */
#define WC_GPU()	R_GPU ()
#else
#define WC_GPU()	false
#endif

/* The -gpu standalone depth copy's format; older GL headers (mingw's) lack
 * them, as gl_postprocess.c has them for the scene FBO. */
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
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
#ifdef USE_SDL_GPU
/* -gpu's copy into mip 0 is a draw (#340 P5f, hiz_copy_frag.glsl): into
 * hiz_copy_fbo, whose colour attachment is the pyramid's level 0, with
 * ssao_composite.vert's attribute-less triangle.  The program outlives a map
 * (-gpu keeps graphics programs; R_HiZ_Shutdown forgets it at vid_restart). */
static GLuint	hiz_copy_draw_prog;
static GLuint	hiz_copy_vao;
static GLuint	hiz_copy_fbo;
#endif
static int	hiz_width;		/* mip 0 size in pixels */
static int	hiz_height;
static int	hiz_mip_count;
static float	hiz_prev_mvp[16];	/* world MVP captured at end of last 3D pass */
static qboolean	hiz_prev_mvp_valid;
/* The glDepthRange hiz_prev_mvp drew with.  At r_mirroralpha != 1, on any
 * map, R_Clear puts the view into half the window-Z range, so the world's
 * depth in the pyramid lies in that half, and an unscaled [0, 1] surface
 * depth compared against it over-rejected: 306 of 493 post-frustum surfaces
 * against 164 at r_mirroralpha 1 (tools/hiz-mirror-check.sh, #533). */
static float	hiz_prev_depthrange[2] = { 0.0f, 1.0f };
/* The pyramid holds the depth of the frame hiz_prev_mvp was captured in.  A
 * frame whose build was skipped (the MSAA scene FBO has no sampler-readable
 * depth, a resize failed) leaves an older frame's depth behind, and culling
 * against it with this frame's matrix hides surfaces that are in view: what
 * a runtime toggle that raises the post chain (r_ssao, gamma, #539) did. */
static qboolean	hiz_pyramid_valid;

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
static int	cull_drawn_buckets;	/* those with a texture: R_DrawWorldCulled's draws */
static int	cull_num_leaves;	/* total BSP leaves */

static int	*cull_bucket_map;	/* surface index -> texture bucket */
/* What the passes were given, kept for worldcull_verify (#340 P5c): the
 * surface and marksurface arrays as uploaded, each bucket's first index, and
 * the last dispatch's view leaf, frustum and Hi-Z switch. */
static gpu_surface_t	*cull_cpu_surfs;
static gpu_marksurf_t	*cull_cpu_marks;
static unsigned int	*cull_bucket_first;
static mleaf_t		*wc_last_leaf;
static float		wc_last_frust[16];
static qboolean		wc_last_hiz;
/* and, with Hi-Z on, what its test compared against (#340 P5d) */
static float		wc_last_prevmvp[16], wc_last_depthrange[2];
static int		wc_last_hizw, wc_last_hizh, wc_last_mips;
static qboolean		wc_last_rz;
/* worldcull_verify asked with Hi-Z on: it runs right after the next mark
 * dispatch, while the pyramid is still the one that dispatch read -- the
 * frame's end rebuilds it (#340 P5d). */
static qboolean		wc_verify_pending;
static void		WC_Verify (void);

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

/* What hiz_copy reads: GL uploads it, -gpu pushes it as the registered
 * mirror of the block (#340 P5d, the CullParams pattern below). */
static hiz_params_t	hiz_params;

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
	float	depthrange[4];	/* x near, y far: glDepthRange of the pyramid's frame, zw unused */
} cull_params_t;		/* 240 bytes */

COMPILE_TIME_ASSERT(cullparams_size, sizeof(cull_params_t) == 240);

/* What each cull dispatch reads: GL uploads it, -gpu pushes it as the
 * registered mirror of the block (#340 P5c, the LcParams pattern). */
static cull_params_t	cull_params;

#ifdef USE_SDL_GPU
/* Under -gpu, whether this frame's two cull dispatches both ran: one the
 * backend dropped leaves the indirect buffer as an older frame left it
 * (counts the clear never reset, or no counts at all), so the indirect
 * draws are skipped and the chain pass draws the world alone (#340 P5c). */
static qboolean		wc_cull_ran;
static unsigned long	wc_cull_skipped;
/* gl_hiz_stats' asynchronous readbacks, oldest first (RG_ReadBufferAsync). */
#define WC_STATS_PENDING	8
static unsigned int	wc_stats_ticket[WC_STATS_PENDING];
static int		wc_stats_npending;
#endif


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

#ifdef USE_SDL_GPU
	/* The SPIR-V fixes the block's slot; each dispatch pushes the mirror
	 * by the block's name (#340 P5c). */
	if (R_GPU ())
	{
		RG_UniformBlockRegister ("CullParams", &cull_params, sizeof(cull_params));
		return;
	}
#endif
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

/* hiz_copy's HizParams block to its binding point.  Safe to skip: an
 * unassigned block already defaults to binding point 0, which is the point
 * we ask for. */
static void WC_BindHizParamsBlock (GLuint program)
{
	GLuint	blk;

#ifdef USE_SDL_GPU
	/* The SPIR-V fixes the block's slot; each dispatch pushes the mirror
	 * by the block's name (#340 P5d). */
	if (R_GPU ())
	{
		RG_UniformBlockRegister ("HizParams", &hiz_params, sizeof(hiz_params));
		return;
	}
#endif
	if (!program || !glGetUniformBlockIndex_fp || !glUniformBlockBinding_fp)
		return;
	blk = glGetUniformBlockIndex_fp(program, "HizParams");
	if (blk != GL_INVALID_INDEX)
		glUniformBlockBinding_fp(program, blk, HIZ_PARAMS_UBO_BINDING);
}

void R_BuildWorldCull (void)
{
	qmodel_t	*m = cl.worldmodel;
	msurface_t	*surf;
	mleaf_t		*leaf;
	gpu_surface_t	*gpu_surfs;
	gpu_marksurf_t	*gpu_marks;
	int		i, j, ms_count;

	/* Under -gpu the null table's glDispatchCompute is not NULL, and the
	 * dispatches, the readback and the indirect draw are routed (#340 P5c). */
	if (!m || !glDispatchCompute_fp)
		return;

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
	/* u_hiz_pyramid reads unit 1, which TEXBIND2(1, 0) already says to GL;
	 * the Vulkan SPIR-V keeps only its slot, so -gpu is told (#340 P5c). */
	{
		GLint	loc;

		R_UseProgram (cull_mark_prog);
		loc = R_ProgramSamplerLoc (cull_mark_prog, "u_hiz_pyramid");
		R_ProgramSamplerUnit (cull_mark_prog, loc, "u_hiz_pyramid", 1);
		R_UseProgram (0);
	}

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
	R_GenBuffers(1, &cull_surf_ssbo);
	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_surf_ssbo);
	R_BufferData(GL_SHADER_STORAGE_BUFFER,
			cull_num_surfs * sizeof(gpu_surface_t),
			gpu_surfs, GL_STATIC_DRAW);
	cull_cpu_surfs = gpu_surfs;	/* worldcull_verify's; R_FreeWorldCull frees it */

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

	R_GenBuffers(1, &cull_marksurf_ssbo);
	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_marksurf_ssbo);
	R_BufferData(GL_SHADER_STORAGE_BUFFER,
			cull_num_marksurfs * sizeof(gpu_marksurf_t),
			gpu_marks, GL_STATIC_DRAW);
	cull_cpu_marks = gpu_marks;	/* as cull_cpu_surfs */

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
		cull_bucket_first = (unsigned int *) calloc(cull_num_buckets, sizeof(unsigned int));
		offset = 0;
		cull_drawn_buckets = 0;
		for (i = 0; i < cull_num_buckets; i++)
			cull_drawn_buckets += (m->textures && m->textures[i]) ? 1 : 0;
		for (i = 0; i < cull_num_buckets; i++)
		{
			cmds[i].count = 0;		/* filled by compute each frame */
			cmds[i].instanceCount = 1;
			cmds[i].firstIndex = offset;	/* fixed region start */
			if (cull_bucket_first)
				cull_bucket_first[i] = offset;
			cmds[i].baseVertex = 0;
			cmds[i].baseInstance = 0;
			offset += bucket_max[i];
		}
		cull_total_indices = offset;
		free(bucket_max);

		R_GenBuffers(1, &cull_indirect_buf);
		GL_BindBuffer(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);
		R_BufferData(GL_DRAW_INDIRECT_BUFFER,
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
			R_GenBuffers(1, &cull_src_ibo);
			GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_src_ibo);
			R_BufferData(GL_SHADER_STORAGE_BUFFER,
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
	 * Sized to hold the worst case (all surfaces visible).  Under -gpu made
	 * through the element target, so SDL_GPU reads it as indices after the
	 * pass that writes it, which a buffer made through any other target
	 * would be read unsynchronised as (rg_buffer.c RGB_Usage, #340 P5c).
	 * The element target is the bound VAO's: VAO 0's here, which the bind
	 * to 0 below empties again. */
	if (cull_total_indices > 0)
	{
		GLenum	dst_target = GL_SHADER_STORAGE_BUFFER;

#ifdef USE_SDL_GPU
		if (R_GPU ())
		{
			dst_target = GL_ELEMENT_ARRAY_BUFFER;
			R_BindVertexArray(0);
		}
#endif
		R_GenBuffers(1, &cull_dst_ibo);
		GL_BindBuffer(dst_target, cull_dst_ibo);
		R_BufferData(dst_target,
				cull_total_indices * sizeof(unsigned int),
				NULL, GL_DYNAMIC_DRAW);
	}

	/* Dedup buffer: one int per surface, atomicExchange'd with framecount */
	{
		int *zeros = (int *) calloc(cull_num_surfs, sizeof(int));
		R_GenBuffers(1, &cull_dedup_ssbo);
		GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_dedup_ssbo);
		R_BufferData(GL_SHADER_STORAGE_BUFFER,
				cull_num_surfs * sizeof(int),
				zeros, GL_DYNAMIC_DRAW);
		free(zeros);
	}

	/* Stats buffer: 8 uints, zeroed each dispatch when stats enabled */
	{
		unsigned int zeros[8] = {0};
		R_GenBuffers(1, &cull_stats_ssbo);
		GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
		R_BufferData(GL_SHADER_STORAGE_BUFFER,
				sizeof(zeros), zeros, GL_DYNAMIC_DRAW);
	}

	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	GL_BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	cull_initialized = true;

	/* Hi-Z reprojection lives across the map change only when the prior
	 * map's last-frame MVP is meaningful here — it isn't, so reset. */
	R_BuildHiZ();

	Con_SafePrintf("GPU world cull: %d surfs, %d marksurfs, %d buckets (%d with a texture, "
		       "drawn), %d max indices\n", cull_num_surfs, cull_num_marksurfs, cull_num_buckets,
		       cull_drawn_buckets, cull_total_indices);
}

void R_FreeWorldCull (void)
{
	if (cull_surf_ssbo) { GL_ForgetBuffer(cull_surf_ssbo); R_DeleteBuffers(1, &cull_surf_ssbo); cull_surf_ssbo = 0; }
	if (cull_marksurf_ssbo) { GL_ForgetBuffer(cull_marksurf_ssbo); R_DeleteBuffers(1, &cull_marksurf_ssbo); cull_marksurf_ssbo = 0; }
	if (cull_indirect_buf) { GL_ForgetBuffer(cull_indirect_buf); R_DeleteBuffers(1, &cull_indirect_buf); cull_indirect_buf = 0; }
	if (cull_src_ibo) { GL_ForgetBuffer(cull_src_ibo); R_DeleteBuffers(1, &cull_src_ibo); cull_src_ibo = 0; }
	if (cull_dst_ibo) { GL_ForgetBuffer(cull_dst_ibo); R_DeleteBuffers(1, &cull_dst_ibo); cull_dst_ibo = 0; }
	if (cull_dedup_ssbo) { GL_ForgetBuffer(cull_dedup_ssbo); R_DeleteBuffers(1, &cull_dedup_ssbo); cull_dedup_ssbo = 0; }
	if (cull_stats_ssbo) { GL_ForgetBuffer(cull_stats_ssbo); R_DeleteBuffers(1, &cull_stats_ssbo); cull_stats_ssbo = 0; }
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (cull_clear_prog) { R_DeleteProgram(cull_clear_prog); cull_clear_prog = 0; }
	if (cull_mark_prog) { R_DeleteProgram(cull_mark_prog); cull_mark_prog = 0; }
	if (cull_bucket_map) { free(cull_bucket_map); cull_bucket_map = NULL; }
	free(cull_cpu_surfs); cull_cpu_surfs = NULL;
	free(cull_cpu_marks); cull_cpu_marks = NULL;
	free(cull_bucket_first); cull_bucket_first = NULL;
	wc_last_leaf = NULL;
	wc_verify_pending = false;
	R_FreeHiZ();
	cull_initialized = false;
	cull_total_indices = 0;
	cull_drawn_buckets = 0;
#ifdef USE_SDL_GPU
	/* reads still in flight are the old map's: their slots are freed as
	 * they land, so the new map's requests find room */
	while (wc_stats_npending > 0)
		RG_ReadbackDiscard (wc_stats_ticket[--wc_stats_npending]);
	wc_cull_ran = false;
#endif
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
	/* A scene depth the device cannot sample binds a placeholder, whose
	 * zeros are reversed-Z's far plane: a pyramid that culls nothing, paid
	 * for every frame (#340 P5d; RG_DepthSampled, P4b). */
	if (R_GPU () && !RG_DepthSampled ())
		return false;
#endif
	return (gl_hiz_cull.integer != 0)
	    && (glDispatchCompute_fp != NULL)
	    && (glTexStorage2D_fp    != NULL)
	    && (glBindImageTexture_fp != NULL)
	    && (glMemoryBarrier_fp   != NULL);
}

#ifdef USE_SDL_GPU
/* -gpu: the pyramid just made gets its read-only storage use declared, and
 * hiz_copy_fbo its level 0 (#340 P5f); the program and the VAO once. */
static qboolean WC_HiZCopyDrawEnsure (void)
{
	GLuint	prev;
	GLenum	status;

	/* the reduce's writes too: the copy's draw makes the texture now */
	R_BindImageTexture (0, hiz_pyramid_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
	R_BindImageTexture (0, hiz_pyramid_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
	R_BindImageTexture (0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
	if (!hiz_copy_draw_prog)
	{
		GLint	loc;

		hiz_copy_draw_prog = R_Program_Create (RPROG_HIZ_COPY_DRAW);
		if (!hiz_copy_draw_prog)
			return false;
		R_UseProgram (hiz_copy_draw_prog);
		loc = R_ProgramSamplerLoc (hiz_copy_draw_prog, "u_scene_depth");
		R_ProgramSamplerUnit (hiz_copy_draw_prog, loc, "u_scene_depth", 0);
		R_UseProgram (0);
	}
	if (!hiz_copy_vao)
		R_GenVertexArrays (1, &hiz_copy_vao);
	if (!hiz_copy_fbo)
		R_GenFramebuffers (1, &hiz_copy_fbo);
	prev = R_FramebufferBinding (GL_DRAW_FRAMEBUFFER_BINDING);
	R_BindFramebuffer (GL_DRAW_FRAMEBUFFER, hiz_copy_fbo);
	R_FramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hiz_pyramid_tex, 0);
	status = R_CheckFramebufferStatus (GL_DRAW_FRAMEBUFFER);
	R_BindFramebuffer (GL_DRAW_FRAMEBUFFER, prev);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf ("Hi-Z: the depth copy's framebuffer is incomplete (0x%x)\n", status);
		return false;
	}
	return true;
}

/* -gpu: the scene depth into mip 0 by a draw (hiz_copy_frag.glsl).  The
 * caller's framebuffer, viewport, cull, blend and depth state are put back;
 * the colour mask is left all on, the scissor off, no program and no VAO
 * bound, which is what every caller here has (the end of the 3D pass). */
static void WC_HiZCopyDraw (GLuint scene_depth_tex, int scene_w, int scene_h)
{
	GLint		vp[4];
	GLuint		prev = R_FramebufferBinding (GL_DRAW_FRAMEBUFFER_BINDING);
	qboolean	cull = R_GetCull (), blend = R_GetBlend ();
	qboolean	depthtest = R_GetDepthTest (), depthmask = R_GetDepthMask ();

	R_GetViewport (vp);
	R_BindFramebuffer (GL_DRAW_FRAMEBUFFER, hiz_copy_fbo);
	R_SetViewport (0, 0, scene_w, scene_h);
	R_SetCull (false);
	R_SetBlend (false);
	R_SetDepthTest (false);
	R_SetDepthMask (false);
	R_SetColorMask (true, true, true, true);
	GL_ResetScissor ();
	R_UseProgram (hiz_copy_draw_prog);
	R_BindTextureSlot (0, scene_depth_tex);
	R_BindVertexArray (hiz_copy_vao);
	R_DrawArrays (GL_TRIANGLES, 0, 3);
	R_BindVertexArray (0);
	R_BindTextureSlot (0, 0);
	R_UseProgram (0);
	R_SetDepthMask (depthmask);
	R_SetDepthTest (depthtest);
	R_SetBlend (blend);
	R_SetCull (cull);
	R_BindFramebuffer (GL_DRAW_FRAMEBUFFER, prev);
	R_SetViewport (vp[0], vp[1], vp[2], vp[3]);
}
#endif

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
	/* True on GL; under -gpu a device that cannot write R32F from compute
	 * runs without Hi-Z (decision 12, #340 P5d). */
	if (!R_StorageFormatOk (GL_R32F, GL_TEXTURE_2D, GL_WRITE_ONLY))
	{
		static int	warned;

		if (!warned++)
			Con_Printf ("Hi-Z: this device cannot write R32F storage images, Hi-Z is off\n");
		return false;
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
#ifdef USE_SDL_GPU
	/* -gpu reads the pyramid as a read-only storage texture, in the
	 * reduce and the cull (#340 P5f); SDL_GPU fixes a texture's usage when
	 * it is made, so that is declared before anything makes it. */
	if (R_GPU () && !WC_HiZCopyDrawEnsure ())
	{
		Con_Printf("Hi-Z: the depth copy's draw cannot be set up, disabling\n");
		R_FreeHiZ();
		return false;
	}
#endif

	/* -gpu copies into mip 0 with a draw instead (WC_HiZCopyDrawEnsure) */
	if (!hiz_copy_prog && !WC_GPU ())
	{
		hiz_copy_prog = R_Program_Create(RPROG_HIZ_COPY);
		WC_BindHizParamsBlock(hiz_copy_prog);
	}
	if (!hiz_reduce_prog)
	{
		hiz_reduce_prog = R_Program_Create(RPROG_HIZ_REDUCE);
		WC_BindCullParamsBlock(hiz_reduce_prog);
	}

	if ((!hiz_copy_prog && !WC_GPU ()) || !hiz_reduce_prog)
	{
		Con_Printf("Hi-Z: compute shader failed, disabling\n");
		R_FreeHiZ();
		return false;
	}
	/* The reduce reads unit 0, which TEXBIND(0) already says to GL; the
	 * Vulkan SPIR-V keeps only the slot, so -gpu is told (#340 P5d), and
	 * -gpu alone, so GL's calls are what they were before (P5e). */
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		GLint	loc;

		R_UseProgram (hiz_reduce_prog);
		loc = R_ProgramSamplerLoc (hiz_reduce_prog, "u_src");
		R_ProgramSamplerUnit (hiz_reduce_prog, loc, "u_src", 0);
		R_UseProgram (0);
	}
#endif

	Con_DPrintf("Hi-Z: %dx%d pyramid, %d mips\n",
		    hiz_width, hiz_height, hiz_mip_count);
	return true;
}

void R_BuildHiZ (void)
{
	/* Allocation deferred to R_BuildHiZForNextFrame so we don't burn
	 * GPU memory when gl_hiz_cull is off. */
	memset(hiz_prev_mvp, 0, sizeof(hiz_prev_mvp));
	hiz_prev_depthrange[0] = 0.0f;
	hiz_prev_depthrange[1] = 1.0f;
	hiz_prev_mvp_valid = false;
}

/* A view frame drawn without the cull (r_gpucull 0, gl_cull 0 on a mirror
 * map, ...) still gets its pyramid built, but keeps no MVP or depth range:
 * the ones kept are an older frame's, and the next culled frame would pair
 * them with this frame's pyramid -- an r_mirroralpha change in between is a
 * half-range pyramid against a [0, 1] range.  Drop them; that frame runs
 * without Hi-Z.  Not called for the reflected pass, which follows the view
 * in the same frame and must leave the view's kept. */
void R_HiZ_ForgetView (void)
{
	hiz_prev_mvp_valid = false;
}

void R_FreeHiZ (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
#ifdef USE_SDL_GPU
	if (hiz_copy_fbo)      { R_DeleteFramebuffers(1, &hiz_copy_fbo); hiz_copy_fbo = 0; }
#endif
	if (hiz_pyramid_tex)   { R_DeleteTextures (1, &hiz_pyramid_tex); hiz_pyramid_tex = 0; }
	if (hiz_copy_prog)     { R_DeleteProgram(hiz_copy_prog);        hiz_copy_prog = 0; }
	if (hiz_reduce_prog)   { R_DeleteProgram(hiz_reduce_prog);      hiz_reduce_prog = 0; }
	if (hiz_resolve_fbo)       { R_DeleteFramebuffers(1, &hiz_resolve_fbo);   hiz_resolve_fbo = 0; }
	if (hiz_resolve_depth_tex) { R_DeleteTextures (1, &hiz_resolve_depth_tex); hiz_resolve_depth_tex = 0; }
	hiz_resolve_w = hiz_resolve_h = 0;
	hiz_width = hiz_height = hiz_mip_count = 0;
	hiz_prev_mvp_valid = false;
	hiz_pyramid_valid = false;
}

/* At video shutdown, after R_FreeWorldCull: what R_FreeHiZ keeps for the
 * next map.  -gpu's copy program is not deleted, only forgotten -- the
 * device going away takes every program with it, and -gpu deletes no
 * graphics program (#340 P5f; vid_restart is refused under -gpu, so this
 * runs as the device goes).  GL never makes it. */
void R_HiZ_Shutdown (void)
{
#ifdef USE_SDL_GPU
	hiz_copy_draw_prog = 0;
	if (hiz_copy_vao) { R_DeleteVertexArrays (1, &hiz_copy_vao); hiz_copy_vao = 0; }
#endif
}

/* uhexen2-9912: standalone depth resolve.  When the postprocess pipeline
 * isn't active (no FXAA/HDR/gamma override/etc.), the scene is drawn
 * straight into the default framebuffer and there is no sampler-readable
 * depth texture for Hi-Z to seed from.  Maintain a private depth texture
 * and blit-resolve the default fb's depth into it.  Returns the texture
 * or 0 on failure. */
static GLuint R_HiZ_ResolveStandaloneDepth (int scene_w, int scene_h)
{
	GLenum	status;
	GLenum	fmt = GL_DEPTH_COMPONENT24, attach = GL_DEPTH_ATTACHMENT;
	qboolean	ok;

	if (scene_w <= 0 || scene_h <= 0)
		return 0;
#ifdef USE_SDL_GPU
	/* Framebuffer 0 is a render target there, in the scene's own depth
	 * format, which GL_DEPTH24_STENCIL8 names (rg_resource.c RGT_Format);
	 * the backend copies depth only between textures of one format, the
	 * GL_SoftDepth_Capture arrangement (#340 P4b, P5d). */
	if (R_GPU ())
	{
		fmt = GL_DEPTH24_STENCIL8;
		attach = GL_DEPTH_STENCIL_ATTACHMENT;
	}
#endif

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
		R_DeleteFramebuffers(1, &hiz_resolve_fbo);
		hiz_resolve_fbo = 0;
	}

	R_GenTextures(1, &hiz_resolve_depth_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, hiz_resolve_depth_tex);
	R_TexImage2D(GL_TEXTURE_2D, hiz_resolve_depth_tex, 0, fmt,
			scene_w, scene_h,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT, NULL);
	R_TexSetFilter(GL_TEXTURE_2D, hiz_resolve_depth_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, hiz_resolve_depth_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	R_GenFramebuffers(1, &hiz_resolve_fbo);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	R_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attach,
			       GL_TEXTURE_2D, hiz_resolve_depth_tex, 0);
	/* Depth-only FBO needs DRAW_BUFFER = NONE for completeness on strict
	 * drivers.  READ_BUFFER state is irrelevant since we only ever bind
	 * this fbo as the DRAW target of glBlitFramebuffer. */
	{
		GLenum none = GL_NONE;
		R_DrawBuffers(1, &none);
	}

	status = R_CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf("Hi-Z standalone resolve FBO incomplete (0x%x)\n", status);
		R_DeleteFramebuffers(1, &hiz_resolve_fbo);
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
	R_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	/* Always made on GL.  A copy the backend declines leaves an older
	 * frame's depth in the texture, and a pyramid built from it culls
	 * what this frame's matrix sees (#340 P5d). */
	ok = R_BlitFramebuffer(0, 0, scene_w, scene_h,
			       0, 0, scene_w, scene_h,
			       GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	R_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	return ok ? hiz_resolve_depth_tex : 0;
}

void R_BuildHiZForNextFrame (void)
{
	GLuint	scene_depth_tex;
	int	scene_w, scene_h;
	int	mip;
#ifdef USE_SDL_GPU
	unsigned long	dropped = 0, drawsdropped = 0;
#endif

	hiz_pyramid_valid = false;
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

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		dropped = RG_ComputeDropped ();
		drawsdropped = RG_DrawsDropped ();
		WC_HiZCopyDraw (scene_depth_tex, scene_w, scene_h);
	}
	else
#endif
	{
		/* Copy scene depth -> mip 0 */
		R_UseProgram (hiz_copy_prog);
		R_BindTextureSlot (0, scene_depth_tex);
		R_BindImageTexture (0, hiz_pyramid_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
		/* Pushed unconditionally every dispatch.  u_size used to be a
		 * loose uniform, which is per-PROGRAM state and so survived
		 * between frames on its own; a block member is global and any
		 * future writer of this binding point would silently win. */
		hiz_params.size[0] = scene_w;
		hiz_params.size[1] = scene_h;
		hiz_params.size[2] = 0;
		hiz_params.size[3] = 0;
		{
			GLuint ubo;
			GLintptr ofs;

			GL_Upload(GL_UNIFORM_BUFFER, &hiz_params, sizeof(hiz_params), &ubo, &ofs);
			GL_BindBufferRange(GL_UNIFORM_BUFFER, HIZ_PARAMS_UBO_BINDING,
					   ubo, ofs, sizeof(hiz_params));
		}
		R_DispatchCompute((scene_w + 7) / 8, (scene_h + 7) / 8, 1);
		R_MemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}

	/* Reduce mip N -> mip N+1 */
	R_UseProgram (hiz_reduce_prog);
	R_BindTextureSlot (0, hiz_pyramid_tex);
	{
		/* Per-mip CullParams upload (the HizParams pattern above):
		 * zero-filled so the members this program never reads are
		 * zero, never stale ring data.  The cull's own mirror: each
		 * of its dispatches refills it whole before it flushes. */
		int src_w = scene_w, src_h = scene_h;
		for (mip = 1; mip < hiz_mip_count; mip++)
		{
			int dst_w = src_w > 1 ? src_w >> 1 : 1;
			int dst_h = src_h > 1 ? src_h >> 1 : 1;

			R_BindImageTexture (0, hiz_pyramid_tex, mip, GL_FALSE, 0,
					    GL_WRITE_ONLY, GL_R32F);
			memset(&cull_params, 0, sizeof(cull_params));
			cull_params.reduce[0] = src_w;
			cull_params.reduce[1] = src_h;
			cull_params.reduce[2] = dst_w;
			cull_params.reduce[3] = dst_h;
			cull_params.reduce2[0] = mip - 1;
			cull_params.reduce2[1] = gl_clipcontrol_able ? 1 : 0;
#ifdef USE_SDL_GPU
			if (R_GPU ())
				RG_UniformBlockFlushed ("CullParams");
			else
#endif
			{
				GLuint ubo;
				GLintptr ofs;

				GL_Upload(GL_UNIFORM_BUFFER, &cull_params, sizeof(cull_params), &ubo, &ofs);
				GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
						   ubo, ofs, sizeof(cull_params));
			}
			R_DispatchCompute((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
			R_MemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
			src_w = dst_w;
			src_h = dst_h;
		}
	}

	R_UseProgram (0);
	R_BindTextureSlot (0, 0);
#ifdef USE_SDL_GPU
	/* A level the backend dropped holds an older frame's depth, or none:
	 * culling against it hides what is in view.  Next frame runs without
	 * Hi-Z, and renderer_status counts the drop (#340 P5d). */
	if (R_GPU () && (RG_ComputeDropped () != dropped || RG_DrawsDropped () != drawsdropped))
		return;
#endif
	hiz_pyramid_valid = true;
}

qboolean R_WorldCullAvailable (void)
{
	return cull_initialized;
}

/* The gl_hiz_stats line from the eight counters cull_mark_comp keeps. */
static void WC_PrintStats (const unsigned int counts[8])
{
	unsigned int	post = counts[4] + counts[5] + counts[6];

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

#ifdef USE_SDL_GPU
/* gl_hiz_stats under -gpu (#340 P5c, open question 5): this frame's counters
 * are downloaded in the frame's command buffer, after the dispatch that
 * counts them, and printed once its fence has signalled -- a frame or two
 * late, where GL's glGetBufferSubData stalls the frame for them. */
static void WC_StatsRequest (void)
{
	unsigned int	t;

	if (wc_stats_npending == WC_STATS_PENDING)
		return;		/* the reads in flight are what it would print */
	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
	t = RG_ReadBufferAsync(GL_SHADER_STORAGE_BUFFER, 0, 8 * sizeof(unsigned int));
	if (t)
		wc_stats_ticket[wc_stats_npending++] = t;
}

/* Every read that has landed, in the order they were asked for. */
static void WC_StatsCollect (void)
{
	while (wc_stats_npending > 0)
	{
		unsigned int	counts[8] = {0};
		int		r = RG_ReadbackCollect(wc_stats_ticket[0], counts, sizeof(counts));

		if (r == 0)
			break;
		if (r > 0)
			WC_PrintStats(counts);
		wc_stats_npending--;
		memmove(wc_stats_ticket, wc_stats_ticket + 1, wc_stats_npending * sizeof(wc_stats_ticket[0]));
	}
}
#endif

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
#ifdef USE_SDL_GPU
	unsigned long	dropped = 0;
#endif

	if (!cull_initialized)
		return;
#ifdef USE_SDL_GPU
	if (R_GPU ())
		dropped = RG_ComputeDropped ();
#endif

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
	/* CullParams, zero-filled (the HizParams pattern): only u_ccounts0.z
	 * is read by this program. */
	memset(&cull_params, 0, sizeof(cull_params));
	cull_params.counts0[2] = cull_num_buckets;
#ifdef USE_SDL_GPU
	if (R_GPU ())
		RG_UniformBlockFlushed ("CullParams");
	else
#endif
	{
		GLuint ubo;
		GLintptr ofs;

		GL_Upload(GL_UNIFORM_BUFFER, &cull_params, sizeof(cull_params), &ubo, &ofs);
		GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
				   ubo, ofs, sizeof(cull_params));
	}
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	R_DispatchCompute((cull_num_buckets + 63) / 64, 1, 1);
	R_MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

	/* Pass 2: cull + mark surfaces */
	R_UseProgram (cull_mark_prog);

	/* Hi-Z occlusion (uhexen2-xd87).  Sampler binding 1 = pyramid; the
	 * enable flag is also gated by gl_hiz_cull/prev_mvp_valid CPU-side
	 * so shaders early-out cheaply when not in use. */
	hiz_on = gl_hiz_cull.integer && hiz_pyramid_tex && hiz_pyramid_valid && hiz_prev_mvp_valid;
	if (hiz_on)
		R_BindTextureSlot (1, hiz_pyramid_tex);
#ifdef USE_SDL_GPU
	/* With Hi-Z off nothing on unit 1 is read, but the pass declares the
	 * sampler and is bound whatever the unit holds: empty, that is a
	 * placeholder, and a dispatch sampling only placeholders pays no
	 * submit-and-wait first (rg_compute.c).  GL leaves the unit alone. */
	else if (R_GPU ())
		R_BindTextureSlot (1, 0);
#endif

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

	wc_last_leaf = r_viewleaf;
	memcpy(wc_last_frust, frust, sizeof(wc_last_frust));
	wc_last_hiz = hiz_on;

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
		R_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
	}
	GL_BindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, cull_stats_ssbo);

	/* CullParams: one per-dispatch upload, zero-filled so the members
	 * this program never reads are zero, never stale ring data (the
	 * HizParams pattern). */
	memset(&cull_params, 0, sizeof(cull_params));
	cull_params.counts0[0] = framecount;
	cull_params.counts0[1] = cull_num_marksurfs;
	cull_params.counts0[2] = cull_num_buckets;
	cull_params.counts0[3] = cull_total_indices;
	cull_params.counts1[0] = hiz_mip_count;
	cull_params.counts1[1] = hiz_on ? 1 : 0;
	cull_params.counts1[2] = gl_clipcontrol_able ? 1 : 0;
	cull_params.counts1[3] = stats_on ? 1 : 0;
	cull_params.vieworg[0] = r_origin[0];
	cull_params.vieworg[1] = r_origin[1];
	cull_params.vieworg[2] = r_origin[2];
	memcpy(cull_params.frustum, frust, sizeof(cull_params.frustum));
	memcpy(cull_params.prevmvp, hiz_prev_mvp, sizeof(cull_params.prevmvp));
	cull_params.hizsize[0] = (float)hiz_width;
	cull_params.hizsize[1] = (float)hiz_height;
	cull_params.depthrange[0] = hiz_prev_depthrange[0];
	cull_params.depthrange[1] = hiz_prev_depthrange[1];
	memcpy(wc_last_prevmvp, hiz_prev_mvp, sizeof(wc_last_prevmvp));
	wc_last_depthrange[0] = hiz_prev_depthrange[0];
	wc_last_depthrange[1] = hiz_prev_depthrange[1];
	wc_last_hizw = hiz_width;
	wc_last_hizh = hiz_height;
	wc_last_mips = hiz_mip_count;
	wc_last_rz = gl_clipcontrol_able;
#ifdef USE_SDL_GPU
	if (R_GPU ())
		RG_UniformBlockFlushed ("CullParams");
	else
#endif
	{
		GLuint ubo;
		GLintptr ofs;

		GL_Upload(GL_UNIFORM_BUFFER, &cull_params, sizeof(cull_params), &ubo, &ofs);
		GL_BindBufferRange(GL_UNIFORM_BUFFER, CULL_PARAMS_UBO_BINDING,
				   ubo, ofs, sizeof(cull_params));
	}

	R_DispatchCompute((cull_num_marksurfs + 63) / 64, 1, 1);
	R_MemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT |
			GL_SHADER_STORAGE_BARRIER_BIT);
#ifdef USE_SDL_GPU
	if (R_GPU ())
		wc_cull_ran = (RG_ComputeDropped () == dropped);
#endif

	/* Gated stats readback: only every gl_hiz_stats frames, so the
	 * implied glGetBufferSubData stall amortizes.  Print to console;
	 * caller decides whether to leave running or set 0 after reading. */
	if (gl_hiz_stats.integer > 0 && cull_stats_ssbo)
	{
		hiz_stats_frame_counter++;
		if (hiz_stats_frame_counter >= gl_hiz_stats.integer)
		{
			hiz_stats_frame_counter = 0;
#ifdef USE_SDL_GPU
			if (R_GPU ())
				WC_StatsRequest ();
			else
#endif
			{
				unsigned int counts[8] = {0};
				GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
				if (R_ReadBuffer(GL_SHADER_STORAGE_BUFFER, 0, sizeof(counts), counts))
					WC_PrintStats(counts);
			}
		}
	}
#ifdef USE_SDL_GPU
	/* collected whatever gl_hiz_stats is now: a read in flight when it was
	 * set to 0 still prints, as GL's had already */
	if (R_GPU ())
		WC_StatsCollect ();
#endif
	if (wc_verify_pending)
	{
		wc_verify_pending = false;
		WC_Verify ();
	}

	R_UseProgram (0);
}

/* ------------------------------------------------------------------ */
/* Draw with indirect commands after compute cull                      */
/* ------------------------------------------------------------------ */

/* Whether this frame's cull dispatches left something to draw from: under
 * -gpu not when the backend dropped one (#340 P5c), which renderer_status
 * also counts as a dropped dispatch. */
static qboolean WC_CullRan (void)
{
#ifdef USE_SDL_GPU
	if (R_GPU () && !wc_cull_ran)
	{
		if (!wc_cull_skipped++)
			Con_Printf("GPU world cull: a cull dispatch was dropped; the indirect draws are "
				   "skipped and the chain pass draws the world alone\n");
		return false;
	}
#endif
	return true;
}

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
	if (!WC_CullRan ())
	{
		/* No MVP kept for this frame: the one kept is an older frame's,
		 * and paired with the pyramid this frame's depth builds it would
		 * cull against the wrong view (#340 P5d). */
		R_HiZ_ForgetView ();
		return;
	}

	/* Bind world VAO but replace its IBO with the compute-written dest IBO */
	R_BindVertexArray(world_vao);
	GL_BindBuffer(GL_ELEMENT_ARRAY_BUFFER, cull_dst_ibo);
	R_VertexAttrib4f(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

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
	GL_BindBuffer(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);

	for (i = 0; i < cull_num_buckets; i++)
	{
		texture_t *t = (cl.worldmodel->textures) ? cl.worldmodel->textures[i] : NULL;
		if (!t)
			continue;

		/* Bind diffuse texture + matching fullbright mask (uhexen2-sjvf) */
		R_BindTextureSlot (0, t->gl_texturenum);
		R_BindTextureSlot (2, t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture);

		/* Issue indirect draw for this bucket */
		R_DrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
				       (void *)((size_t)i * sizeof(gpu_indirect_cmd_t)));
	}

	GL_BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

	/* Restore original world IBO in the VAO so later code that
	 * binds world_vao gets the correct element buffer */
	{
		extern GLuint world_ibo;
		GL_BindBuffer(GL_ELEMENT_ARRAY_BUFFER, world_ibo);
	}

	R_BindVertexArray(0);
	R_UseProgram (0);

	/* Capture this frame's world MVP for next-frame Hi-Z reprojection.
	 * The pyramid will be built at end-of-3D (after every depth write
	 * has landed), and the cull dispatch one frame from now consults
	 * this matrix when reprojecting AABBs onto the pyramid. */
	memcpy(hiz_prev_mvp, mvp, sizeof(hiz_prev_mvp));
	{
		double	znear, zfar;
		R_GetDepthRange (&znear, &zfar);
		hiz_prev_depthrange[0] = (float)znear;
		hiz_prev_depthrange[1] = (float)zfar;
	}
	hiz_prev_mvp_valid = true;
}


/* ------------------------------------------------------------------ */
/* worldcull_verify (#340 P5c, Hi-Z P5d)                               */
/* ------------------------------------------------------------------ */

static int WC_CompareUint (const void *a, const void *b)
{
	unsigned int	x = *(const unsigned int *) a, y = *(const unsigned int *) b;

	return (x > y) - (x < y);
}

/* The Hi-Z pyramid read back, a level per mip, rows as GL numbers them. */
#define WC_MAX_MIPS	16
typedef struct {
	float	*lv[WC_MAX_MIPS];
	int	w[WC_MAX_MIPS], h[WC_MAX_MIPS];
	int	mips;
} wc_pyramid_t;

/* The texels a nearest fetch at u (in [0, 1] of n texels) can land on: one,
 * or the two either side of a boundary it sits within rounding of.  Clamped
 * to the edge, as the pyramid's wrap is. */
static void WC_TexelRange (float u, int n, int *a, int *b)
{
	double	x = (double) u * n;
	double	k = floor (x + 0.5);

	*a = *b = (int) floor (x);
	/* 1/64 of a texel: Vulkan promises 4 bits of subtexel precision, and
	 * the devices here have 8 */
	if (fabs (x - k) < 1.0 / 64.0 && k > 0 && k < n)
	{
		*a = (int) k - 1;
		*b = (int) k;
	}
	*a = q_max (0, q_min (n - 1, *a));
	*b = q_max (0, q_min (n - 1, *b));
}

/* The lowest and highest value a nearest fetch at (u, v) of level `mip` can
 * return, over the texels rounding leaves it between. */
static void WC_Fetch (const wc_pyramid_t *p, int mip, float u, float v, float *lo, float *hi)
{
	int	x0, x1, y0, y1, x, y;

	WC_TexelRange (u, p->w[mip], &x0, &x1);
	WC_TexelRange (v, p->h[mip], &y0, &y1);
	*lo = *hi = p->lv[mip][y0 * p->w[mip] + x0];
	for (y = y0; y <= y1; y++)
	{
		for (x = x0; x <= x1; x++)
		{
			float	t = p->lv[mip][y * p->w[mip] + x];

			*lo = q_min (*lo, t);
			*hi = q_max (*hi, t);
		}
	}
}

/* cull_mark_comp's Hi-Z test on the CPU, in its order and in float, over the
 * pyramid read back and what the last dispatch was given: 1 the surface is
 * culled, 0 kept, -1 too close to call -- where the GPU's rounding of the
 * same arithmetic could land either side (a corner on the w = 0 plane, a
 * rectangle whose size is a power of two, a sample on a texel boundary
 * between unequal texels, a depth within 1e-5 of the occluder). */
static int WC_CpuHiZ (const gpu_surface_t *s, const wc_pyramid_t *p)
{
	const float	*m = wc_last_prevmvp;
	const float	W = (float) wc_last_hizw, H = (float) wc_last_hizh;
	float		rx0 = 1.0f, ry0 = 1.0f, rx1 = -1.0f, ry1 = -1.0f;
	float		dnear = wc_last_rz ? 0.0f : 1.0f;
	float		r0x, r0y, r1x, r1y, dim, cu, cv, texel, lo, hi, occ_lo = 0.0f, occ_hi = 0.0f;
	static const float	su[4] = { -0.5f, 0.5f, -0.5f, 0.5f }, sv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
	double		l;
	int		ci, mip, k, amb = 0;

	for (ci = 0; ci < 8; ci++)
	{
		float	x = (ci & 1) ? s->maxs[0] : s->mins[0];
		float	y = (ci & 2) ? s->maxs[1] : s->mins[1];
		float	z = (ci & 4) ? s->maxs[2] : s->mins[2];
		/* u_cprevmvp * vec4(c, 1): the block holds the matrix column by column */
		float	cx = m[0] * x + m[4] * y + m[8] * z + m[12];
		float	cy = m[1] * x + m[5] * y + m[9] * z + m[13];
		float	cz = m[2] * x + m[6] * y + m[10] * z + m[14];
		float	cw = m[3] * x + m[7] * y + m[11] * z + m[15];
		float	nx, ny, nz, dn;

		if (fabsf (cw) < 1e-3f)
			amb = 1;
		if (cw <= 0.0f)
			return amb ? -1 : 0;
		nx = cx / cw;
		ny = cy / cw;
		nz = cz / cw;
		dn = wc_last_rz ? nz : (nz * 0.5f + 0.5f);
		dnear = wc_last_rz ? q_max (dnear, dn) : q_min (dnear, dn);
		rx0 = q_min (rx0, nx); ry0 = q_min (ry0, ny);
		rx1 = q_max (rx1, nx); ry1 = q_max (ry1, ny);
	}
	dnear = wc_last_depthrange[0] + (wc_last_depthrange[1] - wc_last_depthrange[0]) * dnear;
	rx0 = q_max (-1.0f, q_min (1.0f, rx0)); ry0 = q_max (-1.0f, q_min (1.0f, ry0));
	rx1 = q_max (-1.0f, q_min (1.0f, rx1)); ry1 = q_max (-1.0f, q_min (1.0f, ry1));
	r0x = (rx0 * 0.5f + 0.5f) * W; r0y = (ry0 * 0.5f + 0.5f) * H;
	r1x = (rx1 * 0.5f + 0.5f) * W; r1y = (ry1 * 0.5f + 0.5f) * H;
	dim = q_max (r1x - r0x, r1y - r0y);
	if (fabsf (dim - 0.5f) < 1e-3f)
		amb = 1;
	if (dim <= 0.5f)
		return amb ? -1 : 0;
	l = log2 ((double) dim);
	/* in log2, so relative: dim within 0.07% of a power of two */
	if (fabs (l - floor (l + 0.5)) < 1e-3)
		amb = 1;
	mip = (int) ceil (l);
	mip = q_max (0, q_min (wc_last_mips - 1, mip));
	cu = (r0x + r1x) * 0.5f / W;
	cv = (r0y + r1y) * 0.5f / H;
	texel = (float) (1 << mip) / q_max (W, H);
	for (k = 0; k < 4; k++)
	{
		WC_Fetch (p, mip, cu + su[k] * texel, cv + sv[k] * texel, &lo, &hi);
		if (k == 0)
		{
			occ_lo = lo;
			occ_hi = hi;
		}
		else if (wc_last_rz)	/* the occluder is the least of the four */
		{
			occ_lo = q_min (occ_lo, lo);
			occ_hi = q_min (occ_hi, hi);
		}
		else			/* the greatest */
		{
			occ_lo = q_max (occ_lo, lo);
			occ_hi = q_max (occ_hi, hi);
		}
	}
	/* culled where the surface's nearest depth is behind the occluder:
	 * below it under reversed-Z, above it forward */
	if (wc_last_rz ? (dnear < occ_lo - 1e-5f) : (dnear > occ_hi + 1e-5f))
		return amb ? -1 : 1;
	if (wc_last_rz ? (dnear >= occ_hi + 1e-5f) : (dnear <= occ_lo - 1e-5f))
		return amb ? -1 : 0;
	return -1;
}

/* hiz_reduce_comp on the CPU: the texels of level `mip` that are not the
 * reduce of the level below.  min and max are exact, so this is too. */
static int WC_PyramidOff (const wc_pyramid_t *p, int mip)
{
	const float	*src = p->lv[mip - 1], *dst = p->lv[mip];
	int		sw = p->w[mip - 1], sh = p->h[mip - 1], x, y, off = 0;

#define WC_RED(a, b)	(wc_last_rz ? q_min ((a), (b)) : q_max ((a), (b)))
#define WC_SRC(px, py)	src[q_min ((py), sh - 1) * sw + q_min ((px), sw - 1)]
	for (y = 0; y < p->h[mip]; y++)
	{
		for (x = 0; x < p->w[mip]; x++)
		{
			int	sx = x * 2, sy = y * 2;
			float	r = WC_RED (WC_RED (WC_SRC (sx, sy), WC_SRC (sx + 1, sy)),
					    WC_RED (WC_SRC (sx, sy + 1), WC_SRC (sx + 1, sy + 1)));

			if ((sw & 1) && sx + 2 < sw)
				r = WC_RED (r, WC_RED (WC_SRC (sx + 2, sy), WC_SRC (sx + 2, sy + 1)));
			if ((sh & 1) && sy + 2 < sh)
				r = WC_RED (r, WC_RED (WC_SRC (sx, sy + 2), WC_SRC (sx + 1, sy + 2)));
			off += (r != dst[y * p->w[mip] + x]);
		}
	}
#undef WC_SRC
#undef WC_RED
	return off;
}

/* cull_mark_comp's tests on the CPU: the surface marksurface `ms` draws, or
 * NULL.  `pvs` is the view leaf's, decompressed; `seen` the dedup across
 * leaves.  With `p`, the Hi-Z test too, between the frustum and the dedup as
 * the shader has it: a surface too close to call is drawn when `keep_amb`
 * says so and counted in `amb`, a culled one in `hiz`. */
static const gpu_surface_t *WC_CpuAccepts (const gpu_marksurf_t *ms, const byte *pvs, byte *seen,
					   const wc_pyramid_t *p, qboolean keep_amb, int *hiz, int *amb)
{
	extern int		world_num_indices;
	const gpu_surface_t	*s;
	int			leaf = ms->leaf_idx, k;

	if (leaf < 1 || !(pvs[(leaf - 1) >> 3] & (1 << ((leaf - 1) & 7))))
		return NULL;
	if (ms->surf_idx < 0 || ms->surf_idx >= cull_num_surfs)
		return NULL;
	s = &cull_cpu_surfs[ms->surf_idx];
	if ((s->flags & 0xB34) || s->numindices <= 0)
		return NULL;
	for (k = 0; k < 4; k++)
	{
		const float	*fn = &wc_last_frust[k * 4];
		float		pv[3];

		pv[0] = fn[0] >= 0.0f ? s->maxs[0] : s->mins[0];
		pv[1] = fn[1] >= 0.0f ? s->maxs[1] : s->mins[1];
		pv[2] = fn[2] >= 0.0f ? s->maxs[2] : s->mins[2];
		if (fn[0] * pv[0] + fn[1] * pv[1] + fn[2] * pv[2] + fn[3] < -512.0f)
			return NULL;
	}
	if (p)
	{
		int	h = WC_CpuHiZ (s, p);

		*amb += (h < 0);
		*hiz += (h > 0);
		if (h > 0 || (h < 0 && !keep_amb))
			return NULL;
	}
	if (seen[ms->surf_idx])
		return NULL;
	seen[ms->surf_idx] = 1;
	if (s->tex_bucket < 0 || s->tex_bucket >= cull_num_buckets ||
	    s->firstindex < 0 || s->firstindex + s->numindices > world_num_indices)
		return NULL;
	return s;
}

/* What cull_mark_comp would have written for the last dispatch, on the CPU,
 * in its order: per bucket the count (`nwant`) and the indices (`want`, from
 * `first`).  Returns the surfaces accepted; `hiz` and `amb` count the
 * marksurfaces the Hi-Z test culled and could not call. */
static int WC_CpuCull (const byte *pvs, byte *seen, const wc_pyramid_t *p, qboolean keep_amb,
		       unsigned int *want, int *nwant, int *first, int *total, int *hiz, int *amb)
{
	extern unsigned int	*world_index_data;
	int			i, b, pass, accepted = 0;

	*total = 0;
	for (pass = 0; pass < 2; pass++)
	{
		memset (nwant, 0, cull_num_buckets * sizeof(int));
		memset (seen, 0, cull_num_surfs);
		*hiz = *amb = 0;
		for (i = 0; i < cull_num_marksurfs; i++)
		{
			const gpu_surface_t	*s = WC_CpuAccepts (&cull_cpu_marks[i], pvs, seen, p, keep_amb,
								    hiz, amb);

			if (!s)
				continue;
			if (pass)
				memcpy (want + first[s->tex_bucket] + nwant[s->tex_bucket],
					world_index_data + s->firstindex, s->numindices * sizeof(unsigned int));
			else
				accepted++;
			nwant[s->tex_bucket] += s->numindices;
		}
		if (pass)
			break;
		for (b = 0; b < cull_num_buckets; b++)
		{
			first[b] = *total;
			*total += nwant[b];
		}
	}
	return accepted;
}

/* Whether sorted `a` (na) is a sub-multiset of sorted `b` (nb). */
static qboolean WC_SubMultiset (const unsigned int *a, int na, const unsigned int *b, int nb)
{
	int	i = 0, j = 0;

	while (i < na && j < nb)
	{
		if (a[i] == b[j])
		{
			i++;
			j++;
		}
		else if (a[i] > b[j])
			j++;
		else
			return false;
	}
	return i == na;
}

/* The pyramid the last dispatch culled against, read back, and its levels
 * held to hiz_reduce_comp run on the CPU.  Called right after that dispatch,
 * before the frame's end builds the next one.  Mip 0 is the copy of the
 * scene depth, which this cannot read back under -gpu (D24S8): the
 * gl_hiz_stats counts against GL's are what hold it (tools/hiz-diff.sh). */
static qboolean WC_ReadPyramid (wc_pyramid_t *p, int *off)
{
	int	mip;

	*off = 0;
	if (!hiz_pyramid_tex || !hiz_pyramid_valid ||
	    hiz_width != wc_last_hizw || hiz_height != wc_last_hizh || hiz_mip_count != wc_last_mips ||
	    wc_last_mips < 1 || wc_last_mips > WC_MAX_MIPS)
	{
		CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: the Hi-Z pyramid the last dispatch read is gone "
			    "(rebuilt at another size, or not valid now)\n");
		return false;
	}
	p->mips = wc_last_mips;
	for (mip = 0; mip < p->mips; mip++)
	{
		p->w[mip] = q_max (1, wc_last_hizw >> mip);
		p->h[mip] = q_max (1, wc_last_hizh >> mip);
		p->lv[mip] = (float *) malloc ((size_t) p->w[mip] * p->h[mip] * sizeof(float));
		if (!p->lv[mip] ||
		    !R_ReadTexImage (GL_TEXTURE_2D, hiz_pyramid_tex, mip, GL_RED, GL_FLOAT, p->lv[mip]))
		{
			CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: the Hi-Z pyramid cannot be read back\n");
			return false;
		}
		if (mip)
			*off += WC_PyramidOff (p, mip);
	}
	return true;
}

/* Reads the last frame's indirect commands and compacted indices back and
 * checks them against cull_mark_comp's tests run on the CPU over the same
 * inputs: per bucket the count, the first index (fixed at the build) and the
 * indices as a multiset, since the atomicAdd compaction orders them however
 * the invocations land.  The chain pass draws every surface the cull draws,
 * after it, so a cull that draws the wrong thing or nothing hardly shows in
 * a frame; this is what does.  Both backends, and the same line on each.
 *
 * With Hi-Z on (#340 P5d) the CPU runs that test too, against the pyramid
 * read back -- each of whose levels is first held to the reduce of the one
 * below -- with the MVP and depth range the dispatch was given.  A surface
 * too close to call may go either way, so a bucket agrees when the GPU drew
 * at least what the CPU draws with every such surface culled, and at most
 * what it draws with every one kept.  Asked with Hi-Z on, it runs right
 * after the next mark dispatch (wc_verify_pending), against the pyramid that
 * dispatch read, whatever moves in between. */
static void WC_Verify (void)
{
	qmodel_t		*m = cl.worldmodel;
	gpu_indirect_cmd_t	*cmds = NULL;
	unsigned int		*dst = NULL, *want = NULL, *want_lo = NULL;
	int			*nwant = NULL, *first = NULL, *nwant_lo = NULL, *first_lo = NULL;
	byte			*seen = NULL;
	int			i, b, total = 0, total_lo = 0, accepted, hiz = 0, amb = 0, hiz_lo = 0, amb_lo = 0;
	int			bad_count = 0, bad_first = 0, bad_cmd = 0, bad_idx = 0, off = 0;
	unsigned int		gpu_total = 0;
	extern unsigned int	*world_index_data;
	extern cvar_t		r_gpucull;
	const byte		*pvs;
	wc_pyramid_t		pyr, *p = NULL;

	memset (&pyr, 0, sizeof(pyr));
	if (!cull_initialized || !m || !cull_cpu_surfs || !cull_cpu_marks || !cull_bucket_first ||
	    !cull_dst_ibo || !wc_last_leaf || !world_index_data)
	{
		CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: no cull this frame (r_gpucull %d, built %d)\n",
			    r_gpucull.integer, cull_initialized);
		return;
	}
	cmds = (gpu_indirect_cmd_t *) malloc (cull_num_buckets * sizeof(gpu_indirect_cmd_t));
	dst = (unsigned int *) malloc (cull_total_indices * sizeof(unsigned int));
	want = (unsigned int *) malloc (cull_total_indices * sizeof(unsigned int));
	want_lo = (unsigned int *) malloc (cull_total_indices * sizeof(unsigned int));
	nwant = (int *) calloc (cull_num_buckets, sizeof(int));
	first = (int *) calloc (cull_num_buckets, sizeof(int));
	nwant_lo = (int *) calloc (cull_num_buckets, sizeof(int));
	first_lo = (int *) calloc (cull_num_buckets, sizeof(int));
	seen = (byte *) calloc (cull_num_surfs, 1);
	if (!cmds || !dst || !want || !want_lo || !nwant || !first || !nwant_lo || !first_lo || !seen)
	{
		CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: out of memory\n");
		goto done;
	}
	if (wc_last_hiz)
	{
		if (!WC_ReadPyramid (&pyr, &off))
			goto done;
		p = &pyr;
	}
	GL_BindBuffer(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);
	i = R_ReadBuffer(GL_DRAW_INDIRECT_BUFFER, 0, cull_num_buckets * sizeof(gpu_indirect_cmd_t), cmds);
	GL_BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_dst_ibo);
	i = i && R_ReadBuffer(GL_SHADER_STORAGE_BUFFER, 0, cull_total_indices * sizeof(unsigned int), dst);
	GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	if (!i)
	{
		CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: the cull's buffers cannot be read back\n");
		goto done;
	}

	/* cull_mark_comp's tests over the same marksurfaces, in its order: once
	 * keeping the surfaces too close to call, once culling them */
	pvs = Mod_LeafPVS (wc_last_leaf, m);
	accepted = WC_CpuCull (pvs, seen, p, true, want, nwant, first, &total, &hiz, &amb);
	WC_CpuCull (pvs, seen, p, false, want_lo, nwant_lo, first_lo, &total_lo, &hiz_lo, &amb_lo);

	for (b = 0; b < cull_num_buckets; b++)
	{
		unsigned int	n = cmds[b].count, f = cmds[b].firstIndex;

		gpu_total += n;
		if (f != cull_bucket_first[b])
			bad_first++;
		if (cmds[b].instanceCount != 1 || cmds[b].baseVertex != 0 || cmds[b].baseInstance != 0)
			bad_cmd++;
		if (n < (unsigned int) nwant_lo[b] || n > (unsigned int) nwant[b])
		{
			bad_count++;
			continue;
		}
		if (!n || f != cull_bucket_first[b] || (size_t) f + n > (size_t) cull_total_indices)
			continue;
		qsort (dst + f, n, sizeof(unsigned int), WC_CompareUint);
		qsort (want + first[b], nwant[b], sizeof(unsigned int), WC_CompareUint);
		qsort (want_lo + first_lo[b], nwant_lo[b], sizeof(unsigned int), WC_CompareUint);
		if (!WC_SubMultiset (want_lo + first_lo[b], nwant_lo[b], dst + f, n) ||
		    !WC_SubMultiset (dst + f, n, want + first[b], nwant[b]))
			bad_idx++;
	}
	CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: %d surfaces accepted, %d indices on the CPU, %u on the "
		    "GPU; of %d buckets %d disagree on the count, %d on the first index, %d on the rest of "
		    "the command, %d on the indices\n", accepted, total, gpu_total, cull_num_buckets,
		    bad_count, bad_first, bad_cmd, bad_idx);
	if (p)
		CON_Printf (_PRINT_NONOTIFY, "worldcull_verify: Hi-Z pyramid %dx%d in %d levels, %d texels off "
			    "the reduce of the level below; %d marksurfaces culled by it on the CPU, %d too close "
			    "to call\n", wc_last_hizw, wc_last_hizh, wc_last_mips, off, hiz, amb);
done:
	for (i = 0; i < WC_MAX_MIPS; i++)
		free (pyr.lv[i]);
	free (cmds);
	free (dst);
	free (want);
	free (want_lo);
	free (nwant);
	free (first);
	free (nwant_lo);
	free (first_lo);
	free (seen);
}

static void R_WorldCull_Verify_f (void)
{
	if (cull_initialized && gl_hiz_cull.integer && wc_last_hiz)
		wc_verify_pending = true;
	else
		WC_Verify ();
}

void R_WorldCull_Init (void)
{
	Cmd_AddCommand ("worldcull_verify", R_WorldCull_Verify_f);
}


#else	/* USE_GLES — GPU culling needs compute shaders and SSBOs, which
	 * WebGL2 lacks, so the whole module is compiled out.  Only the
	 * shutdown hook is reachable from live code (gl_vidsdl.c); stub it so
	 * VID_Shutdown links without an ifdef at the call site, matching the
	 * gl_buffer.c treatment. */

void R_FreeWorldCull (void) {}
void R_HiZ_Shutdown (void) {}
void R_WorldCull_Init (void) {}

#endif /* !USE_GLES */
#endif /* GLQUAKE */
