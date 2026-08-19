/* gl_uniforms.c -- std140 uniform blocks shared by every draw program
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
#include "gl_matrix.h"
#include "gl_uniforms.h"

#ifndef GL_UNIFORM_BLOCK_DATA_SIZE
#define GL_UNIFORM_BLOCK_DATA_SIZE	0x8A40
#endif

/* On the ES tier the entry points are real symbols, not pointers, so they
 * cannot be tested as values -- same shape as HW_OIT_HAS_BLEND_FUNCI. */
#ifdef USE_GLES
#define RU_HAVE_UBO_BIND	1
#define RU_HAVE_UBO_QUERY	1
#else
#define RU_HAVE_UBO_BIND	(glGetUniformBlockIndex_fp != NULL && \
				 glUniformBlockBinding_fp != NULL)
#define RU_HAVE_UBO_QUERY	(glGetActiveUniformBlockiv_fp != NULL)
#endif

static GLuint		ubo_perframe;
static GLuint		ubo_perdraw;

static r_perframe_t	perframe;
static r_perdraw_t	perdraw;

static qboolean		perframe_dirty;
static qboolean		perdraw_dirty;
static qboolean		layout_checked;

/* The C mirrors are only trustworthy if they are the size std140 says they
 * are.  Both are built from vec4/mat4/ivec4 only, so this is arithmetic, not
 * hope: 10 vec4 + 1 mat4, and 3 mat4 + 3 vec4. */
COMPILE_TIME_ASSERT(perframe_size, sizeof(r_perframe_t) == 10 * 16 + 64);
COMPILE_TIME_ASSERT(perdraw_size, sizeof(r_perdraw_t) == 3 * 64 + 3 * 16);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static qboolean RU_Set4 (float *dst, float a, float b, float c, float d)
{
	if (dst[0] == a && dst[1] == b && dst[2] == c && dst[3] == d)
		return false;
	dst[0] = a;
	dst[1] = b;
	dst[2] = c;
	dst[3] = d;
	return true;
}

static qboolean RU_SetMat (float *dst, const float *m16)
{
	if (!memcmp(dst, m16, 16 * sizeof(float)))
		return false;
	memcpy(dst, m16, 16 * sizeof(float));
	return true;
}

/* ------------------------------------------------------------------ */
/* Per-frame setters                                                   */
/* ------------------------------------------------------------------ */

void R_SetFog (float density, const float *rgb)
{
	if (RU_Set4(perframe.fog, rgb[0], rgb[1], rgb[2], density))
		perframe_dirty = true;
}

void R_SetEyePos (const float *xyz)
{
	if (RU_Set4(perframe.eyetime, xyz[0], xyz[1], xyz[2], perframe.eyetime[3]))
		perframe_dirty = true;
}

void R_SetFrameTime (float t)
{
	if (perframe.eyetime[3] != t)
	{
		perframe.eyetime[3] = t;
		perframe_dirty = true;
	}
}

void R_SetWorldCaustics (float intensity, float time)
{
	if (perframe.worldp[0] != intensity || perframe.worldp[1] != time)
	{
		perframe.worldp[0] = intensity;
		perframe.worldp[1] = time;
		perframe_dirty = true;
	}
}

void R_SetOverbright (float v)
{
	if (perframe.worldp[2] != v)
	{
		perframe.worldp[2] = v;
		perframe_dirty = true;
	}
}

void R_SetLightmapBicubic (float v)
{
	if (perframe.worldp[3] != v)
	{
		perframe.worldp[3] = v;
		perframe_dirty = true;
	}
}

void R_SetLightDebug (float fullbright, float lightmapdbg)
{
	if (RU_Set4(perframe.debugp, fullbright, lightmapdbg, 0.0f, 0.0f))
		perframe_dirty = true;
}

void R_SetSkyFog (float r, float g, float b, float a)
{
	if (RU_Set4(perframe.skyfog, r, g, b, a))
		perframe_dirty = true;
}

void R_SetSkyWind (float u, float v)
{
	if (RU_Set4(perframe.wind, u, v, 0.0f, 0.0f))
		perframe_dirty = true;
}

void R_SetParticleBasis (const float *pup, const float *pright,
			 const float *vpn, const float *origin, float ctime)
{
	int changed = 0;

	changed |= RU_Set4(perframe.pup, pup[0], pup[1], pup[2], 0.0f);
	changed |= RU_Set4(perframe.pright, pright[0], pright[1], pright[2], 0.0f);
	changed |= RU_Set4(perframe.vpn, vpn[0], vpn[1], vpn[2], 0.0f);
	changed |= RU_Set4(perframe.origin, origin[0], origin[1], origin[2], ctime);
	if (changed)
		perframe_dirty = true;
}

