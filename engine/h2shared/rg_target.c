/* rg_target.c -- SDL_GPU backend: framebuffers and renderbuffers (#340 P4a-2)
 *
 * gl_fbo.h's SDL_GPU arms.  Names stay GL-shaped (decision 1 on #340): a
 * framebuffer is a record of what is attached to it, by GL texture name, and
 * the textures themselves are rg_resource.c's.  A renderbuffer is a hidden
 * rg_resource.c texture, named from the texture namespace, with target usage
 * and no SAMPLER: storage for an attachment nothing reads.
 *
 * GL binds a read and a draw framebuffer (GL_FRAMEBUFFER both).  The draw
 * one is where rg_frame.c's draws go: binding it moves the frame recorder to
 * that target (RG_Frame_BindTarget), which ends the open segment only if it
 * has something to put in the old one.  Framebuffer 0 is the scene target,
 * the one the present blits to the swapchain.
 *
 * glCheckFramebufferStatus is honest: COMPLETE only for what a render pass
 * can be begun on -- at most one colour attachment (rg_pipeline.c builds one
 * colour target; two would be OIT's MRT, P6c), a defined 2D texture with the
 * target usage for each attachment, or for depth one layer of a layered one
 * (the shadow maps' cube-map array, #340 P6b), equal sizes.  Anything else is
 * incomplete, and the post-process chain turns itself off on that rather
 * than drawing into a target that is not there.
 *
 * glBlitFramebuffer ends the segment and records outside any pass: colour by
 * SDL_BlitGPUTexture with GL's rectangles as they are -- render targets and
 * uploads both hold GL's row order (VK_FLIP_Y, layout.inc), so there is no
 * flip to add -- and depth by SDL_CopyGPUTextureToTexture, which copies
 * whole texels: only a full-size 1:1 copy between identical formats, which
 * is why GL_DEPTH24_STENCIL8 is the scene's own depth format here
 * (rg_resource.c RGT_Format).  A blit this cannot make returns false
 * (counted), and the soft-particle snapshot is valid only if it was made
 * (gl_postprocess.c GL_SoftDepth_Capture, P4b).  A blit is
 * checked before it ends the segment, so a declined one costs nothing.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_target.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"

#ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT		0x8CD6
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT	0x8CD7
#endif
#ifndef GL_FRAMEBUFFER_UNSUPPORTED
#define GL_FRAMEBUFFER_UNSUPPORTED			0x8CDD
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING			0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING			0x8CAA
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT				0x8D20
#endif
#ifndef GL_COLOR_ATTACHMENT15
#define GL_COLOR_ATTACHMENT15				0x8CEF
#endif

#define RGTG_EXTRA	15	/* colour attachments past the first */

typedef struct {
	qboolean	alive;
	GLuint		color;		/* GL texture names; a renderbuffer's is its hidden one */
	GLuint		depth;
	GLuint		stencil;	/* GL_STENCIL_ATTACHMENT alone: complete only as depth's */
	GLuint		extra[RGTG_EXTRA];	/* GL_COLOR_ATTACHMENT1 .. 15 */
	/* The layer glFramebufferTextureLayer attached depth at; -1 when it
	 * was attached whole, as a 2D texture or a renderbuffer (#340 P6b). */
	int		dlayer;
	/* RGTG_BAD_*: what no render pass can take (a level, a cube face, a
	 * renderbuffer never made, MRT), by attachment point, so that what
	 * replaces it there, or deleting it, takes it back as GL's does. */
	unsigned int	bad;
	qboolean	draw_none;	/* glDrawBuffers (GL_NONE): no colour drawn */
} rgfbo_t;

#define RGTG_BAD_COLOR(n)	(1u << (n))	/* GL_COLOR_ATTACHMENT0 + n */
#define RGTG_BAD_DEPTH		(1u << 16)
#define RGTG_BAD_STENCIL	(1u << 17)
#define RGTG_BAD_DRAWBUFS	(1u << 18)	/* MRT (P6) */

typedef struct {
	qboolean	alive;
	GLuint		tex;		/* the hidden texture */
} rgrb_t;

static rgfbo_t		*rgfb;
static int		rgfb_count, rgfb_used;	/* table size; highest name handed out */
static GLuint		*rgfb_free;
static int		rgfb_numfree, rgfb_maxfree;
static GLuint		rgfb_draw, rgfb_read;	/* bindings, 0 the scene */

