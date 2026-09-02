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
#include "gl_pipeline.h"
#include "gl_lightcluster.h"	/* froxel grid dimensions and binding points (uhexen2-26bm) */

/* Paste an integer macro into a GLSL source string.  The froxel grid's
 * dimensions and binding points have to agree between the compute pass that
 * fills the grid and the fragment shaders that read it, and the only way to
 * guarantee that is for both to say LIGHT_TILES_X rather than 32.  The compute
 * side gets them as #defines in a generated header (gl_lightcluster.c); the
 * world shaders are fixed strings, so they get them spliced in here. */
#define GLSL_STR_(x)	#x
#define GLSL_STR(x)	GLSL_STR_(x)

extern float	r_fog_density;
extern float	r_fog_color[3];

glprogram_t	gl_shader_world;
glprogram_t	gl_shader_world_opaque;	/* uhexen2-5c6r: early_fragment_tests, no discard */
glprogram_t	gl_shader_alias;
/* Affine-mapping twins, compiled from the same sources with NOPERSP defined.
 * Selected per draw by R_SoftEmuMdlWarp; unused and inert at r_softemu 0,
 * which is the default.  uhexen2-ktjv. */
glprogram_t	gl_shader_alias_np;
glprogram_t	gl_shader_skeletal_np;
glprogram_t	gl_shader_skeletal;
glprogram_t	gl_shader_skeletal_oit;
glprogram_t	gl_shader_2d;
glprogram_t	gl_shader_particle;
glprogram_t	gl_shader_flat;
glprogram_t	gl_shader_sky_layers;	/* two scrolling cloud layers (uhexen2-a5nn.4) */
glprogram_t	gl_shader_sky_boxside;	/* one sky texture, UVs from the vertex */
glprogram_t	gl_shader_sky_cubemap;	/* skybox as a cubemap, sampled by direction (uhexen2-ctk9) */

/* null fullbright texture: 1x1 black RGBA bound at unit 2 for world
 * surfaces whose diffuse texture has no fullbright pixels.  Lets
 * sworld_frag unconditionally sample u_texture2 and add the result
 * (zero contribution) instead of branching per-fragment.  uhexen2-sjvf. */
GLuint		gl_null_fb_texture;
GLuint		gl_flat_normal_texture;	/* uhexen2-mfql */
GLuint		gl_null_gloss_texture;	/* uhexen2-mfql */

/* Solid white 1x1 RGBA.  The shaders that back the immediate-mode batches
 * all sample a texture unconditionally and multiply it into the vertex
 * colour, so a caller that wants a flat untextured draw has to hand them
 * something that multiplies by one.  Binding this is that "no texture". */
GLuint		gl_solid_white_texture;

/* OIT variants of translucent shaders */
glprogram_t	gl_shader_world_oit;
glprogram_t	gl_shader_alias_oit;
glprogram_t	gl_shader_alias_np_oit;
glprogram_t	gl_shader_skeletal_np_oit;
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

GLuint GL_LoadComputeProgram (const char *header, const char *body, const char *name)
{
	const char	*sources[2];
	GLuint		shader, prog;
	GLint		status;
	char		log[2048];

	sources[0] = header;
	sources[1] = body;

	shader = glCreateShader_fp(GL_COMPUTE_SHADER);
	glShaderSource_fp(shader, 2, sources, NULL);
	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("[SHADER] COMPUTE COMPILE ERROR (%s):\n%s\n", name, log);
		glDeleteShader_fp(shader);
		return 0;
	}

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, shader);
	glLinkProgram_fp(prog);
	glDeleteShader_fp(shader);	/* flagged for deletion; the program holds it */

	glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
	if (!status)
	{
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_Printf("[SHADER] COMPUTE LINK ERROR (%s):\n%s\n", name, log);
		glDeleteProgram_fp(prog);
		return 0;
	}

	if (developer.integer)
		Con_SafePrintf("[SHADER] compute program '%s' linked OK (id=%u)\n", name, prog);
	return prog;
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
/*
===============
GL_SpliceDefines

Return a malloc'd copy of `src` with `defines` inserted where a #define is
legal: after the #version line, and after any default-precision declarations
that follow it.  GLSL ES requires `precision <qual> <type>;` before the first
use of that type, so splicing directly under #version is not safe in general --
the OIT splice below learned that the hard way (uhexen2-g63k).

Caller frees.  NULL on a source with no newline at all, which cannot be a
valid shader.  uhexen2-ktjv.
===============
*/
static char *GL_SpliceDefines (const char *src, const char *defines)
{
	const char	*rest = strchr (src, '\n');
	size_t		head_len, total;
	char		*buf;

	if (!rest)
		return NULL;
	rest++;

	while (!strncmp (rest, "precision ", 10))
	{
		const char *eol = strchr (rest, '\n');
		if (!eol)
			return NULL;
		rest = eol + 1;
	}

	head_len = (size_t)(rest - src);
	total = head_len + strlen (defines) + strlen (rest) + 1;
	buf = (char *) malloc (total);
	if (!buf)
		Sys_Error ("%s: out of memory", __thisfunc__);

	memcpy (buf, src, head_len);
	memcpy (buf + head_len, defines, strlen (defines));
	strcpy (buf + head_len + strlen (defines), rest);
	return buf;
}

/*
===============
GL_CompileShaderDefines

GL_CompileShader with a preamble spliced in.  One source, several variants --
which is how a `noperspective` qualifier gets to be optional, since an
interpolation qualifier cannot be switched by a uniform the way the softemu
stages in the world shader can.  uhexen2-ktjv.
===============
*/
static GLuint GL_CompileShaderDefines (GLenum type, const char *src, const char *defines)
{
	char	*buf = GL_SpliceDefines (src, defines);
	GLuint	shader;

	if (!buf)
		return 0;
	shader = GL_CompileShader (type, buf);
	free (buf);
	return shader;
}

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
		if (p->u_texture3 >= 0) glUniform1i_fp(p->u_texture3, 3);	/* uhexen2-mfql */
		if (p->u_texture4 >= 0) glUniform1i_fp(p->u_texture4, 4);
		if (p->u_lightgrid >= 0) glUniform1i_fp(p->u_lightgrid, LIGHT_GRID_TMU);	/* uhexen2-26bm */
		if (p->u_soft_depth >= 0) glUniform1i_fp(p->u_soft_depth, 1);	/* uhexen2-mf9u */
		if (p->u_alias_model >= 0)	/* uhexen2-0gn3, see GL_InitProgram */
		{
			float ident[16];
			Mat4_Identity(ident);
			glUniformMatrix4fv_fp(p->u_alias_model, 1, GL_FALSE, ident);
		}
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
	p->u_mvp             = glGetUniformLocation_fp(p->program, "u_mvp");
	p->u_texture0        = glGetUniformLocation_fp(p->program, "u_texture0");
	p->u_texture1        = glGetUniformLocation_fp(p->program, "u_texture1");
	p->u_texture2        = glGetUniformLocation_fp(p->program, "u_texture2");
	p->u_texture3        = glGetUniformLocation_fp(p->program, "u_texture3");
	p->u_texture4        = glGetUniformLocation_fp(p->program, "u_texture4");
	p->u_material        = glGetUniformLocation_fp(p->program, "u_material");
	p->u_color           = glGetUniformLocation_fp(p->program, "u_color");
	p->u_fog_density     = glGetUniformLocation_fp(p->program, "u_fog_density");
	p->u_fog_color       = glGetUniformLocation_fp(p->program, "u_fog_color");
	p->u_alpha_threshold = glGetUniformLocation_fp(p->program, "u_alpha_threshold");
	p->u_modelview       = glGetUniformLocation_fp(p->program, "u_modelview");
	p->u_time            = glGetUniformLocation_fp(p->program, "u_time");
	p->u_skyfog          = glGetUniformLocation_fp(p->program, "u_skyfog");
	p->u_eyepos          = glGetUniformLocation_fp(p->program, "u_eyepos");
	p->u_wind            = glGetUniformLocation_fp(p->program, "u_wind");
	p->u_wind3           = glGetUniformLocation_fp(p->program, "u_wind3");
	p->u_skyrot          = glGetUniformLocation_fp(p->program, "u_skyrot");
	p->u_caustics        = glGetUniformLocation_fp(p->program, "u_caustics");
	p->u_overbright      = glGetUniformLocation_fp(p->program, "u_overbright");
	p->u_lightmap_bicubic = glGetUniformLocation_fp(p->program, "u_lightmap_bicubic");
	p->u_lightdebug      = glGetUniformLocation_fp(p->program, "u_lightdebug");
	p->u_softemu         = glGetUniformLocation_fp(p->program, "u_softemu");
	/* Clustered dynamic lighting (uhexen2-26bm); -1 on every non-world
	 * program and on the whole ES tier. */
	p->u_dlight_scale    = glGetUniformLocation_fp(p->program, "u_dlight_scale");
	p->u_lightview       = glGetUniformLocation_fp(p->program, "u_lightview");
	p->u_lightgrid_xy    = glGetUniformLocation_fp(p->program, "u_lightgrid_xy");
	p->u_lightgrid_z     = glGetUniformLocation_fp(p->program, "u_lightgrid_z");
	p->u_lightgrid       = glGetUniformLocation_fp(p->program, "u_lightgrid");
	p->u_alias_dlight    = glGetUniformLocation_fp(p->program, "u_alias_dlight");	/* uhexen2-waum */
	p->u_force_opaque_alpha = glGetUniformLocation_fp(p->program, "u_force_opaque_alpha");
	p->u_alias_caustics   = glGetUniformLocation_fp(p->program, "u_alias_caustics");
	p->u_turb             = glGetUniformLocation_fp(p->program, "u_turb");
	p->u_alias_model      = glGetUniformLocation_fp(p->program, "u_alias_model");
	p->u_soft_depth       = glGetUniformLocation_fp(p->program, "u_soft_depth");
	p->u_soft_params      = glGetUniformLocation_fp(p->program, "u_soft_params");
	/* Skeletal-only (uhexen2-7ok0.3); -1 on every other program. */
	p->u_pose_base        = glGetUniformLocation_fp(p->program, "u_pose_base");
	p->u_pose_base2       = glGetUniformLocation_fp(p->program, "u_pose_base2");
	p->u_skel_blend       = glGetUniformLocation_fp(p->program, "u_skel_blend");
	p->u_shadevector      = glGetUniformLocation_fp(p->program, "u_shadevector");
	p->u_lightcolor       = glGetUniformLocation_fp(p->program, "u_lightcolor");
	p->u_fullbright       = glGetUniformLocation_fp(p->program, "u_fullbright");
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
/* bitfieldReverse is GLSL 4.00 / ES 3.10; the ES tier is 3.00.  Same fallback
 * gl_postprocess.c carries for its own copy of the Bayer matrix. */
#define GLSL_BITFIELD_REVERSE \
	"uint bitfieldReverse(uint v) {\n" \
	"    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);\n" \
	"    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);\n" \
	"    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);\n" \
	"    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);\n" \
	"    return (v << 16) | (v >> 16);\n" \
	"}\n"
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
#define GLSL_BITFIELD_REVERSE	""
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
	"out highp vec2 v_texcoord;\n"	/* highp: the material tangent frame differentiates this; see v_eyepos (uhexen2-mfql) */
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
	/* Eye-space position, for the material-map tangent frame (uhexen2-mfql).
	 * EYE space and not world: the frame, the view vector and the geometric
	 * normal all have to live in one space, and eye space is the only one
	 * this shader can produce for BOTH world surfaces and brush ents.  For a
	 * brush ent the entity transform is inside u_modelview and never reaches
	 * a_position -- which is exactly why v_worldxy above is documented as
	 * wrong for brush ents -- so anything derived from a_position directly
	 * would put a rotating platform's relief in its own local frame.  In eye
	 * space the camera sits at the origin, so the view vector is just
	 * -normalize(v_eyepos), and no eye-position uniform is needed. */
	/* highp explicitly.  The ES tier's fragment stage runs at mediump by
	 * default (GLSL_FRAG_HEADER), and mediump carries about 10 bits of
	 * mantissa -- fine for a colour, useless for a position that can be
	 * thousands of world units from the camera, whose screen-space
	 * derivative is then the difference of two badly-rounded large numbers.
	 * That is precisely the quantity the tangent frame is built from, so
	 * without this the WebGL2 build gets a frame made of noise.  Desktop
	 * GLSL accepts and ignores the qualifier.  uhexen2-mfql. */
	"out highp vec3 v_eyepos;\n"
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
	"    v_eyepos = eyepos.xyz;\n"
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


