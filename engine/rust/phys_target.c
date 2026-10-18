/* phys_target.c -- the facts engine/rust/src/sv_phys.rs needs from this
 * target's own headers and globals.  See phys_target.h.  Every target
 * compiles this file with its own include path and defines; nothing here is
 * game logic, each value is an offsetof, a header constant or one global.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "quakedef.h"
#include "phys_target.h"

/* sv_phys.c's cvars; no header declares them */
extern	cvar_t	sv_maxvelocity;
#ifndef H2W
extern	cvar_t	sv_gravity;
#endif

#define V(member)	offsetof (entvars_t, member)

const phys_target_facts_t Phys_TargetFacts =
{
	V(velocity), V(nextthink), V(think), V(hoverz),
	SOLID_PHASE, MOVETYPE_WALK, MOVETYPE_FLYMISSILE, MOVETYPE_BOUNCE,
	MOVETYPE_BOUNCEMISSILE, MOVETYPE_SWIM
};

double Phys_HostFrametime (void)
{
	return host_frametime;
}

struct edict_s *Phys_SvPlayer (void)
{
	return sv_player;
}

float Phys_MaxVelocity (void)
{
	return sv_maxvelocity.value;
}

float Phys_Gravity (void)
{
#ifdef H2W
	return movevars.gravity;
#else
	return sv_gravity.value;
#endif
}
