/*
 * world.c -- world query functions
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

#include "quakedef.h"

typedef struct
{
	vec3_t		boxmins, boxmaxs;// enclose the test object along entire move
	float		*mins, *maxs;	// size of the moving object
	vec3_t		mins2, maxs2;	// size when clipping against mosnters
	float		*start, *end;
	trace_t		trace;
	int			type;
	edict_t		*passedict;
} moveclip_t;



/*
===============================================================================

HULL BOXES

===============================================================================
*/


static	int			move_type;

/*
 * The box hull (box_hull, box_clipnodes, box_planes, SV_InitBoxHull,
 * SV_HullForBox) and SV_HullForEntity are Rust now:
 * engine/rust/src/world_hull.rs (#455).  The C that was here is the
 * reference half of its differential harness, moved verbatim to
 * engine/rust/world_hull/tests/world_hull_hw.c.
 */

/*
===============================================================================

ENTITY AREA CHECKING

===============================================================================
*/

areanode_t	sv_areanodes[AREA_NODES];
static	int			sv_numareanodes;

/*
===============
SV_CreateAreaNode

===============
*/
static areanode_t *SV_CreateAreaNode (int depth, vec3_t mins, vec3_t maxs)
{
	areanode_t	*anode;
	vec3_t		size;
	vec3_t		mins1, maxs1, mins2, maxs2;

	anode = &sv_areanodes[sv_numareanodes];
	sv_numareanodes++;

	ClearLink (&anode->trigger_edicts);
	ClearLink (&anode->solid_edicts);

	if (depth == AREA_DEPTH)
	{
		anode->axis = -1;
		anode->children[0] = anode->children[1] = NULL;
		return anode;
	}

	VectorSubtract (maxs, mins, size);
	if (size[0] > size[1])
		anode->axis = 0;
	else
		anode->axis = 1;

	anode->dist = 0.5 * (maxs[anode->axis] + mins[anode->axis]);
	VectorCopy (mins, mins1);
	VectorCopy (mins, mins2);
	VectorCopy (maxs, maxs1);
	VectorCopy (maxs, maxs2);

	maxs1[anode->axis] = mins2[anode->axis] = anode->dist;

	anode->children[0] = SV_CreateAreaNode (depth+1, mins2, maxs2);
	anode->children[1] = SV_CreateAreaNode (depth+1, mins1, maxs1);

	return anode;
}

/*
===============
SV_ClearWorld

===============
*/
void SV_ClearWorld (void)
{
	SV_InitBoxHull ();

	memset (sv_areanodes, 0, sizeof(sv_areanodes));
	sv_numareanodes = 0;
	SV_CreateAreaNode (0, sv.worldmodel->mins, sv.worldmodel->maxs);
}


/*
===============
SV_UnlinkEdict

===============
*/
static link_t	**sv_link_next;
static link_t	**sv_link_prev;

void SV_UnlinkEdict (edict_t *ent)
{
	if (!ent->area.prev)
		return;		// not linked in anywhere
	RemoveLink (&ent->area);
	if (sv_link_next && *sv_link_next == &ent->area)
		*sv_link_next = ent->area.next;
	if (sv_link_prev && *sv_link_prev == &ent->area)
		*sv_link_prev = ent->area.prev;
	ent->area.prev = ent->area.next = NULL;
}


