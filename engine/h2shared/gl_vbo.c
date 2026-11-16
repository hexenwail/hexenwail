/* gl_vbo.c -- VBO/VAO helpers and streaming immediate-mode replacement
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
#include "gl_pipeline.h"
#include "gl_matrix.h"
#include "gl_vbo.h"
#include "gl_sky.h"
#include "gl_postprocess.h"
#include "gl_uniforms.h"

/* fog globals from gl_fog.c */
extern float r_fog_density;
extern float r_fog_color[3];

/* ------------------------------------------------------------------ */
/* Vertex format for the streaming VBO:                                */
/*   pos[3] + texcoord[2] + lmcoord[2] + color[4] = 11 floats         */
/* ------------------------------------------------------------------ */

#define IMM_FLOATS_PER_VERT	11
#define IMM_STRIDE		(IMM_FLOATS_PER_VERT * sizeof(float))

#define IMM_OFF_POS		0
#define IMM_OFF_TEXCOORD	(3 * sizeof(float))
#define IMM_OFF_LMCOORD		(5 * sizeof(float))
#define IMM_OFF_COLOR		(7 * sizeof(float))

typedef struct {
	float	pos[3];
	float	texcoord[2];
	float	lmcoord[2];
	float	color[4];
} immvert_t;

static immvert_t	imm_buffer[GL_IMM_MAX_VERTS];
static int		imm_count;
static float	imm_alpha_threshold = -1.0f;	/* -1 = use shader default */
/* uhexen2-khsa r13: tracks whether the current immediate-mode batch should
 * force fragColor.a = 1.0 (opaque) or preserve color.a (translucent).  Set
 * by GL_SetForceOpaqueAlpha; flushed in GL_ImmEnd. */
static float	imm_force_opaque_alpha = -1.0f;	/* -1 = use shader default */
/* Underwater caustics on alias batches (uhexen2-0gn3).  These have a
 * meaningful "off" value rather than a -1 sentinel: 0
 * intensity is the resting state, and the alias draw paths restore it when
 * they finish so sprites / warp polys / brush polys sharing gl_shader_alias
 * never inherit the effect. */
static float	imm_alias_caustics[2] = { 0.0f, 0.0f };
/* Clustered dynamic light for the alias family (uhexen2-waum).  Resting
 * state is 0 scale, which is what every sprite / particle / warp-poly /
 * unlit-brush-poly batch that shares gl_shader_alias must see: they have no
 * business taking a model's dynamic light, and R_DrawAliasModel puts this
 * back to zero on the way out for exactly that reason. */
static float	imm_alias_dlight = 0.0f;
static float	imm_turb[2] = { 0.0f, 0.0f };	/* uhexen2-9o7u: x=amplitude (0=off), y=time */
/* Soft-particle fade for sprite batches (uhexen2-mf9u).  Like the caustics
 * pair above, 0 intensity is the resting state rather than a -1 sentinel, and
 * R_DrawSpriteModel restores it after each sprite so the alias / warp / brush
 * batches that share gl_shader_alias never inherit the fade. */
static float	imm_soft[3] = { 0.0f, 0.0f, 0.0f };
/* Material maps on alias skins (uhexen2-4kcb).  Same shape as the caustics and
 * dlight pairs above and for the same reason: gl_shader_alias is shared with
 * sprites, particles, warp polys and unlit brush polys, and zero intensity is
 * the resting state they must all see.  Because the resting state IS zero,
 * those batches need no explicit clear of their own -- only the alias paths
 * ever raise it, and they put it back on the way out.  The exponent rests at
 * 1, which is the shader's own floor, so a stale value can never be read as a
 * divide-by-nothing.
 *
 * lightdir is the direction relief is lit from, in the frame that program's
 * v_matpos uses.  Its resting value is +Z, a unit vector, so the shader's
 * normalize() is safe even on a batch that never sets it. */
static float	imm_alias_material[3] = { 0.0f, 0.0f, 1.0f };
static float	imm_alias_lightdir[3] = { 0.0f, 0.0f, 1.0f };
static float	imm_alias_model[16] = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
};

/* Current vertex state (accumulated between calls) */
static float	imm_cur_tc[2];
static float	imm_cur_lm[2];
static float	imm_cur_color[4] = { 1, 1, 1, 1 };

