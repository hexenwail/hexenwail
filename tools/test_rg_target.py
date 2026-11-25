#!/usr/bin/env python3
"""rg_target.c keeps GL's framebuffer rules, and an upload into the bound
target lands after its pending clear (GitHub #548, #340 P4a-2).

tools/test_rg_sources.py pins the lines the -gpu post-process sweep rests
on.  This drives the seams the sweep does not reach.  It compiles the real
engine/h2shared/rg_target.c against a fake SDL_GPU and a fake frame
recorder and resource table, then calls the RG_* entry points gl_fbo.c
calls and checks, case by case:

  * glDrawBuffers (GL_NONE) is the draw side's: a framebuffer with no
    colour drawn still has its colour to read, so a blit from it is made,
    and drawing into it still has no colour target;
  * nothing about an unsupported attachment is sticky.  A mip level or a
    cube face attached, a renderbuffer never generated, MRT draw buffers:
    each makes the framebuffer incomplete, and what replaces it at that
    point (another attachment, a detach, a later glDrawBuffers) or deleting
    the texture makes it complete again, as GL's does.  A second colour
    attachment deleted is detached too.  An attachment point GL does not
    have is GL_INVALID_ENUM and changes nothing;
  * every attach, detach and glDrawBuffers on the bound draw framebuffer,
    a bad one included, and every delete of a texture it holds, re-resolves
    the frame's target (RG_Frame_BindTarget), so the frame never keeps
    drawing into a target GL says is incomplete, or drops draws into one
    GL says is complete;
  * a blit ends the segment (RG_Frame_Break) before it writes anything, and
    nothing after it marks the scene written.  This one is a pin, not a bug
    proof: on the scene the segment's end is the frame's first pass, which
    clears every aspect, so the RG_Frame_SceneWritten call that used to
    follow a blit into framebuffer 0 only set a flag already set, and was
    removed as dead.  The first-pass condition is pinned in
    test_rg_sources.py (RGF_EndSegment), and RG_Clear resolving a dirty
    target before it records a clear there too.

And, lifted from rg_resource.c and rg_frame.c, compiled against stubs:

  * RGT_FlushIfRead ends the segment for an upload into the bound target
    when the segment holds a clear for it and no draws: the copy pass runs
    ahead of the render pass whose load op is that clear, so without the
    flush the clear wipes the upload GL put after it;
  * RG_Frame_SegmentWritesTarget says so for draws, a clear, or both, and
    not for neither.

Stock python3 and a C compiler (cc / gcc / clang); lint.yml.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "engine/h2shared/rg_target.c"
INTERNAL = ROOT / "engine/h2shared/rg_internal.h"
RESOURCE = ROOT / "engine/h2shared/rg_resource.c"
FRAME = ROOT / "engine/h2shared/rg_frame.c"

QUAKEDEF = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int qboolean;
#define true 1
#define false 0
typedef uint8_t Uint8;
typedef uint32_t Uint32;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
#define q_min(a, b) ((a) < (b) ? (a) : (b))
#define q_max(a, b) ((a) > (b) ? (a) : (b))
#define GL_NONE 0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR_ATTACHMENT1 0x8CE1
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
void Sys_Error (const char *fmt, ...);
void Con_Printf (const char *fmt, ...);
'''

RG_INTERNAL = r'''
#include "quakedef.h"
#define SDL_zero(x) memset (&(x), 0, sizeof(x))
typedef struct SDL_GPUTexture { int id; } SDL_GPUTexture;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUCopyPass SDL_GPUCopyPass;
typedef enum { SDL_GPU_TEXTUREFORMAT_INVALID, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
	       SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTUREFORMAT_D24_UNORM } SDL_GPUTextureFormat;
typedef Uint32 SDL_GPUTextureUsageFlags;
#define SDL_GPU_TEXTUREUSAGE_SAMPLER 1u
#define SDL_GPU_TEXTUREUSAGE_COLOR_TARGET 2u
#define SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET 4u
typedef enum { SDL_FLIP_NONE, SDL_FLIP_HORIZONTAL, SDL_FLIP_VERTICAL } SDL_FlipMode;
typedef enum { SDL_GPU_LOADOP_LOAD, SDL_GPU_LOADOP_CLEAR } SDL_GPULoadOp;
typedef enum { SDL_GPU_FILTER_NEAREST, SDL_GPU_FILTER_LINEAR } SDL_GPUFilter;
typedef struct { SDL_GPUTexture *texture; Uint32 mip_level, layer_or_depth_plane, x, y, w, h; } SDL_GPUBlitRegion;
typedef struct { SDL_GPUBlitRegion source, destination; SDL_GPULoadOp load_op; SDL_FlipMode flip_mode;
		 SDL_GPUFilter filter; } SDL_GPUBlitInfo;
typedef struct { SDL_GPUTexture *texture; Uint32 mip_level, layer, x, y, z; } SDL_GPUTextureLocation;
void SDL_BlitGPUTexture (SDL_GPUCommandBuffer *cmd, const SDL_GPUBlitInfo *info);
SDL_GPUCopyPass *SDL_BeginGPUCopyPass (SDL_GPUCommandBuffer *cmd);
void SDL_CopyGPUTextureToTexture (SDL_GPUCopyPass *p, const SDL_GPUTextureLocation *s,
				  const SDL_GPUTextureLocation *d, Uint32 w, Uint32 h, Uint32 depth, qboolean cycle);
void SDL_EndGPUCopyPass (SDL_GPUCopyPass *p);
void RG_NoteUnhandled (const char *what);
/* rg_resource.c */
void RG_GenTextures (GLsizei n, GLuint *textures);
void RG_DeleteTextures (GLsizei n, const GLuint *textures);
void RG_TexImage (GLenum target, GLuint tex, GLint level, GLint internalformat,
		  GLsizei width, GLsizei height, GLsizei depth, GLenum format,
		  GLenum type, const void *pixels);
qboolean RG_Res_RenderTarget (GLuint name, SDL_GPUTextureUsageFlags usage, SDL_GPUTexture **tex,
			      SDL_GPUTextureFormat *fmt, SDL_GPUTextureUsageFlags *have, int *w, int *h);
void RG_Res_TargetOnly (GLuint name);
/* the real header's target type */
%(rgtarget)s
void RG_Target_TextureDeleted (GLuint tex);
qboolean RG_Target_Resolve (GLuint fbo, rgtarget_t *out);
/* rg_frame.c */
void RG_Frame_BindTarget (GLuint fbo);
void RG_Frame_SceneTarget (rgtarget_t *out);
SDL_GPUCommandBuffer *RG_Frame_Break (void);
/* rg_target.c's entry points, from rg_device.h */
void RG_GenFramebuffers (GLsizei n, GLuint *fbos);
void RG_DeleteFramebuffers (GLsizei n, const GLuint *fbos);
void RG_BindFramebuffer (GLenum target, GLuint fbo);
void RG_FramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget,
			      GLuint texture, GLint level);
void RG_FramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum rbtarget,
				 GLuint renderbuffer);
GLenum RG_CheckFramebufferStatus (GLenum target);
void RG_DrawBuffers (GLsizei n, const GLenum *bufs);
void RG_GenRenderbuffers (GLsizei n, GLuint *rbs);
void RG_DeleteRenderbuffers (GLsizei n, const GLuint *rbs);
void RG_BindRenderbuffer (GLenum target, GLuint rb);
void RG_RenderbufferStorage (GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
qboolean RG_BlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1,
			     GLint dx0, GLint dy0, GLint dx1, GLint dy1,
			     GLbitfield mask, GLenum filter);
void RG_Target_Init (void);
void RG_Target_Shutdown (void);
'''

# The fakes alone, which tools/test_soft_depth_gpu.py builds on too.
FAKES = r'''
#include <stdarg.h>
#include "rg_internal.h"

static int failures, unhandled, binds, breaks;
static char events[256];
static int nevents;
static void Event (char c) { if (nevents < (int) sizeof(events) - 1) events[nevents++] = c; }

void Sys_Error (const char *fmt, ...) { va_list a; va_start (a, fmt); vprintf (fmt, a); va_end (a); exit (2); }
void Con_Printf (const char *fmt, ...) { (void) fmt; }
void RG_NoteUnhandled (const char *what) { (void) what; unhandled++; }

/* rg_frame.c's current target, as the last RG_Frame_BindTarget resolved it */
static SDL_GPUTexture scene_color = { 1001 }, scene_depth = { 1002 };
static GLuint frame_fbo;
static rgtarget_t frame_tgt;
static qboolean frame_ok = true;

/* rg_resource.c: a texture is defined, a colour or depth target, sampled
 * unless it is a renderbuffer's. */
typedef struct { int alive, defined; SDL_GPUTexture tex; SDL_GPUTextureFormat fmt;
		 SDL_GPUTextureUsageFlags usage; int w, h; } ftex_t;
static ftex_t tex[256];
static int ntex;
void RG_GenTextures (GLsizei n, GLuint *t)
{
	GLsizei i;
	for (i = 0; i < n; i++)
	{
		t[i] = (GLuint) ++ntex;
		memset (&tex[t[i]], 0, sizeof(ftex_t));
		tex[t[i]].alive = 1;
		tex[t[i]].tex.id = (int) t[i];
	}
}
void RG_DeleteTextures (GLsizei n, const GLuint *t)
{
	GLsizei i;
	for (i = 0; i < n; i++)
	{
		/* In the real one's order: RG_Frame_ReleaseTarget marks the
		 * frame's target dirty if it holds the texture, then every
		 * framebuffer drops the name; a dirty target is resolved
		 * again before the next draw or pass, after both. */
		qboolean dirty = frame_ok && (frame_tgt.color == &tex[t[i]].tex || frame_tgt.depth == &tex[t[i]].tex);
		RG_Target_TextureDeleted (t[i]);
		tex[t[i]].alive = 0;
		if (dirty)
			RG_Frame_BindTarget (frame_fbo);
	}
}
void RG_TexImage (GLenum target, GLuint t, GLint level, GLint ifmt, GLsizei w, GLsizei h,
		  GLsizei d, GLenum format, GLenum type, const void *pixels)
{
	(void) target; (void) level; (void) d; (void) format; (void) type; (void) pixels;
	tex[t].defined = 1;
	tex[t].w = w;
	tex[t].h = h;
	if (ifmt == GL_DEPTH24_STENCIL8 || ifmt == GL_DEPTH_COMPONENT24)
	{
		/* GL_DEPTH24_STENCIL8 is the scene's own format, as RGT_Format
		 * maps it; GL_DEPTH_COMPONENT24 is not. */
		tex[t].fmt = (ifmt == GL_DEPTH24_STENCIL8) ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
							   : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
		tex[t].usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	}
	else
	{
		tex[t].fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		tex[t].usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	}
}
qboolean RG_Res_RenderTarget (GLuint name, SDL_GPUTextureUsageFlags usage, SDL_GPUTexture **t,
			      SDL_GPUTextureFormat *fmt, SDL_GPUTextureUsageFlags *have, int *w, int *h)
{
	if (!name || name > (GLuint) ntex || !tex[name].alive || !tex[name].defined || !(tex[name].usage & usage))
		return false;
	*t = &tex[name].tex;
	*fmt = tex[name].fmt;
	*have = tex[name].usage;
	*w = tex[name].w;
	*h = tex[name].h;
	return true;
}
void RG_Res_TargetOnly (GLuint name) { tex[name].usage &= ~SDL_GPU_TEXTUREUSAGE_SAMPLER; }

/* rg_frame.c: the frame's target is what the bound draw framebuffer
 * resolved to at the last RG_Frame_BindTarget. */
static SDL_GPUCommandBuffer *const cmdbuf = (SDL_GPUCommandBuffer *) 16;
void RG_Frame_SceneTarget (rgtarget_t *out)
{
	memset (out, 0, sizeof(*out));
	out->color = &scene_color;
	out->depth = &scene_depth;
	out->cfmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	out->dfmt = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
	out->cusage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	out->dusage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	out->w = 64;
	out->h = 32;
}
void RG_Frame_BindTarget (GLuint fbo)
{
	binds++;
	frame_fbo = fbo;
	memset (&frame_tgt, 0, sizeof(frame_tgt));
	frame_ok = RG_Target_Resolve (fbo, &frame_tgt);
}
SDL_GPUCommandBuffer *RG_Frame_Break (void) { breaks++; Event ('B'); return cmdbuf; }

/* SDL_GPU */
static SDL_GPUBlitInfo last_blit;
static int nblits, ncopies;
static SDL_GPUTexture *last_copy_src, *last_copy_dst;
void SDL_BlitGPUTexture (SDL_GPUCommandBuffer *cmd, const SDL_GPUBlitInfo *info)
{
	(void) cmd;
	last_blit = *info;
	nblits++;
	Event ('C');
}
SDL_GPUCopyPass *SDL_BeginGPUCopyPass (SDL_GPUCommandBuffer *cmd) { (void) cmd; return (SDL_GPUCopyPass *) 32; }
void SDL_CopyGPUTextureToTexture (SDL_GPUCopyPass *p, const SDL_GPUTextureLocation *s,
				  const SDL_GPUTextureLocation *d, Uint32 w, Uint32 h, Uint32 depth, qboolean cycle)
{
	(void) p; (void) w; (void) h; (void) depth; (void) cycle;
	last_copy_src = s->texture;
	last_copy_dst = d->texture;
	ncopies++;
	Event ('D');
}
void SDL_EndGPUCopyPass (SDL_GPUCopyPass *p) { (void) p; }
'''

HARNESS = FAKES + r'''
#define CHECK(c, what) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __func__, __LINE__, what); failures++; } } while (0)

static GLuint ColorTex (int w, int h)
{
	GLuint t;
	RG_GenTextures (1, &t);
	RG_TexImage (GL_TEXTURE_2D, t, 0, GL_RGBA8, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	return t;
}

static GLuint DepthTex (int w, int h)
{
	GLuint t;
	RG_GenTextures (1, &t);
	RG_TexImage (GL_TEXTURE_2D, t, 0, GL_DEPTH24_STENCIL8, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	return t;
}

static GLenum Status (void) { return RG_CheckFramebufferStatus (GL_FRAMEBUFFER); }

/* #548 2: glDrawBuffers is the draw side's alone. */
static void read_side_drawbuffers (void)
{
	GLuint fbo[2], a = ColorTex (64, 32), b = ColorTex (64, 32);
	const GLenum none = GL_NONE;

	RG_GenFramebuffers (2, fbo);
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo[0]);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0);
	RG_DrawBuffers (1, &none);
	CHECK (frame_ok && frame_fbo == fbo[0] && !frame_tgt.color && frame_tgt.w == 64,
	       "drawn into, a GL_NONE framebuffer is complete with no colour target");
	CHECK (RG_CheckFramebufferStatus (GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
	       "a GL_NONE framebuffer is complete to read");
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo[1]);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b, 0);
	RG_BindFramebuffer (GL_READ_FRAMEBUFFER, fbo[0]);
	nblits = 0;
	CHECK (RG_BlitFramebuffer (0, 0, 64, 32, 0, 0, 64, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST),
	       "a colour blit from a framebuffer whose draw buffer is GL_NONE is made");
	CHECK (nblits == 1 && last_blit.source.texture == &tex[a].tex && last_blit.destination.texture == &tex[b].tex,
	       "it reads the read framebuffer's colour attachment");
	/* The other way round: drawing into it has no colour to blit to. */
	RG_BindFramebuffer (GL_READ_FRAMEBUFFER, fbo[1]);
	RG_BindFramebuffer (GL_DRAW_FRAMEBUFFER, fbo[0]);
	nblits = 0;
	CHECK (!RG_BlitFramebuffer (0, 0, 64, 32, 0, 0, 64, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST) && nblits == 0,
	       "a colour blit into a GL_NONE framebuffer has nothing to write (declined)");
	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	RG_DeleteFramebuffers (2, fbo);
}

/* #548 3: a bad attachment is replaced like any other. */
static void bad_attachment_replaced (void)
{
	GLuint fbo, c = ColorTex (64, 32), d = DepthTex (64, 32), rb = 0;
	const GLenum one = GL_COLOR_ATTACHMENT0, two[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	int b0, u0;

	RG_GenFramebuffers (1, &fbo);
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d, 0);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok && frame_tgt.color == &tex[c].tex,
	       "colour + depth-stencil is complete and is the frame's target");

	/* A mip level: incomplete, and the frame knows at once. */
	b0 = binds;
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 1);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE, "a mip level attached is incomplete");
	CHECK (binds > b0 && !frame_ok, "a bad attachment to the bound framebuffer re-resolves the frame's target");
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok && frame_tgt.color == &tex[c].tex,
	       "level 0 attached in its place: complete again");

	/* A cube face, then a detach. */
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, c, 0);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE && !frame_ok, "a cube face attached is incomplete");
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok && !frame_tgt.color && frame_tgt.depth == &tex[d].tex,
	       "detached: complete, depth only");
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);

	/* A bad depth point does not stay when the colour point is reattached. */
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d, 2);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE, "a bad depth point is not cleared by another point's attach");
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d, 0);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE, "the depth point replaced: complete again");

	/* A renderbuffer never generated, then a real one. */
	RG_FramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 77);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE && !frame_ok, "a renderbuffer never generated is incomplete");
	RG_GenRenderbuffers (1, &rb);
	RG_BindRenderbuffer (GL_RENDERBUFFER, rb);
	RG_RenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 64, 32);
	RG_FramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok && frame_tgt.depth != NULL,
	       "a real renderbuffer in its place: complete again");

	/* MRT draw buffers, then one. */
	RG_DrawBuffers (2, two);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE && !frame_ok, "MRT draw buffers are incomplete (P6)");
	RG_DrawBuffers (1, &one);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok && frame_tgt.color == &tex[c].tex,
	       "a later glDrawBuffers (GL_COLOR_ATTACHMENT0) replaces it: complete again");

	/* GL_INVALID_ENUM changes nothing. */
	u0 = unhandled;
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, 0x1234, GL_TEXTURE_2D, c, 0);
	CHECK (unhandled > u0, "an attachment point GL does not have is noted");
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok, "and changes nothing");

	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	RG_DeleteFramebuffers (1, &fbo);
	RG_DeleteRenderbuffers (1, &rb);
}

/* #548 3: a delete detaches whatever held the name, badly or as a second colour. */
static void delete_detaches (void)
{
	GLuint fbo, c = ColorTex (64, 32), m = ColorTex (64, 32), x = ColorTex (64, 32);
	const GLenum none = GL_NONE;
	int b0;

	RG_GenFramebuffers (1, &fbo);
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, x, 0);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE && !frame_ok, "a second colour attachment is unsupported (P6)");
	RG_DeleteTextures (1, &x);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok, "deleting the second colour detaches it");

	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m, 3);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE, "a mip level attached is incomplete");
	RG_DeleteTextures (1, &m);
	CHECK (Status () != GL_FRAMEBUFFER_COMPLETE,
	       "deleting the badly attached texture leaves nothing attached (missing, not complete)");
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	CHECK (Status () == GL_FRAMEBUFFER_COMPLETE && frame_ok, "and a good one attached after is complete");

	/* With no colour drawn the frame's target holds no colour texture, so
	 * only the detach can tell it the framebuffer is now empty. */
	RG_DrawBuffers (1, &none);
	CHECK (frame_ok && !frame_tgt.color, "GL_NONE: complete, no colour target");
	b0 = binds;
	RG_DeleteTextures (1, &c);
	CHECK (binds > b0 && !frame_ok, "deleting the bound framebuffer's only attachment re-resolves the frame's target");
	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	RG_DeleteFramebuffers (1, &fbo);
}

/* #548 1 (not a bug; a pin): a blit into the scene comes after the
 * segment's end, which is the scene's first (clearing) pass, and nothing
 * after it marks the scene written. */
static void blit_after_break (void)
{
	GLuint fbo, c = ColorTex (64, 32);

	RG_GenFramebuffers (1, &fbo);
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	RG_BindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
	nevents = 0;
	memset (events, 0, sizeof(events));
	CHECK (RG_BlitFramebuffer (0, 0, 64, 32, 0, 0, 32, 16, GL_COLOR_BUFFER_BIT, GL_LINEAR),
	       "a colour blit into part of the scene is made");
	CHECK (!strcmp (events, "BC"), "the segment ends, then the blit writes");
	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	RG_DeleteFramebuffers (1, &fbo);
}

int main (void)
{
	RG_Target_Init ();
	read_side_drawbuffers ();
	bad_attachment_replaced ();
	delete_detaches ();
	blit_after_break ();
	RG_Target_Shutdown ();
	if (failures)
		return 1;
	printf ("ok\n");
	return 0;
}
'''

LIFTED = r'''
#include <stdio.h>
typedef int qboolean;
typedef unsigned int GLbitfield;
#define true 1
#define false 0
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct { qboolean read_ever; unsigned int read_segment; SDL_GPUTexture *tex; } rgtex_t;

static int rgf_numdraws;
static GLbitfield rgf_clear_mask;
static int holds, flushes;
static SDL_GPUTexture *const target = (SDL_GPUTexture *) 64;
unsigned int RG_Frame_Segment (void) { return 9; }
qboolean RG_Frame_SegmentHasDraws (void) { return rgf_numdraws > 0; }
qboolean RG_Frame_TargetHolds (const SDL_GPUTexture *t) { return holds && t == target; }
void RG_Frame_Flush (void) { flushes++; rgf_numdraws = 0; rgf_clear_mask = 0; }
%(writes)s
%(flush)s

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf ("FAIL %%s\n", what); failures++; } } while (0)

static int Case (int draws, GLbitfield clear, int h, qboolean read)
{
	rgtex_t t = { read, 9, target };
	rgf_numdraws = draws;
	rgf_clear_mask = clear;
	holds = h;
	flushes = 0;
	RGT_FlushIfRead (&t);
	return flushes;
}

int main (void)
{
	rgf_numdraws = 0; rgf_clear_mask = 0;
	CHECK (!RG_Frame_SegmentWritesTarget (), "an empty segment writes no target");
	rgf_clear_mask = GL_COLOR_BUFFER_BIT;
	CHECK (RG_Frame_SegmentWritesTarget (), "a pending clear writes the target");
	rgf_clear_mask = 0; rgf_numdraws = 2;
	CHECK (RG_Frame_SegmentWritesTarget (), "draws write the target");
	rgf_clear_mask = GL_DEPTH_BUFFER_BIT;
	CHECK (RG_Frame_SegmentWritesTarget (), "draws and a clear write the target");

	CHECK (Case (0, GL_COLOR_BUFFER_BIT, 1, false) == 1,
	       "an upload into the bound target behind a pending clear ends the segment first");
	CHECK (Case (0, GL_DEPTH_BUFFER_BIT, 1, false) == 1,
	       "behind a pending depth clear too (one pass loads both)");
	CHECK (Case (3, 0, 1, false) == 1, "an upload into the bound target behind its draws ends the segment");
	CHECK (Case (0, 0, 1, false) == 0, "nothing pending for the bound target: no flush");
	CHECK (Case (0, GL_COLOR_BUFFER_BIT, 0, false) == 0,
	       "a clear pending for another target does not flush an upload into this one");
	CHECK (Case (3, 0, 0, true) == 1, "a texture a draw of the segment reads still flushes");
	CHECK (Case (0, GL_COLOR_BUFFER_BIT, 0, true) == 0, "read with no draws recorded: no flush");
	if (failures)
		return 1;
	printf ("ok\n");
	return 0;
}
'''


def strip_comments(text):
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def function(text, head_rx, what):
    m = re.search(head_rx, text, flags=re.M)
    if not m:
        raise AssertionError("cannot find " + what)
    brace = text.index("{", m.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


def rgtarget_block():
    h = INTERNAL.read_text()
    m = re.search(r"typedef struct \{[^{}]*\} rgtarget_t;", h)
    if not m:
        sys.exit("FAIL: rg_internal.h has no rgtarget_t")
    return m.group(0)


def compile_and_run(cc, td, sources, what):
    exe = Path(td) / ("x" + str(abs(hash(what))))
    r = subprocess.run([cc, "-std=gnu99", "-Wall", "-Werror=implicit-function-declaration",
                        "-DUSE_SDL_GPU", "-I", td, "-o", str(exe)] + [str(s) for s in sources],
                       capture_output=True, text=True)
    if r.returncode:
        return "%s does not build against the harness:\n%s" % (what, r.stderr[-3000:])
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    if r.returncode:
        return "%s disagrees with GL:\n%s" % (what, (r.stdout + r.stderr)[-3000:])
    return None


def main():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        sys.exit("FAIL: no C compiler")
    errors = []
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        (t / "quakedef.h").write_text(QUAKEDEF)
        (t / "rg_internal.h").write_text(RG_INTERNAL % {"rgtarget": rgtarget_block()})
        (t / "harness.c").write_text(HARNESS)
        # Its own quoted includes would find the real headers beside it.
        shutil.copy2(SRC, t / "rg_target.c")
        err = compile_and_run(cc, td, [t / "harness.c", t / "rg_target.c"], "rg_target.c")
        if err:
            errors.append(err)

        res = strip_comments(RESOURCE.read_text())
        frame = strip_comments(FRAME.read_text())
        try:
            flush = function(res, r"^static void RGT_FlushIfRead \(const rgtex_t \*t\)\s*$",
                             "RGT_FlushIfRead in rg_resource.c")
            writes = function(frame, r"^qboolean RG_Frame_SegmentWritesTarget \(void\)\s*$",
                              "qboolean RG_Frame_SegmentWritesTarget (void) in rg_frame.c")
        except AssertionError as e:
            errors.append(str(e))
        else:
            (t / "lifted.c").write_text(LIFTED % {"flush": flush, "writes": writes})
            err = compile_and_run(cc, td, [t / "lifted.c"], "RGT_FlushIfRead / RG_Frame_SegmentWritesTarget")
            if err:
                errors.append(err)
    if errors:
        sys.exit("FAIL: " + "\nFAIL: ".join(errors))
    print("PASS rg_target.c keeps GL's framebuffer rules (glDrawBuffers on the draw side alone, no "
          "sticky bad attachment, every change to the bound target re-resolved, a blit after the "
          "segment's end) and an upload into the bound target lands after its pending clear (#548)")


if __name__ == "__main__":
    main()