/*
====================
SV_TouchLinks
====================
*/
static void SV_TouchLinks (edict_t *ent, areanode_t *node)
{
	link_t		*l, *lnext;
	edict_t		*touch;
	int			old_self, old_other;

loc0:
	// touch linked edicts
	sv_link_next = &lnext;
	for (l = node->trigger_edicts.next ; l != &node->trigger_edicts ; l = lnext)
	{
		if (!l)
		{
		// my area got removed out from under me!
			Con_Printf ("%s: encountered NULL link!\n", __thisfunc__);
			break;
		}

		lnext = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch == ent)
			continue;
		if (!touch->v.touch || touch->v.solid != SOLID_TRIGGER)
			continue;
		if (ent->v.absmin[0] > touch->v.absmax[0]
				|| ent->v.absmin[1] > touch->v.absmax[1]
				|| ent->v.absmin[2] > touch->v.absmax[2]
				|| ent->v.absmax[0] < touch->v.absmin[0]
				|| ent->v.absmax[1] < touch->v.absmin[1]
				|| ent->v.absmax[2] < touch->v.absmin[2] )
			continue;

		old_self = *sv_globals.self;
		old_other = *sv_globals.other;

		*sv_globals.self = EDICT_TO_PROG(touch);
		*sv_globals.other = EDICT_TO_PROG(ent);
		*sv_globals.time = sv.time;
		PR_ExecuteProgram (touch->v.touch, "touch");

		*sv_globals.self = old_self;
		*sv_globals.other = old_other;
	}

	sv_link_next = NULL;

	// recurse down both sides
	if (node->axis == -1)
		return;

	// LordHavoc: optimized recursion
	//if (ent->v.absmax[node->axis] > node->dist) SV_TouchLinks (ent, node->children[0]);
	//if (ent->v.absmin[node->axis] < node->dist) SV_TouchLinks (ent, node->children[1]);
	if (ent->v.absmax[node->axis] > node->dist)
	{
		if (ent->v.absmin[node->axis] < node->dist)
			SV_TouchLinks(ent, node->children[1]); // order reversed to reduce code
		node = node->children[0];
		goto loc0;
	}
	else
	{
		if (ent->v.absmin[node->axis] < node->dist)
		{
			node = node->children[1];
			goto loc0;
		}
	}
}


/*
===============
SV_FindTouchedLeafs

===============
*/
static void SV_FindTouchedLeafs (edict_t *ent, mnode_t *node)
{
	mplane_t	*splitplane;
	mleaf_t		*leaf;
	int			sides;
	int			leafnum;

loc0:
	if (node->contents == CONTENTS_SOLID)
		return;

	// add an efrag if the node is a leaf

	if (node->contents < 0)
	{
		if (ent->num_leafs == MAX_ENT_LEAFS)
			return;

		leaf = (mleaf_t *)node;
		leafnum = leaf - sv.worldmodel->leafs - 1;

		ent->leafnums[ent->num_leafs] = leafnum;
		ent->num_leafs++;
		return;
	}

	// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(ent->v.absmin, ent->v.absmax, splitplane);

	// recurse down the contacted sides
	// LordHavoc: optimized recursion
	//if (sides & 1) SV_FindTouchedLeafs (ent, node->children[0]);
	//if (sides & 2) SV_FindTouchedLeafs (ent, node->children[1]);
	switch (sides)
	{
	case 1:
		node = node->children[0];
		goto loc0;
	case 2:
		node = node->children[1];
		goto loc0;
	default: // 3
		if (node->children[0]->contents != CONTENTS_SOLID)
			SV_FindTouchedLeafs (ent, node->children[0]);
		node = node->children[1];
		goto loc0;
	}
}

/*
===============
SV_LinkEdict

===============
*/
void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)
{
	areanode_t	*node;

	if (ent->area.prev)
		SV_UnlinkEdict (ent);	// unlink from old position

	if (ent == sv.edicts)
		return;		// don't add the world

	if (ent->free)
		return;

	// set the abs box
	if (ent->v.solid == SOLID_BSP && 
		(ent->v.angles[0] || ent->v.angles[1] || ent->v.angles[2]) )
	{	// expand for rotation
		float		v, maxv;
		int			i;

		maxv = 0;
		for (i = 0; i < 3; i++)
		{
			v = fabs(ent->v.mins[i]);
			if (v > maxv)
				maxv = v;
			v = fabs(ent->v.maxs[i]);
			if (v > maxv)
				maxv = v;
		}
		for (i = 0; i < 3; i++)
		{
			ent->v.absmin[i] = ent->v.origin[i] - maxv;
			ent->v.absmax[i] = ent->v.origin[i] + maxv;
		}
	}
	else
	{
		VectorAdd (ent->v.origin, ent->v.mins, ent->v.absmin);
		VectorAdd (ent->v.origin, ent->v.maxs, ent->v.absmax);
	}

//
// to make items easier to pick up and allow them to be grabbed off
// of shelves, the abs sizes are expanded
//
	if ((int)ent->v.flags & FL_ITEM)
	{
		ent->v.absmin[0] -= 15;
		ent->v.absmin[1] -= 15;
		ent->v.absmax[0] += 15;
		ent->v.absmax[1] += 15;
	}
	else
	{	// because movement is clipped an epsilon away from an actual edge,
		// we must fully check even when bounding boxes don't quite touch
		ent->v.absmin[0] -= 1;
		ent->v.absmin[1] -= 1;
		ent->v.absmin[2] -= 1;
		ent->v.absmax[0] += 1;
		ent->v.absmax[1] += 1;
		ent->v.absmax[2] += 1;
	}

	// link to PVS leafs
	ent->num_leafs = 0;
	if (ent->v.modelindex)
		SV_FindTouchedLeafs (ent, sv.worldmodel->nodes);

	if (ent->v.solid == SOLID_NOT)
		return;

	// find the first node that the ent's box crosses
	node = sv_areanodes;
	while (1)
	{
		if (node->axis == -1)
			break;
		if (ent->v.absmin[node->axis] > node->dist)
			node = node->children[0];
		else if (ent->v.absmax[node->axis] < node->dist)
			node = node->children[1];
		else
			break;		// crosses the node
	}

	// link it in

	if (ent->v.solid == SOLID_TRIGGER)
		InsertLinkBefore (&ent->area, &node->trigger_edicts);
	else
		InsertLinkBefore (&ent->area, &node->solid_edicts);

	// if touch_triggers, touch all entities at this node and decend for more
	if (touch_triggers)
		SV_TouchLinks ( ent, sv_areanodes );
}


