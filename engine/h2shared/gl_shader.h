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

extern glprogram_t	gl_shader_world;	/* textured + lightmap, fog */
extern glprogram_t	gl_shader_alias;	/* vertex-colored, textured, fog */
extern glprogram_t	gl_shader_2d;		/* orthographic textured quads */
extern glprogram_t	gl_shader_particle;	/* textured triangles, per-vertex color */
extern glprogram_t	gl_shader_flat;		/* untextured, vertex-colored */
extern glprogram_t	gl_shader_sky;		/* textured quads for skybox */

/* Vertex attribute locations (fixed, shared across all programs) */
#define ATTR_POSITION	0
#define ATTR_TEXCOORD	1
#define ATTR_LMCOORD	2
#define ATTR_COLOR	3

void	GL_Shaders_Init (void);
void	GL_Shaders_Shutdown (void);

#endif /* GL_SHADER_H */
