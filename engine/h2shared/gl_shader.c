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
#include "gl_uniforms.h"
#include "gl_pipeline.h"

extern float	r_fog_density;
extern float	r_fog_color[3];

glprogram_t	gl_shader_world;
glprogram_t	gl_shader_world_opaque;	/* uhexen2-5c6r: early_fragment_tests, no discard */
glprogram_t	gl_shader_alias;
glprogram_t	gl_shader_skeletal;
glprogram_t	gl_shader_2d;
glprogram_t	gl_shader_particle;
glprogram_t	gl_shader_flat;
glprogram_t	gl_shader_sky;

/* null fullbright texture: 1x1 black RGBA bound at unit 2 for world
 * surfaces whose diffuse texture has no fullbright pixels.  Lets
 * sworld_frag unconditionally sample u_texture2 and add the result
 * (zero contribution) instead of branching per-fragment.  uhexen2-sjvf. */
GLuint		gl_null_fb_texture;

/* Solid white 1x1 RGBA.  The shaders that back the immediate-mode batches
 * all sample a texture unconditionally and multiply it into the vertex
 * colour, so a caller that wants a flat untextured draw has to hand them
 * something that multiplies by one.  Binding this is that "no texture". */
GLuint		gl_solid_white_texture;

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
	char log[2048];
	const char *type_name = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";

	shader = glCreateShader_fp(type);

	glShaderSource_fp(shader, 1, &source, NULL);

	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);

	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("[SHADER] %s COMPILE ERROR:\n%s\n", type_name, log);
		Con_Printf("[SHADER] First 300 chars of source:\n%.300s\n", source);
		glDeleteShader_fp(shader);
		return 0;
	}
	if (developer.integer)
		Con_SafePrintf("[SHADER] %s shader compiled OK (id=%u)\n", type_name, shader);
	return shader;
}

GLuint GL_LinkProgram (GLuint vert, GLuint frag)
{
	GLuint prog;
	GLint status;
	char log[2048];

	if (developer.integer)
		Con_SafePrintf("[SHADER] Linking program (vert=%u, frag=%u)\n", vert, frag);

	if (!vert || !frag)
	{
		Con_Printf("[SHADER] LINK ERROR: Invalid shader IDs (vert=%u, frag=%u)\n", vert, frag);
		return 0;
	}

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
		Con_Printf("[SHADER] LINK ERROR:\n%s\n", log);
		glDeleteProgram_fp(prog);
		return 0;
	}
	if (developer.integer)
		Con_SafePrintf("[SHADER] Program linked OK (id=%u)\n", prog);
	return prog;
}

/* The OIT program builders below are desktop-only: OIT cannot run on the ES
 * tier at all (no glBlendFunci in WebGL2), so nothing there would call
 * them.  See GL_Shaders_Init and uhexen2-ha7n. */
#ifndef USE_GLES
/* Compile an OIT variant of a fragment shader.
 * Injects `#define OIT 1` and OIT MRT outputs after the #version line,
 * replacing the `out vec4 fragColor;` declaration. */
static GLuint GL_CompileOITFragShader (const char *frag_src)
{
	/* The OIT output block (inserted after #version, before the rest).
	 * Uses the main→main_body rename trick from Ironwail. */
	/* fragColor is initialized rather than merely declared.  main() below
	 * reads it after calling main_body(), which is only a prototype at
	 * that point, so the compiler cannot see the assignment inside and
	 * correctly reports a read of an uninitialized global — three
	 * GL_DEBUG_SEVERITY_HIGH lines on every cold start.  More than noise:
	 * this wrapper is only correct because every shader it wraps happens
	 * to assign fragColor on every non-discard path, and nothing enforces
	 * that.  An early `return;` added to one of them later would yield
	 * undefined colour in OIT mode only.  Seeding it makes that case
	 * transparent black instead of garbage.  uhexen2-nkvm. */
	static const char oit_preamble[] =
		"#define OIT 1\n"
		"vec4 fragColor = vec4(0.0);\n"
		"layout(location=0) out vec4 out_accum;\n"
		"layout(location=1) out vec4 out_reveal;\n"
		"void main_body();\n"
		"void main() {\n"
		"    main_body();\n"
		"    fragColor = clamp(fragColor, 0.0, 1.0);\n"
		"    float z = 1.0 / gl_FragCoord.w;\n"
		"    float w = clamp(fragColor.a * fragColor.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);\n"
		"    out_accum = vec4(fragColor.rgb * fragColor.a * w, fragColor.a * w);\n"
		"    out_reveal = vec4(fragColor.a, 0.0, 0.0, 0.0);\n"
		"}\n"
		"#define main main_body\n";

	/* Find end of #version line */
	const char *rest = strchr(frag_src, '\n');
	if (!rest) return 0;
	rest++; /* skip past newline */

	/* ...and past any default-precision declarations following it.  GLSL ES
	 * requires `precision <qual> <type>;` to appear before the first use of
	 * that type, and the preamble spliced in below declares vec4s of its
	 * own.  Splicing between #version and the precision line therefore made
	 * every OIT fragment shader on the ES tier fail to compile with "No
	 * precision specified in this scope for type `vec4'", losing OIT for
	 * world, alias and particle translucency there.  GLSL_FRAG_HEADER
	 * carries no precision line on desktop, so this loop does nothing on
	 * that path.  uhexen2-g63k, surfaced by uhexen2-0py6. */
	while (!strncmp(rest, "precision ", 10))
	{
		const char *eol = strchr(rest, '\n');
		if (!eol) return 0;
		rest = eol + 1;
	}

	/* Build modified source: version line + OIT preamble + rest (minus `out vec4 fragColor;\n`) */
	{
		/* Skip the `out vec4 fragColor;\n` line in the rest */
		const char *skip = strstr(rest, "out vec4 fragColor;\n");
		if (!skip) return 0;
		skip += strlen("out vec4 fragColor;\n");

		/* Allocate buffer: version + preamble + (rest before fragColor) + (rest after fragColor) */
		size_t ver_len = rest - frag_src;
		size_t before_len = skip - strlen("out vec4 fragColor;\n") - rest;
		size_t total = ver_len + strlen(oit_preamble) + before_len + strlen(skip) + 1;
		char *buf = (char *)malloc(total);
		GLuint shader;

		if (!buf)
			Sys_Error("GL_CompileOITFragShader: out of memory");
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
		/* bind texture unit defaults (mirrors GL_InitProgram) */
		R_UseProgram (p->program);
		if (p->u_texture0 >= 0) glUniform1i_fp(p->u_texture0, 0);
		if (p->u_texture1 >= 0) glUniform1i_fp(p->u_texture1, 1);
		if (p->u_texture2 >= 0) glUniform1i_fp(p->u_texture2, 2);
		if (p->u_soft_depth >= 0) glUniform1i_fp(p->u_soft_depth, 1);	/* uhexen2-mf9u */
		R_UseProgram (0);
		Con_SafePrintf("  %s_oit: OK (prog=%u)\n", name, p->program);
	}
	else
	{
		Con_SafePrintf("  %s_oit: FAILED\n", name);
	}
}
#endif	/* !USE_GLES */


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
	p->u_texture0        = glGetUniformLocation_fp(p->program, "u_texture0");
	p->u_texture1        = glGetUniformLocation_fp(p->program, "u_texture1");
	p->u_texture2        = glGetUniformLocation_fp(p->program, "u_texture2");
	p->u_soft_depth      = glGetUniformLocation_fp(p->program, "u_soft_depth");
	R_BindProgramBlocks (p->program);
}

