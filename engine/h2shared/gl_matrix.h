/* gl_matrix.h -- software matrix stack (replaces fixed-function pipeline)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef GL_MATRIX_H
#define GL_MATRIX_H

/* Matrix modes (match GL constants for easy transition) */
#define GL_MAT_MODELVIEW	0
#define GL_MAT_PROJECTION	1

void	GL_MatrixMode (int mode);
void	GL_LoadIdentity (void);
void	GL_PushMatrix (void);
void	GL_PopMatrix (void);

void	GL_Translatef (float x, float y, float z);
void	GL_Rotatef (float angle, float x, float y, float z);
void	GL_Scalef (float x, float y, float z);
void	GL_Ortho (double left, double right, double bottom, double top, double znear, double zfar);
void	GL_Frustum (double left, double right, double bottom, double top, double znear, double zfar);
void	GL_LoadMatrixf (const float *m);

/* Retrieve current matrices (column-major, OpenGL convention) */
void	GL_GetModelview (float *out);
void	GL_GetProjection (float *out);
void	GL_GetMVP (float *out);		/* modelview * projection */

/* 4x4 matrix math utilities */
void	Mat4_Identity (float *out);
void	Mat4_Multiply (const float *a, const float *b, float *out);
void	Mat4_Copy (const float *src, float *dst);

#endif /* GL_MATRIX_H */
