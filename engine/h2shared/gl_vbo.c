/* gl_vbo.c -- VBO/VAO helpers and streaming immediate-mode replacement
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
#include "gl_shader.h"
#include "gl_uniforms.h"
#include "gl_pipeline.h"
#include "gl_matrix.h"
#include "gl_vbo.h"
#include "gl_sky.h"
#include "gl_postprocess.h"

/* fog globals from gl_fog.c */
extern float r_fog_density;
extern float r_fog_color[3];

/* gl_overbright cvar (gl_rmain.c) — read directly for the u_overbright uniform.
 * Kept here so GL_ImmEnd can push the value alongside other auto-uploaded
 * uniforms.  uhexen2-f29y. */
extern cvar_t gl_overbright;
extern cvar_t r_fullbright;

/* ------------------------------------------------------------------ */
/* Vertex format for the streaming VBO:                                */
/*   pos[3] + texcoord[2] + lmcoord[2] + color[4] = 11 floats         */
/* ------------------------------------------------------------------ */

#define IMM_FLOATS_PER_VERT	11
#define IMM_STRIDE		(IMM_FLOATS_PER_VERT * sizeof(float))

#define IMM_OFF_POS		0
#define IMM_OFF_TEXCOORD	(3 * sizeof(float))
#define IMM_OFF_LMCOORD		(5 * sizeof(float))
#define IMM_OFF_COLOR		(7 * sizeof(float))

typedef struct {
	float	pos[3];
	float	texcoord[2];
	float	lmcoord[2];
	float	color[4];
} immvert_t;

static immvert_t	imm_buffer[GL_IMM_MAX_VERTS];
static int		imm_count;
static float	imm_alpha_threshold = -1.0f;	/* -1 = use shader default */
/* uhexen2-khsa r13: tracks whether the current immediate-mode batch should
 * force fragColor.a = 1.0 (opaque) or preserve color.a (translucent).  Set
 * by GL_SetForceOpaqueAlpha; flushed in GL_ImmEnd. */
static float	imm_force_opaque_alpha = -1.0f;	/* -1 = use shader default */
/* Underwater caustics on alias batches (uhexen2-0gn3).  These have a
 * meaningful "off" value rather than a -1 sentinel: 0
 * intensity is the resting state, and the alias draw paths restore it when
 * they finish so sprites / warp polys / brush polys sharing gl_shader_alias
 * never inherit the effect. */
static float	imm_alias_caustics[2] = { 0.0f, 0.0f };
/* Soft-particle fade for sprite batches (uhexen2-mf9u).  Like the caustics
 * pair above, 0 intensity is the resting state rather than a -1 sentinel, and
 * R_DrawSpriteModel restores it after each sprite so the alias / warp / brush
 * batches that share gl_shader_alias never inherit the fade. */
static float	imm_soft[3] = { 0.0f, 0.0f, 0.0f };
static float	imm_alias_model[16] = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
};

/* Current vertex state (accumulated between calls) */
static float	imm_cur_tc[2];
static float	imm_cur_lm[2];
static float	imm_cur_color[4] = { 1, 1, 1, 1 };

/* GPU objects.  On desktop GL the VBO is gone — vertex data is streamed
 * through GL_Upload's frame ring and bound per-draw via glBindVertexBuffer.
 * On WebGL2 (no ARB_vertex_attrib_binding, no buffer_storage / ring) we
 * keep the original per-draw glBufferData(STREAM_DRAW) path on a
 * dedicated VBO.  uhexen2-y1v5. */
static GLuint	imm_vao;
#ifdef USE_GLES
static GLuint	imm_vbo;
#endif

/* Index buffer for quad-to-triangle conversion */
#define IMM_MAX_QUADS		(GL_IMM_MAX_VERTS / 4)
#define IMM_MAX_QUAD_INDICES	(IMM_MAX_QUADS * 6)
static GLuint	imm_quad_ibo;

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                     */
/* ------------------------------------------------------------------ */