/* ------------------------------------------------------------------ */
/* Shader sources                                                      */
/* ------------------------------------------------------------------ */

/* GLSL version header: desktop GL 4.3 vs WebGL2 (ES 3.0) */
#ifdef USE_GLES
#define GLSL_VERT_HEADER	"#version 300 es\nprecision highp float;\n"
#define GLSL_FRAG_HEADER	"#version 300 es\nprecision mediump float;\n"
/* GLSL ES 3.00 doesn't support early_fragment_tests */
#define GLSL_EARLY_Z		""
#define GLSL_EARLY_Z_OPAQUE	""
#else
#define GLSL_VERT_HEADER	"#version 430 core\n"
#define GLSL_FRAG_HEADER	"#version 430 core\n"
/* Cutout shaders that use `discard` MUST NOT force early_fragment_tests:
 * early tests run depth+stencil — and write them — BEFORE the fragment
 * shader executes; a later `discard` cannot undo the depth write that
 * already happened.  For an alpha-tested fence (e.g. a func_illusionary
 * bush), every cutout pixel still wrote the bush's depth, occluding any
 * entity drawn after at that pixel even though no color was written there.
 * Visible on mill.bsp (SoT): bush silhouette z-rejected the tree behind
 * it.  uhexen2-238u. */
#define GLSL_EARLY_Z		""
/* Opaque-only variant has no discard, so early_fragment_tests is safe and
 * recovers Hi-Z on the world bucket (+0.34ms regression measured in
 * uhexen2-23a9).  Used by gl_shader_world_opaque (uhexen2-5c6r). */
#define GLSL_EARLY_Z_OPAQUE	"layout(early_fragment_tests) in;\n"
#endif

/* --- shader_2d: orthographic HUD/text rendering --- */
static const char s2d_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char s2d_frag[] =
	GLSL_FRAG_HEADER
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"
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
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec4 a_color;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char sflat_frag[] =
	GLSL_FRAG_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = v_color;\n"
	"}\n";

/* --- shader_world: textured + lightmap multitexture, with fog --- */
static const char sworld_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec2 a_lmcoord;\n"
	"in vec4 a_color;\n"
	"out vec2 v_texcoord;\n"
	"out vec2 v_lmcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	/* World-space XY for caustics sampling.  For world surfaces a_position
	 * IS world space.  For brush ents R_RotateForEntity multiplies into
	 * u_modelview, not into a_position, so a_position here is still
	 * model-local for brush ents — caustics will tile with the brush ent's
	 * local frame, which is acceptable for moving func_* ents (the effect
	 * is subtle and rarely visible mid-motion).  uhexen2-6bfm. */
	"out vec2 v_worldxy;\n"
	/* invariant gl_Position: pin position math so within-shader vertex
	 * transforms produce stable depth across draw calls.  Brush ents
	 * share this same compiled program (uhexen2-mf45), so the
	 * within-shader guarantee covers coplanar joins between brush ents
	 * and world surfaces — no cross-shader 1-ULP drift. */
	"invariant gl_Position;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_lmcoord = a_lmcoord;\n"
	"    v_color = a_color;\n"
	"    v_worldxy = a_position.xy;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