static rgrb_t		*rgrb;
static int		rgrb_count, rgrb_used;
static GLuint		*rgrb_free;
static int		rgrb_numfree, rgrb_maxfree;
static GLuint		rgrb_bound;

static unsigned long	rgtg_blits, rgtg_copies, rgtg_declined, rgtg_incomplete;

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* Hand out n names from a table of `size`-byte records, reusing deleted
 * ones as GL does; each record zeroed and marked alive by the caller. */
static GLuint RGTG_Name (void **table, int *count, int *used, GLuint *freelist, int *numfree,
			 size_t size, const char *what)
{
	if (*numfree > 0)
		return freelist[--*numfree];
	if (*used + 1 >= *count)
	{
		int	n = *count ? *count * 2 : 64;

		*table = realloc (*table, n * size);
		if (!*table)
			Sys_Error ("SDL_GPU: out of memory for %d %s names", n, what);
		memset ((Uint8 *) *table + *count * size, 0, (n - *count) * size);
		*count = n;
	}
	return (GLuint) ++*used;
}

static void RGTG_FreeName (GLuint **freelist, int *numfree, int *maxfree, GLuint name, const char *what)
{
	if (*numfree == *maxfree)
	{
		*maxfree = *maxfree ? *maxfree * 2 : 64;
		*freelist = (GLuint *) realloc (*freelist, *maxfree * sizeof(GLuint));
		if (!*freelist)
			Sys_Error ("SDL_GPU: out of memory freeing %s names", what);
	}
	(*freelist)[(*numfree)++] = name;
}

static rgfbo_t *RGTG_Fbo (GLuint name)
{
	if (name == 0 || name > (GLuint) rgfb_used || !rgfb[name].alive)
		return NULL;
	return &rgfb[name];
}

static rgrb_t *RGTG_Rb (GLuint name)
{
	if (name == 0 || name > (GLuint) rgrb_used || !rgrb[name].alive)
		return NULL;
	return &rgrb[name];
}

/* The framebuffer bound to `target`, as a name; false for a target GL does
 * not have. */
static qboolean RGTG_Binding (GLenum target, GLuint *fbo)
{
	switch (target)
	{
	case GL_FRAMEBUFFER:
	case GL_DRAW_FRAMEBUFFER:	*fbo = rgfb_draw; return true;
	case GL_READ_FRAMEBUFFER:	*fbo = rgfb_read; return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Framebuffers                                                        */
/* ------------------------------------------------------------------ */

void RG_GenFramebuffers (GLsizei n, GLuint *fbos)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		GLuint	name = RGTG_Name ((void **) &rgfb, &rgfb_count, &rgfb_used, rgfb_free,
					  &rgfb_numfree, sizeof(rgfbo_t), "framebuffer");

		memset (&rgfb[name], 0, sizeof(rgfbo_t));
		rgfb[name].alive = true;
		rgfb[name].dlayer = -1;
		fbos[i] = name;
	}
}

void RG_DeleteFramebuffers (GLsizei n, const GLuint *fbos)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		rgfbo_t	*f = RGTG_Fbo (fbos[i]);

		if (!f)
			continue;
		/* GL: deleting a bound framebuffer binds 0 in its place. */
		if (rgfb_draw == fbos[i])
		{
			rgfb_draw = 0;
			RG_Frame_BindTarget (0);
		}
		if (rgfb_read == fbos[i])
			rgfb_read = 0;
		f->alive = false;
		RGTG_FreeName (&rgfb_free, &rgfb_numfree, &rgfb_maxfree, fbos[i], "framebuffer");
	}
}

void RG_BindFramebuffer (GLenum target, GLuint fbo)
{
	if (fbo && !RGTG_Fbo (fbo))
	{
		RG_NoteUnhandled ("framebuffer bind of a name never generated (ignored)");
		return;
	}
	switch (target)
	{
	case GL_FRAMEBUFFER:
		rgfb_read = fbo;
		/* fall through */
	case GL_DRAW_FRAMEBUFFER:
		rgfb_draw = fbo;
		RG_Frame_BindTarget (fbo);
		return;
	case GL_READ_FRAMEBUFFER:
		rgfb_read = fbo;
		return;
	}
	RG_NoteUnhandled ("framebuffer bind to a target GL does not have (ignored)");
}

