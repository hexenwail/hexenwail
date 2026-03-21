/* gl_shader.c -- GLSL shader manager
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_shader.h"

glprogram_t	gl_shader_world;
glprogram_t	gl_shader_alias;
glprogram_t	gl_shader_2d;
glprogram_t	gl_shader_particle;
glprogram_t	gl_shader_flat;
glprogram_t	gl_shader_sky;

/* ------------------------------------------------------------------ */
/* Shader compilation helpers                                          */
/* ------------------------------------------------------------------ */

GLuint GL_CompileShader (GLenum type, const char *source)
{
	GLuint shader;
	GLint status;
	char log[1024];

	shader = glCreateShader_fp(type);
	glShaderSource_fp(shader, 1, &source, NULL);
	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("Shader compile error: %s\n", log);
		glDeleteShader_fp(shader);
		return 0;
	}
	return shader;
}

GLuint GL_LinkProgram (GLuint vert, GLuint frag)
{
	GLuint prog;
	GLint status;
	char log[1024];

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, vert);
	glAttachShader_fp(prog, frag);

	/* bind fixed attribute locations before linking */
	glBindAttribLocation_fp(prog, ATTR_POSITION, "a_position");
	glBindAttribLocation_fp(prog, ATTR_TEXCOORD, "a_texcoord");
	glBindAttribLocation_fp(prog, ATTR_LMCOORD,  "a_lmcoord");
	glBindAttribLocation_fp(prog, ATTR_COLOR,    "a_color");

	glLinkProgram_fp(prog);
	glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
	if (!status)
	{
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_Printf("Shader link error: %s\n", log);
		glDeleteProgram_fp(prog);
		return 0;
	}
	return prog;
}

GLuint GL_LoadProgram (const char *vert_src, const char *frag_src)
{
	GLuint vs, fs, prog;

	vs = GL_CompileShader(GL_VERTEX_SHADER, vert_src);
	if (!vs) return 0;
	fs = GL_CompileShader(GL_FRAGMENT_SHADER, frag_src);
	if (!fs) { glDeleteShader_fp(vs); return 0; }

	prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	return prog;
}

/* ------------------------------------------------------------------ */
/* Helper to look up all common uniforms                               */
/* ------------------------------------------------------------------ */

static void GL_InitProgramUniforms (glprogram_t *p)
{
	p->u_mvp             = glGetUniformLocation_fp(p->program, "u_mvp");
	p->u_texture0        = glGetUniformLocation_fp(p->program, "u_texture0");
	p->u_texture1        = glGetUniformLocation_fp(p->program, "u_texture1");
	p->u_color           = glGetUniformLocation_fp(p->program, "u_color");
	p->u_fog_density     = glGetUniformLocation_fp(p->program, "u_fog_density");
	p->u_fog_color       = glGetUniformLocation_fp(p->program, "u_fog_color");
	p->u_alpha_threshold = glGetUniformLocation_fp(p->program, "u_alpha_threshold");
	p->u_modelview       = glGetUniformLocation_fp(p->program, "u_modelview");
	p->u_time            = glGetUniformLocation_fp(p->program, "u_time");
}

/* ------------------------------------------------------------------ */
/* Shader sources                                                      */
/* ------------------------------------------------------------------ */

/* Use GLSL 1.50 core for desktop GL 3.2+.  GLES 3.2 would use
 * #version 320 es — we can switch when we drop desktop GL. */

/* --- shader_2d: orthographic HUD/text rendering --- */
static const char s2d_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char s2d_frag[] =
	"#version 430 core\n"
	"uniform sampler2D u_texture0;\n"
	"uniform float u_alpha_threshold;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 color = tex * v_color;\n"
	"    if (color.a < u_alpha_threshold) discard;\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_flat: untextured, vertex-colored (dlights, blendpoly) --- */
static const char sflat_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char sflat_frag[] =
	"#version 430 core\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = v_color;\n"
	"}\n";