/* 4-tap B-spline bicubic lightmap fetch (Sigg/Hadwiger).  16-luxel Hexen II
 * lightmaps expose the bilinear lattice on big flat walls; bicubic
 * smooths it for ~3 extra texture fetches per fragment.  Cost gated by
 * u_lightmap_bicubic: uniform-static branch -> no work on the bilinear path.
 * Uses textureSize() so the helper is agnostic to atlas resolution.
 * uhexen2-b2f0. */
#define GLSL_BICUBIC_LM_FN \
	"vec4 BicubicLightmap(sampler2D s, vec2 uv) {\n" \
	"    vec2 ts = vec2(textureSize(s, 0));\n" \
	"    vec2 invts = 1.0 / ts;\n" \
	"    vec2 tc = uv * ts - 0.5;\n" \
	"    vec2 fr = fract(tc);\n" \
	"    vec2 ic = floor(tc) + 0.5;\n" \
	"    vec2 fr2 = fr * fr;\n" \
	"    vec2 fr3 = fr2 * fr;\n" \
	"    vec2 omf = 1.0 - fr;\n" \
	"    vec2 omf3 = omf * omf * omf;\n" \
	"    vec2 w0 = (1.0/6.0) * omf3;\n" \
	"    vec2 w1 = (1.0/6.0) * (3.0*fr3 - 6.0*fr2 + 4.0);\n" \
	"    vec2 w3 = (1.0/6.0) * fr3;\n" \
	"    vec2 g0 = w0 + w1;\n" \
	"    vec2 g1 = 1.0 - g0;\n" \
	"    vec2 h0 = ((w1 / g0) - 1.0) * invts;\n" \
	"    vec2 h1 = ((w3 / g1) + 1.0) * invts;\n" \
	"    vec2 base = ic * invts;\n" \
	"    vec4 c00 = texture(s, vec2(base.x + h0.x, base.y + h0.y));\n" \
	"    vec4 c10 = texture(s, vec2(base.x + h1.x, base.y + h0.y));\n" \
	"    vec4 c01 = texture(s, vec2(base.x + h0.x, base.y + h1.y));\n" \
	"    vec4 c11 = texture(s, vec2(base.x + h1.x, base.y + h1.y));\n" \
	"    return mix(mix(c00, c10, g1.x), mix(c01, c11, g1.x), g1.y);\n" \
	"}\n"

/* Procedural underwater caustics — cheap 2-sin product, ~104 world-unit
 * tiling (Hexen II uses 1 unit ≈ 1 inch).  Pow steepens the highlight so
 * the additive contribution looks like crisp light caustics rather than a
 * smooth modulation.  Time advances via u_caustics.y so the pattern flows.
 * uhexen2-6bfm. */
#define GLSL_CAUSTICS_FN \
	"float Caustics(vec2 p, float t) {\n" \
	"    vec2 q1 = p * 0.06 + vec2(t * 0.42, t * 0.31);\n" \
	"    vec2 q2 = p * 0.05 - vec2(t * 0.27, t * 0.49);\n" \
	"    float c1 = sin(q1.x + sin(q1.y + t));\n" \
	"    float c2 = sin(q2.y * 1.3 + sin(q2.x * 0.9 + t * 1.1));\n" \
	"    return pow(max(c1 * c2 * 0.5 + 0.5, 0.0), 4.0);\n" \
	"}\n"

static const char sworld_frag[] =
	GLSL_FRAG_HEADER
	GLSL_EARLY_Z
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"	/* diffuse */
	"uniform sampler2D u_texture1;\n"	/* lightmap atlas */
	"uniform sampler2D u_texture2;\n"	/* fullbright mask (uhexen2-sjvf) */
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	"out vec4 fragColor;\n"
	GLSL_BICUBIC_LM_FN
	GLSL_CAUSTICS_FN
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 lm = (u_lightmap_bicubic > 0.5)\n"
	"        ? BicubicLightmap(u_texture1, v_lmcoord)\n"
	/* r_fullbright / r_lightmap.  Applied to the samples rather than to the
	 * final colour so everything downstream -- overbright, the fullbright
	 * mask, caustics, fog, the alpha test -- keeps seeing a well-formed
	 * value.  rgb only on the diffuse: tex.a still gates the alpha test, so
	 * r_lightmap must not turn a fence into a solid sheet.  uhexen2-isq7. */
	"        : texture(u_texture1, v_lmcoord);\n"
	"    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);\n"
	"    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);\n"
	/* Sample the fullbright mask BEFORE the alpha-test discard.  texture()
	 * uses implicit dFdx/dFdy to pick the mip level; derivatives are
	 * undefined in a 2x2 quad where some lanes have already discarded, so
	 * sampling after discard produces dark outlines along fence/holey
	 * edges (Ironwail 017fdd2).  Add fullbright contribution: palette-
	 * index >= vid.fullbright pixels in the diffuse texture get rendered
	 * at full intensity regardless of lightmap.  For surfaces with no
	 * fullbright pixels the engine binds a 1x1 black sentinel at unit 2
	 * so the sample contributes 0.  uhexen2-9a1l. */
	"    vec3 fb = texture(u_texture2, v_texcoord).rgb * (1.0 - u_lightdebug.y);\n"
	"    vec4 color = tex * lm * v_color;\n"
	"    color.rgb *= u_overbright;\n"		/* Ironwail-style overbright (uhexen2-f29y) */
	"    if (color.a < u_alpha_threshold) discard;\n"
	"    color.rgb += fb;\n"
	/* Underwater caustics: gated by u_caustics.x (set to 0 by C when the
	 * view leaf is not CONTENTS_WATER or the cvar is off, otherwise to
	 * r_caustics_intensity).  Applied as a brightness multiplier so dark
	 * areas still receive a visible highlight band.  uhexen2-6bfm. */
	"    if (u_caustics.x > 0.0) {\n"
	"        float c = Caustics(v_worldxy, u_caustics.y);\n"
	"        color.rgb += color.rgb * c * u_caustics.x;\n"
	"    }\n"
	"    float fogfac = u_fog_density * v_fogdist;\n"
	"    float fog = exp(-fogfac * fogfac);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	/* For cutout alpha-test (threshold > 0.5 = fence/holey, A2C enabled):
	 * surviving fragments are by definition opaque, so force alpha=1 to
	 * stop A2C from dithering their coverage based on the noisy
	 * lightmap.a × tex.a × v_color.a multiply.
	 * For non-cutout draws (threshold ~ 0.01): preserve actual alpha so
	 * GL_BLEND with GL_SRC_ALPHA works for translucent surfaces.
	 * OIT path always preserves alpha for weighted blending. */
	/* uhexen2-khsa r13: u_force_opaque_alpha replaces the threshold test.
	 * See salias_frag for rationale. */
	"#ifdef OIT\n"
	"    fragColor = color;\n"
	"#else\n"
	"    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);\n"
	"#endif\n"
	"}\n";

