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
GLuint	GL_LinkProgram (GLuint vert, GLuint frag);
/* Compute program from a two-part source: `header` carries the #version line
 * and any #defines, `body` the shader itself, so one body can be compiled as
 * several variants without splicing strings.  `name` only labels the log.
 * Desktop GL 4.3 only -- the ES tier has no compute stage; callers gate on
 * gl_renderer_caps.compute_shaders.  uhexen2-h8yy. */
GLuint	GL_LoadComputeProgram (const char *header, const char *body, const char *name);
GLuint	GL_LoadProgramHeaders (const char *vert_header, const char *vert_src,
			       const char *frag_header, const char *frag_src);

/* Shader programs */
typedef struct glprogram_s {
	GLuint	program;
	/* No u_mvp / u_modelview: sworld_vert, the last stage declaring them
	 * loose, reads them from VertParams since #450 W1, so every draw writes
	 * them with R_SetMVP / R_SetModelView and flushes. */
	GLint	u_texture0;
	GLint	u_texture1;
	GLint	u_texture2;	/* fullbright mask sampler for world (uhexen2-sjvf) */
	GLint	u_texture3;	/* normal-map sampler for world (uhexen2-mfql) */
	GLint	u_texture4;	/* gloss-map sampler for world (uhexen2-mfql) */
	GLint	u_lmlayer1, u_lmlayer2, u_lmlayer3;	/* lightstyle layer atlases, units LMSTYLE_TMU.. (#439) */
	/* MBOIT moment-target samplers, OIT programs only (#87); the mode
	 * switches beside them are OitFrag members since #450 W3. */
	GLint	u_mboit_b0, u_mboit_mom, u_mboit_b0_ms, u_mboit_mom_ms;
	GLint	u_lightgrid;	/* usampler3D of per-froxel light bitmasks, texture unit LIGHT_GRID_TMU; world programs, desktop only (uhexen2-26bm) */
	/* The per-draw values of both families -- fog, the alpha pair,
	 * overbright, bicubic, caustics, the debug views, material, warp, span
	 * cell, software emulation, lightstyle mode, the froxel lookup and dlight
	 * scale, and the alias family's own -- have no field here: since #450
	 * every stage that reads them takes them from VertParams / FragParams /
	 * AliasFrag / WorldFrag (GL_ImmPushBlocks, R_SetWorldFrag), and no live
	 * shader declares those names loose.  tools/test_alias_no_loose.py keeps
	 * it that way.  Only samplers keep a location: the OIT mode switches are
	 * OitFrag members since #450 W3 (R_SetOITMode). */
	GLint	u_soft_depth;	    /* alias FS: opaque-scene depth snapshot, texture unit 1 (uhexen2-mf9u) */
	/* Indices of the two SHARED uniform blocks (gl_uniforms.h), for the
	 * shaders that have moved out of C string literals into engine/shaders/.
	 * Those declare their non-opaque uniforms inside a block, because Vulkan
	 * GLSL has no loose ones at all and the SDL_GPU backend consumes SPIR-V.
	 * A block member has no glGetUniformLocation, so whichever u_* fields
	 * above the block absorbed come back -1 and GL_ImmEnd takes the block
	 * path instead -- which is why the dual path is keyed on these two and
	 * never on shader identity.  -1 on every program that still uses loose
	 * uniforms, and on any stage that declares no block at all. */
	GLint	ub_vert;	    /* VertParams, at UBO_BINDING_VERT */
	GLint	ub_frag;	    /* FragParams, at UBO_BINDING_FRAG */
	GLint	ub_afrag;	    /* AliasFrag, at UBO_BINDING_ALIAS_FRAG: the ten programs linking salias_frag (#450) */
	GLint	ub_wfrag;	    /* WorldFrag, at UBO_BINDING_WORLD_FRAG: the three world programs (#450 W2) */
	GLint	ub_ofrag;	    /* OitFrag, at UBO_BINDING_OIT_FRAG: the six OIT twins (#450 W3) */
} glprogram_t;

/* Texture units of the MBOIT moment targets during the composite pass (#87);
 * above everything a translucent draw binds (0-5, LIGHT_GRID_TMU 5). */
/* GPU lightstyle layer atlases (gl_rsurf.c, #439): units 6, 7, 8. */
#define LMSTYLE_TMU		6
#define MBOIT_TMU_B0		12
#define MBOIT_TMU_MOM		13
#define MBOIT_TMU_B0_MS		14
#define MBOIT_TMU_MOM_MS	15

