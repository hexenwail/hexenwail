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

cvar_t	sv_maxvelocity		= { "sv_maxvelocity", "2000", CVAR_NONE };
cvar_t	sv_nostep		= { "sv_nostep", "0", CVAR_NONE };
cvar_t	sv_gravity		= { "sv_gravity", "800", CVAR_NOTIFY|CVAR_SERVERINFO };
cvar_t	sv_stopspeed		= { "sv_stopspeed", "100", CVAR_NONE };
cvar_t	sv_friction		= { "sv_friction", "4", CVAR_NOTIFY|CVAR_SERVERINFO };
cvar_t	sv_flypitch		= { "sv_flypitch", "20", CVAR_NONE };
cvar_t	sv_walkpitch		= { "sv_walkpitch", "0", CVAR_NONE };

/* 0 = off, 1 = clients only, 2 = all entities.  See SV_PushMove. */
cvar_t	sv_gameplayfix_elevators = { "sv_gameplayfix_elevators", "1", CVAR_ARCHIVE };

/* Freeze everything that is not a player: physics and think chains stop, and
 * so does server time, so a scene can be inspected mid-motion. */
cvar_t	sv_freezenonclients = { "sv_freezenonclients", "0", CVAR_NONE };

#ifdef QUAKE2
static	vec3_t	vec_origin = {0.0, 0.0, 0.0};
#endif

#define	MOVE_EPSILON	0.01

static void SV_Physics_Toss (edict_t *ent);

/*
================
SV_CheckAllEnts
================
*/
#if 0	// all uses of it are commented out
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
#ifdef QUAKE2
				|| check->v.movetype == MOVETYPE_FOLLOW
#endif
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
SV_FlyExtras, SV_AddGravity, SV_PushEntity

engine/rust/src/sv_phys.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_base_h2.c.  Declared in world.h.
================
*/


/*
================
SV_PushMove, SV_PushRotate, SV_Physics_Pusher

engine/rust/src/sv_phys_push.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_push_h2.c.  Declared in world.h.
================
*/


/*
===============================================================================

CLIENT MOVEMENT

===============================================================================
*/

/*
=============
SV_CheckStuck

This is a big hack to try and fix the rare case of getting stuck in the world
clipping hull.
=============
*/
static void SV_CheckStuck (edict_t *ent)
{
	int		i, j;
	int		z;
	vec3_t	org;

	if (!SV_TestEntityPosition(ent))
	{
		VectorCopy (ent->v.origin, ent->v.oldorigin);
		return;
	}

	VectorCopy (ent->v.origin, org);
	VectorCopy (ent->v.oldorigin, ent->v.origin);
	if (!SV_TestEntityPosition(ent))
	{
		Con_DPrintf ("moved back Unstuck.\n");
		SV_LinkEdict (ent, true);
		return;
	}

	for (z = 0; z < 18; z++)
	{
		for (i = -1; i <= 1; i++)
		{
			for (j = -1; j <= 1; j++)
			{
				ent->v.origin[0] = org[0] + i;
				ent->v.origin[1] = org[1] + j;
				ent->v.origin[2] = org[2] + z;
				if (!SV_TestEntityPosition(ent))
				{
					Con_DPrintf ("scooted Unstuck.\n");
					SV_LinkEdict (ent, true);
					return;
				}
			}
		}
	}

	VectorCopy (org, ent->v.origin);
	if (ent->v.oldorigin!=ent->v.origin)
		Con_DPrintf ("player is stuck.\n");
}


