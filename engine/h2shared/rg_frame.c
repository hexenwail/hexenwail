/* rg_frame.c -- SDL_GPU backend: the frame recorder (#340 P1c)
 *
 * GL takes a draw the moment it is issued; SDL_GPU wants copies (stream data,
 * texture uploads) in a copy pass and draws in a render pass, and no copy
 * inside a render pass.  So draws are recorded, not issued: each carries its
 * pipeline, its vertex/index bytes (copied into a stream arena), its
 * texture+sampler bindings, its uniform snapshot, viewport and scissor.  A
 * SEGMENT is a run of them into one render target; ending it does
 *
 *   1. one copy pass: the segment's stream arena into the stream buffer,
 *      every queued texture upload (rg_resource.c), the quad index buffer
 *      the first time;
 *   2. one render pass on the segment's render target, replaying the draws
 *      in order.
 *
 * A segment ends at the present, and early whenever something recorded next
 * cannot share its passes: an upload into a texture one of its draws reads
 * (RG_Frame_Flush), a different render target bound (RG_Frame_BindTarget,
 * #340 P4a-2), a blit or a copy between targets (RG_Frame_Break), a compute
 * dispatch (RG_Frame_ComputeBegin, P5a-2: with no draws in the segment, its
 * copy pass alone, the pending clear kept), a readback (RG_Frame_Readback,
 * which also submits and waits, the frame going on in a new command buffer)
 * and an asynchronous one (RG_Frame_ReadbackRequest, P5c: no wait; read
 * once the fence of the submit that carried it signals).
 *
 * The render target is GL's draw framebuffer: 0 is the SCENE target, any
 * other a framebuffer rg_target.c resolves to its attachments.  The frame
 * reaches the screen from the scene alone, not the swapchain: the Vulkan
 * stages flip y so render targets hold GL's rows in GL's order (VK_FLIP_Y,
 * layout.inc), and the present is the one place that flips back, blitting
 * the scene into the swapchain upside down (SDL_FLIP_VERTICAL).  So the
 * post-process chain's final pass draws into the scene, as it draws into
 * framebuffer 0 on GL.
 *
 * Clears (#340 P2b).  SDL_GPU has no clear command: a render pass begins by
 * clearing or loading each target, so a glClear (gl_pipeline.c
 * R_ClearBuffers -> RG_Clear) becomes the load op of the next pass, and a
 * clear after draws ends the segment so that those draws land first.  The
 * scene's first pass of a frame clears every aspect nothing asked for --
 * colour to gpu_clearcolor, depth and stencil to the last values a clear
 * asked for -- so no frame starts from the previous one's contents; a frame
 * whose scene had no pass of its own gets one at the end.  A framebuffer
 * loads what it holds, as GL's does across frames; it was cleared once, when
 * its texture was made (rg_resource.c RG_Res_ClearTargets).
 *
 * Two kinds of draw.  GL_ImmEnd's batches (the 2D, alias models, sprites,
 * everything drawn immediate): SDL_GPU has no quads and no fans, so quads
 * use a static index buffer (0,1,2, 0,2,3 per quad, as GL_VBO_Init's) and
 * polygons/fans and line loops get indices in the stream.  And since P2d,
 * indexed draws from a VAO (R_DrawElements): the world, brush models and the
 * sky pre-pass, from their static buffers (rg_buffer.c), so only the
 * constant-attribute record lands in the stream.  Their pipeline is keyed on
 * the VAO's interned vertex layout (rg_pipeline.c).  Since P5c an indexed
 * draw can take its count and first index from a GL_DRAW_INDIRECT_BUFFER
 * command a compute pass wrote (R_DrawElementsIndirect, the world cull's).
 *
 * Every slot a draw's program declares is bound (#340 P2a), because SDL_GPU
 * faults a draw that leaves one empty: each sampler its texture or a typed
 * placeholder (rg_resource.c), each read-only storage buffer the buffer
 * bound to its GL binding point (rg_buffer.c, since P2e: the alias poses,
 * shadedots, instances and skeletal bones) or, where that holds nothing
 * SDL_GPU can bind, the zeroed stand-in below.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_frame.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"
#include "gl_vbo.h"		/* GL_IMM_MAX_VERTS */
#include "gl_uniforms.h"	/* r_vertparams_t: the pose type a draw reads (#520) */
#include "gl_lightcluster.h"	/* R_LightCluster_Available: LightBuffer's excuse (#340 P5a-2) */
#include "gl_shadow.h"		/* SHADOWPASS_POSE_FORMAT_OFS: an alias caster's pose type (#340 P6b) */
#include <stddef.h>		/* offsetof */

/* ------------------------------------------------------------------ */
/* Uniform blocks                                                      */
/* ------------------------------------------------------------------ */

/* A CPU mirror gl_uniforms.c registered, and what was last flushed from it:
 * a draw sees the flushed copy, as GL's buffer would hold it. */
typedef struct {
	char		name[32];
	const void	*mirror;
	int		size;
	Uint8		*flushed;
	unsigned int	version;
	/* where this version sits in the open segment's uniform arena */
	unsigned int	arena_version, arena_segment;
	Uint32		arena_ofs;
	qboolean	arena_valid;
} rgblock_t;

#define RG_MAX_BLOCKS	24
static rgblock_t	rgb_blocks[RG_MAX_BLOCKS];
static int		rgb_count;

void RG_UniformBlockRegister (const char *name, const void *mirror, int size)
{
	rgblock_t	*b;
	int		i;

	for (i = 0; i < rgb_count; i++)
	{
		if (!strcmp (rgb_blocks[i].name, name))
			break;
	}
	if (i == rgb_count)
	{
		if (rgb_count == RG_MAX_BLOCKS)
			Sys_Error ("SDL_GPU: more than %d uniform blocks", RG_MAX_BLOCKS);
		rgb_count++;
	}
	b = &rgb_blocks[i];
	free (b->flushed);
	memset (b, 0, sizeof(*b));
	q_strlcpy (b->name, name, sizeof(b->name));
	b->mirror = mirror;
	b->size = size;
	b->flushed = (Uint8 *) calloc (1, size);
	if (!b->flushed)
		Sys_Error ("SDL_GPU: out of memory for uniform block %s", name);
}

int RG_Frame_BlockIndex (const char *name)
{
	int	i;

	for (i = 0; i < rgb_count; i++)
	{
		if (!strcmp (rgb_blocks[i].name, name))
			return i;
	}
	return -1;
}

const void *RG_Frame_BlockFlushed (int block, int *size)
{
	if (block < 0 || block >= rgb_count)
		return NULL;
	*size = rgb_blocks[block].size;
	return rgb_blocks[block].flushed;
}

void RG_UniformBlockFlushed (const char *name)
{
	int	i = RG_Frame_BlockIndex (name);

	if (i < 0)
		return;
	memcpy (rgb_blocks[i].flushed, rgb_blocks[i].mirror, rgb_blocks[i].size);
	rgb_blocks[i].version++;
}

/* ------------------------------------------------------------------ */
/* State and storage                                                   */
/* ------------------------------------------------------------------ */

#define RG_STREAM_ALIGN	16

typedef struct {
	Uint8	stage;		/* RPROG_STAGE_VERTEX / _FRAGMENT */
	Uint8	slot;
	Uint32	ofs, len;	/* in the uniform arena */
} rgpush_t;

/* Where a draw's indices come from. */
enum {
	RGD_ARRAYS,		/* none */
	RGD_STREAM,		/* 16-bit, in the stream arena at ib_ofs */
	RGD_QUADS,		/* 16-bit, the static quad buffer */
	RGD_BUFFER,		/* the VAO's element buffer, ib_size wide (#340 P2d) */
	RGD_INDIRECT		/* the VAO's element buffer, the count and first index
				 * the command at `indirect_ofs` of `indirect` holds (P5c) */
};

typedef struct {
	SDL_GPUGraphicsPipeline		*pipe;
	/* Vertex buffer slots in layout order; a NULL buffer is the stream
	 * arena, vb_ofs into it. */
	Uint8				nvb;
	SDL_GPUBuffer			*vb[RG_MAX_VBUFS];
	Uint32				vb_ofs[RG_MAX_VBUFS];
	Uint32				ib_ofs;		/* RGD_STREAM: stream arena */
	Uint8				indexed;	/* RGD_* */
	SDL_GPUBuffer			*ib;		/* RGD_BUFFER */
	SDL_GPUIndexElementSize		ib_size;
	Uint32				first_index;	/* RGD_BUFFER */
	Uint32				first_vertex;	/* RGD_ARRAYS: RG_DrawArrays's first */
	SDL_GPUBuffer			*indirect;	/* RGD_INDIRECT: the command's buffer */
	Uint32				indirect_ofs;	/* and its byte offset in it */
	Uint32				instances;
	Uint32				count;		/* vertices, or indices */
	int				nvs, nfs;
	SDL_GPUTextureSamplerBinding	vs[RG_MAX_STAGE_SAMPLERS], fs[RG_MAX_STAGE_SAMPLERS];
	Uint8				nvssbo, nfssbo;	/* storage-buffer slots */
	SDL_GPUBuffer			*vssbo[RG_MAX_STAGE_SSBOS], *fssbo[RG_MAX_STAGE_SSBOS];
	int				npush;
	rgpush_t			push[2 * RG_MAX_STAGE_UBOS];
	SDL_GPUViewport			vp;
	SDL_Rect			scissor;
	Uint8				stencil_ref;
} rgdraw_t;

/* The frame */
static SDL_GPUCommandBuffer	*rgf_cmd;
static qboolean			rgf_active;
static qboolean			rgf_cleared;	/* the scene's clear has been issued */
static unsigned int		rgf_segment = 1;
static int			rgf_w, rgf_h;

/* Scene target */
static SDL_GPUTexture		*rgf_color, *rgf_depth;
static SDL_GPUTextureFormat	rgf_color_fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
static SDL_GPUTextureFormat	rgf_depth_fmt;
static qboolean			rgf_depth_sampled;	/* rgf_depth_fmt can also be sampled */
static int			rgf_tw, rgf_th;

/* The current render target: GL's draw framebuffer and what it resolved to.
 * Dirty when what it resolves to may have changed with nothing recorded
 * against it (a new frame's scene, a texture of it deleted): resolved again
 * before the next draw or pass. */
static GLuint			rgf_fbo;
static rgtarget_t		rgf_tgt;
static qboolean			rgf_tgt_ok, rgf_tgt_dirty = true;

/* Recording */
static rgdraw_t			*rgf_draws;
static int			rgf_numdraws, rgf_maxdraws;
static Uint8			*rgf_stream;	/* vertex + index bytes */
static Uint32			rgf_streamlen, rgf_streammax;
static Uint8			*rgf_uarena;	/* uniform snapshots */
static Uint32			rgf_ulen, rgf_umax;

/* GPU-side */
static SDL_GPUBuffer		*rgf_vbuf;	/* the stream, vertex usage */
static Uint32			rgf_vbuf_size;
/* The stream again, index usage, for the draws whose indices are in it.
 * One buffer with both usages is read as indices after a copy SDL syncs
 * for vertex reads alone: SDL's Vulkan backend returns a buffer to the first
 * of its usages after each write, and syncval reports every such draw as a
 * read-after-write hazard (#340 P5a-2). */
static SDL_GPUBuffer		*rgf_ibuf;
static Uint32			rgf_ibuf_size;
static qboolean			rgf_stream_indices;	/* the open segment has RGD_STREAM draws */
static SDL_GPUBuffer		*rgf_quadbuf;	/* static quad indices */
static qboolean			rgf_quad_uploaded;
/* RG_SSBO_STANDIN_BYTES of zeros, for a storage slot with nothing bound */
static SDL_GPUBuffer		*rgf_ssbo;
static qboolean			rgf_ssbo_uploaded;
static SDL_GPUTransferBuffer	*rgf_tbuf;
static Uint32			rgf_tbuf_size;
static SDL_GPUBuffer		*rgf_retired_bufs[8];
static int			rgf_numretired_bufs;

/* Dynamic state, persistent like GL's */
static qboolean			rgf_vp_set;
static int			rgf_vp[4];
static qboolean			rgf_scissor_on;
static int			rgf_scissor[4];

/* The clear the next render pass begins with (RG_Clear): the
 * GL_*_BUFFER_BIT aspects asked for since the last pass, and the values.
 * Depth and stencil outlive the request -- they are what a frame's first
 * pass clears to when nothing asked.  Depth starts at 0, reversed-Z's far
 * plane: -gpu always runs reversed-Z (GL_Init_NullGL). */
static GLbitfield		rgf_clear_mask;
/* The colour each colour target clears to, and which do (bit i, target i):
 * glClear's colour is every target's, glClearBufferfv (GL_COLOR, i) target
 * i's alone (#340 P6c).  GL_COLOR_BUFFER_BIT is in rgf_clear_mask whenever
 * a bit is set here. */