/* The framebuffer an attach call names, or NULL (counted): GL makes
 * attaching to framebuffer 0 an error. */
static rgfbo_t *RGTG_AttachTo (GLenum target)
{
	GLuint	fbo;
	rgfbo_t	*f;

	if (!RGTG_Binding (target, &fbo) || !(f = RGTG_Fbo (fbo)))
	{
		RG_NoteUnhandled ("framebuffer attachment with no framebuffer bound (ignored)");
		return NULL;
	}
	return f;
}

/* `f` changed: if it is the bound draw framebuffer, the frame's target is
 * whatever it resolves to now. */
static void RGTG_Changed (const rgfbo_t *f)
{
	if (f == RGTG_Fbo (rgfb_draw))
		RG_Frame_BindTarget (rgfb_draw);
}

/* The RGTG_BAD_* bits of an attachment point, 0 for one GL does not have. */
static unsigned int RGTG_Point (GLenum attachment)
{
	switch (attachment)
	{
	case GL_DEPTH_ATTACHMENT:		return RGTG_BAD_DEPTH;
	case GL_STENCIL_ATTACHMENT:		return RGTG_BAD_STENCIL;
	case GL_DEPTH_STENCIL_ATTACHMENT:	return RGTG_BAD_DEPTH | RGTG_BAD_STENCIL;
	}
	if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT15)
		return RGTG_BAD_COLOR (attachment - GL_COLOR_ATTACHMENT0);
	return 0;
}

/* Attach texture name `tex` (0 detaches) at `attachment` of `f`, replacing
 * whatever was there; `bad`: as something no render pass can take.  `layer`
 * the layer of a layered texture, -1 for a whole one; only a depth point
 * keeps it (a colour layer is bad, RG_FramebufferTextureLayer). */
static void RGTG_Attach (rgfbo_t *f, GLenum attachment, GLuint tex, qboolean bad, int layer)
{
	unsigned int	point = RGTG_Point (attachment);

	if (!point)
	{
		/* GL_INVALID_ENUM: nothing changes. */
		RG_NoteUnhandled ("framebuffer attachment point GL does not have (ignored)");
		return;
	}
	f->bad = bad ? (f->bad | point) : (f->bad & ~point);
	switch (attachment)
	{
	case GL_COLOR_ATTACHMENT0:
		f->color = tex;
		break;
	case GL_DEPTH_ATTACHMENT:
		f->depth = tex;
		f->dlayer = tex ? layer : -1;
		break;
	case GL_STENCIL_ATTACHMENT:
		f->stencil = tex;
		break;
	case GL_DEPTH_STENCIL_ATTACHMENT:
		f->depth = f->stencil = tex;
		f->dlayer = tex ? layer : -1;
		break;
	default:
		f->extra[attachment - GL_COLOR_ATTACHMENT0 - 1] = tex;
		break;
	}
	/* What the bound draw target holds may just have changed, a bad
	 * attachment included. */
	RGTG_Changed (f);
}

void RG_FramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget,
			      GLuint texture, GLint level)
{
	rgfbo_t		*f = RGTG_AttachTo (target);
	qboolean	bad = false;

	if (!f)
		return;
	if (texture && (textarget != GL_TEXTURE_2D || level != 0))
	{
		/* Only level 0 of a 2D texture is ever attached here. */
		RG_NoteUnhandled ("framebuffer attachment of a cube face or a mip level (incomplete)");
		bad = true;
	}
	RGTG_Attach (f, attachment, texture, bad, -1);
}

void RG_FramebufferTextureLayer (GLenum target, GLenum attachment, GLuint texture,
				 GLint level, GLint layer)
{
	rgfbo_t		*f = RGTG_AttachTo (target);
	qboolean	bad = false;

	if (!f)
		return;
	if (texture && (level != 0 || layer < 0))
	{
		RG_NoteUnhandled ("layered framebuffer attachment of a mip level past 0 (incomplete)");
		bad = true;
	}
	else if (texture && !(RGTG_Point (attachment) & (RGTG_BAD_DEPTH | RGTG_BAD_STENCIL)))
	{
		/* A colour layer would be a pass's colour target's layer: MRT and
		 * the layered colour targets are P6c's. */
		RG_NoteUnhandled ("layered colour attachment (P6c; incomplete)");
		bad = true;
	}
	/* Whether the texture is layered, and has the layer, is RGTG_Complete's
	 * to find: GL checks it at the attach, but either way the framebuffer
	 * is incomplete and nothing draws. */
	RGTG_Attach (f, attachment, texture, bad, texture ? layer : -1);
}

