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
#include "gl_matrix.h"
#include "gl_shader.h"

extern float	r_fog_density;
extern float	r_fog_color[3];

glprogram_t	gl_shader_world;
glprogram_t	gl_shader_alias;
glprogram_t	gl_shader_2d;
glprogram_t	gl_shader_particle;
glprogram_t	gl_shader_flat;
glprogram_t	gl_shader_sky;
gl_particle_gpu_prog_t	gl_shader_particle_gpu;
gl_alias_gpu_prog_t	gl_shader_alias_gpu;

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
	p->u_skyfog          = glGetUniformLocation_fp(p->program, "u_skyfog");
	p->u_eyepos          = glGetUniformLocation_fp(p->program, "u_eyepos");
}

/* ------------------------------------------------------------------ */
/* Shader sources                                                      */
/* ------------------------------------------------------------------ */

/* GLSL version header: desktop GL 4.3 vs WebGL2 (ES 3.0) */
#ifdef __EMSCRIPTEN__
#define GLSL_VERT_HEADER	"#version 300 es\nprecision highp float;\n"
#define GLSL_FRAG_HEADER	"#version 300 es\nprecision mediump float;\n"
#else
#define GLSL_VERT_HEADER	"#version 430 core\n"
#define GLSL_FRAG_HEADER	"#version 430 core\n"
#endif

/* --- shader_2d: orthographic HUD/text rendering --- */
static const char s2d_vert[] =
	GLSL_VERT_HEADER
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
	GLSL_FRAG_HEADER
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
	GLSL_VERT_HEADER
	"in vec3 a_position;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char sflat_frag[] =
	GLSL_FRAG_HEADER
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = v_color;\n"
	"}\n";

/* --- shader_world: textured + lightmap multitexture, with fog --- */
static const char sworld_vert[] =
	GLSL_VERT_HEADER
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
	GLSL_FRAG_HEADER
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
	"    float fog = exp(-u_fog_density * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_alias: vertex-colored, textured, fog (models) --- */
static const char salias_vert[] =
	GLSL_VERT_HEADER
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
	GLSL_FRAG_HEADER
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
	"    float fog = exp(-u_fog_density * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