static float			rgf_clear_rgba[RG_MAX_COLOR][4];
static Uint32			rgf_clear_cbits;
static float			rgf_clear_depth = 0.0f;
static Uint8			rgf_clear_stencil;
/* The depth the next pass clears to when its mask asks: glClear's value,
 * which rgf_clear_depth keeps too, or glClearBufferfv's, which it does not
 * -- that one is for its own pass, and GL's glClearDepth is left as it was
 * (RG_ClearBuffer, #340 P6b). */
static float			rgf_pass_clear_depth = 0.0f;

#define RG_QUAD_INDICES	((GL_IMM_MAX_VERTS / 4) * 6)

/* Last frame's counts, for renderer_status */
static int			rgf_last_draws, rgf_last_segments, rgf_frame_draws, rgf_frame_segments;
static int			rgf_last_indexed, rgf_frame_indexed;	/* RGD_BUFFER draws */
static Uint32			rgf_last_stream, rgf_frame_stream;	/* stream arena bytes */
/* Storage slots bound per frame: to a GL binding point's buffer, and to the
 * stand-in (#340 P2e) */
static int			rgf_last_ssbo, rgf_frame_ssbo, rgf_last_standin, rgf_frame_standin;
/* Compute passes and readbacks per frame, and since boot (#340 P5a-2); the
 * boot-time palette LUT runs outside any frame */
static int			rgf_last_cpasses, rgf_frame_cpasses, rgf_last_reads, rgf_frame_reads;
static unsigned long		rgf_cpasses, rgf_reads;
/* Submits mid-frame that wait on their fence (RGF_SubmitMidFrame): each
 * readback's, and each dispatch's that samples (#340 P5b: the stall P5f
 * measures, counted so a pass that starts paying it shows). */
static int			rgf_last_waits, rgf_frame_waits;
static unsigned long		rgf_waits;
/* Indirect draws per frame (#340 P5c: the world cull's, one per texture
 * bucket), the compute dispatches dropped in it (RG_ComputeDropped at the
 * frame's start), and the asynchronous readbacks asked for in it. */
static int			rgf_last_indirect, rgf_frame_indirect;
static unsigned long		rgf_cdropped_base, rgf_last_cdropped;
static int			rgf_last_areads, rgf_frame_areads;
/* Render passes per frame with no colour target, and those on a layer of a
 * layered depth texture: the shadow maps' faces, six per map drawn
 * (#340 P6b) */
static int			rgf_last_depthonly, rgf_frame_depthonly;
static int			rgf_last_layered, rgf_frame_layered;
/* Render passes per frame with more than one colour target: OIT's WBOIT
 * pass and MBOIT's moment pass (#340 P6c) */
static int			rgf_last_mrt, rgf_frame_mrt;

/* The storage buffers the draw being recorded reads, stamped once it is
 * committed (RG_Buf_StampRead), so an upload into one ends the segment. */
static void			*rgf_pending_reads[2 * RG_MAX_STAGE_SSBOS];
static int			rgf_npending_reads, rgf_pending_ssbo, rgf_pending_standin;
static unsigned long		rgf_frames;
static unsigned long		rgf_dropped;

/* Frame fences and frame times (#340 P5f, dev-only: gpu_frametimes).  Every
 * submit takes a fence, kept here until it has signalled; a frame begins by
 * waiting for all but the newest, so at most two frames are in flight, as
 * SDL's swapchain would allow (SDL_SetGPUAllowedFramesInFlight's default),
 * and with -gpuheadless, where there is no swapchain to hold the CPU back.
 * The wall time of that wait says whether the GPU (a long wait) or the CPU
 * (none) sets the pace; RGF_SubmitMidFrame's waits are timed apart. */
#define RGF_FENCES		8
static SDL_GPUFence		*rgf_fences[RGF_FENCES];
static int			rgf_numfences;
#define RGF_TIMES		128
typedef struct {
	Uint64		interval_ns, cpu_ns, fence_ns, mid_ns, acquire_ns;
	Uint64		seg_ns, comp_ns, submit_ns;	/* parts of cpu_ns */
	int		render, compute, copy, waits, draws, indirect;
	Uint32		stream;
} rgftime_t;
static rgftime_t		rgf_times[RGF_TIMES];
static unsigned long		rgf_numtimes;
static Uint64			rgf_t_begin, rgf_t_lastbegin;
static Uint64			rgf_frame_fence_ns, rgf_frame_mid_ns, rgf_frame_acquire_ns;
/* the recording's parts: ending segments (their copy pass and the replay of
 * their draws into a render pass), compute dispatches outside those, and
 * the submits */
static Uint64			rgf_frame_seg_ns, rgf_frame_comp_ns, rgf_frame_submit_ns;
static Uint64			rgf_comp_t0, rgf_comp_mid0, rgf_comp_seg0;
static int			rgf_frame_copies;
/* -gpuheadless's present: the scene is blitted here instead (rg_device.c) */
static SDL_GPUTexture		*rgf_headless_tex;
static Uint32			rgf_headless_w, rgf_headless_h;

static void RGF_ReadbacksAge (void);
static qboolean RGF_CopyOnly (void);
static void RGF_ReadbackFenceDone (SDL_GPUFence *fence, int state);
static void RGF_FencesWait (int keep);
static SDL_GPUTexture *RGF_HeadlessTarget (Uint32 *w, Uint32 *h);
static void RGF_NoteTimes (void);

/* A device that fails once tends to fail every frame: say it a few times. */
static int	rgf_warnings;
static void RGF_Warn (const char *fmt, ...) FUNC_PRINTF(1,2);
static void RGF_Warn (const char *fmt, ...)
{
	va_list	ap;
	char	msg[256];

	if (rgf_warnings >= 8)
		return;
	if (++rgf_warnings == 8)
		Con_Printf ("SDL_GPU: further frame errors are not shown\n");
	va_start (ap, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, ap);
	va_end (ap);
	Con_Printf ("%s", msg);
}

unsigned int RG_Frame_Segment (void)
{
	return rgf_segment;
}

qboolean RG_Frame_SegmentHasDraws (void)
{
	return rgf_numdraws > 0;
}

qboolean RG_Frame_SegmentWritesTarget (void)
{
	return rgf_numdraws > 0 || rgf_clear_mask != 0;
}

void RG_SetViewport (int x, int y, int width, int height)
{
	rgf_vp_set = true;
	rgf_vp[0] = x;
	rgf_vp[1] = y;
	rgf_vp[2] = width;
	rgf_vp[3] = height;
}

void RG_GetViewport (int vp[4])
{
	if (rgf_vp_set)
	{
		vp[0] = rgf_vp[0];
		vp[1] = rgf_vp[1];
		vp[2] = rgf_vp[2];
		vp[3] = rgf_vp[3];
		return;
	}
	vp[0] = 0;
	vp[1] = 0;
	vp[2] = rgf_w;
	vp[3] = rgf_h;
}

void RG_SetScissor (qboolean enable, int x, int y, int width, int height)
{
	rgf_scissor_on = enable;
	rgf_scissor[0] = x;
	rgf_scissor[1] = y;
	rgf_scissor[2] = width;
	rgf_scissor[3] = height;
}

qboolean RG_GetScissor (int box[4])
{
	memcpy (box, rgf_scissor, sizeof(rgf_scissor));
	return rgf_scissor_on;
}

static void *RGF_Grow (void *p, Uint32 *max, Uint32 need, const char *what)
{
	Uint32	n = *max ? *max : 65536;

	if (need <= *max)
		return p;
	while (n < need)
	{
		if (n > 0x7FFFFFFFu)
			Sys_Error ("SDL_GPU: the %s would pass 4 GB", what);
		n *= 2;
	}
	p = realloc (p, n);
	if (!p)
		Sys_Error ("SDL_GPU: out of memory growing the %s to %u bytes", what, (unsigned int)n);
	*max = n;
	return p;
}

/* Reserve `len` bytes of stream, 16-byte aligned; the offset. */
static Uint32 RGF_StreamAlloc (Uint32 len)
{
	Uint32	ofs = (rgf_streamlen + RG_STREAM_ALIGN - 1) & ~(Uint32)(RG_STREAM_ALIGN - 1);

	rgf_stream = (Uint8 *) RGF_Grow (rgf_stream, &rgf_streammax, ofs + len, "stream arena");
	rgf_streamlen = ofs + len;
	return ofs;
}

/* ------------------------------------------------------------------ */
/* Targets and buffers                                                 */
/* ------------------------------------------------------------------ */

static void RGF_RetireBuffer (SDL_GPUBuffer *buf)
{
	if (!buf)
		return;
	if (rgf_numretired_bufs == (int)(sizeof(rgf_retired_bufs) / sizeof(rgf_retired_bufs[0])))
	{
		SDL_WaitForGPUIdle (rg_dev);
		while (rgf_numretired_bufs)
			SDL_ReleaseGPUBuffer (rg_dev, rgf_retired_bufs[--rgf_numretired_bufs]);
	}
	rgf_retired_bufs[rgf_numretired_bufs++] = buf;
}

static SDL_GPUTexture *RGF_MakeTarget (SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage,
				       int w, int h)
{
	SDL_GPUTextureCreateInfo	ci;
	SDL_GPUTexture			*t;

	SDL_zero (ci);
	ci.type = SDL_GPU_TEXTURETYPE_2D;
	ci.format = fmt;
	ci.usage = usage;
	ci.width = w;
	ci.height = h;
	ci.layer_count_or_depth = 1;
	ci.num_levels = 1;
	ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
	t = SDL_CreateGPUTexture (rg_dev, &ci);
	if (!t)
		Sys_Error ("SDL_GPU: cannot create a %dx%d scene target (%s)", w, h, SDL_GetError ());
	return t;
}

static void RGF_EnsureScene (int w, int h)
{
	if (rgf_color && rgf_tw == w && rgf_th == h)
		return;
	/* The old ones may be named by this frame's submitted passes; SDL
	 * keeps a released texture alive until they retire. */
	if (rgf_color)
		SDL_ReleaseGPUTexture (rg_dev, rgf_color);
	if (rgf_depth)
		SDL_ReleaseGPUTexture (rg_dev, rgf_depth);
	rgf_color = RGF_MakeTarget (rgf_color_fmt,
				    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h);
	rgf_depth = RGF_MakeTarget (rgf_depth_fmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, w, h);
	rgf_tw = w;
	rgf_th = h;
	rgf_tgt_dirty = true;
}

static void RGF_ResolveTarget (void)
{
	rgf_tgt_ok = RG_Target_Resolve (rgf_fbo, &rgf_tgt);
	rgf_tgt_dirty = false;
}

void RG_Frame_SceneTarget (rgtarget_t *out)
{
	SDL_zero (*out);
	out->color[0] = rgf_color;
	out->depth = rgf_depth;
	out->cfmt[0] = rgf_color_fmt;
	out->ncolor = 1;
	out->dfmt = rgf_depth_fmt;
	out->cusage[0] = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	out->dusage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	out->w = rgf_tw;
	out->h = rgf_th;
}

qboolean RG_Frame_TargetHolds (const SDL_GPUTexture *tex)
{
	int	i;

	if (!tex || !rgf_tgt_ok)
		return false;
	for (i = 0; i < rgf_tgt.ncolor; i++)
	{
		if (tex == rgf_tgt.color[i])
			return true;
	}
	return tex == rgf_tgt.depth;
}

