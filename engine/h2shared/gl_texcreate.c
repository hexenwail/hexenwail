/* gl_texcreate.c -- texture creation, upload and sampler state
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* The GL implementation of gl_texcreate.h.  See the header for the shape and
 * for where the SDL_GPU arm goes; tools/test_texture_create_routing.py keeps
 * every other caller out.  #340 P1a. */

#include "quakedef.h"
#include "sdl_inc.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

#ifndef GL_TEXTURE_CUBE_MAP_ARRAY
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#endif
#ifndef GL_MAX_CUBE_MAP_TEXTURE_SIZE
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE	0x851C
#endif

static int	rtc_mismatches;

/* No GL call: the record is gl_texbind.c's shadow of unit 0.  An untracked
 * target or a shadow just reset answers GL_UNUSED_TEXTURE, which means
 * "unknown" and is not a disagreement. */
static void RTC_Check (const char *fn, GLenum target, GLuint tex)
{
	GLuint	bound = R_EditBinding (target);

	if (bound == GL_UNUSED_TEXTURE || bound == tex)
		return;
	if (++rtc_mismatches <= 8)
		Con_Printf ("%s: names texture %u, but %u is bound for edit\n",
			    fn, (unsigned int)tex, (unsigned int)bound);
}

int R_TexCreate_Mismatches (void)
{
	return rtc_mismatches;
}

void R_GenTextures (GLsizei n, GLuint *textures)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GenTextures (n, textures);
		return;
	}
#endif
	glGenTextures_fp (n, textures);
}

void R_TexImage2D (GLenum target, GLuint tex, GLint level, GLint internalformat,
		   GLsizei width, GLsizei height, GLenum format, GLenum type,
		   const void *pixels)
{
	RTC_Check ("R_TexImage2D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexImage (target, tex, level, internalformat, width, height, 1, format, type, pixels);
		return;
	}
#endif
	glTexImage2D_fp (target, level, internalformat, width, height, 0, format, type, pixels);
}

void R_TexImage3D (GLenum target, GLuint tex, GLint level, GLint internalformat,
		   GLsizei width, GLsizei height, GLsizei depth, GLenum format,
		   GLenum type, const void *pixels)
{
	RTC_Check ("R_TexImage3D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexImage (target, tex, level, internalformat, width, height, depth, format, type, pixels);
		return;
	}
#endif
	glTexImage3D_fp (target, level, internalformat, width, height, depth, 0, format, type, pixels);
}

void R_TexImage2DMultisample (GLenum target, GLuint tex, GLsizei samples,
			      GLenum internalformat, GLsizei width, GLsizei height,
			      GLboolean fixedsamplelocations)
{
	RTC_Check ("R_TexImage2DMultisample", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_TexImage2DMultisample (MSAA targets are P6)");
		return;
	}
#endif
	glTexImage2DMultisample_fp (target, samples, internalformat, width, height,
				    fixedsamplelocations);
}

void R_TexStorage2D (GLenum target, GLuint tex, GLsizei levels, GLenum internalformat,
		     GLsizei width, GLsizei height)
{
	RTC_Check ("R_TexStorage2D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexStorage2D (target, tex, levels, internalformat, width, height);
		return;
	}
#endif
	glTexStorage2D_fp (target, levels, internalformat, width, height);
}

void R_TexCompressedImage2D (GLenum target, GLuint tex, GLint level, GLenum internalformat,
			     GLsizei width, GLsizei height, GLsizei imagesize,
			     const void *data)
{
	RTC_Check ("R_TexCompressedImage2D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_TexCompressedImage2D (no block formats yet)");
		return;
	}
#endif
	glCompressedTexImage2D_fp (target, level, internalformat, width, height, 0,
				   imagesize, data);
}

void R_TexSubImage2D (GLenum target, GLuint tex, GLint level, GLint x, GLint y,
		      GLsizei width, GLsizei height, GLenum format, GLenum type,
		      const void *pixels)
{
	RTC_Check ("R_TexSubImage2D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexSubImage (target, tex, level, x, y, width, height, width, format, type, pixels);
		return;
	}
#endif
	glTexSubImage2D_fp (target, level, x, y, width, height, format, type, pixels);
}

void R_TexSubImage2DRows (GLenum target, GLuint tex, GLint level, GLint x, GLint y,
			  GLsizei width, GLsizei height, GLint row_length,
			  GLenum format, GLenum type, const void *pixels)
{
	RTC_Check ("R_TexSubImage2DRows", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexSubImage (target, tex, level, x, y, width, height, row_length, format, type, pixels);
		return;
	}
#endif
	glPixelStorei_fp (GL_UNPACK_ROW_LENGTH, row_length);
	glTexSubImage2D_fp (target, level, x, y, width, height, format, type, pixels);
	glPixelStorei_fp (GL_UNPACK_ROW_LENGTH, 0);
}

void R_TexCopySubImage2D (GLenum target, GLuint tex, GLint level, GLint xoffset,
			  GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
	RTC_Check ("R_TexCopySubImage2D", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("R_TexCopySubImage2D (framebuffer reads are P4/P7)");
		return;
	}
#endif
	glCopyTexSubImage2D_fp (target, level, xoffset, yoffset, x, y, width, height);
}

void R_TexSetFilter (GLenum target, GLuint tex, GLint min_filter, GLint mag_filter)
{
	RTC_Check ("R_TexSetFilter", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexParameter (target, tex, GL_TEXTURE_MIN_FILTER, min_filter, (GLfloat)min_filter);
		RG_TexParameter (target, tex, GL_TEXTURE_MAG_FILTER, mag_filter, (GLfloat)mag_filter);
		return;
	}
#endif
	glTexParameteri_fp (target, GL_TEXTURE_MIN_FILTER, min_filter);
	glTexParameteri_fp (target, GL_TEXTURE_MAG_FILTER, mag_filter);
}

void R_TexSetWrap (GLenum target, GLuint tex, GLint wrap)
{
	RTC_Check ("R_TexSetWrap", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexParameter (target, tex, GL_TEXTURE_WRAP_S, wrap, (GLfloat)wrap);
		RG_TexParameter (target, tex, GL_TEXTURE_WRAP_T, wrap, (GLfloat)wrap);
		RG_TexParameter (target, tex, GL_TEXTURE_WRAP_R, wrap, (GLfloat)wrap);
		return;
	}
#endif
	glTexParameteri_fp (target, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri_fp (target, GL_TEXTURE_WRAP_T, wrap);
	if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP_ARRAY)
		glTexParameteri_fp (target, GL_TEXTURE_WRAP_R, wrap);
}

void R_TexParameteri (GLenum target, GLuint tex, GLenum pname, GLint value)
{
	RTC_Check ("R_TexParameteri", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexParameter (target, tex, pname, value, (GLfloat)value);
		return;
	}
#endif
	glTexParameteri_fp (target, pname, value);
}

void R_TexParameterf (GLenum target, GLuint tex, GLenum pname, GLfloat value)
{
	RTC_Check ("R_TexParameterf", target, tex);
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_TexParameter (target, tex, pname, (GLint)value, value);
		return;
	}
#endif
	glTexParameterf_fp (target, pname, value);
}

#ifndef USE_GLES
int R_MaxCubeMapSize (void)
{
	GLint	size = 0;

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_MaxTextureSize ();
	}
#endif
	glGetIntegerv_fp (GL_MAX_CUBE_MAP_TEXTURE_SIZE, &size);
	return size;
}
#endif