/* GPU objects.  On desktop GL the VBO is gone — vertex data is streamed
 * through GL_Upload's frame ring and bound per-draw via glBindVertexBuffer.
 * On WebGL2 (no ARB_vertex_attrib_binding, no buffer_storage / ring) we
 * keep the original per-draw glBufferData(STREAM_DRAW) path on a
 * dedicated VBO.  uhexen2-y1v5. */
static GLuint	imm_vao;
#ifdef USE_GLES
static GLuint	imm_vbo;
#endif

/* Index buffer for quad-to-triangle conversion */
#define IMM_MAX_QUADS		(GL_IMM_MAX_VERTS / 4)
#define IMM_MAX_QUAD_INDICES	(IMM_MAX_QUADS * 6)
static GLuint	imm_quad_ibo;

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                     */
/* ------------------------------------------------------------------ */

void GL_VBO_Init (void)
{
	unsigned short *indices;
	int i;

	/* create VAO */
	glGenVertexArrays_fp(1, &imm_vao);
	glBindVertexArray_fp(imm_vao);

#ifdef USE_GLES
	/* WebGL2: dedicated VBO; attributes baked to it via VertexAttribPointer. */
	glGenBuffers_fp(1, &imm_vbo);
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, sizeof(imm_buffer), NULL, GL_STREAM_DRAW);

	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribPointer_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_POS);
	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribPointer_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_TEXCOORD);
	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribPointer_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_LMCOORD);
	glEnableVertexAttribArray_fp(ATTR_COLOR);
	glVertexAttribPointer_fp(ATTR_COLOR, 4, GL_FLOAT, GL_FALSE,
				  IMM_STRIDE, (void *)(size_t)IMM_OFF_COLOR);
#else
	/* Desktop GL 4.3: separate vertex attribute bindings.  Format is
	 * recorded in the VAO once; the source buffer + offset is rebound
	 * per draw via glBindVertexBuffer(0, ring_buf, ring_ofs, stride). */
	glEnableVertexAttribArray_fp(ATTR_POSITION);
	glVertexAttribFormat_fp(ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, IMM_OFF_POS);
	glVertexAttribBinding_fp(ATTR_POSITION, 0);

	glEnableVertexAttribArray_fp(ATTR_TEXCOORD);
	glVertexAttribFormat_fp(ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, IMM_OFF_TEXCOORD);
	glVertexAttribBinding_fp(ATTR_TEXCOORD, 0);

	glEnableVertexAttribArray_fp(ATTR_LMCOORD);
	glVertexAttribFormat_fp(ATTR_LMCOORD, 2, GL_FLOAT, GL_FALSE, IMM_OFF_LMCOORD);
	glVertexAttribBinding_fp(ATTR_LMCOORD, 0);

	glEnableVertexAttribArray_fp(ATTR_COLOR);
	glVertexAttribFormat_fp(ATTR_COLOR, 4, GL_FLOAT, GL_FALSE, IMM_OFF_COLOR);
	glVertexAttribBinding_fp(ATTR_COLOR, 0);
#endif

	/* create index buffer for quad-to-triangle conversion */
	glGenBuffers_fp(1, &imm_quad_ibo);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
	indices = (unsigned short *) malloc(IMM_MAX_QUAD_INDICES * sizeof(unsigned short));
	if (!indices)
		Sys_Error("GL_VBO_Init: out of memory allocating quad IBO scratch buffer");
	for (i = 0; i < IMM_MAX_QUADS; i++)
	{
		indices[i*6 + 0] = i*4 + 0;
		indices[i*6 + 1] = i*4 + 1;
		indices[i*6 + 2] = i*4 + 2;
		indices[i*6 + 3] = i*4 + 0;
		indices[i*6 + 4] = i*4 + 2;
		indices[i*6 + 5] = i*4 + 3;
	}
	glBufferData_fp(GL_ELEMENT_ARRAY_BUFFER,
			 IMM_MAX_QUAD_INDICES * sizeof(unsigned short),
			 indices, GL_STATIC_DRAW);
	free(indices);

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);

	Con_SafePrintf("GL_VBO: streaming VBO initialized (%d max verts)\n",
		       GL_IMM_MAX_VERTS);
}

void GL_VBO_Shutdown (void)
{
	if (imm_quad_ibo) { glDeleteBuffers_fp(1, &imm_quad_ibo); imm_quad_ibo = 0; }
#ifdef USE_GLES
	if (imm_vbo)      { glDeleteBuffers_fp(1, &imm_vbo); imm_vbo = 0; }
#endif
	if (imm_vao)      { glDeleteVertexArrays_fp(1, &imm_vao); imm_vao = 0; }
}