SDL_GPUTextureFormat RG_Frame_DepthFormat (void)
{
	return rgf_depth_fmt;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

void RG_BeginFrame (int width, int height)
{
	if (width <= 0 || height <= 0)
		SDL_GetWindowSizeInPixels (rg_win, &width, &height);
	if (width <= 0 || height <= 0)
		width = height = 1;
	rgf_w = width;
	rgf_h = height;
	if (rgf_active)
		return;
	rgf_t_begin = SDL_GetTicksNS ();
	RGF_FencesWait (1);
	rgf_frame_fence_ns = SDL_GetTicksNS () - rgf_t_begin;
	rgf_frame_mid_ns = rgf_frame_acquire_ns = 0;
	rgf_frame_seg_ns = rgf_frame_comp_ns = rgf_frame_submit_ns = 0;
	rgf_frame_copies = 0;
	RGF_EnsureScene (width, height);
	rgf_active = true;
	rgf_cleared = false;
	rgf_tgt_dirty = true;
	rgf_frame_draws = rgf_frame_segments = rgf_frame_indexed = 0;
	rgf_frame_stream = 0;
	rgf_frame_ssbo = rgf_frame_standin = 0;
	rgf_frame_cpasses = rgf_frame_reads = rgf_frame_waits = 0;
	rgf_frame_indirect = rgf_frame_areads = 0;
	rgf_frame_depthonly = rgf_frame_layered = rgf_frame_mrt = 0;
	rgf_cdropped_base = RG_ComputeDropped ();
	RGF_ReadbacksAge ();
}

/* Snapshot of block `b` in the open segment's uniform arena. */
static Uint32 RGF_BlockOffset (rgblock_t *b)
{
	if (b->arena_valid && b->arena_segment == rgf_segment && b->arena_version == b->version)
		return b->arena_ofs;
	rgf_uarena = (Uint8 *) RGF_Grow (rgf_uarena, &rgf_umax, rgf_ulen + b->size + 16, "uniform arena");
	b->arena_ofs = rgf_ulen;
	memcpy (rgf_uarena + rgf_ulen, b->flushed, b->size);
	rgf_ulen += (b->size + 15) & ~15;
	b->arena_segment = rgf_segment;
	b->arena_version = b->version;
	b->arena_valid = true;
	return b->arena_ofs;
}

static qboolean RGF_StageHasBlock (const rgstage_t *st, const char *name)
{
	int	i;

	for (i = 0; i < st->num_ubos; i++)
	{
		if (!strcmp (st->ubos[i].name, name))
			return true;
	}
	return false;
}

/* The pose type this draw reads, as last flushed, which is what the draw's
 * uniform arena copy holds: VertParams' u_poseverttype, or for a shadow
 * caster, whose stage has no VertParams, ShadowPass's u_shadowpose.z
 * (sshadow_alias_vert, #340 P6b).  -1 when the stage has neither or the
 * block is not registered. */
static int RGF_PoseVertType (const rgstage_t *st)
{
	const char	*name;
	int		b, ofs, need;
	int		v;

	if (RGF_StageHasBlock (st, "VertParams"))
	{
		name = "VertParams";
		ofs = (int) (offsetof(r_vertparams_t, posei) + 2 * sizeof(int));
		need = (int) sizeof(r_vertparams_t);
	}
	else if (RGF_StageHasBlock (st, "ShadowPass"))
	{
		name = "ShadowPass";
		ofs = SHADOWPASS_POSE_FORMAT_OFS;
		need = SHADOWPASS_SIZE;
	}
	else
		return -1;
	b = RG_Frame_BlockIndex (name);
	if (b < 0 || rgb_blocks[b].size < need)
		return -1;
	memcpy (&v, rgb_blocks[b].flushed + ofs, sizeof(v));
	return v;
}

/* Whether storage slot `sb` of stage `st` may go unbound because this draw
 * provably does not read it (rg_ssbokind_t, #520). */
static qboolean RGF_StandinExpected (const rgstage_t *st, const rgssbo_t *sb)
{
	int	i, pv;

	switch (sb->kind)
	{
	case RG_SSBO_COMPUTE_FED:
		return !R_LightCluster_Available ();
	case RG_SSBO_POSE:
		/* Excused only when the pose type names ANOTHER pose block of this
		 * stage: salias_inst_vert reads MD3PoseBuffer at PV_MD3 and
		 * PoseBuffer otherwise, so at a type no block of the stage names
		 * (or a stage with one pose block, the skeletal) nothing is. */
		pv = RGF_PoseVertType (st);
		if (pv < 0 || pv == sb->format)
			return false;
		for (i = 0; i < st->num_ssbos; i++)
		{
			const rgssbo_t	*o = &st->ssbos[i];

			if (o != sb && o->kind == RG_SSBO_POSE && o->format == pv)
				return true;
		}
		return false;
	default:
		return false;
	}
}

/* False when the stage cannot be bound whole: the draw is dropped. */
static qboolean RGF_RecordStage (rgdraw_t *d, const rgprog_t *prog, int stage)
{
	const rgstage_t	*st = &prog->stage[stage];
	SDL_GPUTextureSamplerBinding *out = (stage == RPROG_STAGE_VERTEX) ? d->vs : d->fs;
	int		i, n = 0;

	for (i = 0; i < st->num_samplers; i++)
	{
		const rgsampler_t *s = &st->samplers[i];

		if (s->ms)
		{
			RG_NoteUnhandled ("multisample sampler (SDL_GPU samples none; P6) -- draw dropped");
			return false;
		}
		if (s->unit == RG_UNIT_NONE)
			RG_NoteUnhandled ("sampler with no GL unit mapping (bound to a placeholder)");
		if (!RG_Res_ForDraw (s->unit, (rg_textype_t) s->type, (rg_sampclass_t) s->sclass,
				     s->shadow, &out[s->binding]))
			return false;
		n = q_max (n, s->binding + 1);
	}
	if (stage == RPROG_STAGE_VERTEX)
		d->nvs = n;
	else
		d->nfs = n;

	/* Each storage slot gets the buffer at its block's GL binding point,
	 * as the GL program would read it; none bound, the zeroed stand-in. */
	{
		SDL_GPUBuffer	**slots = (stage == RPROG_STAGE_VERTEX) ? d->vssbo : d->fssbo;

		for (i = 0; i < st->num_ssbos; i++)
		{
			const rgssbo_t	*sb = &st->ssbos[i];
			void		*read;
			SDL_GPUBuffer	*buf = RG_Buf_Storage (sb->point, RGF_StandinExpected (st, sb),
							       &read);

			/* RG_Buf_Storage notes why when it has nothing */
			if (buf)
			{
				slots[sb->slot] = buf;
				rgf_pending_reads[rgf_npending_reads++] = read;
				rgf_pending_ssbo++;
			}
			else
			{
				slots[sb->slot] = rgf_ssbo;
				rgf_pending_standin++;
			}
		}
	}
	if (stage == RPROG_STAGE_VERTEX)
		d->nvssbo = (Uint8) st->num_ssbos;
	else
		d->nfssbo = (Uint8) st->num_ssbos;

	for (i = 0; i < st->num_ubos; i++)
	{
		rgubo_t		*u = (rgubo_t *) &st->ubos[i];
		rgpush_t	*p;

		if (u->block == -2)
		{
			u->block = RG_Frame_BlockIndex (u->name);
			if (u->block < 0)
				RG_NoteUnhandled ("uniform block no mirror is registered for (left unpushed)");
		}
		if (u->block < 0)
			continue;
		p = &d->push[d->npush++];
		p->stage = (Uint8) stage;
		p->slot = u->binding;
		p->ofs = RGF_BlockOffset (&rgb_blocks[u->block]);
		p->len = rgb_blocks[u->block].size;
	}
	return true;
}

/* The draw about to be recorded, with its pipeline and bindings, for the
 * current viewport and scissor; NULL when it draws nothing (no viewport, an
 * empty scissor) or cannot be drawn (no pipeline, a stage that cannot be
 * bound: counted).  The caller fills its geometry, then RGF_CommitDraw. */
static rgdraw_t *RGF_BeginDraw (const rgprog_t *prog, SDL_GPUPrimitiveType prim, Uint32 layout)
{
	rgdraw_t	*d;
	int		sc_x0, sc_y0, sc_x1, sc_y1;

	if (!rgf_vp_set)
		RG_SetViewport (0, 0, rgf_w, rgf_h);
	if (rgf_vp[2] <= 0 || rgf_vp[3] <= 0)
		return NULL;
	if (rgf_tgt_dirty)
		RGF_ResolveTarget ();
	if (!rgf_tgt_ok || (!rgf_tgt.ncolor && !rgf_tgt.depth))
	{
		/* GL draws nothing into an incomplete framebuffer either.  A
		 * depth-only one is drawn into (#340 P6b: the shadow maps). */
		RG_NoteUnhandled ("draw into an incomplete framebuffer, or one with nothing to draw into (dropped)");
		rgf_dropped++;
		return NULL;
	}

	/* GL clamps a scissor box to the target; SDL_GPU requires it.  An
	 * empty one passes nothing: drop the draw before recording any of it. */
	sc_x0 = 0; sc_y0 = 0; sc_x1 = rgf_tgt.w; sc_y1 = rgf_tgt.h;
	if (rgf_scissor_on)
	{
		sc_x0 = q_max (rgf_scissor[0], 0);
		sc_y0 = q_max (rgf_scissor[1], 0);
		sc_x1 = q_min (rgf_scissor[0] + rgf_scissor[2], rgf_tgt.w);
		sc_y1 = q_min (rgf_scissor[1] + rgf_scissor[3], rgf_tgt.h);
		if (sc_x1 <= sc_x0 || sc_y1 <= sc_y0)
			return NULL;
	}

	if (rgf_numdraws == rgf_maxdraws)
	{
		rgf_maxdraws = rgf_maxdraws ? rgf_maxdraws * 2 : 256;
		rgf_draws = (rgdraw_t *) realloc (rgf_draws, rgf_maxdraws * sizeof(rgdraw_t));
		if (!rgf_draws)
			Sys_Error ("SDL_GPU: out of memory recording draws");
	}
	d = &rgf_draws[rgf_numdraws];
	memset (d, 0, sizeof(*d));
	rgf_npending_reads = rgf_pending_ssbo = rgf_pending_standin = 0;
	d->pipe = RG_PipelineFor (prog, prim, layout, &rgf_tgt);
	if (!d->pipe)
	{
		rgf_dropped++;
		return NULL;
	}

	/* Bindings and uniforms first: a texture realized here may queue
	 * nothing, but a hazard flush must not split this draw's bytes. */
	if (!RGF_RecordStage (d, prog, RPROG_STAGE_VERTEX) ||
	    !RGF_RecordStage (d, prog, RPROG_STAGE_FRAGMENT))
	{
		rgf_dropped++;
		return NULL;
	}
	d->instances = 1;
	d->scissor.x = sc_x0;
	d->scissor.y = sc_y0;
	d->scissor.w = sc_x1 - sc_x0;
	d->scissor.h = sc_y1 - sc_y0;
	return d;
}

static void RGF_CommitDraw (rgdraw_t *d, const rpipestate_t *ps)
{
	d->vp.x = (float) rgf_vp[0];
	d->vp.y = (float) rgf_vp[1];
	d->vp.w = (float) rgf_vp[2];
	d->vp.h = (float) rgf_vp[3];
	d->vp.min_depth = (float) ps->depth_near;
	d->vp.max_depth = (float) ps->depth_far;
	/* Dynamic state, not in the pipeline key; GL clamps the reference to
	 * the stencil buffer's 8 bits, a Uint8 cast would wrap it. */
	d->stencil_ref = (Uint8) CLAMP (0, ps->stencil_ref, 255);
	rgf_numdraws++;
	while (rgf_npending_reads > 0)
		RG_Buf_StampRead (rgf_pending_reads[--rgf_npending_reads]);
	rgf_frame_ssbo += rgf_pending_ssbo;
	rgf_frame_standin += rgf_pending_standin;
	rgf_pending_ssbo = rgf_pending_standin = 0;
}

void RG_DrawImm (GLenum mode, const void *verts, int count, int stride)
{
	const rpipestate_t	*ps = R_PipelineState ();
	const rgprog_t		*prog = RG_Program (ps->program);
	SDL_GPUPrimitiveType	prim;
	rgdraw_t		*d;
	Uint32			nidx = 0;
	Uint8			indexed = RGD_ARRAYS;
	int			i;

	if (count <= 0)
		return;
	if (!prog || prog->compute)
	{
		RG_NoteUnhandled ("immediate draw with no SDL_GPU program bound");
		return;
	}
	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);

	switch (mode)
	{
	case GL_QUADS:
		count -= count % 4;
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		indexed = RGD_QUADS;
		nidx = (Uint32)(count / 4) * 6;
		break;
	case GL_POLYGON:
	case GL_TRIANGLE_FAN:
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		indexed = RGD_STREAM;
		nidx = (count >= 3) ? (Uint32)(count - 2) * 3 : 0;
		break;
	case GL_TRIANGLES:
		count -= count % 3;
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		break;
	case GL_TRIANGLE_STRIP:
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
		break;
	case GL_LINES:
		count -= count % 2;
		prim = SDL_GPU_PRIMITIVETYPE_LINELIST;
		break;
	case GL_LINE_STRIP:
		prim = SDL_GPU_PRIMITIVETYPE_LINESTRIP;
		break;
	case GL_LINE_LOOP:
		prim = SDL_GPU_PRIMITIVETYPE_LINESTRIP;
		indexed = RGD_STREAM;
		nidx = (count >= 2) ? (Uint32)count + 1 : 0;
		break;
	case GL_POINTS:
		prim = SDL_GPU_PRIMITIVETYPE_POINTLIST;
		break;
	default:
		RG_NoteUnhandled ("immediate draw in a GL primitive mode with no SDL_GPU equivalent");
		return;
	}
	if (count <= 0 || (indexed != RGD_ARRAYS && !nidx))
		return;

	d = RGF_BeginDraw (prog, prim, RG_VL_IMM);
	if (!d)
		return;

	d->nvb = 1;
	d->vb[0] = NULL;
	d->vb_ofs[0] = RGF_StreamAlloc ((Uint32)count * stride);
	memcpy (rgf_stream + d->vb_ofs[0], verts, (size_t)count * stride);
	d->indexed = indexed;
	d->ib_size = SDL_GPU_INDEXELEMENTSIZE_16BIT;
	d->count = (indexed != RGD_ARRAYS) ? nidx : (Uint32)count;
	if (indexed == RGD_STREAM)
	{
		Uint16	*ix;

		rgf_stream_indices = true;
		d->ib_ofs = RGF_StreamAlloc (nidx * sizeof(Uint16));
		ix = (Uint16 *)(rgf_stream + d->ib_ofs);
		if (mode == GL_LINE_LOOP)
		{
			for (i = 0; i < count; i++)
				ix[i] = (Uint16) i;
			ix[count] = 0;
		}
		else
		{
			for (i = 1; i + 1 < count; i++, ix += 3)
			{
				ix[0] = 0;
				ix[1] = (Uint16) i;
				ix[2] = (Uint16)(i + 1);
			}
		}
	}
	RGF_CommitDraw (d, ps);
}

