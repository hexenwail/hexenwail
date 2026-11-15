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
#include "gl_uniforms.h"	/* the two shared std140 blocks (uhexen2-p4ln) */

/* Generated from the .glsl files in engine/shaders by EmbedShaders.cmake.
 * Embedded rather than loaded at runtime: the engine has no shader search
 * path, so a missing file would be an unrecoverable black screen. */
#include "shaders_gen.h"

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
static GLuint GL_LoadProgramSplit (const char *vert_header, const char *vert_src,
				   const char *frag_header, const char *frag_src);

/* ------------------------------------------------------------------ */
/* Shader compilation helpers                                          */
/* ------------------------------------------------------------------ */

/* `header` carries the #version line and, on the ES tier, the default
 * precision declarations that must precede it; NULL for a source that still
 * spells its own.  Kept a separate glShaderSource chunk rather than spliced
 * because the files in engine/shaders deliberately have no #version: one file
 * has to serve the desktop tier, the ES tier and the offline SPIR-V compile,
 * and those want three different ones. */
static GLuint GL_CompileShaderSplit (GLenum type, const char *header,
				     const char *source)
{
	GLuint shader;
	GLint status;
	char log[2048];
	const char *type_name = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
	const char *chunks[2];
	GLsizei nchunks = 0;

	if (header)
		chunks[nchunks++] = header;
	chunks[nchunks++] = source;

	shader = glCreateShader_fp(type);

	glShaderSource_fp(shader, nchunks, chunks, NULL);

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
	GLsizei		nsources = 0;
	GLuint		shader, prog;
	GLint		status;
	char		log[2048];

	/* header may be NULL for a body that still carries its own #version
	 * (gl_worldcull.c's inline cull shaders). */
	if (header)
		sources[nsources++] = header;
	sources[nsources++] = body;

	shader = glCreateShader_fp(GL_COMPUTE_SHADER);
	glShaderSource_fp(shader, nsources, sources, NULL);
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

	if (developer.integer >= 2)
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

/* The OIT program builder below is desktop-only: OIT cannot run on the ES
 * tier at all (no glBlendFunci in WebGL2), so nothing there would call it.
 * See GL_Shaders_Init and uhexen2-ha7n. */
#ifndef USE_GLES
/*
===============
GL_InitOITProgramSplit

The OIT twin of a program: the same two stages, the fragment one compiled with
"#define OIT 1" after its header chunk.  That swaps the stage's colour output
for engine/shaders/oit.inc -- the accum/revealage outputs, the moment maths and
the main() that wraps the stage's own -- so the twin is a variant of the file,
not a splice of it.  (It was a splice, of gl_shader.c's oit_preamble, until
#450 W3.)  Fails the program rather than build one that cannot be told which
OIT output to write: a twin without OitFrag would write WBOIT output into the
moment pass.
===============
*/
static void GL_InitOITProgramSplit (glprogram_t *p, const char *name,
				    const char *vert_header, const char *vert_src,
				    const char *frag_header, const char *frag_src)
{
	char	oit_header[1024];
	int	n;

	n = q_snprintf (oit_header, sizeof(oit_header), "%s%s", frag_header, "#define OIT 1\n");
	p->program = 0;
	if (n > 0 && n < (int)sizeof(oit_header))
		p->program = GL_LoadProgramSplit (vert_header, vert_src, oit_header, frag_src);
	if (p->program)
	{
		GL_InitProgramUniforms (p);
		if (p->ub_ofrag < 0)
		{
			Con_Printf ("[SHADER] %s_oit declares no OitFrag block\n", name);
			glDeleteProgram_fp (p->program);
			p->program = 0;
		}
	}
	if (p->program)
	{
		/* bind texture unit defaults (mirrors GL_InitProgramSplit) */
		R_UseProgram (p->program);
		if (p->u_texture0 >= 0) glUniform1i_fp(p->u_texture0, 0);
		if (p->u_texture1 >= 0) glUniform1i_fp(p->u_texture1, 1);
		if (p->u_texture2 >= 0) glUniform1i_fp(p->u_texture2, 2);
		if (p->u_texture3 >= 0) glUniform1i_fp(p->u_texture3, 3);	/* uhexen2-mfql */
		if (p->u_texture4 >= 0) glUniform1i_fp(p->u_texture4, 4);
		if (p->u_lightgrid >= 0) glUniform1i_fp(p->u_lightgrid, LIGHT_GRID_TMU);	/* uhexen2-26bm */
		if (p->u_lmlayer1 >= 0) glUniform1i_fp(p->u_lmlayer1, LMSTYLE_TMU);	/* #439 */
		if (p->u_lmlayer2 >= 0) glUniform1i_fp(p->u_lmlayer2, LMSTYLE_TMU + 1);
		if (p->u_lmlayer3 >= 0) glUniform1i_fp(p->u_lmlayer3, LMSTYLE_TMU + 2);
		if (p->u_soft_depth >= 0) glUniform1i_fp(p->u_soft_depth, 1);	/* uhexen2-mf9u */
		/* MBOIT moment targets (#87): their own units, the 2D and the
		 * multisample pair apart, so the two sampler types never share a
		 * unit -- that is an error at draw time even when one is unused */
		if (p->u_mboit_b0 >= 0)     glUniform1i_fp(p->u_mboit_b0, MBOIT_TMU_B0);
		if (p->u_mboit_mom >= 0)    glUniform1i_fp(p->u_mboit_mom, MBOIT_TMU_MOM);
		if (p->u_mboit_b0_ms >= 0)  glUniform1i_fp(p->u_mboit_b0_ms, MBOIT_TMU_B0_MS);
		if (p->u_mboit_mom_ms >= 0) glUniform1i_fp(p->u_mboit_mom_ms, MBOIT_TMU_MOM_MS);
		R_UseProgram (0);
		Con_SafePrintf ("  %s_oit: OK (prog=%u)\n", name, p->program);
	}
	else
	{
		Con_SafePrintf ("  %s_oit: FAILED\n", name);
	}
}
#endif	/* !USE_GLES */


/* Compile and link a program where each stage is a (header, body) pair; see
 * GL_CompileShaderSplit for why the header is kept separate. */
static GLuint GL_LoadProgramSplit (const char *vert_header, const char *vert_src,
				   const char *frag_header, const char *frag_src)
{
	GLuint vs, fs, prog;

	vs = GL_CompileShaderSplit(GL_VERTEX_SHADER, vert_header, vert_src);
	if (!vs) return 0;
	fs = GL_CompileShaderSplit(GL_FRAGMENT_SHADER, frag_header, frag_src);
	if (!fs) { glDeleteShader_fp(vs); return 0; }

	prog = GL_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	return prog;
}

/* A program whose stages carry no #version of their own (engine/shaders):
 * each gets its header as a separate source chunk.  gl_ssao.c. */
GLuint GL_LoadProgramHeaders (const char *vert_header, const char *vert_src,
			      const char *frag_header, const char *frag_src)
{
	return GL_LoadProgramSplit (vert_header, vert_src, frag_header, frag_src);
}

/* ------------------------------------------------------------------ */
/* Helper to look up all common uniforms                               */
/* ------------------------------------------------------------------ */

static void GL_InitProgramUniforms (glprogram_t *p)
{
	p->u_texture0        = glGetUniformLocation_fp(p->program, "u_texture0");
	p->u_texture1        = glGetUniformLocation_fp(p->program, "u_texture1");
	p->u_texture2        = glGetUniformLocation_fp(p->program, "u_texture2");
	p->u_texture3        = glGetUniformLocation_fp(p->program, "u_texture3");
	p->u_texture4        = glGetUniformLocation_fp(p->program, "u_texture4");
	p->u_lmlayer1        = glGetUniformLocation_fp(p->program, "u_lmlayer1");
	p->u_lmlayer2        = glGetUniformLocation_fp(p->program, "u_lmlayer2");
	p->u_lmlayer3        = glGetUniformLocation_fp(p->program, "u_lmlayer3");
	p->u_mboit_b0        = glGetUniformLocation_fp(p->program, "u_mboit_b0");	/* #87 */
	p->u_mboit_mom       = glGetUniformLocation_fp(p->program, "u_mboit_mom");
	p->u_mboit_b0_ms     = glGetUniformLocation_fp(p->program, "u_mboit_b0_ms");
	p->u_mboit_mom_ms    = glGetUniformLocation_fp(p->program, "u_mboit_mom_ms");
	/* Clustered dynamic lighting (uhexen2-26bm); -1 on every non-world
	 * program and on the whole ES tier. */
	p->u_lightgrid       = glGetUniformLocation_fp(p->program, "u_lightgrid");
	p->u_soft_depth       = glGetUniformLocation_fp(p->program, "u_soft_depth");

	/* The two shared uniform blocks, for the shaders that have moved to
	 * engine/shaders/.  Looked up on every program the same way the
	 * samplers above are, and -1 on the ones that declare no such block.
	 * These two indices are what GL_ImmEnd keys its block path on. */
	R_BindProgramBlocks(p->program, &p->ub_vert, &p->ub_frag);
	/* And the alias family's own (#450): >= 0 on exactly the programs that
	 * link salias_frag, which is what GL_ImmEnd and R_SetSpanCell key their
	 * AliasFrag writes on. */
	p->ub_afrag = R_BindAliasFragBlock(p->program);
	/* AliasVert: present on the four skeletal programs, whose vertex stage
	 * reads its lighting from it (#450 S4).  Only the binding matters -- no
	 * draw keys on the index, so it is not kept.  The instanced pair are
	 * linked by GL_InitAliasInstProgram and bind it there. */
	R_BindAliasVertBlock(p->program);
	/* WorldFrag: >= 0 on exactly the three world programs (#450 W2), which is
	 * what GL_ImmPushBlocks and R_SetSpanCell key their world writes on. */
	p->ub_wfrag = R_BindWorldFragBlock(p->program);
	/* OitFrag: >= 0 on exactly the six OIT twins (#450 W3), which
	 * GL_InitOITProgramSplit requires of each. */
	p->ub_ofrag = R_BindOITFragBlock(p->program);
}

/* ------------------------------------------------------------------ */
/* Shader sources                                                      */
/* ------------------------------------------------------------------ */

/* GLSL version header: desktop GL 4.3 vs WebGL2 (ES 3.0) */
#ifdef USE_GLES
#define GLSL_VERT_HEADER	"#version 300 es\nprecision highp float;\n"
#define GLSL_FRAG_HEADER	"#version 300 es\nprecision highp float;\n"
#else
#define GLSL_VERT_HEADER	"#version 430 core\n"
#define GLSL_FRAG_HEADER	"#version 430 core\n"
#endif
/* The tier switches the world fragment stages used to splice in -- no
 * early_fragment_tests below ES 3.10, the bitfieldReverse fallback ES 3.00
 * needs -- are #ifdef GL_ES in those files now (#450 W2): early-Z in
 * sworld_opaque_frag.glsl, with the note on why the cutout twin must never
 * have it (uhexen2-238u); the fallback in softemu_noise.inc. */

/* --- shader_2d: orthographic HUD/text rendering ---
 * Lives in engine/shaders/s2d_vert.glsl and s2d_frag.glsl, and reaches us as
 * s2d_vert[] / s2d_frag[] via shaders_gen.h.  Their #version headers are
 * supplied separately, as a second glShaderSource chunk, so the files stay
 * tier-agnostic; GL_InitProgramSplit below is what pairs them up.
 * u_mvp and u_alpha_threshold sit inside uniform blocks there rather than
 * being loose uniforms, which is the one thing Vulkan GLSL will not have. */

/* --- shader_flat: untextured, vertex-colored (dlights, blendpoly) ---
 * Lives in engine/shaders/sflat_vert.glsl and sflat_frag.glsl, and reaches us
 * as sflat_vert[] / sflat_frag[] via shaders_gen.h.  Same arrangement as s2d
 * above: u_mvp is a member of the shared VertParams block rather than a loose
 * uniform, so gl_shader_flat.u_mvp is now -1 -- and the two paths that used to
 * upload it by hand (the sky stencil pass in gl_rsurf.c and r_showtris in
 * gl_rmain.c) go through R_SetMVP + R_FlushUniforms instead. */

/* --- shader_world: textured + lightmap multitexture, with fog ---
 * All three stages are files: engine/shaders/sworld_vert.glsl (#450, world
 * slice W1), sworld_frag.glsl and sworld_opaque_frag.glsl (slice W2), reaching
 * us as sworld_vert[] / sworld_frag[] / sworld_opaque_frag[] via
 * shaders_gen.h.  Their header comments carry the notes that were here.  The
 * vertex stage reads VertParams; the fragment stages read FragParams and
 * WorldFrag, so every world draw path writes the whole fragment set with
 * R_SetWorldFrag and flushes before it draws -- see r_worldfrag_t in
 * gl_uniforms.h.  The helpers the fragment literals used to splice are .inc
 * files beside them: bicubic_lm, caustics, turb_uv, span_uv, softemu_noise,
 * material, lightgrid, world_dlight and lmstyles. */

/* The froxel-grid sizes for the file-backed fragment stages that read the
 * light cluster (alias and world), as #defines in their header chunk, because
 * the files cannot see these C constants and MAX_LIGHTSTYLES differs per
 * game.  lightgrid.inc's #ifndef defaults are only for the offline SPIR-V
 * compile.  Desktop only: clustered lighting and GPU lightstyles need an SSBO
 * and a compute pass, and the ES/WebGL2 tier has neither.  #450. */
#ifndef USE_GLES
#define GLSL_LIGHTGRID_CONSTS \
	"#define LIGHT_TILES_X " GLSL_STR(LIGHT_TILES_X) "\n" \
	"#define LIGHT_TILES_Y " GLSL_STR(LIGHT_TILES_Y) "\n" \
	"#define LIGHT_TILES_Z " GLSL_STR(LIGHT_TILES_Z) "\n" \
	"#define MAX_LIGHTSTYLES " GLSL_STR(MAX_LIGHTSTYLES) "\n" \
	"#define LIGHT_SSBO_BINDING " GLSL_STR(LIGHT_SSBO_BINDING) "\n"

#else	/* ES/WebGL2: no SSBO, no findLSB, no compute pass to fill the grid */
#define GLSL_LIGHTGRID_CONSTS	""	/* lightgrid.inc is #ifndef GL_ES */
#endif

/* The header chunk every alias-family program compiles salias_frag.glsl with:
 * the tier's version line, plus the froxel-grid sizes on desktop.  The five
 * affine (NOPERSP) twins, all desktop-only, append "#define NOPERSP 1\n". */
#define GLSL_ALIAS_FRAG_HEADER	GLSL_FRAG_HEADER GLSL_LIGHTGRID_CONSTS

/* The same pair for the three world programs' fragment stages (#450 W2),
 * which include lightgrid.inc and lmstyles.inc on desktop. */
#define GLSL_WORLD_FRAG_HEADER	GLSL_FRAG_HEADER GLSL_LIGHTGRID_CONSTS

/* --- shader_alias: vertex-colored, textured, fog (models) ---
 * Both stages are files now: engine/shaders/salias_vert.glsl (#450, alias
 * slice S1) and engine/shaders/salias_frag.glsl (slice S2), reaching us as
 * salias_vert[] / salias_frag[] via shaders_gen.h.  Their header comments
 * carry the notes that were here.  salias_frag is also the fragment stage of
 * the skeletal and instanced programs below, whose vertex stages are files
 * too (slices S4 and S3); it reads its non-opaque uniforms from FragParams and AliasFrag,
 * so every path drawing with one of the ten writes the members it needs
 * first -- see r_aliasfrag_t in gl_uniforms.h. */

/* --- shader_skeletal: skeletal animation with bone-weighted deformation ---
 * Lives in engine/shaders/sskeletal_vert.glsl since #450 alias slice S4,
 * paired with salias_frag.glsl, and reaches us as sskeletal_vert[] via
 * shaders_gen.h.  Its storage buffer, blocks and varyings are listed in that
 * file's header comment.  Desktop only: the bone matrices arrive in a std430
 * shader storage block, which is GLSL ES 3.10 and absent from WebGL2
 * entirely.  See GL_Shaders_Init and uhexen2-dfay.  Its header chunk is the
 * tier's version line; the affine twins append "#define NOPERSP 1\n", as for
 * the other alias programs. */

/* --- shader_particle: textured triangles with per-vertex color ---
 * Lives in engine/shaders/spart_vert.glsl and spart_frag.glsl, and reaches us
 * as spart_vert[] / spart_frag[] via shaders_gen.h.  Same arrangement as s2d
 * above: u_mvp / u_modelview are members of the shared VertParams block and
 * u_fog_density / u_fog_color are members of FragParams, rather than loose
 * uniforms, so gl_shader_particle.u_mvp and .u_fog_* are now -1 -- and the
 * per-frame uploads go through R_Set* + R_FlushUniforms in GL_ImmPushBlocks
 * instead. */

/* --- shader_particle_gpu: SSBO-driven billboard particles ---
 * Lives in engine/shaders/spart_gpu_vert.glsl, paired with spart_frag.glsl.
 * Desktop-only: shader storage blocks are GLSL ES 3.10, not 3.00.  The
 * u_pup / u_pright / u_vpn / u_origin / u_ctime uniforms are VertParams
 * members now (R_SetParticleBasis); the u_fog_* declarations the old inline
 * body carried are dropped -- they were never read by this stage. */

/* --- shader_alias_instanced: GL 4.3 SSBO-based instanced alias models ---
 * Lives in engine/shaders/salias_inst_vert.glsl since #450 alias slice S3,
 * paired with salias_frag.glsl, and reaches us as salias_inst_vert[] via
 * shaders_gen.h.  Its storage buffers, blocks and varyings are listed in
 * that file's header comment.  Desktop-only: shader storage blocks are GLSL
 * ES 3.10, not 3.00.
 *
 * Its header chunk is the tier's version line plus the two ShadedotRow
 * constants from gl_shader.h, which the file cannot see; its #ifndef copies
 * of them are only for the offline SPIR-V compile.  The affine twin appends
 * "#define NOPERSP 1\n", as for the other alias programs. */
#ifndef USE_GLES
#define GLSL_ALIAS_INST_VERT_HEADER \
	GLSL_VERT_HEADER \
	"#define ALIAS_INST_SHADEDOT_ROW_MASK " GLSL_STR(ALIAS_INST_SHADEDOT_ROW_MASK) "\n" \
	"#define ALIAS_INST_DLIGHT_BIT " GLSL_STR(ALIAS_INST_DLIGHT_BIT) "\n"
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

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                     */
/* ------------------------------------------------------------------ */

/* Each stage is a (header, body) pair; a NULL header is for a source that
 * carries its own #version, which no program built here does since the world
 * fragment stages moved to files (#450 W2).  (GL_InitProgram, the both-NULL
 * wrapper, went with its last caller when sworld_vert moved, #450 W1.) */
static qboolean GL_InitProgramSplit (glprogram_t *p, const char *name,
				     const char *vert_header, const char *vert_src,
				     const char *frag_header, const char *frag_src)
{
	p->program = GL_LoadProgramSplit(vert_header, vert_src,
					 frag_header, frag_src);
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
	if (p->u_lmlayer1 >= 0) glUniform1i_fp(p->u_lmlayer1, LMSTYLE_TMU);	/* #439 */
	if (p->u_lmlayer2 >= 0) glUniform1i_fp(p->u_lmlayer2, LMSTYLE_TMU + 1);
	if (p->u_lmlayer3 >= 0) glUniform1i_fp(p->u_lmlayer3, LMSTYLE_TMU + 2);
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
	p->base.program = GL_LoadProgramSplit(GLSL_VERT_HEADER, spart_gpu_vert, GLSL_FRAG_HEADER, spart_frag);
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

void GL_ParticleGPU_SetUniforms (const gl_particle_gpu_prog_t *prog,
				  const float *pup, const float *pright,
				  const float *vpn, const float *origin,
				  float ctime)
{
	float mvp[16], mv[16];

	/* The GPU particle path has no draw caller yet, so `prog` carries no
	 * live locations -- the setters below write the shared shadow, not
	 * the program.  (void)prog keeps the signature for when the path is
	 * wired; the loose glUniform* pushes this replaced are in
	 * GL_ImmPushBlocks for the plain program. */
	(void)prog;

	GL_GetMVP(mvp);
	GL_GetModelview(mv);

	R_SetMVP (mvp);
	R_SetModelView (mv);
	R_SetFog (r_fog_density, r_fog_color);
	R_SetParticleBasis (pup, pright, vpn, origin, ctime);
	R_FlushUniforms ();
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
	char	vert_header[512];
	char	frag_header[512];

	/* `defines` is NULL for the ordinary program and "#define NOPERSP 1" for
	 * the affine twin -- one source, two programs.  uhexen2-ktjv.  Both
	 * stages are files (#450), so the defines ride each one's header chunk,
	 * as for the other eight alias-family programs. */
	q_snprintf (vert_header, sizeof(vert_header), "%s%s",
		    GLSL_ALIAS_INST_VERT_HEADER, defines ? defines : "");
	vs = GL_CompileShaderSplit(GL_VERTEX_SHADER, vert_header, salias_inst_vert);
	q_snprintf (frag_header, sizeof(frag_header), "%s%s",
		    GLSL_ALIAS_FRAG_HEADER, defines ? defines : "");
	fs = GL_CompileShaderSplit(GL_FRAGMENT_SHADER, frag_header, salias_frag);
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
	p->u_lightgrid = glGetUniformLocation_fp(prog, "u_lightgrid");	/* uhexen2-waum */
	/* This program is linked by hand rather than by GL_InitProgramSplit, so
	 * it binds its blocks by hand too (#450, hazard 9).  Without this its
	 * four blocks would all sit at binding 0 -- Hi-Z's HizParams. */
	R_BindProgramBlocks(prog, &p->ub_vert, &p->ub_frag);
	p->ub_afrag = R_BindAliasFragBlock(prog);
	R_BindAliasVertBlock(prog);

	/* Bind skin texture to unit 0 */
	R_UseProgram (prog);
	{
		GLint u_tex = glGetUniformLocation_fp(prog, "u_texture0");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 0);
		/* Material maps on units 3 and 4, matching the world program and
		 * what GL_InitProgramSplit does for every other user of salias_frag.
		 * Without this both samplers default to unit 0 and every model
		 * would read its own skin as a normal map.  uhexen2-4kcb. */
		u_tex = glGetUniformLocation_fp(prog, "u_texture3");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 3);
		u_tex = glGetUniformLocation_fp(prog, "u_texture4");
		if (u_tex >= 0)
			glUniform1i_fp(u_tex, 4);
	}
	/* The froxel grid lives on its own unit for the whole world+entity draw;
	 * GL_InitProgramSplit does this for every other program that declares it. */
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
		GL_ForgetBuffer(gl_shader_alias_inst.ubo_shadedots);
		glDeleteBuffers_fp(1, &gl_shader_alias_inst.ubo_shadedots);
	memset(&gl_shader_alias_inst, 0, sizeof(gl_shader_alias_inst));
	if (gl_shader_alias_inst_np.program)
		glDeleteProgram_fp(gl_shader_alias_inst_np.program);
	if (gl_shader_alias_inst_np.ubo_shadedots)
		GL_ForgetBuffer(gl_shader_alias_inst_np.ubo_shadedots);
		glDeleteBuffers_fp(1, &gl_shader_alias_inst_np.ubo_shadedots);
	memset(&gl_shader_alias_inst_np, 0, sizeof(gl_shader_alias_inst_np));
}