/* sworld_frag_opaque: opaque-only variant with early_fragment_tests and no
 * discard.  Bound for batches that contain no fence/holey surfaces — i.e.
 * the world MDI pass, brush-ent MDI opaque pass, the DrawTextureChains fast
 * path, and the R_DrawBrushModel fast path.  Recovers the Hi-Z benefit that
 * was lost in uhexen2-238u without re-introducing the fence-occludes-tree
 * bug.  Keeps the full uniform layout of sworld_frag (same uniform names +
 * types) so call sites can switch between the two programs by glUseProgram
 * + re-upload, no shader-specific code paths required.  uhexen2-5c6r. */
static const char sworld_frag_opaque[] =
	GLSL_FRAG_HEADER
	GLSL_EARLY_Z_OPAQUE
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"	/* diffuse */
	"uniform sampler2D u_texture1;\n"	/* lightmap atlas */
	"uniform sampler2D u_texture2;\n"	/* fullbright mask */
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	"out vec4 fragColor;\n"
	GLSL_BICUBIC_LM_FN
	GLSL_CAUSTICS_FN
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 lm = (u_lightmap_bicubic > 0.5)\n"
	"        ? BicubicLightmap(u_texture1, v_lmcoord)\n"
	"        : texture(u_texture1, v_lmcoord);\n"
	"    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);\n"	/* uhexen2-isq7 */
	"    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);\n"
	"    vec4 color = tex * lm * v_color;\n"
	"    color.rgb *= u_overbright;\n"		/* Ironwail-style overbright (uhexen2-f29y) */
	"    vec3 fb = texture(u_texture2, v_texcoord).rgb * (1.0 - u_lightdebug.y);\n"
	"    color.rgb += fb;\n"
	"    if (u_caustics.x > 0.0) {\n"
	"        float c = Caustics(v_worldxy, u_caustics.y);\n"
	"        color.rgb += color.rgb * c * u_caustics.x;\n"
	"    }\n"
	"    float fogfac = u_fog_density * v_fogdist;\n"
	"    float fog = exp(-fogfac * fogfac);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = vec4(color.rgb, 1.0);\n"
	"}\n";

/* --- shader_alias: vertex-colored, textured, fog (models) ---
 * invariant gl_Position pins the position math across draws so the
 * additive fullbright re-draw lands on the exact depth the base pass
 * wrote. Without it the GLSL compiler may reorder the matrix multiply
 * and produce slightly different Z, so fullbright fragments fail the
 * GL_LEQUAL/GL_GEQUAL test polygon-by-polygon and the model develops
 * blocky chunks (uhexen2-iir3). Mesa sometimes ignores the qualifier;
 * the fullbright pass also enables polygon offset as a backstop. */