#ifndef __EMSCRIPTEN__  /* SSBO shaders require GL 4.3 — not available in WebGL2 */
/* --- shader_alias_gpu: SSBO-driven alias models with GPU pose lerp --- */
static const char salias_gpu_vert[] =
	"#version 430 core\n"
	"\n"
	"struct PoseVert {\n"
	"    uint packed;  /* byte v[3] + byte lightnormalindex, packed into uint */\n"
	"};\n"
	"\n"
	"layout(std430, binding = 1) readonly buffer PoseBuffer {\n"
	"    PoseVert poses[];\n"
	"};\n"
	"\n"
	"in vec2 a_texcoord;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"uniform int u_pose0;\n"
	"uniform int u_pose1;\n"
	"uniform float u_lerp;\n"
	"uniform vec3 u_scale;\n"
	"uniform vec3 u_scale_origin;\n"
	"uniform int u_poseverts;\n"
	"uniform float u_shade_light;\n"
	"uniform vec3 u_light_color;\n"
	"uniform float u_ent_alpha;\n"
	"uniform float u_fog_density;\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"\n"
	"/* Quantized normal dot products (SHADEDOT_QUANT=16, 256 normals) */\n"
	"layout(std430, binding = 2) readonly buffer ShadeDots {\n"
	"    float shadedots[256];\n"
	"};\n"
	"\n"
	"void main() {\n"
	"    int vid = gl_VertexID;\n"
	"    uint p0 = poses[u_pose0 * u_poseverts + vid].packed;\n"
	"    uint p1 = poses[u_pose1 * u_poseverts + vid].packed;\n"
	"\n"
	"    /* Unpack trivertx_t: byte v[3] + byte lightnormalindex */\n"
	"    vec3 v0 = vec3(float(p0 & 0xFFu), float((p0 >> 8) & 0xFFu), float((p0 >> 16) & 0xFFu));\n"
	"    vec3 v1 = vec3(float(p1 & 0xFFu), float((p1 >> 8) & 0xFFu), float((p1 >> 16) & 0xFFu));\n"
	"    uint ni0 = (p0 >> 24) & 0xFFu;\n"
	"\n"
	"    /* Lerp between poses and decompress */\n"
	"    vec3 pos = mix(v1, v0, u_lerp) * u_scale + u_scale_origin;\n"
	"\n"
	"    /* Lighting from shadedots table */\n"
	"    float l = shadedots[ni0] * u_shade_light;\n"
	"    v_color = vec4(u_light_color * l, u_ent_alpha);\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	"    vec4 eyepos = u_modelview * vec4(pos, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
	"}\n";

/* --- shader_particle: textured triangles with per-vertex color --- */
static const char spart_vert[] =
	GLSL_VERT_HEADER
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
	GLSL_FRAG_HEADER
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
	"    float fog = exp(-u_fog_density * v_fogdist);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

/* --- shader_particle_gpu: SSBO-driven billboard particles ---
 * Vertex shader reads particle state from an SSBO (binding=0).
 * Each particle uses 3 vertices (gl_VertexID/3 = particle index).
 * Dead particles (die < u_ctime) become zero-area triangles. */
static const char spart_gpu_vert[] =
	"#version 430 core\n"
	"\n"
	"struct GpuParticle {\n"
	"    vec4 pos_die;  /* xyz=position, w=die_time */\n"
	"    vec4 color;    /* rgba, pre-converted from palette */\n"
	"};\n"
	"\n"
	"layout(std430, binding = 0) readonly buffer GpuParticleBuffer {\n"
	"    GpuParticle particles[];\n"
	"};\n"
	"\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"uniform vec3 u_pup;\n"
	"uniform vec3 u_pright;\n"
	"uniform vec3 u_vpn;\n"
	"uniform vec3 u_origin;\n"
	"uniform float u_ctime;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"\n"
	"/* Default particle texcoords (ptex_coord[0] from r_part.c) */\n"
	"const vec2 ptc[3] = vec2[3](\n"
	"    vec2(1.0, 0.0),\n"
	"    vec2(1.0, 0.5),\n"
	"    vec2(0.5, 0.0)\n"
	");\n"
	"\n"
	"void main() {\n"
	"    int pidx  = gl_VertexID / 3;\n"
	"    int corner = gl_VertexID % 3;\n"
	"    GpuParticle p = particles[pidx];\n"
	"\n"
	"    /* Dead particle -> degenerate (zero-area) triangle */\n"
	"    if (p.pos_die.w < u_ctime) {\n"
	"        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"        v_color     = vec4(0.0);\n"
	"        v_texcoord  = vec2(0.0);\n"
	"        v_fogdist   = 0.0;\n"
	"        return;\n"
	"    }\n"
	"\n"
	"    vec3 base = p.pos_die.xyz;\n"
	"\n"
	"    /* Distance-based scale (mirrors the CPU hack in r_part.c) */\n"
	"    float depth = dot(base - u_origin, u_vpn);\n"
	"    float scale = (depth < 20.0) ? 1.0 : 1.0 + depth * 0.004;\n"
	"\n"
	"    vec3 pos;\n"
	"    if (corner == 0)      pos = base;\n"
	"    else if (corner == 1) pos = base + u_pup    * scale;\n"
	"    else                  pos = base + u_pright * scale;\n"
	"\n"
	"    v_texcoord = ptc[corner];\n"
	"    v_color    = p.color;\n"
	"\n"
	"    vec4 eyepos = u_modelview * vec4(pos, 1.0);\n"
	"    v_fogdist   = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
	"}\n";
#endif /* !__EMSCRIPTEN__ */

/* --- shader_alias_instanced: ES 3.0 compatible instanced alias models ---
 * Pose data in a R32UI texture (texelFetch).
 * Shadedots in a UBO (16 rows × 256 floats = 16KB).
 * Per-instance data via vertex attribute divisors.
 * Attribute layout:
 *   0: (unused, vertex ID implicit)
 *   1: a_texcoord (vec2, per-vertex)
 *   2-5: a_inst_mvp (mat4, per-instance, divisor=1)
 *   6-9: a_inst_mv (mat4, per-instance, divisor=1)
 *   10: a_inst_scale_shade (vec4: scale.xyz + shade_light, per-instance)
 *   11: a_inst_origin_alpha (vec4: scale_origin.xyz + ent_alpha, per-instance)
 *   12: a_inst_light_lerp (vec4: light_color.xyz + lerp, per-instance)
 *   13: a_inst_poses (ivec4: pose0, pose1, shadedot_row, pad, per-instance)
 */
static const char salias_inst_vert[] =
	GLSL_VERT_HEADER
#ifdef __EMSCRIPTEN__
	"precision highp usampler2D;\n"
#endif
	"\n"
	"in vec2 a_texcoord;\n"
	"\n"
	"/* per-instance attributes (divisor=1) */\n"
	"in mat4 a_inst_mvp;\n"        /* locations 2-5 */
	"in mat4 a_inst_mv;\n"         /* locations 6-9 */
	"in vec4 a_inst_scale_shade;\n" /* location 10 */
	"in vec4 a_inst_origin_alpha;\n" /* location 11 */
	"in vec4 a_inst_light_lerp;\n" /* location 12 */
	"in ivec4 a_inst_poses;\n"     /* location 13 */
	"\n"
	"uniform usampler2D u_pose_tex;\n"  /* R32UI: width=poseverts, height=numposes */
	"\n"
	"/* Shadedots UBO: 16 quantized rows × 256 normal dot products */\n"
	"layout(std140) uniform ShadeDots {\n"
	"    vec4 shadedots[1024];\n"  /* 16*256/4 = 1024 vec4s = 16KB */
	"};\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"\n"
	"void main() {\n"
	"    int vid = gl_VertexID;\n"
	"    int pose0 = a_inst_poses.x;\n"
	"    int pose1 = a_inst_poses.y;\n"
	"    int sdot_row = a_inst_poses.z;\n"
	"    float lerp = a_inst_light_lerp.w;\n"
	"\n"
	"    /* Fetch packed pose vertices from texture */\n"
	"    uint p0 = texelFetch(u_pose_tex, ivec2(vid, pose0), 0).r;\n"
	"    uint p1 = texelFetch(u_pose_tex, ivec2(vid, pose1), 0).r;\n"
	"\n"
	"    /* Unpack: v[0..2] as bytes, lightnormalindex in top byte */\n"
	"    vec3 v0 = vec3(float(p0 & 0xFFu), float((p0>>8)&0xFFu), float((p0>>16)&0xFFu));\n"
	"    vec3 v1 = vec3(float(p1 & 0xFFu), float((p1>>8)&0xFFu), float((p1>>16)&0xFFu));\n"
	"    uint ni = (p0 >> 24) & 0xFFu;\n"
	"\n"
	"    /* Lerp between poses and decompress */\n"
	"    vec3 pos = mix(v1, v0, lerp) * a_inst_scale_shade.xyz + a_inst_origin_alpha.xyz;\n"
	"\n"
	"    /* Lighting: look up shadedot from UBO */\n"
	"    int sdot_idx = sdot_row * 256 + int(ni);\n"
	"    float sdot = shadedots[sdot_idx / 4][sdot_idx % 4];\n"
	"    float l = sdot * a_inst_scale_shade.w;\n"
	"    v_color = vec4(a_inst_light_lerp.xyz * l, a_inst_origin_alpha.w);\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	"    vec4 eyepos = a_inst_mv * vec4(pos, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = a_inst_mvp * vec4(pos, 1.0);\n"
	"}\n";

/* --- shader_sky: two-layer scrolling sky (solid + alpha) --- */
static const char ssky_vert[] =
	GLSL_VERT_HEADER
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec2 a_lmcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform vec3 u_eyepos;\n"
	"out vec3 v_dir;\n"
	"out vec2 v_texcoord;\n"
	"out vec2 v_lmcoord;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    vec3 dir = a_position - u_eyepos;\n"
	"    dir.z *= 3.0;\n"
	"    v_dir = dir;\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_lmcoord = a_lmcoord;\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char ssky_frag[] =
	GLSL_FRAG_HEADER
	"uniform sampler2D u_texture0;\n"
	"uniform sampler2D u_texture1;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_fog_density;\n"
	"uniform vec4 u_skyfog;\n"
	"uniform float u_time;\n"
	"uniform float u_alpha_threshold;\n"
	"in vec3 v_dir;\n"
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    if (u_alpha_threshold > 0.5) {\n"
	"        vec4 solid = texture(u_texture0, v_texcoord);\n"
	"        fragColor = solid * v_color;\n"
	"    } else {\n"
	"        vec2 uv = normalize(v_dir).xy * (189.0 / 64.0);\n"
	"        vec4 solid = texture(u_texture0, uv + u_time / 16.0);\n"
	"        vec4 layer = texture(u_texture1, uv + u_time / 8.0);\n"
	"        vec3 color = mix(solid.rgb, layer.rgb, layer.a);\n"
	"        color = mix(color, u_skyfog.rgb, u_skyfog.a);\n"
	"        fragColor = vec4(color, 1.0) * v_color;\n"
	"    }\n"
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

#ifndef __EMSCRIPTEN__
static qboolean GL_InitParticleGPUProgram (gl_particle_gpu_prog_t *p)
{
	p->base.program = GL_LoadProgram(spart_gpu_vert, spart_frag);
	if (!p->base.program)
	{
		Con_Printf("Failed to load shader: particle_gpu\n");
		return false;
	}
	GL_InitProgramUniforms(&p->base);

	/* Look up extra uniforms */
	p->u_pup    = glGetUniformLocation_fp(p->base.program, "u_pup");
	p->u_pright = glGetUniformLocation_fp(p->base.program, "u_pright");
	p->u_vpn    = glGetUniformLocation_fp(p->base.program, "u_vpn");
	p->u_origin = glGetUniformLocation_fp(p->base.program, "u_origin");
	p->u_ctime  = glGetUniformLocation_fp(p->base.program, "u_ctime");

	/* Bind texture unit defaults */
	glUseProgram_fp(p->base.program);
	if (p->base.u_texture0 >= 0) glUniform1i_fp(p->base.u_texture0, 0);
	if (p->base.u_fog_density >= 0) glUniform1f_fp(p->base.u_fog_density, 0.0f);
	glUseProgram_fp(0);

	Con_SafePrintf("  shader 'particle_gpu' loaded (program %u)\n", p->base.program);
	return true;
}

void GL_ParticleGPU_SetUniforms (const gl_particle_gpu_prog_t *prog,
				  const float *pup, const float *pright,
				  const float *vpn, const float *origin,
				  float ctime)
{
	float mvp[16], mv[16];

	GL_GetMVP(mvp);
	GL_GetModelview(mv);

	if (prog->base.u_mvp >= 0)
		glUniformMatrix4fv_fp(prog->base.u_mvp, 1, GL_FALSE, mvp);
	if (prog->base.u_modelview >= 0)
		glUniformMatrix4fv_fp(prog->base.u_modelview, 1, GL_FALSE, mv);
	if (prog->base.u_fog_density >= 0)
		glUniform1f_fp(prog->base.u_fog_density, r_fog_density);
	if (prog->base.u_fog_color >= 0)
		glUniform3f_fp(prog->base.u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (prog->u_pup >= 0)
		glUniform3f_fp(prog->u_pup,    pup[0],    pup[1],    pup[2]);
	if (prog->u_pright >= 0)
		glUniform3f_fp(prog->u_pright, pright[0], pright[1], pright[2]);
	if (prog->u_vpn >= 0)
		glUniform3f_fp(prog->u_vpn,    vpn[0],    vpn[1],    vpn[2]);
	if (prog->u_origin >= 0)
		glUniform3f_fp(prog->u_origin, origin[0], origin[1], origin[2]);
	if (prog->u_ctime >= 0)
		glUniform1f_fp(prog->u_ctime, ctime);
}

static qboolean GL_InitAliasGPUProgram (gl_alias_gpu_prog_t *p)
{
	/* Reuse the existing alias fragment shader */
	p->base.program = GL_LoadProgram(salias_gpu_vert, salias_frag);
	if (!p->base.program)
	{
		Con_Printf("Failed to load shader: alias_gpu\n");
		return false;
	}
	GL_InitProgramUniforms(&p->base);

	p->u_pose0        = glGetUniformLocation_fp(p->base.program, "u_pose0");
	p->u_pose1        = glGetUniformLocation_fp(p->base.program, "u_pose1");
	p->u_lerp         = glGetUniformLocation_fp(p->base.program, "u_lerp");
	p->u_scale        = glGetUniformLocation_fp(p->base.program, "u_scale");
	p->u_scale_origin = glGetUniformLocation_fp(p->base.program, "u_scale_origin");
	p->u_poseverts    = glGetUniformLocation_fp(p->base.program, "u_poseverts");
	p->u_shade_light  = glGetUniformLocation_fp(p->base.program, "u_shade_light");
	p->u_light_color  = glGetUniformLocation_fp(p->base.program, "u_light_color");
	p->u_ent_alpha    = glGetUniformLocation_fp(p->base.program, "u_ent_alpha");

	glUseProgram_fp(p->base.program);
	if (p->base.u_texture0 >= 0) glUniform1i_fp(p->base.u_texture0, 0);
	if (p->base.u_fog_density >= 0) glUniform1f_fp(p->base.u_fog_density, 0.0f);
	glUseProgram_fp(0);

	Con_SafePrintf("  shader 'alias_gpu' loaded (program %u)\n", p->base.program);
	return true;
}

void GL_AliasGPU_SetUniforms (const gl_alias_gpu_prog_t *prog,
			       int pose0, int pose1, float lerp,
			       const float *scale, const float *scale_origin,
			       int poseverts, float shade_light,
			       const float *light_color, float alpha)
{
	float mvp[16], mv[16];

	GL_GetMVP(mvp);
	GL_GetModelview(mv);

	if (prog->base.u_mvp >= 0)
		glUniformMatrix4fv_fp(prog->base.u_mvp, 1, GL_FALSE, mvp);
	if (prog->base.u_modelview >= 0)
		glUniformMatrix4fv_fp(prog->base.u_modelview, 1, GL_FALSE, mv);
	if (prog->base.u_fog_density >= 0)
		glUniform1f_fp(prog->base.u_fog_density, r_fog_density);
	if (prog->base.u_fog_color >= 0)
		glUniform3f_fp(prog->base.u_fog_color, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (prog->u_pose0 >= 0)
		glUniform1i_fp(prog->u_pose0, pose0);
	if (prog->u_pose1 >= 0)
		glUniform1i_fp(prog->u_pose1, pose1);
	if (prog->u_lerp >= 0)
		glUniform1f_fp(prog->u_lerp, lerp);
	if (prog->u_scale >= 0)
		glUniform3f_fp(prog->u_scale, scale[0], scale[1], scale[2]);
	if (prog->u_scale_origin >= 0)
		glUniform3f_fp(prog->u_scale_origin, scale_origin[0], scale_origin[1], scale_origin[2]);
	if (prog->u_poseverts >= 0)
		glUniform1i_fp(prog->u_poseverts, poseverts);
	if (prog->u_shade_light >= 0)
		glUniform1f_fp(prog->u_shade_light, shade_light);
	if (prog->u_light_color >= 0)
		glUniform3f_fp(prog->u_light_color, light_color[0], light_color[1], light_color[2]);
	if (prog->u_ent_alpha >= 0)
		glUniform1f_fp(prog->u_ent_alpha, alpha);
}
#endif /* !__EMSCRIPTEN__ */

gl_alias_inst_prog_t gl_shader_alias_inst;

/* Shadedots table — defined in gl_rmain.c */
extern float r_avertexnormal_dots[16][256];

static qboolean GL_InitAliasInstProgram (gl_alias_inst_prog_t *p)
{
	GLuint prog;
	int i;

	prog = GL_LoadProgram(salias_inst_vert, salias_frag);
	if (!prog)
		return false;

	/* Bind attribute locations before linking won't work (already linked).
	 * Instead, query and verify locations match our layout. Use
	 * glBindAttribLocation before link in a custom path. */
	/* Actually, we need to set attrib locations before linking.
	 * Re-do with explicit binding: */
	glDeleteProgram_fp(prog);

	{
		GLuint vs, fs;
		vs = GL_CompileShader(GL_VERTEX_SHADER, salias_inst_vert);
		fs = GL_CompileShader(GL_FRAGMENT_SHADER, salias_frag);
		if (!vs || !fs) {
			if (vs) glDeleteShader_fp(vs);
			if (fs) glDeleteShader_fp(fs);
			return false;
		}
		prog = glCreateProgram_fp();
		glAttachShader_fp(prog, vs);
		glAttachShader_fp(prog, fs);

		/* Bind per-vertex attributes */
		glBindAttribLocation_fp(prog, ATTR_TEXCOORD, "a_texcoord");

		/* Bind per-instance attributes (mat4 occupies 4 consecutive locations) */
		glBindAttribLocation_fp(prog, ATTR_INST_MVP0, "a_inst_mvp");
		glBindAttribLocation_fp(prog, ATTR_INST_MV0, "a_inst_mv");
		glBindAttribLocation_fp(prog, ATTR_INST_SCALE_SHADE, "a_inst_scale_shade");
		glBindAttribLocation_fp(prog, ATTR_INST_ORIGIN_ALPHA, "a_inst_origin_alpha");
		glBindAttribLocation_fp(prog, ATTR_INST_LIGHT_LERP, "a_inst_light_lerp");
		glBindAttribLocation_fp(prog, ATTR_INST_POSES, "a_inst_poses");

		glLinkProgram_fp(prog);
		glDeleteShader_fp(vs);
		glDeleteShader_fp(fs);

		{
			GLint status;
			glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
			if (!status) {
				char log[1024];
				glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
				Sys_Printf("alias_instanced link failed: %s\n", log);
				glDeleteProgram_fp(prog);
				return false;
			}
		}
	}

	memset(p, 0, sizeof(*p));
	p->base.program = prog;
	p->base.u_fog_density = glGetUniformLocation_fp(prog, "u_fog_density");
	p->base.u_fog_color = glGetUniformLocation_fp(prog, "u_fog_color");
	p->base.u_alpha_threshold = glGetUniformLocation_fp(prog, "u_alpha_threshold");
	p->base.u_texture0 = glGetUniformLocation_fp(prog, "u_texture0");
	p->u_pose_tex = glGetUniformLocation_fp(prog, "u_pose_tex");

	/* Set texture unit defaults */
	glUseProgram_fp(prog);
	if (p->base.u_texture0 >= 0) glUniform1i_fp(p->base.u_texture0, 0);
	if (p->u_pose_tex >= 0) glUniform1i_fp(p->u_pose_tex, 1);
	glUseProgram_fp(0);

	/* UBO for shadedots */
	p->ubo_block_idx = glGetUniformBlockIndex_fp(prog, "ShadeDots");
	if (p->ubo_block_idx != GL_INVALID_INDEX)
		glUniformBlockBinding_fp(prog, p->ubo_block_idx, 0);

	/* Create and fill shadedots UBO (static data, upload once) */
	glGenBuffers_fp(1, &p->ubo_shadedots);
	glBindBuffer_fp(GL_UNIFORM_BUFFER, p->ubo_shadedots);
	/* std140 requires vec4 alignment — the float[16][256] data is already
	 * tightly packed, and we read it as shadedots[idx/4][idx%4] in the shader */
	glBufferData_fp(GL_UNIFORM_BUFFER, 16 * 256 * sizeof(float),
			r_avertexnormal_dots, GL_STATIC_DRAW);
	glBindBuffer_fp(GL_UNIFORM_BUFFER, 0);

	Sys_Printf("  alias_instanced: OK (prog=%u, ubo=%u)\n", prog, p->ubo_shadedots);
	return true;
}

void GL_AliasInst_Init (void)
{
	if (!GL_InitAliasInstProgram(&gl_shader_alias_inst))
		Sys_Printf("WARNING: instanced alias shader failed to init\n");
}

void GL_AliasInst_Shutdown (void)
{
	if (gl_shader_alias_inst.base.program)
		glDeleteProgram_fp(gl_shader_alias_inst.base.program);
	if (gl_shader_alias_inst.ubo_shadedots)
		glDeleteBuffers_fp(1, &gl_shader_alias_inst.ubo_shadedots);
	memset(&gl_shader_alias_inst, 0, sizeof(gl_shader_alias_inst));
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
#ifndef __EMSCRIPTEN__
	GL_InitParticleGPUProgram(&gl_shader_particle_gpu);
	GL_InitAliasGPUProgram(&gl_shader_alias_gpu);
#endif
	GL_AliasInst_Init();
}

void GL_Shaders_Shutdown (void)
{
	glprogram_t *progs[] = {
		&gl_shader_2d, &gl_shader_flat, &gl_shader_world,
		&gl_shader_alias, &gl_shader_particle, &gl_shader_sky,
		&gl_shader_particle_gpu.base, &gl_shader_alias_gpu.base
	};
	int i;

	for (i = 0; i < 8; i++)
	{
		if (progs[i]->program)
		{
			glDeleteProgram_fp(progs[i]->program);
			progs[i]->program = 0;
		}
	}
	GL_AliasInst_Shutdown();
}