void RG_FramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum rbtarget,
				 GLuint renderbuffer)
{
	rgfbo_t	*f = RGTG_AttachTo (target);
	rgrb_t	*r = RGTG_Rb (renderbuffer);

	if (!f)
		return;
	if (renderbuffer && (rbtarget != GL_RENDERBUFFER || !r))
	{
		RG_NoteUnhandled ("framebuffer attachment of a renderbuffer never generated (incomplete)");
		RGTG_Attach (f, attachment, 0, true, -1);
		return;
	}
	RGTG_Attach (f, attachment, r ? r->tex : 0, false, -1);
}

void RG_DrawBuffers (GLsizei n, const GLenum *bufs)
{
	rgfbo_t	*f = RGTG_Fbo (rgfb_draw);

	if (!f)
	{
		/* The default framebuffer's are GL_BACK, which is all -gpu has. */
		RG_NoteUnhandled ("glDrawBuffers on framebuffer 0 (ignored)");
		return;
	}
	/* Each call replaces the last, an unsupported one included. */
	f->bad &= ~RGTG_BAD_DRAWBUFS;
	if (n == 1 && bufs[0] == GL_NONE)
		f->draw_none = true;
	else if (n == 1 && bufs[0] == GL_COLOR_ATTACHMENT0)
		f->draw_none = false;
	else
	{
		RG_NoteUnhandled ("glDrawBuffers other than GL_NONE or GL_COLOR_ATTACHMENT0 alone (MRT is P6)");
		f->bad |= RGTG_BAD_DRAWBUFS;
	}
	RGTG_Changed (f);
}

static qboolean RGTG_HasExtra (const rgfbo_t *f)
{
	int	i;

	for (i = 0; i < RGTG_EXTRA; i++)
	{
		if (f->extra[i])
			return true;
	}
	return false;
}

/* Whether `f` is complete, its GL status, and when it is the target it
 * describes, drawn into (`draw`) or read from: glDrawBuffers is the draw
 * side's alone.  Realizes the attachments, so a format the device cannot
 * render to is found here. */
static GLenum RGTG_Complete (const rgfbo_t *f, qboolean draw, rgtarget_t *out)
{
	rgtarget_t	t;

	SDL_zero (t);
	t.dfmt = SDL_GPU_TEXTUREFORMAT_INVALID;
	if (f->bad)
		return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	if (RGTG_HasExtra (f))
		return GL_FRAMEBUFFER_UNSUPPORTED;	/* one colour target per pass (P6) */
	if (!f->color && !f->depth && !f->stencil)
		return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
	if (f->stencil && f->stencil != f->depth)
		return GL_FRAMEBUFFER_UNSUPPORTED;	/* SDL_GPU has one depth-stencil texture */
	if (f->color)
	{
		if (!RG_Res_RenderTarget (f->color, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, -1, &t.color,
					  &t.cfmt, &t.cusage, &t.w, &t.h))
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
	}
	if (f->depth)
	{
		int	w, h;

		if (!RG_Res_RenderTarget (f->depth, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, f->dlayer,
					  &t.depth, &t.dfmt, &t.dusage, &w, &h))
			return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
		t.dlayered = f->dlayer >= 0;
		t.dlayer = t.dlayered ? f->dlayer : 0;
		if (t.color && (w != t.w || h != t.h))
			return GL_FRAMEBUFFER_UNSUPPORTED;	/* a render pass takes equal sizes */
		t.w = w;
		t.h = h;
	}
	if (draw && f->draw_none)
	{
		t.color = NULL;
		t.cfmt = SDL_GPU_TEXTUREFORMAT_INVALID;
		t.cusage = 0;
	}
	if (out)
		*out = t;
	return GL_FRAMEBUFFER_COMPLETE;
}