/* --- shader_world: textured + lightmap multitexture, with fog --- */
static const char sworld_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec2 a_lmcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"out vec2 v_texcoord;\n"
	"out vec2 v_lmcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_lmcoord = a_lmcoord;\n"
	"    v_color = a_color;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char sworld_frag[] =
	"#version 430 core\n"
	"uniform sampler2D u_texture0;\n"
	"uniform sampler2D u_texture1;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_alpha_threshold;\n"
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 lm = texture(u_texture1, v_lmcoord);\n"
	"    vec4 color = tex * lm * v_color;\n"
	"    if (color.a < u_alpha_threshold) discard;\n"
	"    float fog = exp(-u_fog_density * u_fog_density * v_fogdist * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_alias: vertex-colored, textured, fog (models) --- */
static const char salias_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char salias_frag[] =
	"#version 430 core\n"
	"uniform sampler2D u_texture0;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_alpha_threshold;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 color = tex * v_color;\n"
	"    if (color.a < u_alpha_threshold) discard;\n"
	"    float fog = exp(-u_fog_density * u_fog_density * v_fogdist * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_particle: textured triangles with per-vertex color --- */
static const char spart_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char spart_frag[] =
	"#version 430 core\n"
	"uniform sampler2D u_texture0;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 color = tex * v_color;\n"
	"    if (color.a < 0.01) discard;\n"
	"    float fog = exp(-u_fog_density * u_fog_density * v_fogdist * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_sky: two-layer scrolling sky (solid + alpha) --- */
static const char ssky_vert[] =
	"#version 430 core\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec2 a_lmcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_texcoord;\n"
	"out vec2 v_lmcoord;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_lmcoord = a_lmcoord;\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char ssky_frag[] =
	"#version 430 core\n"
	"uniform sampler2D u_texture0;\n"
	"uniform sampler2D u_texture1;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_fog_density;\n"
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 solid = texture(u_texture0, v_texcoord);\n"
	"    vec4 alpha = texture(u_texture1, v_lmcoord);\n"
	"    vec3 color = mix(solid.rgb, alpha.rgb, alpha.a);\n"
	"    fragColor = vec4(color, 1.0) * v_color;\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                     */
/* ------------------------------------------------------------------ */

static qboolean GL_InitProgram (glprogram_t *p, const char *name,
				const char *vert_src, const char *frag_src)
{
	p->program = GL_LoadProgram(vert_src, frag_src);
	if (!p->program)
	{
		Con_Printf("Failed to load shader: %s\n", name);
		return false;
	}
	GL_InitProgramUniforms(p);

	/* bind texture unit defaults */
	glUseProgram_fp(p->program);
	if (p->u_texture0 >= 0) glUniform1i_fp(p->u_texture0, 0);
	if (p->u_texture1 >= 0) glUniform1i_fp(p->u_texture1, 1);
	if (p->u_alpha_threshold >= 0) glUniform1f_fp(p->u_alpha_threshold, 0.0f);
	if (p->u_fog_density >= 0) glUniform1f_fp(p->u_fog_density, 0.0f);
	glUseProgram_fp(0);

	Con_SafePrintf("  shader '%s' loaded (program %u)\n", name, p->program);
	return true;
}

void GL_Shaders_Init (void)
{
	Con_SafePrintf("Initializing shaders...\n");

	GL_InitProgram(&gl_shader_2d,       "2d",       s2d_vert,    s2d_frag);
	GL_InitProgram(&gl_shader_flat,     "flat",     sflat_vert,  sflat_frag);
	GL_InitProgram(&gl_shader_world,    "world",    sworld_vert, sworld_frag);
	GL_InitProgram(&gl_shader_alias,    "alias",    salias_vert, salias_frag);
	GL_InitProgram(&gl_shader_particle, "particle", spart_vert,  spart_frag);
	GL_InitProgram(&gl_shader_sky,      "sky",      ssky_vert,   ssky_frag);

}

void GL_Shaders_Shutdown (void)
{
	glprogram_t *progs[] = {
		&gl_shader_2d, &gl_shader_flat, &gl_shader_world,
		&gl_shader_alias, &gl_shader_particle, &gl_shader_sky
	};
	int i;

	for (i = 0; i < 6; i++)
	{
		if (progs[i]->program)
		{
			glDeleteProgram_fp(progs[i]->program);
			progs[i]->program = 0;
		}
	}
}
