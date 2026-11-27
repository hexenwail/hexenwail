/* gl_shadow.c -- cube shadow maps for the clustered dynamic lights (GitHub #97)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Phases P3, P4 and P6 of #97, desktop GL 4.3 with r_lightclusters 1, under
 * r_dlight_occlusion 2.  Ironwail has nothing here; this is ours.
 *
 * WHAT IT FIXES.  P1's one-sided test drops a light behind a face, and P2's CPU
 * visibility (r_dlightvis.c) drops a light that cannot see a face at all -- but
 * on the GPU path a light is shaded per pixel from the froxel grid, and P2 can
 * only answer per surface on the CPU path.  The case left is a face that does
 * face the light, is in its PVS, and has a wall between: tools/dlight-leak-check.sh's
 * scene 2, and its alias scene, a model in a recess the torch's wall hides.
 *
 * HOW.  The top r_shadow_maxlights of the frame's lights each get a cube map:
 * six depth-only passes from the light, one per face, into one layer-face of a
 * GL_TEXTURE_CUBE_MAP_ARRAY (layer = slot * 6 + face, GL's face order).  Each
 * texel stores |P - L| / radius through gl_FragDepth (sshadow_frag), and the
 * pass keeps only the faces turned away from the light, so what a texel holds
 * is the far side of the first wall a ray from the light enters.  The receivers (dlshadow.inc) compare their
 * own distance against it, and anything on the light's side of that wall is
 * nearer by the wall's thickness -- so there is no bias to tune against acne,
 * only nearest-texel slack along grazing rays.
 *
 * CASTERS.  The world, from world_vao / world_ibo: the surfaces of every BSP
 * node the light's sphere touches whose own box it touches and whose FRONT
 * faces away from the light (the GPU would cull the rest anyway), merged into
 * contiguous index runs and drawn with one glMultiDrawElements per face.  Then
 * the opaque brush entities whose box the sphere touches, each with its own
 * transform.  Not casters: sky and liquids (not in the world VBO), fences and
 * translucent brush entities (light gets through them).  Liquids and fences
 * are receivers as before.  The brush entities are cl_visedicts' at R_SetupGL
 * time, so ones the server does not send (outside the view's PVS) and static
 * ones (added later, by R_DrawWorld) cast nothing.
 *
 * ALIAS CASTERS (P6), under r_shadow_casters 2: the alias models of
 * cl_visedicts the view draws opaque (SH_AliasEligible -- the same
 * translucency and r_entdist tests, no frustum cull, never the viewmodel),
 * each posed by R_AliasShadowCaster (gl_rmain.c) with the matrix and pose pair
 * its draw will use, and drawn from its own GPU mesh by sshadow_alias_vert:
 * Quake MDL and MD3 from their pose buffers, MD5 meshes skinned from their bone
 * buffer -- all three formats, whichever path (instanced, legacy, skeletal)
 * draws them in the view.  A light never takes a shadow from its own model
 * (SH_AliasCasts): not from the entity it is keyed to, which is how the torch
 * artifact (keyed to the player), a missile and a torch flame are lit, nor from
 * any model whose box holds it.  The faces culled are the world's, so a model
 * stores its far side too; but an alias receiver has no one-sided test to
 * spare that far side from comparing against itself, so the alias pass pushes
 * every vertex SH_ALIAS_PUSH units plus one texel's width further along its
 * ray from the light (sshadow_alias_vert says why that moves no texel).  A
 * wall closer than that behind a model gets no shadow from it there.  Static
 * entities are not in cl_visedicts, as for brush entities.
 *
 * WHICH LIGHTS.  Every light of R_LightCluster_GatherLights, in its order (the
 * order the shader indexes), minus those whose origin is in a solid or sky leaf
 * -- a torch mounted a unit inside its wall would see every face from behind
 * and shadow its whole room -- ranked frustum-visible first, then by radius over
 * distance from the eye.
 *
 * THE CACHE.  A map is drawn into a slot and kept while its light (same
 * cl_dlights[] slot and key) has moved less than SH_SLOP units, its radius sits
 * inside the band the map was drawn for, and the brush entities inside that
 * radius have not moved (a hash of their transforms -- a door opening redraws
 * the map).  A torch therefore costs six passes once.  Under r_shadow_casters
 * 2 the hash also holds every alias caster in range, by its model matrix and
 * pose pair: a monster that walks or animates near a light redraws that light's
 * map every frame it does, within the budget below, and over budget the map is
 * a frame or more behind the monster -- its shadow lags, it does not vanish.
 * A model standing still in a single pose costs nothing after its first map,
 * and neither does one that only turns or bobs on the clock (a pickup's
 * EF_ROTATE): it is keyed by origin, so its shadow keeps the angle it was
 * drawn at.  A model the view does not draw this frame (behind the camera)
 * still casts, but its lerp state is not advanced, so its shadow steps from
 * pose to pose.  The pass keeps far sides, so an open, one-sided mesh casts
 * only from the side whose faces turn away from the light.  Redraws are budgeted at
 * r_shadow_updates a frame, most important light first; a light over budget
 * keeps its stale map if it has one -- read from the origin it was drawn from,
 * so it is late, not wrong (dlshadow.inc) -- and goes unshadowed for a frame
 * if it has none.  0 lifts the budget.  r_shadow_cache 0 bypasses the cache
 * and redraws every map every frame (budget permitting), which is the worst
 * case r_speeds 2 should be read at, and a way to run the drawing path without
 * the cache in the way.
 *
 * TWO HALVES.  R_Shadow_Prepare is the CPU side -- choosing the lights,
 * checking the cache, walking each redrawn map's casters into one set of lists
 * -- and R_Shadow_Draw the GPU side, every face of every map in one burst, then
 * the block.  Split so r_speeds 2 can time them apart: a GPU timestamp pair
 * around a CPU walk measures the walk while the GPU waits for it.
 *
 * STATE.  Runs inside R_SetupGL, between the light-cluster update and the world
 * draw, so everything it changes it puts back: framebuffer, viewport, scissor,
 * depth test/mask/func/range, cull enable/face, stencil, polygon offset.  The
 * program and VAO are left at 0, as the light-cluster pass leaves them; every
 * draw after R_SetupGL binds its own.  Cull face is set explicitly (SH_CULL_FACE) rather than inherited,
 * which keeps the mirror pass's swapped face out of it -- and the mirror pass
 * does not redraw maps at all: the lights are the same, and so is the block.
 */

#include "quakedef.h"
#include "gl_matrix.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_lightcluster.h"
#include "gl_shadow.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"	/* R_GPU (), the DlShadow mirror (#340 P2a) */
#define SH_GPU()	R_GPU ()
#else
#define SH_GPU()	false
#endif

/* Cvars exist on every GL tier, so a config written on desktop loads quietly
 * on the web; only desktop reads them. */
cvar_t	r_shadow_resolution = {"r_shadow_resolution", "128", CVAR_ARCHIVE};
cvar_t	r_shadow_maxlights = {"r_shadow_maxlights", "8", CVAR_ARCHIVE};
cvar_t	r_shadow_filter = {"r_shadow_filter", "1", CVAR_ARCHIVE};
cvar_t	r_shadow_updates = {"r_shadow_updates", "2", CVAR_ARCHIVE};
cvar_t	r_shadow_bias = {"r_shadow_bias", "1", CVAR_ARCHIVE};
cvar_t	r_shadow_cache = {"r_shadow_cache", "1", CVAR_NONE};	/* 0: redraw every frame (profiling) */
cvar_t	r_shadow_debug = {"r_shadow_debug", "0", CVAR_NONE};
/* 1: the world and the opaque brush entities cast; 2: the alias models too (P6). */
cvar_t	r_shadow_casters = {"r_shadow_casters", "1", CVAR_ARCHIVE};

#ifndef USE_GLES

/* The three programs are RPROG_SHADOW (sshadow_vert + sshadow_frag),
 * RPROG_SHADOW_DEBUG (sshadowdbg_vert + sshadowdbg_frag) and
 * RPROG_SHADOW_ALIAS (sshadow_alias_vert + sshadow_frag), built by id
 * (gl_shader.c's rprog_defs). */

#ifndef GL_TEXTURE_CUBE_MAP_ARRAY
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16		0x81A5
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE		0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC		0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE	0x884E
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL		0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL		0x813D
#endif
#ifndef GL_MAX_CUBE_MAP_TEXTURE_SIZE
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE	0x851C
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE		0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING		0x8CA6
#endif
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST			0x0C11
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX			0x0C10
#endif
#ifndef GL_DEPTH
#define GL_DEPTH			0x1801
#endif

/* A map is kept while its light stays within this many units of the origin it
 * was drawn from.  The lookup reads from that origin, not the light's. */
#define SH_SLOP		8.0f
/* Radius band: a map is drawn for the light's radius rounded up to SH_RSTEP
 * plus SH_RSTEP of headroom, and kept while the radius stays within
 * SH_RBAND below what it was drawn for -- so a flickering radius does not
 * redraw it every frame. */
