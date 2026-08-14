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
GLuint	GL_LinkProgram (GLuint vert, GLuint frag);
GLuint	GL_LoadProgram (const char *vert_src, const char *frag_src);

/* Shader programs */
typedef struct glprogram_s {
	GLuint	program;
	GLint	u_mvp;
	GLint	u_texture0;
	GLint	u_texture1;
	GLint	u_texture2;	/* fullbright mask sampler for world (uhexen2-sjvf) */
	GLint	u_color;
	GLint	u_fog_density;
	GLint	u_fog_color;
	GLint	u_alpha_threshold;
	GLint	u_modelview;
	GLint	u_time;
	GLint	u_skyfog;
	GLint	u_eyepos;
	GLint	u_wind;		/* sky shader: per-skybox wind UV offset (uhexen2-typa) */
	GLint	u_caustics;	/* world shader: vec2(intensity, time) for underwater caustics (uhexen2-6bfm) */
	GLint	u_overbright;	/* world shader: lightmap multiplier (1.0 = off, 2.0 = on); Ironwail parity (uhexen2-f29y) */
	GLint	u_lightmap_bicubic; /* world shader: 0.0 = hardware bilinear, 1.0 = 4-tap B-spline bicubic lightmap fetch (uhexen2-b2f0) */
	GLint	u_lightdebug;	/* world shader: vec2(r_fullbright, r_lightmap).  x > 0.5 replaces the lightmap sample with white, y > 0.5 replaces the diffuse sample with white.  Both zero in normal rendering.  uhexen2-isq7. */
	GLint	u_force_opaque_alpha; /* alias/world FS: when > 0.5, fragColor.a is forced to 1.0 regardless of color.a.  Set to 1 by C for confirmed-opaque draws, to 0 for ENTALPHA / DRF_TRANSLUCENT / OIT translucent paths that need color.a preserved for blend.  uhexen2-khsa r13. */
	/* Alias caustics (uhexen2-0gn3).  Deliberately NOT named u_caustics:
	 * gl_shader_alias is the generic textured+vertex-color program and is
	 * also used for sprites, warp polys and unlit brush polys, so its
	 * caustics value is per-batch state pushed by GL_ImmEnd.  A shared name
	 * would make GL_ImmEnd clobber the per-frame world u_caustics that
	 * R_SetupFrame uploads. */
	GLint	u_alias_caustics;   /* alias FS: vec2(intensity, time); x=0 disables */
	GLint	u_alias_model;	    /* alias VS: model-only matrix (no view), needed because u_modelview is view*model and caustics must be sampled in world XY */
} glprogram_t;

/* Extended program for GPU particle SSBO rendering */
typedef struct {
	glprogram_t base;       /* standard uniforms (u_mvp, u_modelview, u_fog_density, u_fog_color) */
	GLint   u_pup;          /* r_pup billboard-up vector */
	GLint   u_pright;       /* r_pright billboard-right vector */
	GLint   u_vpn;          /* view forward vector (for distance-based scale) */
	GLint   u_origin;       /* camera origin (for distance-based scale) */
	GLint   u_ctime;        /* current cl.time (for dead particle culling) */
} gl_particle_gpu_prog_t;

extern glprogram_t	gl_shader_world;	/* textured + lightmap, fog (cutout: has discard) */
extern glprogram_t	gl_shader_world_opaque;	/* opaque variant: early_fragment_tests, no discard (uhexen2-5c6r) */
extern glprogram_t	gl_shader_alias;	/* vertex-colored, textured, fog */
extern glprogram_t	gl_shader_2d;		/* orthographic textured quads */
extern glprogram_t	gl_shader_particle;	/* textured triangles, per-vertex color */
extern glprogram_t	gl_shader_flat;		/* untextured, vertex-colored */
extern glprogram_t	gl_shader_sky;		/* textured quads for skybox */

extern GLuint		gl_null_fb_texture;	/* 1x1 black sentinel for u_texture2 (uhexen2-sjvf) */
extern gl_particle_gpu_prog_t gl_shader_particle_gpu; /* SSBO billboard particles */

/* OIT variants — same shaders but output to MRT accum+revealage */
extern glprogram_t	gl_shader_world_oit;
extern glprogram_t	gl_shader_alias_oit;
extern glprogram_t	gl_shader_particle_oit;

/* Instanced alias program (GL 4.3 SSBO — pose + instances in SSBOs) */
typedef struct {
	GLuint	program;	/* shader program handle */
	GLuint	ubo_shadedots;	/* SSBO handle for shadedots table */
	GLint	u_fog_density;	/* fragment fog uniforms */
	GLint	u_fog_color;
	GLint	u_alpha_threshold;
	GLint	u_viewproj;	/* view-projection matrix (uhexen2-8pc2) */
	GLint	u_inst_base;	/* base instance index for gl_InstanceID offset */
	GLint	u_eyepos;	/* camera position for fog distance */
	GLint	u_poseverttype;	/* vertex format: 0=PV_QUAKE1, 1=PV_MD3 */
	GLint	u_force_opaque_alpha; /* uhexen2-khsa r13 */
	GLint	u_alias_caustics; /* uhexen2-0gn3 — vec2(intensity, time); no model matrix needed, the instance world matrix already yields world space */
} gl_alias_inst_prog_t;

extern gl_alias_inst_prog_t gl_shader_alias_inst;

void	GL_AliasInst_Init (void);
void	GL_AliasInst_Shutdown (void);

void	GL_ParticleGPU_SetUniforms (const gl_particle_gpu_prog_t *prog,
				     const float *pup, const float *pright,
				     const float *vpn, const float *origin,
				     float ctime);

/* Vertex attribute locations (fixed, shared across all programs) */
#define ATTR_POSITION	0
#define ATTR_TEXCOORD	1
#define ATTR_LMCOORD	2
#define ATTR_COLOR	3

void	GL_Shaders_Init (void);
void	GL_Shaders_Shutdown (void);

#endif /* GL_SHADER_H */