void GL_VBO_Init (void)
{
	unsigned short *indices;
	int i;

	/* create VAO */
	glGenVertexArrays_fp(1, &imm_vao);
	glBindVertexArray_fp(imm_vao);

#ifdef USE_GLES
	/* WebGL2: dedicated VBO; attributes baked to it via VertexAttribPointer. */
	glGenBuffers_fp(1, &imm_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, sizeof(imm_buffer), NULL, GL_STREAM_DRAW);

	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribPointer_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_POS);
	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribPointer_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_TEXCOORD);
	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribPointer_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_LMCOORD);
	glEnableVertexAttribArray_fp(ATTR_COLOR);
	glVertexAttribPointer_fp(ATTR_COLOR, 4, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_COLOR);
#else
	/* Desktop GL 4.3: separate vertex attribute bindings.  Format is
	 * recorded in the VAO once; the source buffer + offset is rebound
	 * per draw via glBindVertexBuffer(0, ring_buf, ring_ofs, stride). */
	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribFormat_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, IMM_OFF_POS);
	glVertexAttribBinding_fp(ATTR_POSITION, 0);

	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribFormat_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, IMM_OFF_TEXCOORD);
	glVertexAttribBinding_fp(ATTR_TEXCOORD, 0);

	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribFormat_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE, IMM_OFF_LMCOORD);
	glVertexAttribBinding_fp(ATTR_LMCOORD, 0);

	glEnableVertexAttribArray_fp(ATTR_COLOR);
	glVertexAttribFormat_fp(ATTR_COLOR, 4, GL_FLOAT, GL_FALSE, IMM_OFF_COLOR);
	glVertexAttribBinding_fp(ATTR_COLOR, 0);
#endif

	/* create index buffer for quad-to-triangle conversion */
	glGenBuffers_fp(1, &imm_quad_ibo);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
	indices = (unsigned short *) malloc(IMM_MAX_QUAD_INDICES * sizeof(unsigned short));
	if (!indices)
		Sys_Error("GL_VBO_Init: out of memory allocating quad IBO scratch buffer");
	for (i = 0; i < IMM_MAX_QUADS; i++)
	{
		indices[i*6 + 0] = i*4 + 0;
		indices[i*6 + 1] = i*4 + 1;
		indices[i*6 + 2] = i*4 + 2;
		indices[i*6 + 3] = i*4 + 0;
		indices[i*6 + 4] = i*4 + 2;
		indices[i*6 + 5] = i*4 + 3;
	}
	glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER,
			 IMM_MAX_QUAD_INDICES * sizeof(unsigned short),
			 indices, GL_STATIC_DRAW);
	free(indices);

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);

	Con_SafePrintf("GL_VBO: streaming VBO initialized (%d max verts)\n",
		       GL_IMM_MAX_VERTS);
}

void GL_VBO_Shutdown (void)
{
	if (imm_quad_ibo) { glDeleteBuffers_fp(1, &imm_quad_ibo); imm_quad_ibo = 0; }
#ifdef USE_GLES
	if (imm_vbo)      { glDeleteBuffers_fp(1, &imm_vbo); imm_vbo = 0; }
#endif
	if (imm_vao)      { glDeleteVertexArrays_fp(1, &imm_vao); imm_vao = 0; }
}

/* ------------------------------------------------------------------ */
/* Immediate-mode replacement API                                      */
/* ------------------------------------------------------------------ */

int GL_ImmCount (void)
{
	return imm_count;
}

void GL_ImmBegin (void)
{
	imm_count = 0;
}

void GL_ImmTexCoord2f (float s, float t)
{
	imm_cur_tc[0] = s;
	imm_cur_tc[1] = t;
}

void GL_ImmLMCoord2f (float s, float t)
{
	imm_cur_lm[0] = s;
	imm_cur_lm[1] = t;
}

void GL_SetAlphaThreshold (float threshold)
{
	imm_alpha_threshold = threshold;
}

float GL_GetAlphaThreshold (void)
{
	return imm_alpha_threshold;
}

/* uhexen2-khsa r13.  Pass 1.0 for opaque draws (force fragColor.a=1.0 so
 * downstream consumers of FB.a aren't confused), 0.0 to preserve color.a
 * (needed for ENTALPHA / DRF_TRANSLUCENT immediate-mode batches). */
void GL_SetForceOpaqueAlpha (float v)
{
	imm_force_opaque_alpha = v;
}