#define SH_RSTEP	32.0f
#define SH_RBAND	96.0f
/* An over-budget light keeps its stale map only while within this many units of
 * where it was drawn: further, and the map is some other place's occlusion
 * (a cl_dlights[] slot reused by a new key-0 light, a missile), so the light
 * goes unshadowed until the budget reaches it. */
#define SH_STALE	32.0f
/* Constant part of the receiver bias, in units (dlshadow.inc). */
#define SH_BIAS		1.0f
/* Constant part of an alias caster's push along the light ray, in units
 * (sshadow_alias_vert); its per-distance part is the receivers' one. */
#define SH_ALIAS_PUSH	2.0f

/* GL's cube-map face selection (the GL 4.6 core spec, table 8.19): for a
 * direction d, the face whose major axis ma is largest, and within it
 * s = (sc / |ma| + 1) / 2, t = (tc / |ma| + 1) / 2.  Each row is that face's
 * sc, tc and ma as axes of d, in GL_TEXTURE_CUBE_MAP_POSITIVE_X + i order --
 * the layer-face order of a cube-map array.  Drawing a face with clip
 * x = sc.d, y = tc.d, w = ma.d puts every texel where the sampler will look
 * for it.  tools/test_shadow_cube_faces.py checks this table against the spec's.
 *
 * THE WINDING.  Every row triple has determinant -1, and so does the main
 * view's (x, y, w) -- a GL camera looks down -z -- so a face of this table is
 * an ordinary camera, not a mirror: a triangle facing the light comes out with
 * the winding a triangle facing the main camera does.  The main view keeps
 * those by culling GL_FRONT (R_SetupGL, default CCW front face); this pass wants
 * the faces turned AWAY from the light, so it culls GL_BACK.  The test checks
 * that by projecting a triangle through both, so the sign is the engine's
 * answer rather than an argument. */
static const signed char sh_faceaxes[6][3][3] = {
	/*   sc            tc            ma         */
	{ {  0, 0,-1 }, {  0,-1, 0 }, {  1, 0, 0 } },	/* +X */
	{ {  0, 0, 1 }, {  0,-1, 0 }, { -1, 0, 0 } },	/* -X */
	{ {  1, 0, 0 }, {  0, 0, 1 }, {  0, 1, 0 } },	/* +Y */
	{ {  1, 0, 0 }, {  0, 0,-1 }, {  0,-1, 0 } },	/* -Y */
	{ {  1, 0, 0 }, {  0,-1, 0 }, {  0, 0, 1 } },	/* +Z */
	{ { -1, 0, 0 }, {  0,-1, 0 }, {  0, 0,-1 } },	/* -Z */
};
#define SH_CULL_FACE	GL_BACK
/* Clip z = w - SH_ZNEAR_W: never read (the depth is gl_FragDepth), it only
 * has to keep -w <= z <= w (or 0 <= z <= w under glClipControl's zero-to-one,
 * which reversed-Z leaves set) for everything more than a unit from the light. */
#define SH_ZNEAR_W	1.0f

/* Mirrors the std140 ShadowPass block in engine/shaders/sshadow_vert.glsl and
 * sshadow_alias_vert.glsl, one block declared twice. */
typedef struct {
	float	vp[16];		/* light-relative -> clip, one cube face */
	float	model[16];	/* model -> world, minus the light origin */
	float	light[4];	/* x 1 / radius; alias: y push, z push per unit, w pose 0 weight */
	int	pose[4];	/* alias: x pose 0 base, y pose 1 base, z PV_* format */
} r_shadowpass_t;		/* 160 bytes */
COMPILE_TIME_ASSERT(shadowpass_size, sizeof(r_shadowpass_t) == 160);

/* Mirrors the std140 DlShadow block in engine/shaders/dlshadow.inc. */
typedef struct {
	int	mask[4];			/* bit i & 31 of word i >> 5: light i has a map */
	int	layers[4];			/* 4-bit layer of the k-th mapped light, 8 per word */
	float	params[4];			/* x any map, y bias in units, z bias per unit distance */
	float	origin[SHADOW_MAX_MAPS][4];	/* per layer: xyz drawn from, w 1 / radius */
} r_dlshadow_t;					/* 304 bytes */
COMPILE_TIME_ASSERT(dlshadow_size, sizeof(r_dlshadow_t) == 304);

typedef struct {
	qboolean	valid;
	int		dlight;		/* cl_dlights[] slot it was drawn for */
	int		key;		/* and that slot's key then */
	vec3_t		origin;		/* drawn from */
	float		radius;		/* drawn with */
	unsigned int	enthash;	/* brush entities in range, then */
	int		frame;		/* r_framecount it was last assigned */
	int		acasts;		/* alias models drawn into it (r_shadowinfo) */
} shslot_t;

typedef struct {
	int	light;		/* index in the grid's list: the shader's i */
	int	dlight;		/* cl_dlights[] slot */
	float	score;
	int	slot;		/* -1 none */
} shcand_t;

static GLuint		sh_prog, sh_dbgprog, sh_dbgvao;
static GLuint		sh_aprog;		/* alias casters; 0 if it failed to build */
static GLuint		sh_tex, sh_ubo;
/* What DLSHADOW_TMU holds on a frame with no maps (#526); see SH_NoneTexture. */
static GLuint		sh_none;
/* One framebuffer per layer-face, attached once at allocation, so a face is a
 * bind rather than an attachment change.  (Measured the same as re-pointing
 * one framebuffer on the P4; kept as the shape drivers expect.) */
static GLuint		sh_fbos[SHADOW_MAX_MAPS * 6];
static int		sh_res, sh_maps;	/* the texture as allocated */
static int		sh_filter = -1;		/* the filter it last got */
static qboolean		sh_failed;		/* programs or FBO failed; never retry */
static shslot_t		sh_slots[SHADOW_MAX_MAPS];
static float		sh_facevp[6][16];
static r_dlshadow_t	sh_block;
static int		sh_frame_updates, sh_frame_maps, sh_frame_acasts;	/* r_shadowinfo */

/* The map's surface table: per world-model surface, its box (6 floats), and
 * the scratch the caster lists are built in.  sh_counts / sh_offsets hold the
 * world runs of every face of every map drawn this frame end to end
 * (sh_runcap entries, grown a map's worth -- 6 * sh_nsurf -- at a time). */
static qmodel_t		*sh_model;
static int		sh_nsurf;
static float		*sh_sbox;
static int		*sh_list;
static byte		*sh_smask;	/* per surface, the faces of the current map it reaches */
static GLsizei		*sh_counts;
static const void	**sh_offsets;
static int		sh_nlist, sh_runcap;

/* This frame's maps to draw, collected first and drawn in one burst, so the
 * CPU's caster walks do not leave the GPU idle between faces (which a GPU
 * timestamp around the pass would also count).  Brush-entity casters are
 * capped per frame; one past the cap is left out of that frame's maps, not
 * half drawn. */
#define SH_MAX_ENTS	256
#define SH_ENT_RUNS	1024
typedef struct {
	int	slot;
	vec3_t	org;
	float	radius;
	int	wfirst[6], wcount[6];	/* each face's world runs, in sh_counts / sh_offsets */
	int	efirst, ecount;		/* brush entities, in sh_ents */
	int	afirst, acount;		/* alias casters, in sh_acasts */
} shdraw_t;
typedef struct {
	float	model[16];		/* entity -> world */
	int	first, runs;		/* in sh_ecounts / sh_eoffsets */
	int	faces;			/* the faces its box reaches */
} shent_t;
static shdraw_t		sh_draws[SHADOW_MAX_MAPS];
static int		sh_ndraws;
static shent_t		sh_ents[SH_MAX_ENTS];
static int		sh_nents;
static GLsizei		sh_ecounts[SH_ENT_RUNS];
static const void	*sh_eoffsets[SH_ENT_RUNS];
static int		sh_neruns;

/* The alias models that may cast this frame (r_shadow_casters 2): those of
 * cl_visedicts the view would draw opaque whose bounding sphere reaches a
 * chosen light, posed on first use, once a frame -- R_AliasShadowCaster's
 * matrix and pose pair, and a hash of both for the cache.  One past the cap
 * leaves every map drawn this frame incomplete, never quietly short. */
#define SH_MAX_ALIAS	2048	/* ~400 KB; one past it leaves every map of the frame incomplete */
#define SH_MAX_ACASTS	4096
typedef struct {
	entity_t	*ent;
	int		num;		/* cl_entities index, -1 if not one: matched against a light's key */
	vec3_t		mins, maxs;	/* the view's cull box, scaled: a light inside it is the model's own */
	vec3_t		center;
	float		radius;		/* bounding sphere, any rotation */
	qboolean	posed, ok;
	unsigned int	hash;
	r_aliascaster_t	c;
} shalias_t;
typedef struct {
	int	alias;			/* in sh_alias */
	int	faces;
} shacast_t;
static shalias_t	sh_alias[SH_MAX_ALIAS];
static int		sh_nalias;
static qboolean		sh_alias_full;
static shacast_t	sh_acasts[SH_MAX_ACASTS];
static int		sh_nacasts;

extern GLuint		world_vao;

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */

static void SH_BuildFaceMatrices (void)
{
	int	f, c;

	for (f = 0; f < 6; f++)
	{
		float *m = sh_facevp[f];
		memset (m, 0, sizeof(sh_facevp[f]));
		for (c = 0; c < 3; c++)	/* column-major: m[col * 4 + row] */
		{
			m[c * 4 + 0] = sh_faceaxes[f][0][c];
			m[c * 4 + 1] = sh_faceaxes[f][1][c];
			m[c * 4 + 2] = sh_faceaxes[f][2][c];
			m[c * 4 + 3] = sh_faceaxes[f][2][c];
		}
		m[14] = -SH_ZNEAR_W;
	}
}

static GLuint SH_LinkProgram (rprog_id_t id)
{
	GLuint p = R_Program_Create (id);
	if (!p)
		Con_Printf ("Shadow maps: the %s program failed to build\n", R_Program_Name (id));
	return p;
}

/* Every way the programs, texture or framebuffers can fail ends here: nothing
 * half-built is left for the next frame to run with, and sh_failed keeps it
 * from being retried (and reported) every frame until vid_restart. */
static qboolean SH_Fail (void)
{
	R_PipelineForgetProgram ();
	if (sh_prog) { glDeleteProgram_fp (sh_prog); sh_prog = 0; }
	if (sh_dbgprog) { glDeleteProgram_fp (sh_dbgprog); sh_dbgprog = 0; }
	if (sh_aprog) { glDeleteProgram_fp (sh_aprog); sh_aprog = 0; }
	sh_failed = true;
	return false;
}

static qboolean SH_BuildPrograms (void)
{
	GLuint	blk;
	GLint	size = 0, loc;

	if (sh_failed)
		return false;
	if (sh_prog)
		return true;
	if (!glFramebufferTextureLayer_fp || !glMultiDrawElements_fp || !glTexImage3D_fp ||
	    !glClearBufferfv_fp || !glDrawBuffers_fp || !glGetUniformBlockIndex_fp)
	{
		Con_Printf ("Shadow maps: disabled, the driver lacks an entry point they need\n");
		return SH_Fail ();
	}

	sh_prog = SH_LinkProgram (RPROG_SHADOW);
	sh_dbgprog = SH_LinkProgram (RPROG_SHADOW_DEBUG);
	if (!sh_prog || !sh_dbgprog)
		return SH_Fail ();
	blk = glGetUniformBlockIndex_fp (sh_prog, "ShadowPass");
	if (blk == GL_INVALID_INDEX)
	{
		Con_Printf ("Shadow maps: disabled, no ShadowPass block in the program\n");
		return SH_Fail ();
	}
	glGetActiveUniformBlockiv_fp (sh_prog, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
	if (size != (GLint)sizeof(r_shadowpass_t))
		Con_Printf ("WARNING: uniform block ShadowPass is %d bytes on this driver, "
			    "C mirror is %d -- shadow maps will be wrong\n",
			    (int)size, (int)sizeof(r_shadowpass_t));
	glUniformBlockBinding_fp (sh_prog, blk, UBO_BINDING_SHADOW_PASS);

	/* The alias casters' program is an extra, not a requirement: without it
	 * the maps still hold the world, and r_shadow_casters 2 reads as 1. */
	sh_aprog = SH_LinkProgram (RPROG_SHADOW_ALIAS);
	if (sh_aprog)
	{
		blk = glGetUniformBlockIndex_fp (sh_aprog, "ShadowPass");
		size = 0;
		if (blk != GL_INVALID_INDEX)
			glGetActiveUniformBlockiv_fp (sh_aprog, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
		if (blk == GL_INVALID_INDEX || size != (GLint)sizeof(r_shadowpass_t))
		{
			Con_Printf ("Shadow maps: alias casters disabled, their ShadowPass block "
				    "is %d bytes here, C mirror is %d\n", (int)size, (int)sizeof(r_shadowpass_t));
			R_PipelineForgetProgram ();
			glDeleteProgram_fp (sh_aprog);
			sh_aprog = 0;
		}
		else
			glUniformBlockBinding_fp (sh_aprog, blk, UBO_BINDING_SHADOW_PASS);
	}

	R_UseProgram (sh_dbgprog);
	loc = R_ProgramSamplerLoc (sh_dbgprog, "u_shadowdbg");
	R_ProgramSamplerUnit (sh_dbgprog, loc, "u_shadowdbg", DLSHADOW_TMU);
	R_UseProgram (0);
	glGenVertexArrays_fp (1, &sh_dbgvao);

	SH_BuildFaceMatrices ();
	return true;
}

static void SH_FreeTexture (void)
{
	if (sh_maps > 0 && sh_fbos[0])
		glDeleteFramebuffers_fp (sh_maps * 6, sh_fbos);
	memset (sh_fbos, 0, sizeof(sh_fbos));
	if (sh_tex)
		R_DeleteTextures (1, &sh_tex);
	sh_tex = 0;
	sh_res = sh_maps = 0;
	sh_filter = -1;
	memset (sh_slots, 0, sizeof(sh_slots));
}

/* (Re)allocates the array for r_shadow_resolution and r_shadow_maxlights, and
 * keeps its filter in step with r_shadow_filter.  False if there is to be no
 * array this frame. */
static qboolean SH_EnsureTexture (void)
{
	GLint	maxsize = 0, prevfbo = 0;
	GLenum	status = GL_FRAMEBUFFER_COMPLETE, none = GL_NONE;
	int	i, res = r_shadow_resolution.integer;
	int	maps = r_shadow_maxlights.integer;
	int	filter = r_shadow_filter.integer ? 1 : 0;

	if (maps > SHADOW_MAX_MAPS)
		maps = SHADOW_MAX_MAPS;
	if (maps < 1)
		return false;
	glGetIntegerv_fp (GL_MAX_CUBE_MAP_TEXTURE_SIZE, &maxsize);
	if (res < 16)
		res = 16;
	if (res > 1024)
		res = 1024;
	if (maxsize > 0 && res > maxsize)
		res = maxsize;

	if (sh_tex && (res != sh_res || maps != sh_maps))
		SH_FreeTexture ();
	if (!sh_tex)
	{
		R_GenTextures (1, &sh_tex);
		R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);
		R_TexImage3D (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, 0, GL_DEPTH_COMPONENT16,
				 res, res, maps * 6, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL);
		R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_BASE_LEVEL, 0);
		R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_MAX_LEVEL, 0);
		R_TexSetWrap (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_CLAMP_TO_EDGE);
		R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
		/* Off unit 0 again: the passes render into this array, and nothing
		 * should hold it on a unit but DLSHADOW_TMU when it is sampled. */
		R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, 0);
		/* PCF's 2x2 footprint across a face edge: without this it clamps
		 * inside the face and draws faint lines along the cube's twelve
		 * edges.  Context-wide in GL; it only ever improves cube sampling,
		 * and nothing enables it before a map is allocated, so
		 * r_dlight_occlusion 0 and 1 are untouched. */
		glEnable_fp (GL_TEXTURE_CUBE_MAP_SEAMLESS);
		sh_res = res;
		sh_maps = maps;
		sh_filter = -1;

		/* Layer-face i of the array is framebuffer i's only attachment:
		 * depth, with the draw buffer off. */
		glGetIntegerv_fp (GL_FRAMEBUFFER_BINDING, &prevfbo);
		glGenFramebuffers_fp (maps * 6, sh_fbos);
		for (i = 0; i < maps * 6 && status == GL_FRAMEBUFFER_COMPLETE; i++)
		{
			glBindFramebuffer_fp (GL_FRAMEBUFFER, sh_fbos[i]);
			glDrawBuffers_fp (1, &none);
			glFramebufferTextureLayer_fp (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, sh_tex, 0, i);
			status = glCheckFramebufferStatus_fp (GL_FRAMEBUFFER);
		}
		glBindFramebuffer_fp (GL_FRAMEBUFFER, (GLuint)prevfbo);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_Printf ("Shadow maps: disabled, the %dx%d depth cube array is not "
				    "renderable here (0x%x)\n", res, res, (unsigned int)status);
			SH_FreeTexture ();
			return SH_Fail ();
		}
		if (developer.integer >= 2)
			Con_SafePrintf ("Shadow maps: %d cubes of %dx%d\n", maps, res, res);
	}
	if (filter != sh_filter)
	{
		GLint f = filter ? GL_LINEAR : GL_NEAREST;
		R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);
		R_TexSetFilter (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, f, f);
		R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, 0);
		sh_filter = filter;
	}
	return true;
}

/* A 1x1 depth cube array with the compare on, for DLSHADOW_TMU whenever the
 * frame has no maps (#526).  The world and alias programs declare the
 * samplerCubeArrayShadow whatever the frame holds, and GL validates a
 * sampler against what is bound to its unit at every draw, read or not.
 * Without this the unit held texture 0 -- not a depth format, no compare --
 * on every frame before the first shadow map, so on every frame of
 * r_dynamic 0, r_dlight_occlusion 0 or 1, r_shadow_maxlights 0 or a scene
 * with no dlight yet: one undefined-behaviour report per world and alias
 * draw.  Never sampled: u_dlsparams.x is 0 on such a frame, so the shader
 * takes no lookup.  Its texels are 1.0 all the same, so a gate that ever
 * let a lookup through would read "lit" rather than noise.  GL only: -gpu
 * fills an empty slot with its own shadow placeholder (rg_resource.c). */
