/*
 * world_move_h2.c -- the C original of SV_Move and the rest of world.c,
 * moved verbatim out of engine/hexen2/world.c.  The differential harness
 * beside this file includes it; no engine target compiles it.  The
 * engine's implementation is engine/rust/src/world_move.rs.
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

/* world.c's own include, quakedef.h, comes from the harness, and so does
 * sv_areanodes, which stays world.c's storage. */

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

static	int			move_type;

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

This could be a lot more efficient...
FIXME!!!  For rotating doors, this is totally inaccurate
============
*/
edict_t	*SV_TestEntityPosition (edict_t *ent)
{
	trace_t	trace;

	trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, ent->v.origin, 0, ent);

	if (trace.startsolid)
	{
	//	Con_DPrintf("%s inside check\n", PR_GetString(trace.ent->v.classname));
		return sv.edicts;
	}

	return NULL;
}

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
			Sys_Error ("Trigger in clipping list (%s)", PR_GetString(touch->v.classname));

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

		// SOLID_GHOST: only clipped by projectiles with .ghost_damage set
		if (touch->v.solid == SOLID_GHOST)
		{
			eval_t *val;
			if (!clip->passedict || clip->type != MOVE_MISSILE)
				continue;
			val = GetEdictFieldValue(clip->passedict, "ghost_damage");
			if (!val || !val->_float)
				continue;
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

//	type = MOVE_WATER;
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
	//Larger for projectiles against monsters
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
