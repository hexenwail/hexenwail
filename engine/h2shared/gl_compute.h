/* gl_compute.h -- compute dispatch, barriers and readback, as a seam
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* The compute passes -- the softemu palette LUT (gl_postprocess.c), the light
 * cluster (gl_lightcluster.c), the world cull and Hi-Z (gl_worldcull.c) and
 * SSAO (gl_ssao.c) -- make these calls with GL's signatures, so none of them
 * names a GL entry point: the GL arm of each is the bare call it replaced, so
 * a GL build issues what it issued before, call for call (#340 P5a-1).
 *
 * Under -gpu each SDL_GPU arm is the backend call of the same name (#340
 * P5a-2): a dispatch is a compute pass of its own (rg_compute.c), so a
 * barrier is already met; the readbacks submit what is recorded and wait for
 * it; the storage formats are SDL_GPUTextureSupportsFormat's answer.
 * GL_Init_NullGL turns compute_shaders on, and the passes not routed yet --
 * the light cluster, the world cull, Hi-Z, SSAO -- keep a gate each, which
 * their sub-phases lift; the palette LUT runs.
 *
 * Desktop GL only: the ES tier has no compute and no image load/store, and
 * every consumer compiles out there, so does this. */

#ifndef GL_COMPUTE_H
#define GL_COMPUTE_H

#ifndef USE_GLES

/* glDispatchCompute with the program bound by R_UseProgram. */
void	R_DispatchCompute (GLuint groups_x, GLuint groups_y, GLuint groups_z);
/* glMemoryBarrier.  Under -gpu met by the pass boundary every dispatch has. */
void	R_MemoryBarrier (GLbitfield barriers);
/* glDeleteProgram of a program R_Program_Create made. */
void	R_DeleteProgram (GLuint program);
/* glGetIntegerv (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS). */
int	R_MaxStorageBindings (void);
/* glGetBufferSubData from the buffer bound to `target`.  False when nothing
 * was read: the GL arm without the entry point, the -gpu arm when the range
 * is outside the buffer or the device fails. */
qboolean R_ReadBuffer (GLenum target, GLintptr offset, GLsizeiptr size, void *data);
/* glGetTexImage of `texture`'s `level`, binding it for edit around the read
 * as the callers did.  False when nothing was read, as R_ReadBuffer. */
qboolean R_ReadTexImage (GLenum target, GLuint texture, GLint level, GLenum format,
			 GLenum type, void *pixels);
/* Whether a texture of `internalformat` on `target` can be bound as a
 * storage image with `access` (GL_WRITE_ONLY / GL_READ_WRITE).  GL 4.3
 * guarantees every format the passes use, so the GL arm says yes; SDL_GPU
 * guarantees few of them, and its arm asks the device. */
qboolean R_StorageFormatOk (GLenum internalformat, GLenum target, GLenum access);

#endif	/* !USE_GLES */

#endif	/* GL_COMPUTE_H */