void GL_Shaders_Init (void)
{
	Con_SafePrintf("Initializing shaders...\n");

	GL_InitProgramSplit(&gl_shader_2d,  "2d",       GLSL_VERT_HEADER, s2d_vert,
							GLSL_FRAG_HEADER, s2d_frag);
	GL_InitProgramSplit(&gl_shader_flat, "flat",    GLSL_VERT_HEADER, sflat_vert,
							GLSL_FRAG_HEADER, sflat_frag);
	/* All three stages are files (#450, world slices W1 and W2). */
	GL_InitProgramSplit(&gl_shader_world, "world", GLSL_VERT_HEADER, sworld_vert,
							GLSL_WORLD_FRAG_HEADER, sworld_frag);
	GL_InitProgramSplit(&gl_shader_world_opaque, "world_opaque", GLSL_VERT_HEADER, sworld_vert,
							GLSL_WORLD_FRAG_HEADER, sworld_opaque_frag);
	/* Both stages are files (#450, alias slices S1 and S2). */
	GL_InitProgramSplit(&gl_shader_alias, "alias", GLSL_VERT_HEADER, salias_vert,
							GLSL_ALIAS_FRAG_HEADER, salias_frag);
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
	/* NOPERSP reaches both file-backed stages through their header chunks. */
	GL_InitProgramSplit(&gl_shader_alias_np, "alias_np",
			    GLSL_VERT_HEADER "#define NOPERSP 1\n", salias_vert,
			    GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\n", salias_frag);
#endif
#ifndef USE_GLES
	/* Skipped on the ES tier: sskeletal_vert reads its bone matrices from a
	 * `layout(std430) buffer`, and shader storage blocks are GLSL ES 3.10,
	 * not 3.00 — WebGL2 has none at all.  Compiling it there only produced a
	 * guaranteed failure on every startup, and left the driver linking a
	 * program built from a shader it had already rejected.  On desktop
	 * GL_DrawAliasSkeletal draws PV_IQM models with it (uhexen2-7ok0.3);
	 * its ES arm is an empty stub, so skeletal models stay unsupported on
	 * that tier until it gets a UBO or texture-buffer skinning path.
	 * uhexen2-dfay. */
	/* Both stages are files (#450, alias slices S4 and S2); NOPERSP reaches
	 * the affine twin's two through their header chunks. */
	GL_InitProgramSplit(&gl_shader_skeletal, "skeletal", GLSL_VERT_HEADER, sskeletal_vert,
			    GLSL_ALIAS_FRAG_HEADER, salias_frag);
	GL_InitProgramSplit(&gl_shader_skeletal_np, "skeletal_np",
			    GLSL_VERT_HEADER "#define NOPERSP 1\n", sskeletal_vert,
			    GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\n", salias_frag);
#endif
	GL_InitProgramSplit(&gl_shader_particle, "particle", GLSL_VERT_HEADER, spart_vert,
							GLSL_FRAG_HEADER, spart_frag);
	GL_InitProgramSplit(&gl_shader_sky_layers,  "sky_layers",  GLSL_VERT_HEADER, ssky_layers_vert,
							GLSL_FRAG_HEADER, ssky_layers_frag);
	GL_InitProgramSplit(&gl_shader_sky_boxside, "sky_boxside", GLSL_VERT_HEADER, ssky_side_vert,
							GLSL_FRAG_HEADER, ssky_side_frag);
	GL_InitProgramSplit(&gl_shader_sky_cubemap, "sky_cubemap", GLSL_VERT_HEADER, ssky_cube_vert,
							GLSL_FRAG_HEADER, ssky_cube_frag);

	/* Create the 1x1 black sentinel texture used as u_texture2 in
	 * gl_shader_world for surfaces with no fullbright pixels.  Sampled
	 * unconditionally; black contributes 0 to the additive sum.
	 * uhexen2-sjvf. */
	{
		static const unsigned char black_pixel[4] = {0, 0, 0, 255};
		glGenTextures_fp(1, &gl_null_fb_texture);
		R_BindTextureSlot (0, gl_null_fb_texture);
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
		R_BindTextureSlot (0, gl_flat_normal_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, flat_normal_pixel);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glGenTextures_fp(1, &gl_null_gloss_texture);
		R_BindTextureSlot (0, gl_null_gloss_texture);
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
		R_BindTextureSlot (0, gl_solid_white_texture);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
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
	GL_InitOITProgramSplit(&gl_shader_world_oit, "world", GLSL_VERT_HEADER, sworld_vert,
							      GLSL_WORLD_FRAG_HEADER, sworld_frag);
	GL_InitOITProgramSplit(&gl_shader_alias_oit, "alias", GLSL_VERT_HEADER, salias_vert,
							      GLSL_ALIAS_FRAG_HEADER, salias_frag);
	GL_InitOITProgramSplit(&gl_shader_alias_np_oit, "alias_np",
			       GLSL_VERT_HEADER "#define NOPERSP 1\n", salias_vert,
			       GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\n", salias_frag);
	GL_InitOITProgramSplit(&gl_shader_skeletal_oit, "skeletal", GLSL_VERT_HEADER, sskeletal_vert,
			       GLSL_ALIAS_FRAG_HEADER, salias_frag);
	GL_InitOITProgramSplit(&gl_shader_skeletal_np_oit, "skeletal_np",
			       GLSL_VERT_HEADER "#define NOPERSP 1\n", sskeletal_vert,
			       GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\n", salias_frag);
	GL_InitOITProgramSplit(&gl_shader_particle_oit, "particle", GLSL_VERT_HEADER, spart_vert,
							GLSL_FRAG_HEADER, spart_frag);
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
		R_DeleteTextures (1, &gl_null_fb_texture);
		gl_null_fb_texture = 0;
	}
	if (gl_flat_normal_texture)
	{
		R_DeleteTextures (1, &gl_flat_normal_texture);
		gl_flat_normal_texture = 0;
	}
	if (gl_null_gloss_texture)
	{
		R_DeleteTextures (1, &gl_null_gloss_texture);
		gl_null_gloss_texture = 0;
	}
	if (gl_solid_white_texture)
	{
		R_DeleteTextures (1, &gl_solid_white_texture);
		gl_solid_white_texture = 0;
	}
	R_ResetTextureBindings ();
}
