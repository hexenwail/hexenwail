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
 * Under -gpu (#340 P4a-2) every arm is rg_target.c's: framebuffers and
 * renderbuffers keyed by the same GL names, on SDL_GPU textures, with GL's
 * binding rules and an honest glCheckFramebufferStatus -- what a render pass
 * cannot be begun on is incomplete, and the chain turns post-process off on
 * that.  R_BlitFramebuffer declines (false, counted in gpu_unrouted) a blit
 * SDL_GPU cannot make: a depth copy other than whole, 1:1 and between one
 * format, a colour blit from a renderbuffer.
 *
 * The layered attachment, the per-draw-buffer clear and multisampled
 * storage (#340 P6a) are seams with GL's signatures too, each GL arm the
 * bare call.  Their -gpu arms count themselves in gpu_unrouted and do
 * nothing until the sub-phase that builds them: layers are P6b (the cube
 * shadow maps), per-buffer clears P6b/P6c, multisampled storage P6d.  Every
 * caller is held off under -gpu today (R_Shadow_Prepare, the OIT gates,
 * PP_SceneSamples). */

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
/* One layer of an array, cube or cube-array texture as the attachment. */
void	R_FramebufferTextureLayer (GLenum target, GLenum attachment, GLuint texture,
				   GLint level, GLint layer);
/* glClearBufferfv: GL_COLOR and a draw-buffer index, or GL_DEPTH and 0, of
 * the framebuffer bound to GL_DRAW_FRAMEBUFFER. */
void	R_ClearBufferfv (GLenum buffer, GLint drawbuffer, const GLfloat *value);

void	R_GenRenderbuffers (GLsizei n, GLuint *rbs);
void	R_DeleteRenderbuffers (GLsizei n, const GLuint *rbs);
void	R_BindRenderbuffer (GLenum target, GLuint rb);
void	R_RenderbufferStorage (GLenum target, GLenum internalformat,
			       GLsizei width, GLsizei height);
void	R_RenderbufferStorageMultisample (GLenum target, GLsizei samples, GLenum internalformat,
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