/* ------------------------------------------------------------------ */
/* Immediate-mode replacement API                                      */
/* ------------------------------------------------------------------ */

int GL_ImmCount (void)
{
	return imm_count;
}

void GL_ImmBegin (void)
{
	imm_count = 0;
}

void GL_ImmTexCoord2f (float s, float t)
{
	imm_cur_tc[0] = s;
	imm_cur_tc[1] = t;
}

void GL_ImmLMCoord2f (float s, float t)
{
	imm_cur_lm[0] = s;
	imm_cur_lm[1] = t;
}

void GL_SetAlphaThreshold (float threshold)
{
	imm_alpha_threshold = threshold;
}

float GL_GetAlphaThreshold (void)
{
	return imm_alpha_threshold;
}

/* uhexen2-khsa r13.  Pass 1.0 for opaque draws (force fragColor.a=1.0 so
 * downstream consumers of FB.a aren't confused), 0.0 to preserve color.a
 * (needed for ENTALPHA / DRF_TRANSLUCENT immediate-mode batches). */
void GL_SetForceOpaqueAlpha (float v)
{
	imm_force_opaque_alpha = v;
}

float GL_GetForceOpaqueAlpha (void)
{
	return imm_force_opaque_alpha;
}

/* uhexen2-0gn3: intensity 0 disables the caustics overlay entirely. */
void GL_SetAliasDlight (float scale)
{
	imm_alias_dlight = scale;
}

void GL_SetAliasCaustics (float intensity, float time)
{
	imm_alias_caustics[0] = intensity;
	imm_alias_caustics[1] = time;
}

/* uhexen2-9o7u.  amplitude 0 disables the per-pixel liquid warp, which is the
 * resting state for every batch that is not a turb surface under
 * r_water_pixel_warp 1. */
void GL_SetTurb (float amplitude, float time)
{
	imm_turb[0] = amplitude;
	imm_turb[1] = time;
}

/* uhexen2-mf9u.  inv_dist 0 disables the fade entirely; za/zb are the
 * window-depth -> view-distance coefficients for the live projection. */
void GL_SetSoftParticles (float inv_dist, float za, float zb)
{
	imm_soft[0] = inv_dist;
	imm_soft[1] = za;
	imm_soft[2] = zb;
}

void GL_SetAliasModelMatrix (const float *m)
{
	memcpy(imm_alias_model, m, sizeof(imm_alias_model));
}

/* Read back what R_DrawAliasModel last pushed.  The skeletal draw path
 * (uhexen2-7ok0.3) sets its uniforms directly rather than through GL_ImmEnd,
 * so it needs the same two values the immediate path would have applied —
 * recomputing them there would mean replaying the whole entity transform
 * chain a third time and risking a different answer than the batch beside
 * it. */
void GL_GetAliasModelMatrix (float *out)
{
	memcpy(out, imm_alias_model, sizeof(imm_alias_model));
}

float GL_GetAliasDlight (void)
{
	return imm_alias_dlight;
}

void GL_GetAliasCaustics (float *out2)
{
	out2[0] = imm_alias_caustics[0];
	out2[1] = imm_alias_caustics[1];
}

/* uhexen2-4kcb.  Both intensities 0 disables the material path in the shader,
 * which is the resting state every non-alias user of gl_shader_alias sees. */
void GL_SetAliasMaterial (float nrm, float gloss, float glossexp)
{
	imm_alias_material[0] = nrm;
	imm_alias_material[1] = gloss;
	imm_alias_material[2] = glossexp;
}

void GL_SetAliasLightDir (const float *dir3)
{
	imm_alias_lightdir[0] = dir3[0];
	imm_alias_lightdir[1] = dir3[1];
	imm_alias_lightdir[2] = dir3[2];
}

/* Read back for the skeletal path, which sets its uniforms directly rather
 * than through GL_ImmEnd -- the same reason GL_GetAliasModelMatrix exists. */
void GL_GetAliasMaterial (float *out3)
{
	out3[0] = imm_alias_material[0];
	out3[1] = imm_alias_material[1];
	out3[2] = imm_alias_material[2];
}

void GL_GetAliasLightDir (float *out3)
{
	out3[0] = imm_alias_lightdir[0];
	out3[1] = imm_alias_lightdir[1];
	out3[2] = imm_alias_lightdir[2];
}