/* The constant slot's record in the open segment's stream: one FLOAT4 per
 * constant location, repeated per instance (the slot is instance-rate).  A
 * single-instance record is reused within a segment while the values hold,
 * which is every world draw in it (white). */
static Uint32	rgf_const_ofs, rgf_const_segment;
static int	rgf_const_n;
static float	rgf_const_vals[RG_MAX_VATTRS][4];

static Uint32 RGF_ConstRecord (const rgvinput_t *in, Uint32 instances)
{
	Uint32	rec = (Uint32)(in->nconst * 4 * sizeof(float)), ofs, i;

	if (instances == 1 && rgf_const_segment == rgf_segment && rgf_const_n == in->nconst &&
	    !memcmp (rgf_const_vals, in->consts, rec))
		return rgf_const_ofs;
	ofs = RGF_StreamAlloc (rec * instances);
	for (i = 0; i < instances; i++)
		memcpy (rgf_stream + ofs + i * rec, in->consts, rec);
	if (instances == 1)
	{
		rgf_const_ofs = ofs;
		rgf_const_segment = rgf_segment;
		rgf_const_n = in->nconst;
		memcpy (rgf_const_vals, in->consts, rec);
	}
	return ofs;
}

void RG_DrawIndexed (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances)
{
	const rpipestate_t	*ps = R_PipelineState ();
	const rgprog_t		*prog = RG_Program (ps->program);
	SDL_GPUPrimitiveType	prim;
	rgvinput_t		in;
	rgdraw_t		*d;
	Uint32			layout, isize, first;
	int			s;

	if (count <= 0 || instances <= 0)
		return;
	if (!prog || prog->compute)
	{
		RG_NoteUnhandled ("indexed draw with no SDL_GPU program bound");
		return;
	}
	switch (mode)
	{
	case GL_TRIANGLES:	prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
	case GL_TRIANGLE_STRIP:	prim = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
	case GL_LINES:		prim = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
	case GL_LINE_STRIP:	prim = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
	case GL_POINTS:		prim = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
	default:
		/* Fans, loops and quads would need their indices rewritten, and
		 * these live in a GPU buffer; no routed path draws one. */
		RG_NoteUnhandled ("indexed draw in a GL primitive mode with no SDL_GPU equivalent (dropped)");
		return;
	}
	/* GL's type is the index width; SDL_GPU has no 8-bit indices. */
	switch (type)
	{
	case GL_UNSIGNED_SHORT:	isize = 2; break;
	case GL_UNSIGNED_INT:	isize = 4; break;
	default:
		RG_NoteUnhandled ("indexed draw with GL_UNSIGNED_BYTE or an unknown index type (dropped)");
		return;
	}
	/* `indices` is a byte offset into the element buffer; SDL_GPU counts
	 * indices. */
	if ((uintptr_t) indices % isize)
	{
		RG_NoteUnhandled ("indexed draw at an offset that is not a whole index (dropped)");
		return;
	}
	first = (Uint32)((uintptr_t) indices / isize);
	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);
	if (!RG_Buf_ForDraw ((int) isize, &in))
	{
		rgf_dropped++;
		return;
	}
	RG_Buf_FeedInputs (&in, prog->stage[RPROG_STAGE_VERTEX].in_mask,
			   prog->stage[RPROG_STAGE_VERTEX].in_int_mask,
			   prog->stage[RPROG_STAGE_VERTEX].in_sint_mask);
	/* GL reads past the data store as an error or as zeros (robust
	 * access); a Vulkan read past the buffer is undefined. */
	if ((Uint64) first + (Uint64) count > in.ib_count)
	{
		RG_NoteUnhandled ("indexed draw past the end of its element buffer (dropped)");
		rgf_dropped++;
		return;
	}
	layout = RG_LayoutIntern (&in.layout);

	d = RGF_BeginDraw (prog, prim, layout);
	if (!d)
		return;
	RG_Buf_MarkRead (&in);
	d->nvb = (Uint8) in.layout.num_slots;
	for (s = 0; s < d->nvb; s++)
	{
		d->vb[s] = in.vb[s];
		d->vb_ofs[s] = in.vb_ofs[s];
	}
	if (in.const_slot >= 0)
		d->vb_ofs[in.const_slot] = RGF_ConstRecord (&in, (Uint32) instances);
	d->indexed = RGD_BUFFER;
	d->ib = in.ib;
	d->ib_size = (isize == 4) ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT;
	d->first_index = first;
	d->count = (Uint32) count;
	d->instances = (Uint32) instances;
	RGF_CommitDraw (d, ps);
}

/* GL's DrawElementsIndirectCommand, SDL_GPUIndexedIndirectDrawCommand's
 * layout: count, instances, first index, base vertex, first instance. */
#define RG_INDIRECT_CMD_BYTES	20u

void RG_DrawElementsIndirect (GLenum mode, GLenum type, const void *indirect)
{
	const rpipestate_t	*ps = R_PipelineState ();
	const rgprog_t		*prog = RG_Program (ps->program);
	rgvinput_t		in;
	rgdraw_t		*d;
	SDL_GPUBuffer		*cmdbuf;
	void			*cmdread;
	Uint32			layout, isize;
	int			s;

	if (!prog || prog->compute)
	{
		RG_NoteUnhandled ("indirect draw with no SDL_GPU program bound");
		return;
	}
	/* The one caller, the world cull, draws triangles (R_DrawWorldCulled). */
	if (mode != GL_TRIANGLES)
	{
		RG_NoteUnhandled ("indirect draw in a GL primitive mode other than GL_TRIANGLES (dropped)");
		return;
	}
	switch (type)
	{
	case GL_UNSIGNED_SHORT:	isize = 2; break;
	case GL_UNSIGNED_INT:	isize = 4; break;
	default:
		RG_NoteUnhandled ("indirect draw with GL_UNSIGNED_BYTE or an unknown index type (dropped)");
		return;
	}
	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);
	cmdbuf = RG_Buf_Indirect ((GLintptr)(uintptr_t) indirect, RG_INDIRECT_CMD_BYTES, &cmdread);
	if (!cmdbuf || !RG_Buf_ForDraw ((int) isize, &in))
	{
		rgf_dropped++;
		return;
	}
	RG_Buf_FeedInputs (&in, prog->stage[RPROG_STAGE_VERTEX].in_mask,
			   prog->stage[RPROG_STAGE_VERTEX].in_int_mask,
			   prog->stage[RPROG_STAGE_VERTEX].in_sint_mask);
	/* See RGB_Usage: an element buffer with vertex usage is read as
	 * indices with no barrier after the pass that wrote them. */
	if (in.ib_usage & SDL_GPU_BUFFERUSAGE_VERTEX)
		RG_NoteUnhandled ("indirect draw from an element buffer with vertex usage "
				  "(its indices are read unsynchronised)");
	layout = RG_LayoutIntern (&in.layout);

	d = RGF_BeginDraw (prog, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, layout);
	if (!d)
		return;
	RG_Buf_MarkRead (&in);
	RG_Buf_StampRead (cmdread);
	d->nvb = (Uint8) in.layout.num_slots;
	for (s = 0; s < d->nvb; s++)
	{
		d->vb[s] = in.vb[s];
		d->vb_ofs[s] = in.vb_ofs[s];
	}
	/* one record: the command's first instance is 0 and its instances 1
	 * (cull_clear_comp writes them) */
	if (in.const_slot >= 0)
		d->vb_ofs[in.const_slot] = RGF_ConstRecord (&in, 1);
	d->indexed = RGD_INDIRECT;
	d->ib = in.ib;
	d->ib_size = (isize == 4) ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT;
	d->indirect = cmdbuf;
	d->indirect_ofs = (Uint32)(uintptr_t) indirect;
	RGF_CommitDraw (d, ps);
}

void RG_DrawArrays (GLenum mode, GLint first, GLsizei count)
{
	const rpipestate_t	*ps = R_PipelineState ();
	const rgprog_t		*prog = RG_Program (ps->program);
	SDL_GPUPrimitiveType	prim;
	rgdraw_t		*d;

	if (count <= 0 || first < 0)
		return;
	if (!prog || prog->compute)
	{
		RG_NoteUnhandled ("array draw with no SDL_GPU program bound");
		return;
	}
	switch (mode)
	{
	case GL_TRIANGLES:
		count -= count % 3;
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		break;
	case GL_TRIANGLE_STRIP:	prim = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
	case GL_LINES:
		count -= count % 2;
		prim = SDL_GPU_PRIMITIVETYPE_LINELIST;
		break;
	case GL_LINE_STRIP:	prim = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
	case GL_POINTS:		prim = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
	default:
		RG_NoteUnhandled ("array draw in a GL primitive mode with no SDL_GPU equivalent (dropped)");
		return;
	}
	if (count <= 0)
		return;
	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);
	if (!RG_Buf_Bufferless ())
	{
		rgf_dropped++;
		return;
	}
	d = RGF_BeginDraw (prog, prim, RG_VL_NONE);
	if (!d)
		return;
	d->nvb = 0;
	d->indexed = RGD_ARRAYS;
	d->first_vertex = (Uint32) first;
	d->count = (Uint32) count;
	RGF_CommitDraw (d, ps);
}

/* ------------------------------------------------------------------ */
/* Segments                                                            */
/* ------------------------------------------------------------------ */

static qboolean RGF_EnsureBuffer (SDL_GPUBuffer **buf, Uint32 *size, Uint32 need,
				  SDL_GPUBufferUsageFlags usage)
{
	SDL_GPUBufferCreateInfo	ci;
	Uint32			n = *size ? *size : 1u << 20;

	if (*buf && need <= *size)
		return true;
	while (n < need)
		n *= 2;
	SDL_zero (ci);
	ci.usage = usage;
	ci.size = n;
	RGF_RetireBuffer (*buf);
	*buf = SDL_CreateGPUBuffer (rg_dev, &ci);
	*size = *buf ? n : 0;
	if (!*buf)
		RGF_Warn ("SDL_GPU: cannot create a %u-byte buffer (%s)\n", (unsigned int)n, SDL_GetError ());
	return *buf != NULL;
}

static qboolean RGF_EnsureTransfer (Uint32 need)
{
	SDL_GPUTransferBufferCreateInfo	ci;
	Uint32				n = rgf_tbuf_size ? rgf_tbuf_size : 1u << 20;

	if (rgf_tbuf && need <= rgf_tbuf_size)
		return true;
	while (n < need)
		n *= 2;
	if (rgf_tbuf)
		SDL_ReleaseGPUTransferBuffer (rg_dev, rgf_tbuf);
	SDL_zero (ci);
	ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	ci.size = n;
	rgf_tbuf = SDL_CreateGPUTransferBuffer (rg_dev, &ci);
	rgf_tbuf_size = rgf_tbuf ? n : 0;
	if (!rgf_tbuf)
		RGF_Warn ("SDL_GPU: cannot create a %u-byte transfer buffer (%s)\n",
			    (unsigned int)n, SDL_GetError ());
	return rgf_tbuf != NULL;
}

static qboolean RGF_EnsureCmd (void)
{
	if (rgf_cmd)
		return true;
	rgf_cmd = SDL_AcquireGPUCommandBuffer (rg_dev);
	if (!rgf_cmd)
		RGF_Warn ("SDL_GPU: no command buffer (%s)\n", SDL_GetError ());
	return rgf_cmd != NULL;
}