/*
=============
SV_CheckWater
=============
*/
static qboolean SV_CheckWater (edict_t *ent)
{
	vec3_t	point;
	int		cont;
#ifdef QUAKE2
	int		truecont;
#endif

	point[0] = ent->v.origin[0];
	point[1] = ent->v.origin[1];
	point[2] = ent->v.origin[2] + ent->v.mins[2] + 1;

	ent->v.waterlevel = 0;
	ent->v.watertype = CONTENTS_EMPTY;
	cont = SV_PointContents (point);
	if (cont <= CONTENTS_WATER)
	{
#ifdef QUAKE2
		truecont = SV_TruePointContents (point);
#endif
		ent->v.watertype = cont;
		ent->v.waterlevel = 1;
		point[2] = ent->v.origin[2] + (ent->v.mins[2] + ent->v.maxs[2])*0.5;
		cont = SV_PointContents (point);
		if (cont <= CONTENTS_WATER)
		{
			ent->v.waterlevel = 2;
			point[2] = ent->v.origin[2] + ent->v.view_ofs[2];
			cont = SV_PointContents (point);
			if (cont <= CONTENTS_WATER)
				ent->v.waterlevel = 3;
		}
#ifdef QUAKE2
		if (truecont <= CONTENTS_CURRENT_0 && truecont >= CONTENTS_CURRENT_DOWN)
		{
			static vec3_t current_table[] =
			{
				{1, 0, 0},
				{0, 1, 0},
				{-1, 0, 0},
				{0, -1, 0},
				{0, 0, 1},
				{0, 0, -1}
			};

			VectorMA (ent->v.basevelocity, 150.0*ent->v.waterlevel/3.0, current_table[CONTENTS_CURRENT_0 - truecont], ent->v.basevelocity);
		}
#endif
	}

	return ent->v.waterlevel > 1;
}


/*
============
SV_WallFriction

============
*/
static void SV_WallFriction (edict_t *ent, trace_t *trace)
{
	vec3_t		forward, right, up;
	float		d, i;
	vec3_t		into, side;

	AngleVectors (ent->v.v_angle, forward, right, up);
	d = DotProduct (trace->plane.normal, forward);

	d += 0.5;
	if (d >= 0)
		return;

// cut the tangential velocity
	i = DotProduct (trace->plane.normal, ent->v.velocity);
	VectorScale (trace->plane.normal, i, into);
	VectorSubtract (ent->v.velocity, into, side);

	ent->v.velocity[0] = side[0] * (1 + d);
	ent->v.velocity[1] = side[1] * (1 + d);
}


/*
=====================
SV_TryUnstick

Player has come to a dead stop, possibly due to the problem with limited
float precision at some angle joins in the BSP hull.

Try fixing by pushing one pixel in each direction.

This is a hack, but in the interest of good gameplay...
======================
*/
static int SV_TryUnstick (edict_t *ent, vec3_t oldvel)
{
	int		i;
	vec3_t	oldorg;
	vec3_t	dir;
	int		clip;
	trace_t	steptrace;

	VectorCopy (ent->v.origin, oldorg);
	VectorClear (dir);

	for (i = 0; i < 8; i++)
	{
	// try pushing a little in an axial direction
		switch (i)
		{
		case 0:
			dir[0] = 2;
			dir[1] = 0;
			break;
		case 1:
			dir[0] = 0;
			dir[1] = 2;
			break;
		case 2:
			dir[0] = -2;
			dir[1] = 0;
			break;
		case 3:
			dir[0] = 0;
			dir[1] = -2;
			break;
		case 4:
			dir[0] = 2;
			dir[1] = 2;
			break;
		case 5:
			dir[0] = -2;
			dir[1] = 2;
			break;
		case 6:
			dir[0] = 2;
			dir[1] = -2;
			break;
		case 7:
			dir[0] = -2;
			dir[1] = -2;
			break;
		}

		SV_PushEntity (ent, dir);

	// retry the original move
		ent->v.velocity[0] = oldvel[0];
		ent->v. velocity[1] = oldvel[1];
		ent->v. velocity[2] = 0;
		clip = SV_FlyMove (ent, 0.1, &steptrace);

		if ( fabs(oldorg[1] - ent->v.origin[1]) > 4
			|| fabs(oldorg[0] - ent->v.origin[0]) > 4 )
		{
		//	Con_DPrintf ("unstuck!\n");
			return clip;
		}

	// go back to the original pos and try again
		VectorCopy (oldorg, ent->v.origin);
	}

	VectorClear (ent->v.velocity);
	return 7;		// still not moving
}


