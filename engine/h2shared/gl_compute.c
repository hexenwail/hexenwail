/* gl_compute.c -- compute dispatch, barriers and readback, as a seam
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* gl_compute.h's implementation; see the header for the contract.  The GL
 * arm of each seam is the bare call (#340 P5a-1); each SDL_GPU arm is the
 * backend call of the same name (P5a-2: rg_compute.c, rg_resource.c,
 * rg_buffer.c, rg_shader.c). */

#include "quakedef.h"
#include "gl_texbind.h"
#include "gl_compute.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

#ifndef USE_GLES

void R_DispatchCompute (GLuint groups_x, GLuint groups_y, GLuint groups_z)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_DispatchCompute (groups_x, groups_y, groups_z);
		return;
	}
#endif
	glDispatchCompute_fp (groups_x, groups_y, groups_z);
}

void R_MemoryBarrier (GLbitfield barriers)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_MemoryBarrier (barriers);
		return;
	}
#endif
	glMemoryBarrier_fp (barriers);
}

void R_DeleteProgram (GLuint program)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_ProgramDelete (program);
		return;
	}
#endif
	glDeleteProgram_fp (program);
}

int R_MaxStorageBindings (void)
{
	GLint	n = 0;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_MaxStorageBindings ();
	}
#endif
	glGetIntegerv_fp (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &n);
	return (int) n;
}

qboolean R_ReadBuffer (GLenum target, GLintptr offset, GLsizeiptr size, void *data)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_ReadBuffer (target, offset, size, data);
	}
#endif
	if (!glGetBufferSubData_fp)
		return false;
	glGetBufferSubData_fp (target, offset, size, data);
	return true;
}

qboolean R_ReadTexImage (GLenum target, GLuint texture, GLint level, GLenum format,
			 GLenum type, void *pixels)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_ReadTexture (target, texture, level, format, type, pixels);
	}
#endif
	if (!glGetTexImage_fp)
		return false;
	R_BindTextureForEdit (target, texture);
	glGetTexImage_fp (target, level, format, type, pixels);
	R_BindTextureForEdit (target, 0);
	return true;
}

qboolean R_StorageFormatOk (GLenum internalformat, GLenum target, GLenum access)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_StorageFormatOk (internalformat, target, access);
	}
#endif
	(void) internalformat;
	(void) target;
	(void) access;
	return true;
}

#endif	/* !USE_GLES */
