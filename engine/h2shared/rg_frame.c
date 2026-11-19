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
 *   2. one render pass on the scene target, replaying the draws in order.
 *
 * A segment ends at the present, and early whenever something recorded next
 * cannot share its passes -- today an upload into a texture one of its draws
 * reads (RG_Frame_Flush).  P4's render targets and P5's compute will be more
 * segment breaks of the same kind.
 *
 * Every frame draws into one SCENE target, not the swapchain: the Vulkan
 * stages flip y so render targets hold GL's rows in GL's order (VK_FLIP_Y,
 * layout.inc), and the present is the one place that flips back, blitting
 * the scene into the swapchain upside down (SDL_FLIP_VERTICAL).  The scene
 * is cleared to gpu_clearcolor at the start of each frame; the world (P2)
 * is not drawn yet, so that colour is what a map's 3D view shows for now.
 *
 * GL_ImmEnd's batches are the only draws in P1c.  SDL_GPU has no quads and
 * no fans, so quads use a static index buffer (0,1,2, 0,2,3 per quad, as
 * GL_VBO_Init's) and polygons/fans and line loops get indices in the stream.
 *
 * Every slot a draw's program declares is bound (#340 P2a), because SDL_GPU
 * faults a draw that leaves one empty: each sampler its texture or a typed
 * placeholder (rg_resource.c), each read-only storage buffer the zeroed
 * stand-in below until P5 routes the real ones.
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

#define RG_MAX_BLOCKS	16
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

typedef struct {
	SDL_GPUGraphicsPipeline		*pipe;
	Uint32				vb_ofs;		/* stream arena */
	Uint32				ib_ofs;		/* stream arena, or 0 into the quad buffer */
	Uint8				indexed;	/* 0 no, 1 stream indices, 2 quad buffer */
	Uint32				count;		/* vertices, or indices */
	int				nvs, nfs;
	SDL_GPUTextureSamplerBinding	vs[RG_MAX_STAGE_SAMPLERS], fs[RG_MAX_STAGE_SAMPLERS];
	Uint8				nvssbo, nfssbo;	/* storage-buffer slots, all the stand-in */
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
static int			rgf_tw, rgf_th;

/* Recording */
static rgdraw_t			*rgf_draws;
static int			rgf_numdraws, rgf_maxdraws;
static Uint8			*rgf_stream;	/* vertex + index bytes */
static Uint32			rgf_streamlen, rgf_streammax;
static Uint8			*rgf_uarena;	/* uniform snapshots */
static Uint32			rgf_ulen, rgf_umax;

/* GPU-side */
static SDL_GPUBuffer		*rgf_vbuf;	/* the stream, vertex + index usage */
static Uint32			rgf_vbuf_size;
static SDL_GPUBuffer		*rgf_quadbuf;	/* static quad indices */
static qboolean			rgf_quad_uploaded;
/* RG_SSBO_STANDIN_BYTES of zeros, every read-only storage-buffer slot's */
static SDL_GPUBuffer		*rgf_ssbo;
static SDL_GPUBuffer		*rgf_ssbo_slots[RG_MAX_STAGE_SSBOS];
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

#define RG_QUAD_INDICES	((GL_IMM_MAX_VERTS / 4) * 6)

/* Last frame's counts, for renderer_status */
static int			rgf_last_draws, rgf_last_segments, rgf_frame_draws, rgf_frame_segments;
static unsigned long		rgf_frames;
static unsigned long		rgf_dropped;

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

void RG_SetViewport (int x, int y, int width, int height)
{
	rgf_vp_set = true;
	rgf_vp[0] = x;
	rgf_vp[1] = y;
	rgf_vp[2] = width;
	rgf_vp[3] = height;
}

void RG_SetScissor (qboolean enable, int x, int y, int width, int height)
{
	rgf_scissor_on = enable;
	rgf_scissor[0] = x;
	rgf_scissor[1] = y;
	rgf_scissor[2] = width;
	rgf_scissor[3] = height;
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
	RGF_EnsureScene (width, height);
	rgf_active = true;
	rgf_cleared = false;
	rgf_frame_draws = rgf_frame_segments = 0;
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

	if (st->num_ssbos > 0)
		RG_NoteUnhandled ("read-only storage buffer read as the zeroed stand-in (routing is P5)");
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

void RG_DrawImm (GLenum mode, const void *verts, int count, int stride)
{
	const rpipestate_t	*ps = R_PipelineState ();
	const rgprog_t		*prog = RG_Program (ps->program);
	SDL_GPUPrimitiveType	prim;
	rgdraw_t		*d;
	Uint32			nidx = 0;
	Uint8			indexed = 0;
	int			i, sc_x0, sc_y0, sc_x1, sc_y1;

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
		indexed = 2;
		nidx = (Uint32)(count / 4) * 6;
		break;
	case GL_POLYGON:
	case GL_TRIANGLE_FAN:
		prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		indexed = 1;
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
		indexed = 1;
		nidx = (count >= 2) ? (Uint32)count + 1 : 0;
		break;
	case GL_POINTS:
		prim = SDL_GPU_PRIMITIVETYPE_POINTLIST;
		break;
	default:
		RG_NoteUnhandled ("immediate draw in a GL primitive mode with no SDL_GPU equivalent");
		return;
	}
	if (count <= 0 || (indexed && !nidx))
		return;
	if (!rgf_vp_set)
		RG_SetViewport (0, 0, rgf_w, rgf_h);
	if (rgf_vp[2] <= 0 || rgf_vp[3] <= 0)
		return;

	/* GL clamps a scissor box to the target; SDL_GPU requires it.  An
	 * empty one passes nothing: drop the draw before recording any of it. */
	sc_x0 = 0; sc_y0 = 0; sc_x1 = rgf_tw; sc_y1 = rgf_th;
	if (rgf_scissor_on)
	{
		sc_x0 = q_max (rgf_scissor[0], 0);
		sc_y0 = q_max (rgf_scissor[1], 0);
		sc_x1 = q_min (rgf_scissor[0] + rgf_scissor[2], rgf_tw);
		sc_y1 = q_min (rgf_scissor[1] + rgf_scissor[3], rgf_th);
		if (sc_x1 <= sc_x0 || sc_y1 <= sc_y0)
			return;
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
	d->pipe = RG_PipelineFor (prog, prim, RG_VL_IMM, rgf_color_fmt, rgf_depth_fmt);
	if (!d->pipe)
	{
		rgf_dropped++;
		return;
	}

	/* Bindings and uniforms first: a texture realized here may queue
	 * nothing, but a hazard flush must not split this draw's bytes. */
	if (!RGF_RecordStage (d, prog, RPROG_STAGE_VERTEX) ||
	    !RGF_RecordStage (d, prog, RPROG_STAGE_FRAGMENT))
	{
		rgf_dropped++;
		return;
	}

	d->vb_ofs = RGF_StreamAlloc ((Uint32)count * stride);
	memcpy (rgf_stream + d->vb_ofs, verts, (size_t)count * stride);
	d->indexed = indexed;
	d->count = indexed ? nidx : (Uint32)count;
	if (indexed == 1)
	{
		Uint16	*ix;

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

	d->vp.x = (float) rgf_vp[0];
	d->vp.y = (float) rgf_vp[1];
	d->vp.w = (float) rgf_vp[2];
	d->vp.h = (float) rgf_vp[3];
	d->vp.min_depth = (float) ps->depth_near;
	d->vp.max_depth = (float) ps->depth_far;
	d->scissor.x = sc_x0;
	d->scissor.y = sc_y0;
	d->scissor.w = sc_x1 - sc_x0;
	d->scissor.h = sc_y1 - sc_y0;
	d->stencil_ref = (Uint8) ps->stencil_ref;
	rgf_numdraws++;
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
	Uint32			total = stream + quad + ssbo + tex;
	Uint8			*map;
	SDL_GPUCopyPass		*copy;
	SDL_GPUTransferBufferLocation src;
	SDL_GPUBufferRegion	dst;

	if (!total)
		return true;
	if (!RGF_EnsureTransfer (total))
		return false;
	if (stream && !RGF_EnsureBuffer (&rgf_vbuf, &rgf_vbuf_size, stream,
					 SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX))
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
	SDL_UnmapGPUTransferBuffer (rg_dev, rgf_tbuf);

	copy = SDL_BeginGPUCopyPass (rgf_cmd);
	if (stream)
	{
		src.transfer_buffer = rgf_tbuf;
		src.offset = 0;
		dst.buffer = rgf_vbuf;
		dst.offset = 0;
		dst.size = stream;
		/* cycle: an earlier segment's draws still read the old contents */
		SDL_UploadToGPUBuffer (copy, &src, &dst, true);
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
		SDL_GPUBufferBinding	vb;

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
			SDL_BindGPUVertexStorageBuffers (pass, 0, rgf_ssbo_slots, d->nvssbo);
		if (d->nfssbo)
			SDL_BindGPUFragmentStorageBuffers (pass, 0, rgf_ssbo_slots, d->nfssbo);

		vb.buffer = rgf_vbuf;
		vb.offset = d->vb_ofs;
		SDL_BindGPUVertexBuffers (pass, 0, &vb, 1);
		if (d->indexed)
		{
			SDL_GPUBufferBinding	ib;

			ib.buffer = (d->indexed == 2) ? rgf_quadbuf : rgf_vbuf;
			ib.offset = (d->indexed == 2) ? 0 : d->ib_ofs;
			SDL_BindGPUIndexBuffer (pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_DrawGPUIndexedPrimitives (pass, d->count, 1, 0, 0, 0);
		}
		else
			SDL_DrawGPUPrimitives (pass, d->count, 1, 0, 0);
	}
}

/* End the open segment: copy pass, then (in a frame) its render pass. */
/* The open segment's recording is spent: every arena offset in it dies, so
 * the serial moves on too (RGF_BlockOffset's cache is keyed on it). */
static void RGF_ResetSegment (void)
{
	rgf_numdraws = 0;
	rgf_streamlen = 0;
	rgf_ulen = 0;
	rgf_segment++;
}

static void RGF_EndSegment (void)
{
	if (!RGF_EnsureCmd ())
	{
		RGF_ResetSegment ();
		return;
	}
	if (!RGF_CopyPass ())
		rgf_numdraws = 0;	/* nothing to read; drawing would fault */
	RG_Res_ClearPlaceholders (rgf_cmd);

	if (rgf_active && (rgf_numdraws || !rgf_cleared))
	{
		SDL_GPUColorTargetInfo		ct;
		SDL_GPUDepthStencilTargetInfo	ds;
		SDL_GPURenderPass		*pass;
		const float			*cc = RG_ClearColor ();

		SDL_zero (ct);
		ct.texture = rgf_color;
		ct.load_op = rgf_cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
		ct.store_op = SDL_GPU_STOREOP_STORE;
		ct.clear_color.r = cc[0];
		ct.clear_color.g = cc[1];
		ct.clear_color.b = cc[2];
		ct.clear_color.a = 1.0f;
		SDL_zero (ds);
		ds.texture = rgf_depth;
		ds.load_op = rgf_cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
		ds.store_op = SDL_GPU_STOREOP_STORE;
		ds.stencil_load_op = ds.load_op;
		ds.stencil_store_op = SDL_GPU_STOREOP_STORE;
		ds.clear_depth = 1.0f;
		ds.clear_stencil = 0;
		pass = SDL_BeginGPURenderPass (rgf_cmd, &ct, 1, &ds);
		if (pass)
		{
			RGF_Replay (pass);
			SDL_EndGPURenderPass (pass);
		}
		rgf_cleared = true;
		rgf_frame_draws += rgf_numdraws;
		rgf_frame_segments++;
	}
	RGF_ResetSegment ();
}

static void RGF_Submit (void)
{
	if (rgf_cmd && !SDL_SubmitGPUCommandBuffer (rgf_cmd))
		RGF_Warn ("SDL_GPU: submit failed (%s)\n", SDL_GetError ());
	rgf_cmd = NULL;
	/* Released now, SDL keeps each alive until the submitted work retires. */
	RG_Res_EndFrame ();
	while (rgf_numretired_bufs)
		SDL_ReleaseGPUBuffer (rg_dev, rgf_retired_bufs[--rgf_numretired_bufs]);
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
	if (show && !SDL_WaitAndAcquireGPUSwapchainTexture (rgf_cmd, rg_win, &swap, &sw, &sh))
		Con_DPrintf ("SDL_GPU: no swapchain texture (%s)\n", SDL_GetError ());
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
	rgf_frames++;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void RG_Frame_Init (void)
{
	rgf_depth_fmt = SDL_GPUTextureSupportsFormat (rg_dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
						      SDL_GPU_TEXTURETYPE_2D,
						      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
			? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
	SDL_GetWindowSizeInPixels (rg_win, &rgf_w, &rgf_h);
	rgf_active = false;
	rgf_vp_set = false;
	rgf_scissor_on = false;
	rgf_quad_uploaded = false;
	{
		SDL_GPUBufferCreateInfo	ci;
		int			i;

		SDL_zero (ci);
		ci.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
		ci.size = RG_SSBO_STANDIN_BYTES;
		rgf_ssbo = SDL_CreateGPUBuffer (rg_dev, &ci);
		if (!rgf_ssbo)
			Sys_Error ("SDL_GPU: cannot create the storage-buffer stand-in (%s)", SDL_GetError ());
		for (i = 0; i < RG_MAX_STAGE_SSBOS; i++)
			rgf_ssbo_slots[i] = rgf_ssbo;
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
	while (rgf_numretired_bufs)
		SDL_ReleaseGPUBuffer (rg_dev, rgf_retired_bufs[--rgf_numretired_bufs]);
	if (rgf_vbuf)	SDL_ReleaseGPUBuffer (rg_dev, rgf_vbuf);
	if (rgf_quadbuf) SDL_ReleaseGPUBuffer (rg_dev, rgf_quadbuf);
	if (rgf_ssbo)	SDL_ReleaseGPUBuffer (rg_dev, rgf_ssbo);
	rgf_ssbo = NULL;
	memset (rgf_ssbo_slots, 0, sizeof(rgf_ssbo_slots));
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
	Con_Printf ("[RENDERER] SDL_GPU frame: scene %dx%d, last frame %d draws in %d segments, "
		    "%d programs, %d pipelines, %lu draws dropped, %lu frames\n",
		    rgf_tw, rgf_th, rgf_last_draws, rgf_last_segments, RG_ProgramCount (),
		    RG_PipelineCount (), rgf_dropped, rgf_frames);
}