/*
=====================
SV_WalkMove

Only used by players
======================
*/
#define	STEPSIZE	18
static void SV_WalkMove (edict_t *ent)
{
	vec3_t		upmove, downmove;
	vec3_t		oldorg, oldvel;
	vec3_t		nosteporg, nostepvel;
	int			clip;
	int			oldonground;
	trace_t		steptrace, downtrace;

//
// do a regular slide move unless it looks like you ran into a step
//
	oldonground = (int)ent->v.flags & FL_ONGROUND;
	ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;

	VectorCopy (ent->v.origin, oldorg);
	VectorCopy (ent->v.velocity, oldvel);

	clip = SV_FlyMove (ent, host_frametime, &steptrace);

	if ( !(clip & 2) )
		return;		// move didn't block on a step

	if (!oldonground && ent->v.waterlevel == 0)
		return;		// don't stair up while jumping

	if (ent->v.movetype != MOVETYPE_WALK)
		return;		// gibbed by a trigger

	if (sv_nostep.integer)
		return;

	if ( (int)sv_player->v.flags & FL_WATERJUMP )
		return;

	VectorCopy (ent->v.origin, nosteporg);
	VectorCopy (ent->v.velocity, nostepvel);

//
// try moving up and forward to go up a step
//
	VectorCopy (oldorg, ent->v.origin);	// back to start pos

	VectorClear (upmove);
	VectorClear (downmove);
	upmove[2] = STEPSIZE;
	downmove[2] = -STEPSIZE + oldvel[2]*host_frametime;

// move up
	SV_PushEntity (ent, upmove);	// FIXME: don't link?

// move forward
	ent->v.velocity[0] = oldvel[0];
	ent->v. velocity[1] = oldvel[1];
	ent->v. velocity[2] = 0;
	clip = SV_FlyMove (ent, host_frametime, &steptrace);

// check for stuckness, possibly due to the limited precision of floats
// in the clipping hulls
	if (clip)
	{
		if ( fabs(oldorg[1] - ent->v.origin[1]) < 0.03125
			&& fabs(oldorg[0] - ent->v.origin[0]) < 0.03125 )
		{	// stepping up didn't make any progress
			clip = SV_TryUnstick (ent, oldvel);
		}
	}

// extra friction based on view angle
	if ( clip & 2 )
		SV_WallFriction (ent, &steptrace);

// move down
	downtrace = SV_PushEntity (ent, downmove);	// FIXME: don't link?

	if (downtrace.plane.normal[2] > 0.7)
	{
		if (ent->v.solid == SOLID_BSP)
		{
			ent->v.flags =	(int)ent->v.flags | FL_ONGROUND;
			ent->v.groundentity = EDICT_TO_PROG(downtrace.ent);
		}
	}
	else
	{
// if the push down didn't end up on good ground, use the move without
// the step up.  This happens near wall / slope combinations, and can
// cause the player to hop up higher on a slope too steep to climb
		VectorCopy (nosteporg, ent->v.origin);
		VectorCopy (nostepvel, ent->v.velocity);
	}
}