static const char salias_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	/* Model-only matrix (entity rotation + scale_origin + scale, no view).
	 * u_modelview is view*model, so it lands vertices in EYE space — sampling
	 * caustics there would make the pattern swim with the camera.  C uploads
	 * the same transform chain R_DrawAliasModel pushes onto the matrix stack,
	 * rebuilt on an identity base.  Defaults to identity so the sprite / warp
	 * / brush-poly batches that share this program (which submit world-space
	 * positions already) stay correct.  uhexen2-0gn3. */
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	"invariant gl_Position;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    v_worldxy = (u_alias_model * vec4(a_position, 1.0)).xy;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char salias_frag[] =
	GLSL_FRAG_HEADER
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"
	/* Soft particles (uhexen2-mf9u).  Explicitly highp: GLSL_FRAG_HEADER
	 * defaults the ES tier to mediump, whose 10-bit mantissa cannot tell
	 * two window-space depths apart, and the whole fade is a difference of
	 * two of them.  Desktop GLSL accepts the qualifier and ignores it. */
	"uniform highp sampler2D u_soft_depth;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	"out vec4 fragColor;\n"
	GLSL_CAUSTICS_FN
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 color = tex * v_color;\n"
	/* uhexen2-khsa r20: revert r15's threshold gate.  r15 only ran the
	 * discard for u_alpha_threshold > 0.5, exempting opaque batches.
	 * Confirmed via bisect (bobberb): some alias models route through
	 * the opaque path (threshold=0.01) yet have palette-255 cutout
	 * texels in their skin — trees, viewmodel hands, etc.  r14's
	 * unconditional test caught them; r15's gate let them render with
	 * the cutout texels' RGB (black/white/teal depending on upload
	 * path), which looked like the cutout area was filled in.  Restore
	 * the r14 behavior. */
	"    if (color.a < u_alpha_threshold) discard;\n"
	/* Underwater caustics, same formula and same pre-fog position in the
	 * chain as sworld_frag so a model and the floor it stands on receive
	 * the identical highlight band.  v_worldxy is world space on every
	 * vertex shader that links against this fragment shader.  Gated by
	 * u_alias_caustics.x, which C leaves at 0 for every non-alias batch
	 * that shares this program.  uhexen2-0gn3. */
	"    if (u_alias_caustics.x > 0.0) {\n"
	"        float c = Caustics(v_worldxy, u_alias_caustics.y);\n"
	"        color.rgb += color.rgb * c * u_alias_caustics.x;\n"
	"    }\n"
	"    float fogfac = u_fog_density * v_fogdist;\n"
	"    float fog = exp(-fogfac * fogfac);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	/* Soft particles (uhexen2-mf9u).  A view-parallel billboard whose origin
	 * sits on an impact surface has up to half its quad behind that surface
	 * at any oblique view angle; depth-testing it per fragment slices the
	 * sprite along a hard line.  Fade alpha with the distance to whatever
	 * opaque geometry is behind instead, so it dissolves into the wall.
	 *
	 * u_soft_params.yz linearize a window-space depth to a positive
	 * view-space distance as z = b / (a - d).  C derives a and b from the
	 * live projection matrix with glDepthRange folded in, so the expression
	 * is identical for the reversed-Z and forward-Z conventions and for the
	 * mirror pass's split depth range.
	 *
	 * Deliberately after the alpha test: the cutout decision belongs to the
	 * sprite's own texel alpha, and a texel faded to near-zero by proximity
	 * must still blend rather than discard.  Gated by .x, which every
	 * non-sprite user of this program leaves at 0. */
	"    if (u_soft_params.x > 0.0) {\n"
	"        highp float dscene = texelFetch(u_soft_depth, ivec2(gl_FragCoord.xy), 0).r;\n"
	"        highp float zscene = u_soft_params.z / (u_soft_params.y - dscene);\n"
	"        highp float zfrag  = u_soft_params.z / (u_soft_params.y - gl_FragCoord.z);\n"
	"        color.a *= clamp((zscene - zfrag) * u_soft_params.x, 0.0, 1.0);\n"
	"    }\n"
	/* uhexen2-khsa r13: u_force_opaque_alpha replaces the old threshold-
	 * based test.  Threshold > 0.5 was correct for fence cutouts but
	 * caught opaque-as-translucent ents (CASTLE_TR.MDL etc.) that need
	 * fragColor.a=1.0 to avoid bleeding a garbage color.a into FB.a
	 * where any downstream pass (OIT compositor, post-process) could
	 * read it as a transparency key.  C sets force_opaque_alpha=1 in
	 * confirmed-opaque alias paths and =0 for ENTALPHA / DRF_TRANSLUCENT
	 * which legitimately need blend-stage src.a from the shader. */
	"#ifdef OIT\n"
	"    fragColor = color;\n"
	"#else\n"
	"    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);\n"
	"#endif\n"
	"}\n";

/* --- shader_skeletal: skeletal animation with bone-weighted deformation ---
 * Desktop only: the bone matrices arrive in a std430 shader storage block,
 * which is GLSL ES 3.10 and absent from WebGL2 entirely.  See GL_Shaders_Init
 * and uhexen2-dfay. */