/* Analytic band-limited liquid UV warp (uhexen2-9o7u), shared by the alias
 * and world fragment shaders so an unlit liquid and a lit one ripple
 * identically -- lit water renders through the WORLD program (uhexen2-a5nn.2),
 * and two hand-copies of this would be free to drift apart.  Callers gate the
 * call on turb.x > 0.0, which keeps the fwidth() below inside uniform control
 * flow and costs a non-liquid draw nothing.
 *
 * WHY THIS IS NOT JUST sin().  The legacy CPU warp evaluates the same sine at
 * tile corners and lets the rasteriser interpolate linearly.  That reconstructs
 * the sine badly -- 93% peak error even at gl_subdivide_size 24, see
 * tools/warp_recon.py -- but it is inherently band-limited: it cannot alias in
 * screen space, because the samples are fixed in world space.  Evaluating the
 * true sine per pixel fixes the shape and forfeits that protection: at a
 * glancing angle one pixel can span many warp periods and the sine aliases into
 * banding.  That is what sank the first attempt (uhexen2-tlsh) and its
 * fwidth-threshold follow-up (uhexen2-famb).
 *
 * The band-limit here is analytic rather than a threshold.  The box average of
 * sin(k*x) over a footprint of width W is sin(k*x)*sinc(k*W/2), so scaling the
 * amplitude by that sinc IS the filtered signal: full throw up close, smoothly
 * to zero as the footprint grows, reaching zero precisely when one pixel covers
 * one whole warp period (k*W/2 = pi).  Clamped at the first zero so the sinc's
 * negative lobes cannot bring back a phase-inverted ripple further out.
 *
 * s is displaced by a sine of t and vice versa (EmitWaterPolys), so each axis
 * is attenuated by the OTHER axis' footprint. */
#define GLSL_TURB_UV_FN \
	"vec2 TurbUV(vec2 uv, vec2 turb) {\n" \
	"    const float K = 0.125;\n" \
	"    const float TAU = 6.2831853;\n" \
	"    vec2 raw = uv * 64.0;\n" \
	"    vec2 fw  = fwidth(raw);\n" \
	"    vec2 z   = 0.5 * K * vec2(fw.y, fw.x);\n" \
	"    vec2 att = vec2(z.x > 1e-4 ? sin(z.x) / z.x : 1.0,\n" \
	"                    z.y > 1e-4 ? sin(z.y) / z.y : 1.0);\n" \
	"    att = clamp(att, 0.0, 1.0);\n" \
	"    float as = mod(K * raw.y + turb.y, TAU);\n" \
	"    float at = mod(K * raw.x + turb.y, TAU);\n" \
	"    raw += turb.x * vec2(att.x * sin(as), att.y * sin(at));\n" \
	"    return raw * (1.0 / 64.0);\n" \
	"}\n"


/* Software-emulation noise, ported from Ironwail (Quake/gl_shaders.h,
 * NOISE_FUNCTIONS).  uhexen2-a5nn.3.
 *
 * bayer01 is the same ALU-only 16x16 ordered matrix gl_postprocess.c already
 * carries -- that one returns it centred on zero, this one in [0,1) because
 * tri() wants it that way.  tri() reshapes a uniform distribution into a
 * triangular one, which is what makes ordered dither read as film grain rather
 * than as a visible checker.
 *
 * whitenoise01 is "Hash without Sine", https://www.shadertoy.com/view/4djSRW
 *
 *   Copyright (c) 2014 David Hoskins.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 *   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 */
#define GLSL_SOFTEMU_NOISE_FN \
	GLSL_BITFIELD_REVERSE \
	"float bayer01(ivec2 coord) {\n" \
	"    coord &= 15;\n" \
	"    coord.y ^= coord.x;\n" \
	"    uint v = uint(coord.y | (coord.x << 8));\n" \
	"    v = (v ^ (v << 2)) & 0x3333u;\n" \
	"    v = (v ^ (v << 1)) & 0x5555u;\n" \
	"    v |= v >> 7;\n" \
	"    v = bitfieldReverse(v) >> 24;\n" \
	"    return float(v) * (1.0/256.0);\n" \
	"}\n" \
	"float whitenoise01(vec2 p) {\n" \
	"    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n" \
	"    p3 += dot(p3, p3.yzx + 33.33);\n" \
	"    return fract((p3.x + p3.y) * p3.z);\n" \
	"}\n" \
	/* Uniform [0,1] -> triangular [-1,1].  Based on
	 * https://www.shadertoy.com/view/4t2SDh */ \
	"float tri(float x) {\n" \
	"    float orig = x * 2.0 - 1.0;\n" \
	"    uint signbit = floatBitsToUint(orig) & 0x80000000u;\n" \
	"    x = sqrt(abs(orig)) - 1.0;\n" \
	"    x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);\n" \
	"    return x;\n" \
	"}\n" \
	"#define SCREEN_NOISE()     tri(bayer01(ivec2(floor(gl_FragCoord.xy) + 0.5)))\n" \
	"#define SUPPRESS_BANDING() (bayer01(ivec2(gl_FragCoord.xy)) - 0.5)\n"


/* Material maps -- tangent-space normal + specular from _norm/_bump/_gloss
 * sidecars.  uhexen2-mfql.
 *
 * TANGENT FRAME WITHOUT TANGENTS.  BSP surfaces carry no tangent attribute,
 * and adding one would mean a vertex-format change plus a generation pass
 * over every surface at load.  It is not needed: a tangent frame is fully
 * determined by how the texture coordinate varies across the surface, and
 * the fragment stage can read that straight off the screen-space derivatives
 * of position and UV (Christian Schuler's cotangent frame).  That costs a
 * handful of ALU, needs no new vertex data, and works unchanged for world
 * surfaces, brush ents and any other geometry that reaches this program.
 *
 * The geometric normal comes from cross(dFdx(p), dFdy(p)) rather than a
 * vertex normal, which is exact here -- every BSP surface is planar by
 * construction, so the face normal IS the surface normal with no
 * interpolation error.  Its sign follows triangle winding, so it is flipped
 * to face the viewer; in eye space "toward the viewer" is just -p.
 *
 * FAKE DELUXEMAPPING.  Hexen II bakes all lighting, dynamic lights included,
 * into the lightmap on the CPU (R_AddDynamicLights), so at this point there
 * is no light DIRECTION anywhere in the frame -- only an irradiance value.
 * Real per-pixel normal mapping needs one, and the ways to get it are a
 * deluxemap (which Hexen II BSPs do not have) or realtime lights (a far
 * larger project than this).  DarkPlaces hits the same wall and answers it
 * with r_glsl_deluxemapping 2, a fixed light vector in tangent space; this
 * is the same trick.  It is not physically correct and is not claimed to be:
 * it makes surface relief legible, which is the thing the feature was asked
 * for, and it does so consistently because the tangent frame is anchored to
 * the texture's own axes.
 *
 * BRIGHTNESS IS PRESERVED BY CONSTRUCTION.  The N.L term is divided by the
 * response a FLAT normal would have produced, so a flat region of a normal
 * map yields exactly 1.0 and multiplies the lightmap by nothing.  Only
 * deviation from flat shades.  This is what stops installing a normal-map
 * pack from globally darkening a map -- the naive form multiplies everything
 * by L.z and quietly dims the whole level.
 */
#define GLSL_MATERIAL_FN \
	"const vec3 MAT_LIGHTDIR = vec3(-0.35, -0.35, 0.868);\n" \
	"mat3 CotangentFrame(highp vec3 N, highp vec3 p, highp vec2 uv) {\n" \
	"    highp vec3 dp1 = dFdx(p);\n" \
	"    highp vec3 dp2 = dFdy(p);\n" \
	"    highp vec2 duv1 = dFdx(uv);\n" \
	"    highp vec2 duv2 = dFdy(uv);\n" \
	"    highp vec3 dp2perp = cross(dp2, N);\n" \
	"    highp vec3 dp1perp = cross(N, dp1);\n" \
	"    highp vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;\n" \
	"    highp vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;\n" \
	/* Degenerate UVs (a zero-area triangle, or a surface seen exactly
	 * edge-on) make both T and B zero and inversesqrt(0) infinite.  The
	 * max() floor keeps the frame finite; the normal it produces is
	 * meaningless there, but the fragment is sub-pixel and the alternative
	 * is a NaN that propagates into the final colour as a black or white
	 * speck. */ \
	"    highp float d = max(dot(T,T), dot(B,B));\n" \
	"    highp float invmax = (d > 0.0) ? inversesqrt(d) : 0.0;\n" \
	"    return mat3(T * invmax, B * invmax, N);\n" \
	"}\n" \
	/* Returns vec4(diffuse scale, specular rgb): .x multiplies the lightmap,
	 * .yzw is added after it.  One call rather than two so the cutout and
	 * opaque fragment variants cannot drift apart. */ \
	"vec4 MaterialShade(sampler2D normtex, sampler2D glosstex, highp vec2 uv,\n" \
	"                   highp vec3 p, vec2 params, float glossexp) {\n" \
	"    highp vec3 Ng = normalize(cross(dFdx(p), dFdy(p)));\n" \
	"    if (dot(Ng, p) > 0.0) Ng = -Ng;\n" \
	"    mat3 TBN = CotangentFrame(Ng, p, uv);\n" \
	/* Decode [0,1] -> [-1,1].  A pack that ships a flat 128,128,255 map,
	 * or the sentinel bound when it ships none, decodes to (0,0,1). */ \
	"    vec3 n = texture(normtex, uv).xyz * 2.0 - 1.0;\n" \
	"    n = normalize(mix(vec3(0.0, 0.0, 1.0), n, params.x));\n" \
	"    float ndotl = max(dot(n, MAT_LIGHTDIR), 0.0);\n" \
	/* Divide out the flat response so flat == no change.  MAT_LIGHTDIR.z
	 * is a nonzero literal, so no guard is needed. */ \
	"    float diffuse = ndotl / MAT_LIGHTDIR.z;\n" \
	"    vec3 spec = vec3(0.0);\n" \
	"    if (params.y > 0.0) {\n" \
	/* Camera is at the eye-space origin, so the view vector is -p. */ \
	"        highp vec3 V = normalize(-p) * TBN;\n" \
	"        vec3 H = normalize(MAT_LIGHTDIR + V);\n" \
	"        float s = pow(max(dot(n, H), 0.0), glossexp);\n" \
	"        spec = texture(glosstex, uv).rgb * s * params.y;\n" \
	"    }\n" \
	"    return vec4(diffuse, spec);\n" \
	"}\n"

