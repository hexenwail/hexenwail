/*
 * sv_phys_base_hw.c -- the C original of sv_phys.c's base helpers,
 * moved verbatim out of engine/hexenworld/server/sv_phys.c: SV_CheckVelocity through
 * SV_PushEntity.  The differential harness beside this file includes it; no
 * engine target compiles it.  The engine's implementation is
 * engine/rust/src/sv_phys.rs.
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
================
SV_CheckVelocity
================
*/
void SV_CheckVelocity (edict_t *ent)
{
	int		i;
	float		w;

//
// bound velocity
//
	for (i = 0; i < 3; i++)
	{
		if (IS_NAN(ent->v.velocity[i]))
		{
			Con_Printf ("Got a NaN velocity on %s\n", PR_GetString(ent->v.classname));
			ent->v.velocity[i] = 0;
		}
		if (IS_NAN(ent->v.origin[i]))
		{
			Con_Printf ("Got a NaN origin on %s\n", PR_GetString(ent->v.classname));
			ent->v.origin[i] = 0;
		}
	}

	w = VectorLength(ent->v.velocity);
	if (w > sv_maxvelocity.value)
	{	// sv_maxvelocity fix by Maddes
		VectorScale (ent->v.velocity, sv_maxvelocity.value/w, ent->v.velocity);
	}
}


/*
=============
SV_RunThink

Runs thinking code if time.  There is some play in the exact time the think
function will be called, because it is called before any movement is done
in a frame.  Not used for pushmove objects, because they must be exact.
Returns false if the entity removed itself.
=============
*/
qboolean SV_RunThink (edict_t *ent)
{
	float	thinktime;

	thinktime = ent->v.nextthink;
	if (thinktime <= 0)
	{
		return true;
	}
	if (thinktime > sv.time + host_frametime)
	{
		return true;
	}

	if (thinktime < sv.time)
		thinktime = sv.time;	// don't let things stay in the past.
					// it is possible to start that way
					// by a trigger with a local time.
	ent->v.nextthink = 0;
	*sv_globals.time = thinktime;
	*sv_globals.self = EDICT_TO_PROG(ent);
	*sv_globals.other = EDICT_TO_PROG(sv.edicts);
	PR_ExecuteProgram (ent->v.think, "think in SV_RunThink");

	if (ent->free)
	{
		return false;
	}

	return true;
}


/*
==================
SV_Impact

Two entities have touched, so run their touch functions
==================
*/
void SV_Impact (edict_t *e1, edict_t *e2)
{
	int		old_self, old_other;

	old_self = *sv_globals.self;
	old_other = *sv_globals.other;

	*sv_globals.time = sv.time;
	if (e1->v.touch && e1->v.solid != SOLID_NOT)
	{
		*sv_globals.self = EDICT_TO_PROG(e1);
		*sv_globals.other = EDICT_TO_PROG(e2);
		PR_ExecuteProgram (e1->v.touch, "touch");
	}

	if (e2->v.touch && e2->v.solid != SOLID_NOT)
	{
		*sv_globals.self = EDICT_TO_PROG(e2);
		*sv_globals.other = EDICT_TO_PROG(e1);
		PR_ExecuteProgram (e2->v.touch, "touch");
	}

	*sv_globals.self = old_self;
	*sv_globals.other = old_other;
}


/*
==================
ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define	STOP_EPSILON	0.1

static int ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	float	backoff;
	float	change;
	int		i, blocked;

	blocked = 0;
	if (normal[2] > 0)
		blocked |= 1;	// floor
	if (!normal[2])
		blocked |= 2;	// step

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++)
	{
		change = normal[i]*backoff;
		out[i] = in[i] - change;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}

	return blocked;
}


/*
============
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
Returns the clipflags if the velocity was modified (hit something solid)
1 = floor
2 = wall / step
4 = dead stop
If steptrace is not NULL, the trace of any vertical wall hit will be stored
============
*/
#define	MAX_CLIP_PLANES	5
static int SV_FlyMove (edict_t *ent, float time, trace_t *steptrace)
{
	int			bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity, new_velocity;
	int			i, j;
	trace_t		trace;
	vec3_t		end;
	float		time_left;
	int			blocked;

	numbumps = 4;

	blocked = 0;
	VectorCopy (ent->v.velocity, original_velocity);
	VectorCopy (ent->v.velocity, primal_velocity);
	numplanes = 0;

	time_left = time;

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		if (!ent->v.velocity[0] && !ent->v.velocity[1] && !ent->v.velocity[2])
			break;

		for (i = 0; i < 3; i++)
			end[i] = ent->v.origin[i] + time_left * ent->v.velocity[i];

		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, false, ent);

		if (trace.allsolid)
		{	// entity is trapped in another solid
			VectorClear (ent->v.velocity);
			return 3;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, ent->v.origin);
			VectorCopy (ent->v.velocity, original_velocity);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			break;		// moved the entire distance

		if (!trace.ent)
			SV_Error ("%s: !trace.ent", __thisfunc__);

		if (trace.plane.normal[2] > 0.7)
		{
			blocked |= 1;		// floor
			if (trace.ent->v.solid == SOLID_BSP)
			{
				ent->v.flags =	(int)ent->v.flags | FL_ONGROUND;
				ent->v.groundentity = EDICT_TO_PROG(trace.ent);
			}
		}
		if (!trace.plane.normal[2])
		{
			blocked |= 2;		// step
			if (steptrace)
				*steptrace = trace;	// save for player extrafriction
		}