#ifndef USE_GLES
static const char sskeletal_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	/* Explicit locations, because the IQM VAO (GL_CreateAliasGPUMesh in
	 * gl_mesh.c) hardcodes this layout and it does not match the generic
	 * a_position/a_texcoord/a_lmcoord/a_color bindings GL_LoadProgram
	 * applies to every other program.  A layout qualifier overrides
	 * glBindAttribLocation, so this stays local to the skeletal program
	 * and leaves the shared contract alone.  uhexen2-bynk. */
	"layout(location=0) in vec3 a_position;\n"
	"layout(location=1) in vec4 a_normal;\n"
	"layout(location=2) in vec2 a_texcoord;\n"
	"layout(location=3) in vec4 a_weights;\n"
	"layout(location=4) in uvec4 a_indices;\n"
	"\n"
	"layout(std430, binding=4) restrict readonly buffer BoneBuffer {\n"
	"    mat3x4 bones[];\n"
	"};\n"
	"\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	"invariant gl_Position;\n"
	"\n"
	"void main() {\n"
	"    mat3x4 blend = bones[u_pose_base + int(a_indices.x)] * a_weights.x;\n"
	"    if (a_weights.y > 0.01) blend += bones[u_pose_base + int(a_indices.y)] * a_weights.y;\n"
	"    if (a_weights.z > 0.01) blend += bones[u_pose_base + int(a_indices.z)] * a_weights.z;\n"
	"    if (a_weights.w > 0.01) blend += bones[u_pose_base + int(a_indices.w)] * a_weights.w;\n"
	"\n"
	"    mat4x3 mat_3x4 = transpose(blend);\n"
	"    vec3 skinned_pos = mat_3x4 * vec4(a_position, 1.0);\n"
	"    vec3 skinned_normal = normalize(mat_3x4 * vec4(a_normal.xyz, 0.0));\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = vec4(skinned_normal * 0.5 + 0.5, 1.0);\n"
	"    v_worldxy = (u_alias_model * vec4(skinned_pos, 1.0)).xy;\n"
	"    vec4 eyepos = u_modelview * vec4(skinned_pos, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	"    gl_Position = u_mvp * vec4(skinned_pos, 1.0);\n"
	"}\n";
#endif /* !USE_GLES */

/* --- shader_particle: textured triangles with per-vertex color --- */
static const char spart_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
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
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 tex = texture(u_texture0, v_texcoord);\n"
	"    vec4 color = tex * v_color;\n"
	"    if (color.a < 0.01) discard;\n"
	"    float fogfac = u_fog_density * v_fogdist;\n"
	"    float fog = exp(-fogfac * fogfac);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	"    fragColor = color;\n"
	"}\n";

#ifndef USE_GLES  /* SSBO shaders require GL 4.3 — not available in WebGL2 */
/* --- shader_particle_gpu: SSBO-driven billboard particles ---
 * Vertex shader reads particle state from an SSBO (binding=0).
 * Each particle uses 3 vertices (gl_VertexID/3 = particle index).
 * Dead particles (die < u_ctime) become zero-area triangles. */
static const char spart_gpu_vert[] =
	"#version 430 core\n"
	GLSL_UNIFORM_BLOCKS
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
#endif /* !USE_GLES */

/* --- shader_alias_instanced: GL 4.3 SSBO-based instanced alias models ---
 * Instance data in SSBO binding 0 (streamed via gl_buffer.c each frame).
 * Pose data in SSBO binding 1 (per-batch, from model's ssbo_pose).
 * Shadedots in SSBO binding 2 (static lighting cosine table).
 * View-projection passed as a uniform — uhexen2-8pc2 moved it out of the
 * SSBO so the streaming ring uploads only the instance array.
 * Scale/origin baked into the world matrix CPU-side.
 * 80-byte instance struct matching Ironwail's compact layout.
 */
#ifndef USE_GLES
static const char salias_inst_vert[] =
	"#version 430 core\n"
	GLSL_UNIFORM_BLOCKS
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
	"layout(std430, binding=3) restrict readonly buffer MD3PoseBuffer {\n"
	"    uvec2 md3_pose_data[];\n"  /* short xyz[3] + ubyte normal[2] packed as two uint32s */
	"};\n"
	"\n"
	"in vec2 a_texcoord;\n"
	"\n"
	"\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	"\n"
	"invariant gl_Position;\n"
	"\n"
	"vec3 MD3_DecodePosition(uvec2 vdata) {\n"
	"    int x = int((vdata.x) & 0xFFFFu);\n"
	"    int y = int((vdata.x >> 16) & 0xFFFFu);\n"
	"    int z = int((vdata.y) & 0xFFFFu);\n"
	"    if (x > 32767) x -= 65536;\n"
	"    if (y > 32767) y -= 65536;\n"
	"    if (z > 32767) z -= 65536;\n"
	"    return vec3(x, y, z) / 64.0;\n"
	"}\n"
	"\n"
	"vec3 MD3_DecodeNormal(uvec2 vdata) {\n"
	"    uint latbyte = (vdata.y >> 16) & 0xFFu;\n"
	"    uint lngbyte = (vdata.y >> 24) & 0xFFu;\n"
	"    float lat = float(latbyte) * (3.14159265 / 128.0);\n"
	"    float lng = float(lngbyte) * (3.14159265 / 128.0);\n"
	"    float sinlng = sin(lng);\n"
	"    return vec3(sinlng * cos(lat), sinlng * sin(lat), cos(lng));\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    InstanceData inst = instances[u_inst_base + gl_InstanceID];\n"
	"    vec3 local_pos, normal;\n"
	"    uint ni;\n"
	"\n"
	"    if (u_poseverttype == 1) {\n"  /* PV_MD3 */
	"        uvec2 p0 = md3_pose_data[inst.Pose0 + gl_VertexID];\n"
	"        uvec2 p1 = md3_pose_data[inst.Pose1 + gl_VertexID];\n"
	"        vec3 v0 = MD3_DecodePosition(p0);\n"
	"        vec3 v1 = MD3_DecodePosition(p1);\n"
	"        vec3 n0 = MD3_DecodeNormal(p0);\n"
	"        vec3 n1 = MD3_DecodeNormal(p1);\n"
	"        local_pos = mix(v1, v0, inst.Blend);\n"
	"        normal = normalize(mix(n1, n0, inst.Blend));\n"
	"        ni = uint(0);\n"  /* MD3 uses vector normal, not dot-product table */
	"    } else {\n"  /* PV_QUAKE1 (default) */
	"        uint p0 = pose_data[inst.Pose0 + gl_VertexID];\n"
	"        uint p1 = pose_data[inst.Pose1 + gl_VertexID];\n"
	"        vec3 v0 = vec3(float(p0 & 0xFFu), float((p0>>8)&0xFFu), float((p0>>16)&0xFFu));\n"
	"        vec3 v1 = vec3(float(p1 & 0xFFu), float((p1>>8)&0xFFu), float((p1>>16)&0xFFu));\n"
	"        ni = (p0 >> 24) & 0xFFu;\n"
	"        local_pos = mix(v1, v0, inst.Blend);\n"
	"    }\n"
	"\n"
	"    mat4x3 world = transpose(mat3x4(\n"
	"        inst.WorldMatrix0, inst.WorldMatrix1, inst.WorldMatrix2));\n"
	"    vec3 world_pos = world * vec4(local_pos, 1.0);\n"
	"\n"
	"    float sdot;\n"
	"    if (u_poseverttype == 1) {\n"  /* MD3: use decoded normal */
	"        vec3 world_normal = normalize((mat3(inst.WorldMatrix0.xyz, inst.WorldMatrix1.xyz, inst.WorldMatrix2.xyz)) * normal);\n"
	"        sdot = max(world_normal.z, 0.0);\n"
	"    } else {\n"  /* QUAKE1: use shadedots table */
	"        int sdot_idx = inst.ShadedotRow * 256 + int(ni);\n"
	"        sdot = shadedots[sdot_idx];\n"
	"        sdot = max(sdot, 0.0);\n"
	"    }\n"
	"    v_color = vec4(inst.LightAlpha.rgb * sdot, inst.LightAlpha.a);\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	/* The instance world matrix is model-only, so world_pos is already the
	 * world-space vertex — no u_alias_model needed on this path and no
	 * change to the 80-byte InstanceData layout.  uhexen2-0gn3. */
	"    v_worldxy = world_pos.xy;\n"
	"    v_fogdist = distance(world_pos, u_eyepos);\n"
	"    gl_Position = u_viewproj * vec4(world_pos, 1.0);\n"
	"}\n";
