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

/* GPU objects */
static GLuint	imm_vao;
static GLuint	imm_vbo;

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

	/* create streaming VBO */
	glGenBuffers_fp(1, &imm_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, sizeof(imm_buffer), NULL, GL_STREAM_DRAW);

	/* set up vertex attributes */
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
	if (imm_vbo)      { glDeleteBuffers_fp(1, &imm_vbo); imm_vbo = 0; }
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

/* Cached GL state to avoid redundant binds */
static GLuint		imm_active_program;
static qboolean		imm_vao_bound;
static float		imm_cached_fog_density = -1;
static float		imm_cached_fog_color[3] = {-1, -1, -1};

void GL_ImmResetState (void)
{
	imm_active_program = 0;
	imm_vao_bound = false;
	imm_cached_fog_density = -1;
}

void GL_ImmEnd (GLenum mode, const glprogram_t *shader)
{
	float mvp[16];

	if (imm_count == 0)
		return;

	/* bind VAO once, keep bound across draws */
	if (!imm_vao_bound)
	{
		glBindVertexArray_fp(imm_vao);
		glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
		imm_vao_bound = true;
	}

	/* upload vertex data (always needed — streaming buffer) */
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);

	/* activate shader only if changed */
	if (imm_active_program != shader->program)
	{
		glUseProgram_fp(shader->program);
		imm_active_program = shader->program;
		/* force uniform re-upload on shader change */
		imm_cached_fog_density = -1;
	}

	/* MVP always changes (per-entity transforms) */
	GL_GetMVP(mvp);
	if (shader->u_mvp >= 0)
		glUniformMatrix4fv_fp(shader->u_mvp, 1, GL_FALSE, mvp);

	/* modelview for fog distance */
	if (shader->u_modelview >= 0)
	{
		float mv[16];
		GL_GetModelview(mv);
		glUniformMatrix4fv_fp(shader->u_modelview, 1, GL_FALSE, mv);
	}

	/* alpha threshold — only upload if set */
	if (imm_alpha_threshold >= 0.0f && shader->u_alpha_threshold >= 0)
		glUniform1f_fp(shader->u_alpha_threshold, imm_alpha_threshold);

	/* fog uniforms — skip if unchanged since last upload */
	if (shader->u_fog_density >= 0 && r_fog_density != imm_cached_fog_density)
	{
		glUniform1f_fp(shader->u_fog_density, r_fog_density);
		imm_cached_fog_density = r_fog_density;
	}
	if (shader->u_fog_color >= 0 &&
	    (r_fog_color[0] != imm_cached_fog_color[0] ||
	     r_fog_color[1] != imm_cached_fog_color[1] ||
	     r_fog_color[2] != imm_cached_fog_color[2]))
	{
		glUniform3f_fp(shader->u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
		imm_cached_fog_color[0] = r_fog_color[0];
		imm_cached_fog_color[1] = r_fog_color[1];
		imm_cached_fog_color[2] = r_fog_color[2];
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

	/* DON'T unbind VAO/shader — keep them hot for next draw */
	imm_count = 0;
}

/* GL_ImmDraw: upload and draw without touching the shader program.
 * Caller must have already called glUseProgram and set uniforms. */
void GL_ImmDraw (GLenum mode)
{
	if (imm_count == 0)
		return;

	glBindVertexArray_fp(imm_vao);
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);

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
