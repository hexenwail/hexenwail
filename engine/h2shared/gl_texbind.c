/* gl_texbind.c -- texture binding, shaped like a descriptor set
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_texbind.h"

#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE	0x9100
#endif

/* Targets worth shadowing.  Binding a 3D texture does not disturb the 2D
 * binding on the same unit, so they need separate slots or the shadow would
 * claim a bind had happened that had not. */
enum {
	RT_T2D = 0,
	RT_T3D,
	RT_T2DMS,
	RT_NUM_TARGETS
};

static GLuint	bound[RT_MAX_TEXTURE_UNITS][RT_NUM_TARGETS];
static GLuint	active_unit;

static int RT_TargetIndex (GLenum target)
{
	switch (target)
	{
	case GL_TEXTURE_2D:		return RT_T2D;
	case GL_TEXTURE_3D:		return RT_T3D;
	case GL_TEXTURE_2D_MULTISAMPLE:	return RT_T2DMS;
	default:			return -1;
	}
}

/* The cursor.  Kept private so the invariant in the header -- unit 0 active on
 * return from every entry point -- is this file's problem and nobody else's. */
static void RT_Activate (GLuint slot)
{
	if (active_unit == slot)
		return;
	active_unit = slot;
	glActiveTexture_fp (GL_TEXTURE0 + slot);
}

static void RT_Bind (GLuint slot, GLenum target, GLuint texture)
{
	int ti = RT_TargetIndex (target);

	if (slot < RT_MAX_TEXTURE_UNITS && ti >= 0)
	{
		if (bound[slot][ti] == texture)
			return;
		bound[slot][ti] = texture;
	}

	RT_Activate (slot);
	glBindTexture_fp (target, texture);
}

void R_BindTextures (GLuint first, GLsizei count, const GLuint *textures)
{
	GLsizei i;

	for (i = 0; i < count; i++)
		RT_Bind (first + i, GL_TEXTURE_2D, textures[i]);

	RT_Activate (0);
}

void R_BindTextureSlot (GLuint slot, GLuint texture)
{
	RT_Bind (slot, GL_TEXTURE_2D, texture);
	RT_Activate (0);
}

void R_BindTextureTarget (GLuint slot, GLenum target, GLuint texture)
{
	RT_Bind (slot, target, texture);
	RT_Activate (0);
}

GLuint R_CurrentTexture (void)
{
	return bound[0][RT_T2D];
}

/* Zero is a real binding -- it means "nothing bound" -- so clearing the shadow
 * to zero would claim the units are already unbound when they are not.  Use the
 * sentinel no texture name can take. */
void R_ResetTextureBindings (void)
{
	int u, t;

	for (u = 0; u < RT_MAX_TEXTURE_UNITS; u++)
		for (t = 0; t < RT_NUM_TARGETS; t++)
			bound[u][t] = GL_UNUSED_TEXTURE;
}

void R_DeleteTextures (GLsizei n, const GLuint *textures)
{
	glDeleteTextures_fp (n, textures);
	R_ResetTextureBindings ();
}

void R_TexBind_Init (void)
{
	R_ResetTextureBindings ();
	active_unit = 0;
	glActiveTexture_fp (GL_TEXTURE0);
}

void R_TexBind_Shutdown (void)
{
	R_ResetTextureBindings ();
}
