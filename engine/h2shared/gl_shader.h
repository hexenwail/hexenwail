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
	GLint	u_color;
	GLint	u_fog_density;
	GLint	u_fog_color;
	GLint	u_alpha_threshold;
	GLint	u_modelview;
	GLint	u_time;
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

extern glprogram_t	gl_shader_world;	/* textured + lightmap, fog */
extern glprogram_t	gl_shader_alias;	/* vertex-colored, textured, fog */
extern glprogram_t	gl_shader_2d;		/* orthographic textured quads */
extern glprogram_t	gl_shader_particle;	/* textured triangles, per-vertex color */
extern glprogram_t	gl_shader_flat;		/* untextured, vertex-colored */
extern glprogram_t	gl_shader_sky;		/* textured quads for skybox */
extern gl_particle_gpu_prog_t gl_shader_particle_gpu; /* SSBO billboard particles */

/* Extended program for GPU alias model SSBO rendering */
typedef struct {
	glprogram_t base;	/* standard uniforms */
	GLint	u_pose0;	/* current pose index */
	GLint	u_pose1;	/* previous pose index */
	GLint	u_lerp;		/* interpolation factor */
	GLint	u_scale;	/* aliashdr->scale */
	GLint	u_scale_origin;	/* aliashdr->scale_origin */
	GLint	u_poseverts;	/* verts per pose */
	GLint	u_shade_light;	/* shading intensity */
	GLint	u_light_color;	/* RGB light color */
	GLint	u_ent_alpha;	/* entity alpha */
} gl_alias_gpu_prog_t;

extern gl_alias_gpu_prog_t gl_shader_alias_gpu;

void	GL_AliasGPU_SetUniforms (const gl_alias_gpu_prog_t *prog,
				  int pose0, int pose1, float lerp,
				  const float *scale, const float *scale_origin,
				  int poseverts, float shade_light,
				  const float *light_color, float alpha);

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