/*
================
SV_Physics_Client

Player character actions
================
*/
static void SV_Physics_Client (edict_t *ent, int num)
{
	if ( ! svs.clients[num-1].active )
		return;		// unconnected slot

//
// call standard client pre-think
//
	*sv_globals.time = sv.time;
	*sv_globals.self = EDICT_TO_PROG(ent);
	PR_ExecuteProgram (*sv_globals.PlayerPreThink, "PlayerPreThink");

//
// do a move
//
	SV_CheckVelocity (ent);

//
// decide which move function to call
//
	switch ((int)ent->v.movetype)
	{
	case MOVETYPE_NONE:
		if (!SV_RunThink (ent))
			return;
		break;

	case MOVETYPE_WALK:
		if (!SV_RunThink (ent))
			return;
		if (!SV_CheckWater (ent) && ! ((int)ent->v.flags & FL_WATERJUMP) )
			SV_AddGravity (ent);
		SV_CheckStuck (ent);
#ifdef QUAKE2
		VectorAdd (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);
#endif
		SV_WalkMove (ent);

#ifdef QUAKE2
		VectorSubtract (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);
#endif
		break;

	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
		SV_Physics_Toss (ent);
		break;

	case MOVETYPE_FLY:
	case MOVETYPE_SWIM:
		if (!SV_RunThink (ent))
			return;
		SV_CheckWater (ent);
		SV_FlyMove (ent, host_frametime, NULL);
		SV_FlyExtras (ent, host_frametime, NULL);  // Hover & friction
		break;

	case MOVETYPE_NOCLIP:
		if (!SV_RunThink (ent))
			return;
		VectorMA (ent->v.origin, host_frametime, ent->v.velocity, ent->v.origin);
		break;

	default:
		Sys_Error ("%s: bad movetype %i", __thisfunc__, (int)ent->v.movetype);
	}

//
// call standard player post-think
//
	SV_LinkEdict (ent, true);

	*sv_globals.time = sv.time;
	*sv_globals.self = EDICT_TO_PROG(ent);
	PR_ExecuteProgram (*sv_globals.PlayerPostThink, "PlayerPostThink");
}


//============================================================================

/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
static void SV_Physics_None (edict_t *ent)
{
// regular thinking -- guard against NULL think (mod compat)
	if (ent->v.think)
		SV_RunThink (ent);
}


#ifdef QUAKE2
/*
=============
SV_Physics_Follow

Entities that are "stuck" to another entity
=============
*/
static void SV_Physics_Follow (edict_t *ent)
{
// regular thinking
	SV_RunThink (ent);
	VectorAdd (PROG_TO_EDICT(ent->v.aiment)->v.origin, ent->v.v_angle, ent->v.origin);
	SV_LinkEdict (ent, true);
}
#endif


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
#ifdef QUAKE2
	vec3_t	point;

	point[0] = ent->v.origin[0];
	point[1] = ent->v.origin[1];
	point[2] = ent->v.origin[2] + ent->v.mins[2] + 1;
	cont = SV_PointContents (point);
#else
	cont = SV_PointContents (ent->v.origin);