/* The copy pass: stream, quad indices (once), texture uploads. */
static qboolean RGF_CopyPass (void)
{
	Uint32			stream = (rgf_streamlen + 15) & ~15u;
	Uint32			quad = rgf_quad_uploaded ? 0 : RG_QUAD_INDICES * sizeof(Uint16);
	Uint32			ssbo = (rgf_ssbo_uploaded || !rgf_ssbo) ? 0 : RG_SSBO_STANDIN_BYTES;
	Uint32			tex = RG_Res_PendingBytes ();
	Uint32			bufs = RG_Buf_PendingBytes ();
	Uint32			total = stream + quad + ssbo + tex + bufs;
	Uint8			*map;
	SDL_GPUCopyPass		*copy;
	SDL_GPUTransferBufferLocation src;
	SDL_GPUBufferRegion	dst;

	if (!total)
		return true;
	if (!RGF_EnsureTransfer (total))
		return false;
	if (stream && !RGF_EnsureBuffer (&rgf_vbuf, &rgf_vbuf_size, stream, SDL_GPU_BUFFERUSAGE_VERTEX))
		return false;
	if (stream && rgf_stream_indices &&
	    !RGF_EnsureBuffer (&rgf_ibuf, &rgf_ibuf_size, stream, SDL_GPU_BUFFERUSAGE_INDEX))
		return false;
	if (quad && !rgf_quadbuf)
	{
		Uint32	qsize = 0;

		if (!RGF_EnsureBuffer (&rgf_quadbuf, &qsize, quad, SDL_GPU_BUFFERUSAGE_INDEX))
			return false;
	}
	map = (Uint8 *) SDL_MapGPUTransferBuffer (rg_dev, rgf_tbuf, true);
	if (!map)
	{
		RGF_Warn ("SDL_GPU: cannot map the transfer buffer (%s)\n", SDL_GetError ());
		return false;
	}
	if (rgf_streamlen)
		memcpy (map, rgf_stream, rgf_streamlen);
	if (quad)
	{
		Uint16	*ix = (Uint16 *)(map + stream);
		int	i;

		for (i = 0; i < RG_QUAD_INDICES / 6; i++, ix += 6)
		{
			ix[0] = (Uint16)(i * 4 + 0);
			ix[1] = (Uint16)(i * 4 + 1);
			ix[2] = (Uint16)(i * 4 + 2);
			ix[3] = (Uint16)(i * 4 + 0);
			ix[4] = (Uint16)(i * 4 + 2);
			ix[5] = (Uint16)(i * 4 + 3);
		}
	}
	if (ssbo)
		memset (map + stream + quad, 0, ssbo);
	if (tex)
		RG_Res_StageUploads (map + stream + quad + ssbo, stream + quad + ssbo);
	if (bufs)
		RG_Buf_StageUploads (map + stream + quad + ssbo + tex, stream + quad + ssbo + tex);
	SDL_UnmapGPUTransferBuffer (rg_dev, rgf_tbuf);

	copy = SDL_BeginGPUCopyPass (rgf_cmd);
	RG_Frame_CountCopyPass ();
	if (stream)
	{
		src.transfer_buffer = rgf_tbuf;
		src.offset = 0;
		dst.buffer = rgf_vbuf;
		dst.offset = 0;
		dst.size = stream;
		/* cycle: an earlier segment's draws still read the old contents */
		SDL_UploadToGPUBuffer (copy, &src, &dst, true);
		if (rgf_stream_indices)
		{
			dst.buffer = rgf_ibuf;
			SDL_UploadToGPUBuffer (copy, &src, &dst, true);
		}
	}
	if (quad)
	{
		src.transfer_buffer = rgf_tbuf;
		src.offset = stream;
		dst.buffer = rgf_quadbuf;
		dst.offset = 0;
		dst.size = quad;
		SDL_UploadToGPUBuffer (copy, &src, &dst, false);
		rgf_quad_uploaded = true;
	}
	if (ssbo)
	{
		src.transfer_buffer = rgf_tbuf;
		src.offset = stream + quad;
		dst.buffer = rgf_ssbo;
		dst.offset = 0;
		dst.size = ssbo;
		SDL_UploadToGPUBuffer (copy, &src, &dst, false);
		rgf_ssbo_uploaded = true;
	}
	if (tex)
		RG_Res_IssueUploads (copy, rgf_tbuf);
	if (bufs)
		RG_Buf_IssueUploads (copy, rgf_tbuf);
	SDL_EndGPUCopyPass (copy);
	return true;
}

static void RGF_Replay (SDL_GPURenderPass *pass)
{
	SDL_GPUGraphicsPipeline	*pipe = NULL;
	Uint32			pushed[2][RG_MAX_STAGE_UBOS];
	qboolean		have_vp = false, have_sc = false;
	SDL_GPUViewport		vp;
	SDL_Rect		sc;
	int			i, k, ref = -1;

	memset (pushed, 0xFF, sizeof(pushed));
	for (i = 0; i < rgf_numdraws; i++)
	{
		const rgdraw_t		*d = &rgf_draws[i];
		SDL_GPUBufferBinding	vb[RG_MAX_VBUFS];

		if (d->pipe != pipe)
		{
			SDL_BindGPUGraphicsPipeline (pass, d->pipe);
			pipe = d->pipe;
		}
		if (!have_vp || memcmp (&vp, &d->vp, sizeof(vp)))
		{
			vp = d->vp;
			SDL_SetGPUViewport (pass, &vp);
			have_vp = true;
		}
		if (!have_sc || memcmp (&sc, &d->scissor, sizeof(sc)))
		{
			sc = d->scissor;
			SDL_SetGPUScissor (pass, &sc);
			have_sc = true;
		}
		if (d->stencil_ref != ref)
		{
			ref = d->stencil_ref;
			SDL_SetGPUStencilReference (pass, (Uint8) ref);
		}
		for (k = 0; k < d->npush; k++)
		{
			const rgpush_t	*p = &d->push[k];

			if (pushed[p->stage][p->slot] == p->ofs)
				continue;
			pushed[p->stage][p->slot] = p->ofs;
			if (p->stage == RPROG_STAGE_VERTEX)
				SDL_PushGPUVertexUniformData (rgf_cmd, p->slot, rgf_uarena + p->ofs, p->len);
			else
				SDL_PushGPUFragmentUniformData (rgf_cmd, p->slot, rgf_uarena + p->ofs, p->len);
		}
		if (d->nvs)
			SDL_BindGPUVertexSamplers (pass, 0, d->vs, d->nvs);
		if (d->nfs)
			SDL_BindGPUFragmentSamplers (pass, 0, d->fs, d->nfs);
		if (d->nvssbo)
			SDL_BindGPUVertexStorageBuffers (pass, 0, d->vssbo, d->nvssbo);
		if (d->nfssbo)
			SDL_BindGPUFragmentStorageBuffers (pass, 0, d->fssbo, d->nfssbo);

		for (k = 0; k < d->nvb; k++)
		{
			vb[k].buffer = d->vb[k] ? d->vb[k] : rgf_vbuf;
			vb[k].offset = d->vb_ofs[k];
		}
		if (d->nvb)
			SDL_BindGPUVertexBuffers (pass, 0, vb, d->nvb);
		if (d->indexed != RGD_ARRAYS)
		{
			SDL_GPUBufferBinding	ib;

			ib.buffer = (d->indexed == RGD_QUADS) ? rgf_quadbuf :
				    (d->indexed == RGD_BUFFER || d->indexed == RGD_INDIRECT) ? d->ib : rgf_ibuf;
			ib.offset = (d->indexed == RGD_STREAM) ? d->ib_ofs : 0;
			SDL_BindGPUIndexBuffer (pass, &ib, d->ib_size);
			if (d->indexed == RGD_INDIRECT)
				SDL_DrawGPUIndexedPrimitivesIndirect (pass, d->indirect, d->indirect_ofs, 1);
			else
				SDL_DrawGPUIndexedPrimitives (pass, d->count, d->instances, d->first_index, 0, 0);
		}
		else
			SDL_DrawGPUPrimitives (pass, d->count, d->instances, d->first_vertex, 0);
	}
}

/* End the open segment: copy pass, then (in a frame) its render pass. */
/* The open segment's recording is spent: every arena offset in it dies, so
 * the serial moves on too (RGF_BlockOffset's cache is keyed on it). */
static void RGF_ResetSegment (void)
{
	rgf_numdraws = 0;
	rgf_streamlen = 0;
	rgf_stream_indices = false;
	rgf_ulen = 0;
	rgf_segment++;
}

static void RGF_EndSegment (void)
{
	qboolean	first;
	int		i;
	Uint64		t0 = SDL_GetTicksNS ();

	if (!RGF_EnsureCmd ())
	{
		RGF_ResetSegment ();
		return;
	}
	/* Nothing is recorded against a dirty target: see rgf_tgt_dirty. */
	if (rgf_tgt_dirty)
		RGF_ResolveTarget ();
	/* Ahead of the copy pass, so an upload into a new target lands after
	 * its clear, not under it. */
	RG_Res_ClearTargets (rgf_cmd);
	if (!RGF_CopyPass ())
	{
		rgf_dropped += rgf_numdraws;	/* counted: RG_DrawsDropped's callers rely on it */
		rgf_numdraws = 0;	/* nothing to read; drawing would fault */
	}
	RG_Res_ClearPlaceholders (rgf_cmd);

	first = (rgf_fbo == 0 && !rgf_cleared);
	if (rgf_active && (rgf_numdraws || first || rgf_clear_mask))
	{
		SDL_GPUColorTargetInfo		ct[RG_MAX_COLOR];
		SDL_GPUDepthStencilTargetInfo	ds;
		SDL_GPURenderPass		*pass;
		const float			*cc = RG_ClearColor ();

		if (!rgf_tgt_ok || (!rgf_tgt.ncolor && !rgf_tgt.depth))
		{
			/* Draws into it were dropped as they were recorded. */
			RG_NoteUnhandled ("glClear of an incomplete framebuffer (ignored)");
			rgf_numdraws = 0;
		}
		else
		{
			SDL_zero (ct);
			for (i = 0; i < rgf_tgt.ncolor; i++)
			{
				ct[i].texture = rgf_tgt.color[i];
				ct[i].store_op = SDL_GPU_STOREOP_STORE;
				/* Never cycled, any attachment: a pass draws into
				 * what the passes before it left, and a cycle hands
				 * it a fresh texture.  For the shadow maps' cube-map
				 * array that would be every cached map lost at the
				 * first face drawn (#340 P6b). */
				ct[i].cycle = false;
				/* Each target its own clear (#340 P6c: WBOIT's
				 * accum to 0, revealage to 1). */
				if (rgf_clear_cbits & (1u << i))
				{
					ct[i].load_op = SDL_GPU_LOADOP_CLEAR;
					ct[i].clear_color.r = rgf_clear_rgba[i][0];
					ct[i].clear_color.g = rgf_clear_rgba[i][1];
					ct[i].clear_color.b = rgf_clear_rgba[i][2];
					ct[i].clear_color.a = rgf_clear_rgba[i][3];
				}
				else if (first)
				{
					ct[i].load_op = SDL_GPU_LOADOP_CLEAR;
					ct[i].clear_color.r = cc[0];
					ct[i].clear_color.g = cc[1];
					ct[i].clear_color.b = cc[2];
					ct[i].clear_color.a = 1.0f;
				}
				else
					ct[i].load_op = SDL_GPU_LOADOP_LOAD;
			}
			SDL_zero (ds);
			ds.texture = rgf_tgt.depth;
			ds.layer = (Uint8) rgf_tgt.dlayer;
			ds.cycle = false;
			ds.load_op = ((rgf_clear_mask & GL_DEPTH_BUFFER_BIT) || first)
					? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
			ds.store_op = SDL_GPU_STOREOP_STORE;
			ds.stencil_load_op = ((rgf_clear_mask & GL_STENCIL_BUFFER_BIT) || first)
					? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
			ds.stencil_store_op = SDL_GPU_STOREOP_STORE;
			ds.clear_depth = (rgf_clear_mask & GL_DEPTH_BUFFER_BIT) ? rgf_pass_clear_depth
										 : rgf_clear_depth;
			ds.clear_stencil = rgf_clear_stencil;
			pass = SDL_BeginGPURenderPass (rgf_cmd, rgf_tgt.ncolor ? ct : NULL,
						       (Uint32) rgf_tgt.ncolor, rgf_tgt.depth ? &ds : NULL);
			if (pass)
			{
				RGF_Replay (pass);
				SDL_EndGPURenderPass (pass);
			}
			if (rgf_fbo == 0)
				rgf_cleared = true;
			rgf_frame_draws += rgf_numdraws;
			rgf_frame_segments++;
			if (!rgf_tgt.ncolor)
			{
				rgf_frame_depthonly++;
				rgf_frame_layered += rgf_tgt.dlayered;
			}
			rgf_frame_mrt += (rgf_tgt.ncolor > 1);
			for (i = 0; i < rgf_numdraws; i++)
			{
				rgf_frame_indexed += (rgf_draws[i].indexed == RGD_BUFFER);
				rgf_frame_indirect += (rgf_draws[i].indexed == RGD_INDIRECT);
			}
		}
		rgf_clear_mask = 0;
		rgf_clear_cbits = 0;
	}
	rgf_frame_stream += rgf_streamlen;
	RGF_ResetSegment ();
	rgf_frame_seg_ns += SDL_GetTicksNS () - t0;
}

void RG_Frame_BindTarget (GLuint fbo)
{
	rgtarget_t	t;
	qboolean	ok;

	SDL_zero (t);
	ok = RG_Target_Resolve (fbo, &t);
	if (fbo == rgf_fbo && !rgf_tgt_dirty && ok == rgf_tgt_ok &&
	    (!ok || !memcmp (&t, &rgf_tgt, sizeof(t))))
		return;
	/* The old target's draws and clear go into a pass of their own. */
	if (rgf_numdraws || rgf_clear_mask)
		RGF_EndSegment ();
	rgf_fbo = fbo;
	rgf_tgt = t;
	rgf_tgt_ok = ok;
	rgf_tgt_dirty = false;
}

void RG_Frame_ReleaseTarget (const SDL_GPUTexture *tex)
{
	if (!RG_Frame_TargetHolds (tex))
		return;
	if (rgf_numdraws || rgf_clear_mask)
		RGF_EndSegment ();
	rgf_tgt_dirty = true;
}

