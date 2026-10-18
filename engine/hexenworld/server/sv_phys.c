/*
 * sv_phys.c -- sv physics
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

/*

pushmove objects do not obey gravity, and do not interact with
each other or trigger fields, but block normal movement and push
normal objects when they move.

onground is set for toss objects when they come to a complete rest.
it is set for steping or walking objects

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/

cvar_t	sv_maxvelocity		= { "sv_maxvelocity","2000", CVAR_NONE };
//cvar_t	sv_nostep		= { "sv_nostep","0", CVAR_NONE };
cvar_t	sv_gravity		= { "sv_gravity", "800", CVAR_NONE };
cvar_t	sv_stopspeed		= { "sv_stopspeed", "100", CVAR_NONE };
cvar_t	sv_maxspeed		= { "sv_maxspeed", "360", CVAR_NOTIFY|CVAR_SERVERINFO };
cvar_t	sv_spectatormaxspeed	= { "sv_spectatormaxspeed", "500", CVAR_NONE };
cvar_t	sv_accelerate		= { "sv_accelerate", "10", CVAR_NONE };
cvar_t	sv_airaccelerate	= { "sv_airaccelerate", "0.7", CVAR_NONE };
cvar_t	sv_wateraccelerate	= { "sv_wateraccelerate", "10", CVAR_NONE };
cvar_t	sv_friction		= { "sv_friction", "4", CVAR_NONE };
cvar_t	sv_waterfriction	= { "sv_waterfriction", "1", CVAR_NONE };
cvar_t	sv_flypitch		= { "sv_flypitch", "20", CVAR_NONE };
cvar_t	sv_walkpitch		= { "sv_walkpitch", "0", CVAR_NONE };

#if 0
static	vec3_t	vec_origin = {0.0, 0.0, 0.0};
#endif

#define	MOVE_EPSILON	0.01

void SV_Physics_Toss (edict_t *ent);

/*
================
SV_CheckAllEnts
================
*/
#if 0	// not used
static void SV_CheckAllEnts (void)
{
	int			e;
	edict_t		*check;

	// see if any solid entities are inside the final position
	check = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.movetype == MOVETYPE_PUSH
				|| check->v.movetype == MOVETYPE_NONE
				|| check->v.movetype == MOVETYPE_FOLLOW
				|| check->v.movetype == MOVETYPE_NOCLIP)
			continue;

		if (SV_TestEntityPosition (check))
			Con_Printf ("entity in invalid position\n");
	}
}
#endif


/*
================
SV_CheckVelocity, SV_RunThink, SV_Impact, ClipVelocity, SV_FlyMove,
SV_AddGravity, SV_PushEntity (and SV_FlyExtras, under #if 0 there)

engine/rust/src/sv_phys.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_base_hw.c.  Declared in world.h.
================
*/


/*
================
SV_Push, SV_PushMove, SV_PushRotate, SV_Physics_Pusher

engine/rust/src/sv_phys_push.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_push_hw.c.  Declared in world.h.
================
*/


/*
===============================================================================

CLIENT MOVEMENT: SV_Physics_Client (the rest of the client movement is #if 0 there)

engine/rust/src/sv_phys_walk.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_walk_hw.c.  Declared in world.h.

===============================================================================
*/

/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
static void SV_Physics_None (edict_t *ent)
{
// regular thinking
	SV_RunThink (ent);
}


/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
static void SV_Physics_Noclip (edict_t *ent)
{
// regular thinking
	if (!SV_RunThink (ent))
		return;

	VectorMA (ent->v.angles, host_frametime, ent->v.avelocity, ent->v.angles);
	VectorMA (ent->v.origin, host_frametime, ent->v.velocity, ent->v.origin);

	SV_LinkEdict (ent, false);
}


/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_CheckWaterTransition

=============
*/
static void SV_CheckWaterTransition (edict_t *ent)
{
	int		cont;

	cont = SV_PointContents (ent->v.origin);
	if (!ent->v.watertype)
	{	// just spawned here
		ent->v.watertype = cont;
		ent->v.waterlevel = 1;
		return;
	}

	if (cont <= CONTENTS_WATER)
	{
		if (ent->v.watertype == CONTENTS_EMPTY)
		{	// just crossed into water
			SV_StartSound (ent, 0, "misc/hith2o.wav", 255, 1);
		}
		ent->v.watertype = cont;
		ent->v.waterlevel = 1;
	}
	else
	{
		if (ent->v.watertype != CONTENTS_EMPTY)
		{	// just crossed into water
			SV_StartSound (ent, 0, "misc/hith2o.wav", 255, 1);
		}
		ent->v.watertype = CONTENTS_EMPTY;
		ent->v.waterlevel = cont;
	}
}


