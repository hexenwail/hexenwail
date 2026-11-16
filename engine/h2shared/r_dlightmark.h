/* r_dlightmark.h -- which entity brought each dynamic light to a surface (GitHub #496)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef R_DLIGHTMARK_H
#define R_DLIGHTMARK_H

struct msurface_s;
struct entity_s;

/* One entity's lights on one surface, this frame.  Only made when a second
 * entity marks a surface another already marked: two entities sharing a `*N`
 * submodel share its surfaces.  See r_dlightmark.c. */
typedef struct dlightmark_s
{
	const struct entity_s	*ent;
	unsigned int		bits[4];	/* as msurface_t.dlightbits */
	struct dlightmark_s	*next;
} dlightmark_t;

/* A new map: r_framecount starts over, so the pool's frame stamp must too. */
void	R_DlightMark_NewMap (void);

/* R_MarkLights, on a surface already marked this frame by surf->dlightent or
 * by more than one entity, before it sets lnum's bit in surf->dlightbits. */
void	R_DlightMark_Shared (struct msurface_s *surf, int lnum, const struct entity_s *ent, int frame);

/* R_AddDynamicLights: cl_dlights[lnum]'s origin `world`, in the model space of
 * the entity that brought it to this surface.  false when that cannot be told
 * (the pool ran out): lorg is then in surf->dlightent's space, and the caller
 * skips the one-sided test rather than drop a light on a guess. */
qboolean R_DlightSurfaceOrigin (const struct msurface_s *surf, int lnum, const vec3_t world, vec3_t lorg);

#endif	/* R_DLIGHTMARK_H */
