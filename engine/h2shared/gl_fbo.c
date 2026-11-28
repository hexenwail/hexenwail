/* gl_fbo.c -- framebuffer and renderbuffer objects, as a seam
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* gl_fbo.h's implementation; see the header for the contract.  The GL arm
 * of each seam is the bare call (#340 P4a-1), the SDL_GPU arm rg_target.c's
 * (P4a-2), or for P6a's three a counted no-op until P6b-d. */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_fbo.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

void R_GenFramebuffers (GLsizei n, GLuint *fbos)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GenFramebuffers (n, fbos);
		return;
	}
#endif
	glGenFramebuffers_fp (n, fbos);
}

void R_DeleteFramebuffers (GLsizei n, const GLuint *fbos)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DeleteFramebuffers (n, fbos);
		return;
	}
#endif
	glDeleteFramebuffers_fp (n, fbos);
}

void R_BindFramebuffer (GLenum target, GLuint fbo)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindFramebuffer (target, fbo);
		return;
	}
#endif
	glBindFramebuffer_fp (target, fbo);
}

void R_FramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget,
			     GLuint texture, GLint level)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_FramebufferTexture2D (target, attachment, textarget, texture, level);
		return;
	}
#endif
	glFramebufferTexture2D_fp (target, attachment, textarget, texture, level);
}

void R_FramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum rbtarget,
				GLuint renderbuffer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_FramebufferRenderbuffer (target, attachment, rbtarget, renderbuffer);
		return;
	}
#endif
	glFramebufferRenderbuffer_fp (target, attachment, rbtarget, renderbuffer);
}

GLenum R_CheckFramebufferStatus (GLenum target)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_CheckFramebufferStatus (target);
	}
#endif
	return glCheckFramebufferStatus_fp (target);
}

void R_DrawBuffers (GLsizei n, const GLenum *bufs)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DrawBuffers (n, bufs);
		return;
	}
#endif
	glDrawBuffers_fp (n, bufs);
}

void R_FramebufferTextureLayer (GLenum target, GLenum attachment, GLuint texture,
				GLint level, GLint layer)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_FramebufferTextureLayer (layered attachments are P6b)");
		return;
	}
#endif
	glFramebufferTextureLayer_fp (target, attachment, texture, level, layer);
}

void R_ClearBufferfv (GLenum buffer, GLint drawbuffer, const GLfloat *value)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_ClearBufferfv (per-draw-buffer clears are P6b/P6c)");
		return;
	}
#endif
	glClearBufferfv_fp (buffer, drawbuffer, value);
}

void R_GenRenderbuffers (GLsizei n, GLuint *rbs)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GenRenderbuffers (n, rbs);
		return;
	}
#endif
	glGenRenderbuffers_fp (n, rbs);
}

void R_DeleteRenderbuffers (GLsizei n, const GLuint *rbs)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DeleteRenderbuffers (n, rbs);
		return;
	}
#endif
	glDeleteRenderbuffers_fp (n, rbs);
}

void R_BindRenderbuffer (GLenum target, GLuint rb)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_BindRenderbuffer (target, rb);
		return;
	}
#endif
	glBindRenderbuffer_fp (target, rb);
}

void R_RenderbufferStorage (GLenum target, GLenum internalformat,
			    GLsizei width, GLsizei height)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_RenderbufferStorage (target, internalformat, width, height);
		return;
	}
#endif
	glRenderbufferStorage_fp (target, internalformat, width, height);
}

void R_RenderbufferStorageMultisample (GLenum target, GLsizei samples, GLenum internalformat,
				       GLsizei width, GLsizei height)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_RenderbufferStorageMultisample (MSAA targets are P6d)");
		return;
	}
#endif
	glRenderbufferStorageMultisample_fp (target, samples, internalformat, width, height);
}

qboolean R_BlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1,
			    GLint dx0, GLint dy0, GLint dx1, GLint dy1,
			    GLbitfield mask, GLenum filter)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_BlitFramebuffer (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	}
#endif
	glBlitFramebuffer_fp (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	return true;
}

GLuint R_FramebufferBinding (GLenum pname)
{
	GLint	fbo = 0;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_FramebufferBinding (pname);
	}
#endif
	glGetIntegerv_fp (pname, &fbo);
	return (GLuint) fbo;
}
