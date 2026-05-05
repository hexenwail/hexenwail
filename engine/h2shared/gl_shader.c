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

/* OIT variants of translucent shaders */
glprogram_t	gl_shader_world_oit;
glprogram_t	gl_shader_alias_oit;
glprogram_t	gl_shader_particle_oit;
gl_particle_gpu_prog_t	gl_shader_particle_gpu;

/* Forward declarations */
static void GL_InitProgramUniforms (glprogram_t *p);

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

/* Compile an OIT variant of a fragment shader.
 * Injects `#define OIT 1` and OIT MRT outputs after the #version line,
 * replacing the `out vec4 fragColor;` declaration. */
static GLuint GL_CompileOITFragShader (const char *frag_src)
{
	/* The OIT output block (inserted after #version, before the rest).
	 * Uses the main→main_body rename trick from Ironwail. */
	static const char oit_preamble[] =
		"#define OIT 1\n"
		"vec4 fragColor;\n"
		"layout(location=0) out vec4 out_accum;\n"
		"layout(location=1) out float out_reveal;\n"
		"void main_body();\n"
		"void main() {\n"
		"    main_body();\n"
		"    fragColor = clamp(fragColor, 0.0, 1.0);\n"
		"    float z = 1.0 / gl_FragCoord.w;\n"
		"    float w = clamp(fragColor.a * fragColor.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);\n"
		"    out_accum = vec4(fragColor.rgb * fragColor.a * w, fragColor.a * w);\n"
		"    out_reveal = fragColor.a;\n"
		"}\n"
		"#define main main_body\n";

	/* Find end of #version line */
	const char *rest = strchr(frag_src, '\n');
	if (!rest) return 0;
	rest++; /* skip past newline */

	/* Build modified source: version line + OIT preamble + rest (minus `out vec4 fragColor;\n`) */
	{
		/* Skip the `out vec4 fragColor;\n` line in the rest */
		const char *skip = strstr(rest, "out vec4 fragColor;\n");
		if (!skip) return 0;
		skip += strlen("out vec4 fragColor;\n");

		/* Allocate buffer: version + preamble + (rest before fragColor) + (rest after fragColor) */
		size_t ver_len = rest - frag_src;
		size_t before_len = skip - strlen("out vec4 fragColor;\n") - rest;
		size_t after_start = skip - frag_src;
		size_t total = ver_len + strlen(oit_preamble) + before_len + strlen(skip) + 1;
		char *buf = (char *)malloc(total);
		GLuint shader;

		memcpy(buf, frag_src, ver_len);
		memcpy(buf + ver_len, oit_preamble, strlen(oit_preamble));
		/* Copy everything between version line and "out vec4 fragColor;\n" */
		memcpy(buf + ver_len + strlen(oit_preamble), rest, before_len);
		/* Copy everything after "out vec4 fragColor;\n" */
		strcpy(buf + ver_len + strlen(oit_preamble) + before_len, skip);

		shader = GL_CompileShader(GL_FRAGMENT_SHADER, buf);
		free(buf);
		return shader;
	}
}

static GLuint GL_LoadOITProgram (const char *vert_src, const char *frag_src)
{
	GLuint vs, fs, prog;

	vs = GL_CompileShader(GL_VERTEX_SHADER, vert_src);
	if (!vs) return 0;
	fs = GL_CompileOITFragShader(frag_src);
	if (!fs) { glDeleteShader_fp(vs); return 0; }

	prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	return prog;
}