static GLuint SH_NoneTexture (void)
{
	/* Six texels, padded for a 4-byte unpack row alignment: every one 1.0
	 * whichever stride the upload reads the layers at. */
	static const unsigned short far_depth[6 * 2] = {
		0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
		0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
	};

	if (sh_none)
		return sh_none;
	R_GenTextures (1, &sh_none);
	R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none);
	R_TexImage3D (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, 0, GL_DEPTH_COMPONENT16,
			 1, 1, 6, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, far_depth);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_TEXTURE_BASE_LEVEL, 0);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_TEXTURE_MAX_LEVEL, 0);
	R_TexSetFilter (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_CLAMP_TO_EDGE);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_none, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, 0);
	return sh_none;
}

/* ------------------------------------------------------------------ */
/* The map's surface table                                             */
/* ------------------------------------------------------------------ */

static void SH_FreeSurfaces (void)
{
	free (sh_sbox);
	free (sh_list);
	free (sh_smask);
	free (sh_counts);
	free ((void *)sh_offsets);
	sh_sbox = NULL;
	sh_list = NULL;
	sh_smask = NULL;
	sh_counts = NULL;
	sh_offsets = NULL;
	sh_model = NULL;
	sh_nsurf = 0;
	sh_runcap = 0;
}

static qboolean SH_ReserveRuns (int used);

static qboolean SH_EnsureSurfaces (void)
{
	qmodel_t	*m = cl.worldmodel;
	int		i, j;

	if (sh_model == m && sh_sbox)
		return true;
	SH_FreeSurfaces ();
	if (!m || m->numsurfaces <= 0)
		return false;

	sh_sbox = (float *) malloc (m->numsurfaces * 6 * sizeof(float));
	sh_list = (int *) malloc (m->numsurfaces * sizeof(int));
	sh_smask = (byte *) malloc (m->numsurfaces);
	sh_counts = (GLsizei *) malloc (m->numsurfaces * sizeof(GLsizei));
	sh_offsets = (const void **) malloc (m->numsurfaces * sizeof(void *));
	if (!sh_sbox || !sh_list || !sh_smask || !sh_counts || !sh_offsets)
	{
		SH_FreeSurfaces ();
		return false;
	}
	for (i = 0; i < m->numsurfaces; i++)
	{
		msurface_t	*s = &m->surfaces[i];
		float		*b = &sh_sbox[i * 6];

		b[0] = b[1] = b[2] = 1e30f;
		b[3] = b[4] = b[5] = -1e30f;
		for (j = 0; j < s->numedges; j++)
		{
			int		e = m->surfedges[s->firstedge + j];
			const float	*v = (e >= 0) ? m->vertexes[m->edges[e].v[0]].position
						      : m->vertexes[m->edges[-e].v[1]].position;
			int		c;
			for (c = 0; c < 3; c++)
			{
				if (v[c] < b[c]) b[c] = v[c];
				if (v[c] > b[3 + c]) b[3 + c] = v[c];
			}
		}
	}
	sh_model = m;
	sh_nsurf = m->numsurfaces;
	sh_runcap = m->numsurfaces;
	return SH_ReserveRuns (0);
}

/* Room for one more map's world runs, six faces of them, after `used`. */
static qboolean SH_ReserveRuns (int used)
{
	GLsizei		*c;
	const void	**o;
	int		cap, need = used + 6 * sh_nsurf;

	if (need <= sh_runcap)
		return true;
	cap = need > 2 * sh_runcap ? need : 2 * sh_runcap;
	c = (GLsizei *) realloc (sh_counts, cap * sizeof(GLsizei));
	if (c)
		sh_counts = c;
	o = (const void **) realloc ((void *)sh_offsets, cap * sizeof(void *));
	if (o)
		sh_offsets = o;
	if (!c || !o)
		return false;
	sh_runcap = cap;
	return true;
}

/* Does the sphere touch the box? */
static qboolean SH_SphereBox (const vec3_t org, float radius, const float *mins, const float *maxs)
{
	float	d2 = 0.0f, d;
	int	c;

	for (c = 0; c < 3; c++)
	{
		if (org[c] < mins[c])		d = mins[c] - org[c];
		else if (org[c] > maxs[c])	d = org[c] - maxs[c];
		else				continue;
		d2 += d * d;
	}
	return d2 <= radius * radius;
}

/* Which of the six faces a box (world space) can reach from a light at org:
 * face f's view is the pyramid ma.d >= |sc.d|, |tc.d|, so a box with every
 * corner behind one of its five planes cannot reach it.  Conservative -- a
 * box beside the pyramid's edge can pass all five -- which only costs a
 * clipped draw.  Drawing every caster into every face is what the faces
 * otherwise cost: six times the vertices and the multi-draw's per-run work. */
static int SH_FaceMask (const vec3_t org, const float *mins, const float *maxs)
{
	float	lo[3], hi[3];
	int	f, p, c, mask = 0;

	for (c = 0; c < 3; c++)
	{
		lo[c] = mins[c] - org[c];
		hi[c] = maxs[c] - org[c];
	}
	for (f = 0; f < 6; f++)
	{
		const signed char *sc = sh_faceaxes[f][0], *tc = sh_faceaxes[f][1], *ma = sh_faceaxes[f][2];
		for (p = 0; p < 5; p++)
		{
			float n[3], best = 0.0f;
			for (c = 0; c < 3; c++)
			{
				n[c] = ma[c];
				if (p == 1) n[c] -= sc[c];
				if (p == 2) n[c] += sc[c];
				if (p == 3) n[c] -= tc[c];
				if (p == 4) n[c] += tc[c];
				best += n[c] * (n[c] > 0.0f ? hi[c] : lo[c]);
			}
			if (best < 0.0f)
				break;
		}
		if (p == 5)
			mask |= 1 << f;
	}
	return mask;
}

/* A surface that can cast: in the world VBO and opaque to light. */
static qboolean SH_Caster (const msurface_t *s)
{
	return s->vbo_numtris > 0 &&
	       !(s->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWFENCE | SURF_TRANSLUCENT));
}

static void SH_AddSurface (int idx, int faces)
{
	sh_smask[idx] = (byte)faces;
	sh_list[sh_nlist++] = idx;
}

/* The world surfaces within `radius` of org whose front faces away from it.
 * R_MarkLights' walk; a node's surfaces lie on its plane, so `dist` is also
 * each one's distance from the light. */
static void SH_MarkWorld (mnode_t *node, const vec3_t org, float radius)
{
	while (node->contents >= 0)
	{
		mplane_t	*pl = node->plane;
		float		dist = DotProduct (org, pl->normal) - pl->dist;
		unsigned int	k;

		if (dist > radius)
		{
			node = node->children[0];
			continue;
		}
		if (dist < -radius)
		{
			node = node->children[1];
			continue;
		}
		for (k = 0; k < node->numsurfaces; k++)
		{
			int		idx = (int)node->firstsurface + (int)k, faces;
			msurface_t	*s = &sh_model->surfaces[idx];
			float		front = (s->flags & SURF_PLANEBACK) ? -dist : dist;

			if (front >= 0.0f || !SH_Caster (s))
				continue;
			if (!SH_SphereBox (org, radius, &sh_sbox[idx * 6], &sh_sbox[idx * 6 + 3]))
				continue;
			faces = SH_FaceMask (org, &sh_sbox[idx * 6], &sh_sbox[idx * 6 + 3]);
			if (faces)
				SH_AddSurface (idx, faces);
		}
		SH_MarkWorld (node->children[0], org, radius);
		node = node->children[1];
	}
}

static int SH_CompareFirstIndex (const void *a, const void *b)
{
	int fa = sh_model->surfaces[*(const int *)a].vbo_firstindex;
	int fb = sh_model->surfaces[*(const int *)b].vbo_firstindex;
	return (fa > fb) - (fa < fb);
}

/* The surfaces of sorted sh_list[0..sh_nlist) that reach face f -> merged
 * index runs in sh_counts / sh_offsets from `base`; returns the number of
 * runs. */
static int SH_BuildRuns (int base, int f)
{
	int	i, n = base, first = 0, len = 0;

	for (i = 0; i < sh_nlist; i++)
	{
		const msurface_t *s = &sh_model->surfaces[sh_list[i]];
		if (!(sh_smask[sh_list[i]] & (1 << f)))
			continue;
		if (len && s->vbo_firstindex == first + len)
		{
			len += s->vbo_numtris * 3;
			continue;
		}
		if (len)
		{
			sh_counts[n] = len;
			sh_offsets[n] = (const void *)((intptr_t)first * (intptr_t)sizeof(unsigned int));
			n++;
		}
		first = s->vbo_firstindex;
		len = s->vbo_numtris * 3;
	}
	if (!len)
		return 0;
	sh_counts[n] = len;
	sh_offsets[n] = (const void *)((intptr_t)first * (intptr_t)sizeof(unsigned int));
	return n + 1 - base;
}

/* ------------------------------------------------------------------ */
/* Brush entities                                                      */
/* ------------------------------------------------------------------ */