/* CLUSTERED GPU DYNAMIC LIGHTING, the shading half.  uhexen2-26bm, phase B of
 * uhexen2-a5nn.1; gl_lightcluster.c is the half that fills the grid.
 *
 * Every fragment finds the froxel it sits in, reads that froxel's 128-bit light
 * mask, and evaluates the handful of lights the mask names.  What it replaces
 * is R_AddDynamicLights: a CPU walk that rewrote lightmap blocks and re-uploaded
 * every dirtied atlas page, every frame, for every torch.
 *
 * DESKTOP GL 4.3 ONLY, and not by choice.  The whole thing needs an SSBO and a
 * compute pass to fill the grid, and the ES/WebGL2 tier has neither (WebGL2 is
 * GLSL ES 3.00: no shader storage blocks, no findLSB).  The macros below are
 * therefore empty strings on that tier and the CPU path stays live there
 * permanently -- finding 1 on uhexen2-a5nn.1, and the reason this is an
 * ADDITIONAL path rather than a replacement.
 *
 * THE MATH IS R_AddDynamicLights, PER PIXEL.  Same shape: reduce the radius by
 * the light's distance to the surface plane, drop the light if what remains is
 * under its minlight, project the light onto the plane, and fall off linearly
 * from there.  Three differences, all of which make the GPU version the more
 * correct one, and the first of which is VISIBLE and worth knowing about:
 *
 *   - THE DISTANCE IS MEASURED IN WORLD UNITS.  The CPU path measures it in
 *     TEXTURE units: its `local` comes from DotProduct(impact, tex->vecs[i]),
 *     and a surface's texture vectors are the texture axes divided by its
 *     texture scale, so they are only unit length at scale 1.  The falloff then
 *     compares that texture-space distance against a radius in world units.
 *     The upshot is a dynamic light whose SIZE DEPENDS ON THE TEXTURE SCALE of
 *     whatever it lands on -- measured on demo1's corridor floor, |vecs| is
 *     0.6667 (scale 1.5), so the CPU pool there is half again as wide as the
 *     light's actual radius, and correspondingly brighter at any given point.
 *     This is inherited Quake behaviour, not something Hexen II added, and
 *     Ironwail's per-pixel path does not reproduce it.  Neither does this.
 *     EXPECT DYNAMIC LIGHTS TO TIGHTEN on scaled-texture surfaces; that is the
 *     bug being removed, and it is the bulk of what an A/B between the two
 *     paths shows.
 *   - THE IN-PLANE DISTANCE IS EUCLIDEAN, not the CPU's `max + min/2` octagon,
 *     a software-rasteriser trick for dodging a square root per luxel.  It
 *     over-estimates diagonal distance by about 6%, so it draws lights very
 *     slightly octagonal.  Small next to the above, and the opposite sign.
 *   - IT IS EVALUATED PER PIXEL, not per 16-unit luxel and interpolated.  Close
 *     up against a wall this is the visible win: the light no longer has a
 *     lattice.
 *
 * THE NORMAL COMES FROM SCREEN-SPACE DERIVATIVES of the eye-space position,
 * which is exact here -- every BSP face is planar by construction, so the face
 * normal IS the surface normal with no interpolation error.  Same reasoning as
 * the material-map tangent frame above, and the same reason neither needs a
 * normal attribute on the vertex.  Its sign follows triangle winding and does
 * NOT need correcting: the plane distance below is used through abs(), and the
 * projection N * dot(lpos - p, N) is unchanged when N flips.
 *
 * IT WORKS IN EYE SPACE, so lights are transformed by u_lightview per light per
 * fragment.  The froxel grid is defined in eye space, v_eyepos is the one
 * position this shader has that is correct for BOTH world surfaces and brush
 * ents (the entity transform lives inside u_modelview and never reaches
 * a_position), and one space for the lookup and the shading is one space to get
 * right.  u_lightview is the view matrix WITHOUT the entity transform, which is
 * exactly the matrix the grid was clustered with.
 *
 * A NOTE ON WHY THE NORMAL IS COMPUTED BEFORE THE MASK IS TESTED.  It is
 * tempting to bail out early on an empty mask, and wrong: derivatives are
 * undefined in non-uniform control flow, neighbouring pixels in a 2x2 quad
 * routinely fall in different froxels, and the resulting garbage normal would
 * show up as speckle along froxel boundaries.  The only branch this may sit
 * behind is `u_dlight_scale > 0.0`, which is uniform across the draw.
 */
#ifndef USE_GLES
/* Everything a consumer of the froxel grid needs except its own scale uniform.
 * Shared by the world programs and, since uhexen2-waum, by the alias family --
 * one copy of the SSBO layout and one copy of the slice mapping, because two
 * copies drifting is a bug that renders as "some things take dynamic light a
 * froxel early" rather than as anything obviously wrong. */
#define GLSL_LIGHTGRID_DECL \
	"struct Light { vec3 origin; float radius; vec3 color; float minlight; };\n" \
	/* Layout and binding must match gl_lightcluster.c's compute pass.  The
	 * LightStyles array is not read here -- light styles are a static
	 * lightmap concern and the CPU has already applied them -- but it has
	 * to be declared or std430 puts Lights[] at the wrong offset. */ \
	"layout(std430, binding = " GLSL_STR(LIGHT_SSBO_BINDING) ") restrict readonly buffer LightBuffer {\n" \
	"    float LightStyles[" GLSL_STR(MAX_LIGHTSTYLES) "];\n" \
	"    Light Lights[];\n" \
	"};\n" \
	"uniform usampler3D u_lightgrid;\n"	/* per-froxel light bitmasks */ \
	"uniform mat4 u_lightview;\n"		/* world -> eye, no entity transform */ \
	"uniform vec4 u_lightgrid_xy;\n"	/* xy viewport origin, zw froxels per pixel */ \
	"uniform vec2 u_lightgrid_z;\n"	/* zlogscale, zlogbias */

/* Which froxel an eye-space position falls in, and that froxel's 128-bit light
 * mask.  Both consumers call this rather than each inlining the mapping: the
 * z term is the exact inverse of the compute pass's `d = exp2((slice - bias) /
 * scale)`, and the whole feature is only correct while the three agree.
 *
 * max() guards log2 of a non-positive depth.  Geometry at or behind the near
 * plane is clipped, but a fragment exactly on it is not. */
#define GLSL_LIGHTGRID_FETCH_FN \
	"uvec4 LightGridMask(highp vec3 p) {\n" \
	"    vec3 tile;\n" \
	"    tile.xy = (gl_FragCoord.xy - u_lightgrid_xy.xy) * u_lightgrid_xy.zw;\n" \
	"    tile.z = log2(max(-p.z, 1.0)) * u_lightgrid_z.x + u_lightgrid_z.y;\n" \
	"    ivec3 t = clamp(ivec3(tile), ivec3(0), ivec3(" \
		GLSL_STR(LIGHT_TILES_X) " - 1, " GLSL_STR(LIGHT_TILES_Y) " - 1, " \
		GLSL_STR(LIGHT_TILES_Z) " - 1));\n" \
	"    return texelFetch(u_lightgrid, t, 0);\n" \
	"}\n"

#define GLSL_DLIGHT_DECL \
	GLSL_LIGHTGRID_DECL \
	"uniform float u_dlight_scale;\n"	/* blocklight -> lightmap units; 0 = path off */

#define GLSL_DLIGHT_FN \
	GLSL_LIGHTGRID_FETCH_FN \
	"vec3 ClusteredDlight(vec3 lm, highp vec3 p) {\n" \
	/* Geometric normal first -- see the header comment on derivatives. */ \
	"    highp vec3 N = normalize(cross(dFdx(p), dFdy(p)));\n" \
	"    uvec4 mask = LightGridMask(p);\n" \
	"    for (int w = 0; w < 4; w++) {\n" \
	"        uint bits = mask[w];\n" \
	"        while (bits != 0u) {\n" \
	"            int i = (w << 5) + findLSB(bits);\n" \
	"            bits &= bits - 1u;\n"	/* clear that bit before any continue */ \
	"            vec3 lpos = (u_lightview * vec4(Lights[i].origin, 1.0)).xyz;\n" \
	"            float pdist = dot(lpos - p, N);\n" \
	"            float rad = Lights[i].radius - abs(pdist);\n" \
	"            float ml = Lights[i].minlight;\n" \
	"            if (rad < ml) continue;\n" \
	"            float d = length((lpos - N * pdist) - p);\n" \
	"            if (d >= rad - ml) continue;\n" \
	/* Hexen II's subtractive lights (dlight_t.dark, EF_DARKLIGHT) ride
	 * with the colour negated by the gather in gl_lightcluster.c, so one
	 * accumulate serves both.  The clamp at zero is not decoration: it is
	 * what the `dark` arm of R_AddDynamicLights does, clamping against the
	 * running total rather than the final one, which is why this clamps
	 * inside the loop and not after it.  Lights are visited in ascending
	 * index order -- findLSB within a word, words in order -- which is the
	 * order the CPU applies them in, so the two agree even where a dark
	 * light and a bright one overlap.
	 *
	 * ONE PLACE THEY CANNOT AGREE, and it is not fixable here.  The CPU
	 * subtracts from a 32-bit blocklight accumulator that has not been
	 * clamped yet, so on a surface whose static light already exceeds full
	 * scale a dark light eats headroom the player never sees and appears to
	 * do nothing.  All this shader has is the atlas texel, which was clamped
	 * to 255 on the way in, so the same dark light visibly darkens.  Ours is
	 * the behaviour the effect was presumably after; recording it because it
	 * is a genuine difference an A/B in a very bright room will show, not a
	 * bug to go hunting for.  Reproducing the CPU exactly would mean
	 * carrying the unclamped sum, which the 8-bit atlas cannot. */ \
	"            lm = max(lm + (rad - d) * u_dlight_scale * Lights[i].color, vec3(0.0));\n" \
	"        }\n" \
	"    }\n" \
	/* R_BuildLightMap clamps the blocklight sum at 255 before it reaches the
	 * atlas, so the CPU path cannot exceed a full lightmap either.  Matching
	 * it keeps the A/B honest.  There IS headroom to be had here -- nothing
	 * forces a GPU-evaluated light through an 8-bit texel -- but taking it
	 * would be a brightness change dressed up as a port. */ \
	"    return min(lm, vec3(1.0));\n" \
	"}\n"

#define GLSL_DLIGHT_APPLY \
	"    if (u_dlight_scale > 0.0) lm.rgb = ClusteredDlight(lm.rgb, v_eyepos);\n"