void GL_ImmColor4f (float r, float g, float b, float a)
{
	imm_cur_color[0] = r;
	imm_cur_color[1] = g;
	imm_cur_color[2] = b;
	imm_cur_color[3] = a;
}

void GL_ImmColor3f (float r, float g, float b)
{
	imm_cur_color[0] = r;
	imm_cur_color[1] = g;
	imm_cur_color[2] = b;
	imm_cur_color[3] = 1.0f;
}

void GL_ImmColor4ubv (const unsigned char *c)
{
	imm_cur_color[0] = c[0] / 255.0f;
	imm_cur_color[1] = c[1] / 255.0f;
	imm_cur_color[2] = c[2] / 255.0f;
	imm_cur_color[3] = c[3] / 255.0f;
}

void GL_ImmColor3ubv (const unsigned char *c)
{
	imm_cur_color[0] = c[0] / 255.0f;
	imm_cur_color[1] = c[1] / 255.0f;
	imm_cur_color[2] = c[2] / 255.0f;
	imm_cur_color[3] = 1.0f;
}

void GL_ImmVertex3f (float x, float y, float z)
{
	immvert_t *v;

	if (imm_count >= GL_IMM_MAX_VERTS)
		return;

	v = &imm_buffer[imm_count++];
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = z;
	v->texcoord[0] = imm_cur_tc[0];
	v->texcoord[1] = imm_cur_tc[1];
	v->lmcoord[0] = imm_cur_lm[0];
	v->lmcoord[1] = imm_cur_lm[1];
	v->color[0] = imm_cur_color[0];
	v->color[1] = imm_cur_color[1];
	v->color[2] = imm_cur_color[2];
	v->color[3] = imm_cur_color[3];
}

void GL_ImmVertex2f (float x, float y)
{
	GL_ImmVertex3f(x, y, 0.0f);
}

/* ------------------------------------------------------------------ */
/* Flush / draw                                                        */
/* ------------------------------------------------------------------ */

/* GL_QUADS is not available in GLES / GL core.  We convert quads to
 * triangles using the pre-built index buffer.  (The enum itself comes from
 * glheader.h, which shims it for GLES3.) */

/* No per-program uniform cache any more: the world programs were the last to
 * declare loose non-opaque uniforms, and since #450 W2 every program GL_ImmEnd
 * draws with reads its per-batch values from blocks, whose shadow in
 * gl_uniforms.c already skips an upload when nothing changed. */

