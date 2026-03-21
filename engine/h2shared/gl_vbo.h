/* gl_vbo.h -- VBO/VAO helpers and streaming immediate-mode replacement
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef GL_VBO_H
#define GL_VBO_H

/* Maximum vertices per GL_Imm batch */
#define GL_IMM_MAX_VERTS	8192

/* Streaming immediate-mode replacement.
 * Usage:
 *   GL_ImmBegin();
 *   for each vertex:
 *     GL_ImmTexCoord2f(s, t);
 *     GL_ImmColor4f(r, g, b, a);
 *     GL_ImmVertex3f(x, y, z);     // must be last — commits vertex
 *   GL_ImmEnd(GL_TRIANGLES, &gl_shader_flat);
 */

void	GL_ImmBegin (void);
void	GL_ImmVertex3f (float x, float y, float z);
void	GL_ImmVertex2f (float x, float y);
void	GL_ImmTexCoord2f (float s, float t);
void	GL_ImmLMCoord2f (float s, float t);
void	GL_ImmColor4f (float r, float g, float b, float a);
void	GL_ImmColor3f (float r, float g, float b);
void	GL_ImmColor4ubv (const unsigned char *c);
void	GL_ImmColor3ubv (const unsigned char *c);

struct glprogram_s;
void	GL_ImmEnd (GLenum mode, const struct glprogram_s *shader);
void	GL_ImmDraw (GLenum mode);	/* draw without changing shader (caller manages program) */

/* Set alpha threshold for subsequent draws (0 = no discard, 0.666 = standard) */
void	GL_SetAlphaThreshold (float threshold);

/* Init / shutdown */
void	GL_VBO_Init (void);
void	GL_VBO_Shutdown (void);

#endif /* GL_VBO_H */
