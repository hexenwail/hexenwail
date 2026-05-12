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
#include "gl_matrix.h"
#include "gl_vbo.h"
#include "gl_sky.h"

/* fog globals from gl_fog.c */
extern float r_fog_density;
extern float r_fog_color[3];

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
#ifdef __EMSCRIPTEN__
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

#ifdef __EMSCRIPTEN__
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
#ifdef __EMSCRIPTEN__
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
 * triangles using the pre-built index buffer. */

#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0x0009
#endif

/* Per-shader uniform cache: most HUD/2D draws share identical fog,
 * alpha threshold, and MVP. Driver-side glUniform calls aren't free —
 * skipping the redundant uploads cuts ~100 calls/frame on a typical
 * scene with HUD + center print + several pic draws. The cache is
 * invalidated when the shader changes (different uniform locations)
 * and via GL_ImmInvalidateState() at context boundaries. */
static const glprogram_t *imm_cache_shader;
static float	imm_cache_mvp[16];
static float	imm_cache_mv[16];
static float	imm_cache_alpha = -2.0f;
static float	imm_cache_fog_density = -1.0f;
static float	imm_cache_fog_color[3] = { -1.0f, -1.0f, -1.0f };
static float	imm_cache_time = -1.0f;
static float	imm_cache_eyepos[3] = { -99999.0f, -99999.0f, -99999.0f };
static float	imm_cache_wind[2] = { -99999.0f, -99999.0f };
static qboolean	imm_cache_mvp_set;
static qboolean	imm_cache_mv_set;

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
	imm_cache_shader = NULL;
	imm_cache_alpha = -2.0f;
	imm_cache_fog_density = -1.0f;
	imm_cache_fog_color[0] = imm_cache_fog_color[1] = imm_cache_fog_color[2] = -1.0f;
	imm_cache_time = -1.0f;
	imm_cache_eyepos[0] = imm_cache_eyepos[1] = imm_cache_eyepos[2] = -99999.0f;
	imm_cache_wind[0] = imm_cache_wind[1] = -99999.0f;
	imm_cache_mvp_set = false;
	imm_cache_mv_set = false;
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
#ifdef __EMSCRIPTEN__
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

	/* activate shader; reset uniform cache when the program changes
	 * because uniform locations are per-program */
	glUseProgram_fp(shader->program);
	if (shader != imm_cache_shader)
	{
		imm_cache_shader = shader;
		imm_cache_alpha = -2.0f;
		imm_cache_fog_density = -1.0f;
		imm_cache_fog_color[0] = imm_cache_fog_color[1] = imm_cache_fog_color[2] = -1.0f;
		imm_cache_time = -1.0f;
		imm_cache_eyepos[0] = imm_cache_eyepos[1] = imm_cache_eyepos[2] = -99999.0f;
		imm_cache_wind[0] = imm_cache_wind[1] = -99999.0f;
		imm_cache_mvp_set = false;
		imm_cache_mv_set = false;
	}

	GL_GetMVP(mvp);
	if (shader->u_mvp >= 0 &&
	    (!imm_cache_mvp_set || memcmp(mvp, imm_cache_mvp, sizeof(mvp)) != 0))
	{
		glUniformMatrix4fv_fp(shader->u_mvp, 1, GL_FALSE, mvp);
		memcpy(imm_cache_mvp, mvp, sizeof(mvp));
		imm_cache_mvp_set = true;
	}

	if (shader->u_modelview >= 0)
	{
		float mv[16];
		GL_GetModelview(mv);
		if (!imm_cache_mv_set || memcmp(mv, imm_cache_mv, sizeof(mv)) != 0)
		{
			glUniformMatrix4fv_fp(shader->u_modelview, 1, GL_FALSE, mv);
			memcpy(imm_cache_mv, mv, sizeof(mv));
			imm_cache_mv_set = true;
		}
	}

	if (imm_alpha_threshold >= 0.0f && shader->u_alpha_threshold >= 0 &&
	    imm_alpha_threshold != imm_cache_alpha)
	{
		glUniform1f_fp(shader->u_alpha_threshold, imm_alpha_threshold);
		imm_cache_alpha = imm_alpha_threshold;
	}

	if (shader->u_fog_density >= 0 && r_fog_density != imm_cache_fog_density)
	{
		glUniform1f_fp(shader->u_fog_density, r_fog_density);
		imm_cache_fog_density = r_fog_density;
	}
	if (shader->u_fog_color >= 0 &&
	    (r_fog_color[0] != imm_cache_fog_color[0] ||
	     r_fog_color[1] != imm_cache_fog_color[1] ||
	     r_fog_color[2] != imm_cache_fog_color[2]))
	{
		glUniform3f_fp(shader->u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
		imm_cache_fog_color[0] = r_fog_color[0];
		imm_cache_fog_color[1] = r_fog_color[1];
		imm_cache_fog_color[2] = r_fog_color[2];
	}

	if (shader->u_time >= 0 && cl.time != imm_cache_time)
	{
		glUniform1f_fp(shader->u_time, cl.time);
		imm_cache_time = cl.time;
	}

	if (shader->u_eyepos >= 0 &&
	    (r_origin[0] != imm_cache_eyepos[0] ||
	     r_origin[1] != imm_cache_eyepos[1] ||
	     r_origin[2] != imm_cache_eyepos[2]))
	{
		glUniform3f_fp(shader->u_eyepos, r_origin[0], r_origin[1], r_origin[2]);
		imm_cache_eyepos[0] = r_origin[0];
		imm_cache_eyepos[1] = r_origin[1];
		imm_cache_eyepos[2] = r_origin[2];
	}

	if (shader->u_wind >= 0 &&
	    (sky_wind_uv[0] != imm_cache_wind[0] ||
	     sky_wind_uv[1] != imm_cache_wind[1]))
	{
		glUniform2f_fp(shader->u_wind, sky_wind_uv[0], sky_wind_uv[1]);
		imm_cache_wind[0] = sky_wind_uv[0];
		imm_cache_wind[1] = sky_wind_uv[1];
	}

	/* draw */
	if (mode == GL_QUADS)
	{
		int num_quads = imm_count / 4;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
		glDrawElements_fp(GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		glDrawArrays_fp(GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		glDrawArrays_fp(mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);
	glUseProgram_fp(0);

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
#ifdef __EMSCRIPTEN__
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
		glDrawElements_fp(GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		glDrawArrays_fp(GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		glDrawArrays_fp(mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);

	imm_count = 0;
}
