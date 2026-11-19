/* gl_lightcluster.c -- clustered GPU dynamic lighting: the froxel grid
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Phase A of uhexen2-a5nn.1, ported from Ironwail's cluster_lights compute pass
 * (Quake/gl_shaders.h).  The frustum is diced into LIGHT_TILES_X * _Y * _Z
 * froxels -- linear across the screen, logarithmic in depth -- and each froxel
 * gets a bitmask of the dynamic lights that reach it.  Consumers then evaluate
 * only the handful of lights their pixel's froxel names, instead of the engine
 * rewriting lightmap blocks on the CPU every frame.
 *
 * THE CONSUMER IS THE WORLD FRAGMENT SHADER (phase B, uhexen2-26bm): see
 * engine/shaders/world_dlight.inc (with lightgrid.inc), which reads a
 * fragment's froxel out of this grid and evaluates only the lights whose bits
 * are set in it.  Alias models
 * are NOT a consumer and stay on the CPU term -- finding 2 on uhexen2-a5nn.1.
 *
 * `lightcluster_verify` at the bottom reads the grid back and checks it against
 * a CPU implementation of the same test.  It predates the consumer and is kept:
 * "the picture looks right" cannot separate a clustering bug from a shading
 * one, and this can.
 *
 * THREE WAYS THIS DEPARTS FROM UPSTREAM, all forced:
 *
 * 1. OUR VIEW SPACE IS GL'S, NOT QUAKE'S.  Ironwail's shader works in a space
 *    where depth runs along +X: its near plane is vec4(-1,0,0,z0) and its
 *    IntersectDepthPlane returns vec3(depth, ...).  Our modelview is the
 *    ordinary GL one built in R_SetupGL, where the camera looks down -Z.
 *    Rather than build a second set of matrices in a second convention -- and
 *    have them drift from the ones everything else uses -- the shader below is
 *    rewritten for -Z depth.  That is why the near/far planes and
 *    IntersectDepthPlane do not match upstream line for line; the side planes,
 *    which come out of the projection matrix, do.
 *
 * 2. OUR MAX_DLIGHTS IS 128, UPSTREAM'S IS 64.  Their per-froxel mask is a
 *    uvec4 with only .xy used (rg32ui); ours fills all four (rgba32ui), which
 *    is 16384 froxels * 16 bytes = 256 KB.  surf->dlightbits has been
 *    unsigned int[4] for the same reason since uhexen2-liqz, so the width is
 *    one the engine already assumes.
 *
 * 3. HEXEN II HAS SUBTRACTIVE DLIGHTS.  dlight_t carries `dark` (client.h),
 *    and Quake has no equivalent, so upstream's Light struct has nowhere to put
 *    it.  Clustering does not care -- a dark light occupies exactly the same
 *    sphere as a bright one -- so the colour rides negated, and the shading
 *    side accumulates with a clamp at zero, which is what the CPU path does
 *    (gl_rsurf.c, the `dark` arm of R_AddDynamicLights).  Decided in phase B;
 *    see world_dlight.inc.
 *
 * A NOTE ON NEGATIVE RADII, which look like a fourth case and are not.
 * EF_SPIT allocates a dlight with radius -120 (cl_main.c) and leaves `dark`
 * false.  Such a light lights nothing today: R_MarkLights compares dist against
 * light->radius and, with the radius negative, always descends one child and
 * marks no surface, and R_AddDynamicLights would reject it anyway once
 * rad -= fabs(dist) drives it below minlight.  The alias path rejects it too
 * (add = radius - dist is never positive).  The gather below drops them for the
 * same reason, so the GPU path inherits exactly the existing behaviour rather
 * than lighting the world with something that has never lit it.
 */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_matrix.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_sky.h"		/* gl_farclip -- the projection far plane lives there */
#include "gl_lightcluster.h"
#include "gl_uniforms.h"	/* R_SetWorldDlight, R_SetAliasLightGrid -- see the two binds */

#ifndef USE_GLES

#define LIGHT_MASK_WORDS	(MAX_DLIGHTS / 32)	/* 4 at MAX_DLIGHTS 128 */
#define LIGHT_FROXELS		(LIGHT_TILES_X * LIGHT_TILES_Y * LIGHT_TILES_Z)

/* Matches the std430 `Light` in the shader: vec3+float, vec3+float. */
typedef struct {
	float	origin[3];
	float	radius;
	float	color[3];
	float	minlight;
} gpulight_t;

