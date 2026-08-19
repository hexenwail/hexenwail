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

/* Forward declarations */
static void GL_InitProgramUniforms (glprogram_t *p);

/* ------------------------------------------------------------------ */
/* Shader compilation helpers                                          */
/* ------------------------------------------------------------------ */

GLuint GL_CompileShaderParts (GLenum type, int count, const char **parts)
{
	GLuint shader;
	GLint status;
	char log[2048];
	const char *type_name = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
	const char *source = parts[count - 1];

	shader = glCreateShader_fp(type);

	glShaderSource_fp(shader, count, parts, NULL);

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

GLuint GL_CompileShader (GLenum type, const char *source)
{
	return GL_CompileShaderParts (type, 1, &source);
}

/* Compile one stage of a draw shader: the dialect header, an optional
 * per-program prefix, then the body from shaders_gen.h.  Passed to GL as three
 * separate chunks rather than concatenated, so the driver's error line numbers
 * are offset by the header alone rather than by everything above the body. */
static GLuint GL_CompileStage (GLenum type, const char *prefix, const char *body)
{
	const char *parts[3];
	int n = 0;

	parts[n++] = (type == GL_VERTEX_SHADER) ? GLSL_VERT_HEADER : GLSL_FRAG_HEADER;
	if (prefix && *prefix)
		parts[n++] = prefix;
	parts[n++] = body;
	return GL_CompileShaderParts (type, n, parts);
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
static GLuint GL_CompileOITFragShader (const char *frag_body)
{
	/* Uses the main->main_body rename trick from Ironwail.
	 *
	 * fragColor is initialized rather than merely declared.  main() below
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

	/* The body declares its own fragColor as a fragment output; the preamble
	 * replaces it with a plain global plus the two MRT outputs, so that one
	 * line has to come out.  This used to have to find the #version line and
	 * then step past any precision declarations to know where it was safe to
	 * splice (uhexen2-g63k) -- with the header supplied as a separate
	 * glShaderSource chunk there is nothing to step past. */
	static const char decl[] = "out vec4 fragColor;\n";
	const char *skip = strstr(frag_body, decl);
	size_t before_len;
	size_t total;
	char *buf;
	GLuint shader;

	if (!skip)
		return 0;
	before_len = skip - frag_body;
	skip += strlen(decl);

	total = strlen(oit_preamble) + before_len + strlen(skip) + 1;
	buf = (char *) malloc(total);
	if (!buf)
		Sys_Error("GL_CompileOITFragShader: out of memory");
	memcpy(buf, oit_preamble, strlen(oit_preamble));
	memcpy(buf + strlen(oit_preamble), frag_body, before_len);
	strcpy(buf + strlen(oit_preamble) + before_len, skip);

	shader = GL_CompileStage(GL_FRAGMENT_SHADER, NULL, buf);
	free(buf);
	return shader;
}

static GLuint GL_LoadOITProgram (const char *vert_src, const char *frag_src)
{
	GLuint vs, fs, prog;

	vs = GL_CompileStage(GL_VERTEX_SHADER, NULL, vert_src);
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
	return GL_LoadProgramEx (vert_src, frag_src, NULL);
}

/* frag_prefix is spliced between the dialect header and the body.  Only
 * gl_shader_world_opaque uses it, for early_fragment_tests. */
GLuint GL_LoadProgramEx (const char *vert_src, const char *frag_src,
			 const char *frag_prefix)
{
	GLuint vs, fs, prog;

	vs = GL_CompileStage(GL_VERTEX_SHADER, NULL, vert_src);
	if (!vs) return 0;
	fs = GL_CompileStage(GL_FRAGMENT_SHADER, frag_prefix, frag_src);
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


/* The shader bodies themselves live in engine/shaders/*.glsl and arrive here
 * as string literals generated by engine/cmake/EmbedShaders.cmake.  The dialect
 * header above stays in C because it is the one part that genuinely differs
 * between the two tiers; it is prepended at compile time as a separate
 * glShaderSource chunk rather than concatenated into the body, so a compile log
 * line number still points at a real line of a real file.  uhexen2-p4ln.4. */
#include "shaders_gen.h"

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                     */
/* ------------------------------------------------------------------ */

static qboolean GL_InitProgramEx (glprogram_t *p, const char *name,
				  const char *vert_src, const char *frag_src,
				  const char *frag_prefix)
{
	p->program = GL_LoadProgramEx(vert_src, frag_src, frag_prefix);
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

static qboolean GL_InitProgram (glprogram_t *p, const char *name,
				const char *vert_src, const char *frag_src)
{
	return GL_InitProgramEx (p, name, vert_src, frag_src, NULL);
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

	vs = GL_CompileStage(GL_VERTEX_SHADER, NULL, salias_inst_vert);
	fs = GL_CompileStage(GL_FRAGMENT_SHADER, NULL, salias_frag);
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
	GL_InitProgramEx(&gl_shader_world_opaque, "world_opaque", sworld_vert,
			 sworld_frag_opaque, GLSL_EARLY_Z_OPAQUE);
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
