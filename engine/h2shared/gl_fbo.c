/* gl_fbo.c -- framebuffer and renderbuffer objects, as a seam
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* The GL implementation of gl_fbo.h; see the header for the contract.
 * #340 P4a-1. */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_fbo.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

#ifndef GL_FRAMEBUFFER_UNSUPPORTED
#define GL_FRAMEBUFFER_UNSUPPORTED	0x8CDD
#endif

#ifdef USE_SDL_GPU
/* Until P4a-2, each arm below notes the call and does nothing else. */
static void RG_FboZeroNames (GLsizei n, GLuint *names)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
		names[i] = 0;
}
#endif

void R_GenFramebuffers (GLsizei n, GLuint *fbos)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_GenFramebuffers (render targets are P4a-2)");
		RG_FboZeroNames (n, fbos);
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
		RG_NoteUnhandled ("R_DeleteFramebuffers (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_BindFramebuffer (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_FramebufferTexture2D (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_FramebufferRenderbuffer (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_CheckFramebufferStatus (render targets are P4a-2)");
		return GL_FRAMEBUFFER_UNSUPPORTED;
	}
#endif
	return glCheckFramebufferStatus_fp (target);
}

void R_DrawBuffers (GLsizei n, const GLenum *bufs)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_DrawBuffers (render targets are P4a-2)");
		return;
	}
#endif
	glDrawBuffers_fp (n, bufs);
}

void R_GenRenderbuffers (GLsizei n, GLuint *rbs)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_GenRenderbuffers (render targets are P4a-2)");
		RG_FboZeroNames (n, rbs);
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
		RG_NoteUnhandled ("R_DeleteRenderbuffers (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_BindRenderbuffer (render targets are P4a-2)");
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
		RG_NoteUnhandled ("R_RenderbufferStorage (render targets are P4a-2)");
		return;
	}
#endif
	glRenderbufferStorage_fp (target, internalformat, width, height);
}

qboolean R_BlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1,
			    GLint dx0, GLint dy0, GLint dx1, GLint dy1,
			    GLbitfield mask, GLenum filter)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_BlitFramebuffer (render targets are P4a-2)");
		return false;
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
		RG_NoteUnhandled ("R_FramebufferBinding (render targets are P4a-2)");
		return 0;
	}
#endif
	glGetIntegerv_fp (pname, &fbo);
	return (GLuint) fbo;
}