/*
===============
GL_ImmPushBlocks

The block-backed half of GL_ImmEnd's dual path, for the shaders that have moved
into engine/shaders/ and declare their non-opaque uniforms inside one of the
two shared std140 blocks (gl_uniforms.h).

Push granularity is per-BLOCK, which is why the values go through the shadow in
gl_uniforms.c rather than being handed over here one at a time.  A per-uniform
push takes fresh storage and rebinds for each value, so a second value either
lands in a range the first one is no longer bound to, or silently orphans the
first -- an arrangement that only works while a block has exactly one member.

Nothing is cached against the program the way the loose uniforms were.  A
loose uniform is per-PROGRAM state, so "we already sent this" is a fact about
the program; a block is context-wide, so the only meaningful question is
whether the CONTENTS changed, and that is exactly what the shadow's dirty flags
answer.  Each half is gated on its own block index so a program declaring only
one of the two (sflat's fragment stage reads nothing) pushes only that one.
===============
*/
static void GL_ImmPushBlocks (const glprogram_t *shader, const float *mvp)
{
	/* Eye position and the sky wind used to reach the sky programs as loose
	 * uniforms pushed from the same globals in the glUniform* half of
	 * GL_ImmEnd; this is now their only feed on that path.  Written
	 * unconditionally, because the shadow's dirty flags make an unchanged
	 * value free.  The wind members are read only by the ssky_* stages.  The
	 * eye position is also read by salias_inst_vert (u_eyepos, VertParams
	 * u_vscene), whose draw path sets the same r_origin itself, so this
	 * changes nothing it sees.  R_SetEyePos fills both blocks' copies from
	 * one value. */
	if (shader->ub_vert >= 0 || shader->ub_frag >= 0)
		R_SetEyePos (r_origin);

	if (shader->ub_vert >= 0)
	{
		float mv[16];

		/* u_mvp and u_modelview (VertParams u_mvpm / u_modelviewm): the
		 * only feed on this path for every program, the world family
		 * included since sworld_vert moved to a file (#450 W1) -- no
		 * program declares either loose any more.  Both written every
		 * batch; the shadow's dirty flags make an unchanged value free. */
		R_SetMVP (mvp);
		GL_GetModelview(mv);
		R_SetModelView (mv);
		/* The alias vertex stages read u_alias_model and u_alias_dlight
		 * from VertParams since #450, and no program declares either
		 * loose any more, so this is their only feed.  No other block
		 * program reads either member, and an unchanged value costs
		 * nothing. */
		R_SetModelMatrix (imm_alias_model);
		R_SetAliasDlight (imm_alias_dlight);
	}

	if (shader->ub_frag >= 0)
	{
		R_SetSkyWind (sky_wind_uv[0], sky_wind_uv[1]);
		R_SetSkyCubeWind (sky_wind_vec, sky_box_rot);

		/* The -1 sentinels mean "leave the shader default alone", which
		 * was a coherent thing to say only while these were loose
		 * uniforms: a program's own copy started at zero and
		 * GL_InitProgram never wrote another value, so the default was
		 * observable and per-program.  A shared block has no per-program
		 * default to fall back to -- skipping the write would inherit
		 * whatever the last unrelated batch left in the block -- so the
		 * sentinel is resolved here to the value it used to stand for.
		 *
		 * Set as a PAIR, always.  Half of this pair going unwritten,
		 * with the other half inheriting a stranger's value, is the
		 * defect that got the first attempt at uniform blocks reverted. */
		R_SetAlphaMask (imm_alpha_threshold >= 0.0f ? imm_alpha_threshold : 0.0f,
				imm_force_opaque_alpha >= 0.0f ? imm_force_opaque_alpha : 0.0f);
		/* spart_frag reads u_fog_density / u_fog_color (FragParams
		 * u_ffogv). */
		R_SetFog (r_fog_density, r_fog_color);

		/* salias_frag reads the rest of its per-batch state from FragParams
		 * and AliasFrag since #450.  Every value comes from an imm_* global
		 * (u_turb's is the one the world programs take too), and is
		 * written unconditionally: the loose copies were
		 * per-program and cached, so at every alias draw the program
		 * already held exactly imm_*, and a write per batch is what gives a
		 * global block the same property.  Gated on AliasFrag because only the ten programs
		 * that link salias_frag read any of these.  Not written here: the
		 * span cell, which R_SetSpanCell puts in AliasFrag around its own
		 * batches, and the froxel lookup, which R_LightCluster_BindForAlias
		 * writes once a frame. */
		if (shader->ub_afrag >= 0)
		{
			R_SetAliasCausticsU (imm_alias_caustics[0], imm_alias_caustics[1]);
			R_SetSoftParams (imm_soft[0], imm_soft[1], imm_soft[2]);
			R_SetAliasTurb (imm_turb[0], imm_turb[1]);
			R_SetAliasMaterialU (imm_alias_material[0], imm_alias_material[1],
					    imm_alias_material[2]);
			R_SetAliasLightDirU (imm_alias_lightdir);
		}

		/* The world programs' whole fragment set (#450 W2): lit water,
		 * underwater walls, the legacy brush-poly fallback and translucent
		 * world polys, on gl_shader_world or its OIT twin.  The values the
		 * loose half of GL_ImmEnd used to push per program -- the alpha
		 * pair from the same imm_* (with -1 resolved as above), fog,
		 * overbright (R_WorldOverbright, as every world path), and
		 * the liquid warp -- plus the frame record, which these programs
		 * held per program from R_SetupFrame on.  Every batch, because the
		 * block is global: the DrawTextureChains fast path, a brush entity
		 * or an alias batch may have drawn since the last one.  The span
		 * cell rides the frame record, where lit water's R_SetSpanCell
		 * brackets put it. */
		if (shader->ub_wfrag >= 0)
			R_SetWorldFrag (r_fog_density, r_fog_color,
					imm_alpha_threshold >= 0.0f ? imm_alpha_threshold : 0.0f,
					imm_force_opaque_alpha >= 0.0f ? imm_force_opaque_alpha : 0.0f,
					R_WorldOverbright (),
					imm_turb[0], imm_turb[1]);
	}

	R_FlushUniforms ();
}

/* Force a full re-upload and rebind of every block at the next flush, after
 * a vid_restart.  (It also reset GL_ImmEnd's per-program loose-uniform cache,
 * which went with the last loose world uniform, #450 W2.  That is what the
 * world draw paths called it for, so they stopped, W3: nothing outside
 * R_FlushUniforms binds GL_UNIFORM_BUFFER points 1, 2 or 10-13 -- every other
 * indexed uniform binding is 0 or 3-9, and only gl_buffer.c issues any -- and
 * a block's buffer is written only by that flush, so between flushes the
 * shadow is what the GPU holds.) */
