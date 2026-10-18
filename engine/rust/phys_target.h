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
} phys_target_facts_t;

extern const phys_target_facts_t	Phys_TargetFacts;

double		Phys_HostFrametime (void);
struct edict_s	*Phys_SvPlayer (void);
/* sv_maxvelocity.value */
float		Phys_MaxVelocity (void);
/* Hexen II: sv_gravity.value.  HexenWorld: movevars.gravity, which
 * SV_AddGravity reads there in place of the cvar. */
float		Phys_Gravity (void);

#endif	/* PHYS_TARGET_H */
