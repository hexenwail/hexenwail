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
extern	cvar_t	sv_gameplayfix_elevators;
extern	cvar_t	sv_nostep;
extern	cvar_t	sv_freezenonclients;
#endif

#define V(member)	offsetof (entvars_t, member)

const phys_target_facts_t Phys_TargetFacts =
{
	V(velocity), V(nextthink), V(think), V(hoverz),
	SOLID_PHASE, MOVETYPE_WALK, MOVETYPE_FLYMISSILE, MOVETYPE_BOUNCE,
	MOVETYPE_BOUNCEMISSILE, MOVETYPE_SWIM,
	V(ltime), V(avelocity), V(watertype), V(blocked),
	MOVETYPE_NONE, MOVETYPE_NOCLIP, MOVETYPE_FOLLOW, MAX_EDICTS,
	V(oldorigin), V(waterlevel), V(v_angle),
	FL_WATERJUMP, MOVETYPE_STEP, MOVETYPE_TOSS, MOVETYPE_FLY,
	V(movechain), V(chainmoved),
#ifdef H2W
	V(lastruntime),
#else
	0,
#endif
	offsetof (edict_t, spawnwalk),
	FL_MOVECHAIN_ANGLE, MOVETYPE_PUSHPULL,
#ifdef H2W
	MAX_CLIENTS
#else
	0
#endif
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

int Phys_EdictSize (void)
{
	return pr_edict_size;
}

int Phys_NumEdicts (void)
{
	return sv.num_edicts;
}

int Phys_MaxClients (void)
{
#ifdef H2W
	return 0;
#else
	return svs.maxclients;
#endif
}

int Phys_MapChecks (void)
{
#ifdef H2W
	return 0;
#else
	return map_checks.integer;
#endif
}

float Phys_Elevators (void)
{
#ifdef H2W
	return 0;
#else
	return sv_gameplayfix_elevators.value;
#endif
}

int Phys_NoStep (void)
{
#ifdef H2W
	return 0;
#else
	return sv_nostep.integer;
#endif
}

int Phys_ClientActive (int num)
{
#ifdef H2W
	(void) num;
	return 0;
#else
	return svs.clients[num - 1].active;
#endif
}

int Phys_PlayerPreThink (void)
{
#ifdef H2W
	return 0;
#else
	return *sv_globals.PlayerPreThink;
#endif
}

int Phys_PlayerPostThink (void)
{
#ifdef H2W
	return 0;
#else
	return *sv_globals.PlayerPostThink;
#endif
}

float *Phys_ForceRetouch (void)
{
	return sv_globals.force_retouch;
}

int Phys_StartFrame (void)
{
	return *sv_globals.StartFrame;
}

int *Phys_PhysicsWalk (void)
{
	return &sv_physics_walk;
}

double Phys_Realtime (void)
{
	return realtime;
}

double *Phys_HostFrametimePtr (void)
{
	return &host_frametime;
}

double *Phys_SvTimePtr (void)
{
	return &sv.time;
}

int Phys_FreezeNonClients (void)
{
#ifdef H2W
	return 0;
#else
	return sv_freezenonclients.integer;
#endif
}

float *Phys_FrameTimeGlobal (void)
{
#ifdef H2W
	return sv_globals.frametime;
#else
	return NULL;
#endif
}

int *Phys_Newmis (void)
{
#ifdef H2W
	return sv_globals.newmis;
#else
	return NULL;
#endif
}

float Phys_MinTic (void)
{
#ifdef H2W
	return sv_mintic.value;
#else
	return 0;
#endif
}

float Phys_MaxTic (void)
{
#ifdef H2W
	return sv_maxtic.value;
#else
	return 0;
#endif
}
