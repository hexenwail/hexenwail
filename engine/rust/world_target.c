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
	offsetof (edict_t, free), offsetof (edict_t, area),
	offsetof (edict_t, num_leafs), offsetof (edict_t, leafnums),
	V(touch), V(absmin), V(absmax), V(angles),
	offsetof (mnode_t, contents), offsetof (mnode_t, plane),
	offsetof (mnode_t, children), sizeof (mleaf_t),
	offsetof (qmodel_t, mins), offsetof (qmodel_t, maxs),
	offsetof (qmodel_t, nodes), offsetof (qmodel_t, leafs),
	V(size), V(owner), V(classname),
	SOLID_BSP, MOVETYPE_PUSH, mod_brush, FL_MONSTER,
	SOLID_TRIGGER, SOLID_NOT, FL_ITEM, MAX_ENT_LEAFS, AREA_DEPTH, AREA_NODES,
	MOVE_NOMONSTERS, MOVE_MISSILE, MOVE_WATER, MOVE_PHASE,
#ifdef SOLID_GHOST
	SOLID_GHOST
#else
	-1
#endif
};

struct qmodel_s **World_SvModels (void)
{
	return sv.models;
}

struct edict_s *World_SvEdicts (void)
{
	return sv.edicts;
}

struct qmodel_s *World_SvWorldModel (void)
{
	return sv.worldmodel;
}

double World_SvTime (void)
{
	return sv.time;
}


int *World_GlobalSelf (void)
{
	return sv_globals.self;
}

int *World_GlobalOther (void)
{
	return sv_globals.other;
}

float *World_GlobalTime (void)
{
	return sv_globals.time;
}