/* --- the alias family's half of the same grid (uhexen2-waum, phase C) ---
 *
 * WHAT THIS REPLACES.  Every alias model used to take the whole frame's
 * dynamic light as ONE number, computed once at the entity's ORIGIN
 * (R_DrawAliasModel's `add = radius - VectorLengthFast(e->origin - dl->origin)`
 * loop) and folded flat into the vertex colour.  A torch at a monster's feet
 * lit its head exactly as much as its boots, and a model straddling a light's
 * edge either took all of it or none.  This evaluates the same expression per
 * pixel instead, from the froxel grid phase A builds.
 *
 * IT IS NOT A REUSE OF ClusteredDlight ABOVE, and the bead that filed this
 * expected it to be.  Two reasons, both structural:
 *
 *  - THE FALLOFF IS A DIFFERENT FUNCTION.  The world one is R_AddDynamicLights'
 *    -- project the light onto the surface plane, subtract the perpendicular
 *    distance from the radius, then subtract the in-plane distance.  That shape
 *    exists because a lightmap luxel IS a point on a plane.  The alias CPU term
 *    this replaces is a plain `radius - distance`, and matching it is the whole
 *    point: the only thing phase C is meant to change is WHERE the expression
 *    is evaluated, not what it computes.
 *  - THE NORMAL WOULD BE WRONG.  ClusteredDlight recovers its plane from
 *    screen-space derivatives, which is exact for world surfaces because every
 *    BSP face is planar.  An alias model is not, so the same trick yields the
 *    flat triangle normal and the light would break into visible facets on
 *    exactly the low-poly meshes Hexen II is made of.
 *
 * WHAT DELIBERATELY DOES NOT CARRY OVER FROM THE CPU TERM.  There, the dlight
 * sum is added to the entity's static light BEFORE the vertex is multiplied by
 * the shadedots table (Quake's fake directional shading, keyed to the entity's
 * quantized yaw) and by the colorshade tint.  Here it is added after, so a
 * dynamic light is neither modulated by a fake light direction that has nothing
 * to do with where the torch actually is, nor tinted by a palette recolour that
 * belongs to the skin.  Both are defensible the other way; what is NOT
 * defensible is having the three alias paths disagree, and the shadedot is
 * recoverable in only two of them -- the legacy path streams a finished colour
 * through GL_ImmColor4f with the dot already multiplied in.  Uniformity decided
 * it.  Expect dynamic light on models to sit slightly flatter across a surface
 * and slightly stronger on faces turned away from the shadevector.
 *
 * u_alias_dlight is per-BATCH state and named apart from the world's
 * u_dlight_scale for the reason u_alias_caustics is named apart from
 * u_caustics: this fragment shader also serves sprites, particles, warp polys
 * and unlit brush polys through GL_ImmEnd, and a shared name would let one
 * path's value leak into another's draw.  It is a VERTEX-stage uniform -- the
 * fragment stage reads the scale off v_dlightscale instead, for the reason
 * below.
 *
 * v_dlightscale, not u_alias_dlight.x, is what the loop is gated on.  The
 * instanced path batches entities whose CPU lighting came from DIFFERENT arms
 * of R_DrawAliasModel's branch chain -- an MLS_ABSLIGHT model and an ordinary
 * one can share a draw call -- and only the ordinary arm ever accumulated
 * dlights.  A uniform cannot distinguish them; a flat varying the vertex shader
 * fills from the instance record can.  The other two paths set it from the
 * uniform unchanged. */
#define GLSL_ALIAS_DLIGHT_DECL \
	GLSL_LIGHTGRID_DECL

#define GLSL_ALIAS_DLIGHT_FN \
	GLSL_LIGHTGRID_FETCH_FN \
	"vec3 ClusteredAliasDlight(vec3 lit, highp vec3 p, float scale) {\n" \
	"    uvec4 mask = LightGridMask(p);\n" \
	"    for (int w = 0; w < 4; w++) {\n" \
	"        uint bits = mask[w];\n" \
	"        while (bits != 0u) {\n" \
	"            int i = (w << 5) + findLSB(bits);\n" \
	"            bits &= bits - 1u;\n"	/* clear that bit before any continue */ \
	"            vec3 lpos = (u_lightview * vec4(Lights[i].origin, 1.0)).xyz;\n" \
	"            float add = Lights[i].radius - distance(lpos, p);\n" \
	"            if (add <= 0.0) continue;\n" \
	/* Subtractive lights (dlight_t.dark) arrive with their colour negated by
	 * the gather in gl_lightcluster.c, so one accumulate serves both kinds.
	 * The clamp is inside the loop and against the RUNNING total, which is
	 * what the CPU term did -- its dark arm clamps each channel at zero as it
	 * subtracts, so a dark light cannot bank negative headroom that a later
	 * bright one spends.  Ascending light index is the CPU's order too
	 * (findLSB within a word, words in order), so the two agree even where a
	 * dark light and a bright one overlap the same pixel. */ \
	"            lit = max(lit + add * scale * Lights[i].color, vec3(0.0));\n" \
	"        }\n" \
	"    }\n" \
	/* NO UPPER CLAMP, and that is a decision rather than an omission.
	 * gl_overbright_models 0 makes the CPU clamp each lightcolor channel at
	 * 192 after its dlight loop -- but it does that BEFORE the shadedots
	 * multiply, and `lit` here is already past it: v_color carries
	 * static * shadedot * tint, and shadedot runs to 2.0.  Re-applying the
	 * same ceiling on this side of that multiply would cap a legitimately
	 * bright vertex at 0.96 and visibly darken models that took no dynamic
	 * light at all.  The faithful expression, min(static + dlight, 192) *
	 * shadedot, needs the two factors separately and the legacy path has
	 * already multiplied them together in GL_ImmColor4f.
	 *
	 * Leaving it off is consistent with the shadedot decision above -- a
	 * dlight that is not modulated by the fake light direction has no reason
	 * to be bounded by a ceiling derived from it -- and it costs little:
	 * gl_overbright_models defaults to on, where the CPU path has no ceiling
	 * either, and even at the worst case (a full-radius light one unit away)
	 * the term adds about 1.0, which is where the CPU's own clamped result
	 * lands too. */ \
	"    return lit;\n" \
	"}\n"

/* Replaces `vec4 color = tex * v_color;` outright rather than adding a line
 * after it, so the ES arm below can be that statement verbatim and the two
 * tiers stay provably identical when the path is off. */
#define GLSL_ALIAS_DLIGHT_APPLY \
	"    vec3 lit = v_color.rgb;\n" \
	"    if (v_dlightscale > 0.0) lit = ClusteredAliasDlight(lit, v_eyepos, v_dlightscale);\n" \
	"    vec4 color = vec4(tex.rgb * lit, tex.a * v_color.a);\n"

/* The varying pair the above needs, declared on both sides of the link.  Kept
 * out of the ES arm entirely: an `in` with no matching `out` is a link error,
 * and there is nothing on that tier to fill them. */
#define GLSL_ALIAS_DLIGHT_VS_OUT \
	"out highp vec3 v_eyepos;\n" \
	"flat out float v_dlightscale;\n" \
	"uniform float u_alias_dlight;\n"	/* blocklight -> vertex-colour scale; 0 = path off */
#define GLSL_ALIAS_DLIGHT_FS_IN \
	"in highp vec3 v_eyepos;\n" \
	"flat in float v_dlightscale;\n"

/* Fills them, for the two vertex shaders that already have `eyepos` in hand.
 * salias_inst_vert does its own -- it works in world space and has a per-
 * instance switch to consult, so it has neither of this line's inputs. */
#define GLSL_ALIAS_DLIGHT_VS_CALC \
	"    v_eyepos = eyepos.xyz;\n" \
	"    v_dlightscale = u_alias_dlight;\n"

#else	/* ES/WebGL2: no SSBO, no findLSB, no compute pass to fill the grid */
#define GLSL_DLIGHT_DECL	""
#define GLSL_DLIGHT_FN		""
#define GLSL_DLIGHT_APPLY	""
#define GLSL_ALIAS_DLIGHT_DECL	""
#define GLSL_ALIAS_DLIGHT_FN	""
#define GLSL_ALIAS_DLIGHT_APPLY	"    vec4 color = tex * v_color;\n"
#define GLSL_ALIAS_DLIGHT_VS_OUT	""
#define GLSL_ALIAS_DLIGHT_FS_IN		""
#define GLSL_ALIAS_DLIGHT_VS_CALC	""
#endif