/*
===============================================================================

POINT TESTING IN HULLS

===============================================================================
*/

/*
 * SV_HullPointContents and SV_RecursiveHullCheck are Rust now:
 * engine/rust/src/world_trace.rs (#455).  The C that was here is the
 * reference half of its differential harness, moved verbatim to
 * engine/rust/world_trace/tests/world_trace_hw.c.
 */


/*
==================
SV_PointContents

==================
*/
int SV_PointContents (vec3_t p)
{
	int		cont;

	cont = SV_HullPointContents (&sv.worldmodel->hulls[0], 0, p);
	if (cont <= CONTENTS_CURRENT_0 && cont >= CONTENTS_CURRENT_DOWN)
		cont = CONTENTS_WATER;
	return cont;
}

#ifdef QUAKE2
int SV_TruePointContents (vec3_t p)
{
	return SV_HullPointContents (&sv.worldmodel->hulls[0], 0, p);
}
#endif

//===========================================================================

/*
============
SV_TestEntityPosition

A small wrapper around SV_BoxInSolidEntity that never clips against the
supplied entity.
============
*/
edict_t	*SV_TestEntityPosition (edict_t *ent)
{
	trace_t	trace;

	trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, ent->v.origin, 0, ent);

	if (trace.startsolid)
	{
		return sv.edicts;
	}

	return NULL;
}


/*
===============================================================================

LINE TESTING IN HULLS

===============================================================================
*/

/*
 * SV_HullPointContents and SV_RecursiveHullCheck are Rust now:
 * engine/rust/src/world_trace.rs (#455).  The C that was here is the
 * reference half of its differential harness, moved verbatim to
 * engine/rust/world_trace/tests/world_trace_hw.c.
 */