/* An opaque brush entity whose box the sphere touches; the box, in world
 * space, goes to mins / maxs. */
static qboolean SH_EntityCasts (entity_t *e, const vec3_t org, float radius, vec3_t mins, vec3_t maxs)
{
	brush_render_state_t	st;
	qmodel_t		*m = e->model;
	int			c;

	if (!m || m->type != mod_brush || m == cl.worldmodel || m->nummodelsurfaces <= 0)
		return false;
	if (m->flags & EF_TRANSPARENT)
		return false;
	st = R_BrushEntityRenderState (e, BRUSH_SURFACE_REGULAR, 1.0f);
	if (!st.visible || st.translucent)
		return false;
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		for (c = 0; c < 3; c++)
		{
			mins[c] = e->origin[c] - m->radius;
			maxs[c] = e->origin[c] + m->radius;
		}
	}
	else
	{
		VectorAdd (e->origin, m->mins, mins);
		VectorAdd (e->origin, m->maxs, maxs);
	}
	return SH_SphereBox (org, radius, mins, maxs);
}

/* ------------------------------------------------------------------ */
/* Alias models (P6, r_shadow_casters 2)                               */
/* ------------------------------------------------------------------ */

static float SH_MapRadius (float radius);

static unsigned int SH_Fnv (unsigned int h, const void *p, size_t n)
{
	const byte	*b = (const byte *)p;
	size_t		i;

	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static qboolean SH_AliasOn (void)
{
	return r_shadow_casters.integer >= 2 && sh_aprog && r_drawentities.integer;
}

/* An alias entity the view draws opaque, which is what can cast: the tests
 * R_DrawEntitiesOnList and R_CollectAliasInstance make before drawing one --
 * not translucent, inside r_entdist -- minus the frustum cull, since a model
 * behind the camera still shadows what is in front of it.  Never the
 * viewmodel: it is drawn in its own depth range, with FOV compensation, at a
 * place that is not where it would block anything.  (The player's own model is
 * not in cl_visedicts outside chase view, so it casts only there.) */
static qboolean SH_AliasEligible (const entity_t *e)
{
	const qmodel_t	*m = e->model;

	if (!m || m->type != mod_alias || e == &cl.viewent)
		return false;
	/* a Shadows of Chaos menu panel is UI, pinned in front of the eye */
	if (R_IsMenuPanel ((entity_t *)e))
		return false;
	if ((e->drawflags & DRF_TRANSLUCENT) ||
	    (m->flags & (EF_TRANSPARENT | EF_HOLEY | EF_SPECIAL_TRANS)) ||
	    (e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha)))
		return false;
	if (r_entdist.value > 0 && e != &cl_entities[cl.viewentity])
	{
		vec3_t	d;
		VectorSubtract (e->origin, r_origin, d);
		if (DotProduct (d, d) > r_entdist.value * r_entdist.value)
			return false;
	}
	return true;
}

/* Does alias caster a cast into a map of the light (key) at org?  Its sphere
 * reaches the light's, and it is not the light's own model: not the entity
 * the light is keyed to (the torch artifact's light is keyed to the player, a
 * missile's to the missile, a torch flame's to the flame), nor any model whose
 * box holds the light -- an effect's light keyed 0 at its model's origin.  A
 * light inside its own model would shadow its whole radius. */
static qboolean SH_AliasCasts (const shalias_t *a, const vec3_t org, float radius, int key)
{
	vec3_t	d;
	float	r = radius + a->radius;

	if (key > 0 && a->num == key)
		return false;
	if (org[0] >= a->mins[0] && org[0] <= a->maxs[0] &&
	    org[1] >= a->mins[1] && org[1] <= a->maxs[1] &&
	    org[2] >= a->mins[2] && org[2] <= a->maxs[2])
		return false;
	VectorSubtract (a->center, org, d);
	return DotProduct (d, d) <= r * r;
}

/* Posed on first use: R_AliasShadowCaster is the cost, so a model no map
 * reaches is never posed. */
static qboolean SH_AliasPosed (shalias_t *a)
{
	if (!a->posed)
	{
		a->posed = true;
		a->ok = R_AliasShadowCaster (a->ent, &a->c);
		if (a->ok)
		{
			unsigned int h = 2166136261u;
			h = SH_Fnv (h, &a->c.gm, sizeof(a->c.gm));
			/* A pickup turning on the clock would redraw its light's map
			 * every frame for as long as it lies there, and take the budget
			 * from lights that moved; its shadow keeps the angle it was drawn
			 * at until something else redraws the map. */
			if (a->c.spins)
				h = SH_Fnv (h, a->ent->origin, sizeof(vec3_t));
			else
				h = SH_Fnv (h, a->c.model, sizeof(a->c.model));
			h = SH_Fnv (h, &a->c.pose0, sizeof(a->c.pose0));
			h = SH_Fnv (h, &a->c.pose1, sizeof(a->c.pose1));
			h = SH_Fnv (h, &a->c.blend, sizeof(a->c.blend));
			a->hash = h;
		}
	}
	return a->ok;
}

/* This frame's candidates: every eligible alias entity whose sphere comes
 * within reach of one of the `n` lights.  The reach is padded by everything
 * the cache lets a map differ from its light by -- SH_SLOP of movement,
 * SH_RBAND of radius -- so SH_EntityHash about a map's drawn origin and radius
 * sees the same set the collection did. */
static void SH_GatherAlias (const shcand_t *cand, int n)
{
	int	i, j, c;

	sh_nalias = 0;
	sh_alias_full = false;
	if (!SH_AliasOn ())
		return;
	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t	*e = cl_visedicts[i];
		shalias_t	*a;
		float		s, r2 = 0.0f;
		vec3_t		lo, hi;

		if (!SH_AliasEligible (e))
			continue;
		s = (e->scale > 100) ? e->scale / 100.0f : 1.0f;
		for (c = 0; c < 3; c++)
		{
			float m = q_max (fabsf (e->model->mins[c]), fabsf (e->model->maxs[c])) * s;
			r2 += m * m;
			lo[c] = e->origin[c] + e->model->mins[c] * s;
			hi[c] = e->origin[c] + e->model->maxs[c] * s;
		}
		for (j = 0; j < n; j++)
		{
			float	org[3], radius, reach;
			int	dl;
			vec3_t	d;

			R_LightCluster_GetLight (cand[j].light, org, &radius, &dl);
			reach = SH_MapRadius (radius) + SH_RBAND + SH_SLOP + sqrtf (r2) + 8.0f;
			VectorSubtract (e->origin, org, d);
			if (DotProduct (d, d) <= reach * reach)
				break;
		}
		if (j == n)
			continue;
		if (sh_nalias >= SH_MAX_ALIAS)
		{
			sh_alias_full = true;
			break;
		}
		a = &sh_alias[sh_nalias++];
		memset (a, 0, sizeof(*a));
		a->ent = e;
		a->num = (e >= cl_entities && e < cl_entities + MAX_EDICTS) ? (int)(e - cl_entities) : -1;
		VectorCopy (lo, a->mins);
		VectorCopy (hi, a->maxs);
		VectorCopy (e->origin, a->center);
		/* + the bob R_AliasModelTransform gives a floating model */
		a->radius = sqrtf (r2) + 8.0f;
	}
}

/* What a map depends on beyond the world: which brush entities are in range
 * and where they stand, and under r_shadow_casters 2 which alias models and
 * in what pose -- a monster walking or animating in range redraws the map, a
 * barrel standing still does not. */
static unsigned int SH_EntityHash (const vec3_t org, float radius, int key)
{
	unsigned int	h = 2166136261u;
	int		i, k;
	vec3_t		mins, maxs;

	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t	*e = cl_visedicts[i];
		unsigned int	w[7];

		if (!SH_EntityCasts (e, org, radius, mins, maxs))
			continue;
		w[0] = (unsigned int)(uintptr_t)e->model;
		for (k = 0; k < 3; k++)
		{
			memcpy (&w[1 + k], &e->origin[k], sizeof(float));
			memcpy (&w[4 + k], &e->angles[k], sizeof(float));
		}
		for (k = 0; k < 7; k++)
			h = (h ^ w[k]) * 16777619u;
	}
	if (SH_AliasOn ())
	{
		h = (h ^ 0xa11a5u) * 16777619u;	/* r_shadow_casters 1 -> 2 redraws */
		for (i = 0; i < sh_nalias; i++)
		{
			shalias_t *a = &sh_alias[i];
			if (SH_AliasCasts (a, org, radius, key) && SH_AliasPosed (a))
				h = (h ^ a->hash) * 16777619u;
		}
	}
	return h;
}

/* The transform R_DrawBrushModel and the instanced brush path give an entity:
 * R_RotateForEntity with pitch and roll negated, as both of them do. */
static void SH_EntityMatrix (const entity_t *e, float *out)
{
	static entity_t	tmp;

	tmp = *e;
	tmp.angles[0] = -e->angles[0];
	tmp.angles[2] = -e->angles[2];
	GL_MatrixMode (GL_MAT_MODELVIEW);
	GL_PushMatrix ();
	GL_LoadIdentity ();
	R_RotateForEntity (&tmp);
	GL_GetModelview (out);
	GL_PopMatrix ();
}

