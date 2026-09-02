/* gl_lightcluster.h -- clustered GPU dynamic lighting, froxel grid
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __GL_LIGHTCLUSTER_H
#define __GL_LIGHTCLUSTER_H

/* uhexen2-a5nn.1.  Phase A built the froxel grid and the compute pass that
 * fills it; phase B (uhexen2-26bm) made the WORLD fragment shaders shade from
 * it, so a dynamic light no longer rewrites lightmap blocks on the CPU.
 *
 * ALIAS MODELS FOLLOWED IN PHASE C (uhexen2-waum).  They used to take the whole
 * frame's dynamic light as one number computed at the entity's origin; they now
 * evaluate it per pixel from the same grid, behind their own r_lightclusters_models
 * switch so the two changes can be A/B'd apart.  Upstream Ironwail does NOT do
 * this -- its alias shader has no froxel lookup at all and its models keep the
 * CPU term (Quake/gl_shaders.h alias_vertex_shader, Quake/r_alias.c:295) -- so
 * this half is ours, and r_lightclusters_models 0 is the way back to parity.
 *
 * Desktop GL 4.3 only, gated on gl_renderer_caps.compute_shaders and
 * .shader_storage.  The ES/WebGL2 tier has neither, which is why the CPU dlight
 * path cannot ever be removed outright -- see the design notes on uhexen2-a5nn.1.
 */

/* 32 x 16 x 32 froxels, logarithmic in depth.  Upstream's dimensions
 * (Ironwail Quake/glquake.h:406-408). */
#define LIGHT_TILES_X	32
#define LIGHT_TILES_Y	16
#define LIGHT_TILES_Z	32

/* SSBO binding point for the light buffer.  Shared by the compute pass and by
 * the world fragment shaders, so there is one number to keep right.
 *
 * 8, AND THAT NUMBER IS LOAD-BEARING.  Unlike every other SSBO in the engine,
 * this binding has to survive from R_SetupGL all the way through the world
 * draw -- the fragment shader reads it.  Every index below 8 is claimed by a
 * compute pass that runs in between and does not put it back:
 * gl_worldcull.c's cull program binds 0..7 (surfaces, marksurfs, vis, indirect,
 * src/dst indices, dedup at 6, stats at 7), and the alias/skeletal and
 * GPU-particle paths use 0..4.  Sharing 6 with the dedup buffer is exactly the
 * bug this comment exists to prevent: the compute pass rebound it mid-frame,
 * every Lights[] read in the fragment shader came back zero, and the symptom
 * was dynamic lights that quietly did nothing rather than anything that looked
 * like a binding error.
 *
 * GL 4.3 only guarantees GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS >= 8, i.e.
 * indices 0..7, so 8 is not guaranteed to exist and R_LightCluster_Build
 * checks for it rather than assuming.  A driver that really only has 8 falls
 * back to the CPU dlight path.  In practice desktop GL 4.3 parts report 16 or
 * more (NVIDIA 96, Mesa 16+). */
#define LIGHT_SSBO_BINDING	8

/* Texture unit the froxel grid is bound to for the world fragment shaders.
 * Units 0..4 are the world program's diffuse/lightmap/fullbright/normal/gloss. */
#define LIGHT_GRID_TMU		5

void	R_LightCluster_Init (void);
void	R_LightCluster_Shutdown (void);

/* Fills the froxel grid for this frame's camera.  Call after R_SetupGL, which
 * is where the view and projection matrices reach their final state.  Cheap
 * no-op when unavailable or switched off. */
void	R_LightCluster_Update (void);

/* True when the grid holds this frame's clustering and its consumers may read
 * it.  False on the ES tier, on a driver missing the pieces, and whenever
 * r_lightclusters is 0.  Only meaningful AFTER R_LightCluster_Update has run
 * for the frame. */
qboolean R_LightCluster_Available (void);

/* True when the world fragment shaders are taking dynamic light from the grid,
 * and the CPU lightmap path must therefore stand down.
 *
 * SEPARATE FROM R_LightCluster_Available BECAUSE OF WHEN IT IS ASKED.  The CPU
 * path's entry point is R_PushDlights, which V_RenderView calls BEFORE
 * R_RenderView -- that is, before R_SetupGL, and so before this frame's grid
 * exists.  Available() would answer for the previous frame there.  This asks
 * the question that does not depend on the frame at all: is the GPU path
 * switched on and supported.  Every input is a cvar or a context fact, so the
 * answer is stable from R_PushDlights through to the last world draw and the
 * two sides of the fork cannot disagree and double-light a surface.
 * uhexen2-26bm. */
qboolean R_LightCluster_ShadesWorld (void);

/* The same question for alias models, and the same contract: stable for the
 * whole frame, answered from cvars and context facts only.  R_DrawAliasModel
 * asks it to decide whether to run its CPU dlight accumulation, which happens
 * well before the draw that would read the grid -- so an answer that could
 * change in between would light a model twice.  uhexen2-waum. */
qboolean R_LightCluster_ShadesAlias (void);

/* Narrower, and per draw rather than per frame: ShadesAlias AND this frame's
 * grid actually holds lights worth looking up.  False makes the alias draw
 * paths push a zero scale, which switches the froxel lookup off inside the
 * shader with one uniform-static compare.  uhexen2-waum. */
qboolean R_LightCluster_AliasActive (void);

/* Binds the light SSBO and the froxel grid where the world fragment shaders
 * expect them, and pushes the froxel-lookup constants into the three world
 * programs.  Call once per frame, after R_LightCluster_Update: the lookup
 * constants come out of the same view setup the grid was clustered against.
 * No-op when the GPU path is not shading. */
void R_LightCluster_BindForWorld (void);

/* The same lookup constants, pushed into every program that links salias_frag:
 * the plain alias program, its NOPERSP twin, both OIT variants, the four
 * skeletal ones and the two instanced ones.  Call once per frame, immediately
 * after R_LightCluster_BindForWorld -- it relies on that call having bound the
 * light SSBO and the grid texture, and on the same view setup.
 *
 * The SCALE is not set here.  It is per-batch state, because gl_shader_alias is
 * also the sprite / particle / warp-poly / unlit-brush-poly program: it travels
 * with the draw through GL_SetAliasDlight, not with the frame.  uhexen2-waum. */
void R_LightCluster_BindForAlias (void);

#endif	/* __GL_LIGHTCLUSTER_H */
