/*
 * sv_phys_walk_hw.c -- the C original of sv_phys.c's client movement,
 * moved verbatim out of engine/hexenworld/server/sv_phys.c: the client movement, all of it under #if 0 but the second SV_Physics_Client.
 * The differential harness beside this file includes it; no engine target
 * compiles it.  The engine's implementation is engine/rust/src/sv_phys_walk.rs.
 *
 * (from sv_phys.c -- sv physics)
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

/* sv_phys.c's own include, quakedef.h, and its cvars come from the harness. */

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
#if 0
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
		Con_DPrintf ("Unstuck.\n");
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
					Con_DPrintf ("Unstuck.\n");
					SV_LinkEdict (ent, true);
					return;
				}
			}
		}
	}

	VectorCopy (org, ent->v.origin);
	Con_DPrintf ("player is stuck.\n");
}
#endif


/*
=============
SV_CheckWater
=============
*/
#if 0
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
#endif


/*
============
SV_WallFriction

============
*/
#if 0
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
#endif


/*
=====================
SV_TryUnstick

Player has come to a dead stop, possibly due to the problem with limited
float precision at some angle joins in the BSP hull.

Try fixing by pushing one pixel in each direction.

This is a hack, but in the interest of good gameplay...
======================
*/
#if 0
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
#endif


/*
=====================
SV_WalkMove

Only used by players
======================
*/
#define	STEPSIZE	18
#if 0
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
	//Con_Printf("Calling pushent\n");
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
#endif


/*
================
SV_Physics_Client

Player character actions
================
*/
#if 0	// Note: this version requires sevenal other if 0'ed out functions
void SV_Physics_Client (edict_t *ent, int num)
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
		Host_Error ("%s: bad movetype %i", __thisfunc__, (int)ent->v.movetype);
	}

//
// call standard player post-think
//
	SV_LinkEdict (ent, true);

	*sv_globals.time = sv.time;
	*sv_globals.self = EDICT_TO_PROG(ent);
	PR_ExecuteProgram (*sv_globals.PlayerPostThink, "PlayerPostThink");
}
#else
void SV_Physics_Client (edict_t	*ent)
{
	trace_t	trace;
	//int		save_hull;

//	save_hull = ent->v.hull;
//	ent->v.hull = 1;
//	trace = SV_Move (ent->v.oldorigin, vec_origin, vec_origin, ent->v.origin, MOVE_NOMONSTERS, ent);
	trace = SV_Move (ent->v.oldorigin, ent->v.mins, ent->v.maxs, ent->v.origin, MOVE_NOMONSTERS, ent);
//	ent->v.hull = save_hull;

	if (trace.fraction < 1.0)
		return;

	trace = SV_Move (ent->v.oldorigin, ent->v.mins, ent->v.maxs, ent->v.origin, MOVE_NORMAL, ent);

	if (ent->v.movetype != MOVETYPE_BOUNCE || (trace.allsolid == 0 && trace.startsolid == 0))
	{
		VectorCopy (trace.endpos, ent->v.origin);
	}
	else
	{
		trace.fraction = 0;
		return;
	}

	if (trace.ent)
		SV_Impact (ent, trace.ent);

	return;
}
#endif


//============================================================================