SDL_GPUCommandBuffer *RG_Frame_Break (void)
{
	if (!rgf_active)
		return NULL;
	RGF_EndSegment ();
	return rgf_cmd;
}

/* Queue `mask`'s clear for the next pass; `keep` false leaves the depth a
 * frame's first pass clears to as it was (glClearBufferfv, RG_ClearBuffer).
 * `drawbuffer` -1 clears colour into every colour target, as glClear does;
 * otherwise into that draw buffer's alone, as glClearBufferfv (GL_COLOR). */
static void RGF_QueueClear (GLbitfield mask, const float color[4], float depth, int stencil,
			    qboolean keep, int drawbuffer);

void RG_Clear (GLbitfield mask, const float color[4], float depth, int stencil)
{
	RGF_QueueClear (mask, color, depth, stencil, true, -1);
}

void RG_ClearBuffer (GLenum buffer, GLint drawbuffer, const GLfloat *value)
{
	if (buffer == GL_DEPTH && drawbuffer == 0)
	{
		RGF_QueueClear (GL_DEPTH_BUFFER_BIT, NULL, value[0], 0, false, -1);
		return;
	}
	if (buffer == GL_COLOR && drawbuffer >= 0 && drawbuffer < RG_MAX_COLOR)
	{
		RGF_QueueClear (GL_COLOR_BUFFER_BIT, value, 0.0f, 0, false, drawbuffer);
		return;
	}
	/* GL_STENCIL and GL_DEPTH_STENCIL nothing calls. */
	RG_NoteUnhandled ("glClearBufferfv other than GL_DEPTH or GL_COLOR 0 .. RG_MAX_COLOR-1 (ignored)");
}

static void RGF_QueueClear (GLbitfield mask, const float color[4], float depth, int stencil,
			    qboolean keep, int drawbuffer)
{
	int	i;

	const rpipestate_t	*ps = R_PipelineState ();

	/* glClear writes through the depth and colour masks. */
	mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	if (!ps->depth_mask)
		mask &= ~GL_DEPTH_BUFFER_BIT;
	if (!ps->color_mask[0] && !ps->color_mask[1] && !ps->color_mask[2] && !ps->color_mask[3])
		mask &= ~GL_COLOR_BUFFER_BIT;
	else if ((mask & GL_COLOR_BUFFER_BIT) &&
		 !(ps->color_mask[0] && ps->color_mask[1] && ps->color_mask[2] && ps->color_mask[3]))
		RG_NoteUnhandled ("glClear under a partial colour mask (every channel cleared)");
	if (!mask)
		return;
	/* A load op covers the whole target. */
	if (rgf_scissor_on)
		RG_NoteUnhandled ("glClear inside a scissor box (the whole target cleared)");
	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);
	/* The pass clears before its first draw, so what this segment has
	 * recorded already has to go out in a pass of its own. */
	if (rgf_numdraws)
		RGF_EndSegment ();
	/* The clear is the target's as GL has it now, realized, so an upload
	 * into it after this sees RG_Frame_TargetHolds and lands after it. */
	if (rgf_tgt_dirty)
		RGF_ResolveTarget ();
	/* glClearBufferfv into a draw buffer that is GL_NONE (past the
	 * target's colour targets) writes nothing. */
	if (drawbuffer >= 0 && rgf_tgt_ok && drawbuffer >= rgf_tgt.ncolor)
		return;
	rgf_clear_mask |= mask;
	if (mask & GL_COLOR_BUFFER_BIT)
	{
		for (i = 0; i < RG_MAX_COLOR; i++)
		{
			if (drawbuffer < 0 || drawbuffer == i)
			{
				memcpy (rgf_clear_rgba[i], color, sizeof(rgf_clear_rgba[i]));
				rgf_clear_cbits |= 1u << i;
			}
		}
	}
	if (mask & GL_DEPTH_BUFFER_BIT)
	{
		rgf_pass_clear_depth = depth;
		if (keep)
			rgf_clear_depth = depth;
	}
	if (mask & GL_STENCIL_BUFFER_BIT)
		rgf_clear_stencil = (Uint8) stencil;
}

/* ------------------------------------------------------------------ */
/* Asynchronous readbacks (#340 P5c)                                   */
/* ------------------------------------------------------------------ */

/* A ring of downloads.  Each goes into the command buffer open when it is
 * asked for, and is read once the fence of the submit that carried that
 * command buffer has signalled: the frame's own at its end
 * (SDL_SubmitGPUCommandBufferAndAcquireFence, taken only when a download is
 * aboard), or at once after a mid-frame submit, which waits anyway.  Nothing
 * stalls for it; gl_hiz_stats reads one or two frames late. */
#define RG_READBACKS		8
/* A completed read nobody collected is reclaimed after this many frames. */
#define RG_READBACK_ABANDON	8

enum {
	RGR_FREE,
	RGR_RECORDED,		/* in rgf_cmd, not submitted */
	RGR_SUBMITTED,		/* submitted; `fence` signals when it is done */
	RGR_READY,		/* the bytes are in `tb` */
	RGR_FAILED		/* the submit or the wait failed */
};

typedef struct {
	int			state;
	unsigned int		ticket;
	SDL_GPUTransferBuffer	*tb;
	Uint32			cap, size;
	SDL_GPUFence		*fence;		/* shared by every read of one submit */
	unsigned long		done_frame;	/* rgf_frames when it became READY / FAILED */
	qboolean		discard;	/* nobody will collect it: freed once done */
} rgreadback_t;

static rgreadback_t	rgr_ring[RG_READBACKS];
static unsigned int	rgr_ticket;
static unsigned long	rgr_requested, rgr_collected, rgr_refused, rgr_abandoned, rgr_failed, rgr_discarded;

static void RGF_ReadbackDone (rgreadback_t *r, int state)
{
	r->state = state;
	r->fence = NULL;
	r->done_frame = rgf_frames;
	if (state == RGR_FAILED)
		rgr_failed++;
}

/* rgf_cmd went out: with `fence` (NULL: the submit failed) the reads it
 * carried complete when it signals; `waited`, they are complete now. */
static void RGF_ReadbacksSubmitted (SDL_GPUFence *fence, qboolean waited)
{
	int	i;

	for (i = 0; i < RG_READBACKS; i++)
	{
		rgreadback_t	*r = &rgr_ring[i];

		if (r->state != RGR_RECORDED)
			continue;
		if (waited)
			RGF_ReadbackDone (r, RGR_READY);
		else if (!fence)
			RGF_ReadbackDone (r, RGR_FAILED);
		else
		{
			r->state = RGR_SUBMITTED;
			r->fence = fence;
		}
	}
}

/* `fence` has signalled (or its wait failed): every read it carried is
 * `state`, READY or FAILED.  The fence is the frame fences' (rgf_fences),
 * which release it (RGF_FencesWait). */
static void RGF_ReadbackFenceDone (SDL_GPUFence *fence, int state)
{
	int	i;

	for (i = 0; i < RG_READBACKS; i++)
	{
		if (rgr_ring[i].state == RGR_SUBMITTED && rgr_ring[i].fence == fence)
			RGF_ReadbackDone (&rgr_ring[i], state);
	}
}

static void RGF_ReadbacksAge (void)
{
	int	i;

	for (i = 0; i < RG_READBACKS; i++)
	{
		rgreadback_t	*r = &rgr_ring[i];

		if (r->state == RGR_SUBMITTED && SDL_QueryGPUFence (rg_dev, r->fence))
			RGF_ReadbackFenceDone (r->fence, RGR_READY);
		if ((r->state == RGR_READY || r->state == RGR_FAILED) &&
		    (r->discard || rgf_frames - r->done_frame > RG_READBACK_ABANDON))
		{
			if (r->discard)
				rgr_discarded++;
			else
				rgr_abandoned++;
			r->state = RGR_FREE;
		}
	}
}

unsigned int RG_Frame_ReadbackRequest (const SDL_GPUBufferRegion *buf)
{
	rgreadback_t		*r = NULL;
	SDL_GPUCopyPass		*copy;
	SDL_GPUTransferBufferLocation d;
	int			i;

	/* No frame, no frame fence to read it by: the caller reads
	 * synchronously there (RG_ReadBuffer). */
	if (!rgf_active || !buf || !buf->size)
	{
		rgr_refused++;
		return 0;
	}
	for (i = 0; i < RG_READBACKS && !r; i++)
	{
		if (rgr_ring[i].state == RGR_FREE)
			r = &rgr_ring[i];
	}
	if (!r)
	{
		rgr_refused++;
		return 0;
	}
	if (!r->tb || r->cap < buf->size)
	{
		SDL_GPUTransferBufferCreateInfo	ci;

		if (r->tb)
			SDL_ReleaseGPUTransferBuffer (rg_dev, r->tb);
		SDL_zero (ci);
		ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		ci.size = (buf->size + 255) & ~255u;
		r->tb = SDL_CreateGPUTransferBuffer (rg_dev, &ci);
		r->cap = r->tb ? ci.size : 0;
		if (!r->tb)
		{
			RGF_Warn ("SDL_GPU: cannot create a %u-byte download buffer (%s)\n",
				  (unsigned int) ci.size, SDL_GetError ());
			rgr_refused++;
			return 0;
		}
	}
	/* What is recorded lands first, as RG_Frame_Readback's does: the
	 * segment's draws in their pass, or with none its uploads alone, the
	 * pending clear kept (a request just after a dispatch splits nothing). */
	if (rgf_numdraws)
		RGF_EndSegment ();
	else if (!RGF_CopyOnly ())
	{
		rgr_refused++;
		return 0;
	}
	if (!RGF_EnsureCmd ())
	{
		rgr_refused++;
		return 0;
	}
	copy = SDL_BeginGPUCopyPass (rgf_cmd);
	RG_Frame_CountCopyPass ();
	if (!copy)
	{
		rgr_refused++;
		return 0;
	}
	SDL_zero (d);
	d.transfer_buffer = r->tb;
	SDL_DownloadFromGPUBuffer (copy, buf, &d);
	SDL_EndGPUCopyPass (copy);
	if (!++rgr_ticket)
		rgr_ticket = 1;
	r->ticket = rgr_ticket;
	r->size = buf->size;
	r->fence = NULL;
	r->discard = false;
	r->state = RGR_RECORDED;
	rgr_requested++;
	rgf_frame_areads++;
	return r->ticket;
}

int RG_ReadbackCollect (unsigned int ticket, void *data, Uint32 size)
{
	rgreadback_t	*r = NULL;
	void		*map;
	int		i;

	for (i = 0; i < RG_READBACKS && !r; i++)
	{
		if (ticket && rgr_ring[i].state != RGR_FREE && rgr_ring[i].ticket == ticket)
			r = &rgr_ring[i];
	}
	if (!r)
		return -1;
	if (r->state == RGR_SUBMITTED && SDL_QueryGPUFence (rg_dev, r->fence))
		RGF_ReadbackFenceDone (r->fence, RGR_READY);
	if (r->state == RGR_RECORDED || r->state == RGR_SUBMITTED)
		return 0;
	if (r->state == RGR_FAILED)
	{
		r->state = RGR_FREE;
		return -1;
	}
	map = SDL_MapGPUTransferBuffer (rg_dev, r->tb, false);
	r->state = RGR_FREE;
	if (!map)
	{
		RGF_Warn ("SDL_GPU: cannot map the download buffer (%s)\n", SDL_GetError ());
		rgr_failed++;
		return -1;
	}
	memcpy (data, map, q_min (size, r->size));
	SDL_UnmapGPUTransferBuffer (rg_dev, r->tb);
	rgr_collected++;
	return 1;
}

void RG_ReadbackDiscard (unsigned int ticket)
{
	int	i;

	for (i = 0; i < RG_READBACKS; i++)
	{
		rgreadback_t	*r = &rgr_ring[i];

		if (!ticket || r->state == RGR_FREE || r->ticket != ticket)
			continue;
		/* one still in flight keeps its slot until its download is done:
		 * the next frame's start frees it (RGF_ReadbacksAge) */
		if (r->state == RGR_READY || r->state == RGR_FAILED)
		{
			r->state = RGR_FREE;
			rgr_discarded++;
		}
		else
			r->discard = true;
		return;
	}
}

static void RGF_ReadbacksShutdown (void)
{
	int	i;

	for (i = 0; i < RG_READBACKS; i++)
	{
		rgreadback_t	*r = &rgr_ring[i];

		/* the fence is released with the frame fences */
		if (r->tb)
			SDL_ReleaseGPUTransferBuffer (rg_dev, r->tb);
		memset (r, 0, sizeof(*r));
	}
}

/* What a frame's end releases and rewinds, once its command buffer is
 * submitted. */
static void RGF_EndOfFrame (void)
{
	/* Released now, SDL keeps each alive until the submitted work retires. */
	RG_Res_EndFrame ();
	RG_Buf_EndFrame ();
	while (rgf_numretired_bufs)
		SDL_ReleaseGPUBuffer (rg_dev, rgf_retired_bufs[--rgf_numretired_bufs]);
}

/* Wait for the kept fences until `keep` are left, newest last; each one
 * that signalled completes the reads it carried, and is released. */