static void GL_InitOITProgram (glprogram_t *p, const char *name,
			       const char *vert_src, const char *frag_src)
{
	p->program = GL_LoadOITProgram(vert_src, frag_src);
	if (p->program)
	{
		GL_InitProgramUniforms(p);
		Con_SafePrintf("  %s_oit: OK (prog=%u)\n", name, p->program);
	}
	else
		Con_SafePrintf("  %s_oit: FAILED\n", name);
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
/* GLSL ES 3.00 doesn't support early_fragment_tests */
#define GLSL_EARLY_Z		""
#else
#define GLSL_VERT_HEADER	"#version 430 core\n"
#define GLSL_FRAG_HEADER	"#version 430 core\n"
/* Force depth test BEFORE fragment shader runs even when shader uses
 * `discard`. Lets the GPU exploit the front-to-back BSP traversal's
 * Hi-Z without `discard` defeating early-Z. GLSL 4.20+. No measured
 * benefit on flat coliseum scenes (BSP gives Hi-Z already), no harm. */
#define GLSL_EARLY_Z		"layout(early_fragment_tests) in;\n"
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
	GLSL_EARLY_Z
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
	/* For cutout alpha-test (threshold > 0.5 = fence/holey, A2C enabled):
	 * surviving fragments are by definition opaque, so force alpha=1 to
	 * stop A2C from dithering their coverage based on the noisy
	 * lightmap.a × tex.a × v_color.a multiply.
	 * For non-cutout draws (threshold ~ 0.01): preserve actual alpha so
	 * GL_BLEND with GL_SRC_ALPHA works for translucent surfaces.
	 * OIT path always preserves alpha for weighted blending. */
	"#ifdef OIT\n"
	"    fragColor = color;\n"
	"#else\n"
	"    fragColor = vec4(color.rgb, u_alpha_threshold > 0.5 ? 1.0 : color.a);\n"
	"#endif\n"
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
	/* See sworld_frag — force alpha=1 only on the cutout path so A2C
	 * doesn't dither.  Translucent draws (sprites, EF_TRANSPARENT alias
	 * models, ENTALPHA, etc.) keep actual alpha for blend. */
	"#ifdef OIT\n"
	"    fragColor = color;\n"
	"#else\n"
	"    fragColor = vec4(color.rgb, u_alpha_threshold > 0.5 ? 1.0 : color.a);\n"
	"#endif\n"
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

#ifndef __EMSCRIPTEN__  /* SSBO shaders require GL 4.3 — not available in WebGL2 */
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

/* --- shader_alias_instanced: GL 4.3 SSBO-based instanced alias models ---
 * Instance data and frame globals in SSBO binding 0.
 * Pose data in SSBO binding 1 (per-batch, from model's ssbo_pose).
 * Shadedots in UBO binding 0.
 * Scale/origin baked into the world matrix CPU-side.
 * 80-byte instance struct matching Ironwail's compact layout.
 */
#ifndef __EMSCRIPTEN__
static const char salias_inst_vert[] =
	"#version 430 core\n"
	"\n"
	"struct InstanceData {\n"
	"    vec4 WorldMatrix0;\n"
	"    vec4 WorldMatrix1;\n"
	"    vec4 WorldMatrix2;\n"
	"    vec4 LightAlpha;\n"      /* rgb = light_color, a = alpha */
	"    int Pose0;\n"
	"    int Pose1;\n"
	"    float Blend;\n"
	"    int ShadedotRow;\n"
	"};\n"
	"\n"
	"layout(std430, binding=0) restrict readonly buffer InstanceBuffer {\n"
	"    mat4 ViewProj;\n"
	"    InstanceData instances[];\n"
	"};\n"
	"\n"
	"layout(std430, binding=1) restrict readonly buffer PoseBuffer {\n"
	"    uint pose_data[];\n"
	"};\n"
	"\n"
	"layout(std430, binding=2) restrict readonly buffer ShadeDots {\n"
	"    float shadedots[4096];\n"
	"};\n"
	"\n"
	"in vec2 a_texcoord;\n"
	"\n"
	"uniform int u_inst_base;\n"
	"uniform vec3 u_eyepos;\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"\n"
	"void main() {\n"
	"    InstanceData inst = instances[u_inst_base + gl_InstanceID];\n"
	"\n"
	"    uint p0 = pose_data[inst.Pose0 + gl_VertexID];\n"
	"    uint p1 = pose_data[inst.Pose1 + gl_VertexID];\n"
	"\n"
	"    vec3 v0 = vec3(float(p0 & 0xFFu), float((p0>>8)&0xFFu), float((p0>>16)&0xFFu));\n"
	"    vec3 v1 = vec3(float(p1 & 0xFFu), float((p1>>8)&0xFFu), float((p1>>16)&0xFFu));\n"
	"    uint ni = (p0 >> 24) & 0xFFu;\n"
	"\n"
	"    vec3 local_pos = mix(v1, v0, inst.Blend);\n"
	"\n"
	"    mat4x3 world = transpose(mat3x4(\n"
	"        inst.WorldMatrix0, inst.WorldMatrix1, inst.WorldMatrix2));\n"
	"    vec3 world_pos = world * vec4(local_pos, 1.0);\n"
	"\n"
	"    int sdot_idx = inst.ShadedotRow * 256 + int(ni);\n"
	"    float sdot = shadedots[sdot_idx];\n"
	"    sdot = max(sdot, 0.0);\n"
	"    v_color = vec4(inst.LightAlpha.rgb * sdot, inst.LightAlpha.a);\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_fogdist = distance(world_pos, u_eyepos);\n"
	"    gl_Position = ViewProj * vec4(world_pos, 1.0);\n"
	"}\n";
#endif /* !__EMSCRIPTEN__ */

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
#endif /* !__EMSCRIPTEN__ */

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

gl_alias_inst_prog_t gl_shader_alias_inst;

/* Shadedots table — defined in gl_rmain.c */
extern float r_avertexnormal_dots[16][256];

#ifndef __EMSCRIPTEN__
static qboolean GL_InitAliasInstProgram (gl_alias_inst_prog_t *p)
{
	GLuint vs, fs, prog;
	GLuint ubo_block_idx;

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

	/* Only per-vertex attribute is texcoord */
	glBindAttribLocation_fp(prog, ATTR_TEXCOORD, "a_texcoord");

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

	memset(p, 0, sizeof(*p));
	p->program = prog;
	p->u_fog_density = glGetUniformLocation_fp(prog, "u_fog_density");
	p->u_fog_color = glGetUniformLocation_fp(prog, "u_fog_color");
	p->u_alpha_threshold = glGetUniformLocation_fp(prog, "u_alpha_threshold");
	p->u_inst_base = glGetUniformLocation_fp(prog, "u_inst_base");
	p->u_eyepos = glGetUniformLocation_fp(prog, "u_eyepos");

	/* Bind skin texture to unit 0 */
	glUseProgram_fp(prog);
	{
		GLint u_tex = glGetUniformLocation_fp(prog, "u_texture0");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 0);
	}
	glUseProgram_fp(0);

	/* Create and fill shadedots SSBO (static data, upload once).
	 * Uses binding=2 to match the non-instanced GPU alias shader. */
	glGenBuffers_fp(1, &p->ubo_shadedots);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, p->ubo_shadedots);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER, 16 * 256 * sizeof(float),
			r_avertexnormal_dots, GL_STATIC_DRAW);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);

	Sys_Printf("  alias_instanced: OK (prog=%u, ubo=%u)\n", prog, p->ubo_shadedots);
	return true;
}
#endif /* !__EMSCRIPTEN__ */