/* ------------------------------------------------------------------ */
/* Drawing one map                                                     */
/* ------------------------------------------------------------------ */

static void SH_Pass (const r_shadowpass_t *pass)
{
	GLuint		buf;
	GLintptr	ofs;

	GL_Upload (GL_UNIFORM_BUFFER, pass, sizeof(*pass), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, UBO_BINDING_SHADOW_PASS, buf, ofs, sizeof(*pass));
}

/* Collect map `slot`'s casters for a light at org (keyed `key`): the world's
 * runs, each brush entity's, and under r_shadow_casters 2 each alias model's,
 * appended to this frame's lists.  False if there was no room; *complete false
 * if a caster in range was left out. */
static qboolean SH_CollectMap (int slot, const vec3_t org, float radius, int key, qboolean *complete)
{
	shdraw_t	*d;
	int		i, f, base;

	if (sh_ndraws >= SHADOW_MAX_MAPS)
		return false;
	base = sh_ndraws ? sh_draws[sh_ndraws - 1].wfirst[5] + sh_draws[sh_ndraws - 1].wcount[5] : 0;
	if (!SH_ReserveRuns (base))
		return false;
	d = &sh_draws[sh_ndraws++];
	d->slot = slot;
	VectorCopy (org, d->org);
	d->radius = radius;

	/* The world's caster runs, face by face. */
	sh_nlist = 0;
	SH_MarkWorld (sh_model->nodes, org, radius);
	qsort (sh_list, sh_nlist, sizeof(int), SH_CompareFirstIndex);
	for (f = 0; f < 6; f++)
	{
		d->wfirst[f] = base;
		d->wcount[f] = SH_BuildRuns (base, f);
		base += d->wcount[f];
	}

	/* Each entity's: its own surfaces, which are contiguous in the VBO in
	 * model order, merged the same way (a gap is a sky or liquid face, rare
	 * in a brush entity). */
	d->efirst = sh_nents;
	*complete = true;
	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t	*e = cl_visedicts[i];
		qmodel_t	*m;
		qboolean	full = false;
		vec3_t		emins, emaxs;
		int		k, first = 0, len = 0, runs = 0, faces;

		if (!SH_EntityCasts (e, org, radius, emins, emaxs))
			continue;
		faces = SH_FaceMask (org, emins, emaxs);
		if (!faces)
			continue;
		if (sh_nents >= SH_MAX_ENTS)
		{
			*complete = false;
			break;
		}
		m = e->model;
		for (k = 0; k < m->nummodelsurfaces; k++)
		{
			const msurface_t *s = &m->surfaces[m->firstmodelsurface + k];
			if (!SH_Caster (s))
				continue;
			if (len && s->vbo_firstindex == first + len)
			{
				len += s->vbo_numtris * 3;
				continue;
			}
			if (len)
			{
				if (sh_neruns + runs >= SH_ENT_RUNS)
				{
					full = true;
					break;
				}
				sh_ecounts[sh_neruns + runs] = len;
				sh_eoffsets[sh_neruns + runs] = (const void *)((intptr_t)first * (intptr_t)sizeof(unsigned int));
				runs++;
			}
			first = s->vbo_firstindex;
			len = s->vbo_numtris * 3;
		}
		if (full || (len && sh_neruns + runs >= SH_ENT_RUNS))
		{
			*complete = false;
			continue;
		}
		if (!len)
			continue;
		sh_ecounts[sh_neruns + runs] = len;
		sh_eoffsets[sh_neruns + runs] = (const void *)((intptr_t)first * (intptr_t)sizeof(unsigned int));
		runs++;
		sh_ents[sh_nents].first = sh_neruns;
		sh_ents[sh_nents].runs = runs;
		sh_ents[sh_nents].faces = faces;
		SH_EntityMatrix (e, sh_ents[sh_nents].model);
		sh_neruns += runs;
		sh_nents++;
	}
	d->ecount = sh_nents - d->efirst;

	/* The alias models'. */
	d->afirst = sh_nacasts;
	if (sh_alias_full)
		*complete = false;
	for (i = 0; i < sh_nalias; i++)
	{
		shalias_t	*a = &sh_alias[i];
		vec3_t		lo, hi;
		int		faces, c;

		if (!SH_AliasCasts (a, org, radius, key) || !SH_AliasPosed (a))
			continue;
		for (c = 0; c < 3; c++)
		{
			lo[c] = a->center[c] - a->radius;
			hi[c] = a->center[c] + a->radius;
		}
		faces = SH_FaceMask (org, lo, hi);
		if (!faces)
			continue;
		if (sh_nacasts >= SH_MAX_ACASTS)
		{
			*complete = false;
			break;
		}
		sh_acasts[sh_nacasts].alias = i;
		sh_acasts[sh_nacasts].faces = faces;
		sh_nacasts++;
	}
	d->acount = sh_nacasts - d->afirst;
	return true;
}

/* Face f's alias casters of map d, with the alias program, then the world
 * program and VAO back for the next face. */
static void SH_DrawAliasFace (const shdraw_t *d, int f, r_shadowpass_t *pass)
{
	float		t[16];
	int		i, drawn = 0;
	const float	*org = d->org;

	for (i = d->afirst; i < d->afirst + d->acount; i++)
	{
		const shalias_t		*a = &sh_alias[sh_acasts[i].alias];
		const r_aliascaster_t	*c = &a->c;

		if (!(sh_acasts[i].faces & (1 << f)))
			continue;
		if (!drawn++)
			R_UseProgram (sh_aprog);
		Mat4_Identity (t);
		Mat4_ApplyTranslation (t, -org[0], -org[1], -org[2]);
		Mat4_Multiply (t, c->model, pass->model);
		pass->light[1] = SH_ALIAS_PUSH;
		pass->light[2] = r_shadow_bias.value * 2.0f / (float)sh_res;
		pass->light[3] = c->blend;
		pass->pose[0] = c->pose0;
		pass->pose[1] = c->pose1;
		pass->pose[2] = c->format;
		SH_Pass (pass);
		if (c->format == PV_IQM)
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 4, c->gm->ssbo_bones);
		else if (c->format == PV_MD3)
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 3, c->gm->ssbo_pose_md3);
		else
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 1, c->gm->ssbo_pose);
		glBindVertexArray_fp (c->gm->vao);
		glDrawElements_fp (GL_TRIANGLES, c->gm->num_indices, GL_UNSIGNED_SHORT, NULL);
		sh_frame_acasts++;
	}
	pass->light[1] = pass->light[2] = pass->light[3] = 0.0f;
	pass->pose[0] = pass->pose[1] = pass->pose[2] = 0;
	if (drawn)
	{
		R_UseProgram (sh_prog);
		glBindVertexArray_fp (world_vao);
	}
}

/* Draw one collected map: six faces, the world's runs then each entity's. */
static void SH_DrawMap (const shdraw_t *d)
{
	r_shadowpass_t	pass;
	float		one = 1.0f, t[16];
	int		slot = d->slot, i, f;
	const float	*org = d->org;

	memset (&pass, 0, sizeof(pass));
	pass.light[0] = 1.0f / d->radius;
	for (f = 0; f < 6; f++)
	{
		glBindFramebuffer_fp (GL_FRAMEBUFFER, sh_fbos[slot * 6 + f]);
		glClearBufferfv_fp (GL_DEPTH, 0, &one);
		memcpy (pass.vp, sh_facevp[f], sizeof(pass.vp));
		if (d->wcount[f] > 0)
		{
			Mat4_Identity (pass.model);
			Mat4_ApplyTranslation (pass.model, -org[0], -org[1], -org[2]);
			SH_Pass (&pass);
			glMultiDrawElements_fp (GL_TRIANGLES, &sh_counts[d->wfirst[f]], GL_UNSIGNED_INT,
						&sh_offsets[d->wfirst[f]], d->wcount[f]);
		}
		for (i = d->efirst; i < d->efirst + d->ecount; i++)
		{
			if (!(sh_ents[i].faces & (1 << f)))
				continue;
			Mat4_Identity (t);
			Mat4_ApplyTranslation (t, -org[0], -org[1], -org[2]);
			Mat4_Multiply (t, sh_ents[i].model, pass.model);
			SH_Pass (&pass);
			glMultiDrawElements_fp (GL_TRIANGLES, &sh_ecounts[sh_ents[i].first], GL_UNSIGNED_INT,
						&sh_eoffsets[sh_ents[i].first], sh_ents[i].runs);
		}
		if (d->acount > 0)
			SH_DrawAliasFace (d, f, &pass);
	}
}