static void RGF_FencesWait (int keep)
{
	int		i, n;
	qboolean	ok;

	if (rgf_numfences <= keep)
		return;
	n = rgf_numfences - keep;
	/* One queue: the newest of them signalling means the rest have too. */
	ok = SDL_WaitForGPUFences (rg_dev, true, rgf_fences, (Uint32) n);
	if (!ok)
		RGF_Warn ("SDL_GPU: waiting for a frame failed (%s)\n", SDL_GetError ());
	for (i = 0; i < n; i++)
	{
		/* a failed wait fails what they carried, as RGF_SubmitMidFrame's */
		RGF_ReadbackFenceDone (rgf_fences[i], ok ? RGR_READY : RGR_FAILED);
		SDL_ReleaseGPUFence (rg_dev, rgf_fences[i]);
	}
	memmove (rgf_fences, rgf_fences + n, (size_t) keep * sizeof(rgf_fences[0]));
	rgf_numfences = keep;
}

static void RGF_Submit (void)
{
	SDL_GPUFence	*fence;
	Uint64		t0 = SDL_GetTicksNS ();

	if (rgf_cmd)
	{
		fence = SDL_SubmitGPUCommandBufferAndAcquireFence (rgf_cmd);
		if (!fence)
			RGF_Warn ("SDL_GPU: submit failed (%s)\n", SDL_GetError ());
		RGF_ReadbacksSubmitted (fence, false);
		if (fence)
		{
			if (rgf_numfences == RGF_FENCES)
				RGF_FencesWait (RGF_FENCES - 1);
			rgf_fences[rgf_numfences++] = fence;
		}
	}
	rgf_cmd = NULL;
	RGF_EndOfFrame ();
	rgf_frame_submit_ns += SDL_GetTicksNS () - t0;
}

/* Submit what is recorded and wait for it to finish, the frame staying open
 * (#340 P5a-2): none of RGF_EndOfFrame's work, since RG_Buf_EndFrame would
 * rewind the storage stream that this frame's later draws still take buffers
 * from.  The next pass acquires a fresh command buffer (RGF_EnsureCmd). */
static qboolean RGF_SubmitMidFrame (void)
{
	SDL_GPUFence	*fence;
	Uint64		t0;
	qboolean	ok;

	if (!rgf_cmd)
		return false;
	rgf_frame_waits++;
	rgf_waits++;
	fence = SDL_SubmitGPUCommandBufferAndAcquireFence (rgf_cmd);
	rgf_cmd = NULL;
	if (!fence)
	{
		RGF_Warn ("SDL_GPU: submit failed (%s)\n", SDL_GetError ());
		RGF_ReadbacksSubmitted (NULL, false);
		return false;
	}
	t0 = SDL_GetTicksNS ();
	ok = SDL_WaitForGPUFences (rg_dev, true, &fence, 1);
	rgf_frame_mid_ns += SDL_GetTicksNS () - t0;
	if (!ok)
	{
		RGF_Warn ("SDL_GPU: waiting for submitted work failed (%s)\n", SDL_GetError ());
		SDL_ReleaseGPUFence (rg_dev, fence);
		RGF_ReadbacksSubmitted (NULL, false);
		return false;
	}
	SDL_ReleaseGPUFence (rg_dev, fence);
	/* the asynchronous reads this carried are done too */
	RGF_ReadbacksSubmitted (NULL, true);
	return true;
}

/* ------------------------------------------------------------------ */
/* Compute and readback (#340 P5a-2)                                   */
/* ------------------------------------------------------------------ */

/* The segment's copy pass with no render pass after it: the uploads a
 * dispatch reads land, and a pending clear stays pending, so a compute pass
 * between a clear and the first draw does not cost an empty render pass. */
static qboolean RGF_CopyOnly (void)
{
	qboolean	ok;

	if (!RGF_EnsureCmd ())
		return false;
	RG_Res_ClearTargets (rgf_cmd);
	ok = RGF_CopyPass ();
	RG_Res_ClearPlaceholders (rgf_cmd);
	/* The stream is the draws', and there are none; what it held went out
	 * in that pass, so it starts again. */
	rgf_frame_stream += rgf_streamlen;
	rgf_streamlen = 0;
	rgf_stream_indices = false;
	return ok;
}

/* Between RG_Frame_ComputeBegin and _End: a dispatch's bindings are being
 * made, with no render pass open (#340 P5d). */
static qboolean		rgf_compute_bind;

qboolean RG_Frame_ComputeBinding (void)
{
	return rgf_compute_bind;
}

SDL_GPUCommandBuffer *RG_Frame_ComputeBegin (qboolean touches_target, qboolean samples)
{
	rgf_comp_t0 = SDL_GetTicksNS ();
	rgf_comp_mid0 = rgf_frame_mid_ns;
	rgf_comp_seg0 = rgf_frame_seg_ns;
	if (rgf_numdraws || (rgf_clear_mask && touches_target))
		RGF_EndSegment ();
	else if (!RGF_CopyOnly ())
		return NULL;
	/* SDL 3.4.12 returns a texture to SAMPLER after a copy or a render pass
	 * with a barrier whose second scope is the vertex and fragment stages
	 * alone, and SDL_BindGPUComputeSamplers adds none: a compute read of
	 * what this command buffer wrote would race it.  A submit and its fence
	 * make the writes visible to the next submit.  Since P5f no pass of the
	 * default frame samples (the Hi-Z reduce and the cull read read-only
	 * storage textures, rg_compute.c); the palette LUT at a metric change
	 * and SSAO's five passes still do, and pay this. */
	if (samples && !RGF_SubmitMidFrame ())
		return NULL;
	if (!RGF_EnsureCmd ())
		return NULL;
	rgf_compute_bind = true;
	return rgf_cmd;
}

void RG_Frame_ComputeEnd (qboolean dispatched)
{
	rgf_compute_bind = false;
	/* the segment it ended and the wait it paid are counted apart */
	rgf_frame_comp_ns += SDL_GetTicksNS () - rgf_comp_t0 - (rgf_frame_mid_ns - rgf_comp_mid0) -
			     (rgf_frame_seg_ns - rgf_comp_seg0);
	if (dispatched)
	{
		rgf_frame_cpasses++;
		rgf_cpasses++;
	}
	if (!rgf_active)
		RGF_Submit ();
}

SDL_GPUBuffer *RG_Frame_Standin (void)
{
	return rgf_ssbo;
}

qboolean RG_Frame_Readback (const SDL_GPUTextureRegion *tex, const SDL_GPUBufferRegion *buf,
			    void *dst, Uint32 bytes)
{
	SDL_GPUTransferBufferCreateInfo	ci;
	SDL_GPUTransferBuffer		*tb;
	SDL_GPUCopyPass			*copy;
	qboolean			ok;
	void				*map;

	if (!bytes)
		return true;
	RGF_EndSegment ();
	if (!rgf_cmd)
		return false;
	SDL_zero (ci);
	ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	ci.size = bytes;
	tb = SDL_CreateGPUTransferBuffer (rg_dev, &ci);
	if (!tb)
	{
		RGF_Warn ("SDL_GPU: cannot create a %u-byte download buffer (%s)\n",
			  (unsigned int) bytes, SDL_GetError ());
		if (!rgf_active)
			RGF_Submit ();	/* what the segment's end recorded, as RG_Frame_Flush */
		return false;
	}
	copy = SDL_BeginGPUCopyPass (rgf_cmd);
	RG_Frame_CountCopyPass ();
	if (tex)
	{
		SDL_GPUTextureTransferInfo	d;

		SDL_zero (d);
		d.transfer_buffer = tb;
		d.pixels_per_row = tex->w;
		d.rows_per_layer = tex->h;
		SDL_DownloadFromGPUTexture (copy, tex, &d);
	}
	else
	{
		SDL_GPUTransferBufferLocation	d;

		SDL_zero (d);
		d.transfer_buffer = tb;
		SDL_DownloadFromGPUBuffer (copy, buf, &d);
	}
	SDL_EndGPUCopyPass (copy);
	ok = RGF_SubmitMidFrame ();
	if (ok)
	{
		map = SDL_MapGPUTransferBuffer (rg_dev, tb, false);
		ok = (map != NULL);
		if (ok)
		{
			memcpy (dst, map, bytes);
			SDL_UnmapGPUTransferBuffer (rg_dev, tb);
		}
		else
			RGF_Warn ("SDL_GPU: cannot map the download buffer (%s)\n", SDL_GetError ());
	}
	SDL_ReleaseGPUTransferBuffer (rg_dev, tb);
	/* Outside a frame the submit was the whole of one, as RG_Frame_Flush's. */
	if (!rgf_active)
		RGF_EndOfFrame ();
	if (ok)
	{
		rgf_frame_reads++;
		rgf_reads++;
	}
	return ok;
}

void RG_Frame_Flush (void)
{
	RGF_EndSegment ();
	if (!rgf_active)
		RGF_Submit ();		/* uploads alone, outside a frame */
}

void RG_Frame_End (qboolean show)
{
	SDL_GPUTexture	*swap = NULL;
	Uint32		sw, sh;

	if (!rgf_active)
		RG_BeginFrame (rgf_w, rgf_h);
	RGF_EndSegment ();
	if (!rgf_cmd)
	{
		rgf_active = false;
		return;
	}
	if (!rgf_cleared)
	{
		/* A framebuffer is bound and the scene had no pass this frame:
		 * give it its first, so the present does not show the last
		 * frame's. */
		GLuint		fbo = rgf_fbo;
		rgtarget_t	tgt = rgf_tgt;
		qboolean	ok = rgf_tgt_ok;

		rgf_fbo = 0;
		RG_Frame_SceneTarget (&rgf_tgt);
		rgf_tgt_ok = true;
		rgf_tgt_dirty = false;
		RGF_EndSegment ();
		rgf_fbo = fbo;
		rgf_tgt = tgt;
		rgf_tgt_ok = ok;
		rgf_tgt_dirty = true;
	}
	if (show && rg_headless)
		swap = RGF_HeadlessTarget (&sw, &sh);
	else if (show)
	{
		Uint64	t0 = SDL_GetTicksNS ();

		if (!SDL_WaitAndAcquireGPUSwapchainTexture (rgf_cmd, rg_win, &swap, &sw, &sh))
			Con_DPrintf ("SDL_GPU: no swapchain texture (%s)\n", SDL_GetError ());
		rgf_frame_acquire_ns = SDL_GetTicksNS () - t0;
	}
	/* Not shown, or NULL with success (a minimised or occluded window): the
	 * frame is still submitted, there is just nowhere to show it. */
	if (swap)
	{
		SDL_GPUBlitInfo	bi;

		SDL_zero (bi);
		bi.source.texture = rgf_color;
		bi.source.w = rgf_tw;
		bi.source.h = rgf_th;
		bi.destination.texture = swap;
		bi.destination.w = sw;
		bi.destination.h = sh;
		bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
		/* The one flip: render targets hold GL's bottom-up rows. */
		bi.flip_mode = SDL_FLIP_VERTICAL;
		bi.filter = ((int)sw == rgf_tw && (int)sh == rgf_th) ? SDL_GPU_FILTER_NEAREST
								      : SDL_GPU_FILTER_LINEAR;
		SDL_BlitGPUTexture (rgf_cmd, &bi);
	}
	RGF_Submit ();
	rgf_active = false;
	rgf_last_draws = rgf_frame_draws;
	rgf_last_segments = rgf_frame_segments;
	rgf_last_indexed = rgf_frame_indexed;
	rgf_last_stream = rgf_frame_stream;
	rgf_last_ssbo = rgf_frame_ssbo;
	rgf_last_standin = rgf_frame_standin;
	rgf_last_cpasses = rgf_frame_cpasses;
	rgf_last_reads = rgf_frame_reads;
	rgf_last_waits = rgf_frame_waits;
	rgf_last_indirect = rgf_frame_indirect;
	rgf_last_areads = rgf_frame_areads;
	rgf_last_depthonly = rgf_frame_depthonly;
	rgf_last_layered = rgf_frame_layered;
	rgf_last_mrt = rgf_frame_mrt;
	rgf_last_cdropped = RG_ComputeDropped () - rgf_cdropped_base;
	rgf_frames++;
	RGF_NoteTimes ();
}

/* -gpuheadless: a texture of the window's size the present blits into, so
 * the frame costs what a shown one does, minus the swapchain. */
static SDL_GPUTexture *RGF_HeadlessTarget (Uint32 *w, Uint32 *h)
{
	int	ww = 0, wh = 0;

	SDL_GetWindowSizeInPixels (rg_win, &ww, &wh);
	if (ww <= 0 || wh <= 0)
		ww = rgf_tw, wh = rgf_th;
	if (!rgf_headless_tex || rgf_headless_w != (Uint32) ww || rgf_headless_h != (Uint32) wh)
	{
		if (rgf_headless_tex)
			SDL_ReleaseGPUTexture (rg_dev, rgf_headless_tex);
		rgf_headless_tex = RGF_MakeTarget (SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
						   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, ww, wh);
		rgf_headless_w = rgf_headless_tex ? (Uint32) ww : 0;
		rgf_headless_h = rgf_headless_tex ? (Uint32) wh : 0;
	}
	*w = rgf_headless_w;
	*h = rgf_headless_h;
	return rgf_headless_tex;
}