GLenum RG_CheckFramebufferStatus (GLenum target)
{
	GLuint	fbo;
	rgfbo_t	*f;
	GLenum	status;

	if (!RGTG_Binding (target, &fbo))
	{
		RG_NoteUnhandled ("glCheckFramebufferStatus of a target GL does not have");
		return 0;
	}
	if (fbo == 0)
		return GL_FRAMEBUFFER_COMPLETE;
	f = RGTG_Fbo (fbo);
	status = f ? RGTG_Complete (f, target != GL_READ_FRAMEBUFFER, NULL) : GL_FRAMEBUFFER_UNSUPPORTED;
	if (status != GL_FRAMEBUFFER_COMPLETE)
		rgtg_incomplete++;
	return status;
}

static qboolean RGTG_Resolve (GLuint fbo, qboolean draw, rgtarget_t *out)
{
	const rgfbo_t	*f;

	if (fbo == 0)
	{
		RG_Frame_SceneTarget (out);
		return true;
	}
	f = RGTG_Fbo (fbo);
	return f && RGTG_Complete (f, draw, out) == GL_FRAMEBUFFER_COMPLETE;
}

qboolean RG_Target_Resolve (GLuint fbo, rgtarget_t *out)
{
	return RGTG_Resolve (fbo, true, out);
}

GLuint RG_FramebufferBinding (GLenum pname)
{
	switch (pname)
	{
	case GL_DRAW_FRAMEBUFFER_BINDING:	/* == GL_FRAMEBUFFER_BINDING */
		return rgfb_draw;
	case GL_READ_FRAMEBUFFER_BINDING:
		return rgfb_read;
	}
	RG_NoteUnhandled ("framebuffer binding query of a pname GL does not have");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Renderbuffers                                                       */
/* ------------------------------------------------------------------ */

void RG_GenRenderbuffers (GLsizei n, GLuint *rbs)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		GLuint	name = RGTG_Name ((void **) &rgrb, &rgrb_count, &rgrb_used, rgrb_free,
					  &rgrb_numfree, sizeof(rgrb_t), "renderbuffer");

		rgrb[name].alive = true;
		RG_GenTextures (1, &rgrb[name].tex);
		rbs[i] = name;
	}
}

void RG_DeleteRenderbuffers (GLsizei n, const GLuint *rbs)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		rgrb_t	*r = RGTG_Rb (rbs[i]);

		if (!r)
			continue;
		RG_DeleteTextures (1, &r->tex);
		r->alive = false;
		r->tex = 0;
		if (rgrb_bound == rbs[i])
			rgrb_bound = 0;
		RGTG_FreeName (&rgrb_free, &rgrb_numfree, &rgrb_maxfree, rbs[i], "renderbuffer");
	}
}

void RG_BindRenderbuffer (GLenum target, GLuint rb)
{
	if (target != GL_RENDERBUFFER || (rb && !RGTG_Rb (rb)))
	{
		RG_NoteUnhandled ("renderbuffer bind of a name never generated (ignored)");
		return;
	}
	rgrb_bound = rb;
}