static const char sworld_frag[] =
	GLSL_FRAG_HEADER
	GLSL_EARLY_Z
	"uniform sampler2D u_texture0;\n"	/* diffuse */
	"uniform sampler2D u_texture1;\n"	/* lightmap atlas */
	"uniform sampler2D u_texture2;\n"	/* fullbright mask (uhexen2-sjvf) */
	"uniform sampler2D u_texture3;\n"	/* normal map, flat sentinel when absent (uhexen2-mfql) */
	"uniform sampler2D u_texture4;\n"	/* gloss map, black sentinel when absent (uhexen2-mfql) */
	"uniform vec3 u_material;\n"		/* x=normalmap intensity, y=gloss intensity, z=gloss exponent; x=y=0 disables */
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_alpha_threshold;\n"
	"uniform float u_force_opaque_alpha;\n"	/* uhexen2-khsa r13 */
	"uniform vec2 u_caustics;\n"		/* x=intensity (0=off), y=time (uhexen2-6bfm) */
	"uniform vec2 u_turb;\n"		/* liquid warp: x=amplitude in texture units (0=off), y=time */
	"uniform float u_overbright;\n"		/* lightmap multiplier: 1.0=off, 2.0=on (uhexen2-f29y) */
	"uniform float u_lightmap_bicubic;\n"	/* 0=bilinear, 1=4-tap B-spline bicubic (uhexen2-b2f0) */
	"uniform vec2 u_lightdebug;\n"		/* x=r_fullbright, y=r_lightmap (uhexen2-isq7) */
	/* Software emulation, decomposed the way Ironwail decomposes it
	 * (uhexen2-a5nn.3).  All four are 0 unless r_softemu is on, which it is
	 * not by default, so a stock configuration pays four scalar compares.
	 *   x = r_softemu_dither_texture scale (0 = off)
	 *   y = 1 when the lightmap is banded to 64 levels (r_softemu_lightmap_banding)
	 *   z = r_softemu_dither_screen scale for the far-field noise (0 = off)
	 *   w = 1 when any softemu stage is live.  Upstream expresses this as a
	 *       DITHER axis on the world program; a uniform-static branch costs
	 *       one compare and saves three more programs. */
	"uniform vec4 u_softemu;\n"
	GLSL_DLIGHT_DECL
	"in highp vec2 v_texcoord;\n"	/* highp: see sworld_vert (uhexen2-mfql) */
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	"in highp vec3 v_eyepos;\n"	/* highp: see sworld_vert (uhexen2-mfql) */
	"out vec4 fragColor;\n"
	GLSL_BICUBIC_LM_FN
	GLSL_CAUSTICS_FN
	GLSL_TURB_UV_FN
	GLSL_SOFTEMU_NOISE_FN
	GLSL_MATERIAL_FN
	GLSL_DLIGHT_FN
	"void main() {\n"
	/* Lit water rides this program (uhexen2-a5nn.2), so the liquid warp has
	 * to live here too.  u_turb.x is 0 for every other draw -- only
	 * EmitWaterPolys raises it, and only under r_water_pixel_warp 1 -- so uv
	 * is v_texcoord untouched for the whole world.  Everything keyed to the
	 * diffuse texture space follows uv; the lightmap deliberately does not,
	 * because warping the light along with the texture would drag the
	 * shoreline shading around with the ripple. */
	"    vec2 uv = v_texcoord;\n"
	"    if (u_turb.x > 0.0) uv = TurbUV(v_texcoord, u_turb);\n"
	/* Negative LOD bias under softemu, as upstream: the 1997 rasteriser had
	 * no trilinear blend, so biasing toward the sharper mip is closer to it
	 * than letting the hardware pick.  -1 where the lightmap is banded too,
	 * -0.5 otherwise; 0 (a no-op) when softemu is off. */
	"    float lodbias = (u_softemu.w > 0.0) ? ((u_softemu.y > 0.0) ? -1.0 : -0.5) : 0.0;\n"
	"    vec4 tex = texture(u_texture0, uv, lodbias);\n"
	/* Snap the lightmap coordinate to the luxel grid at 1/16 texel, which is
	 * the resolution the software renderer interpolated light at.  Without
	 * it the banded lightmap below still fades smoothly between luxels and
	 * the effect reads as a posterised gradient rather than as bands. */
	"    vec2 lmsize = vec2(textureSize(u_texture1, 0)) * 16.0;\n"
	"    vec2 lmuv = v_lmcoord;\n"
	"    if (u_softemu.w > 0.0)\n"
	"        lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;\n"
	"    vec4 lm = (u_lightmap_bicubic > 0.5)\n"
	"        ? BicubicLightmap(u_texture1, lmuv)\n"
	/* r_fullbright / r_lightmap.  Applied to the samples rather than to the
	 * final colour so everything downstream -- overbright, the fullbright
	 * mask, caustics, fog, the alpha test -- keeps seeing a well-formed
	 * value.  rgb only on the diffuse: tex.a still gates the alpha test, so
	 * r_lightmap must not turn a fence into a solid sheet.  uhexen2-isq7. */
	"        : texture(u_texture1, lmuv);\n"
	/* Clustered dynamic lights, added to the static lightmap sample before
	 * anything downstream sees it -- the CPU path they replace baked into
	 * exactly this value, so banding, the debug views, overbright and the
	 * material weighting all have to keep acting on the sum.  uhexen2-26bm. */
	GLSL_DLIGHT_APPLY
	/* 64 light levels, which is what colormap.lmp had.  Upstream folds its
	 * overbright doubling into the same expression; ours stays separate in
	 * u_overbright, so the scale here is 1/63 rather than 2/63. */
	"    if (u_softemu.y > 0.0)\n"
	"        lm.rgb = floor(lm.rgb * 63.0 + 0.5) * (1.0/63.0);\n"
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
	"    vec3 fb = texture(u_texture2, uv).rgb * (1.0 - u_lightdebug.y);\n"
	/* Material maps.  Branch is uniform-static and therefore coherent across
	 * the whole draw, so a map with no _norm/_bump/_gloss anywhere -- or a
	 * user with r_materialmaps 0 -- pays one scalar compare per fragment and
	 * skips the derivatives, the frame build and both texture fetches.
	 * uhexen2-mfql. */
	"    vec3 matspec = vec3(0.0);\n"
	"    if (u_material.x > 0.0 || u_material.y > 0.0) {\n"
	"        vec4 m = MaterialShade(u_texture3, u_texture4, uv,\n"
	"                               v_eyepos, u_material.xy, u_material.z);\n"
	/* Weight the highlight by the lightmap BEFORE the relief term is folded
	 * into it.  Relief redistributes the light that is already arriving;
	 * applying it to the specular as well would count it twice and make lit
	 * bumps bloom.  The r_lightmap debug view (u_lightdebug.y) drops the
	 * highlight entirely, for the same reason it drops the fullbright add --
	 * that view is meant to show lighting alone. */
	"        matspec = m.yzw * lm.rgb * (1.0 - u_lightdebug.y);\n"
	"        lm.rgb *= m.x;\n"
	"    }\n"
	"    vec4 color = tex * lm * v_color;\n"
	"    color.rgb *= u_overbright;\n"		/* Ironwail-style overbright (uhexen2-f29y) */
	"    if (color.a < u_alpha_threshold) discard;\n"
	"    color.rgb += fb;\n"
	"    color.rgb += matspec;\n"
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
	/* Texture-space dither.  Two noises blended by how much world space one
	 * pixel covers: up close, white noise keyed to the LIGHTMAP coordinate,
	 * so the grain sticks to the surface the way the software renderer's
	 * per-luxel quantisation did; far away, where that grain would alias
	 * into a shimmer, an ordered screen-space pattern instead.  Both are
	 * applied in gamma space (the sqrt/square pair) because that is where
	 * the palette quantisation downstream happens.  Ironwail
	 * Quake/gl_shaders.h, the DITHER == 1 tail of its world shader.
	 *
	 * fwidth of the EYE-space position, not the world one upstream uses:
	 * eye space is a rigid transform of world space, so the magnitude this
	 * asks for is the same, and v_eyepos is the position this shader
	 * actually has for a brush entity. */
	"    if (u_softemu.x > 0.0 || u_softemu.z > 0.0) {\n"
	"        vec3 dpos = fwidth(v_eyepos);\n"
	"        float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);\n"
	"        farblend *= farblend;\n"
	"        color.rgb = sqrt(max(color.rgb, vec3(0.0)));\n"
	"        float luma = dot(color.rgb, vec3(0.25, 0.625, 0.125));\n"
	"        float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * u_softemu.x;\n"
	"        float farnoise = (u_fog_density > 0.0) ? SCREEN_NOISE() * u_softemu.z : 0.0;\n"
	"        color.rgb += mix(nearnoise, farnoise, farblend);\n"
	"        color.rgb *= color.rgb;\n"
	"    }\n"
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
	"uniform sampler2D u_texture0;\n"	/* diffuse */
	"uniform sampler2D u_texture1;\n"	/* lightmap atlas */
	"uniform sampler2D u_texture2;\n"	/* fullbright mask */
	"uniform sampler2D u_texture3;\n"	/* normal map, flat sentinel when absent (uhexen2-mfql) */
	"uniform sampler2D u_texture4;\n"	/* gloss map, black sentinel when absent (uhexen2-mfql) */
	"uniform vec3 u_material;\n"		/* x=normalmap intensity, y=gloss intensity, z=gloss exponent; x=y=0 disables */
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_alpha_threshold;\n"	/* unused but kept for layout parity */
	"uniform vec2 u_caustics;\n"		/* x=intensity, y=time (uhexen2-6bfm) */
	"uniform float u_overbright;\n"		/* lightmap multiplier: 1.0=off, 2.0=on (uhexen2-f29y) */
	"uniform float u_lightmap_bicubic;\n"	/* 0=bilinear, 1=4-tap B-spline bicubic (uhexen2-b2f0) */
	/* Same uniform layout as sworld_frag, deliberately: the two are switched
	 * between by glUseProgram plus a re-upload and no call site knows which
	 * it has.  Most of the world draws through THIS one -- the MDI dispatch
	 * and the DrawTextureChains fast path -- so leaving softemu out of it
	 * would have meant the modes reached only fences and brush ents.
	 * uhexen2-a5nn.3. */
	"uniform vec4 u_softemu;\n"
	"uniform vec2 u_lightdebug;\n"		/* x=r_fullbright, y=r_lightmap (uhexen2-isq7) */
	GLSL_DLIGHT_DECL
	"in highp vec2 v_texcoord;\n"	/* highp: see sworld_vert (uhexen2-mfql) */
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	"in highp vec3 v_eyepos;\n"	/* highp: see sworld_vert (uhexen2-mfql) */
	"out vec4 fragColor;\n"
	GLSL_BICUBIC_LM_FN
	GLSL_CAUSTICS_FN
	GLSL_SOFTEMU_NOISE_FN
	GLSL_MATERIAL_FN
	GLSL_DLIGHT_FN
	"void main() {\n"
	"    float lodbias = (u_softemu.w > 0.0) ? ((u_softemu.y > 0.0) ? -1.0 : -0.5) : 0.0;\n"
	"    vec4 tex = texture(u_texture0, v_texcoord, lodbias);\n"
	"    vec2 lmsize = vec2(textureSize(u_texture1, 0)) * 16.0;\n"
	"    vec2 lmuv = v_lmcoord;\n"
	"    if (u_softemu.w > 0.0)\n"
	"        lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;\n"
	"    vec4 lm = (u_lightmap_bicubic > 0.5)\n"
	"        ? BicubicLightmap(u_texture1, lmuv)\n"
	"        : texture(u_texture1, lmuv);\n"
	GLSL_DLIGHT_APPLY	/* before banding and the debug views; see sworld_frag */
	"    if (u_softemu.y > 0.0)\n"
	"        lm.rgb = floor(lm.rgb * 63.0 + 0.5) * (1.0/63.0);\n"
	"    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);\n"	/* uhexen2-isq7 */
	"    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);\n"
	/* Material maps.  Branch is uniform-static and therefore coherent across
	 * the whole draw, so a map with no _norm/_bump/_gloss anywhere -- or a
	 * user with r_materialmaps 0 -- pays one scalar compare per fragment and
	 * skips the derivatives, the frame build and both texture fetches.
	 * uhexen2-mfql. */
	"    vec3 matspec = vec3(0.0);\n"
	"    if (u_material.x > 0.0 || u_material.y > 0.0) {\n"
	"        vec4 m = MaterialShade(u_texture3, u_texture4, v_texcoord,\n"
	"                               v_eyepos, u_material.xy, u_material.z);\n"
	/* Weight the highlight by the lightmap BEFORE the relief term is folded
	 * into it.  Relief redistributes the light that is already arriving;
	 * applying it to the specular as well would count it twice and make lit
	 * bumps bloom.  The r_lightmap debug view (u_lightdebug.y) drops the
	 * highlight entirely, for the same reason it drops the fullbright add --
	 * that view is meant to show lighting alone. */
	"        matspec = m.yzw * lm.rgb * (1.0 - u_lightdebug.y);\n"
	"        lm.rgb *= m.x;\n"
	"    }\n"
	"    vec4 color = tex * lm * v_color;\n"
	"    color.rgb *= u_overbright;\n"		/* Ironwail-style overbright (uhexen2-f29y) */
	"    vec3 fb = texture(u_texture2, v_texcoord).rgb * (1.0 - u_lightdebug.y);\n"
	"    color.rgb += fb;\n"
	"    color.rgb += matspec;\n"
	"    if (u_caustics.x > 0.0) {\n"
	"        float c = Caustics(v_worldxy, u_caustics.y);\n"
	"        color.rgb += color.rgb * c * u_caustics.x;\n"
	"    }\n"
	"    float fogfac = u_fog_density * v_fogdist;\n"
	"    float fog = exp(-fogfac * fogfac);\n"
	"    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));\n"
	/* The DITHER == 1 tail; see sworld_frag for what the two noises are for. */
	"    if (u_softemu.x > 0.0 || u_softemu.z > 0.0) {\n"
	"        vec3 dpos = fwidth(v_eyepos);\n"
	"        float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);\n"
	"        farblend *= farblend;\n"
	"        color.rgb = sqrt(max(color.rgb, vec3(0.0)));\n"
	"        float luma = dot(color.rgb, vec3(0.25, 0.625, 0.125));\n"
	"        float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * u_softemu.x;\n"
	"        float farnoise = (u_fog_density > 0.0) ? SCREEN_NOISE() * u_softemu.z : 0.0;\n"
	"        color.rgb += mix(nearnoise, farnoise, farblend);\n"
	"        color.rgb *= color.rgb;\n"
	"    }\n"
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
	/* Affine texture mapping, the way the software rasteriser did it
	 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform
	 * can switch, so this is a compile-time variant -- the only stage of
	 * r_softemu that needs one.  Empty object-like macro when NOPERSP is not
	 * defined, so the qualifier simply is not there. */
	"#ifdef NOPERSP\n"
	"#define TCQUAL noperspective\n"
	"#else\n"
	"#define TCQUAL\n"
	"#endif\n"
	"in vec3 a_position;\n"
	"in vec2 a_texcoord;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	/* Model-only matrix (entity rotation + scale_origin + scale, no view).
	 * u_modelview is view*model, so it lands vertices in EYE space — sampling
	 * caustics there would make the pattern swim with the camera.  C uploads
	 * the same transform chain R_DrawAliasModel pushes onto the matrix stack,
	 * rebuilt on an identity base.  Defaults to identity so the sprite / warp
	 * / brush-poly batches that share this program (which submit world-space
	 * positions already) stay correct.  uhexen2-0gn3. */
	"uniform mat4 u_alias_model;\n"
	"TCQUAL out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	GLSL_ALIAS_DLIGHT_VS_OUT
	"invariant gl_Position;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = a_color;\n"
	"    v_worldxy = (u_alias_model * vec4(a_position, 1.0)).xy;\n"
	"    vec4 eyepos = u_modelview * vec4(a_position, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	GLSL_ALIAS_DLIGHT_VS_CALC
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char salias_frag[] =
	GLSL_FRAG_HEADER
	/* Affine texture mapping, the way the software rasteriser did it
	 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform
	 * can switch, so this is a compile-time variant -- the only stage of
	 * r_softemu that needs one.  Empty object-like macro when NOPERSP is not
	 * defined, so the qualifier simply is not there. */
	"#ifdef NOPERSP\n"
	"#define TCQUAL noperspective\n"
	"#else\n"
	"#define TCQUAL\n"
	"#endif\n"
	"uniform sampler2D u_texture0;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_alpha_threshold;\n"
	"uniform float u_force_opaque_alpha;\n"	/* uhexen2-khsa r13 */
	"uniform vec2 u_alias_caustics;\n"	/* x=intensity (0=off), y=time (uhexen2-0gn3) */
	"uniform vec2 u_turb;\n"		/* x=warp amplitude in texture units (0=off), y=time (uhexen2-9o7u) */
	/* Soft particles (uhexen2-mf9u).  Explicitly highp: GLSL_FRAG_HEADER
	 * defaults the ES tier to mediump, whose 10-bit mantissa cannot tell
	 * two window-space depths apart, and the whole fade is a difference of
	 * two of them.  Desktop GLSL accepts the qualifier and ignores it. */
	"uniform highp sampler2D u_soft_depth;\n"
	"uniform highp vec3 u_soft_params;\n"	/* x=1/fade distance (0=off), yz=depth linearization */
	"TCQUAL in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"in float v_fogdist;\n"
	"in vec2 v_worldxy;\n"
	GLSL_ALIAS_DLIGHT_FS_IN
	"out vec4 fragColor;\n"
	GLSL_ALIAS_DLIGHT_DECL
	GLSL_CAUSTICS_FN
	GLSL_TURB_UV_FN
	GLSL_ALIAS_DLIGHT_FN
	"void main() {\n"
	/* Per-pixel liquid warp (uhexen2-9o7u).  Off unless C sets u_turb.x, which
	 * only EmitWaterPolys does, and only under r_water_pixel_warp 1 -- every
	 * other user of this program (models, sprites, brush polys) leaves it 0.
	 * The maths, and why it is not just sin(), live on GLSL_TURB_UV_FN; a lit
	 * liquid face takes the world program instead of this one and has to
	 * ripple identically, so the two share one copy.  uhexen2-a5nn.2. */
	"    vec2 uv = v_texcoord;\n"
	"    if (u_turb.x > 0.0) uv = TurbUV(v_texcoord, u_turb);\n"
	/* Point-sample one mip, offset half a texel.  The qualifier alone is not
	 * the look: the rasteriser had no mip chain and no bilinear blend, and
	 * upstream's variant takes both away too (Ironwail gl_shaders.h, the
	 * ALIASSHADER_NOPERSP branch).  uhexen2-ktjv. */
	"#ifdef NOPERSP\n"
	"    uv -= 0.5 / vec2(textureSize(u_texture0, 0));\n"
	"    vec4 tex = textureLod(u_texture0, uv, 0.0);\n"
	"#else\n"
	"    vec4 tex = texture(u_texture0, uv);\n"
	"#endif\n"
	GLSL_ALIAS_DLIGHT_APPLY
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
	/* Affine texture mapping, the way the software rasteriser did it
	 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform
	 * can switch, so this is a compile-time variant -- the only stage of
	 * r_softemu that needs one.  Empty object-like macro when NOPERSP is not
	 * defined, so the qualifier simply is not there. */
	"#ifdef NOPERSP\n"
	"#define TCQUAL noperspective\n"
	"#else\n"
	"#define TCQUAL\n"
	"#endif\n"
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
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_modelview;\n"
	"uniform mat4 u_alias_model;\n"	/* see salias_vert (uhexen2-0gn3) */
	/* Pose selection.  Two bases rather than one so an MD5 model lerps
	 * between keyframes like every other alias model does: u_pose_base is
	 * the pose R_AliasResolveLerp resolved as current, u_pose_base2 the one
	 * it is blending away from, and u_skel_blend runs 0..1 from the latter
	 * to the former.  Both are already multiplied by numbones CPU-side.
	 * uhexen2-7ok0.3. */
	"uniform int u_pose_base;\n"
	"uniform int u_pose_base2;\n"
	"uniform float u_skel_blend;\n"
	/* Lighting.  The legacy path looks up shadedots[lightnormalindex],
	 * a table of `1.0 + dot(normal, shadevector)` with negative dots scaled
	 * by 0.3.  A skinned vertex has a real normal rather than an index into
	 * Quake's 162-normal set, so evaluate the same expression directly and
	 * the two paths agree on the same model to within the table's
	 * quantization.  u_shadevector is in MODEL space, matching the
	 * -e->angles[1] construction in R_DrawAliasModel. */
	"uniform vec3 u_shadevector;\n"
	"uniform vec4 u_lightcolor;\n"	/* rgb = light (tint + scale folded in), a = entity alpha */
	"uniform float u_fullbright;\n"	/* 1.0 during the additive fullbright re-draw */
	"\n"
	"TCQUAL out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	GLSL_ALIAS_DLIGHT_VS_OUT
	"invariant gl_Position;\n"
	"\n"
	/* Bone matrix for influence `i`, already blended across the two poses.
	 * mat3x4 supports scalar multiply and matrix add, so the blend is a
	 * plain linear combination -- mix() is genType-only and does not accept
	 * matrices.  Linear blending of two adjacent keyframes' skinning
	 * matrices is the standard approximation and is what the equivalent
	 * vertex-lerp does on the trivertx path. */
	"mat3x4 BonePose(uint i) {\n"
	"    return bones[u_pose_base  + int(i)] * u_skel_blend +\n"
	"           bones[u_pose_base2 + int(i)] * (1.0 - u_skel_blend);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    mat3x4 blend = BonePose(a_indices.x) * a_weights.x;\n"
	"    if (a_weights.y > 0.0) blend += BonePose(a_indices.y) * a_weights.y;\n"
	"    if (a_weights.z > 0.0) blend += BonePose(a_indices.z) * a_weights.z;\n"
	"    if (a_weights.w > 0.0) blend += BonePose(a_indices.w) * a_weights.w;\n"
	"\n"
	"    mat4x3 mat_3x4 = transpose(blend);\n"
	"    vec3 skinned_pos = mat_3x4 * vec4(a_position, 1.0);\n"
	"    vec3 skinned_normal = normalize(mat_3x4 * vec4(a_normal.xyz, 0.0));\n"
	"\n"
	"    float d = dot(skinned_normal, u_shadevector);\n"
	"    if (d < 0.0) d *= 0.3;\n"
	"    vec3 lit = u_lightcolor.rgb * (1.0 + d);\n"
	"\n"
	"    v_texcoord = a_texcoord;\n"
	"    v_color = vec4(mix(lit, vec3(1.0), u_fullbright), u_lightcolor.a);\n"
	"    v_worldxy = (u_alias_model * vec4(skinned_pos, 1.0)).xy;\n"
	"    vec4 eyepos = u_modelview * vec4(skinned_pos, 1.0);\n"
	"    v_fogdist = length(eyepos.xyz);\n"
	GLSL_ALIAS_DLIGHT_VS_CALC
	"    gl_Position = u_mvp * vec4(skinned_pos, 1.0);\n"
	"}\n";
