/*
 * sv_phys_push_hw.c -- the C original of sv_phys.c's pushers, moved
 * verbatim out of engine/hexenworld/server/sv_phys.c: SV_Push through SV_Physics_Pusher.  The
 * differential harness beside this file includes it; no engine target
 * compiles it.  The engine's implementation is engine/rust/src/sv_phys_push.rs.
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
============
SV_Push

============
*/
static qboolean SV_Push (edict_t *pusher, vec3_t move)
{
	int			i, e;
	edict_t		*check, *block;
	vec3_t		mins, maxs;
	vec3_t		pushorig;
	int			num_moved;
	edict_t		*moved_edict[MAX_EDICTS];
	vec3_t		moved_from[MAX_EDICTS];

	for (i = 0; i < 3; i++)
	{
		mins[i] = pusher->v.absmin[i] + move[i];
		maxs[i] = pusher->v.absmax[i] + move[i];
	}

	VectorCopy (pusher->v.origin, pushorig);

	// move the pusher to it's final position
	VectorAdd (pusher->v.origin, move, pusher->v.origin);
	SV_LinkEdict (pusher, false);

	// see if any solid entities are inside the final position
	num_moved = 0;
	check = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.movetype == MOVETYPE_PUSH
				|| check->v.movetype == MOVETYPE_NONE
				|| check->v.movetype == MOVETYPE_NOCLIP)
			continue;

		pusher->v.solid = SOLID_NOT;
		block = SV_TestEntityPosition (check);
		pusher->v.solid = SOLID_BSP;
		if (block)
			continue;

	// if the entity is standing on the pusher, it will definately be moved
		if ( ! ( ((int)check->v.flags & FL_ONGROUND)
			&& PROG_TO_EDICT(check->v.groundentity) == pusher) )
		{
			if ( check->v.absmin[0] >= maxs[0]
					|| check->v.absmin[1] >= maxs[1]
					|| check->v.absmin[2] >= maxs[2]
					|| check->v.absmax[0] <= mins[0]
					|| check->v.absmax[1] <= mins[1]
					|| check->v.absmax[2] <= mins[2] )
				continue;

		// see if the ent's bbox is inside the pusher's final position
			if (!SV_TestEntityPosition (check))
				continue;
		}

		VectorCopy (check->v.origin, moved_from[num_moved]);
		moved_edict[num_moved] = check;
		num_moved++;

		// try moving the contacted entity
		VectorAdd (check->v.origin, move, check->v.origin);
		block = SV_TestEntityPosition (check);
		if (!block)
		{	// pushed ok
			SV_LinkEdict (check, false);
			continue;
		}

		// if it is ok to leave in the old position, do it
		VectorSubtract (check->v.origin, move, check->v.origin);
		block = SV_TestEntityPosition (check);
		if (!block)
		{
			num_moved--;
			continue;
		}

		// if it is still inside the pusher, block
		if (check->v.mins[0] == check->v.maxs[0])
		{
			SV_LinkEdict (check, false);
			continue;
		}
		if (check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER)
		{	// corpse
			check->v.mins[0] = check->v.mins[1] = 0;
			VectorCopy (check->v.mins, check->v.maxs);
			SV_LinkEdict (check, false);
			continue;
		}

		VectorCopy (pushorig, pusher->v.origin);
		SV_LinkEdict (pusher, false);

		// if the pusher has a "blocked" function, call it
		// otherwise, just stay in place until the obstacle is gone
		if (pusher->v.blocked)
		{
			*sv_globals.self = EDICT_TO_PROG(pusher);
			*sv_globals.other = EDICT_TO_PROG(check);
			PR_ExecuteProgram (pusher->v.blocked, "blocked");
		}

		// move back any entities we already moved
		for (i = 0; i < num_moved; i++)
		{
			VectorCopy (moved_from[i], moved_edict[i]->v.origin);
			SV_LinkEdict (moved_edict[i], false);
		}
		return false;
	}

	return true;
}


