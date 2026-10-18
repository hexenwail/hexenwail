/*
 * sv_phys_move_h2.c -- the C original of sv_phys.c's movetype dispatch,
 * moved verbatim out of engine/hexen2/sv_phys.c: SV_Physics_None to the end of the file.
 * The differential harness beside this file includes it; no engine target
 * compiles it.  The engine's implementation is engine/rust/src/sv_phys_move.rs.
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
void SV_Physics_Toss (edict_t *ent)
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