/* ------------------------------------------------------------------ */
/* Per frame                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	GLint		fbo, vp[4], scissor_box[4];
	qboolean	scissor, depth_test, depth_mask, cull, stencil, poly;
	GLenum		depth_func, cull_face;
	double		znear, zfar;
	float		poly_factor, poly_units;
} shstate_t;

static void SH_SaveState (shstate_t *s)
{
	glGetIntegerv_fp (GL_FRAMEBUFFER_BINDING, &s->fbo);
	glGetIntegerv_fp (GL_VIEWPORT, s->vp);
	s->scissor = glIsEnabled_fp (GL_SCISSOR_TEST) ? true : false;
	if (s->scissor)
		glGetIntegerv_fp (GL_SCISSOR_BOX, s->scissor_box);
	s->depth_test = R_GetDepthTest ();
	s->depth_mask = R_GetDepthMask ();
	s->depth_func = R_GetDepthFunc ();
	R_GetDepthRange (&s->znear, &s->zfar);
	s->cull = R_GetCull ();
	s->cull_face = R_GetCullFace ();
	s->stencil = R_GetStencilTest ();
	s->poly = R_GetPolygonOffsetFill (&s->poly_factor, &s->poly_units);
}

static void SH_RestoreState (const shstate_t *s)
{
	glBindVertexArray_fp (0);
	R_UseProgram (0);
	glBindFramebuffer_fp (GL_FRAMEBUFFER, (GLuint)s->fbo);
	glViewport_fp (s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
	/* The passes only disable the test and never move the box, so
	 * re-setting the saved box puts back exactly what was there. */
	if (s->scissor)
		GL_SetScissor (s->scissor_box[0], s->scissor_box[1],
			       s->scissor_box[2], s->scissor_box[3]);
	R_SetDepthTest (s->depth_test);
	R_SetDepthMask (s->depth_mask);
	if (s->depth_func != RP_ENUM_UNKNOWN)
		R_SetDepthFunc (s->depth_func);
	R_SetDepthRange (s->znear, s->zfar);
	R_SetCull (s->cull);
	if (s->cull_face != RP_ENUM_UNKNOWN)
		R_SetCullFace (s->cull_face);
	R_SetStencilTest (s->stencil);
	R_SetPolygonOffsetFill (s->poly, s->poly_factor, s->poly_units);
}

static void SH_PassState (void)
{
	glViewport_fp (0, 0, sh_res, sh_res);
	GL_ResetScissor ();
	R_SetDepthTest (true);
	R_SetDepthMask (true);
	R_SetDepthFunc (GL_LESS);
	R_SetDepthRange (0.0, 1.0);
	R_SetCull (true);
	R_SetCullFace (SH_CULL_FACE);
	R_SetStencilTest (false);
	R_SetPolygonOffsetFill (false, 0.0f, 0.0f);
	R_UseProgram (sh_prog);
	glBindVertexArray_fp (world_vao);
}

static int SH_CompareScore (const void *a, const void *b)
{
	float sa = ((const shcand_t *)a)->score, sb = ((const shcand_t *)b)->score;
	return (sa < sb) - (sa > sb);	/* descending */
}

static int SH_CompareLight (const void *a, const void *b)
{
	int la = ((const shcand_t *)a)->light, lb = ((const shcand_t *)b)->light;
	return (la > lb) - (la < lb);
}

static float SH_MapRadius (float radius)
{
	return (ceilf (radius / SH_RSTEP) + 1.0f) * SH_RSTEP;
}

/* Is slot s, assigned to cand's light, a map of it as it is now? */
static qboolean SH_Fresh (const shslot_t *s, const float *org, float radius, unsigned int hash)
{
	vec3_t d;

	VectorSubtract (org, s->origin, d);
	return DotProduct (d, d) <= SH_SLOP * SH_SLOP &&
	       radius <= s->radius && radius > s->radius - SH_RBAND &&
	       hash == s->enthash;
}

/* Fills sh_block for this frame, and collects the maps to (re)draw within the
 * budget into sh_draws for R_Shadow_Draw. */
static void SH_Select (void)
{
	static shcand_t	cand[MAX_DLIGHTS];
	qboolean	nocache, complete;
	int		n = 0, nl, i, s, take, budget;

	sh_ndraws = sh_nents = sh_neruns = sh_nacasts = 0;
	sh_nalias = 0;
	nl = R_LightCluster_NumLights ();
	for (i = 0; i < nl; i++)
	{
		float		org[3], radius, d, score;
		int		dlight, p;
		mleaf_t		*leaf;
		vec3_t		v;

		if (!R_LightCluster_GetLight (i, org, &radius, &dlight) || dlight < 0)
			continue;
		leaf = Mod_PointInLeaf (org, cl.worldmodel);
		if (leaf->contents == CONTENTS_SOLID || leaf->contents == CONTENTS_SKY)
			continue;
		VectorSubtract (org, r_refdef.vieworg, v);
		d = sqrtf (DotProduct (v, v));
		score = radius / (d > 16.0f ? d : 16.0f);
		for (p = 0; p < 4; p++)
			if (DotProduct (org, frustum[p].normal) - frustum[p].dist < -radius)
				break;
		if (p == 4)
			score += 1e6f;
		cand[n].light = i;
		cand[n].dlight = dlight;
		cand[n].score = score;
		cand[n].slot = -1;
		n++;
	}
	if (n == 0)
		return;
	qsort (cand, n, sizeof(cand[0]), SH_CompareScore);
	take = n < sh_maps ? n : sh_maps;

	/* First every chosen light finds the slot already drawn for it, so a
	 * redraw below can never take a slot a light kept this frame. */
	for (i = 0; i < take; i++)
	{
		const dlight_t *dl = &cl_dlights[cand[i].dlight];
		for (s = 0; s < sh_maps; s++)
		{
			if (sh_slots[s].valid && sh_slots[s].dlight == cand[i].dlight &&
			    sh_slots[s].key == dl->key && sh_slots[s].frame != r_framecount)
			{
				cand[i].slot = s;
				sh_slots[s].frame = r_framecount;
				break;
			}
		}
	}

	SH_GatherAlias (cand, take);

	nocache = !r_shadow_cache.integer;
	budget = r_shadow_updates.integer > 0 ? r_shadow_updates.integer : MAX_DLIGHTS;
	for (i = 0; i < take; i++)
	{
		float		org[3], radius, mapr;
		int		dlight;
		unsigned int	hash;
		shslot_t	*sl;

		R_LightCluster_GetLight (cand[i].light, org, &radius, &dlight);
		mapr = SH_MapRadius (radius);
		if (cand[i].slot >= 0)
		{
			vec3_t	moved;

			sl = &sh_slots[cand[i].slot];
			hash = SH_EntityHash (sl->origin, sl->radius, sl->key);
			if (!nocache && SH_Fresh (sl, org, radius, hash))
				continue;
			VectorSubtract (org, sl->origin, moved);
			if (budget <= 0 && DotProduct (moved, moved) > SH_STALE * SH_STALE)
				cand[i].slot = -1;	/* too far from where it was drawn */
		}
		if (budget <= 0)
			continue;	/* a nearby stale map if it has one, none if not */
		if (cand[i].slot < 0)
		{
			int best = -1;
			for (s = 0; s < sh_maps; s++)
			{
				if (sh_slots[s].frame == r_framecount && sh_slots[s].valid)
					continue;
				if (best < 0 || !sh_slots[s].valid ||
				    (sh_slots[best].valid && sh_slots[s].frame < sh_slots[best].frame))
					best = s;
				if (!sh_slots[s].valid)
					break;
			}
			if (best < 0)
				continue;
			cand[i].slot = best;
		}
		sl = &sh_slots[cand[i].slot];
		hash = SH_EntityHash (org, mapr, cl_dlights[cand[i].dlight].key);
		if (!SH_CollectMap (cand[i].slot, org, mapr, cl_dlights[cand[i].dlight].key, &complete))
		{
			cand[i].slot = -1;	/* out of list memory: unshadowed this frame */
			sl->valid = false;
			continue;
		}
		sl->valid = true;
		sl->acasts = sh_draws[sh_ndraws - 1].acount;
		sl->dlight = cand[i].dlight;
		sl->key = cl_dlights[cand[i].dlight].key;
		VectorCopy (org, sl->origin);
		sl->radius = mapr;
		/* A map that left a brush entity out (the per-frame caster caps)
		 * must not pass for complete: a hash no frame computes redraws it. */
		sl->enthash = complete ? hash : ~hash;
		sl->frame = r_framecount;
		budget--;
		sh_frame_updates++;
	}

	/* The block: mapped lights in ascending light index, which is the order
	 * the shader counts them in. */
	qsort (cand, take, sizeof(cand[0]), SH_CompareLight);
	for (i = 0, s = 0; i < take; i++)
	{
		int		l = cand[i].light, layer = cand[i].slot;
		shslot_t	*sl;

		if (layer < 0)
			continue;
		sl = &sh_slots[layer];
		if (!sl->valid)
			continue;
		sh_block.mask[l >> 5] |= (int)(1u << (l & 31));
		sh_block.layers[s >> 3] |= (int)((unsigned int)layer << (4 * (s & 7)));
		sh_block.origin[layer][0] = sl->origin[0];
		sh_block.origin[layer][1] = sl->origin[1];
		sh_block.origin[layer][2] = sl->origin[2];
		sh_block.origin[layer][3] = 1.0f / sl->radius;
		s++;
	}
	sh_frame_maps = s;
	if (s > 0)
	{
		sh_block.params[0] = 1.0f;
		sh_block.params[1] = SH_BIAS;
		sh_block.params[2] = r_shadow_bias.value * 2.0f / (float)sh_res;
	}
}