/*
==================
SV_ClipMoveToEntity

Handles selection or creation of a clipping hull, and offseting (and
eventually rotation) of the end points
==================
*/
static trace_t SV_ClipMoveToEntity (edict_t *ent, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *move_ent)
{
	trace_t		trace;
	vec3_t		offset;
	vec3_t		start_l, end_l;
	hull_t		*hull;

// fill in a default trace
	memset (&trace, 0, sizeof(trace_t));
	trace.fraction = 1;
	trace.allsolid = true;
	VectorCopy (end, trace.endpos);

// get the clipping hull
	hull = SV_HullForEntity (ent, mins, maxs, offset, move_ent);

	VectorSubtract (start, offset, start_l);
	VectorSubtract (end, offset, end_l);

	// rotate start and end into the models frame of reference
	if (ent->v.solid == SOLID_BSP && 
		(fabs(ent->v.angles[0]) > 1 || fabs(ent->v.angles[1]) > 1 || fabs(ent->v.angles[2]) > 1) )
	{
		vec3_t	forward, right, up;
		vec3_t	temp;

		AngleVectors (ent->v.angles, forward, right, up);

		VectorCopy (start_l, temp);
		start_l[0] = DotProduct (temp, forward);
		start_l[1] = -DotProduct (temp, right);
		start_l[2] = DotProduct (temp, up);

		VectorCopy (end_l, temp);
		end_l[0] = DotProduct (temp, forward);
		end_l[1] = -DotProduct (temp, right);
		end_l[2] = DotProduct (temp, up);
	}

// trace a line through the apropriate clipping hull
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start_l, end_l, &trace);

	if (move_type == MOVE_WATER)
	{
		if (SV_PointContents (trace.endpos) != CONTENTS_WATER)
		{
			VectorCopy(start_l, trace.endpos);
			trace.fraction = 0;
		}
	}

	// rotate endpos back to world frame of reference
	if (ent->v.solid == SOLID_BSP && 
		(fabs(ent->v.angles[0]) > 1 || fabs(ent->v.angles[1]) > 1 || fabs(ent->v.angles[2]) > 1) )
	{
		vec3_t	a;
		vec3_t	forward, right, up;
		vec3_t	temp;

		if (trace.fraction != 1)
		{
			VectorNegate (ent->v.angles, a);
			AngleVectors (a, forward, right, up);

			VectorCopy (trace.endpos, temp);
			trace.endpos[0] = DotProduct (temp, forward);
			trace.endpos[1] = -DotProduct (temp, right);
			trace.endpos[2] = DotProduct (temp, up);

			VectorCopy (trace.plane.normal, temp);
			trace.plane.normal[0] = DotProduct (temp, forward);
			trace.plane.normal[1] = -DotProduct (temp, right);
			trace.plane.normal[2] = DotProduct (temp, up);
		}
	}

// fix trace up by the offset
	if (trace.fraction != 1)
		VectorAdd (trace.endpos, offset, trace.endpos);

// did we clip the move?
	if (trace.fraction < 1 || trace.startsolid)
		trace.ent = ent;

	return trace;
}

//===========================================================================

/*
====================
SV_ClipToLinks

Mins and maxs enclose the entire area swept by the move
====================
*/
static void SV_ClipToLinks (areanode_t *node, moveclip_t *clip)
{
	link_t		*l, *next;
	edict_t		*touch;
	trace_t		trace;

loc0: // optimized recursion

// touch linked edicts
	for (l = node->solid_edicts.next ; l != &node->solid_edicts ; l = next)
	{
		next = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch->v.solid == SOLID_NOT)
			continue;
		if (touch == clip->passedict)
			continue;
		if (touch->v.solid == SOLID_TRIGGER)
			SV_Error ("Trigger in clipping list");

		if ((clip->type == MOVE_NOMONSTERS || clip->type == MOVE_PHASE)
				&& touch->v.solid != SOLID_BSP)
			continue;

		if (clip->boxmins[0] > touch->v.absmax[0]
				|| clip->boxmins[1] > touch->v.absmax[1]
				|| clip->boxmins[2] > touch->v.absmax[2]
				|| clip->boxmaxs[0] < touch->v.absmin[0]
				|| clip->boxmaxs[1] < touch->v.absmin[1]
				|| clip->boxmaxs[2] < touch->v.absmin[2] )
			continue;

		if (clip->passedict && clip->passedict->v.size[0] && !touch->v.size[0])
			continue;	// points never interact

	// might intersect, so do an exact clip
		if (clip->trace.allsolid)
			return;
		if (clip->passedict)
		{
			if (PROG_TO_EDICT(touch->v.owner) == clip->passedict)
				continue;	// don't clip against own missiles
			if (PROG_TO_EDICT(clip->passedict->v.owner) == touch)
				continue;	// don't clip against owner
		}

		if ((int)touch->v.flags & FL_MONSTER)
			trace = SV_ClipMoveToEntity (touch, clip->start, clip->mins2, clip->maxs2, clip->end, touch);
		else
			trace = SV_ClipMoveToEntity (touch, clip->start, clip->mins, clip->maxs, clip->end, touch);
		if (trace.allsolid || trace.startsolid || trace.fraction < clip->trace.fraction)
		{
			trace.ent = touch;
			if (clip->trace.startsolid)
			{
				clip->trace = trace;
				clip->trace.startsolid = true;
			}
			else
				clip->trace = trace;
		}
		else if (trace.startsolid)
			clip->trace.startsolid = true;
	}