cvar_t	r_lightclusters = {"r_lightclusters", "1", CVAR_ARCHIVE};
/* Phase C's own switch (uhexen2-waum).  Separate from r_lightclusters because
 * the two changes are visible in different places and want separate A/Bs: the
 * world one moves dynamic light off the lightmap atlas, this one changes how
 * EVERY alias model in the game is lit.  Turning this off leaves world surfaces
 * on the GPU path and puts models back on the flat per-entity term; turning
 * r_lightclusters off takes both.  Both default on. */
cvar_t	r_lightclusters_models = {"r_lightclusters_models", "1", CVAR_ARCHIVE};

static GLuint		lc_prog;
static GLuint		lc_ssbo;
static GLuint		lc_grid;		/* LIGHT_TILES^3 RGBA32UI */
static qboolean		lc_ready;		/* resources built */
static qboolean		lc_failed;		/* build failed; do not retry */
static qboolean		lc_valid_this_frame;

/* Uniform-block binding point for LcParams.  0 Hi-Z, 1/2 the shared
 * vert/frag blocks, 3 SsaoParams, 4 CullParams; see gl_shader.h.  The shader
 * cannot name it (ES 3.00 has no layout(binding=) on a block), so it is
 * assigned once per program with glUniformBlockBinding. */
#define LC_PARAMS_UBO_BINDING	5

/* Mirrors the std140 LcParams block in engine/shaders/lightcluster_comp.glsl:
 * every member mat4/vec4/ivec4, so the C layout is std140's by construction.
 * The matrices are copied as the 16 floats glUniformMatrix4fv was handed with
 * transpose GL_FALSE, which is std140's default column_major order. */
typedef struct {
	float	view[16];
	float	tproj[16];
	float	zlog[4];	/* x log-depth scale, y log-depth bias */
	GLint	count[4];	/* x number of lights */
} lc_params_t;			/* 160 bytes */

COMPILE_TIME_ASSERT(lcparams_size, sizeof(lc_params_t) == 160);

/* This frame's lights, in the order the shader indexes them, kept so the
 * verifier can recompute without guessing which dlights were live. */
static gpulight_t	lc_lights[MAX_DLIGHTS];
static int		lc_src[MAX_DLIGHTS];	/* cl_dlights[] slot of each; -1 synthetic */
static int		lc_numlights;
static float		lc_view[16];
static float		lc_tproj[16];
static float		lc_zlogscale, lc_zlogbias;

/* The viewport the grid was clustered against, as the fragment side needs it:
 * xy = origin in window coordinates, zw = froxels per window pixel.  Read back
 * from GL rather than recomputed from r_refdef, because render scale and the
 * "fudge around because of frac screen scale" nudges in R_SetupGL both move it
 * and a second derivation of the same rectangle is a second thing to get wrong. */
static float		lc_vp[4];

static void LC_UploadAndDispatch (void);

/* ------------------------------------------------------------------ */
/* The compute pass                                                    */
/* ------------------------------------------------------------------ */

/* The pass itself is engine/shaders/lightcluster_comp.glsl (#450), built as
 * RPROG_LIGHTCLUSTER.  Its #version and sizes come from that row's header
 * chunk, GLSL_LIGHTCLUSTER_HEADER in gl_shader.c, pasted from the same
 * LIGHT_TILES_* / MAX_DLIGHTS / MAX_LIGHTSTYLES / LIGHT_SSBO_BINDING macros
 * this file sizes its buffers with. */