/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
void SV_Physics_Toss (edict_t *ent)
{
	trace_t	trace;
	vec3_t	move;
	float	backoff;

	// regular thinking
	if (!SV_RunThink (ent))
		return;

	if (ent->v.velocity[2] > 0)
		ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;

// if onground, return without moving
	if ( ((int)ent->v.flags & FL_ONGROUND) )
		return;

	SV_CheckVelocity (ent);

// add gravity
	if (ent->v.movetype != MOVETYPE_FLY
			&& ent->v.movetype != MOVETYPE_BOUNCEMISSILE
			&& ent->v.movetype != MOVETYPE_FLYMISSILE
			&& ent->v.movetype != MOVETYPE_SWIM)
		SV_AddGravity (ent, 1.0);

// move angles
	VectorMA (ent->v.angles, host_frametime, ent->v.avelocity, ent->v.angles);

// move origin
	VectorScale (ent->v.velocity, host_frametime, move);
	trace = SV_PushEntity (ent, move);
	if (trace.fraction == 1)
		return;
	if (ent->free)
		return;

	if (ent->v.movetype == MOVETYPE_BOUNCE)
		backoff = 1.5;
	else if (ent->v.movetype == MOVETYPE_BOUNCEMISSILE)
	{	// Solid phased missiles don't bounce on monsters or players
		if ((ent->v.solid == SOLID_PHASE) && (((int)trace.ent->v.flags & FL_MONSTER) || ((int)trace.ent->v.movetype == MOVETYPE_WALK)))
		{
			return;
		}
		backoff = 2.0;
	}
	else
		backoff = 1;

	ClipVelocity (ent->v.velocity, trace.plane.normal, ent->v.velocity, backoff);

// stop if on ground
	if ((trace.plane.normal[2] > 0.7) && (ent->v.movetype != MOVETYPE_BOUNCEMISSILE))
	{
		if (ent->v.velocity[2] < 60 || ent->v.movetype != MOVETYPE_BOUNCE)
		{
			ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
			ent->v.groundentity = EDICT_TO_PROG(trace.ent);
			VectorClear (ent->v.velocity);
			VectorClear (ent->v.avelocity);
		}
	}

// check for in water
	SV_CheckWaterTransition (ent);
}


/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
FIXME: is this true?
=============
*/
static void SV_Physics_Step (edict_t *ent)
{
	qboolean	hitsound;

// freefall if not onground
	if ( ! ((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM) ) )
	{
		if (ent->v.velocity[2] < movevars.gravity*-0.1)
			hitsound = true;
		else
			hitsound = false;

		SV_AddGravity (ent, 1.0);
		SV_CheckVelocity (ent);
		SV_FlyMove (ent, host_frametime, NULL);
		SV_LinkEdict (ent, true);

		if (((int)ent->v.flags & FL_ONGROUND) && !((int)ent->v.flags & FL_MONSTER))
		{	// just hit ground
			if (hitsound)
				SV_StartSound (ent, 0, "fx/thngland.wav", 255, 1);
		}
	}

// regular thinking
	SV_RunThink (ent);

	SV_CheckWaterTransition (ent);
}


//============================================================================

void SV_ProgStartFrame (void)
{
// let the progs know that a new frame has started
	*sv_globals.self = EDICT_TO_PROG(sv.edicts);
	*sv_globals.other = EDICT_TO_PROG(sv.edicts);
	*sv_globals.time = sv.time;
	PR_ExecuteProgram (*sv_globals.StartFrame, "StartFrame");
}


