/* r_dlightmark.c -- which entity brought each dynamic light to a surface (GitHub #496)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * The CPU dynamic-light path (GLES/WebGL2, WEBSOFT, desktop r_lightclusters 0)
 * measures a light on a brush entity's surface in that entity's model space
 * (#495, R_DlightModelOrigin), so it has to know which entity a light came in
 * through.  For almost every surface that is one entity, msurface_t.dlightent.
 * Two entities that share a `*N` submodel (a progs doing setmodel (e2,
 * e1.model)) share its surfaces, and a single dlightent kept only the last one
 * marked: the first entity's light was then measured in the second's space,
 * and with the one-sided test (#97) could be dropped outright.
 *
 * So a surface that a second entity marks in the same frame gets a list here,
 * one record per entity with the lights it brought, from a per-frame pool.
 * The common case costs nothing: no record is made until a surface is marked
 * by two different entities.  The records are read only while
 * surf->dlightframe is the current frame, and R_MarkLights clears the list
 * whenever it starts a surface's frame, so a record from an earlier frame
 * (the pool restarts with each new r_dlightframecount, and on a new map,
 * where r_framecount starts over and could meet the old stamp) is never read.
 *
 * THE RULE when the same light reaches a shared surface through more than one
 * entity.  Both copies draw one lightmap, so it cannot be right for both;
 * vanilla's shared lightmap had the same limit.  The light is measured in the
 * space of the copy it lights most: the one whose face it is nearest, by
 * R_MarkLights' own reach measure -- squared distance to the face's texel
 * rectangle.  Ties go to the entity marked first.  No copy the light is behind
 * is a candidate: R_MarkLights makes the one-sided test (#97) before it marks,
 * against the same plane, so such a copy never recorded the light.  A light
 * that reached the surface through only one entity is measured in that
 * entity's space, which is the whole fix for two copies standing apart, each
 * with its own light.
 *
 * Pool exhausted (DM_POOL records in one frame, which no shipped map comes near:
 * no Hexen II progs shares a brush submodel at all): the surface is marked
 * ambiguous for the frame.  Its lights are measured in dlightent's space, as
 * before this file, but R_DlightSurfaceOrigin returns false and the caller
 * skips the one-sided test, so a light can be misplaced but never dropped.
 */

#include "quakedef.h"
#if defined(WEBSOFT)
#include "r_local.h"
#endif
#include "r_dlightmark.h"

#define	DM_POOL		1024

static dlightmark_t	dm_pool[DM_POOL];
static int		dm_used;
static int		dm_frame = -1;		/* the r_dlightframecount dm_pool is for */
static dlightmark_t	dm_unknown;		/* sentinel: the pool ran out on this surface */
static qboolean		dm_warned;

static dlightmark_t *DM_Alloc (const entity_t *ent, int frame)
{
	dlightmark_t	*m;

	if (dm_frame != frame)
	{
		dm_frame = frame;
		dm_used = 0;
	}
	if (dm_used >= DM_POOL)
		return NULL;
	m = &dm_pool[dm_used++];
	m->ent = ent;
	memset (m->bits, 0, sizeof(m->bits));
	m->next = NULL;
	return m;
}

void R_DlightMark_NewMap (void)
{
	dm_frame = -1;	/* the next DM_Alloc restarts the pool */
}

void R_DlightMark_Shared (msurface_t *surf, int lnum, const entity_t *ent, int frame)
{
	dlightmark_t	*m, **tail;

	if (surf->dlightmarks == &dm_unknown)
		return;
	if (!surf->dlightmarks)
	{	/* every light marked so far this frame came in through dlightent */
		m = DM_Alloc (surf->dlightent, frame);
		if (!m)
			goto full;
		memcpy (m->bits, surf->dlightbits, sizeof(m->bits));
		surf->dlightmarks = m;
	}
	for (tail = &surf->dlightmarks; *tail; tail = &(*tail)->next)
	{
		if ((*tail)->ent == ent)
		{
			DLIGHTBIT_SET ((*tail)->bits, lnum);
			return;
		}
	}
	m = DM_Alloc (ent, frame);
	if (!m)
		goto full;
	DLIGHTBIT_SET (m->bits, lnum);
	*tail = m;	/* appended: ties in R_DlightSurfaceOrigin go to the first marked */
	return;

full:
	surf->dlightmarks = &dm_unknown;
	if (!dm_warned)
	{
		dm_warned = true;
		Con_DPrintf ("R_DlightMark_Shared: more than %d shared-submodel marks in a frame; "
			     "some dlights skip the one-sided test (GitHub #496)\n", DM_POOL);
	}
}

/* How far, in texels, a point on the plane lies outside the face's texel
 * rectangle along texture axis i: R_MarkLights' clamp. */
static float DM_Overhang (const msurface_t *surf, const vec3_t impact, int i)
{
	const mtexinfo_t	*tex = surf->texinfo;
	float			l;

	l = DotProduct (impact, tex->vecs[i]) + tex->vecs[i][3] - surf->texturemins[i];
	if (l < 0)
		return l;
	if (l > surf->extents[i])
		return l - surf->extents[i];
	return 0;
}

qboolean R_DlightSurfaceOrigin (const msurface_t *surf, int lnum, const vec3_t world, vec3_t lorg)
{
	const dlightmark_t	*m;
	qboolean		found = false;
	float			best = 0;

	if (!surf->dlightmarks || surf->dlightmarks == &dm_unknown)
	{
		R_DlightModelOrigin (surf->dlightent, world, lorg);
		return surf->dlightmarks != &dm_unknown;
	}
	for (m = surf->dlightmarks; m; m = m->next)
	{
		vec3_t		o, impact;
		float		dist, s, t, d2;
		int		j;

		if (!DLIGHTBIT_TEST (m->bits, lnum))
			continue;
		R_DlightModelOrigin (m->ent, world, o);
		dist = DotProduct (o, surf->plane->normal) - surf->plane->dist;
		for (j = 0; j < 3; j++)
			impact[j] = o[j] - surf->plane->normal[j] * dist;
		s = DM_Overhang (surf, impact, 0);
		t = DM_Overhang (surf, impact, 1);
		d2 = s*s + t*t + dist*dist;
		if (!found || d2 < best)
		{
			found = true;
			best = d2;
			VectorCopy (o, lorg);
		}
	}
	if (!found)	/* not reached: the records hold every bit set since they began */
		R_DlightModelOrigin (surf->dlightent, world, lorg);
	return true;
}