#endif /* !USE_GLES */

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
	/* Affine texture mapping, the way the software rasteriser did it
	 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform
	 * can switch, so this is a compile-time variant -- the only stage of
	 * r_softemu that needs one.  Empty object-like macro when NOPERSP is not
	 * defined, so the qualifier simply is not there. */
	"#ifdef NOPERSP\n"
	"#define TCQUAL noperspective\n"
	"#else\n"
	"#define TCQUAL\n"
	"#endif\n"
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
	"uniform mat4 u_viewproj;\n"
	"uniform int u_inst_base;\n"
	"uniform vec3 u_eyepos;\n"
	"uniform int u_poseverttype;\n"  /* 0=PV_QUAKE1, 1=PV_MD3 */
	/* World -> eye, the same matrix gl_lightcluster.c clustered the grid
	 * with.  This path has no u_modelview to fall out of: it builds
	 * gl_Position straight from u_viewproj and a world-space vertex, so eye
	 * space has to be reconstructed explicitly for the froxel lookup.  Shared
	 * by name with salias_frag's copy, which is one uniform after linking.
	 * uhexen2-waum. */
	"uniform mat4 u_lightview;\n"
	"\n"
	"TCQUAL out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdist;\n"
	"out vec2 v_worldxy;\n"
	GLSL_ALIAS_DLIGHT_VS_OUT
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
	/* Byte 6 is longitude and byte 7 is latitude: the file stores one
	 * little-endian short whose LOW byte is longitude (Quake III
	 * tr_surface.c, lat = normal >> 8).  This had the two the wrong way
	 * round, which nothing caught because until uhexen2-2ah9 no loader
	 * existed to reach it.  md3mesh.c's MD3_DecodeNormal is the CPU twin
	 * of this and must stay in step. */
	"vec3 MD3_DecodeNormal(uvec2 vdata) {\n"
	"    uint lngbyte = (vdata.y >> 16) & 0xFFu;\n"
	"    uint latbyte = (vdata.y >> 24) & 0xFFu;\n"
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
	/* MD3 carries a real normal instead of an index into the 256-entry
	 * anorm table, so the shadedots row cannot be looked up -- but the row
	 * IS the quantized entity yaw, which is the only thing that table
	 * varies by, so rebuild the light vector from it and evaluate the same
	 * function the table holds: 1 + d, with d scaled by 0.3 where it faces
	 * away.  That is what r_avertexnormal_dots is, and it is what
	 * md5mesh.c's skeletal shader and md3mesh.c's CPU path both compute --
	 * the three have to agree or one model lights differently depending on
	 * r_alias_gpu and on which format it was shipped in.  The scaffolding
	 * this replaces used max(world_normal.z, 0), a top-down world light
	 * that matched none of them.  uhexen2-2ah9. */
	/* Bit 8 of ShadedotRow is the per-instance dynamic-light switch, not part
	 * of the row -- see ALIAS_INST_SHADEDOT_ROW_MASK in gl_shader.h.  The row
	 * itself is 0..15, so masking costs nothing and keeps the 80-byte
	 * InstanceData layout unchanged.  uhexen2-waum. */
	"    int sdot_row = inst.ShadedotRow & " GLSL_STR(ALIAS_INST_SHADEDOT_ROW_MASK) ";\n"
	"    if (u_poseverttype == 1) {\n"  /* MD3: use decoded normal */
	"        float an = float(sdot_row) * (6.28318531 / 16.0);\n"
	"        vec3 sv = normalize(vec3(cos(-an), sin(-an), 1.0));\n"
	"        float d = dot(normalize(normal), sv);\n"
	"        if (d < 0.0) d *= 0.3;\n"
	"        sdot = max(1.0 + d, 0.0);\n"
	"    } else {\n"  /* QUAKE1: use shadedots table */
	"        int sdot_idx = sdot_row * 256 + int(ni);\n"
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
	"    v_eyepos = (u_lightview * vec4(world_pos, 1.0)).xyz;\n"
	/* Per instance, because one instanced draw mixes entities whose CPU
	 * lighting came from different arms of R_DrawAliasModel's branch chain
	 * and only the ordinary arm ever took dynamic light.  An MLS_ABSLIGHT or
	 * EF_ROTATE entity carries the bit clear and shades exactly as before. */
	"    v_dlightscale = ((inst.ShadedotRow & " GLSL_STR(ALIAS_INST_DLIGHT_BIT) ") != 0)\n"
	"                  ? u_alias_dlight : 0.0;\n"
	"    gl_Position = u_viewproj * vec4(world_pos, 1.0);\n"
	"}\n";
#endif /* !USE_GLES */

