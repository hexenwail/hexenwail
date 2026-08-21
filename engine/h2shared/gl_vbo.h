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

int	GL_ImmCount (void);
void	GL_ImmResetState (void);
void	GL_ImmInvalidateState (void);	/* drop cached uniform state (after vid_restart, etc) */
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
/* Current threshold, so callers that must clobber it can restore the caller's
 * value rather than guessing the engine default.  uhexen2-nudx. */
float	GL_GetAlphaThreshold (void);
/* Force fragColor.a = 1.0 for confirmed-opaque draws.  v=1 to force, v=0 to
 * preserve color.a (ENTALPHA / DRF_TRANSLUCENT translucent paths).  Negative
 * leaves shader default.  uhexen2-khsa r13. */
void	GL_SetForceOpaqueAlpha (float v);
/* Current value, for the same reason GL_GetAlphaThreshold exists: a draw path
 * that sets its uniforms by hand must apply what the caller decided, not the
 * shader default.  uhexen2-7ok0.3. */
float	GL_GetForceOpaqueAlpha (void);
/* Underwater caustics for subsequent alias batches.  intensity 0 = off (the
 * default, and what every non-alias user of gl_shader_alias leaves it at).
 * The model matrix is the entity's model-only transform; salias_vert needs it
 * to recover world XY because u_modelview is view*model.  uhexen2-0gn3. */
void	GL_SetAliasCaustics (float intensity, float time);
void	GL_SetTurb (float amplitude, float time);	/* uhexen2-9o7u */
void	GL_SetAliasModelMatrix (const float *m);	/* 16 floats, column-major */
/* Read back the current values, for draw paths that bypass GL_ImmEnd and must
 * apply the identical state by hand (the PV_IQM skeletal path, uhexen2-7ok0.3). */
void	GL_GetAliasModelMatrix (float *out);		/* 16 floats */
void	GL_GetAliasCaustics (float *out2);		/* 2 floats: intensity, time */
/* Soft-particle depth fade for subsequent sprite batches (uhexen2-mf9u).
 * inv_dist is the reciprocal of the fade distance in world units, 0 = off
 * (the default, and what every non-sprite user of gl_shader_alias leaves it
 * at).  za/zb linearize a window-space depth to a view-space distance as
 * z = zb / (za - d); R_SoftParticleParams derives them from the live
 * projection matrix and depth range. */
void	GL_SetSoftParticles (float inv_dist, float za, float zb);

/* Init / shutdown */
void	GL_VBO_Init (void);
void	GL_VBO_Shutdown (void);

#endif /* GL_VBO_H */
