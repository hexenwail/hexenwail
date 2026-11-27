/* gl_texcreate.h -- texture creation, upload and sampler state
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* GL edits a texture through a binding: glTexImage2D, glTexParameter and the
 * rest act on whatever is bound to the active unit, and name no texture.
 * SDL_GPU and Vulkan have no such binding -- a texture is created from a
 * description, filled through a copy pass, and sampled through a separate
 * sampler object -- so every entry point here names the texture it acts on.
 * GitHub #340, phase P1a.
 *
 * It is the only renderer code that creates, sizes, uploads into or sets the
 * sampling state of a texture (the loader table gl_func.h, the null-GL
 * table rg_null.c and WEBSOFT's raw-GLES canvas web_canvas_gl2.c aside);
 * tools/test_texture_create_routing.py fails on any other caller.
 * Deleting stays in gl_texbind.h (R_DeleteTextures), because a delete is the
 * one texture call that has to update the binding shadow.
 *
 * GL ARM.  Each entry point issues the GL call(s) its call site issued
 * before it existed, and nothing else -- in particular it does NOT bind
 * `tex`.  The two sampler helpers are the only spelling change: they set
 * enum-valued parameters with glTexParameteri (several sites used the
 * float variant) in a fixed order (a few sites set MAG before MIN).
 * Neither is visible in a frame: a parameter is state, read at draw.  The caller still binds for edit
 * (R_BindTextureForEdit, or R_BindTextureSlot on slot 0), as it did; `tex`
 * is the name the SDL_GPU arm will need and GL does not.  Binding here
 * instead would add a glBindTexture for every untracked target
 * (GL_TEXTURE_CUBE_MAP_ARRAY) and after every shadow reset, changing the
 * call stream this phase promised to leave alone.  So that a wrong `tex`
 * cannot hide until there is a backend that reads it, every entry point
 * checks it against gl_texbind.c's record of unit 0 (R_EditBinding) and
 * reports a disagreement on the console -- no GL call, just the shadow.
 *
 * SDL_GPU ARM (#340 P1c).  After the RTC_Check, each entry point is
 * `if (R_GPU ()) { RG_xxx (...); return; }` (rg_resource.c): the texture is
 * named, so the backend needs no binding.  The entry points SDL_GPU cannot
 * do yet -- multisample storage, block-compressed data, framebuffer copies
 * -- count themselves in `gpu_unrouted` instead of drawing wrong.
 * tools/test_rg_sources.py fails a GL call here with no arm ahead of it.
 *
 * Arguments mirror the GL call they replace, minus what core GL fixes: no
 * border (always 0).  Handles and enums stay GL-shaped -- decision 1 on #340:
 * the backend maps GL names to its own objects.
 */

#ifndef GL_TEXCREATE_H
#define GL_TEXCREATE_H

/* glGenTextures.  A fresh name is bound nowhere, so there is no texture to
 * check yet. */
void	R_GenTextures (GLsizei n, GLuint *textures);

/* Storage and contents.  `target` is the image target: a cube face
 * (GL_TEXTURE_CUBE_MAP_POSITIVE_X + i) for R_TexImage2D on a cube map. */
void	R_TexImage2D (GLenum target, GLuint tex, GLint level, GLint internalformat,
		      GLsizei width, GLsizei height, GLenum format, GLenum type,
		      const void *pixels);
void	R_TexImage3D (GLenum target, GLuint tex, GLint level, GLint internalformat,
		      GLsizei width, GLsizei height, GLsizei depth, GLenum format,
		      GLenum type, const void *pixels);
void	R_TexImage2DMultisample (GLenum target, GLuint tex, GLsizei samples,
				 GLenum internalformat, GLsizei width, GLsizei height,
				 GLboolean fixedsamplelocations);
void	R_TexStorage2D (GLenum target, GLuint tex, GLsizei levels, GLenum internalformat,
			GLsizei width, GLsizei height);
void	R_TexCompressedImage2D (GLenum target, GLuint tex, GLint level, GLenum internalformat,
				GLsizei width, GLsizei height, GLsizei imagesize,
				const void *data);

/* Upload into part of an existing level. */
void	R_TexSubImage2D (GLenum target, GLuint tex, GLint level, GLint x, GLint y,
			 GLsizei width, GLsizei height, GLenum format, GLenum type,
			 const void *pixels);
/* The same, from a source whose rows are `row_length` pixels apart rather
 * than `width` -- a rectangle cut out of a larger CPU image.  GL spells the
 * stride as unpack state (GL_UNPACK_ROW_LENGTH, set and put back to 0
 * around the upload); SDL_GPU as the transfer's pixels_per_row. */
void	R_TexSubImage2DRows (GLenum target, GLuint tex, GLint level, GLint x, GLint y,
			     GLsizei width, GLsizei height, GLint row_length,
			     GLenum format, GLenum type, const void *pixels);
/* Copy a rectangle of the current read framebuffer into a level. */
void	R_TexCopySubImage2D (GLenum target, GLuint tex, GLint level, GLint xoffset,
			     GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);

/* Sampling state -- what SDL_GPU puts in a sampler object.  R_TexSetFilter
 * sets the minification then the magnification filter; R_TexSetWrap sets
 * S and T, plus R on the targets that have a third coordinate
 * (GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP_ARRAY).  Everything else -- LOD bias,
 * anisotropy, base and max level, swizzle, depth compare -- goes through the
 * generic pair, named by its GL pname.  The helpers are shorthands, not
 * the only route: the generic pair also carries filter pnames where a site
 * sets MIN and MAG on different branches (Draw_TouchAllFilterModes), so a
 * backend has to map every sampler pname from both. */
void	R_TexSetFilter (GLenum target, GLuint tex, GLint min_filter, GLint mag_filter);
void	R_TexSetWrap (GLenum target, GLuint tex, GLint wrap);
void	R_TexParameteri (GLenum target, GLuint tex, GLenum pname, GLint value);
void	R_TexParameterf (GLenum target, GLuint tex, GLenum pname, GLfloat value);

#ifndef USE_GLES
/* glGetIntegerv (GL_MAX_CUBE_MAP_TEXTURE_SIZE), 0 for no answer: the cube
 * shadow maps' size cap (#340 P6a).  The -gpu arm is P6b's; it answers 0,
 * which the caller reads as no cap. */
int	R_MaxCubeMapSize (void);
#endif

/* How many entry-point calls named a texture other than the one bound for
 * edit (see GL ARM above); `renderer_status` prints it.  Zero is the only
 * correct value.  The check is blind where the shadow is: the untracked
 * GL_TEXTURE_CUBE_MAP_ARRAY, and any edit made after a shadow reset with no
 * bind of its own. */
int	R_TexCreate_Mismatches (void);

#endif	/* GL_TEXCREATE_H */