/* row-major mat4 (the layout GL_GetModelview hands back) times a column vec4 */
static void LC_TransformVec4 (const float m[16], const float v[4], float out[4])
{
	int i;
	for (i = 0; i < 4; i++)
		out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

static int LC_PopCount (unsigned int v)
{
	int n = 0;
	while (v) { v &= v - 1; n++; }
	return n;
}

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */

static qboolean R_LightCluster_Build (void)
{
	if (lc_failed)
		return false;
	if (!gl_renderer_caps.compute_shaders || !gl_renderer_caps.shader_storage)
		return false;
	if (!glBindImageTexture_fp || !glTexImage3D_fp || !glBindBufferBase_fp)
		return false;

	/* LIGHT_SSBO_BINDING is above the 8 bindings GL 4.3 guarantees, because
	 * gl_worldcull.c's compute pass claims every index below it and does not
	 * restore them -- see the header.  Refuse rather than silently bind to an
	 * index the driver does not have: the failure mode of getting this wrong
	 * is dynamic lights that do nothing, which reads as "the port doesn't
	 * work" and not as "this driver is short of binding points". */
	{
		GLint maxbind = 0;
		glGetIntegerv_fp (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxbind);
		if (maxbind <= LIGHT_SSBO_BINDING)
		{
			Con_SafePrintf ("LightCluster: disabled, driver has %d shader "
					"storage bindings and this needs %d\n",
					(int)maxbind, LIGHT_SSBO_BINDING + 1);
			lc_failed = true;
			return false;
		}
	}

	lc_prog = R_Program_Create (RPROG_LIGHTCLUSTER);
	if (!lc_prog)
	{
		lc_failed = true;
		return false;
	}

	/* Pin LcParams to its binding point, and check the driver laid the block
	 * out as lc_params_t does (the RU_CheckBlockSize practice).  The binding
	 * dies with the program, so vid_restart is covered by construction. */
	if (glGetUniformBlockIndex_fp && glUniformBlockBinding_fp)
	{
		GLuint	blk = glGetUniformBlockIndex_fp (lc_prog, "LcParams");
		GLint	size = 0;

		if (blk == GL_INVALID_INDEX)
		{
			Con_SafePrintf ("LightCluster: disabled, no LcParams block in the program\n");
			lc_failed = true;
			return false;
		}
		if (glGetActiveUniformBlockiv_fp)
		{
			glGetActiveUniformBlockiv_fp (lc_prog, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
			if (size != (GLint)sizeof(lc_params_t))
				Con_Printf ("WARNING: uniform block LcParams is %d bytes on this driver, "
					    "C mirror is %d -- light clustering will be wrong\n",
					    (int)size, (int)sizeof(lc_params_t));
		}
		glUniformBlockBinding_fp (lc_prog, blk, LC_PARAMS_UBO_BINDING);
	}

	glGenBuffers_fp (1, &lc_ssbo);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, lc_ssbo);
	glBufferData_fp (GL_SHADER_STORAGE_BUFFER,
			 (GLsizeiptr)(MAX_LIGHTSTYLES * sizeof(float) + sizeof(lc_lights)),
			 NULL, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, 0);

	R_GenTextures (1, &lc_grid);
	R_BindTextureForEdit (GL_TEXTURE_3D, lc_grid);
	R_TexImage3D (GL_TEXTURE_3D, lc_grid, 0, GL_RGBA32UI,
			 LIGHT_TILES_X, LIGHT_TILES_Y, LIGHT_TILES_Z,
			 GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
	R_TexSetFilter (GL_TEXTURE_3D, lc_grid, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap (GL_TEXTURE_3D, lc_grid, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_3D, 0);

	if (!lc_ssbo || !lc_grid)
	{
		lc_failed = true;
		return false;
	}

	lc_ready = true;
	if (developer.integer >= 2)
		Con_SafePrintf ("LightCluster: %dx%dx%d froxels, %d lights max\n",
				LIGHT_TILES_X, LIGHT_TILES_Y, LIGHT_TILES_Z, MAX_DLIGHTS);
	return true;
}

/* ------------------------------------------------------------------ */
/* Per-frame                                                           */
/* ------------------------------------------------------------------ */

/* The dlights that are alive this frame, in the order the shader will index
 * them.  Radius is the cull radius, so a light whose radius has decayed to
 * nothing is dropped rather than clustered into every froxel it grazes. */
static void R_LightCluster_GatherLights (void)
{
	int	i;

	lc_numlights = 0;

	/* The two switches that make the CPU path contribute nothing, honoured
	 * here so the GPU path contributes nothing either.  Getting this wrong
	 * is not a subtle brightness difference, it is dynamic lights appearing
	 * in the two configurations that exist to suppress them:
	 *
	 *   r_dynamic 0    -- R_BuildLightMap simply does not call
	 *                     R_AddDynamicLights (gl_rsurf.c).
	 *   gl_flashblend  -- R_PushDlights returns before marking anything and
	 *                     R_RenderDlights draws blend bubbles instead, so no
	 *                     dlight reaches a lightmap at all.
	 *
	 * Clearing the light list rather than refusing to run leaves the grid
	 * valid and empty, which keeps R_LightCluster_ShadesWorld true and so
	 * keeps the CPU marking suppressed -- both paths dark, one of them free. */
	if (!r_dynamic.integer || gl_flashblend.integer)
		return;

	for (i = 0; i < MAX_DLIGHTS && lc_numlights < MAX_DLIGHTS; i++)
	{
		dlight_t	*dl = &cl_dlights[i];
		gpulight_t	*out;

		if (dl->die < cl.time || dl->radius <= 0.0f)
			continue;

		lc_src[lc_numlights] = i;
		out = &lc_lights[lc_numlights++];
		out->origin[0] = dl->origin[0];
		out->origin[1] = dl->origin[1];
		out->origin[2] = dl->origin[2];
		out->radius    = dl->radius;
		/* Hexen II's subtractive lights ride as a negated colour; see the
		 * departures note at the top.  Clustering ignores it either way --
		 * a dark light fills the same sphere. */
		out->color[0]  = dl->dark ? -dl->color[0] : dl->color[0];
		out->color[1]  = dl->dark ? -dl->color[1] : dl->color[1];
		out->color[2]  = dl->dark ? -dl->color[2] : dl->color[2];
		out->minlight  = dl->minlight;
	}
}

void R_LightCluster_Update (void)
{
	float	proj[16];
	float	znear, zfar;
	GLint	vp[4];
	int	i;

	lc_valid_this_frame = false;

	if (!r_lightclusters.integer)
		return;
	if (!lc_ready && !R_LightCluster_Build())
		return;

	R_LightCluster_GatherLights ();

	/* Window rect -> froxel column/row, for the fragment side.  A zero
	 * dimension would make this a division by zero and every lookup a NaN;
	 * it should not happen (R_SetupGL always leaves a non-empty viewport)
	 * but a minimised window is exactly the sort of thing that produces one. */
	glGetIntegerv_fp (GL_VIEWPORT, vp);
	if (vp[2] <= 0 || vp[3] <= 0)
		return;
	lc_vp[0] = (float)vp[0];
	lc_vp[1] = (float)vp[1];
	lc_vp[2] = (float)LIGHT_TILES_X / (float)vp[2];
	lc_vp[3] = (float)LIGHT_TILES_Y / (float)vp[3];

	GL_GetModelview (lc_view);
	GL_GetProjection (proj);
	for (i = 0; i < 4; i++)
	{
		int j;
		for (j = 0; j < 4; j++)
			lc_tproj[i * 4 + j] = proj[j * 4 + i];
	}

	/* Slice 0 lands on the near plane and slice LIGHT_TILES_Z on the far one.
	 * NEARCLIP is 4 in gl_rmain.c; the far plane follows gl_farclip, clamped
	 * there the same way.  Reversed-Z does not enter into it: only rows 0, 1
	 * and 3 of the projection are read, and the depth row is row 2. */
	znear = 4.0f;
	zfar = gl_farclip.value;
	if (zfar < 4096.0f) zfar = 4096.0f;
	else if (zfar > 262144.0f) zfar = 262144.0f;

	lc_zlogscale = (float)LIGHT_TILES_Z / (log2f(zfar) - log2f(znear));
	lc_zlogbias = -lc_zlogscale * log2f(znear);

	LC_UploadAndDispatch ();
	lc_valid_this_frame = true;
}

/* Uploads lc_lights / the styles and runs the pass against lc_view and the
 * lc_zlog* slice mapping.  Factored out because lightcluster_verify re-runs
 * exactly this, with a synthetic light set, and a verifier that exercised a
 * different code path from the real one would be worth very little. */
static void LC_UploadAndDispatch (void)
{
	int i;

	/* Styles ride in the same buffer as upstream's do, so phase B's shading
	 * can read a style without a second binding.  Nothing reads them yet. */
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, lc_ssbo);
	{
		float styles[MAX_LIGHTSTYLES];
		for (i = 0; i < MAX_LIGHTSTYLES; i++)
			styles[i] = d_lightstylevalue[i] * (1.0f / 256.0f);
		glBufferSubData_fp (GL_SHADER_STORAGE_BUFFER, 0, sizeof(styles), styles);
		if (lc_numlights > 0)
			glBufferSubData_fp (GL_SHADER_STORAGE_BUFFER, sizeof(styles),
					    (GLsizeiptr)(lc_numlights * sizeof(gpulight_t)),
					    lc_lights);
	}
	GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, LIGHT_SSBO_BINDING, lc_ssbo);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, 0);

	R_UseProgram (lc_prog);
	{
		lc_params_t	params;
		GLuint		ubo;
		GLintptr	ofs;

		memset (&params, 0, sizeof(params));
		memcpy (params.view, lc_view, sizeof(params.view));
		memcpy (params.tproj, lc_tproj, sizeof(params.tproj));
		params.zlog[0] = lc_zlogscale;
		params.zlog[1] = lc_zlogbias;
		params.count[0] = lc_numlights;
		GL_Upload (GL_UNIFORM_BUFFER, &params, sizeof(params), &ubo, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, LC_PARAMS_UBO_BINDING,
				    ubo, ofs, sizeof(params));
	}

	R_BindImageTexture (0, lc_grid, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
	glDispatchCompute_fp (LIGHT_TILES_X / 8, LIGHT_TILES_Y / 8, LIGHT_TILES_Z);
	glMemoryBarrier_fp (GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	R_BindImageTexture (0, 0, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);

	R_UseProgram (0);
}

qboolean R_LightCluster_Available (void)
{
	return lc_valid_this_frame;
}

int R_LightCluster_NumLights (void)
{
	return lc_valid_this_frame ? lc_numlights : 0;
}

qboolean R_LightCluster_GetLight (int i, float origin[3], float *radius, int *dlight)
{
	if (!lc_valid_this_frame || i < 0 || i >= lc_numlights)
		return false;
	origin[0] = lc_lights[i].origin[0];
	origin[1] = lc_lights[i].origin[1];
	origin[2] = lc_lights[i].origin[2];
	*radius = lc_lights[i].radius;
	*dlight = lc_src[i];
	return true;
}

qboolean R_LightCluster_ShadesWorld (void)
{
	/* DELIBERATELY NOT `lc_ready`.  The resources are built lazily, inside
	 * the first R_LightCluster_Update -- and Update runs from R_SetupGL,
	 * which is AFTER R_PushDlights.  Keying off lc_ready would therefore
	 * answer false to the CPU gate on the frame the grid is first built and
	 * true to the shading gate a moment later in the same frame, which is
	 * the one combination that must never happen: the surfaces would be lit
	 * by both paths at once.
	 *
	 * So this answers from state that cannot change mid-frame.  Every input
	 * is either a cvar (console commands run between frames) or a fact
	 * settled at context creation.  lc_failed is the exception and it only
	 * ever goes false -> true, at Update time; the frame that trips it has
	 * already stood the CPU path down, so it renders once with no dynamic
	 * light and every frame after takes the CPU path.  Losing dlights for a
	 * frame on a driver that could not build the program is a better failure
	 * than double-lighting the world on every healthy one. */
	return r_lightclusters.integer != 0 && !lc_failed &&
	       gl_renderer_caps.compute_shaders && gl_renderer_caps.shader_storage;
}

qboolean R_LightCluster_ShadesAlias (void)
{
	/* Everything ShadesWorld answers from, plus this feature's own switch.
	 * Same contract as ShadesWorld and for the same reason: R_DrawAliasModel
	 * decides whether to run the CPU accumulation loop long before the draw
	 * that would consume the grid, so the answer has to be stable across the
	 * whole frame or a model gets lit twice. */
	return r_lightclusters_models.integer != 0 && R_LightCluster_ShadesWorld ();
}

qboolean R_LightCluster_AliasActive (void)
{
	/* The per-draw question, as opposed to the per-frame one above: is there
	 * anything in the grid worth a texelFetch.  An empty light list makes the
	 * shader path provably a no-op, so C pushes a zero scale and every alias
	 * fragment in the frame skips the lookup -- the same saving
	 * R_LightCluster_BindForWorld takes for world surfaces.  r_fullbright
	 * excluded on the same grounds it is there: nothing is lit at all. */
	return R_LightCluster_ShadesAlias () && lc_valid_this_frame &&
	       lc_numlights > 0 && !r_fullbright.integer;
}

/* ------------------------------------------------------------------ */
/* Phase B: handing the grid to the world fragment shaders             */
/* ------------------------------------------------------------------ */

void R_LightCluster_BindForWorld (void)
{
	float	scale;

	/* Zero switches the whole path off inside the shader with one
	 * uniform-static compare, which is also what happens on every frame the
	 * grid is not valid -- so a driver that failed to build the compute
	 * program renders the CPU path and nothing here has to know. */
	scale = 0.0f;
	/* lc_numlights == 0 is worth its own test rather than letting the shader
	 * discover an empty mask: with no live dlights the grid is all zeros and
	 * the froxel path provably adds nothing, so switching it off here saves
	 * every fragment in the frame a texelFetch and the two derivatives the
	 * geometric normal costs.  r_dynamic 0 and gl_flashblend both land here
	 * too -- the gather returns an empty list for them. */
	if (lc_valid_this_frame && lc_numlights > 0 && !r_fullbright.integer)
	{
		/* Blocklight units to lightmap units, so a GPU-lit luxel lands
		 * exactly where the CPU path would have put it.
		 *
		 * R_AddDynamicLights accumulates brightness * colour * 256 into
		 * blocklightscolor; R_BuildLightMap then stores that >> lmshift,
		 * clamped to 255, and the sampler divides by 255.  lmshift is
		 * 7 + gl_overbright, the extra bit being the headroom the
		 * shader's u_overbright multiply spends again (uhexen2-f29y).
		 * So the same colour arrives on screen either way, and toggling
		 * gl_overbright does not change dynamic-light brightness -- which
		 * is the property that makes an A/B across that cvar meaningful. */
		int lmshift = 7 + (gl_overbright.integer ? 1 : 0);
		scale = 256.0f / (255.0f * (float)(1 << lmshift));
	}

	/* Into the world frame record, which every world draw copies into
	 * WorldFrag (#450 W2).  It was three per-program glUniform rounds, one
	 * per world program -- cutout, opaque and OIT -- because a surface that
	 * missed one rendered next to its neighbours with different lighting
	 * (the trap uhexen2-uxpp documents for caustics); one shared block makes
	 * that impossible.  The lookup constants are read only while the scale
	 * is non-zero, so on a frame the grid is not live they are simply
	 * whatever the last live frame left.  The last argument is the
	 * one-sided tolerance ClusteredDlight drops a light behind a face with,
	 * the same test and the same constant as the CPU path's (#97). */
	R_SetWorldDlight (scale, lc_view, lc_vp, lc_zlogscale, lc_zlogbias,
			  r_dlight_occlusion.integer >= 1 ? DLIGHT_PLANE_EPSILON : 0.0f);

	if (scale <= 0.0f)
		return;

	/* Bound here as well as in LC_UploadAndDispatch, and not merely out of
	 * caution: the fragment shaders read this binding for the whole world
	 * draw, which is far longer than any other SSBO in the engine has to
	 * survive.  LIGHT_SSBO_BINDING is chosen above everything gl_worldcull.c
	 * claims precisely so nothing between here and the last world surface
	 * can take it back -- see the header for what happened when it wasn't. */
	GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, LIGHT_SSBO_BINDING, lc_ssbo);

	R_BindTextureTarget (LIGHT_GRID_TMU, GL_TEXTURE_3D, lc_grid);
}

/* ------------------------------------------------------------------ */
/* Phase C: handing the same grid to the alias family (uhexen2-waum)    */
/* ------------------------------------------------------------------ */

void R_LightCluster_BindForAlias (void)
{
	if (!R_LightCluster_AliasActive ())
		return;

	/* The froxel-lookup constants only.  The SCALE is deliberately not set
	 * here: it is per-batch state that R_DrawAliasModel drives through
	 * GL_SetAliasDlight and GL_ImmEnd pushes, because one entity may take
	 * dynamic light and the sprite drawn after it must not.  These change
	 * once a frame and are read only when the scale is non-zero.
	 *
	 * ONE WRITE for every program that links salias_frag.  It used to be ten
	 * glUniform rounds, one per program, because missing one showed up as
	 * "this monster is lit differently from that identical monster" (the
	 * trap uhexen2-uxpp documents for caustics).  Since #450 the fragment
	 * stage reads these from AliasFrag, which all ten share, so that trap is
	 * gone by construction; the shadow reaches the GPU with the next alias
	 * batch's flush.  A frame on which the grid is not live writes nothing
	 * and leaves last frame's values, which nothing reads: the scale that
	 * gates the lookup is 0 on such a frame (R_AliasDlightScale).
	 *
	 * The same call writes AliasVert's copy of lc_view, which the instanced
	 * vertex stage builds v_eyepos with (#450 S3).  That was the last
	 * per-program write here -- a glUniform on each instanced program, with
	 * the GL_ImmInvalidateState it obliged -- so nothing here touches a
	 * program any more. */
	R_SetAliasLightGrid (lc_view, lc_vp, lc_zlogscale, lc_zlogbias);

	/* The SSBO and the grid texture are already bound for the frame by
	 * R_LightCluster_BindForWorld, which runs immediately before this, and
	 * nothing between there and the last alias draw takes either back:
	 * LIGHT_SSBO_BINDING sits above everything gl_worldcull.c and the
	 * alias/skeletal/particle paths claim (0..4), and LIGHT_GRID_TMU is the
	 * only user of texture unit 5 in the renderer.  Rebinding here would be
	 * harmless but would also hide a future collision, so it is left to the
	 * one place that owns it. */
}

/* ------------------------------------------------------------------ */
/* Verification                                                        */
/* ------------------------------------------------------------------ */

/* Reads the grid back and recomputes it here, from the same view matrix and
 * the same light list the dispatch used.  The point is that phase A has no
 * consumer to look at, so "the clustering is right" is otherwise an
 * unfalsifiable claim; this makes it a number.
 *
 * The CPU side deliberately derives the froxel bounds a different way from the
 * shader -- corner rays straight out of the tile's NDC extents through the
 * inverse projection, rather than the shader's plane-pair cross products -- so
 * an agreeing answer is two derivations agreeing, not one repeated.
 */
static void R_LightCluster_Verify_f (void)
{
	static unsigned int	readback[LIGHT_FROXELS * 4];
	float			tx, ty;
	int			x, y, z, i;
	int			mismatch = 0, occupied = 0, bits = 0;

	if (!lc_ready || !lc_valid_this_frame)
	{
		CON_Printf (_PRINT_NONOTIFY, "lightcluster_verify: no grid this frame "
			    "(r_lightclusters %d, built %d)\n",
			    r_lightclusters.integer, lc_ready);
		return;
	}

	/* With a count, cluster a synthetic set instead of whatever dlights the
	 * frame happened to have.  The maths is what is under test, and waiting
	 * for gameplay to produce lights is a poor way to source test data -- a
	 * quiet corridor reports "0 disagreements" over zero lights and looks
	 * exactly like a pass.  The set is deterministic and spread through the
	 * view volume, so froxels at every depth slice get hit. */
	if (Cmd_Argc() > 1)
	{
		int	want = atoi (Cmd_Argv(1));
		float	fwd[3], right[3], up[3];

		if (want < 1) want = 1;
		if (want > MAX_DLIGHTS) want = MAX_DLIGHTS;
		AngleVectors (r_refdef.viewangles, fwd, right, up);

		for (i = 0; i < want; i++)
		{
			/* Deterministic spread: depth walks the log slices, lateral
			 * offset walks a coprime cycle so it does not line up with
			 * the froxel grid and hide an off-by-one. */
			float	depth = 8.0f * powf (2.0f, (float)(i % 24) * 0.4f);
			float	sx = (float)((i * 7) % 15 - 7) * 0.12f * depth;
			float	sy = (float)((i * 11) % 11 - 5) * 0.12f * depth;
			int	c;

			for (c = 0; c < 3; c++)
				lc_lights[i].origin[c] = r_refdef.vieworg[c] +
					fwd[c] * depth + right[c] * sx + up[c] * sy;
			lc_lights[i].radius = 40.0f + (float)(i % 9) * 45.0f;
			lc_lights[i].color[0] = lc_lights[i].color[1] = lc_lights[i].color[2] = 1.0f;
			lc_lights[i].minlight = 0.0f;
			lc_src[i] = -1;
		}
		lc_numlights = want;
		LC_UploadAndDispatch ();
		CON_Printf (_PRINT_NONOTIFY, "lightcluster_verify: clustering %d synthetic lights\n", want);
	}
	if (!glGetTexImage_fp)
	{
		CON_Printf (_PRINT_NONOTIFY, "lightcluster_verify: glGetTexImage unavailable\n");
		return;
	}

	R_BindTextureForEdit (GL_TEXTURE_3D, lc_grid);
	glGetTexImage_fp (GL_TEXTURE_3D, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, readback);
	R_BindTextureForEdit (GL_TEXTURE_3D, 0);

	/* The frustum straight from the field of view R_SetupGL built it with.
	 * At view depth d, NDC x spans +/- tan(fov_x/2)*d.  Deliberately NOT the
	 * projection matrix the shader reads: two derivations that agree are
	 * evidence, one derivation run twice is not. */
	tx = tanf ((float)(r_refdef.fov_x * (M_PI / 360.0)));
	ty = tanf ((float)(r_refdef.fov_y * (M_PI / 360.0)));

	for (z = 0; z < LIGHT_TILES_Z; z++)
	{
		float d0 = powf (2.0f, ((float)z         - lc_zlogbias) / lc_zlogscale);
		float d1 = powf (2.0f, ((float)z + 1.0f  - lc_zlogbias) / lc_zlogscale);

		for (y = 0; y < LIGHT_TILES_Y; y++)
		{
			for (x = 0; x < LIGHT_TILES_X; x++)
			{
				float	x0 = -1.0f + (float)x * (2.0f / LIGHT_TILES_X);
				float	x1 = x0 + (2.0f / LIGHT_TILES_X);
				float	y0 = -1.0f + (float)y * (2.0f / LIGHT_TILES_Y);
				float	y1 = y0 + (2.0f / LIGHT_TILES_Y);
				float	center[3], half[3];
				float	xa = x0 * tx * d0, xb = x0 * tx * d1;
				float	xc = x1 * tx * d0, xd = x1 * tx * d1;
				float	ya = y0 * ty * d0, yb = y0 * ty * d1;
				float	yc = y1 * ty * d0, yd = y1 * ty * d1;
				float	xmin = q_min(q_min(xa, xb), q_min(xc, xd));
				float	xmax = q_max(q_max(xa, xb), q_max(xc, xd));
				float	ymin = q_min(q_min(ya, yb), q_min(yc, yd));
				float	ymax = q_max(q_max(ya, yb), q_max(yc, yd));
				unsigned int	mask[4];
				const unsigned int *got;
				int	c;

				center[0] = (xmax + xmin) * 0.5f;  half[0] = (xmax - xmin) * 0.5f;
				center[1] = (ymax + ymin) * 0.5f;  half[1] = (ymax - ymin) * 0.5f;
				center[2] = (-d0 + -d1) * 0.5f;    half[2] = (d1 - d0) * 0.5f;

				mask[0] = mask[1] = mask[2] = mask[3] = 0;
				for (i = 0; i < lc_numlights; i++)
				{
					float	o[4], vpos[4], dist2 = 0.0f;
					o[0] = lc_lights[i].origin[0];
					o[1] = lc_lights[i].origin[1];
					o[2] = lc_lights[i].origin[2];
					o[3] = 1.0f;
					LC_TransformVec4 (lc_view, o, vpos);
					for (c = 0; c < 3; c++)
					{
						float delta = fabsf (vpos[c] - center[c]) - half[c];
						if (delta < 0.0f) delta = 0.0f;
						dist2 += delta * delta;
					}
					if (dist2 < lc_lights[i].radius * lc_lights[i].radius)
						mask[i >> 5] |= 1u << (i & 31);
				}

				got = &readback[((z * LIGHT_TILES_Y + y) * LIGHT_TILES_X + x) * 4];
				for (c = 0; c < 4; c++)
				{
					if (got[c] != mask[c])
						mismatch++;
					bits += LC_PopCount (got[c]);
				}
				if (got[0] | got[1] | got[2] | got[3])
					occupied++;
			}
		}
	}

	CON_Printf (_PRINT_NONOTIFY, "lightcluster_verify: %d lights, %d/%d froxels occupied, "
		    "%d light-refs, %d of %d mask words disagree with the CPU\n",
		    lc_numlights, occupied, LIGHT_FROXELS, bits,
		    mismatch, LIGHT_FROXELS * 4);
	if (mismatch)
		CON_Printf (_PRINT_NONOTIFY, "  non-zero is a bug: the two sides derive the froxel bounds "
			    "differently but must still agree\n");
}

void R_LightCluster_Init (void)
{
	Cvar_RegisterVariable (&r_lightclusters);
	Cvar_RegisterVariable (&r_lightclusters_models);
	Cmd_AddCommand ("lightcluster_verify", R_LightCluster_Verify_f);
}

void R_LightCluster_Shutdown (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (lc_prog) { glDeleteProgram_fp (lc_prog); lc_prog = 0; }
	if (lc_ssbo) { GL_ForgetBuffer (lc_ssbo); glDeleteBuffers_fp (1, &lc_ssbo); lc_ssbo = 0; }
	if (lc_grid) { R_DeleteTextures (1, &lc_grid); lc_grid = 0; }
	lc_ready = false;
	lc_failed = false;
	lc_valid_this_frame = false;
	lc_numlights = 0;
}

#else	/* USE_GLES: no compute shaders and no SSBOs on this tier */

void R_LightCluster_Init (void) {}
void R_LightCluster_Shutdown (void) {}
void R_LightCluster_Update (void) {}
qboolean R_LightCluster_Available (void) { return false; }
int R_LightCluster_NumLights (void) { return 0; }
qboolean R_LightCluster_GetLight (int i, float origin[3], float *radius, int *dlight)
{
	(void)i; (void)origin; (void)radius; (void)dlight;
	return false;
}
qboolean R_LightCluster_ShadesWorld (void) { return false; }
qboolean R_LightCluster_ShadesAlias (void) { return false; }
qboolean R_LightCluster_AliasActive (void) { return false; }
void R_LightCluster_BindForWorld (void) {}
void R_LightCluster_BindForAlias (void) {}

#endif	/* !USE_GLES */
