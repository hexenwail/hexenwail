/* phys_target.h -- the per-target facts engine/rust/src/sv_phys.rs reads,
 * beyond the ones world_target.h already carries.
 *
 * Its own file, not more of world_target.h, because what it reaches is not
 * the world's: host_frametime, sv_player and the physics cvars.  The world.c
 * and sv_move.c harnesses compile world_target.c without any of them, and
 * this keeps them that way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#ifndef PHYS_TARGET_H
#define PHYS_TARGET_H

#include <stddef.h>

typedef struct phys_target_facts_s
{
	/* entvars_t members, relative to v */
	size_t	v_velocity, v_nextthink, v_think, v_hoverz;
	/* header constants */
	int	solid_phase, movetype_walk, movetype_flymissile, movetype_bounce;
	int	movetype_bouncemissile, movetype_swim;
	/* the pushers (sv_phys_push.rs): entvars_t members, relative to v */
	size_t	v_ltime, v_avelocity, v_watertype, v_blocked;
	/* header constants; max_edicts sizes the pushers' moved lists */
	int	movetype_none, movetype_noclip, movetype_follow, max_edicts;
	/* walking (sv_phys_walk.rs): entvars_t members, relative to v */
	size_t	v_oldorigin, v_waterlevel, v_v_angle;
	/* header constants */
	int	fl_waterjump, movetype_step, movetype_toss, movetype_fly;
} phys_target_facts_t;

extern const phys_target_facts_t	Phys_TargetFacts;

double		Phys_HostFrametime (void);
struct edict_s	*Phys_SvPlayer (void);
/* sv_maxvelocity.value */
float		Phys_MaxVelocity (void);
/* Hexen II: sv_gravity.value.  HexenWorld: movevars.gravity, which
 * SV_AddGravity reads there in place of the cvar. */
float		Phys_Gravity (void);
/* pr_edict_size and sv.num_edicts, for the entity walk */
int		Phys_EdictSize (void);
int		Phys_NumEdicts (void);
/* Hexen II's SV_PushMove only: svs.maxclients, map_checks.integer and
 * sv_gameplayfix_elevators.value.  0 in HexenWorld, which has none of them. */
int		Phys_MaxClients (void);
int		Phys_MapChecks (void);
float		Phys_Elevators (void);
/* Hexen II's walking only: sv_nostep.integer; svs.clients[num - 1].active;
 * *sv_globals.PlayerPreThink and PlayerPostThink.  0 in HexenWorld. */
int		Phys_NoStep (void);
int		Phys_ClientActive (int num);
int		Phys_PlayerPreThink (void);
int		Phys_PlayerPostThink (void);

#endif	/* PHYS_TARGET_H */