void R_SetViewProj (const float *m16)
{
	if (RU_SetMat(perframe.viewproj, m16))
		perframe_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Per-draw setters                                                    */
/* ------------------------------------------------------------------ */

void R_SetMVP (const float *m16)
{
	if (RU_SetMat(perdraw.mvp, m16))
		perdraw_dirty = true;
}

void R_SetModelView (const float *m16)
{
	if (RU_SetMat(perdraw.modelview, m16))
		perdraw_dirty = true;
}

void R_SetModelMatrix (const float *m16)
{
	if (RU_SetMat(perdraw.model, m16))
		perdraw_dirty = true;
}

void R_SetAlphaThresholdU (float v)
{
	if (perdraw.draw0[0] != v)
	{
		perdraw.draw0[0] = v;
		perdraw_dirty = true;
	}
}

void R_SetForceOpaqueAlphaU (float v)
{
	if (perdraw.draw0[1] != v)
	{
		perdraw.draw0[1] = v;
		perdraw_dirty = true;
	}
}

void R_SetAliasCaustics (float intensity, float time)
{
	if (perdraw.draw0[2] != intensity || perdraw.draw0[3] != time)
	{
		perdraw.draw0[2] = intensity;
		perdraw.draw0[3] = time;
		perdraw_dirty = true;
	}
}

void R_SetSoftParams (float x, float y, float z)
{
	if (RU_Set4(perdraw.draw1, x, y, z, 0.0f))
		perdraw_dirty = true;
}

void R_SetPoseBase (int v)
{
	if (perdraw.drawi[0] != v)
	{
		perdraw.drawi[0] = v;
		perdraw_dirty = true;
	}
}

void R_SetInstBase (int v)
{
	if (perdraw.drawi[1] != v)
	{
		perdraw.drawi[1] = v;
		perdraw_dirty = true;
	}
}

void R_SetPoseVertType (int v)
{
	if (perdraw.drawi[2] != v)
	{
		perdraw.drawi[2] = v;
		perdraw_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* Upload                                                              */
/* ------------------------------------------------------------------ */

/* glBufferData rather than glBufferSubData: respecifying the whole store
 * orphans the old one, so a flush never waits on a draw that is still reading
 * the previous contents.  Both blocks are a few hundred bytes, so the
 * respecify costs nothing next to the stall it avoids. */
void R_FlushUniforms (void)
{
	if (perframe_dirty)
	{
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_perframe);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(perframe), &perframe,
				 GL_STREAM_DRAW);
		perframe_dirty = false;
	}
	if (perdraw_dirty)
	{
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_perdraw);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(perdraw), &perdraw,
				 GL_STREAM_DRAW);
		perdraw_dirty = false;
	}
}

void R_InvalidateUniforms (void)
{
	perframe_dirty = true;
	perdraw_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Program hookup                                                      */
/* ------------------------------------------------------------------ */

/* Ask the driver where it actually put things, once, and complain rather than
 * render garbage if it disagrees with the C mirror.  A std140 block built only
 * from vec4/mat4/ivec4 leaves the driver no room to differ, which is the point
 * -- but a layout bug here is invisible on screen except as wrong values, so
 * it is worth one query at startup to turn that into a console line. */
static void RU_CheckBlockSize (GLuint program, const char *name, size_t expect)
{
	GLuint idx;
	GLint size = 0;

	if (!RU_HAVE_UBO_QUERY)
		return;
	idx = glGetUniformBlockIndex_fp (program, name);
	if (idx == GL_INVALID_INDEX)
		return;
	glGetActiveUniformBlockiv_fp (program, idx, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
	if (size != (GLint)expect)
		Con_Printf ("WARNING: uniform block %s is %d bytes on this driver, "
			    "C mirror is %d -- uniforms will be wrong\n",
			    name, (int)size, (int)expect);
}

void R_BindProgramBlocks (GLuint program)
{
	GLuint idx;

	if (!program || !RU_HAVE_UBO_BIND)
		return;

	idx = glGetUniformBlockIndex_fp (program, "PerFrame");
	if (idx != GL_INVALID_INDEX)
		glUniformBlockBinding_fp (program, idx, RU_BINDING_PERFRAME);

	idx = glGetUniformBlockIndex_fp (program, "PerDraw");
	if (idx != GL_INVALID_INDEX)
		glUniformBlockBinding_fp (program, idx, RU_BINDING_PERDRAW);

	if (!layout_checked)
	{
		layout_checked = true;
		RU_CheckBlockSize (program, "PerFrame", sizeof(r_perframe_t));
		RU_CheckBlockSize (program, "PerDraw", sizeof(r_perdraw_t));
	}
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void R_Uniforms_Init (void)
{
	float ident[16];

	memset (&perframe, 0, sizeof(perframe));
	memset (&perdraw, 0, sizeof(perdraw));
	layout_checked = false;

	/* GL zero-initialises nothing useful here.  An all-zero model matrix
	 * collapses every vertex to world (0,0), which is what the old
	 * per-program identity upload in GL_InitProgram existed to prevent
	 * (uhexen2-0gn3); the mvp and modelview slots get overwritten before
	 * any draw, but start sane for the same reason. */
	Mat4_Identity (ident);
	memcpy (perdraw.mvp, ident, sizeof(ident));
	memcpy (perdraw.modelview, ident, sizeof(ident));
	memcpy (perdraw.model, ident, sizeof(ident));
	memcpy (perframe.viewproj, ident, sizeof(ident));

	glGenBuffers_fp (1, &ubo_perframe);
	glGenBuffers_fp (1, &ubo_perdraw);

	GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_perframe);
	glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(perframe), &perframe, GL_STREAM_DRAW);
	GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_perdraw);
	glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(perdraw), &perdraw, GL_STREAM_DRAW);

	GL_BindBufferBase (GL_UNIFORM_BUFFER, RU_BINDING_PERFRAME, ubo_perframe);
	GL_BindBufferBase (GL_UNIFORM_BUFFER, RU_BINDING_PERDRAW, ubo_perdraw);

	perframe_dirty = false;
	perdraw_dirty = false;
}

void R_Uniforms_Shutdown (void)
{
	if (ubo_perframe)
	{
		glDeleteBuffers_fp (1, &ubo_perframe);
		ubo_perframe = 0;
	}
	if (ubo_perdraw)
	{
		glDeleteBuffers_fp (1, &ubo_perdraw);
		ubo_perdraw = 0;
	}
	layout_checked = false;
}