/* The frame just submitted, into gpu_frametimes' ring. */
static void RGF_NoteTimes (void)
{
	Uint64		now = SDL_GetTicksNS ();
	rgftime_t	*t = &rgf_times[rgf_numtimes++ % RGF_TIMES];
	Uint64		spent = now - rgf_t_begin;
	Uint64		waits = rgf_frame_fence_ns + rgf_frame_mid_ns + rgf_frame_acquire_ns;

	t->interval_ns = rgf_t_lastbegin ? rgf_t_begin - rgf_t_lastbegin : 0;
	t->cpu_ns = spent > waits ? spent - waits : 0;
	t->fence_ns = rgf_frame_fence_ns;
	t->mid_ns = rgf_frame_mid_ns;
	t->acquire_ns = rgf_frame_acquire_ns;
	t->seg_ns = rgf_frame_seg_ns;
	t->comp_ns = rgf_frame_comp_ns;
	t->submit_ns = rgf_frame_submit_ns;
	t->render = rgf_last_segments;
	t->compute = rgf_last_cpasses;
	t->copy = rgf_frame_copies;
	t->waits = rgf_last_waits;
	t->draws = rgf_last_draws;
	t->indirect = rgf_last_indirect;
	t->stream = rgf_last_stream;
	rgf_t_lastbegin = rgf_t_begin;
}

unsigned long RG_DrawsDropped (void)
{
	return rgf_dropped;
}

void RG_Frame_CountCopyPass (void)
{
	rgf_frame_copies++;
}

/* gpu_frametimes [frames]: the last frames' times and passes (#340 P5f).
 * Dev-only, a command and no cvar.  cpu is the recording, with every wait
 * taken out; fence the wait at the frame's start for the frame before last
 * (the GPU setting the pace), mid the submits inside the frame that wait
 * (RGF_SubmitMidFrame), acquire the swapchain's. */
void RG_FrameTimes_f (void)
{
	int		n = Cmd_Argc () > 1 ? atoi (Cmd_Argv (1)) : RGF_TIMES;
	int		i, have = (int) q_min (rgf_numtimes, (unsigned long) RGF_TIMES);
	double		sum[8] = { 0 }, mx[8] = { 0 }, cnt[7] = { 0 };
	const char	*name[8] = { "frame", "cpu", "fence", "mid", "acquire", "segments", "compute", "submit" };

	if (!rg_dev)
	{
		Con_Printf ("gpu_frametimes: the SDL_GPU backend is not running (-gpu)\n");
		return;
	}
	n = CLAMP (1, n, have);
	if (have < 1)
	{
		Con_Printf ("gpu_frametimes: no frames yet\n");
		return;
	}
	for (i = 0; i < n; i++)
	{
		const rgftime_t	*t = &rgf_times[(rgf_numtimes - 1 - i) % RGF_TIMES];
		double		v[8];
		int		k;

		v[0] = t->interval_ns / 1e6;
		v[1] = t->cpu_ns / 1e6;
		v[2] = t->fence_ns / 1e6;
		v[3] = t->mid_ns / 1e6;
		v[4] = t->acquire_ns / 1e6;
		v[5] = t->seg_ns / 1e6;
		v[6] = t->comp_ns / 1e6;
		v[7] = t->submit_ns / 1e6;
		for (k = 0; k < 8; k++)
		{
			sum[k] += v[k];
			mx[k] = q_max (mx[k], v[k]);
		}
		cnt[0] += t->render;
		cnt[1] += t->compute;
		cnt[2] += t->copy;
		cnt[3] += t->waits;
		cnt[4] += t->draws;
		cnt[5] += t->indirect;
		cnt[6] += t->stream;
	}
	Con_Printf ("gpu_frametimes: last %d frames, ms mean/max:", n);
	for (i = 0; i < 5; i++)
		Con_Printf (" %s %.3f/%.3f", name[i], sum[i] / n, mx[i]);
	Con_Printf ("\n");
	Con_Printf ("gpu_frametimes: of cpu, ms mean/max:");
	for (i = 5; i < 8; i++)
		Con_Printf (" %s %.3f/%.3f", name[i], sum[i] / n, mx[i]);
	Con_Printf ("\n");
	Con_Printf ("gpu_frametimes: per frame: %.1f render passes, %.1f compute passes, %.1f copy "
		    "passes, %.1f fence waits, %.0f draws, %.1f indirect draws, %.0f stream bytes%s\n",
		    cnt[0] / n, cnt[1] / n, cnt[2] / n, cnt[3] / n, cnt[4] / n, cnt[5] / n, cnt[6] / n,
		    rg_headless ? " (headless)" : "");
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

/* The scene's depth target, stencil first (#340 P2c): GL's visual asks for
 * 24 depth bits and 8 of stencil (SDL_GL_STENCIL_SIZE), and SDL guarantees
 * one of the two packed formats.  The stencil-less ones behind them are for
 * a device that broke that promise: -gpu then runs with have_stencil false,
 * as GL does on a visual with no stencil bits (RG_DepthHasStencil). */
static const SDL_GPUTextureFormat rgf_depth_formats[] = {
	SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
	SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
	SDL_GPU_TEXTUREFORMAT_D24_UNORM,
	SDL_GPU_TEXTUREFORMAT_D32_FLOAT
};

qboolean RG_FormatHasStencil (SDL_GPUTextureFormat fmt)
{
	return fmt == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT ||
	       fmt == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

qboolean RG_DepthHasStencil (void)
{
	return rg_dev != NULL && RG_FormatHasStencil (rgf_depth_fmt);
}

qboolean RG_DepthSampled (void)
{
	return rg_dev != NULL && rgf_depth_sampled;
}

static const char *RGF_DepthName (SDL_GPUTextureFormat fmt)
{
	switch (fmt)
	{
	case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:	return "D24S8";
	case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:	return "D32S8";
	case SDL_GPU_TEXTUREFORMAT_D24_UNORM:		return "D24";
	case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:		return "D32F";
	case SDL_GPU_TEXTUREFORMAT_D16_UNORM:		return "D16";
	default:					return "?";
	}
}

void RG_Frame_Init (void)
{
	/* The scene's format is GL_DEPTH24_STENCIL8's too (rg_resource.c), and
	 * the post-process chain samples that: the list in order with SAMPLER
	 * usage first, then without (#340 P4a-2). */
	static const SDL_GPUTextureUsageFlags usage[2] = {
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
	};
	const int	nfmt = (int)(sizeof(rgf_depth_formats) / sizeof(rgf_depth_formats[0]));
	int		i = nfmt, p;

	rgf_depth_fmt = SDL_GPU_TEXTUREFORMAT_D16_UNORM;	/* universally supported */
	rgf_depth_sampled = false;
	for (p = 0; p < 2 && i == nfmt; p++)
	{
		for (i = 0; i < nfmt; i++)
		{
			if (SDL_GPUTextureSupportsFormat (rg_dev, rgf_depth_formats[i], SDL_GPU_TEXTURETYPE_2D,
							  usage[p]))
			{
				rgf_depth_fmt = rgf_depth_formats[i];
				rgf_depth_sampled = (p == 0);
				break;
			}
		}
	}
	if (!RG_FormatHasStencil (rgf_depth_fmt))
		Con_SafePrintf ("SDL_GPU: no depth format with stencil; skyboxes are drawn unmasked\n");
	SDL_GetWindowSizeInPixels (rg_win, &rgf_w, &rgf_h);
	rgf_fbo = 0;
	rgf_tgt_ok = false;
	rgf_tgt_dirty = true;
	rgf_active = false;
	rgf_vp_set = false;
	rgf_scissor_on = false;
	rgf_clear_mask = 0;
	rgf_quad_uploaded = false;
	{
		SDL_GPUBufferCreateInfo	ci;

		SDL_zero (ci);
		/* compute's read-only slots too (#340 P5a-2) */
		ci.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
		ci.size = RG_SSBO_STANDIN_BYTES;
		rgf_ssbo = SDL_CreateGPUBuffer (rg_dev, &ci);
		if (!rgf_ssbo)
			Sys_Error ("SDL_GPU: cannot create the storage-buffer stand-in (%s)", SDL_GetError ());
		rgf_ssbo_uploaded = false;
	}
}

void RG_Frame_Shutdown (void)
{
	if (rgf_cmd)
	{
		SDL_CancelGPUCommandBuffer (rgf_cmd);
		rgf_cmd = NULL;
	}
	rgf_active = false;
	if (!rg_dev)
		return;
	SDL_WaitForGPUIdle (rg_dev);
	RGF_FencesWait (0);
	RGF_ReadbacksShutdown ();
	if (rgf_headless_tex)
		SDL_ReleaseGPUTexture (rg_dev, rgf_headless_tex);
	rgf_headless_tex = NULL;
	rgf_headless_w = rgf_headless_h = 0;
	rgf_t_lastbegin = 0;
	while (rgf_numretired_bufs)
		SDL_ReleaseGPUBuffer (rg_dev, rgf_retired_bufs[--rgf_numretired_bufs]);
	if (rgf_vbuf)	SDL_ReleaseGPUBuffer (rg_dev, rgf_vbuf);
	if (rgf_ibuf)	SDL_ReleaseGPUBuffer (rg_dev, rgf_ibuf);
	rgf_ibuf = NULL;
	rgf_ibuf_size = 0;
	if (rgf_quadbuf) SDL_ReleaseGPUBuffer (rg_dev, rgf_quadbuf);
	if (rgf_ssbo)	SDL_ReleaseGPUBuffer (rg_dev, rgf_ssbo);
	rgf_ssbo = NULL;
	if (rgf_tbuf)	SDL_ReleaseGPUTransferBuffer (rg_dev, rgf_tbuf);
	if (rgf_color)	SDL_ReleaseGPUTexture (rg_dev, rgf_color);
	if (rgf_depth)	SDL_ReleaseGPUTexture (rg_dev, rgf_depth);
	rgf_vbuf = rgf_quadbuf = NULL;
	rgf_tbuf = NULL;
	rgf_color = rgf_depth = NULL;
	rgf_vbuf_size = rgf_tbuf_size = 0;
	rgf_tw = rgf_th = 0;
	free (rgf_draws); rgf_draws = NULL;
	free (rgf_stream); rgf_stream = NULL;
	free (rgf_uarena); rgf_uarena = NULL;
	rgf_numdraws = rgf_maxdraws = 0;
	rgf_streamlen = rgf_streammax = rgf_ulen = rgf_umax = 0;
}

void RG_Frame_Status (void)
{
	Con_Printf ("[RENDERER] SDL_GPU frame: scene %dx%d, last frame %d draws (%d from buffers) in "
		    "%d segments, %u stream bytes, %d programs, %d pipelines, %d vertex layouts, "
		    "%lu draws dropped, %lu frames\n",
		    rgf_tw, rgf_th, rgf_last_draws, rgf_last_indexed, rgf_last_segments,
		    (unsigned int) rgf_last_stream, RG_ProgramCount (), RG_PipelineCount (),
		    RG_LayoutCount (), rgf_dropped, rgf_frames);
	Con_Printf ("[RENDERER] SDL_GPU storage slots last frame: %d bound to a buffer, %d to the "
		    "stand-in\n", rgf_last_ssbo, rgf_last_standin);
	Con_Printf ("[RENDERER] SDL_GPU compute last frame: %d passes, %d readbacks, %d fence waits; "
		    "since boot %lu passes, %lu readbacks, %lu fence waits\n",
		    rgf_last_cpasses, rgf_last_reads, rgf_last_waits, rgf_cpasses, rgf_reads, rgf_waits);
	Con_Printf ("[RENDERER] SDL_GPU last frame: %d indirect draws, %lu dispatches dropped, %d "
		    "asynchronous readbacks asked; asynchronous readbacks since boot %lu asked, %lu "
		    "collected, %lu refused, %lu failed, %lu discarded, %lu never collected\n",
		    rgf_last_indirect, rgf_last_cdropped, rgf_last_areads, rgr_requested, rgr_collected,
		    rgr_refused, rgr_failed, rgr_discarded, rgr_abandoned);
	Con_Printf ("[RENDERER] SDL_GPU last frame: %d depth-only passes, %d of them on a layer of a "
		    "layered depth texture (the shadow maps' faces, 6 per map drawn), %d with more than one "
		    "colour target (OIT)\n",
		    rgf_last_depthonly, rgf_last_layered, rgf_last_mrt);
	RG_Compute_Status ();
	Con_Printf ("[RENDERER] SDL_GPU scene depth %s (%s); drawing into framebuffer %u (%dx%d%s)\n",
		    RGF_DepthName (rgf_depth_fmt), rgf_depth_sampled ? "sampled too" : "not sampled",
		    rgf_fbo, rgf_tgt.w, rgf_tgt.h, rgf_tgt_ok ? "" : ", incomplete");
}