void GL_ImmInvalidateState (void)
{
	/* Re-upload every block and re-assert its binding at the next flush:
	 * after a vid_restart the buffers are new. */
	R_InvalidateUniforms ();
}

void GL_ImmEnd (GLenum mode, const glprogram_t *shader)
{
	float mvp[16];

	if (imm_count == 0)
		return;

	/* Bind VAO and stream vertex data.  Desktop GL 4.3 routes through the
	 * frame ring (GL_Upload returns buf+offset); WebGL2 falls back to a
	 * dedicated VBO orphaned each frame via glBufferData(STREAM_DRAW). */
	glBindVertexArray_fp(imm_vao);
#ifdef USE_GLES
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);
#else
	{
		GLuint   _imm_buf;
		GLintptr _imm_ofs;
		GL_Upload (GL_ARRAY_BUFFER, imm_buffer,
			   imm_count * sizeof(immvert_t),
			   &_imm_buf, &_imm_ofs);
		glBindVertexBuffer_fp(0, _imm_buf, _imm_ofs, IMM_STRIDE);
	}
#endif

	R_UseProgram (shader->program);

	GL_GetMVP(mvp);

	/* Every program drawn here reads its non-opaque uniforms from blocks
	 * (#450; the world family's fragment stages since W2), keyed on whether
	 * the program has a block index and never on which shader it is. */
	if (shader->ub_vert >= 0 || shader->ub_frag >= 0)
		GL_ImmPushBlocks(shader, mvp);

	/* A global glEnable(GL_BLEND) in the translucent draw paths resets the
	 * per-buffer blend funcs to the global default on some drivers, which
	 * breaks the WBOIT revealage buffer (geometry vanishes).  Re-assert the
	 * OIT blend funcs immediately before every draw inside the OIT pass.
	 *
	 * These are re-asserts, so they look exactly like the redundant calls
	 * gl_pipeline.c exists to drop.  They survive because R_SetBlend marks
	 * the indexed funcs unknown whenever it actually issues glEnable(GL_BLEND)
	 * -- the only thing that can clobber them -- so the pair below reaches GL
	 * on precisely the draws that need it and folds away on the rest. */
	if (OIT_InPass())
	{
		R_SetBlendFuncIndexed (0, GL_ONE, GL_ONE);
		R_SetBlendFuncIndexed (1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
	}

	/* draw */
	if (mode == GL_QUADS)
	{
		int num_quads = imm_count / 4;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
		glDrawElements_fp(GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		glDrawArrays_fp(GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		glDrawArrays_fp(mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);
	R_UseProgram (0);

	imm_count = 0;
}

/* GL_ImmDraw: upload and draw without touching the shader program.
 * Caller must have already called glUseProgram and set uniforms. */
void GL_ImmDraw (GLenum mode)
{
	if (imm_count == 0)
		return;

	glBindVertexArray_fp(imm_vao);
#ifdef USE_GLES
	glBindBuffer_fp(GL_ARRAY_BUFFER, imm_vbo);
	glBufferData_fp(GL_ARRAY_BUFFER, imm_count * sizeof(immvert_t),
			 imm_buffer, GL_STREAM_DRAW);
#else
	{
		GLuint   _imm_buf;
		GLintptr _imm_ofs;
		GL_Upload (GL_ARRAY_BUFFER, imm_buffer,
			   imm_count * sizeof(immvert_t),
			   &_imm_buf, &_imm_ofs);
		glBindVertexBuffer_fp(0, _imm_buf, _imm_ofs, IMM_STRIDE);
	}
#endif

	if (mode == GL_QUADS)
	{
		int num_quads = imm_count / 4;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, imm_quad_ibo);
		glDrawElements_fp(GL_TRIANGLES, num_quads * 6,
				   GL_UNSIGNED_SHORT, NULL);
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	else if (mode == GL_POLYGON)
	{
		glDrawArrays_fp(GL_TRIANGLE_FAN, 0, imm_count);
	}
	else
	{
		glDrawArrays_fp(mode, 0, imm_count);
	}

	glBindVertexArray_fp(0);
	glBindBuffer_fp(GL_ARRAY_BUFFER, 0);

	imm_count = 0;
}