/* Extended program for GPU particle SSBO rendering */
typedef struct {
	glprogram_t base;       /* program, texture location and block indices */
	/* No basis / camera / time locations: spart_gpu_vert reads them from
	 * VertParams (R_SetParticleBasis). */
} gl_particle_gpu_prog_t;

extern glprogram_t	gl_shader_world;	/* textured + lightmap, fog (cutout: has discard) */
extern glprogram_t	gl_shader_world_opaque;	/* opaque variant: early_fragment_tests, no discard (uhexen2-5c6r) */
extern glprogram_t	gl_shader_alias;	/* vertex-colored, textured, fog */
extern glprogram_t	gl_shader_2d;		/* orthographic textured quads */
extern glprogram_t	gl_shader_particle;	/* textured triangles, per-vertex color */
extern glprogram_t	gl_shader_flat;		/* untextured, vertex-colored */
extern glprogram_t	gl_shader_sky_layers;	/* two scrolling cloud layers (uhexen2-a5nn.4) */
extern glprogram_t	gl_shader_sky_boxside;	/* one sky texture, UVs from the vertex */
extern glprogram_t	gl_shader_sky_cubemap;	/* skybox as a cubemap, by direction (uhexen2-ctk9) */
extern glprogram_t	gl_shader_skeletal;	/* PV_IQM bone-weighted skinning (desktop GL 4.3 only) */
/* NOPERSP twins: affine texture mapping for r_softemu_mdl_warp.  Selected per
 * draw by R_SoftEmuMdlWarp; identical to their siblings otherwise.  uhexen2-ktjv. */
extern glprogram_t	gl_shader_alias_np;
extern glprogram_t	gl_shader_skeletal_np;

extern GLuint		gl_null_fb_texture;	/* 1x1 black sentinel for u_texture2 (uhexen2-sjvf) */
extern GLuint		gl_flat_normal_texture;	/* 1x1 (128,128,255) flat normal, sentinel for u_texture3 (uhexen2-mfql) */
extern GLuint		gl_null_gloss_texture;	/* 1x1 black sentinel for u_texture4 (uhexen2-mfql) */
extern GLuint		gl_solid_white_texture;	/* 1x1 opaque white, for untextured imm batches */
extern gl_particle_gpu_prog_t gl_shader_particle_gpu; /* SSBO billboard particles */

/* OIT variants — same shaders but output to MRT accum+revealage */
extern glprogram_t	gl_shader_world_oit;
extern glprogram_t	gl_shader_alias_oit;
extern glprogram_t	gl_shader_skeletal_oit;
extern glprogram_t	gl_shader_alias_np_oit;
extern glprogram_t	gl_shader_skeletal_np_oit;
extern glprogram_t	gl_shader_particle_oit;

/* Instanced alias program (GL 4.3 SSBO — pose + instances in SSBOs) */
typedef struct {
	GLuint	program;	/* shader program handle */
	GLuint	ubo_shadedots;	/* SSBO handle for shadedots table */
	/* Both stages are files since #450 (salias_inst_vert.glsl in slice S3,
	 * salias_frag.glsl in S2) and read every non-opaque value from a block,
	 * so the only location left is the sampler's.  R_DrawAliasInstanced
	 * writes the vertex stage's view-projection, eye position, instance base,
	 * pose vertex type and dlight scale into VertParams, and
	 * R_LightCluster_BindForAlias its world -> eye matrix into AliasVert. */
	GLint	u_lightgrid;	/* usampler3D, texture unit LIGHT_GRID_TMU */
	/* Block indices, as on glprogram_t: VertParams from the vertex stage,
	 * FragParams and AliasFrag from salias_frag.  AliasVert is bound but its
	 * index is not kept, as on glprogram_t. */
	GLint	ub_vert;
	GLint	ub_frag;
	GLint	ub_afrag;
} gl_alias_inst_prog_t;

extern gl_alias_inst_prog_t gl_shader_alias_inst;
extern gl_alias_inst_prog_t gl_shader_alias_inst_np;	/* affine twin, uhexen2-ktjv */

void	GL_AliasInst_Init (void);
void	GL_AliasInst_Shutdown (void);

void	GL_ParticleGPU_SetUniforms (const gl_particle_gpu_prog_t *prog,
				     const float *pup, const float *pright,
				     const float *vpn, const float *origin,
				     float ctime);