/*
============
SV_PushMove

============
*/
static void SV_PushMove (edict_t *pusher, float movetime, qboolean update_time)
{
	int			i;
	vec3_t		move;

	if (!pusher->v.velocity[0] && !pusher->v.velocity[1] && !pusher->v.velocity[2])
	{
		if (update_time)
		{
			pusher->v.ltime += movetime;
		}
		return;
	}

	for (i = 0; i < 3; i++)
		move[i] = pusher->v.velocity[i] * movetime;

	if (SV_Push (pusher, move))
	{
		if (update_time)
		{
			pusher->v.ltime += movetime;
		}
	}
}


/*
============
SV_PushRotate
Pre-Mission Pack fix
============
*/
#if 0	// Pre-Mission Pack fix
static void SV_PushRotate (edict_t *pusher, float movetime)
{
	int			i, e;
	edict_t		*check, *block;
	vec3_t		move, a, amove;
	vec3_t		entorig, pushorig;
	int			num_moved;
	edict_t		*moved_edict[MAX_EDICTS];
	vec3_t		moved_from[MAX_EDICTS];
	vec3_t		org, org2;
	vec3_t		forward, right, up;
	edict_t		*ground;
//	edict_t		*master;
	edict_t		*slave;
	int			slaves_moved;
	qboolean	moveit;

#   if 0
	Con_DPrintf("%s entity %i (time=%f)\n", __thisfunc__, NUM_FOR_EDICT(pusher), movetime);
	Con_DPrintf("%f %f %f (avelocity)\n", pusher->v.avelocity[0], pusher->v.avelocity[1], pusher->v.avelocity[2]);
	Con_DPrintf("%f %f %f\n", pusher->v.angles[0], pusher->v.angles[1], pusher->v.angles[2]);
#   endif

	for (i = 0; i < 3; i++)
		amove[i] = pusher->v.avelocity[i] * movetime;

	VectorNegate (amove, a);
	AngleVectors (a, forward, right, up);

	VectorCopy (pusher->v.angles, pushorig);

	// move the pusher to it's final position
	VectorAdd (pusher->v.angles, amove, pusher->v.angles);

	pusher->v.ltime += movetime;
	SV_LinkEdict (pusher, false);

	slaves_moved = 0;
/*	master = pusher;
	while (master->v.aiment)
	{
		slave = PROG_TO_EDICT(master->v.aiment);

	//	Con_DPrintf("%f %f %f   slave entity %i\n", slave->v.angles[0], slave->v.angles[1], slave->v.angles[2], NUM_FOR_EDICT(slave));

		slaves_moved++;
		VectorCopy (slave->v.angles, moved_from[MAX_EDICTS - slaves_moved]);
		moved_edict[MAX_EDICTS - slaves_moved] = slave;

		if (slave->v.movedir[PITCH])
			slave->v.angles[PITCH] = master->v.angles[PITCH];
		else
			slave->v.angles[PITCH] += slave->v.avelocity[PITCH] * movetime;

		if (slave->v.movedir[YAW])
			slave->v.angles[YAW] = master->v.angles[YAW];
		else
			slave->v.angles[YAW] += slave->v.avelocity[YAW] * movetime;

		if (slave->v.movedir[ROLL])
			slave->v.angles[ROLL] = master->v.angles[ROLL];
		else
			slave->v.angles[ROLL] += slave->v.avelocity[ROLL] * movetime;

		slave->v.ltime = master->v.ltime;
		SV_LinkEdict (slave, false);

		master = slave;
	}
*/

	// see if any solid entities are inside the final position
	num_moved = 0;
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

		// if the entity is standing on the pusher, it will definitely be moved
		moveit = false;
		ground = PROG_TO_EDICT(check->v.groundentity);
		if ((int)check->v.flags & FL_ONGROUND)
		{
			if (ground == pusher)
			{
				moveit = true;
			}
			else
			{
				for (i = 0; i < slaves_moved; i++)
				{
					if (ground == moved_edict[MAX_EDICTS - i - 1])
					{
						moveit = true;
						break;
					}
				}
			}
		}

		if (!moveit)
		{
			if ( check->v.absmin[0] >= pusher->v.absmax[0]
					|| check->v.absmin[1] >= pusher->v.absmax[1]
					|| check->v.absmin[2] >= pusher->v.absmax[2]
					|| check->v.absmax[0] <= pusher->v.absmin[0]
					|| check->v.absmax[1] <= pusher->v.absmin[1]
					|| check->v.absmax[2] <= pusher->v.absmin[2] )
			{
				for (i = 0; i < slaves_moved; i++)
				{
					slave = moved_edict[MAX_EDICTS - i - 1];
					if ( check->v.absmin[0] >= slave->v.absmax[0]
							|| check->v.absmin[1] >= slave->v.absmax[1]
							|| check->v.absmin[2] >= slave->v.absmax[2]
							|| check->v.absmax[0] <= slave->v.absmin[0]
							|| check->v.absmax[1] <= slave->v.absmin[1]
							|| check->v.absmax[2] <= slave->v.absmin[2] )
						continue;
				}
				if (i == slaves_moved)
					continue;
			}

		// see if the ent's bbox is inside the pusher's final position
			if (!SV_TestEntityPosition (check))
				continue;
		}

		// remove the onground flag for non-players
		if (check->v.movetype != MOVETYPE_WALK)
			check->v.flags = (int)check->v.flags & ~FL_ONGROUND;

		VectorCopy (check->v.origin, entorig);
		VectorCopy (check->v.origin, moved_from[num_moved]);
		moved_edict[num_moved] = check;
		num_moved++;

		// calculate destination position
		VectorSubtract (check->v.origin, pusher->v.origin, org);
		org2[0] = DotProduct (org, forward);
		org2[1] = -DotProduct (org, right);
		org2[2] = DotProduct (org, up);
		VectorSubtract (org2, org, move);

		// try moving the contacted entity
		pusher->v.solid = SOLID_NOT;
		SV_PushEntity (check, move);
	//@@TODO: do we ever want to do anybody's angles?  maybe just yaw???
	//	if (!((int)check->v.flags & (FL_CLIENT | FL_MONSTER)))
	//		VectorAdd (check->v.angles, amove, check->v.angles);
		check->v.angles[YAW] += amove[YAW];

		pusher->v.solid = SOLID_BSP;

		// if it is still inside the pusher, block
		block = SV_TestEntityPosition (check);
		if (block)
		{	// fail the move
			if (check->v.mins[0] == check->v.maxs[0])
				continue;
			if (check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER)
			{	// corpse
				check->v.mins[0] = check->v.mins[1] = 0;
				VectorCopy (check->v.mins, check->v.maxs);
				continue;
			}

			VectorCopy (entorig, check->v.origin);
			SV_LinkEdict (check, true);

			VectorCopy (pushorig, pusher->v.angles);
			SV_LinkEdict (pusher, false);
			pusher->v.ltime -= movetime;

			for (i = 0; i < slaves_moved; i++)
			{
				slave = moved_edict[MAX_EDICTS - i - 1];
				VectorCopy (moved_from[MAX_EDICTS - i - 1], slave->v.angles);
				SV_LinkEdict (slave, false);
				slave->v.ltime -= movetime;
			}

			// if the pusher has a "blocked" function, call it
			// otherwise, just stay in place until the obstacle is gone
			if (pusher->v.blocked)
			{
				*sv_globals.self = EDICT_TO_PROG(pusher);
				*sv_globals.other = EDICT_TO_PROG(check);
				PR_ExecuteProgram (pusher->v.blocked, "blocked");
			}

			// move back any entities we already moved
			for (i = 0; i < num_moved; i++)
			{
				VectorCopy (moved_from[i], moved_edict[i]->v.origin);
			//@@TODO:: see above
			//	if (!((int)moved_edict[i]->v.flags & (FL_CLIENT | FL_MONSTER)))
			//		VectorSubtract (moved_edict[i]->v.angles, amove, moved_edict[i]->v.angles);
				moved_edict[i]->v.angles[YAW] -= amove[YAW];

				SV_LinkEdict (moved_edict[i], false);
			}
			return;
		}
	}