#endif /* !USE_GLES */

/* --- shader_sky: two-layer scrolling sky (solid + alpha) --- */
static const char ssky_vert[] =
	GLSL_VERT_HEADER
	GLSL_UNIFORM_BLOCKS
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec2 a_lmcoord;\n"
	"in vec4 a_color;\n"
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
	GLSL_UNIFORM_BLOCKS
	"uniform sampler2D u_texture0;\n"
	"uniform sampler2D u_texture1;\n"
	"in vec3 v_dir;\n"
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    if (u_alpha_threshold > 0.5) {\n"
	"        vec4 solid = texture(u_texture0, v_texcoord + u_wind);\n"
	"        fragColor = solid * v_color;\n"
	"    } else {\n"
	"        vec2 uv = normalize(v_dir).xy * (189.0 / 64.0);\n"
	"        vec4 solid = texture(u_texture0, uv + u_time / 16.0 + u_wind);\n"
	"        vec4 layer = texture(u_texture1, uv + u_time / 8.0 + u_wind);\n"
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

	/* Bind texture unit defaults.  These are the only loose uniforms left:
	 * samplers cannot live in a uniform block, and they are link-time
	 * constants rather than per-draw state.  Everything the old code seeded
	 * here -- the zero alpha threshold, zero fog density, and the identity
	 * model matrix that keeps v_worldxy meaningful (uhexen2-0gn3) -- is now
	 * seeded once for every program in R_Uniforms_Init. */
	R_UseProgram (p->program);
	if (p->u_texture0 >= 0) glUniform1i_fp(p->u_texture0, 0);
	if (p->u_texture1 >= 0) glUniform1i_fp(p->u_texture1, 1);
	if (p->u_texture2 >= 0) glUniform1i_fp(p->u_texture2, 2);
	/* uhexen2-mf9u.  Unit 1 is free on every program that declares this —
	 * gl_shader_alias has no u_texture1 — so the depth snapshot can sit
	 * there permanently and only the sprite path ever binds to it. */
	if (p->u_soft_depth >= 0) glUniform1i_fp(p->u_soft_depth, 1);
	R_UseProgram (0);

	Con_SafePrintf("  shader '%s' loaded (program %u)\n", name, p->program);
	return true;
}

#ifndef USE_GLES
static qboolean GL_InitParticleGPUProgram (gl_particle_gpu_prog_t *p)
{
	p->base.program = GL_LoadProgram(spart_gpu_vert, spart_frag);
	if (!p->base.program)
	{
		Con_Printf("Failed to load shader: particle_gpu\n");
		return false;
	}
	GL_InitProgramUniforms(&p->base);

	/* Bind texture unit defaults */
	R_UseProgram (p->base.program);
	if (p->base.u_texture0 >= 0) glUniform1i_fp(p->base.u_texture0, 0);
	R_UseProgram (0);

	Con_SafePrintf("  shader 'particle_gpu' loaded (program %u)\n", p->base.program);
	return true;
}
#endif /* !USE_GLES */


gl_alias_inst_prog_t gl_shader_alias_inst;

