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

/* Phase A of uhexen2-a5nn.1: the froxel grid and the compute pass that fills
 * it.  NOTHING SHADES FROM IT YET -- the world and alias fragment shaders still
 * take dynamic light the way they always have (a CPU lightmap rewrite for
 * surfaces, a flat per-entity term for models).  This is the buffer and the
 * clustering those consumers will read in phase B, landed on its own so it can
 * be reviewed and proved on its own.
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

void	R_LightCluster_Init (void);
void	R_LightCluster_Shutdown (void);

/* Fills the froxel grid for this frame's camera.  Call after R_SetupGL, which
 * is where the view and projection matrices reach their final state.  Cheap
 * no-op when unavailable or switched off. */
void	R_LightCluster_Update (void);

/* True when the grid holds this frame's clustering and phase B's consumers may
 * read it.  False on the ES tier, on a driver missing the pieces, and whenever
 * r_lightclusters is 0. */
qboolean R_LightCluster_Available (void);

#endif	/* __GL_LIGHTCLUSTER_H */
