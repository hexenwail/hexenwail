/* gl_shader.h -- GLSL shader manager
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef GL_SHADER_H
#define GL_SHADER_H

/* Shader compilation helpers */
GLuint	GL_CompileShader (GLenum type, const char *source);
/* Multi-chunk form: GL concatenates the parts itself, which is how the dialect
 * header stays out of the shader files in engine/shaders/.  uhexen2-p4ln.4. */
GLuint	GL_CompileShaderParts (GLenum type, int count, const char **parts);
GLuint	GL_LinkProgram (GLuint vert, GLuint frag);
GLuint	GL_LoadProgram (const char *vert_src, const char *frag_src);
GLuint	GL_LoadProgramEx (const char *vert_src, const char *frag_src,
			  const char *frag_prefix);

/* Shader programs.
 *
 * Only sampler bindings live here now.  Everything else -- matrices, fog, the
 * alpha threshold, the world lighting knobs -- moved into the two std140
 * blocks in gl_uniforms.h, because loose uniforms are per-PROGRAM state and a
 * value like fog density had to be re-pushed once per program that wanted it.
 * Samplers cannot go in a uniform block, and they are set once at link time
 * rather than per draw, so they stay.  uhexen2-p4ln.2. */
typedef struct glprogram_s {
	GLuint	program;
	GLint	u_texture0;
	GLint	u_texture1;
	GLint	u_texture2;	/* fullbright mask sampler for world (uhexen2-sjvf) */
	GLint	u_soft_depth;	/* alias FS: opaque-scene depth snapshot, texture unit 1 (uhexen2-mf9u) */
} glprogram_t;

/* Extended program for GPU particle SSBO rendering.  The billboard basis it
 * used to carry (u_pup / u_pright / u_vpn / u_origin / u_ctime) is per-frame
 * data and now lives in the PerFrame block. */
typedef struct {
	glprogram_t base;
} gl_particle_gpu_prog_t;

extern glprogram_t	gl_shader_world;	/* textured + lightmap, fog (cutout: has discard) */
extern glprogram_t	gl_shader_world_opaque;	/* opaque variant: early_fragment_tests, no discard (uhexen2-5c6r) */
extern glprogram_t	gl_shader_alias;	/* vertex-colored, textured, fog */
extern glprogram_t	gl_shader_2d;		/* orthographic textured quads */
extern glprogram_t	gl_shader_particle;	/* textured triangles, per-vertex color */
extern glprogram_t	gl_shader_flat;		/* untextured, vertex-colored */
extern glprogram_t	gl_shader_sky;		/* textured quads for skybox */

extern GLuint		gl_null_fb_texture;	/* 1x1 black sentinel for u_texture2 (uhexen2-sjvf) */
extern GLuint		gl_solid_white_texture;	/* 1x1 opaque white, for untextured imm batches */
extern gl_particle_gpu_prog_t gl_shader_particle_gpu; /* SSBO billboard particles */

/* OIT variants — same shaders but output to MRT accum+revealage */
extern glprogram_t	gl_shader_world_oit;
extern glprogram_t	gl_shader_alias_oit;
extern glprogram_t	gl_shader_particle_oit;

/* Instanced alias program (GL 4.3 SSBO — pose + instances in SSBOs).
 *
 * Its uniforms are all in the shared blocks now: the view-projection and eye
 * position in PerFrame, the instance base / pose vertex type / fog / alpha
 * threshold / caustics in PerDraw.  Note it never needed a model matrix --
 * the per-instance world matrix in the SSBO already yields world space
 * (uhexen2-0gn3). */
typedef struct {
	GLuint	program;	/* shader program handle */
	GLuint	ubo_shadedots;	/* SSBO handle for shadedots table */
} gl_alias_inst_prog_t;

extern gl_alias_inst_prog_t gl_shader_alias_inst;

void	GL_AliasInst_Init (void);
void	GL_AliasInst_Shutdown (void);

/* Vertex attribute locations (fixed, shared across all programs) */
#define ATTR_POSITION	0
#define ATTR_TEXCOORD	1
#define ATTR_LMCOORD	2
#define ATTR_COLOR	3

void	GL_Shaders_Init (void);
void	GL_Shaders_Shutdown (void);

#endif /* GL_SHADER_H */