//
// run the impact function
//
		SV_Impact (ent, trace.ent);
		if (ent->free)
			break;		// removed by the impact function

		time_left -= time_left * trace.fraction;

	// cliped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorClear (ent->v.velocity);
			return 3;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

//
// modify original_velocity so it parallels all of the clip planes
//
		for (i = 0; i < numplanes; i++)
		{
			ClipVelocity (original_velocity, planes[i], new_velocity, 1);
			for (j = 0; j < numplanes; j++)
			{
				if (j != i)
				{
					if (DotProduct (new_velocity, planes[j]) < 0)
						break;	// not ok
				}
			}

			if (j == numplanes)
				break;
		}

		if (i != numplanes)
		{	// go along this plane
			VectorCopy (new_velocity, ent->v.velocity);
		}
		else
		{	// go along the crease
			if (numplanes != 2)
			{
			//	Con_Printf ("clip velocity, numplanes == %i\n",numplanes);
				VectorClear (ent->v.velocity);
				return 7;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, ent->v.velocity);
			VectorScale (dir, d, ent->v.velocity);
		}

//
// if original velocity is against the original velocity, stop dead
// to avoid tiny occilations in sloping corners
//
		if (DotProduct (ent->v.velocity, primal_velocity) <= 0)
		{
			VectorClear (ent->v.velocity);
			return blocked;
		}
	}

	return blocked;
}


/*
============
SV_FlyExtras

============
*/
#if 0
static const float hoverinc = 0.4;
static void SV_FlyExtras (edict_t *ent, float time, trace_t *steptrace)
{
	// Jumping makes you loose this flag so reset it
	ent->v.flags = (int) ent->v.flags | FL_ONGROUND;

	if ((ent->v.velocity[2] <= 6) && (ent->v.velocity[2] >= -6))
	{
		ent->v.velocity[2] += ent->v.hoverz;

		if (ent->v.velocity[2] >= 6)
		{
			ent->v.hoverz = -hoverinc;
			ent->v.velocity[2] += ent->v.hoverz;
		}
		else if (ent->v.velocity[2] <= -6)
		{
			ent->v.hoverz = hoverinc;
			ent->v.velocity[2] += ent->v.hoverz;
		}
	}
	else	// friction for upward or downward progress once key is released
	{
		ent->v.velocity[2] -= sv_player->v.velocity[2] * .1;
	}
}
#endif


/*
============
SV_AddGravity

============
*/
void SV_AddGravity (edict_t *ent, float scale)
{
	ent->v.velocity[2] -= scale * movevars.gravity * host_frametime;
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
trace_t SV_PushEntity (edict_t *ent, vec3_t push)
{
	trace_t	trace;
	vec3_t	start, end, impact;
	edict_t *impact_e;

	VectorCopy (ent->v.origin, start);
	(void) start; /* variable set but not used */
	VectorAdd (ent->v.origin, push, end);
//	if ((int)ent->v.flags & FL_CLIENT)
//		Con_Printf("Player exec pushent\n");

	if (ent->v.movetype == MOVETYPE_FLYMISSILE  || ent->v.movetype == MOVETYPE_BOUNCEMISSILE)
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_MISSILE, ent);
	else if (ent->v.solid == SOLID_TRIGGER || ent->v.solid == SOLID_NOT)
// only clip against bmodels
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_NOMONSTERS, ent);
	else if (ent->v.movetype == MOVETYPE_SWIM)
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_WATER, ent);
	else
		trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_NORMAL, ent);

	if (ent->v.solid != SOLID_PHASE)
	{
		if (ent->v.movetype != MOVETYPE_BOUNCE || (trace.allsolid == 0 && trace.startsolid == 0))
		{
			VectorCopy (trace.endpos, ent->v.origin);   // Macro - watchout
		}
		else
		{
			trace.fraction = 0;

			return trace;
		}
	}
	else	// Entity is PHASED so bounce off walls and other entities, go through monsters and players
	{
		if (trace.ent)
		{	// Go through MONSTERS and PLAYERS, can't use FL_CLIENT cause rotating brushes do
			if (((int) trace.ent->v.flags & FL_MONSTER) || (trace.ent->v.movetype == MOVETYPE_WALK))
			{
				VectorCopy (trace.endpos, impact);
				impact_e = trace.ent;

				trace = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_PHASE, ent);

				VectorCopy (impact, ent->v.origin);
				SV_Impact (ent, impact_e);

				VectorCopy (trace.endpos, ent->v.origin);
			}
			else
			{
				VectorCopy (trace.endpos, ent->v.origin);
			}
		}
		else
		{
			VectorCopy (trace.endpos, ent->v.origin);
		}
	}

	SV_LinkEdict (ent, true);

	if (trace.ent)
		SV_Impact (ent, trace.ent);

	return trace;
}
