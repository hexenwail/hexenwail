/* gl_uniforms.c -- the two std140 uniform blocks shared by every file-backed
 * draw program.  See gl_uniforms.h for the layout and the reasoning.
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
#include "gl_shader.h"		/* UBO_BINDING_VERT / UBO_BINDING_FRAG */
#include "gl_uniforms.h"

#ifndef GL_UNIFORM_BLOCK_DATA_SIZE
#define GL_UNIFORM_BLOCK_DATA_SIZE	0x8A40
#endif

/* On the ES tier these are real symbols rather than loader-table pointers, so
 * they cannot be tested as values at all -- the comparison is not merely
 * always-true, it does not compile as written. */
#ifdef USE_GLES
#define RU_HAVE_UBO_BIND	1
#define RU_HAVE_UBO_QUERY	1
#else
#define RU_HAVE_UBO_BIND	(glGetUniformBlockIndex_fp != NULL && \
				 glUniformBlockBinding_fp != NULL)
#define RU_HAVE_UBO_QUERY	(glGetActiveUniformBlockiv_fp != NULL)
#endif

#define RU_NAME_VERT	"VertParams"
#define RU_NAME_FRAG	"FragParams"

static GLuint		ubo_vert;
static GLuint		ubo_frag;

static r_vertparams_t	vertparams;
static r_fragparams_t	fragparams;

static qboolean		vert_dirty;
static qboolean		frag_dirty;
static qboolean		bindings_dirty;
static qboolean		layout_checked;

/* The mirrors are only trustworthy if they are the size std140 says they are.
 * Both are built from vec4/mat4/ivec4 only, so this is arithmetic rather than
 * hope: 4 mat4 + 5 vec4 + 1 ivec4, and 8 vec4. */
COMPILE_TIME_ASSERT(vertparams_size, sizeof(r_vertparams_t) == 4 * 64 + 6 * 16);
COMPILE_TIME_ASSERT(fragparams_size, sizeof(r_fragparams_t) == 8 * 16);

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
/* VertParams setters                                                  */
/* ------------------------------------------------------------------ */

void R_SetMVP (const float *m16)
{
	if (RU_SetMat(vertparams.mvp, m16))
		vert_dirty = true;
}

void R_SetModelView (const float *m16)
{
	if (RU_SetMat(vertparams.modelview, m16))
		vert_dirty = true;
}

void R_SetModelMatrix (const float *m16)
{
	if (RU_SetMat(vertparams.model, m16))
		vert_dirty = true;
}

void R_SetViewProj (const float *m16)
{
	if (RU_SetMat(vertparams.viewproj, m16))
		vert_dirty = true;
}

void R_SetParticleBasis (const float *pup, const float *pright,
			 const float *vpn, const float *origin, float ptime)
{
	int changed = 0;

	changed |= RU_Set4(vertparams.pup, pup[0], pup[1], pup[2], 0.0f);
	changed |= RU_Set4(vertparams.pright, pright[0], pright[1], pright[2], 0.0f);
	changed |= RU_Set4(vertparams.vpn, vpn[0], vpn[1], vpn[2], 0.0f);
	changed |= RU_Set4(vertparams.porigin, origin[0], origin[1], origin[2], ptime);
	if (changed)
		vert_dirty = true;
}

void R_SetPoseBase (int v)
{
	if (vertparams.posei[0] != v)
	{
		vertparams.posei[0] = v;
		vert_dirty = true;
	}
}

void R_SetInstBase (int v)
{
	if (vertparams.posei[1] != v)
	{
		vertparams.posei[1] = v;
		vert_dirty = true;
	}
}

