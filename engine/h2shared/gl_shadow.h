/* gl_shadow.h -- cube shadow maps for the clustered dynamic lights (GitHub #97)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __GL_SHADOW_H
#define __GL_SHADOW_H

/* Phases P3 (the maps) and P4 (the receivers) of #97, under
 * r_dlight_occlusion 2.  Desktop GL 4.3 with the clustered path
 * (r_lightclusters 1) only: the receivers are ClusteredDlight and
 * ClusteredAliasDlight, which exist nowhere else.  The ES/WebGL2 tier and
 * r_lightclusters 0 keep P2's CPU visibility (r_dlightvis.c) and nothing here;
 * on USE_GLES every entry point is a stub.
 *
 * Texture unit of the samplerCubeArrayShadow in the world and alias fragment
 * stages.  Above everything a world or alias draw binds (0-8; MBOIT 12-15),
 * and no other sampler type may share it in those programs. */
#define DLSHADOW_TMU		9

/* Most lights shadowed at once: the layers the DlShadow block can name
 * (dlshadow.inc's DLSHADOW_MAX_MAPS, four bits each). */
#define SHADOW_MAX_MAPS		16

void	R_Shadow_Init (void);		/* cvars and commands, from R_Init */
void	R_Shadow_Shutdown (void);	/* GL objects, before the context goes */
void	R_Shadow_NewMap (void);		/* forget every map and the map's surface table */

/* Pick this frame's shadowed lights, fill the DlShadow block, and collect the
 * casters of the maps that are missing or stale (the CPU half).  Call from
 * R_SetupGL after R_LightCluster_Update, whose light list (and its order) the
 * block indexes, and after R_SetFrustum, which the light priority reads. */
void	R_Shadow_Prepare (void);

/* Draw what Prepare collected, then upload and bind the block (the GPU half).
 * Call right after Prepare, every frame on desktop, so the block's binding
 * always holds a buffer, if one that says "no maps".  Saves and restores the
 * framebuffer, viewport and every pipeline state it touches. */
void	R_Shadow_Draw (void);

/* Point a freshly linked program's DlShadow block at UBO_BINDING_DLSHADOW and
 * its u_dlshadowmap at DLSHADOW_TMU.  `program` must be current.  Harmless on
 * a program that declares neither, which is every program on the ES tier. */
void	R_Shadow_InitProgram (GLuint program);

/* r_shadow_debug: map slot 0 unwrapped into the corner of the view. */
void	R_Shadow_DrawDebug (void);

#endif	/* __GL_SHADOW_H */