/*
================
SV_RunEntity

================
*/
static void SV_RunEntity (edict_t *ent)
{
	int		c, originMoved;
	edict_t	*ent2;
	vec3_t	oldOrigin, oldAngle;

	if (ent->v.lastruntime == (float)realtime)
		return;
	ent->v.lastruntime = (float)realtime;

	ent2 = PROG_TO_EDICT(ent->v.movechain);
	if (ent2 != sv.edicts)
	{
		VectorCopy(ent->v.origin,oldOrigin);
		VectorCopy(ent->v.angles,oldAngle);
	}
	else
	{	// avoid compiler warning
		VectorClear(oldOrigin);
		VectorClear(oldAngle);
	}

	switch ( (int)ent->v.movetype)
	{
	case MOVETYPE_PUSH:
		SV_Physics_Pusher (ent);
		break;
	case MOVETYPE_NONE:
		SV_Physics_None (ent);
		break;
	case MOVETYPE_NOCLIP:
		SV_Physics_Noclip (ent);
		break;
	case MOVETYPE_STEP:
	case MOVETYPE_PUSHPULL:
		SV_Physics_Step (ent);
		break;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
	case MOVETYPE_SWIM:
		SV_Physics_Toss (ent);
		break;
	case MOVETYPE_FOLLOW:
		break;
	case MOVETYPE_WALK:
		SV_RunThink (ent);
		break;
	default:
		SV_Error ("%s: bad movetype %i", __thisfunc__, (int)ent->v.movetype);
	}

	if (ent2 != sv.edicts)
	{
		originMoved = !VectorCompare(ent->v.origin,oldOrigin);
		if (originMoved || !VectorCompare(ent->v.angles,oldAngle))
		{
			VectorSubtract(ent->v.origin,oldOrigin,oldOrigin);
			VectorSubtract(ent->v.angles,oldAngle,oldAngle);

			for (c = 0; c < 10; c++)
			{	// chain a max of 10 objects
				if (ent2->free)
					break;

				VectorAdd(oldOrigin,ent2->v.origin,ent2->v.origin);
				if ((int)ent2->v.flags & FL_MOVECHAIN_ANGLE)
				{
					VectorAdd(oldAngle,ent2->v.angles,ent2->v.angles);
				}

				if (originMoved && ent2->v.chainmoved)
				{	// callback function
					*sv_globals.self = EDICT_TO_PROG(ent2);
					*sv_globals.other = EDICT_TO_PROG(ent);
					PR_ExecuteProgram (ent2->v.chainmoved, "chainmoved");
				}

				ent2 = PROG_TO_EDICT(ent2->v.movechain);
				if (ent2 == sv.edicts)
					break;
			}
		}
	}
}


/*
================
SV_RunNewmis

================
*/
void SV_RunNewmis (void)
{
	edict_t	*ent;

	if (! *sv_globals.newmis)
		return;
	ent = PROG_TO_EDICT(*sv_globals.newmis);
	host_frametime = 0.05;
	*sv_globals.newmis = 0;

	SV_RunEntity (ent);
}


/*
================
SV_Physics

================
*/
void SV_Physics (void)
{
	int		i;
	edict_t	*ent;
	static double	old_time;

// don't bother running a frame if sys_ticrate seconds haven't passed
	host_frametime = realtime - old_time;
	if (host_frametime < sv_mintic.value)
		return;
	if (host_frametime > sv_maxtic.value)
		host_frametime = sv_maxtic.value;
	old_time = realtime;

	*sv_globals.frametime = host_frametime;

	SV_ProgStartFrame ();

//
// treat each object in turn
// even the world gets a chance to think
//
	ent = sv.edicts;
	for (i = 0; i < sv.num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;

		if (*sv_globals.force_retouch)
			SV_LinkEdict (ent, true);	// force retouch even for stationary

		if (i > 0 && i <= MAX_CLIENTS)
		{
		//	SV_Physics_Client(ent);
		//	VectorCopy (ent->v.origin,ent->v.oldorigin);

			continue;	// clients are run directly from packets
		}

		SV_RunEntity (ent);
		SV_RunNewmis ();
	}

	if (*sv_globals.force_retouch)
		(*sv_globals.force_retouch)--;
}


void SV_SetMoveVars (void)
{
	movevars.gravity		= sv_gravity.value;
	movevars.stopspeed		= sv_stopspeed.value;
	movevars.maxspeed		= sv_maxspeed.value;
	movevars.spectatormaxspeed	= sv_spectatormaxspeed.value;
	movevars.accelerate		= sv_accelerate.value;
	movevars.airaccelerate		= sv_airaccelerate.value;
	movevars.wateraccelerate	= sv_wateraccelerate.value;
	movevars.friction		= sv_friction.value;
	movevars.waterfriction		= sv_waterfriction.value;
	movevars.entgravity		= 1.0;
}