void R_SetPoseVertType (int v)
{
	if (vertparams.posei[2] != v)
	{
		vertparams.posei[2] = v;
		vert_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* Duplicated into both blocks                                         */
/* ------------------------------------------------------------------ */

void R_SetEyePos (const float *xyz)
{
	if (RU_Set4(vertparams.scene, xyz[0], xyz[1], xyz[2], vertparams.scene[3]))
		vert_dirty = true;
	if (RU_Set4(fragparams.scene, xyz[0], xyz[1], xyz[2], fragparams.scene[3]))
		frag_dirty = true;
}

void R_SetFrameTime (float t)
{
	if (vertparams.scene[3] != t)
	{
		vertparams.scene[3] = t;
		vert_dirty = true;
	}
	if (fragparams.scene[3] != t)
	{
		fragparams.scene[3] = t;
		frag_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* FragParams setters                                                  */
/* ------------------------------------------------------------------ */

void R_SetFog (float density, const float *rgb)
{
	if (RU_Set4(fragparams.fog, rgb[0], rgb[1], rgb[2], density))
		frag_dirty = true;
}

void R_SetAlphaMask (float threshold, float force_opaque)
{
	if (fragparams.mask[0] != threshold || fragparams.mask[1] != force_opaque)
	{
		fragparams.mask[0] = threshold;
		fragparams.mask[1] = force_opaque;
		frag_dirty = true;
	}
}

void R_SetOverbright (float v)
{
	if (fragparams.mask[2] != v)
	{
		fragparams.mask[2] = v;
		frag_dirty = true;
	}
}

void R_SetLightmapBicubic (float v)
{
	if (fragparams.mask[3] != v)
	{
		fragparams.mask[3] = v;
		frag_dirty = true;
	}
}

void R_SetWorldCaustics (float intensity, float time)
{
	if (fragparams.caustics[0] != intensity || fragparams.caustics[1] != time)
	{
		fragparams.caustics[0] = intensity;
		fragparams.caustics[1] = time;
		frag_dirty = true;
	}
}

void R_SetAliasCausticsU (float intensity, float time)
{
	if (fragparams.caustics[2] != intensity || fragparams.caustics[3] != time)
	{
		fragparams.caustics[2] = intensity;
		fragparams.caustics[3] = time;
		frag_dirty = true;
	}
}

void R_SetSoftParams (float x, float y, float z)
{
	if (RU_Set4(fragparams.soft, x, y, z, 0.0f))
		frag_dirty = true;
}

void R_SetSkyFog (float r, float g, float b, float a)
{
	if (RU_Set4(fragparams.skyfog, r, g, b, a))
		frag_dirty = true;
}

void R_SetSkyWind (float u, float v)
{
	if (RU_Set4(fragparams.sky, u, v, 0.0f, 0.0f))
		frag_dirty = true;
}

void R_SetLightDebug (float fullbright, float lightmapdbg)
{
	if (RU_Set4(fragparams.debug, fullbright, lightmapdbg, 0.0f, 0.0f))
		frag_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Upload                                                              */
/* ------------------------------------------------------------------ */

/*
===============
R_FlushUniforms

One push per dirty block, whole block, called immediately before the draw that
reads it.

The buffers are permanent and bound once with GL_BindBufferBase, rather than
streamed through GL_Upload's frame ring the way vertex data is.  That is what
makes skipping an unchanged block safe: a ring range that was bound two draws
ago may since have been handed to somebody else, so "nothing changed, so do not
push" would be reading recycled storage.  A named buffer still holds what was
last written to it however long ago that was.  The ring is also a no-op stub on
the ES tier (gl_buffer.c), which would have made this draw nothing at all in
the browser and on macOS.

glBufferData rather than glBufferSubData: respecifying the whole store orphans
the old one, so a flush never waits on a draw still reading the previous
contents.  Both blocks are a few hundred bytes, so the respecify costs nothing
next to the stall it avoids.
===============
*/
void R_FlushUniforms (void)
{
	if (bindings_dirty)
	{
		bindings_dirty = false;
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_VERT, ubo_vert);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_FRAG, ubo_frag);
	}

	if (vert_dirty)
	{
		vert_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_vert);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(vertparams), &vertparams,
				 GL_STREAM_DRAW);
	}
	if (frag_dirty)
	{
		frag_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_frag);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(fragparams), &fragparams,
				 GL_STREAM_DRAW);
	}
}

void R_InvalidateUniforms (void)
{
	vert_dirty = true;
	frag_dirty = true;
	bindings_dirty = true;
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
	GLuint	idx;
	GLint	size = 0;

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
	else
		Con_SafePrintf ("  uniform block %s: %d bytes\n", name, (int)size);
}

static GLint RU_BindOne (GLuint program, const char *name, GLuint binding)
{
	GLuint	index = glGetUniformBlockIndex_fp (program, name);

	if (index == GL_INVALID_INDEX)
		return -1;
	glUniformBlockBinding_fp (program, index, binding);
	return (GLint) index;
}

void R_BindProgramBlocks (GLuint program, GLint *out_vert, GLint *out_frag)
{
	GLint	vidx = -1, fidx = -1;

	if (program && RU_HAVE_UBO_BIND)
	{
		vidx = RU_BindOne (program, RU_NAME_VERT, UBO_BINDING_VERT);
		fidx = RU_BindOne (program, RU_NAME_FRAG, UBO_BINDING_FRAG);

		/* First program that declares either block is the one we ask
		 * about; a size the driver disagrees with is a property of the
		 * layout, not of the program. */
		if (!layout_checked && (vidx >= 0 || fidx >= 0))
		{
			layout_checked = true;
			RU_CheckBlockSize (program, RU_NAME_VERT, sizeof(r_vertparams_t));
			RU_CheckBlockSize (program, RU_NAME_FRAG, sizeof(r_fragparams_t));
		}
	}
	else if (program)
	{
		static qboolean warned = false;

		if (!warned)
		{
			warned = true;
			Con_Printf ("[SHADER] no uniform block entry points; "
				    "block-backed shaders will not render\n");
		}
	}

	if (out_vert) *out_vert = vidx;
	if (out_frag) *out_frag = fidx;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void R_Uniforms_Init (void)
{
	float	ident[16];

	memset (&vertparams, 0, sizeof(vertparams));
	memset (&fragparams, 0, sizeof(fragparams));
	layout_checked = false;

	/* GL zero-initialises nothing useful here.  An all-zero model matrix
	 * collapses every vertex to world (0,0), which is what the per-program
	 * identity upload in GL_InitProgram exists to prevent (uhexen2-0gn3);
	 * mvp and modelview are overwritten before any draw, but start sane for
	 * the same reason. */
	Mat4_Identity (ident);
	memcpy (vertparams.mvp, ident, sizeof(ident));
	memcpy (vertparams.modelview, ident, sizeof(ident));
	memcpy (vertparams.model, ident, sizeof(ident));
	memcpy (vertparams.viewproj, ident, sizeof(ident));

	glGenBuffers_fp (1, &ubo_vert);
	glGenBuffers_fp (1, &ubo_frag);

	/* Seed both stores before anything binds them, so a program that draws
	 * before its first setter call reads zeros and an identity rather than
	 * an unspecified store. */
	vert_dirty = true;
	frag_dirty = true;
	bindings_dirty = true;
	R_FlushUniforms ();
}

void R_Uniforms_Shutdown (void)
{
	if (ubo_vert)
	{
		glDeleteBuffers_fp (1, &ubo_vert);
		ubo_vert = 0;
	}
	if (ubo_frag)
	{
		glDeleteBuffers_fp (1, &ubo_frag);
		ubo_frag = 0;
	}
	layout_checked = false;
	bindings_dirty = true;
}
