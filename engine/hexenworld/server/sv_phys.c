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
===============================================================================

THE MOVETYPE DISPATCH: SV_Physics_None, SV_Physics_Noclip, SV_CheckWaterTransition,
SV_Physics_Toss, SV_Physics_Step, SV_ProgStartFrame, SV_RunEntity,
SV_RunNewmis, SV_Physics

engine/rust/src/sv_phys_move.rs (#455); the C original is
engine/rust/sv_phys/tests/sv_phys_move_hw.c.  SV_Physics, SV_ProgStartFrame,
SV_RunNewmis and SV_Physics_Toss are declared in server.h; nothing in C
calls the rest.
With it every function of this file is Rust; what stays here is the
physics cvars and SV_SetMoveVars, which copies them into movevars.

===============================================================================
*/

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
