/* r_dlightvis.h -- can a dynamic light see a surface or a model? (GitHub #97)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef R_DLIGHTVIS_H
#define R_DLIGHTVIS_H

struct msurface_s;
struct entity_s;

void	R_DlightVis_Init (void);
void	R_DlightVis_NewMap (void);
void	R_DlightVis_BeginFrame (void);

/* Can cl_dlights[lnum] see this WORLD surface?  The caller has already made
 * the one-sided test (and, on GL, R_MarkLights' reach test); this is the rest
 * of the cascade: PVS, then segments to samples on the face.  true for
 * anything it cannot judge -- a brush entity's surface, sky, a light inside
 * solid -- which is vanilla. */
qboolean R_DlightVis_Surface (int lnum, struct msurface_s *surf);

/* Can cl_dlights[lnum] see the point org?  Segment test on the world BSP. */
qboolean R_DlightVis_Point (int lnum, const vec3_t org);

/* Can cl_dlights[lnum] see any of three points up the middle of an alias
 * entity's bounding box? */
qboolean R_DlightVis_Entity (int lnum, const struct entity_s *e);

#endif	/* R_DLIGHTVIS_H */
