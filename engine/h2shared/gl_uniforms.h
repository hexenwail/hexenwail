/* gl_uniforms.h -- the two std140 uniform blocks shared by every file-backed
 * draw program
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* Two blocks, one buffer each, split BY SHADER STAGE:
 *
 *   VertParams  bound at UBO_BINDING_VERT -- the matrices, the particle
 *               billboard basis, the pose/instance indices.
 *   FragParams  bound at UBO_BINDING_FRAG -- fog, the alpha mask pair, the
 *               world lighting knobs, caustics, sky fog and wind.
 *
 * Split by stage rather than by update frequency because SDL_GPU's stage
 * separation is a hard constraint -- SDL_PushGPUVertexUniformData and
 * SDL_PushGPUFragmentUniformData are separate calls into separate descriptor
 * sets (1 and 3) -- where frequency is only an optimization.  A frequency
 * split can still be layered on later by halving either block; shader bodies
 * would not notice, because they name members through the aliases in
 * engine/shaders/uniforms.inc rather than by slot.
 *
 * Eye position and cl.time are the only values both stages want, and a stage
 * cannot reach into the other's descriptor set, so they are duplicated.
 * R_SetEyePos and R_SetFrameTime write both copies, which is what keeps them
 * from drifting.
 *
 * Every member is vec4, mat4 or ivec4 -- see engine/shaders/uniforms.inc for
 * why that is a safety property and not a style choice.  The mirrors below are
 * therefore exact by construction, pinned by COMPILE_TIME_ASSERT in the .c and
 * checked once against the driver's own GL_UNIFORM_BLOCK_DATA_SIZE.
 *
 * uhexen2-p4ln.
 */

#ifndef GL_UNIFORMS_H
#define GL_UNIFORMS_H

/* C mirrors.  Field order and types must match uniforms.inc exactly. */
typedef struct {
	float	mvp[16];	/*   0 */
	float	modelview[16];	/*  64 */
	float	model[16];	/* 128 */
	float	viewproj[16];	/* 192 */
	float	scene[4];	/* 256  xyz eye position, w cl.time */
	float	pup[4];		/* 272  xyz particle billboard up */
	float	pright[4];	/* 288  xyz particle billboard right */
	float	vpn[4];		/* 304  xyz view forward */
	float	porigin[4];	/* 320  xyz camera origin, w particle time */
	int	posei[4];	/* 336  x pose base, y instance base, z pose vertex type */
	float	alias[4];	/* 352  x alias dynamic-light scale (#450) */
} r_vertparams_t;		/* 368 */

typedef struct {
	float	scene[4];	/*   0  xyz eye position, w cl.time */
	float	fog[4];		/*  16  xyz fog color, w fog density */
	float	mask[4];	/*  32  x alpha threshold, y force-opaque alpha,
				 *      z lightmap overbright, w bicubic lightmap */
	float	caustics[4];	/*  48  xy world caustics, zw alias caustics */
	float	soft[4];	/*  64  xyz soft-particle params */
	float	skyfog[4];	/*  80  rgb sky fog color, a blend */
	float	sky[4];		/*  96  xy sky wind uv offset */
	float	debug[4];	/* 112  x r_fullbright, y r_lightmap */
	float	skydir[4];	/* 128  xyz cubemap wind direction, w cubemap yaw */
} r_fragparams_t;		/* 144 */

/* AliasFrag (engine/shaders/alias.inc), salias_frag's own block, at
 * UBO_BINDING_ALIAS_FRAG (#450, alias slice S2).  A block member is GLOBAL,
 * where the loose uniforms these replaced were per-program state, so every
 * path that draws with an alias-family program writes what it reads first:
 *
 *   lightview, grid  R_LightCluster_BindForAlias, once a frame, before any
 *                    alias draw.  Read only where v_dlightscale > 0, which
 *                    needs R_LightCluster_AliasActive -- the same gate the
 *                    write is behind.
 *   warp xy (turb)   GL_ImmEnd from imm_turb; the skeletal and instanced
 *                    draws write 0.
 *   warp z (spans)   R_SetSpanCell around the brush-poly and liquid batches,
 *                    back to 0 after each; the skeletal and instanced draws
 *                    write 0.
 *   material,        GL_ImmEnd from imm_alias_material / imm_alias_lightdir;
 *   lightdir         the skeletal draw passes the same pair through; the
 *                    instanced draw writes them per batch (world-space light),
 *                    and 0 material for its fullbright pass.
 *
 * Seeded with material and lightdir (0,0,1), imm_*'s resting values. */