void RG_RenderbufferStorage (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
	rgrb_t	*r = RGTG_Rb (rgrb_bound);

	if (target != GL_RENDERBUFFER || !r)
	{
		RG_NoteUnhandled ("renderbuffer storage with no renderbuffer bound (ignored)");
		return;
	}
	RG_TexImage (GL_TEXTURE_2D, r->tex, 0, (GLint) internalformat, width, height, 1,
		     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	RG_Res_TargetOnly (r->tex);
}

/* ------------------------------------------------------------------ */
/* Blits                                                               */
/* ------------------------------------------------------------------ */

/* GL's [a0, a1) along one axis as a start, a length and whether it runs
 * backwards. */
static void RGTG_Span (GLint a0, GLint a1, Uint32 *start, Uint32 *len, qboolean *back)
{
	*back = a1 < a0;
	*start = (Uint32)(*back ? a1 : a0);
	*len = (Uint32)(*back ? a0 - a1 : a1 - a0);
}

static qboolean RGTG_Inside (GLint a0, GLint a1, GLint b0, GLint b1, int w, int h)
{
	return q_min (a0, a1) >= 0 && q_max (a0, a1) <= w && q_min (b0, b1) >= 0 && q_max (b0, b1) <= h;
}

static qboolean RGTG_BlitColor (SDL_GPUCommandBuffer *cmd, const rgtarget_t *src, const rgtarget_t *dst,
				GLint sx0, GLint sy0, GLint sx1, GLint sy1,
				GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter)
{
	SDL_GPUBlitInfo	bi;
	qboolean	sbx, sby, dbx, dby;
	int		flip = SDL_FLIP_NONE;

	if (!src->color || !dst->color || src->color == dst->color)
	{
		RG_NoteUnhandled ("colour blit with no colour on a side, or within one texture (declined)");
		return false;
	}
	if (!(src->cusage & SDL_GPU_TEXTUREUSAGE_SAMPLER))
	{
		RG_NoteUnhandled ("colour blit from a renderbuffer (multisample resolve is P6; declined)");
		return false;
	}
	if (!RGTG_Inside (sx0, sx1, sy0, sy1, src->w, src->h) ||
	    !RGTG_Inside (dx0, dx1, dy0, dy1, dst->w, dst->h))
	{
		RG_NoteUnhandled ("colour blit outside a framebuffer (declined)");
		return false;
	}
	SDL_zero (bi);
	bi.source.texture = src->color;
	RGTG_Span (sx0, sx1, &bi.source.x, &bi.source.w, &sbx);
	RGTG_Span (sy0, sy1, &bi.source.y, &bi.source.h, &sby);
	bi.destination.texture = dst->color;
	RGTG_Span (dx0, dx1, &bi.destination.x, &bi.destination.w, &dbx);
	RGTG_Span (dy0, dy1, &bi.destination.y, &bi.destination.h, &dby);
	if (!bi.source.w || !bi.source.h || !bi.destination.w || !bi.destination.h)
		return true;	/* GL draws nothing for an empty rectangle */
	/* A rectangle given backwards mirrors; rows need no flip of their own,
	 * every target holds GL's row order. */
	if (sbx != dbx)
		flip |= SDL_FLIP_HORIZONTAL;
	if (sby != dby)
		flip |= SDL_FLIP_VERTICAL;
	bi.flip_mode = (SDL_FlipMode) flip;
	bi.load_op = SDL_GPU_LOADOP_LOAD;
	bi.filter = (filter == GL_LINEAR) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
	SDL_BlitGPUTexture (cmd, &bi);
	rgtg_blits++;
	return true;
}

static qboolean RGTG_DepthCopyable (const rgtarget_t *src, const rgtarget_t *dst,
				    GLint sx0, GLint sy0, GLint sx1, GLint sy1,
				    GLint dx0, GLint dy0, GLint dx1, GLint dy1)
{
	if (!src->depth || !dst->depth || src->depth == dst->depth)
	{
		RG_NoteUnhandled ("depth blit with no depth on a side, or within one texture (declined)");
		return false;
	}
	/* Texels, not values: the copy needs the whole of each, in one format.
	 * It carries the stencil along; nothing here asks for depth alone into
	 * a target whose stencil it then reads. */
	if (src->dfmt != dst->dfmt || src->w != dst->w || src->h != dst->h ||
	    sx0 != 0 || sy0 != 0 || sx1 != src->w || sy1 != src->h ||
	    dx0 != 0 || dy0 != 0 || dx1 != dst->w || dy1 != dst->h)
	{
		RG_NoteUnhandled ("depth blit other than a whole 1:1 copy between one format (declined)");
		return false;
	}
	return true;
}

static qboolean RGTG_CopyDepth (SDL_GPUCommandBuffer *cmd, const rgtarget_t *src, const rgtarget_t *dst,
				GLint sx0, GLint sy0, GLint sx1, GLint sy1,
				GLint dx0, GLint dy0, GLint dx1, GLint dy1)
{
	SDL_GPUTextureLocation	s, d;
	SDL_GPUCopyPass		*copy;

	if (!RGTG_DepthCopyable (src, dst, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1))
		return false;
	SDL_zero (s);
	s.texture = src->depth;
	s.layer = (Uint32) src->dlayer;	/* the attached layer: GL copies that one */
	SDL_zero (d);
	d.texture = dst->depth;
	d.layer = (Uint32) dst->dlayer;
	copy = SDL_BeginGPUCopyPass (cmd);
	if (!copy)
		return false;
	SDL_CopyGPUTextureToTexture (copy, &s, &d, (Uint32) src->w, (Uint32) src->h, 1, false);
	SDL_EndGPUCopyPass (copy);
	rgtg_copies++;
	return true;
}

qboolean RG_BlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1,
			     GLint dx0, GLint dy0, GLint dx1, GLint dy1,
			     GLbitfield mask, GLenum filter)
{
	rgtarget_t		src, dst;
	SDL_GPUCommandBuffer	*cmd;
	qboolean		ok = true;

	mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	if (!mask)
		return true;
	if (!RGTG_Resolve (rgfb_read, false, &src) || !RGTG_Resolve (rgfb_draw, true, &dst))
	{
		RG_NoteUnhandled ("framebuffer blit from or to an incomplete framebuffer (declined)");
		rgtg_declined++;
		return false;
	}
	if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) &&
	    !RGTG_DepthCopyable (&src, &dst, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1))
	{
		rgtg_declined++;
		return false;
	}
	/* Everything drawn so far lands first, and the blit goes between passes.
	 * Into the scene, that includes the frame's first pass, which clears
	 * every aspect: the blit writes over it, whatever it covers. */
	cmd = RG_Frame_Break ();
	/* Again: the segment's copy pass may have re-created an attachment. */
	if (!cmd || !RGTG_Resolve (rgfb_read, false, &src) || !RGTG_Resolve (rgfb_draw, true, &dst))
	{
		RG_NoteUnhandled ("framebuffer blit outside a frame (declined)");
		rgtg_declined++;
		return false;
	}
	if (mask & GL_COLOR_BUFFER_BIT)
	{
		if (!RGTG_BlitColor (cmd, &src, &dst, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filter))
			ok = false;
	}
	if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
	{
		if (!RGTG_CopyDepth (cmd, &src, &dst, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1))
			ok = false;
	}
	if (!ok)
		rgtg_declined++;
	return ok;
}

