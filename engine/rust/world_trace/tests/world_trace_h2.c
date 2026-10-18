/*
 * world_trace_h2.c -- the C original of SV_HullPointContents and
 * SV_RecursiveHullCheck, moved verbatim out of engine/hexen2/world.c.
 * The differential harness beside this file includes it; no engine target
 * compiles it.  The engine's implementation is engine/rust/src/world_trace.rs.
 *
 * (from world.c -- world query functions)
 *
 * entities never clip against themselves, or their owner
 * line of sight checks trace->crosscontent, but bullets don't
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* world.c's own include, quakedef.h, comes from the harness. */

#if	!id386

/*
==================
SV_HullPointContents

==================
*/
static int SV_HullPointContents (hull_t *hull, int num, vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Sys_Error ("%s: bad node number", __thisfunc__);

		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProductDBL(plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

#endif	/* !id386 */

static void WackyBugFixer(float *p1, float *p2, float *frac, float *mid)
{
	int i;

	for (i = 0; i < 3; i++)
		mid[i] = p1[i] + (*frac)*(p2[i] - p1[i]);
}

/*
==================
SV_RecursiveHullCheck

==================
*/
qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;
	int			contents;

loc0: // optimized recursion

// check for empty
	if (num < 0)
	{
		if (num != CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;

		return true;		// empty
	}

	if (num < hull->firstclipnode || num > hull->lastclipnode)
		Sys_Error ("%s: bad node number", __thisfunc__);

//
// find the point distances
//
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProductDBL(plane->normal, p1) - plane->dist;
		t2 = DotProductDBL(plane->normal, p2) - plane->dist;
	}

#if 1
	if (t1 >= 0 && t2 >= 0)
	{
		//return SV_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
		num = node->children[0];
		goto loc0;
	}
	if (t1 < 0 && t2 < 0)
	{
		//return SV_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
		num = node->children[1];
		goto loc0;
	}
#else
	if ( (t1 >= DIST_EPSILON && t2 >= DIST_EPSILON) || (t2 > t1 && t1 >= 0) )
		return SV_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if ( (t1 <= -DIST_EPSILON && t2 <= -DIST_EPSILON) || (t2 < t1 && t1 <= 0) )
		return SV_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);
#endif

// put the crosspoint DIST_EPSILON pixels on the near side
	side = (t1 < 0);
	if (side)
		frac = (t1 + DIST_EPSILON)/(t1-t2);
	else
		frac = (t1 - DIST_EPSILON)/(t1-t2);
	if (frac < 0)
		frac = 0;
	else if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i = 0; i < 3; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

// move up to the node
	if (!SV_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace) )
		return false;

#ifdef PARANOID
	if (SV_HullPointContents (hull, node->children[side], mid) == CONTENTS_SOLID)
	{
		Con_Printf ("mid PointInHullSolid\n");
		return false;
	}
#endif

	// LordHavoc: this recursion can not be optimized because mid would need to be duplicated on a stack
	contents = SV_HullPointContents (hull, node->children[side^1], mid);
//	if (contents != CONTENTS_SOLID && (contents == CONTENTS_WATER || move_type != MOVE_WATER))
	if (contents != CONTENTS_SOLID)
		// go past the node
		return SV_RecursiveHullCheck (hull, node->children[side^1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		VectorNegate (plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

//	while (SV_HullPointContents (hull, hull->firstclipnode, mid) == CONTENTS_SOLID)
	while (1)
	{
	//	shouldn't really happen, but does occasionally
		contents = SV_HullPointContents (hull, hull->firstclipnode, mid);
	//	if (contents != CONTENTS_SOLID && (contents == CONTENTS_WATER || move_type != MOVE_WATER))
		if (contents != CONTENTS_SOLID)
			break;

		frac -= 0.1;
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Con_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f)*frac;

	//	for (i = 0; i < 3; i++)
	//		mid[i] = p1[i] + frac * (p2[i] - p1[i]);

		WackyBugFixer(p1, p2, &frac, mid);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}
