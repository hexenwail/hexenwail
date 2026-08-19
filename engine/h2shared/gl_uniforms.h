/* gl_uniforms.h -- std140 uniform blocks shared by every draw program
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* Two blocks, one buffer each, shared by all draw programs:
 *
 *   PerFrame  values that change at most once per frame -- fog, eye position,
 *             time, the world lighting knobs, sky fog and wind, the particle
 *             billboard basis, the instanced view-projection.
 *   PerDraw   values that change per batch -- the three matrices, the alpha
 *             threshold, and the small per-batch scalars.
 *
 * Why this exists at all: loose glUniform* calls are per-PROGRAM state, so a
 * value like fog density had to be re-uploaded once for every program that
 * wanted it, every time it changed.  A shared block is uploaded once and every
 * program sees it.  The other half is that SDL_GPU (and Vulkan under it) has
 * no loose-uniform concept -- uniform data arrives as blocks -- so this is the
 * shape the second backend needs.  uhexen2-p4ln.2.
 *
 * EVERY MEMBER IS vec4, mat4 OR ivec4, and that is deliberate rather than
 * tidy.  std140's padding rules are where this kind of change goes silently
 * wrong: a lone float followed by a vec3 does not sit where a naive C struct
 * puts it, and the failure mode is garbage uniforms with a clean compile.  A
 * layout built only from 16-byte-aligned, 16-byte-multiple types has no
 * padding to get wrong, so the C mirror below is exact by construction -- and
 * R_Uniforms_Init still asks the driver for the block sizes and says so loudly
 * if it disagrees.
 *
 * Shader bodies keep the old scalar names via the #defines at the end of
 * uniforms.inc, so moving to blocks did not touch a line of shader logic --
 * only the declarations at the top of each stage.
 */

#ifndef GL_UNIFORMS_H
#define GL_UNIFORMS_H

/* Block binding points.  Bound from C with glUniformBlockBinding rather than
 * declared in the shader with layout(binding=), because that qualifier on a
 * uniform block needs GLSL 4.20 / GLSL ES 3.10 and the ES tier is 3.00. */
#define RU_BINDING_PERFRAME	0
#define RU_BINDING_PERDRAW	1

/* The GLSL side of these blocks is engine/shaders/uniforms.inc, included by
 * every draw shader.  The C mirrors below must match it member for member;
 * R_BindProgramBlocks asks the driver for the block sizes once and says so if
 * they have drifted apart. */

/* C mirrors.  Field order and types must match uniforms.inc exactly. */
typedef struct {
	float	fog[4];
	float	eyetime[4];
	float	worldp[4];
	float	debugp[4];
	float	skyfog[4];
	float	wind[4];
	float	pup[4];
	float	pright[4];
	float	vpn[4];
	float	origin[4];
	float	viewproj[16];
} r_perframe_t;

typedef struct {
	float	mvp[16];
	float	modelview[16];
	float	model[16];
	float	draw0[4];
	float	draw1[4];
	int	drawi[4];
} r_perdraw_t;

void	R_Uniforms_Init (void);
void	R_Uniforms_Shutdown (void);

/* Point a freshly linked program's blocks at our binding points.  Safe to call
 * on a program that declares neither block. */
void	R_BindProgramBlocks (GLuint program);

/* Per-frame setters.  Each is a no-op when the value is unchanged. */
void	R_SetFog (float density, const float *rgb);
void	R_SetEyePos (const float *xyz);
void	R_SetFrameTime (float t);
void	R_SetWorldCaustics (float intensity, float time);
void	R_SetOverbright (float v);
void	R_SetLightmapBicubic (float v);
void	R_SetLightDebug (float fullbright, float lightmapdbg);
void	R_SetSkyFog (float r, float g, float b, float a);
void	R_SetSkyWind (float u, float v);
void	R_SetParticleBasis (const float *pup, const float *pright,
			    const float *vpn, const float *origin, float ctime);
void	R_SetViewProj (const float *m16);

/* Per-draw setters. */
void	R_SetMVP (const float *m16);
void	R_SetModelView (const float *m16);
void	R_SetModelMatrix (const float *m16);
void	R_SetAlphaThresholdU (float v);
void	R_SetForceOpaqueAlphaU (float v);
void	R_SetAliasCaustics (float intensity, float time);
void	R_SetSoftParams (float x, float y, float z);
void	R_SetPoseBase (int v);
void	R_SetInstBase (int v);
void	R_SetPoseVertType (int v);

/* Upload whatever changed.  Called for you by the R_Draw* wrappers in
 * gl_pipeline.h, which is the only reason no draw path has to remember it. */
void	R_FlushUniforms (void);

/* Drop the shadow so the next setter really uploads.  For context loss and
 * for the paths that reload programs. */
void	R_InvalidateUniforms (void);

#endif	/* GL_UNIFORMS_H */