void GL_AliasInst_Init (void)
{
#ifndef __EMSCRIPTEN__
	if (!GL_InitAliasInstProgram(&gl_shader_alias_inst))
		Sys_Printf("WARNING: instanced alias shader failed to init\n");
#endif
}

void GL_AliasInst_Shutdown (void)
{
	if (gl_shader_alias_inst.program)
		glDeleteProgram_fp(gl_shader_alias_inst.program);
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

	/* OIT variants for translucent rendering */
	GL_InitOITProgram(&gl_shader_world_oit,    "world",    sworld_vert, sworld_frag);
	GL_InitOITProgram(&gl_shader_alias_oit,    "alias",    salias_vert, salias_frag);
	GL_InitOITProgram(&gl_shader_particle_oit, "particle", spart_vert,  spart_frag);

#ifndef __EMSCRIPTEN__
	GL_InitParticleGPUProgram(&gl_shader_particle_gpu);
#endif
	GL_AliasInst_Init();
}

void GL_Shaders_Shutdown (void)
{
	glprogram_t *progs[] = {
		&gl_shader_2d, &gl_shader_flat, &gl_shader_world,
		&gl_shader_alias, &gl_shader_particle, &gl_shader_sky,
		&gl_shader_particle_gpu.base,
		&gl_shader_world_oit, &gl_shader_alias_oit, &gl_shader_particle_oit
	};
	int i;

	for (i = 0; i < 10; i++)
	{
		if (progs[i]->program)
		{
			glDeleteProgram_fp(progs[i]->program);
			progs[i]->program = 0;
		}
	}
	GL_AliasInst_Shutdown();
}