/* --- shader_sky: two-layer scrolling sky (solid + alpha) --- */
/* THE SKY PROGRAMS (uhexen2-a5nn.4).
 *
 * One program used to serve every sky mode, branching on u_alpha_threshold:
 * over 0.5 meant "one texture, UVs from the vertex", under it meant "two
 * scrolling layers, UVs from the view direction".  Ironwail compiles four
 * specialised programs instead (Quake/glquake.h:541-544) and we now compile
 * two of them, which is every mode we actually have.
 *
 * THE BRANCH WAS NOT FREE, AND NOT ONLY BECAUSE OF THE BRANCH.
 * u_alpha_threshold is engine-global alpha-test state shared with every other
 * program, so borrowing it as a mode switch meant three call sites had to
 * save it, set it and restore it around their draws.  Each is a documented
 * bug: uhexen2-khsa (leaving it hot at 1.0 turned the next entity's discard
 * into `if (color.a < 1.0)`, which on NVIDIA killed about half the fragments
 * of a fully opaque skin -- the screen-door), uhexen2-sp7v (the skybox path
 * had no save/restore at all) and uhexen2-nudx (a brush entity with a sky
 * face discarding its own translucent faces).  Two programs delete the borrow
 * rather than documenting it a fourth time.
 *
 * WHAT IS DELIBERATELY NOT PORTED.  Upstream carries a [dither] axis on three
 * of its four sky programs; our softemu screen dither lives in the
 * post-process pass instead (see R_SoftEmuParams, uhexen2-a5nn.3), so there is
 * no axis here to compile.  skystencil has no counterpart either -- our sky
 * surfaces reach the depth buffer through the ordinary world chain.
 * skycubemap, including its [anim] variant, is the one real gap and is not
 * here: we have no cubemap sky path at all to hang it on.
 */

/* Two scrolling cloud layers blended in one pass.  Ironwail's skylayers
 * (Quake/gl_shaders.h sky_layers_*).
 *
 * ONE DIVERGENCE FROM UPSTREAM, WHICH FIXES A BUG.  Upstream derives the layer
 * UVs in the fragment shader from the view direction and scrolls them at a
 * hardcoded Time/16 and Time/8, and so did the branch this replaces.  But our
 * callers in gl_warp.c already compute those UVs per vertex -- they have to,
 * because the two-pass path below needs them -- and they compute them from
 * r_skyspeed_back and r_skyspeed_front.  The shader ignoring them meant those
 * two archived cvars silently did nothing whenever r_skyalpha reached 1.0, and
 * worked at every other value.  Taking the vertex UVs makes them live on both
 * paths and deletes the duplicated direction maths.
 *
 * The two agreed at stock settings, which is why it went unnoticed: the CPU
 * normalises the direction to length 6*63 and divides by 128, giving the same
 * 189/64 scale the shader used, and r_skyspeed_back 8 / r_skyspeed_front 16
 * over 128 are exactly the /16 and /8 it hardcoded.
 *
 * It also settles which clock the sky runs on.  The vertex UVs use realtime,
 * as QuakeSpasm's sky always has; the branch this replaces used u_time
 * (cl.time), so the sky used to stop scrolling on pause at r_skyalpha 1.0 and
 * keep scrolling at every other value.  Now it is realtime throughout. */
static const char ssky_layers_vert[] =
	GLSL_VERT_HEADER
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

static const char ssky_layers_frag[] =
	GLSL_FRAG_HEADER
	"uniform sampler2D u_texture0;\n"	/* solid / back layer */
	"uniform sampler2D u_texture1;\n"	/* alpha / front layer */
	"uniform vec4 u_skyfog;\n"
	"uniform vec2 u_wind;\n"
	"in vec2 v_texcoord;\n"
	"in vec2 v_lmcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 solid = texture(u_texture0, v_texcoord + u_wind);\n"
	"    vec4 layer = texture(u_texture1, v_lmcoord + u_wind);\n"
	"    vec3 color = mix(solid.rgb, layer.rgb, layer.a);\n"
	"    color = mix(color, u_skyfog.rgb, u_skyfog.a);\n"
	"    fragColor = vec4(color, 1.0) * v_color;\n"
	"}\n";

/* The skybox as a cubemap, sampled by view direction.  Ironwail's skycubemap
 * (Quake/gl_shaders.h sky_cubemap_*).  uhexen2-ctk9.
 *
 * WHY THIS EXISTS WHEN skyboxside ALREADY DRAWS SKYBOXES.  It is not about the
 * six draws; it is about the wind.  r_skywind animates a skybox by sliding a
 * UV offset across six independent 2D faces (uhexen2-typa), and a 2D slide
 * cannot be continuous across a cube edge -- the offset that is right on +X is
 * not the same offset on +Y, so the seams pull apart as soon as the wind is
 * anything but zero.  Rotating a direction vector has no seams to pull apart.
 *
 * The axis swizzle is upstream's, and it is not arbitrary: our st_to_vec in
 * gl_sky.c puts rt/lf on world +X/-X, bk/ft on +Y/-Y and up/dn on +Z/-Z, which
 * is exactly the convention Ironwail's cubemap_order was written against, so
 * the two agree face for face.
 *
 * ONE DIVERGENCE FROM UPSTREAM, ON PURPOSE.  Ironwail's ANIM arm samples the
 * cubemap twice at two phases and cross-fades them, weighting by the faces'
 * alpha channel over an opaque base.  That is the right shape for a wind that
 * drifts one way forever and has to hide a wrap.  Ours does not: Sky_UpdateWind
 * runs a triangle wave that reverses smoothly and never wraps, so the crossfade
 * would be hiding a discontinuity that is not there, and the alpha weighting
 * assumes cloud-layer skyboxes we do not ship.  A single rotated sample is the
 * exact 3D analogue of the 2D slide it replaces, which is what keeps this a
 * seam fix rather than a change of look. */
static const char ssky_cube_vert[] =
	GLSL_VERT_HEADER
	"in vec3 a_position;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform vec3 u_eyepos;\n"
	"out vec3 v_dir;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    vec3 d = a_position - u_eyepos;\n"
	"    v_dir = vec3(-d.y, d.z, d.x);\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char ssky_cube_frag[] =
	GLSL_FRAG_HEADER
	"uniform samplerCube u_texture0;\n"
	"uniform vec3 u_wind3;\n"
	"uniform float u_skyrot;\n"
	"in vec3 v_dir;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    vec3 dir = normalize(v_dir);\n"
	/* r_skybox_speed is a horizontal scroll.  On six 2D faces that is a UV
	 * slide per face; on a cubemap the honest equivalent is a yaw rotation of
	 * the sample direction, which is also the one that does not tear at the
	 * face edges.  World +Z is v_dir.y after the vertex swizzle, so yaw lives
	 * in the (x, z) plane. */
	"    float cs = cos(u_skyrot), sn = sin(u_skyrot);\n"
	"    dir.xz = vec2(dir.x * cs - dir.z * sn, dir.x * sn + dir.z * cs);\n"
	"    fragColor = texture(u_texture0, dir + u_wind3) * v_color;\n"
	"}\n";

/* One sky texture, UVs supplied by the caller.  Ironwail's skyboxside
 * (Quake/gl_shaders.h sky_boxside_*), and the six faces of a loaded skybox are
 * what it draws there.
 *
 * Ours additionally serves the two-pass cloud path, which upstream has no
 * equivalent of: at r_skyalpha < 1 -- which is our shipped default of 0.67 --
 * gl_warp.c draws the back layer opaque and the front layer blended, and each
 * pass is exactly this, one texture with vertex UVs modulated by v_color.
 *
 * No u_skyfog here on purpose.  The skybox path fogs with a second flat pass
 * over the same quad (Sky_DrawSkyBox), not in this shader, and the cloud
 * passes take their fog the same way the surface did before them. */
static const char ssky_side_vert[] =
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

static const char ssky_side_frag[] =
	GLSL_FRAG_HEADER
	"uniform sampler2D u_texture0;\n"
	"uniform vec2 u_wind;\n"
	"in vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = texture(u_texture0, v_texcoord + u_wind) * v_color;\n"
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
	R_UseProgram (p->program);
	if (p->u_texture0 >= 0) glUniform1i_fp(p->u_texture0, 0);
	if (p->u_texture1 >= 0) glUniform1i_fp(p->u_texture1, 1);
	if (p->u_texture2 >= 0) glUniform1i_fp(p->u_texture2, 2);
		if (p->u_texture3 >= 0) glUniform1i_fp(p->u_texture3, 3);	/* uhexen2-mfql */
		if (p->u_texture4 >= 0) glUniform1i_fp(p->u_texture4, 4);
	if (p->u_lightgrid >= 0) glUniform1i_fp(p->u_lightgrid, LIGHT_GRID_TMU);	/* uhexen2-26bm */
	if (p->u_alpha_threshold >= 0) glUniform1f_fp(p->u_alpha_threshold, 0.0f);
	if (p->u_fog_density >= 0) glUniform1f_fp(p->u_fog_density, 0.0f);
	/* uhexen2-mf9u.  Unit 1 is free on every program that declares this —
	 * gl_shader_alias has no u_texture1 — so the depth snapshot can sit
	 * there permanently and only the sprite path ever binds to it. */
	if (p->u_soft_depth >= 0) glUniform1i_fp(p->u_soft_depth, 1);
	/* GL zero-initialises mat4 uniforms, which would collapse every vertex
	 * to world (0,0).  Identity keeps v_worldxy meaningful for the batches
	 * that submit world-space positions directly.  uhexen2-0gn3. */
	if (p->u_alias_model >= 0)
	{
		float ident[16];
		Mat4_Identity(ident);
		glUniformMatrix4fv_fp(p->u_alias_model, 1, GL_FALSE, ident);
	}
	R_UseProgram (0);

	Con_SafePrintf("  shader '%s' loaded (program %u)\n", name, p->program);
	return true;
}

/*
===============
GL_InitProgramDefines

GL_InitProgram with a preamble spliced into both stages, so one source can be
compiled as more than one program.  Only the alias NOPERSP variants use it;
it delegates rather than duplicating GL_InitProgram's uniform and sampler
setup, which the variants must share exactly.  uhexen2-ktjv.

Desktop-only, with the same guard as GL_SpliceDefines that it calls and as the
two call sites in GL_Shaders_Init: `noperspective` is not in the GLSL ES 3.00
language at all, so the ES tier compiles no NOPERSP variant to need this.
Without the guard the ES tier failed to build outright -- the definition was
visible there while its callee was not.
===============
*/
#ifndef USE_GLES
static void GL_InitProgramDefines (glprogram_t *p, const char *name,
				   const char *vert_src, const char *frag_src,
				   const char *defines)
{
	char	*v = GL_SpliceDefines (vert_src, defines);
	char	*f = GL_SpliceDefines (frag_src, defines);

	if (v && f)
		GL_InitProgram (p, name, v, f);
	else
		Con_SafePrintf ("  %s: FAILED (preamble splice)\n", name);
	free (v);
	free (f);
}
#endif	/* !USE_GLES */

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

	/* Look up extra uniforms */
	p->u_pup    = glGetUniformLocation_fp(p->base.program, "u_pup");
	p->u_pright = glGetUniformLocation_fp(p->base.program, "u_pright");
	p->u_vpn    = glGetUniformLocation_fp(p->base.program, "u_vpn");
	p->u_origin = glGetUniformLocation_fp(p->base.program, "u_origin");
	p->u_ctime  = glGetUniformLocation_fp(p->base.program, "u_ctime");

	/* Bind texture unit defaults */
	R_UseProgram (p->base.program);
	if (p->base.u_texture0 >= 0) glUniform1i_fp(p->base.u_texture0, 0);
	if (p->base.u_fog_density >= 0) glUniform1f_fp(p->base.u_fog_density, 0.0f);
	R_UseProgram (0);

	Con_SafePrintf("  shader 'particle_gpu' loaded (program %u)\n", p->base.program);
	return true;
}
#endif /* !USE_GLES */

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
gl_alias_inst_prog_t gl_shader_alias_inst_np;	/* affine twin, uhexen2-ktjv */

/* Shadedots table — defined in gl_rmain.c */
extern float r_avertexnormal_dots[16][256];