#   if 0
	Con_DPrintf("result:\n");
	Con_DPrintf("%f %f %f\n", pusher->v.angles[0], pusher->v.angles[1], pusher->v.angles[2]);
	for (i = 0; i < slaves_moved; i++)
	{
		slave = moved_edict[MAX_EDICTS - i - 1];
		Con_DPrintf("%f %f %f   slave entity %i\n", slave->v.angles[0], slave->v.angles[1], slave->v.angles[2], NUM_FOR_EDICT(slave));
	}
	Con_DPrintf("\n");
#   endif
}
#endif	// end of Pre-Mission Pack fix

/*
============
SV_PushRotate
NEW
============
*/
static void SV_PushRotate (edict_t *pusher, float movetime)
{
	int			i, e, t;
	edict_t		*check, *block;
	vec3_t		move, a, amove, mins, maxs, move2, move3, testmove;
//	vec3_t		amove_norm;
	vec3_t		entorig, pushorig, pushorigangles;
	int			num_moved;
	edict_t		*moved_edict[MAX_EDICTS];
	vec3_t		moved_from[MAX_EDICTS];
	vec3_t		org, org2, check_center;
	vec3_t		forward, right, up;
	edict_t		*ground;
//	edict_t		*master;
	edict_t		*slave;
	int			slaves_moved;
	qboolean	moveit;
//	float		turn_away, amove_mag;

#   if 0
	Con_DPrintf("%s entity %i (time=%f)\n", __thisfunc__, NUM_FOR_EDICT(pusher), movetime);
	Con_DPrintf("%f %f %f (avelocity)\n", pusher->v.avelocity[0], pusher->v.avelocity[1], pusher->v.avelocity[2]);
	Con_DPrintf("%f %f %f\n", pusher->v.angles[0], pusher->v.angles[1], pusher->v.angles[2]);
#   endif

	for (i = 0; i < 3; i++)
	{
		amove[i] = pusher->v.avelocity[i] * movetime;
		move[i] = pusher->v.velocity[i] * movetime;
		mins[i] = pusher->v.absmin[i] + move[i];
		maxs[i] = pusher->v.absmax[i] + move[i];
	}

	VectorNegate (amove, a);
	AngleVectors (a, forward, right, up);

	VectorCopy (pusher->v.origin, pushorig);
	VectorCopy (pusher->v.angles, pushorigangles);

	// move the pusher to it's final position
	VectorAdd (pusher->v.origin, move, pusher->v.origin);
	VectorAdd (pusher->v.angles, amove, pusher->v.angles);

	pusher->v.ltime += movetime;
	SV_LinkEdict (pusher, false);

	slaves_moved = 0;
/*	master = pusher;
	while (master->v.aiment)
	{
		slave = PROG_TO_EDICT(master->v.aiment);

	//	Con_DPrintf("%f %f %f   slave entity %i\n", slave->v.angles[0], slave->v.angles[1], slave->v.angles[2], NUM_FOR_EDICT(slave));

		slaves_moved++;
		VectorCopy (slave->v.angles, moved_from[MAX_EDICTS - slaves_moved]);
		moved_edict[MAX_EDICTS - slaves_moved] = slave;

		if (slave->v.movedir[PITCH])
			slave->v.angles[PITCH] = master->v.angles[PITCH];
		else
			slave->v.angles[PITCH] += slave->v.avelocity[PITCH] * movetime;

		if (slave->v.movedir[YAW])
			slave->v.angles[YAW] = master->v.angles[YAW];
		else
			slave->v.angles[YAW] += slave->v.avelocity[YAW] * movetime;

		if (slave->v.movedir[ROLL])
			slave->v.angles[ROLL] = master->v.angles[ROLL];
		else
			slave->v.angles[ROLL] += slave->v.avelocity[ROLL] * movetime;

		slave->v.ltime = master->v.ltime;
		SV_LinkEdict (slave, false);

		master = slave;
	}
*/

	// see if any solid entities are inside the final position
	num_moved = 0;
	check = NEXT_EDICT(sv.edicts);
	VectorClear(testmove);	// avoid compiler warning:  testmove is
				// initialized with case 0 in the switch
	for (e = 1; e < sv.num_edicts; e++, check = NEXT_EDICT(check))
	{
		if (check->free)
			continue;
		if (check->v.movetype == MOVETYPE_PUSH
				|| check->v.movetype == MOVETYPE_NONE
				|| check->v.movetype == MOVETYPE_FOLLOW
				|| check->v.movetype == MOVETYPE_NOCLIP)
			continue;

		// if the entity is standing on the pusher, it will definitely be moved
		moveit = false;
		ground = PROG_TO_EDICT(check->v.groundentity);
		if ((int)check->v.flags & FL_ONGROUND)
		{
			if (ground == pusher)
			{
				moveit = true;
			}
			else
			{
				for (i = 0; i < slaves_moved; i++)
				{
					if (ground == moved_edict[MAX_EDICTS - i - 1])
					{
						moveit = true;
						break;
					}
				}
			}
		}

		if (!moveit)
		{
			if ( check->v.absmin[0] >= maxs[0]
				|| check->v.absmin[1] >= maxs[1]
				|| check->v.absmin[2] >= maxs[2]
				|| check->v.absmax[0] <= mins[0]
				|| check->v.absmax[1] <= mins[1]
				|| check->v.absmax[2] <= mins[2] )
			{
				for (i = 0; i < slaves_moved; i++)
				{
					slave = moved_edict[MAX_EDICTS - i - 1];
					if ( check->v.absmin[0] >= slave->v.absmax[0]
							|| check->v.absmin[1] >= slave->v.absmax[1]
							|| check->v.absmin[2] >= slave->v.absmax[2]
							|| check->v.absmax[0] <= slave->v.absmin[0]
							|| check->v.absmax[1] <= slave->v.absmin[1]
							|| check->v.absmax[2] <= slave->v.absmin[2] )
						continue;
				}
				if (i == slaves_moved)
					continue;
			}

		// see if the ent's bbox is inside the pusher's final position
			if (!SV_TestEntityPosition (check))
				continue;
		}

		// remove the onground flag for non-players
		if (check->v.movetype != MOVETYPE_WALK)
			check->v.flags = (int)check->v.flags & ~FL_ONGROUND;

		VectorCopy (check->v.origin, entorig);
		VectorCopy (check->v.origin, moved_from[num_moved]);
		moved_edict[num_moved] = check;
		num_moved++;

		// put check in first move spot
		VectorAdd (check->v.origin, move, check->v.origin);
		// Use center of model, like in QUAKE!!!!
		// Our origins are on the bottom!!!
		for (i = 0; i < 3; i++)
			check_center[i] = (check->v.absmin[i] + check->v.absmax[i]) / 2;
		// calculate destination position
		VectorSubtract (check_center, pusher->v.origin, org);
		// put check back
		VectorSubtract (check->v.origin, move, check->v.origin);
		org2[0] = DotProduct (org, forward);
		org2[1] = -DotProduct (org, right);
		org2[2] = DotProduct (org, up);
		VectorSubtract (org2, org, move2);

	//	Con_DPrintf("%f %f %f (move2)\n", move2[0], move2[1], move2[2]);

	//	VectorAdd (check->v.origin, move2, check->v.origin);

		// Add all moves together
		VectorAdd(move,move2,move3);

		// Find the angle of rotation as compared to vector from pusher
		// origin to check center
	//	turn_away = DotProduct(org,a);

		// try moving the contacted entity
		for (t = 0; t < 13; t++)
		{
			switch (t)
			{
			case 0:
			//try x, y and z
				VectorCopy(move3,testmove);
				break;
			case 1:
			//Try xy only
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = move3[0];
				testmove[1] = move3[1];
				testmove[2] = 0;
				break;
			case 2:
			//Try z only
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = 0;
				testmove[1] = 0;
				testmove[2] = move3[2];
				break;
			case 3:
			//Try none
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = 0;
				testmove[1] = 0;
				testmove[2] = 0;
				break;
			case 4:
			//Try xy in opposite dir
				testmove[0] = move3[0] * -1;
				testmove[1] = move3[1] * -1;
				testmove[2] = move3[2];
				break;
			case 5:
			//Try z in opposite dir
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = move3[0];
				testmove[1] = move3[1];
				testmove[2] = move3[2] * -1;
				break;
			case 6:
			//Try xyz in opposite dir
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = move3[0] * -1;
				testmove[1] = move3[1] * -1;
				testmove[2] = move3[2] * -1;
				break;
			case 7:
			//Try move3 times 2
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				VectorScale(move3,2,testmove);
				break;
			case 8:
			//Try normalized org
				VectorSubtract(check->v.origin,testmove,check->v.origin);

			//	VectorCopy(amove,amove_norm);
			//	amove_mag = VectorNormalize(amove_norm);
			//	//VectorNormalize(org);
			//	VectorScale(org,amove_mag,org);

			//	VectorNormalize(org);
				VectorScale(org,movetime,org);//movetime*20?
				VectorCopy(org,testmove);
				break;
			case 9:
			//Try normalized org z * 3 only
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = 0;
				testmove[1] = 0;
				testmove[2] = org[2] * 3;//was: +org[2]*(fabs(org[1])+fabs(org[2]));
				break;
			case 10:
			//Try normalized org xy * 2 only
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = org[0] * 2;//was: +org[0]*fabs(org[2]);
				testmove[1] = org[1] * 2;//was: +org[1]*fabs(org[2]);
				testmove[2] = 0;
				break;
			case 11:
			//Try xy in opposite org dir
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = org[0] * -2;
				testmove[1] = org[1] * -2;
				testmove[2] = org[2];
				break;
			case 12:
			//Try z in opposite dir
				VectorSubtract(check->v.origin,testmove,check->v.origin);
				testmove[0] = org[0];
				testmove[1] = org[1];
				testmove[2] = org[2] * -3;
				break;
			}

			if (t != 3)
			{
			//THIS IS VERY BAD BAD HACK...
				pusher->v.solid = SOLID_NOT;
				SV_PushEntity (check, move3);
			//@@TODO: do we ever want to do anybody's angles?  maybe just yaw???
			//	if (!((int)check->v.flags & (FL_CLIENT | FL_MONSTER)))
			//		VectorAdd (check->v.angles, amove, check->v.angles);
				check->v.angles[YAW] += amove[YAW];
				pusher->v.solid = SOLID_BSP;
			}
			// if it is still inside the pusher, block
			block = SV_TestEntityPosition (check);
			if (!block)
				break;
		}

	//	Con_DPrintf("t: %i\n",t);

	//	if (turn_away > 0)
	//	{
			if (block)
			{	// fail the move
			//	Con_DPrintf("Check blocked\n");
				if (check->v.mins[0] == check->v.maxs[0])
					continue;
				if (check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER)
				{	// corpse
					check->v.mins[0] = check->v.mins[1] = 0;
					VectorCopy (check->v.mins, check->v.maxs);
					continue;
				}

				VectorCopy (entorig, check->v.origin);
				SV_LinkEdict (check, true);

				VectorCopy (pushorig, pusher->v.origin);
				VectorCopy (pushorigangles, pusher->v.angles);
				SV_LinkEdict (pusher, false);
				pusher->v.ltime -= movetime;

				for (i = 0; i < slaves_moved; i++)
				{
					slave = moved_edict[MAX_EDICTS - i - 1];
					VectorCopy (moved_from[MAX_EDICTS - i - 1], slave->v.angles);
					SV_LinkEdict (slave, false);
					slave->v.ltime -= movetime;
				}

				// if the pusher has a "blocked" function, call it
				// otherwise, just stay in place until the obstacle is gone
				if (pusher->v.blocked)
				{
					*sv_globals.self = EDICT_TO_PROG(pusher);
					*sv_globals.other = EDICT_TO_PROG(check);
					PR_ExecuteProgram (pusher->v.blocked, "blocked");
				}

				// move back any entities we already moved
				for (i = 0; i < num_moved; i++)
				{
					VectorCopy (moved_from[i], moved_edict[i]->v.origin);
				//@@TODO:: see above
				//	if (!((int)moved_edict[i]->v.flags & (FL_CLIENT | FL_MONSTER)))
				//		VectorSubtract (moved_edict[i]->v.angles, amove, moved_edict[i]->v.angles);
					moved_edict[i]->v.angles[YAW] -= amove[YAW];

					SV_LinkEdict (moved_edict[i], false);
				}
				return;
			}
	//	}
	//	else if (block)	// undo last move
	//		VectorCopy (entorig, check->v.origin);
	}

#   if 0
	Con_DPrintf("result:\n");
	Con_DPrintf("%f %f %f\n", pusher->v.angles[0], pusher->v.angles[1], pusher->v.angles[2]);
	for (i = 0; i < slaves_moved; i++)
	{
		slave = moved_edict[MAX_EDICTS - i - 1];
		Con_DPrintf("%f %f %f   slave entity %i\n", slave->v.angles[0], slave->v.angles[1], slave->v.angles[2], NUM_FOR_EDICT(slave));
	}
	Con_DPrintf("\n");
#   endif
}