#endif
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
static void SV_Physics_Toss (edict_t *ent)
{
	trace_t	trace;
	vec3_t	move;
	float	backoff;
#ifdef QUAKE2
	edict_t	*groundentity;

	groundentity = PROG_TO_EDICT(ent->v.groundentity);
	if ((int)groundentity->v.flags & FL_CONVEYOR)
		VectorScale(groundentity->v.movedir, groundentity->v.speed, ent->v.basevelocity);
	else
		VectorCopy(vec_origin, ent->v.basevelocity);
	SV_CheckWater (ent);
#endif
	// regular thinking
	if (!SV_RunThink (ent))
		return;

#ifdef QUAKE2
	if (ent->v.velocity[2] > 0)
		ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;

	if ( ((int)ent->v.flags & FL_ONGROUND) )
//@@
		if (VectorCompare(ent->v.basevelocity, vec_origin))
			return;

	SV_CheckVelocity (ent);

// add gravity
	if (! ((int)ent->v.flags & FL_ONGROUND)
			&& ent->v.movetype != MOVETYPE_FLY
			&& ent->v.movetype != MOVETYPE_BOUNCEMISSILE
			&& ent->v.movetype != MOVETYPE_FLYMISSILE
			&& ent->v.movetype != MOVETYPE_SWIM)
		SV_AddGravity (ent);
#else
// if onground, return without moving
	if ( ((int)ent->v.flags & FL_ONGROUND) )
		return;

	SV_CheckVelocity (ent);

// add gravity
	if (ent->v.movetype != MOVETYPE_FLY
			&& ent->v.movetype != MOVETYPE_BOUNCEMISSILE
			&& ent->v.movetype != MOVETYPE_FLYMISSILE
			&& ent->v.movetype != MOVETYPE_SWIM)
		SV_AddGravity (ent);
#endif

// move angles
	VectorMA (ent->v.angles, host_frametime, ent->v.avelocity, ent->v.angles);

// move origin
#ifdef QUAKE2
	VectorAdd (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);
#endif
	VectorScale (ent->v.velocity, host_frametime, move);
//	VectorCopy(vec_origin,move);
	trace = SV_PushEntity (ent, move);
#ifdef QUAKE2
	VectorSubtract (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);
#endif
	if (trace.fraction == 1)
		return;
	if (ent->free)
		return;

	if (ent->v.movetype == MOVETYPE_BOUNCE)
		backoff = 1.5;
//#ifdef QUAKE2
	else if (ent->v.movetype == MOVETYPE_BOUNCEMISSILE)
	{	// Solid phased missiles don't bounce on monsters or players
		if ( (ent->v.solid == SOLID_PHASE) && (((int)trace.ent->v.flags & FL_MONSTER) || ((int)trace.ent->v.movetype == MOVETYPE_WALK)) )
		{
			return;
		}
		backoff = 2.0;
	}
//#endif
	else
		backoff = 1;

	ClipVelocity (ent->v.velocity, trace.plane.normal, ent->v.velocity, backoff);

// stop if on ground
	if ((trace.plane.normal[2] > 0.7) && (ent->v.movetype != MOVETYPE_BOUNCEMISSILE))
	{
//#ifdef QUAKE2
		if (ent->v.velocity[2] < 60 || ent->v.movetype != MOVETYPE_BOUNCE)
//#else
//		if (ent->v.velocity[2] < 60 || ent->v.movetype != MOVETYPE_BOUNCE)
//#endif
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
=============
*/
#ifdef QUAKE2
static void SV_Physics_Step (edict_t *ent)
{
	qboolean	wasonground;
	qboolean	inwater;
	qboolean	hitsound = false;
	float		*vel;
	float		speed, newspeed, control;
	float		friction;
	edict_t		*groundentity;

	groundentity = PROG_TO_EDICT(ent->v.groundentity);
	if ((int)groundentity->v.flags & FL_CONVEYOR)
		VectorScale(groundentity->v.movedir, groundentity->v.speed, ent->v.basevelocity);
	else
		VectorCopy(vec_origin, ent->v.basevelocity);
//@@
	*sv_globals.time = sv.time;
	*sv_globals.self = EDICT_TO_PROG(ent);
	PF_WaterMove();

	SV_CheckVelocity (ent);

	wasonground = (int)ent->v.flags & FL_ONGROUND;
//	ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;

	// add gravity except:
	//   flying monsters
	//   swimming monsters who are in the water
	inwater = SV_CheckWater(ent);
	if (! wasonground)
	{
		if ( !((int)ent->v.flags & FL_FLY) )
		{
			if ( !( ((int)ent->v.flags & FL_SWIM) && (ent->v.waterlevel > 0) ) )
			{
				if (ent->v.velocity[2] < sv_gravity.value*-0.1)
					hitsound = true;
				if (!inwater)
					SV_AddGravity (ent);
			}
		}
	}

	if (!VectorCompare(ent->v.velocity, vec_origin) || !VectorCompare(ent->v.basevelocity, vec_origin))
	{
		ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;
		// apply friction
		// let dead monsters who aren't completely onground slide
		if (wasonground)
		{
			if (!(ent->v.health <= 0.0 && !SV_CheckBottom(ent)))
			{
				vel = ent->v.velocity;
				speed = sqrt(vel[0]*vel[0] +vel[1]*vel[1]);
				if (speed)
				{
					friction = sv_friction.value;

					control = speed < sv_stopspeed.value ? sv_stopspeed.value : speed;
					newspeed = speed - host_frametime*control*friction;

					if (newspeed < 0)
						newspeed = 0;
					newspeed /= speed;

					vel[0] = vel[0] * newspeed;
					vel[1] = vel[1] * newspeed;
				}
			}
		}

		VectorAdd (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);
		SV_FlyMove (ent, host_frametime, NULL);
		VectorSubtract (ent->v.velocity, ent->v.basevelocity, ent->v.velocity);

		// determine if it's on solid ground at all
		{
			vec3_t	mins, maxs, point;
			int		x, y;

			VectorAdd (ent->v.origin, ent->v.mins, mins);
			VectorAdd (ent->v.origin, ent->v.maxs, maxs);

			point[2] = mins[2] - 1;
			for (x = 0; x <= 1; x++)
			{
				for (y = 0; y <= 1; y++)
				{
					point[0] = x ? maxs[0] : mins[0];
					point[1] = y ? maxs[1] : mins[1];
					if (SV_PointContents (point) == CONTENTS_SOLID)
					{
						ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
						break;
					}
				}
			}
		}

		SV_LinkEdict (ent, true);

		if (((int)ent->v.flags & FL_ONGROUND) && !((int)ent->v.flags & FL_MONSTER))
		{
			if (!wasonground)
				if (hitsound)
					SV_StartSound (ent, 0, "fx/thngland.wav", 255, 1);
		}
	}

// regular thinking
	SV_RunThink (ent);
	SV_CheckWaterTransition (ent);
}
#else
static void SV_Physics_Step (edict_t *ent)
{
	qboolean	hitsound;

// freefall if not onground
	if ( ! ((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM) ) )
	{
		if (ent->v.velocity[2] < sv_gravity.value*-0.1)
			hitsound = true;
		else
			hitsound = false;

		SV_AddGravity (ent);
		SV_CheckVelocity (ent);
		SV_FlyMove (ent, host_frametime, NULL);
		SV_LinkEdict (ent, true);

	//	if ( (int)ent->v.flags & FL_ONGROUND )	// just hit ground
		if (((int)ent->v.flags & FL_ONGROUND) && !((int)ent->v.flags & FL_MONSTER))
		{
			if (hitsound)
				SV_StartSound (ent, 0, "fx/thngland.wav", 255, 1);
		}
	}

// regular thinking
	SV_RunThink (ent);

	SV_CheckWaterTransition (ent);
}
#endif


//============================================================================

/*
================
SV_Physics

================
*/
void SV_Physics (void)
{
	int		i, c, originMoved;
	int		entity_cap;
	edict_t	*ent, *ent2;
	vec3_t	oldOrigin, oldAngle;

// let the progs know that a new frame has started
	*sv_globals.self = EDICT_TO_PROG(sv.edicts);
	*sv_globals.other = EDICT_TO_PROG(sv.edicts);
	*sv_globals.time = sv.time;
	PR_ExecuteProgram (*sv_globals.StartFrame, "StartFrame");

	//SV_CheckAllEnts ();

//
// treat each object in turn
//
	ent = sv.edicts;
	VectorClear(oldOrigin);	// avoid compiler warning
	VectorClear(oldAngle);	// avoid compiler warning	
	/* Edict 0 is the world and 1..maxclients are the players, so capping the
	 * walk there runs physics for clients only. */
	entity_cap = sv_freezenonclients.integer ? svs.maxclients + 1 : sv.num_edicts;
	if (entity_cap > sv.num_edicts)
		entity_cap = sv.num_edicts;	/* never walk past what is allocated */

	/* An edict created during this walk -- a missile from a player's think,
	 * say -- waits for the next frame, so it is first seen where it was
	 * spawned.  It used to depend on the slot: one appended past entity_cap
	 * waited, one reusing a freed slot after its spawner's was moved a whole
	 * tick at once, and after the first few shots nearly every missile took
	 * a freed slot -- the Paladin's purifier first showed 14 units out at 72
	 * Hz and 50 at 20 (measured on #138).  Decided there: every new edict
	 * waits, at any tick rate.  Vanilla moved both kinds in their first
	 * frame, so a projectile now starts one tick later than it did there.
	 * Edicts spawned outside the walk (map load, StartFrame) are unaffected. */
	{
		static unsigned int	walks;	/* unsigned: wrapping is defined */

		if (++walks > 0x7fffffffu)
			walks = 1;	/* stays a positive int; 0 means "not in a walk" */
		sv_physics_walk = (int) walks;
	}

	for (i = 0; i < entity_cap; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		if (ent->spawnwalk == sv_physics_walk && i > svs.maxclients)
			continue;	/* created during this walk: next frame */

		ent2 = PROG_TO_EDICT(ent->v.movechain);
		if (ent2 != sv.edicts)
		{
			VectorCopy(ent->v.origin,oldOrigin);
			VectorCopy(ent->v.angles,oldAngle);
		}

		if (*sv_globals.force_retouch)
		{
			SV_LinkEdict (ent, true);	// force retouch even for stationary
		}

		if (i > 0 && i <= svs.maxclients)
			SV_Physics_Client (ent, i);
		else if (ent->v.movetype == MOVETYPE_PUSH)
			SV_Physics_Pusher (ent);
		else if (ent->v.movetype == MOVETYPE_NONE)
			SV_Physics_None (ent);
#ifdef QUAKE2
		else if (ent->v.movetype == MOVETYPE_FOLLOW)
			SV_Physics_Follow (ent);
#endif
		else if (ent->v.movetype == MOVETYPE_NOCLIP)
			SV_Physics_Noclip (ent);
		else if ((ent->v.movetype == MOVETYPE_STEP) ||
				(ent->v.movetype == MOVETYPE_PUSHPULL))
			SV_Physics_Step (ent);
		else if (ent->v.movetype == MOVETYPE_TOSS
				|| ent->v.movetype == MOVETYPE_BOUNCE
//#ifdef QUAKE2
				|| ent->v.movetype == MOVETYPE_BOUNCEMISSILE
//#endif
				|| ent->v.movetype == MOVETYPE_FLY
				|| ent->v.movetype == MOVETYPE_FLYMISSILE
				|| ent->v.movetype == MOVETYPE_SWIM)
			SV_Physics_Toss (ent);
		else
			Sys_Error ("%s: bad movetype %i", __thisfunc__, (int)ent->v.movetype);

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
						PR_ExecuteProgram(ent2->v.chainmoved, "chainmoved");
					}

					ent2 = PROG_TO_EDICT(ent2->v.movechain);
					if (ent2 == sv.edicts)
						break;
				}
			}
		}
	}

	sv_physics_walk = 0;

	if (*sv_globals.force_retouch)
		(*sv_globals.force_retouch)--;

	/* Hold server time too, or frozen entities still age: think times fall
	 * behind sv.time and every one of them fires at once on unfreeze. */
	if (!sv_freezenonclients.integer)
		sv.time += host_frametime;
}