void R_Shadow_Prepare (void)
{
	memset (&sh_block, 0, sizeof(sh_block));
	sh_frame_updates = sh_frame_maps = sh_frame_acasts = 0;
	sh_ndraws = 0;

	/* Under -gpu the block is a mirror (R_Shadow_Init), not a buffer. */
	if (!sh_ubo && !SH_GPU ())
	{
		glGenBuffers_fp (1, &sh_ubo);
		GL_BindBuffer (GL_UNIFORM_BUFFER, sh_ubo);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(sh_block), &sh_block, GL_DYNAMIC_DRAW);
		GL_BindBuffer (GL_UNIFORM_BUFFER, 0);
	}

#ifdef USE_SDL_GPU
	/* The cube maps are P6 (#340).  The light cluster runs under -gpu
	 * since P5b, so R_LightCluster_Available no longer holds them off at
	 * the default r_dlight_occlusion 2: this does.  With no map drawn, the
	 * frame's block names none and every light reads as unshadowed, which
	 * is r_dlight_occlusion 1's picture. */
	if (R_GPU ())
		return;
#endif
	if (r_dlight_occlusion.integer >= 2 && !sh_failed && R_LightCluster_Available () &&
	    r_shadow_maxlights.integer > 0 && world_vao && cl.worldmodel &&
	    SH_BuildPrograms () && SH_EnsureSurfaces () && SH_EnsureTexture ())
	{
		SH_Select ();
	}
}

void R_Shadow_Draw (void)
{
	shstate_t	saved;
	int		i;

	if (sh_ndraws > 0)
	{
		SH_SaveState (&saved);
		SH_PassState ();
		for (i = 0; i < sh_ndraws; i++)
			SH_DrawMap (&sh_draws[i]);
		if (sh_frame_acasts > 0)
		{
			/* as GL_DrawAliasSkeletal and the instanced path leave them */
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 1, 0);
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 3, 0);
			GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, 4, 0);
		}
		SH_RestoreState (&saved);
		sh_ndraws = 0;
	}
	/* Every frame (#526).  Under -gpu, 0 when there is no map: the backend's
	 * own shadow placeholder fills the slot, with its D32 fallback. */
	R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY,
			     sh_frame_maps > 0 ? sh_tex : SH_GPU () ? 0 : SH_NoneTexture ());

#ifdef USE_SDL_GPU
	/* The same upload for the backend: every draw from here on pushes this
	 * frame's block, as GL's buffer would hold it. */
	if (R_GPU ())
	{
		RG_UniformBlockFlushed ("DlShadow");
		return;
	}
#endif
	GL_BindBuffer (GL_UNIFORM_BUFFER, sh_ubo);
	glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(sh_block), &sh_block, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_UNIFORM_BUFFER, 0);
	GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_DLSHADOW, sh_ubo);
}

void R_Shadow_InitProgram (GLuint program)
{
	GLuint	blk;
	GLint	loc, size = 0;
	static qboolean checked;

	if (!program)
		return;
#ifdef USE_SDL_GPU
	/* The block's slot is fixed in the SPIR-V and found by name at draw
	 * time; only the sampler's unit has to be told (#340 P2a). */
	if (R_GPU ())
	{
		loc = R_ProgramSamplerLoc (program, "u_dlshadowmap");
		R_ProgramSamplerUnit (program, loc, "u_dlshadowmap", DLSHADOW_TMU);
		return;
	}
#endif
	if (!glGetUniformBlockIndex_fp || !glUniformBlockBinding_fp)
		return;
	blk = glGetUniformBlockIndex_fp (program, "DlShadow");
	if (blk != GL_INVALID_INDEX)
	{
		glUniformBlockBinding_fp (program, blk, UBO_BINDING_DLSHADOW);
		if (!checked && glGetActiveUniformBlockiv_fp)
		{
			checked = true;
			glGetActiveUniformBlockiv_fp (program, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
			if (size != (GLint)sizeof(r_dlshadow_t))
				Con_Printf ("WARNING: uniform block DlShadow is %d bytes on this driver, "
					    "C mirror is %d -- dynamic-light shadows will be wrong\n",
					    (int)size, (int)sizeof(r_dlshadow_t));
		}
	}
	loc = R_ProgramSamplerLoc (program, "u_dlshadowmap");
	R_ProgramSamplerUnit (program, loc, "u_dlshadowmap", DLSHADOW_TMU);
}

void R_Shadow_DrawDebug (void)
{
	shstate_t	saved;
	qboolean	blend;
	int		w, h;

	if (!r_shadow_debug.integer || !sh_tex || !sh_dbgprog || !sh_slots[0].valid ||
	    r_dlight_occlusion.integer < 2)
		return;
	SH_SaveState (&saved);
	blend = R_GetBlend ();
	w = (saved.vp[2] / 2) & ~3;
	h = w * 3 / 4;
	if (h > saved.vp[3])
	{
		h = saved.vp[3] - saved.vp[3] % 3;
		w = h * 4 / 3;
	}
	glViewport_fp (saved.vp[0], saved.vp[1], w, h);
	GL_ResetScissor ();
	R_SetDepthTest (false);
	R_SetCull (false);
	R_SetBlend (false);
	R_SetStencilTest (false);
	R_UseProgram (sh_dbgprog);
	R_BindTextureTarget (DLSHADOW_TMU, GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);
	/* The compare mode is texture state, so it is set through unit 0 and
	 * reaches the DLSHADOW_TMU binding: same texture object. */
	R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glBindVertexArray_fp (sh_dbgvao);
	glDrawArrays_fp (GL_TRIANGLES, 0, 3);
	R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex);
	R_TexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, sh_tex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	R_BindTextureForEdit (GL_TEXTURE_CUBE_MAP_ARRAY, 0);
	R_SetBlend (blend);
	SH_RestoreState (&saved);
}

static void R_ShadowInfo_f (void)
{
	int s;

	Con_Printf ("--- r_shadowinfo (r_dlight_occlusion %d, %d cubes of %d, "
		    "this frame %d mapped, %d drawn, %d alias casts) ---\n",
		    r_dlight_occlusion.integer, sh_maps, sh_res, sh_frame_maps, sh_frame_updates,
		    sh_frame_acasts);
	for (s = 0; s < sh_maps; s++)
	{
		const shslot_t *sl = &sh_slots[s];
		if (!sl->valid)
			continue;
		Con_Printf ("slot %d: dlight %d key %d from=(%.0f,%.0f,%.0f) "
			    "radius %.0f age %d alias %d%s\n", s, sl->dlight, sl->key,
			    sl->origin[0], sl->origin[1], sl->origin[2], sl->radius,
			    r_framecount - sl->frame, sl->acasts,
			    (sh_block.origin[s][3] > 0.0f) ? " in use" : "");
	}
}

void R_Shadow_NewMap (void)
{
	memset (sh_slots, 0, sizeof(sh_slots));
	SH_FreeSurfaces ();
}

void R_Shadow_Shutdown (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (sh_prog) { glDeleteProgram_fp (sh_prog); sh_prog = 0; }
	if (sh_dbgprog) { glDeleteProgram_fp (sh_dbgprog); sh_dbgprog = 0; }
	if (sh_aprog) { glDeleteProgram_fp (sh_aprog); sh_aprog = 0; }
	if (sh_dbgvao) { glDeleteVertexArrays_fp (1, &sh_dbgvao); sh_dbgvao = 0; }
	if (sh_ubo) { GL_ForgetBuffer (sh_ubo); glDeleteBuffers_fp (1, &sh_ubo); sh_ubo = 0; }
	SH_FreeTexture ();
	if (sh_none)
		R_DeleteTextures (1, &sh_none);
	sh_none = 0;
	sh_failed = false;
}

#else	/* USE_GLES: no clustered path on this tier, so nothing to shadow */

void R_Shadow_Prepare (void) {}
void R_Shadow_Draw (void) {}
void R_Shadow_InitProgram (GLuint program) { (void)program; }
void R_Shadow_DrawDebug (void) {}
void R_Shadow_NewMap (void) {}
void R_Shadow_Shutdown (void) {}

#endif	/* !USE_GLES */

void R_Shadow_Init (void)
{
	Cvar_RegisterVariable (&r_shadow_resolution);
	Cvar_RegisterVariable (&r_shadow_maxlights);
	Cvar_RegisterVariable (&r_shadow_filter);
	Cvar_RegisterVariable (&r_shadow_updates);
	Cvar_RegisterVariable (&r_shadow_bias);
	Cvar_RegisterVariable (&r_shadow_cache);
	Cvar_RegisterVariable (&r_shadow_debug);
	Cvar_RegisterVariable (&r_shadow_casters);
#ifndef USE_GLES
	Cmd_AddCommand ("r_shadowinfo", R_ShadowInfo_f);
#endif
#ifdef USE_SDL_GPU
	/* After VID_Init, so R_GPU () is settled (-gpu is restart-only).  The
	 * world and alias stages declare DlShadow; without the mirror their
	 * draws would leave its slot unpushed. */
	if (R_GPU ())
		RG_UniformBlockRegister ("DlShadow", &sh_block, sizeof(sh_block));
#endif
}
