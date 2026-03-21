/* gl_matrix.c -- software matrix stack (replaces fixed-function pipeline)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "gl_matrix.h"
#include <string.h>
#include <math.h>

/* Column-major 4x4 matrices, OpenGL convention.
 * Index: m[col*4 + row]
 *
 *  [ m[0]  m[4]  m[8]   m[12] ]
 *  [ m[1]  m[5]  m[9]   m[13] ]
 *  [ m[2]  m[6]  m[10]  m[14] ]
 *  [ m[3]  m[7]  m[11]  m[15] ]
 */

#define MAT_STACK_DEPTH	8

static float	mat_modelview[MAT_STACK_DEPTH][16];
static float	mat_projection[MAT_STACK_DEPTH][16];
static int	mat_mv_depth;
static int	mat_proj_depth;

static int	mat_current_mode = GL_MAT_MODELVIEW;

/* ------------------------------------------------------------------ */
/* 4x4 math                                                            */
/* ------------------------------------------------------------------ */

void Mat4_Identity (float *out)
{
	memset(out, 0, 16 * sizeof(float));
	out[0] = out[5] = out[10] = out[15] = 1.0f;
}

void Mat4_Copy (const float *src, float *dst)
{
	memcpy(dst, src, 16 * sizeof(float));
}

void Mat4_Multiply (const float *a, const float *b, float *out)
{
	float tmp[16];
	int i, j;

	for (i = 0; i < 4; i++)
	{
		for (j = 0; j < 4; j++)
		{
			tmp[i*4 + j] = a[0*4 + j] * b[i*4 + 0]
				     + a[1*4 + j] * b[i*4 + 1]
				     + a[2*4 + j] * b[i*4 + 2]
				     + a[3*4 + j] * b[i*4 + 3];
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

/* ------------------------------------------------------------------ */
/* Stack helpers                                                       */
/* ------------------------------------------------------------------ */

static float *current_matrix (void)
{
	if (mat_current_mode == GL_MAT_PROJECTION)
		return mat_projection[mat_proj_depth];
	return mat_modelview[mat_mv_depth];
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void GL_MatrixMode (int mode)
{
	mat_current_mode = mode;
}

void GL_LoadIdentity (void)
{
	Mat4_Identity(current_matrix());
}

void GL_PushMatrix (void)
{
	if (mat_current_mode == GL_MAT_PROJECTION)
	{
		if (mat_proj_depth < MAT_STACK_DEPTH - 1)
		{
			Mat4_Copy(mat_projection[mat_proj_depth], mat_projection[mat_proj_depth + 1]);
			mat_proj_depth++;
		}
	}
	else
	{
		if (mat_mv_depth < MAT_STACK_DEPTH - 1)
		{
			Mat4_Copy(mat_modelview[mat_mv_depth], mat_modelview[mat_mv_depth + 1]);
			mat_mv_depth++;
		}
	}
}

void GL_PopMatrix (void)
{
	if (mat_current_mode == GL_MAT_PROJECTION)
	{
		if (mat_proj_depth > 0)
			mat_proj_depth--;
	}
	else
	{
		if (mat_mv_depth > 0)
			mat_mv_depth--;
	}
}

void GL_LoadMatrixf (const float *m)
{
	Mat4_Copy(m, current_matrix());
}

void GL_Translatef (float x, float y, float z)
{
	float t[16];

	Mat4_Identity(t);
	t[12] = x;
	t[13] = y;
	t[14] = z;

	/* current = current * t */
	Mat4_Multiply(current_matrix(), t, current_matrix());
}

void GL_Rotatef (float angle, float x, float y, float z)
{
	float	r[16];
	float	s, c, ic;
	float	len, ilength;

	len = (float)sqrt(x*x + y*y + z*z);
	if (len == 0.0f)
		return;
	ilength = 1.0f / len;
	x *= ilength;
	y *= ilength;
	z *= ilength;

	s = (float)sin(angle * M_PI / 180.0);
	c = (float)cos(angle * M_PI / 180.0);
	ic = 1.0f - c;

	r[0]  = x*x*ic + c;
	r[1]  = y*x*ic + z*s;
	r[2]  = z*x*ic - y*s;
	r[3]  = 0.0f;

	r[4]  = x*y*ic - z*s;
	r[5]  = y*y*ic + c;
	r[6]  = z*y*ic + x*s;
	r[7]  = 0.0f;

	r[8]  = x*z*ic + y*s;
	r[9]  = y*z*ic - x*s;
	r[10] = z*z*ic + c;
	r[11] = 0.0f;

	r[12] = 0.0f;
	r[13] = 0.0f;
	r[14] = 0.0f;
	r[15] = 1.0f;

	Mat4_Multiply(current_matrix(), r, current_matrix());
}

void GL_Scalef (float x, float y, float z)
{
	float *m = current_matrix();

	m[0] *= x;  m[1] *= x;  m[2] *= x;  m[3] *= x;
	m[4] *= y;  m[5] *= y;  m[6] *= y;  m[7] *= y;
	m[8] *= z;  m[9] *= z;  m[10] *= z; m[11] *= z;
}

void GL_Ortho (double left, double right, double bottom, double top, double znear, double zfar)
{
	float o[16];

	memset(o, 0, sizeof(o));
	o[0]  = (float)(2.0 / (right - left));
	o[5]  = (float)(2.0 / (top - bottom));
	o[10] = (float)(-2.0 / (zfar - znear));
	o[12] = (float)(-(right + left) / (right - left));
	o[13] = (float)(-(top + bottom) / (top - bottom));
	o[14] = (float)(-(zfar + znear) / (zfar - znear));
	o[15] = 1.0f;

	Mat4_Multiply(current_matrix(), o, current_matrix());
}

void GL_Frustum (double left, double right, double bottom, double top, double znear, double zfar)
{
	float f[16];

	memset(f, 0, sizeof(f));
	f[0]  = (float)(2.0 * znear / (right - left));
	f[5]  = (float)(2.0 * znear / (top - bottom));
	f[8]  = (float)((right + left) / (right - left));
	f[9]  = (float)((top + bottom) / (top - bottom));
	f[10] = (float)(-(zfar + znear) / (zfar - znear));
	f[11] = -1.0f;
	f[14] = (float)(-2.0 * zfar * znear / (zfar - znear));

	Mat4_Multiply(current_matrix(), f, current_matrix());
}

void GL_GetModelview (float *out)
{
	Mat4_Copy(mat_modelview[mat_mv_depth], out);
}

void GL_GetProjection (float *out)
{
	Mat4_Copy(mat_projection[mat_proj_depth], out);
}

void GL_GetMVP (float *out)
{
	Mat4_Multiply(mat_projection[mat_proj_depth], mat_modelview[mat_mv_depth], out);
}