/* uhexen2-0gn3: intensity 0 disables the caustics overlay entirely. */
void GL_SetAliasCaustics (float intensity, float time)
{
	imm_alias_caustics[0] = intensity;
	imm_alias_caustics[1] = time;
}

/* uhexen2-mf9u.  inv_dist 0 disables the fade entirely; za/zb are the
 * window-depth -> view-distance coefficients for the live projection. */
void GL_SetSoftParticles (float inv_dist, float za, float zb)
{
	imm_soft[0] = inv_dist;
	imm_soft[1] = za;
	imm_soft[2] = zb;
}

void GL_SetAliasModelMatrix (const float *m)
{
	memcpy(imm_alias_model, m, sizeof(imm_alias_model));
}

void GL_ImmColor4f (float r, float g, float b, float a)
{
	imm_cur_color[0] = r;
	imm_cur_color[1] = g;
	imm_cur_color[2] = b;
	imm_cur_color[3] = a;
}

void GL_ImmColor3f (float r, float g, float b)
{
	imm_cur_color[0] = r;
	imm_cur_color[1] = g;
	imm_cur_color[2] = b;
	imm_cur_color[3] = 1.0f;
}

void GL_ImmColor4ubv (const unsigned char *c)
{
	imm_cur_color[0] = c[0] / 255.0f;
	imm_cur_color[1] = c[1] / 255.0f;
	imm_cur_color[2] = c[2] / 255.0f;
	imm_cur_color[3] = c[3] / 255.0f;
}

void GL_ImmColor3ubv (const unsigned char *c)
{
	imm_cur_color[0] = c[0] / 255.0f;
	imm_cur_color[1] = c[1] / 255.0f;
	imm_cur_color[2] = c[2] / 255.0f;
	imm_cur_color[3] = 1.0f;
}

void GL_ImmVertex3f (float x, float y, float z)
{
	immvert_t *v;

	if (imm_count >= GL_IMM_MAX_VERTS)
		return;

	v = &imm_buffer[imm_count++];
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = z;
	v->texcoord[0] = imm_cur_tc[0];
	v->texcoord[1] = imm_cur_tc[1];
	v->lmcoord[0] = imm_cur_lm[0];
	v->lmcoord[1] = imm_cur_lm[1];
	v->color[0] = imm_cur_color[0];
	v->color[1] = imm_cur_color[1];
	v->color[2] = imm_cur_color[2];
	v->color[3] = imm_cur_color[3];
}

void GL_ImmVertex2f (float x, float y)
{
	GL_ImmVertex3f(x, y, 0.0f);
}

/* ------------------------------------------------------------------ */
/* Flush / draw                                                        */
/* ------------------------------------------------------------------ */

/* GL_QUADS is not available in GLES / GL core.  We convert quads to
 * triangles using the pre-built index buffer.  (The enum itself comes from
 * glheader.h, which shims it for GLES3.) */

/* The per-shader uniform cache that used to live here is gone.  It existed
 * because uniform locations are per-program, so every value had to be tracked
 * per shader and re-pushed on a program switch -- and it had to be invalidated
 * by hand whenever anything outside GL_ImmEnd touched a uniform.  The std140
 * blocks in gl_uniforms.c are program-independent, so the same value set twice
 * costs nothing and a program switch costs nothing, and the setters do their
 * own change detection.  uhexen2-p4ln.2. */

void GL_ImmResetState (void)
{
	/* no-op — kept for API compat */
}

/* Force the cache to miss on the next GL_ImmEnd. Call after any
 * external glUseProgram / glUniform / matrix manipulation that
 * GL_ImmEnd doesn't see, or after a vid_restart that invalidates
 * shader handles. */
void GL_ImmInvalidateState (void)
{
	R_InvalidateUniforms ();
}

void GL_ImmEnd (GLenum mode, const glprogram_t *shader)
{
	float mvp[16];

	if (imm_count == 0)
		return;

	/* Bind VAO and stream vertex data.  Desktop GL 4.3 routes through the
	 * frame ring (GL_Upload returns buf+offset); WebGL2 falls back to a
	 * dedicated VBO orphaned each frame via glBufferData(STREAM_DRAW). */
	glBindVertexArray_fp(imm_vao);
#ifdef USE_GLES
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);
#else
	{
		GLuint   _imm_buf;
		GLintptr _imm_ofs;
		GL_Upload (GL_ARRAY_BUFFER, imm_buffer,
			   imm_count * sizeof(immvert_t),
			   &_imm_buf, &_imm_ofs);
		glBindVertexBuffer_fp(0, _imm_buf, _imm_ofs, IMM_STRIDE);
	}