#ifdef QUAKE2
trace_t SV_Trace_Toss (edict_t *ent, edict_t *ignore)
{
	edict_t	tempent, *tent;
	trace_t	trace;
	vec3_t	move;
	vec3_t	end;
	double	save_frametime;
//	extern particle_t	*active_particles, *free_particles;
//	particle_t	*p;

	save_frametime = host_frametime;
	host_frametime = 0.05;

	memcpy(&tempent, ent, sizeof(edict_t));
	tent = &tempent;

	while (1)
	{
		SV_CheckVelocity (tent);
		SV_AddGravity (tent);
		VectorMA (tent->v.angles, host_frametime, tent->v.avelocity, tent->v.angles);
		VectorScale (tent->v.velocity, host_frametime, move);
		VectorAdd (tent->v.origin, move, end);
		trace = SV_Move (tent->v.origin, tent->v.mins, tent->v.maxs, end, MOVE_NORMAL, tent);
		VectorCopy (trace.endpos, tent->v.origin);

//		p = free_particles;
//		if (p)
//		{
//			free_particles = p->next;
//			p->next = active_particles;
//			active_particles = p;
//
//			p->die = 256;
//			p->color = 15;
//			p->type = pt_static;
//			VectorClear (p->vel);
//			VectorCopy (tent->v.origin, p->org);
//		}

		if (trace.ent)
			if (trace.ent != ignore)
				break;
	}
//	p->color = 224;
	host_frametime = save_frametime;
	return trace;
}
#endif