void RG_Target_TextureDeleted (GLuint tex)
{
	int	i;

	for (i = 1; i <= rgfb_used; i++)
	{
		rgfbo_t		*f = &rgfb[i];
		qboolean	changed = false;
		int		k;

		if (!f->alive)
			continue;
		/* GL detaches it: a point that held it, badly or not, is empty. */
		if (f->color == tex)
		{
			f->color = 0;
			f->bad &= ~RGTG_BAD_COLOR (0);
			changed = true;
		}
		if (f->depth == tex)
		{
			f->depth = 0;
			f->dlayer = -1;
			f->bad &= ~RGTG_BAD_DEPTH;
			changed = true;
		}
		if (f->stencil == tex)
		{
			f->stencil = 0;
			f->bad &= ~RGTG_BAD_STENCIL;
			changed = true;
		}
		for (k = 0; k < RGTG_EXTRA; k++)
		{
			if (f->extra[k] == tex)
			{
				f->extra[k] = 0;
				f->bad &= ~RGTG_BAD_COLOR (k + 1);
				changed = true;
			}
		}
		if (changed)
			RGTG_Changed (f);
	}
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void RG_Target_Init (void)
{
	rgfb_draw = rgfb_read = 0;
	rgrb_bound = 0;
	rgtg_blits = rgtg_copies = rgtg_declined = rgtg_incomplete = 0;
}

/* The textures are rg_resource.c's, released there; only the tables here. */
void RG_Target_Shutdown (void)
{
	free (rgfb); rgfb = NULL;
	free (rgfb_free); rgfb_free = NULL;
	free (rgrb); rgrb = NULL;
	free (rgrb_free); rgrb_free = NULL;
	rgfb_count = rgfb_used = rgfb_numfree = rgfb_maxfree = 0;
	rgrb_count = rgrb_used = rgrb_numfree = rgrb_maxfree = 0;
	rgfb_draw = rgfb_read = rgrb_bound = 0;
}

void RG_Target_Status (void)
{
	int	i, fbos = 0, rbs = 0;

	for (i = 1; i <= rgfb_used; i++)
		fbos += rgfb[i].alive;
	for (i = 1; i <= rgrb_used; i++)
		rbs += rgrb[i].alive;
	Con_Printf ("[RENDERER] SDL_GPU render targets: %d framebuffers, %d renderbuffers, draw %u read %u; "
		    "%lu colour blits, %lu depth copies, %lu blits declined, %lu incomplete status checks\n",
		    fbos, rbs, rgfb_draw, rgfb_read, rgtg_blits, rgtg_copies, rgtg_declined,
		    rgtg_incomplete);
}
