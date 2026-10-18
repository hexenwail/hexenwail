/* world_target.c -- the facts engine/rust/src/world_hull.rs needs from this
 * target's own headers.  See world_target.h for why they cannot live in the
 * archive.  Every target compiles this file with its own include path and
 * defines, so `quakedef.h` here is the target's: the GL client's qmodel_t is
 * gl_model.h's, h2ded's is sv_model.h's, hwsv's entvars_t is HexenWorld's.
 * Nothing here is game logic: each value is an offsetof or a header constant,
 * and each function returns one member of sv.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "quakedef.h"
#include "world_target.h"

#define V(member)	offsetof (entvars_t, member)

const world_target_facts_t World_TargetFacts =
{
	offsetof (edict_t, v),
	V(solid), V(movetype), V(modelindex), V(hull), V(flags), V(spawnflags),
	V(origin), V(mins), V(maxs),
	offsetof (qmodel_t, type), offsetof (qmodel_t, hulls),
	sizeof (hull_t),
	SOLID_BSP, MOVETYPE_PUSH, mod_brush, FL_MONSTER
};

struct qmodel_s **World_SvModels (void)
{
	return sv.models;
}

struct edict_s *World_SvEdicts (void)
{
	return sv.edicts;
}