#ifndef USE_GLES
static qboolean GL_InitAliasInstProgram (gl_alias_inst_prog_t *p, const char *name,
					 const char *defines)
{
	GLuint vs, fs, prog;

	/* `defines` is NULL for the ordinary program and "#define NOPERSP 1" for
	 * the affine twin -- one source, two programs.  uhexen2-ktjv. */
	vs = defines ? GL_CompileShaderDefines(GL_VERTEX_SHADER, salias_inst_vert, defines)
		     : GL_CompileShader(GL_VERTEX_SHADER, salias_inst_vert);
	fs = defines ? GL_CompileShaderDefines(GL_FRAGMENT_SHADER, salias_frag, defines)
		     : GL_CompileShader(GL_FRAGMENT_SHADER, salias_frag);
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
			Sys_Printf("%s link failed: %s\n", name, log);
			glDeleteProgram_fp(prog);
			return false;
		}
	}

	memset(p, 0, sizeof(*p));
	p->program = prog;
	p->u_fog_density = glGetUniformLocation_fp(prog, "u_fog_density");
	p->u_fog_color = glGetUniformLocation_fp(prog, "u_fog_color");
	p->u_alpha_threshold = glGetUniformLocation_fp(prog, "u_alpha_threshold");
	p->u_viewproj = glGetUniformLocation_fp(prog, "u_viewproj");
	p->u_inst_base = glGetUniformLocation_fp(prog, "u_inst_base");
	p->u_eyepos = glGetUniformLocation_fp(prog, "u_eyepos");
	p->u_poseverttype = glGetUniformLocation_fp(prog, "u_poseverttype");
	p->u_force_opaque_alpha = glGetUniformLocation_fp(prog, "u_force_opaque_alpha");
	p->u_alias_caustics = glGetUniformLocation_fp(prog, "u_alias_caustics");
	/* uhexen2-waum */
	p->u_alias_dlight = glGetUniformLocation_fp(prog, "u_alias_dlight");
	p->u_lightview = glGetUniformLocation_fp(prog, "u_lightview");
	p->u_lightgrid_xy = glGetUniformLocation_fp(prog, "u_lightgrid_xy");
	p->u_lightgrid_z = glGetUniformLocation_fp(prog, "u_lightgrid_z");
	p->u_lightgrid = glGetUniformLocation_fp(prog, "u_lightgrid");

	/* Bind skin texture to unit 0 */
	R_UseProgram (prog);
	{
		GLint u_tex = glGetUniformLocation_fp(prog, "u_texture0");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 0);
	}
	/* The froxel grid lives on its own unit for the whole world+entity draw;
	 * GL_InitProgram does this for every other program that declares it. */
	if (p->u_lightgrid >= 0)
		glUniform1i_fp(p->u_lightgrid, LIGHT_GRID_TMU);	/* uhexen2-waum */
	R_UseProgram (0);

	/* Create and fill shadedots SSBO (static data, upload once).
	 * Uses binding=2 to match the non-instanced GPU alias shader. */
	glGenBuffers_fp(1, &p->ubo_shadedots);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, p->ubo_shadedots);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER, 16 * 256 * sizeof(float),
			r_avertexnormal_dots, GL_STATIC_DRAW);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);

	Sys_Printf("  %s: OK (prog=%u, ubo=%u)\n", name, prog, p->ubo_shadedots);
	return true;
}
#endif /* !USE_GLES */

void GL_AliasInst_Init (void)
{
#ifndef USE_GLES
	GL_InitAliasInstProgram(&gl_shader_alias_inst_np, "alias_instanced_np", "#define NOPERSP 1\n");
	if (!GL_InitAliasInstProgram(&gl_shader_alias_inst, "alias_instanced", NULL))
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
	if (gl_shader_alias_inst_np.program)
		glDeleteProgram_fp(gl_shader_alias_inst_np.program);
	if (gl_shader_alias_inst_np.ubo_shadedots)
		glDeleteBuffers_fp(1, &gl_shader_alias_inst_np.ubo_shadedots);
	memset(&gl_shader_alias_inst_np, 0, sizeof(gl_shader_alias_inst_np));
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
	/* Affine-mapping twin (uhexen2-ktjv).  Same sources, NOPERSP defined:
	 * an interpolation qualifier cannot be switched by a uniform, so this
	 * is the one r_softemu stage that costs a program.  Compiled whether or
	 * not r_softemu is on -- building it lazily would put a shader compile
	 * inside a frame the first time someone flipped the cvar.
	 *
	 * DESKTOP ONLY.  GLSL ES 3.00 has `smooth` and `flat` and nothing else:
	 * `noperspective` is a reserved word there, not a qualifier, so this
	 * would be a guaranteed compile failure logged on every WebGL2 startup.
	 * R_AliasProgram falls back to the perspective-correct program when the
	 * twin has no program, so the ES tier simply does not get the effect --
	 * which is the same answer the ES tier already gives for skeletal
	 * models (uhexen2-dfay). */
	GL_InitProgramDefines(&gl_shader_alias_np, "alias_np", salias_vert, salias_frag,
			      "#define NOPERSP 1\n");
#endif
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
	GL_InitProgramDefines(&gl_shader_skeletal_np, "skeletal_np", sskeletal_vert, salias_frag,
			      "#define NOPERSP 1\n");
#endif
	GL_InitProgram(&gl_shader_particle, "particle", spart_vert,  spart_frag);
	GL_InitProgram(&gl_shader_sky_layers,  "sky_layers",  ssky_layers_vert, ssky_layers_frag);
	GL_InitProgram(&gl_shader_sky_boxside, "sky_boxside", ssky_side_vert,   ssky_side_frag);
	GL_InitProgram(&gl_shader_sky_cubemap, "sky_cubemap", ssky_cube_vert,  ssky_cube_frag);

	/* Create the 1x1 black sentinel texture used as u_texture2 in
	 * gl_shader_world for surfaces with no fullbright pixels.  Sampled
	 * unconditionally; black contributes 0 to the additive sum.
	 * uhexen2-sjvf. */
	{
		static const unsigned char black_pixel[4] = {0, 0, 0, 255};
		glGenTextures_fp(1, &gl_null_fb_texture);
		glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, black_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	/* Material-map sentinels, bound at units 3 and 4 for any surface whose
	 * pack shipped no _norm/_bump/_gloss.  Both fetches are unconditional
	 * inside the material branch, so the sentinels have to be the identity
	 * for that maths: (128,128,255) decodes to (0,0,1), a normal pointing
	 * straight out of the surface, which yields exactly the flat response
	 * the shader divides by and therefore no change; black gloss multiplies
	 * the highlight to nothing.  This is the same shape as gl_null_fb_texture
	 * above, and for the same reason -- one program, no per-surface variant.
	 * uhexen2-mfql. */
	{
		static const unsigned char flat_normal_pixel[4] = {128, 128, 255, 255};
		static const unsigned char black_pixel2[4] = {0, 0, 0, 255};

		glGenTextures_fp(1, &gl_flat_normal_texture);
		glBindTexture_fp(GL_TEXTURE_2D, gl_flat_normal_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, flat_normal_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glGenTextures_fp(1, &gl_null_gloss_texture);
		glBindTexture_fp(GL_TEXTURE_2D, gl_null_gloss_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, black_pixel2);
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
		glBindTexture_fp(GL_TEXTURE_2D, gl_solid_white_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		/* Both blocks above bound straight through glBindTexture_fp, behind
		 * GL_Bind's back.  Tell the cache what is actually bound now, or the
		 * next GL_Bind of a texture that happens to match the stale value
		 * would skip a bind it genuinely needs. */
		currenttexture = gl_solid_white_texture;
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
	{
		char *v = GL_SpliceDefines (salias_vert, "#define NOPERSP 1\n");
		char *f = GL_SpliceDefines (salias_frag, "#define NOPERSP 1\n");
		if (v && f)
			GL_InitOITProgram(&gl_shader_alias_np_oit, "alias_np", v, f);
		free (v);
		free (f);
	}
	GL_InitOITProgram(&gl_shader_skeletal_oit, "skeletal", sskeletal_vert, salias_frag);
	{
		char *v = GL_SpliceDefines (sskeletal_vert, "#define NOPERSP 1\n");
		char *f = GL_SpliceDefines (salias_frag, "#define NOPERSP 1\n");
		if (v && f)
			GL_InitOITProgram(&gl_shader_skeletal_np_oit, "skeletal_np", v, f);
		free (v);
		free (f);
	}
	GL_InitOITProgram(&gl_shader_particle_oit, "particle", spart_vert,  spart_frag);
#endif

#ifndef USE_GLES
	GL_InitParticleGPUProgram(&gl_shader_particle_gpu);
#endif
	GL_AliasInst_Init();
}

/* Backs `renderer_status`.  A program that failed to link is a zero here, and
 * on the ES tier several are deliberately never built at all -- so "disabled"
 * and "FAILED" are different answers.  Ported from alextnewman/hexenwail
 * d2c46f078. */
void GL_ReportShaderStatus (void)
{
	Con_Printf("[RENDERER] shaders: 2d=%s flat=%s world=%s world_opaque=%s "
		   "alias=%s particle=%s sky_layers=%s sky_boxside=%s sky_cubemap=%s "
		   "skeletal=%s OIT=%s/%s/%s\n",
		   gl_shader_2d.program ? "ok" : "FAILED",
		   gl_shader_flat.program ? "ok" : "FAILED",
		   gl_shader_world.program ? "ok" : "FAILED",
		   gl_shader_world_opaque.program ? "ok" : "FAILED",
		   gl_shader_alias.program ? "ok" : "FAILED",
		   gl_shader_particle.program ? "ok" : "FAILED",
		   gl_shader_sky_layers.program ? "ok" : "FAILED",
		   gl_shader_sky_boxside.program ? "ok" : "FAILED",
		   gl_shader_sky_cubemap.program ? "ok" : "FAILED",
		   gl_shader_skeletal.program ? "ok" : "disabled",
		   gl_shader_world_oit.program ? "ok" : "disabled",
		   gl_shader_alias_oit.program ? "ok" : "disabled",
		   gl_shader_particle_oit.program ? "ok" : "disabled");
}

void GL_Shaders_Shutdown (void)
{
	glprogram_t *progs[] = {
		&gl_shader_2d, &gl_shader_flat, &gl_shader_world,
		&gl_shader_world_opaque,
		&gl_shader_alias, &gl_shader_skeletal, &gl_shader_particle,
		&gl_shader_sky_layers, &gl_shader_sky_boxside, &gl_shader_sky_cubemap,
		&gl_shader_particle_gpu.base,
		&gl_shader_alias_np, &gl_shader_skeletal_np,
		&gl_shader_world_oit, &gl_shader_alias_oit, &gl_shader_skeletal_oit,
		&gl_shader_alias_np_oit, &gl_shader_skeletal_np_oit,
		&gl_shader_particle_oit
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

	/* The two 1x1 sentinels GL_Shaders_Init creates are regenerated on the
	 * next init, so leaving them bound here leaked one pair per vid_restart.
	 * d2c46f078. */
	if (gl_null_fb_texture)
	{
		glDeleteTextures_fp(1, &gl_null_fb_texture);
		gl_null_fb_texture = 0;
	}
	if (gl_flat_normal_texture)
	{
		glDeleteTextures_fp(1, &gl_flat_normal_texture);
		gl_flat_normal_texture = 0;
	}
	if (gl_null_gloss_texture)
	{
		glDeleteTextures_fp(1, &gl_null_gloss_texture);
		gl_null_gloss_texture = 0;
	}
	if (gl_solid_white_texture)
	{
		glDeleteTextures_fp(1, &gl_solid_white_texture);
		gl_solid_white_texture = 0;
	}
	currenttexture = GL_UNUSED_TEXTURE;
}