// recurse down both sides
	if (node->axis == -1)
		return;

	if ( clip->boxmaxs[node->axis] > node->dist )
	{
		//SV_ClipToLinks ( node->children[0], clip );
		if (clip->boxmins[node->axis] < node->dist)
			SV_ClipToLinks(node->children[1], clip);
		node = node->children[0];
		goto loc0;
	}
	else if ( clip->boxmins[node->axis] < node->dist )
	{
		//SV_ClipToLinks ( node->children[1], clip );
		node = node->children[1];
		goto loc0;
	}
}


/*
==================
SV_MoveBounds
==================
*/
static void SV_MoveBounds (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, vec3_t boxmins, vec3_t boxmaxs)
{
#if 0
	// debug to test against everything
	boxmins[0] = boxmins[1] = boxmins[2] = -9999999;	//FIXME: change to FLT_MAX/-FLT_MAX
	boxmaxs[0] = boxmaxs[1] = boxmaxs[2] = 9999999;
#else
	int		i;

	for (i = 0; i < 3; i++)
	{
		if (end[i] > start[i])
		{
			boxmins[i] = start[i] + mins[i] - 1;
			boxmaxs[i] = end[i] + maxs[i] + 1;
		}
		else
		{
			boxmins[i] = end[i] + mins[i] - 1;
			boxmaxs[i] = start[i] + maxs[i] + 1;
		}
	}
#endif
}

/*
==================
SV_Move
==================
*/
trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict)
{
	moveclip_t	clip;
	int			i;

	memset ( &clip, 0, sizeof ( moveclip_t ) );

	move_type = type;
// clip to world
	clip.trace = SV_ClipMoveToEntity ( sv.edicts, start, mins, maxs, end, passedict );

	clip.start = start;
	clip.end = end;
	clip.mins = mins;
	clip.maxs = maxs;
	clip.type = type;
	clip.passedict = passedict;

	if (type == MOVE_MISSILE || type == MOVE_PHASE)
	{
		for (i = 0; i < 3; i++)
		{
			clip.mins2[i] = -15;
			clip.maxs2[i] = 15;
		}
	}
	else
	{
		VectorCopy (mins, clip.mins2);
		VectorCopy (maxs, clip.maxs2);
	}

// create the bounding box of the entire move
	SV_MoveBounds ( start, clip.mins2, clip.maxs2, end, clip.boxmins, clip.boxmaxs );

// clip to entities
	SV_ClipToLinks ( sv_areanodes, &clip );

	return clip.trace;
}


//=============================================================================

/*
============
SV_TestPlayerPosition
============
*/
#if 0	/* no callers */
edict_t	*SV_TestPlayerPosition (edict_t *ent, vec3_t origin)
{
	hull_t		*hull;
	edict_t		*check;
	vec3_t		boxmins, boxmaxs;
	vec3_t		offset;
	int			e;

	// check world first
	hull = &sv.worldmodel->hulls[1];
	if ( SV_HullPointContents (hull, hull->firstclipnode, origin) != CONTENTS_EMPTY )
		return sv.edicts;

	// check all entities
	VectorAdd (origin, ent->v.mins, boxmins);
	VectorAdd (origin, ent->v.maxs, boxmaxs);

	check = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.solid != SOLID_BSP &&
				check->v.solid != SOLID_BBOX &&
				check->v.solid != SOLID_SLIDEBOX)
			continue;

		if (boxmins[0] > check->v.absmax[0]
				|| boxmins[1] > check->v.absmax[1]
				|| boxmins[2] > check->v.absmax[2]
				|| boxmaxs[0] < check->v.absmin[0]
				|| boxmaxs[1] < check->v.absmin[1]
				|| boxmaxs[2] < check->v.absmin[2] )
			continue;

		if (check == ent)
			continue;

		// get the clipping hull
		hull = SV_HullForEntity (check, ent->v.mins, ent->v.maxs, offset, ent);

		VectorSubtract (origin, offset, offset);

		// test the point
		if ( SV_HullPointContents (hull, hull->firstclipnode, offset) != CONTENTS_EMPTY )
			return check;
	}

	return NULL;
}
#endif

