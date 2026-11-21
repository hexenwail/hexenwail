/* gl_fbo.h -- framebuffer and renderbuffer objects, as a seam
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* The post-process chain (gl_postprocess.c) renders into framebuffer objects
 * and blits between them.  These are those calls with GL's signatures, so
 * the chain names no GL entry point: the GL arm of each is the bare call it
 * replaced, so a GL build draws what it drew before, call for call.
 *
 * Under -gpu (#340) every arm is RG_NoteUnhandled for now: counted in
 * gpu_unrouted as a routed call the backend did not do.  The chain stays off
 * under -gpu (PP_NeedsPostProcess), so nothing reaches them; a call that
 * does is a gate leak and shows up there.  P4a-2 puts the render-target
 * backend (rg_target.c) behind them.  What each arm answers until then:
 * Gen* fills zero names, R_CheckFramebufferStatus GL_FRAMEBUFFER_UNSUPPORTED,
 * R_FramebufferBinding 0, R_BlitFramebuffer false.
 *
 * MSAA (glRenderbufferStorageMultisample) is P6 and has no seam: -gpu forces
 * the chain's sample count to 0 (PP_CreateFBO). */

#ifndef GL_FBO_H
#define GL_FBO_H

void	R_GenFramebuffers (GLsizei n, GLuint *fbos);
void	R_DeleteFramebuffers (GLsizei n, const GLuint *fbos);
/* GL_FRAMEBUFFER binds both the read and the draw target. */
void	R_BindFramebuffer (GLenum target, GLuint fbo);
void	R_FramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget,
				GLuint texture, GLint level);
void	R_FramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum rbtarget,
				   GLuint renderbuffer);
GLenum	R_CheckFramebufferStatus (GLenum target);
/* The draw buffers of the framebuffer bound to GL_DRAW_FRAMEBUFFER. */
void	R_DrawBuffers (GLsizei n, const GLenum *bufs);

void	R_GenRenderbuffers (GLsizei n, GLuint *rbs);
void	R_DeleteRenderbuffers (GLsizei n, const GLuint *rbs);
void	R_BindRenderbuffer (GLenum target, GLuint rb);
void	R_RenderbufferStorage (GLenum target, GLenum internalformat,
			       GLsizei width, GLsizei height);

/* glBlitFramebuffer from the bound read framebuffer to the bound draw
 * framebuffer.  False when the copy was not made: the GL arm always makes
 * it (GL reports a bad blit as an error, never here), the -gpu arm may
 * decline one it cannot do -- a caller that marks the result valid, as the
 * soft-particle depth snapshot does, tests this. */
qboolean R_BlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1,
			    GLint dx0, GLint dy0, GLint dx1, GLint dy1,
			    GLbitfield mask, GLenum filter);

/* glGetIntegerv of GL_FRAMEBUFFER_BINDING / GL_DRAW_FRAMEBUFFER_BINDING /
 * GL_READ_FRAMEBUFFER_BINDING. */
GLuint	R_FramebufferBinding (GLenum pname);

#endif	/* GL_FBO_H */