/* Shadedots table — defined in gl_rmain.c */
extern float r_avertexnormal_dots[16][256];

#ifndef USE_GLES
static qboolean GL_InitAliasInstProgram (gl_alias_inst_prog_t *p)
{
	GLuint vs, fs, prog;

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
	R_BindProgramBlocks (prog);

	/* Bind skin texture to unit 0 */
	R_UseProgram (prog);
	{
		GLint u_tex = glGetUniformLocation_fp(prog, "u_texture0");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 0);
	}
	R_UseProgram (0);

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
#endif /* !USE_GLES */

void GL_AliasInst_Init (void)
{
#ifndef USE_GLES
	if (!GL_InitAliasInstProgram(&gl_shader_alias_inst))
		Sys_Printf("WARNING: instanced alias shader failed to init\n");
#endif
}

void GL_AliasInst_Shutdown (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
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
	GL_InitProgram(&gl_shader_world_opaque, "world_opaque", sworld_vert, sworld_frag_opaque);
	GL_InitProgram(&gl_shader_alias,    "alias",    salias_vert, salias_frag);
#ifndef USE_GLES
	/* Skipped on the ES tier: sskeletal_vert reads its bone matrices from a
	 * `layout(std430) buffer`, and shader storage blocks are GLSL ES 3.10,
	 * not 3.00 — WebGL2 has none at all.  Compiling it there only produced a
	 * guaranteed failure on every startup, and left the driver linking a
	 * program built from a shader it had already rejected.  Nothing draws
	 * with it yet (the PV_IQM dispatch is uhexen2-7ok0.3); whoever wires
	 * that up owns giving the ES tier a UBO or texture-buffer skinning
	 * path, or leaving skeletal models unsupported there.  uhexen2-dfay. */
	GL_InitProgram(&gl_shader_skeletal, "skeletal", sskeletal_vert, salias_frag);
#endif
	GL_InitProgram(&gl_shader_particle, "particle", spart_vert,  spart_frag);
	GL_InitProgram(&gl_shader_sky,      "sky",      ssky_vert,   ssky_frag);

	/* Create the 1x1 black sentinel texture used as u_texture2 in
	 * gl_shader_world for surfaces with no fullbright pixels.  Sampled
	 * unconditionally; black contributes 0 to the additive sum.
	 * uhexen2-sjvf. */
	{
		static const unsigned char black_pixel[4] = {0, 0, 0, 255};
		glGenTextures_fp(1, &gl_null_fb_texture);
		R_BindTextureSlot (0, gl_null_fb_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, black_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	/* Companion to the above: 1x1 opaque white, so an immediate-mode batch
	 * can draw flat vertex colour through a shader that always samples a
	 * texture.  R_DrawParticles' square mode is the first caller.
	 * uhexen2-2rxl. */
	{
		static const unsigned char white_pixel[4] = {255, 255, 255, 255};
		glGenTextures_fp(1, &gl_solid_white_texture);
		R_BindTextureSlot (0, gl_solid_white_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	/* OIT variants for translucent rendering.
	 *
	 * Not on the ES tier, where they could never be selected: every call
	 * site picks between the OIT and plain program with
	 * `OIT_InPass() ? &gl_shader_X_oit : &gl_shader_X`, and oit_in_pass is
	 * only ever set inside OIT_BeginTranslucency, which returns early
	 * unless HW_OIT_HAS_BLEND_FUNCI — a literal 0 under USE_GLES, because
	 * glBlendFunci is GL 4.0 and WebGL2 exposes no per-draw-buffer blend
	 * equations at all.  So this was six shader compiles and three links
	 * per startup for programs nothing can reach, on the tier least able to
	 * spare either.  GL_PostProcess_Init already guards the resolve half of
	 * OIT on the same condition; this is the half that missed it.
	 *
	 * Note for anyone arriving from uhexen2-g63k: that fixed a genuine
	 * compile failure in these three on ES, but it did not make OIT work
	 * there and could not have.  uhexen2-ha7n. */
#ifndef USE_GLES
	GL_InitOITProgram(&gl_shader_world_oit,    "world",    sworld_vert, sworld_frag);
	GL_InitOITProgram(&gl_shader_alias_oit,    "alias",    salias_vert, salias_frag);
	GL_InitOITProgram(&gl_shader_particle_oit, "particle", spart_vert,  spart_frag);
#endif

#ifndef USE_GLES
	GL_InitParticleGPUProgram(&gl_shader_particle_gpu);
#endif
	GL_AliasInst_Init();
}

void GL_Shaders_Shutdown (void)
{
	glprogram_t *progs[] = {
		&gl_shader_2d, &gl_shader_flat, &gl_shader_world,
		&gl_shader_world_opaque,
		&gl_shader_alias, &gl_shader_skeletal, &gl_shader_particle, &gl_shader_sky,
		&gl_shader_particle_gpu.base,
		&gl_shader_world_oit, &gl_shader_alias_oit, &gl_shader_particle_oit
	};
	int i;

	R_PipelineForgetProgram ();	/* GL reuses program names */
	for (i = 0; i < (int)(sizeof(progs)/sizeof(progs[0])); i++)
	{
		if (progs[i]->program)
		{
			glDeleteProgram_fp(progs[i]->program);
			progs[i]->program = 0;
		}
	}
	GL_AliasInst_Shutdown();
}