#endif

	R_UseProgram (shader->program);

	/* Push what this batch needs.  Every setter below folds away when the
	 * value is unchanged, and unlike the old per-shader cache it does so
	 * across program switches too -- the blocks are shared, so a value set
	 * for the world shader is already correct for the alias shader.
	 * R_DrawElements / R_DrawArrays upload whatever ended up dirty. */
	GL_GetMVP(mvp);
	R_SetMVP (mvp);
	{
		float mv[16];
		GL_GetModelview(mv);
		R_SetModelView (mv);
	}
	if (imm_alpha_threshold >= 0.0f)
		R_SetAlphaThresholdU (imm_alpha_threshold);
	if (imm_force_opaque_alpha >= 0.0f)
		R_SetForceOpaqueAlphaU (imm_force_opaque_alpha);
	R_SetAliasCaustics (imm_alias_caustics[0], imm_alias_caustics[1]);	/* uhexen2-0gn3 */
	R_SetSoftParams (imm_soft[0], imm_soft[1], imm_soft[2]);		/* uhexen2-mf9u */
	R_SetModelMatrix (imm_alias_model);
	R_SetFog (r_fog_density, r_fog_color);
	R_SetFrameTime (cl.time);
	R_SetEyePos (r_origin);
	R_SetSkyWind (sky_wind_uv[0], sky_wind_uv[1]);
	/* Same gate as R_SetupFrame: r_fullbright already replaces the lightmap
	 * sample with white, so doubling it blows every surface to pure white.
	 * uhexen2-isq7. */
	R_SetOverbright ((gl_overbright.integer && !r_fullbright.integer) ? 2.0f : 1.0f);

	/* A global glEnable(GL_BLEND) in the translucent draw paths resets the
	 * per-buffer blend funcs to the global default on some drivers, which
	 * breaks the WBOIT revealage buffer (geometry vanishes).  Re-assert the
	 * OIT blend funcs immediately before every draw inside the OIT pass.
	 *
	 * These are re-asserts, so they look exactly like the redundant calls
	 * gl_pipeline.c exists to drop.  They survive because R_SetBlend marks
	 * the indexed funcs unknown whenever it actually issues glEnable(GL_BLEND)
	 * -- the only thing that can clobber them -- so the pair below reaches GL
	 * on precisely the draws that need it and folds away on the rest. */
	if (OIT_InPass())
	{
		R_SetBlendFuncIndexed (0, GL_ONE, GL_ONE);
		R_SetBlendFuncIndexed (1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
	}

	/* draw */
	if (mode == GL_QUADS)
	{
		int num_quads = imm_count / 4;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
		R_DrawElements (GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		R_DrawArrays (GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		R_DrawArrays (mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);
	R_UseProgram (0);

	/* ensure texture unit 0 is active after draw */
	if (glActiveTexture_fp)
		glActiveTexture_fp(GL_TEXTURE0);

	imm_count = 0;
}

/* GL_ImmDraw: upload and draw without touching the shader program.
 * Caller must have already called glUseProgram and set uniforms. */
void GL_ImmDraw (GLenum mode)
{
	if (imm_count == 0)
		return;

	glBindVertexArray_fp(imm_vao);
#ifdef USE_GLES
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);
#else
	{
		GLuint   _imm_buf;
		GLintptr _imm_ofs;
		GL_Upload (GL_ARRAY_BUFFER, imm_buffer,
			   imm_count * sizeof(immvert_t),
			   &_imm_buf, &_imm_ofs);
		glBindVertexBuffer_fp(0, _imm_buf, _imm_ofs, IMM_STRIDE);
	}
#endif

	if (mode == GL_QUADS)
	{
		int num_quads = imm_count / 4;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
		R_DrawElements (GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		R_DrawArrays (GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		R_DrawArrays (mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);

	imm_count = 0;
}