typedef struct {
	float	lightview[16];	/*   0  world -> eye, no entity transform */
	float	grid[4];	/*  64  xy viewport origin, zw froxels per pixel */
	float	gridz[4];	/*  80  xy zlogscale, zlogbias */
	float	warp[4];	/*  96  xy liquid warp (amplitude, time), z span cell */
	float	material[4];	/* 112  xyz normal-map, gloss intensity, gloss exponent */
	float	lightdir[4];	/* 128  xyz model light direction */
} r_aliasfrag_t;		/* 144 */

void	R_Uniforms_Init (void);
void	R_Uniforms_Shutdown (void);

/* Point a freshly linked program's blocks at our binding points and report
 * which of them it actually declares, -1 for a block it does not.  Safe on a
 * program declaring neither, which is every shader still written as a C string
 * literal.  The out pointers may be NULL.
 *
 * The pinning cannot be expressed in the shader: layout(binding=) on a uniform
 * block is GL 4.2 / GLSL ES 3.10 and the ES tier runs ES 3.00.  So this is the
 * only place the binding point is chosen, and UBO_BINDING_* in gl_shader.h is
 * the only place the two sides meet. */
void	R_BindProgramBlocks (GLuint program, GLint *out_vert, GLint *out_frag);

/* The same for AliasFrag: its block index in `program`, pinned to
 * UBO_BINDING_ALIAS_FRAG, or -1 if the program declares no such block, which
 * is every program but the ten that link salias_frag. */
GLint	R_BindAliasFragBlock (GLuint program);

/* Setters.  Each writes the CPU shadow and marks its block dirty only when the
 * value actually changed; nothing reaches the GPU until R_FlushUniforms. */

/* VertParams */
void	R_SetMVP (const float *m16);
void	R_SetModelView (const float *m16);
void	R_SetModelMatrix (const float *m16);
void	R_SetViewProj (const float *m16);
void	R_SetParticleBasis (const float *pup, const float *pright,
			    const float *vpn, const float *origin, float ptime);
void	R_SetPoseBase (int v);
void	R_SetInstBase (int v);
void	R_SetPoseVertType (int v);
void	R_SetAliasDlight (float scale);	/* u_alias_dlight, salias_vert (#450) */

/* Both blocks -- see the header comment on why these two are duplicated. */
void	R_SetEyePos (const float *xyz);
void	R_SetFrameTime (float t);

/* FragParams */
void	R_SetFog (float density, const float *rgb);
/* ONE setter for the alpha-mask pair, and deliberately not two.  A loose
 * uniform is per-PROGRAM state, so a draw path that set only the threshold
 * inherited its own program's last force-opaque value -- which was usually the
 * value it wanted.  A block member is global, so the same code inherits
 * whatever the last *unrelated* batch left there.  That is the defect that got
 * the first attempt at this reverted (three world-path call sites set the
 * threshold alone).  Requiring both arguments makes writing half of the pair
 * impossible rather than merely discouraged. */
void	R_SetAlphaMask (float threshold, float force_opaque);
void	R_SetOverbright (float v);
void	R_SetLightmapBicubic (float v);
void	R_SetWorldCaustics (float intensity, float time);
void	R_SetAliasCausticsU (float intensity, float time);
void	R_SetSoftParams (float x, float y, float z);
void	R_SetSkyFog (float r, float g, float b, float a);
void	R_SetSkyWind (float u, float v);
/* The cubemap sky's wind as a direction offset, plus r_skybox_speed as a yaw. */
void	R_SetSkyCubeWind (const float *dir3, float yaw);
void	R_SetLightDebug (float fullbright, float lightmapdbg);

/* AliasFrag */
void	R_SetAliasLightGrid (const float *view16, const float *grid4,
			     float zlogscale, float zlogbias);
void	R_SetAliasTurb (float amplitude, float time);
void	R_SetAliasSpans (float cell);
void	R_SetAliasMaterialU (float normal, float gloss, float glossexp);
void	R_SetAliasLightDirU (const float *dir3);

/* Upload whichever blocks changed.  One push per dirty block, whole block,
 * immediately before the draw that reads it. */
void	R_FlushUniforms (void);

/* Force the next flush to re-upload every block and re-assert its binding.
 * For context loss and for anything that may have moved the indexed uniform
 * bindings out from under us. */
void	R_InvalidateUniforms (void);

#endif	/* GL_UNIFORMS_H */
