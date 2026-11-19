/* gl_progid.h -- every GPU program the renderer builds, by stable id
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* GL builds a program from GLSL at run time: hand it source, compile, link.
 * SDL_GPU cannot compile GLSL at all.  It creates a shader from an offline
 * blob, and it has to be told how many samplers, storage textures, storage
 * buffers and uniform buffers that blob binds.  So a program cannot be "this
 * source string" any more; it has to be a name that both backends can look
 * up.  That name is an rprog_id_t.  GitHub #340, phase P1b.
 *
 * Each id maps, in gl_shader.c's rprog_defs[], to:
 *
 *   - per stage, the two glShaderSource chunks GL compiles (the tier header
 *     and the engine/shaders body, exactly as before ids existed);
 *   - per stage, the offline SPIR-V copy EmbedShaders.cmake writes for that
 *     header's #define set (<build>/engine/generated/spirv/<blob>.vk.spv);
 *   - how GL links it (which fixed attribute locations it binds).
 *
 * The per-blob resource counts SDL_CreateGPUShader and
 * SDL_CreateGPUComputePipeline need are in gl_progid_refl.h, generated from
 * the .vk.spv by tools/gen_program_reflection.py and checked against them in
 * shaders-spirv.yml.  The blobs themselves are not embedded yet: that would
 * put glslangValidator into the default build, which #340 decision 6 leaves
 * open.
 *
 * R_Program_Create is the only way any renderer code gets a program; no
 * other file creates, compiles or links one.  tools/test_program_routing.py
 * holds that, and that every id has a blob per stage, a reflection row per
 * blob, and a caller.  tools/test_shader_variants.py reads the same table to
 * prove engine/shaders/variants.txt covers every define set it builds.
 *
 * The ids are one enum for every tier.  A program a tier never builds (the
 * desktop-only ones on GLES/WebGL2) keeps its id there with no table row, and
 * R_Program_Create refuses it -- though no ES caller asks.
 */

#ifndef GL_PROGID_H
#define GL_PROGID_H

typedef enum {
	/* gl_shader.c: the draw programs (glprogram_t) */
	RPROG_2D,
	RPROG_FLAT,
	RPROG_WORLD,
	RPROG_WORLD_OPAQUE,
	RPROG_ALIAS,
	RPROG_ALIAS_NP,			/* affine twins, r_softemu_mdl_warp (uhexen2-ktjv) */
	RPROG_SKELETAL,
	RPROG_SKELETAL_NP,
	RPROG_PARTICLE,
	RPROG_SKY_LAYERS,
	RPROG_SKY_BOXSIDE,
	RPROG_SKY_CUBEMAP,
	RPROG_WORLD_OIT,		/* the six OIT twins: "#define OIT 1" on the fragment stage */
	RPROG_ALIAS_OIT,
	RPROG_ALIAS_NP_OIT,
	RPROG_SKELETAL_OIT,
	RPROG_SKELETAL_NP_OIT,
	RPROG_PARTICLE_OIT,
	RPROG_PARTICLE_GPU,
	RPROG_ALIAS_INSTANCED,
	RPROG_ALIAS_INSTANCED_NP,
	/* gl_postprocess.c */
	RPROG_POSTPROCESS,
	RPROG_BLOOM_BRIGHT,
	RPROG_BLOOM_DOWN,
	RPROG_BLOOM_UP,
	RPROG_OIT_RESOLVE,
	RPROG_OIT_RESOLVE_MSAA,
	RPROG_SOFTEMU_LUT_MODE0,	/* compute, one per softemu_metric_t */
	RPROG_SOFTEMU_LUT_MODE1,
	RPROG_SOFTEMU_LUT_MODE2,
	/* gl_ssao.c */
	RPROG_SSAO_PREPARE,
	RPROG_SSAO_MIP,
	RPROG_SSAO_EVALUATE_Q1,		/* r_ssao 1..3 */
	RPROG_SSAO_EVALUATE_Q2,
	RPROG_SSAO_EVALUATE_Q3,
	RPROG_SSAO_FILTER,
	RPROG_SSAO_COMPOSITE,
	/* gl_shadow.c */
	RPROG_SHADOW,
	RPROG_SHADOW_DEBUG,
	RPROG_SHADOW_ALIAS,
	/* gl_worldcull.c */
	RPROG_CULL_CLEAR,
	RPROG_CULL_MARK,
	RPROG_HIZ_COPY,
	RPROG_HIZ_REDUCE,
	/* gl_lightcluster.c */
	RPROG_LIGHTCLUSTER,
	RPROG_COUNT
} rprog_id_t;

/* Stage slots: a graphics program has a vertex and a fragment stage, a
 * compute program only RPROG_STAGE_COMPUTE (slot 0). */
#define RPROG_STAGE_VERTEX	0
#define RPROG_STAGE_FRAGMENT	1
#define RPROG_STAGE_COMPUTE	0
#define RPROG_MAX_STAGES	2

/* What SDL_GPU needs to create one stage from its blob, by resource class
 * in the order SDL_GPU numbers them (tools/test_sdlgpu_bindings.py has the
 * rules).  A graphics stage's storage is read-only, so its rw_ counts are 0;
 * `threads` is the compute local size and 0 for graphics. */
typedef enum {
	RPROG_REFL_VERTEX,
	RPROG_REFL_FRAGMENT,
	RPROG_REFL_COMPUTE
} rprog_refl_stage_t;

typedef struct {
	const char	*blob;		/* e.g. "salias_np_oit.frag": <blob>.vk.spv */
	unsigned char	stage;		/* rprog_refl_stage_t */
	unsigned char	samplers;
	unsigned char	ro_storage_textures;
	unsigned char	ro_storage_buffers;
	unsigned char	rw_storage_textures;
	unsigned char	rw_storage_buffers;
	unsigned char	uniform_buffers;
	unsigned short	threads[3];
} rprog_refl_t;

/* Compile and link program `id`.  Returns the GL program name, or 0 after
 * logging why (a compile or link error, or an id this tier has no row for).
 * The caller owns the result: it looks up its locations, binds its blocks
 * and samplers, and deletes it, as before. */
GLuint	R_Program_Create (rprog_id_t id);

/* The id's log name ("2d", "world_oit", ...): its enumerator, lower case,
 * without RPROG_.  NULL for an id with no row on this tier. */
const char	*R_Program_Name (rprog_id_t id);

/* The offline SPIR-V copy of one stage, and its reflection row; NULL past
 * the program's stages or for an id with no row.  Nothing calls these in GL
 * mode -- they are the SDL_GPU backend's half of the table. */
const char	*R_Program_Blob (rprog_id_t id, int stage);
const rprog_refl_t *R_Program_Reflection (rprog_id_t id, int stage);

#endif /* GL_PROGID_H */