/* InstanceData.ShadedotRow carries four things, because widening the record
 * would change the 80-byte layout engine/shaders/salias_inst_vert.glsl and
 * R_CollectAliasInstance both hardcode.  The shader gets these through
 * its header chunk (GLSL_ALIAS_INST_VERT_HEADER in gl_shader.c); its #ifndef
 * copies are for the offline SPIR-V compile and are pinned to these by
 * tools/test_alias_inst_vert_block.py.  The low bits are the quantized-yaw row into the shadedots
 * table (0..15); bit 8 says this entity's CPU lighting came from the arm of
 * R_DrawAliasModel's branch chain that accumulates dynamic light, and so may
 * take it per-pixel from the froxel grid.  MLS_ABSLIGHT, model-lightstyle and
 * EF_ROTATE entities carry it clear and shade exactly as they did before --
 * they never took dlights on the CPU either.  uhexen2-waum. */
#define ALIAS_INST_SHADEDOT_ROW_MASK	255
#define ALIAS_INST_DLIGHT_BIT		256
/* Bit 9 marks the gl_fullbrights glow redraw's records: the shader draws them
 * flat white, unshaded, as GL_DrawAliasFrame's model_fullbright_pass does.  A
 * glow is emissive, so the shadedot (or MD3 1 + d) term that the base pass
 * takes must not scale it.  GitHub #491. */
#define ALIAS_INST_FULLBRIGHT_BIT	512
/* Bits 16..31 hold the entity yaw in 1/65536 turns, for the MD3 arm, which
 * shades by a real normal against R_DrawAliasModel's unquantized shadevector
 * and so cannot use the 16-step row.  GitHub #492. */
#define ALIAS_INST_YAW_SHIFT		16

/* Vertex attribute locations (fixed, shared across all programs).  The
 * file-backed shaders in engine/shaders/ repeat these as layout(location=)
 * qualifiers, which OVERRIDE the glBindAttribLocation calls in GL_LinkProgram
 * rather than conflicting with them -- so the two lists have to agree. */
#define ATTR_POSITION	0
#define ATTR_TEXCOORD	1
#define ATTR_LMCOORD	2
#define ATTR_COLOR	3

/* GL_UNIFORM_BUFFER binding points.  One global index space per context,
 * shared with the compute pipeline -- 0 is gl_worldcull.c's Hi-Z HizParams
 * and 4 is its CullParams, 3 is gl_ssao.c's SsaoParams, 5 is
 * gl_lightcluster.c's LcParams, 6 is gl_postprocess.c's BloomParams, 7
 * is its OitParams, 8 its PpVert and 9 is its PpParams, so the graphics
 * blocks start at 1 and the alias, world and OIT families' own pick up at 10.  Keyed by stage rather than by program:
 * only one program is bound at a time, so each program's own vertex-stage
 * block is pinned to the same point rather than every program claiming a new
 * one.  A driver need only guarantee a handful of these.
 *
 * The shader source cannot name them -- layout(binding=) on a uniform block
 * is GL 4.2 / ES 3.10 and the ES tier is ES 3.00 -- so glUniformBlockBinding
 * in GL_InitProgramUniforms is where they are actually decided, and these
 * defines are the only place the two sides meet. */
#define UBO_BINDING_VERT	1
#define UBO_BINDING_FRAG	2
/* The alias family's own blocks (engine/shaders/alias.inc, #450): 11 is
 * salias_frag's AliasFrag, 10 the vertex stages' AliasVert. */
#define UBO_BINDING_ALIAS_VERT	10
#define UBO_BINDING_ALIAS_FRAG	11
/* The world family's (engine/shaders/world.inc, #450 W2): sworld_frag's and
 * sworld_opaque_frag's WorldFrag. */
#define UBO_BINDING_WORLD_FRAG	12
/* The OIT twins' (engine/shaders/oit.inc, #450 W3): OitFrag, the pass's
 * output mode.  Desktop only in practice -- no ES program declares it -- but
 * still under ES 3.00's 24 binding points. */
#define UBO_BINDING_OIT_FRAG	13

void	GL_Shaders_Init (void);
void	GL_Shaders_Shutdown (void);
void	GL_ReportShaderStatus (void);	/* `renderer_status` diagnostic (d2c46f078) */

#endif /* GL_SHADER_H */
