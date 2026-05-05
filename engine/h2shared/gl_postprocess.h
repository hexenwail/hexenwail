/* gl_postprocess.h -- GLSL post-process gamma/contrast correction
 *
 * Since SDL3 removed hardware gamma support, this implements gamma and
 * contrast adjustment via a full-screen GLSL post-process pass.  The
 * scene is rendered into an FBO, then blitted to the default framebuffer
 * with a shader that applies gamma and contrast correction.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __GL_POSTPROCESS_H
#define __GL_POSTPROCESS_H

/* Call once after GL context creation (from GL_Init or similar). */
void GL_PostProcess_Init (void);

/* Call when the GL context is being torn down. */
void GL_PostProcess_Shutdown (void);

/* Call at the start of each frame, before any rendering.
 * Binds the scene FBO if gamma/contrast adjustment is active. */
void GL_PostProcess_BeginFrame (void);

/* Call after 3D rendering but before 2D HUD drawing.
 * Resolves the scaled FBO to native resolution so HUD renders crisp. */
void GL_PostProcess_End3D (void);

/* Call at the end of each frame, after all rendering but before
 * SDL_GL_SwapWindow.  Blits the scene texture to the default
 * framebuffer with gamma/contrast applied. */
void GL_PostProcess_EndFrame (void);

/* Returns true if post-processing is currently active. */
qboolean GL_PostProcess_Active (void);

/* Request a brief waterwarp preview animation (duration in seconds). */
void GL_PostProcess_RequestWaterwarpPreview (float duration);
void GL_PostProcess_ResetWaterwarpPreview (void);

/* Render scale cvar (0.25 - 1.0, lower = chunkier pixels) */
extern cvar_t r_scale;

/* Software rendering emulation (0=off, 1=dithered, 2=banded) */
extern cvar_t r_softemu;
extern cvar_t r_dither;

/* HDR rendering (0=off, 1=ACES tonemapping) */
extern cvar_t r_hdr;
extern cvar_t r_hdr_exposure;

/* Order-Independent Transparency (WBOIT) */
extern cvar_t r_oit;
void OIT_BeginTranslucency (void);
void OIT_EndTranslucency (GLuint scene_fbo);
qboolean OIT_Active (void);	/* OIT is built and r_oit is on */
qboolean OIT_InPass (void);	/* currently between Begin/EndTranslucency */
GLuint GL_GetSceneFBO (void);

/* OIT_OUTPUT macro for injection into translucent fragment shaders.
 * Replaces `out vec4 fragColor` with MRT outputs when OIT==1. */
extern const char *OIT_OUTPUT_GLSL_STR;

#endif	/* __GL_POSTPROCESS_H */