/*
================
SV_Physics_Pusher

================
*/
static void SV_Physics_Pusher (edict_t *ent)
{
	float	thinktime;
	float	oldltime;
	float	movetime;
	vec3_t	oldorg, move;
	float	l;

	oldltime = ent->v.ltime;

	thinktime = ent->v.nextthink;
	if (thinktime < ent->v.ltime + host_frametime)
	{
		movetime = thinktime - ent->v.ltime;
		if (movetime < 0)
			movetime = 0;
	}
	else
		movetime = host_frametime;

	if (movetime)
	{
		if (ent->v.avelocity[0] || ent->v.avelocity[1] || ent->v.avelocity[2])
		{
			//SV_PushMove (ent, movetime, false);
			SV_PushRotate (ent, movetime);
		}
		else
			SV_PushMove (ent, movetime, true);	// advances ent->v.ltime if not blocked
	}

	if (thinktime > oldltime && thinktime <= ent->v.ltime)
	{
		VectorCopy (ent->v.origin, oldorg);

		ent->v.nextthink = 0;
		*sv_globals.time = sv.time;
		*sv_globals.self = EDICT_TO_PROG(ent);
		*sv_globals.other = EDICT_TO_PROG(sv.edicts);
		PR_ExecuteProgram (ent->v.think, "think in SV_Physics_Pusher");
		if (ent->free)
			return;

		VectorSubtract (ent->v.origin, oldorg, move);

		l = VectorLength(move);
		if (l > 1.0/64)
		{
		//	Con_Printf ("**** snap: %f\n", l);
			VectorCopy (oldorg, ent->v.origin);
			SV_Push (ent, move);
		}
	}
}
