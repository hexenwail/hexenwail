/* world_target.h -- the per-target facts engine/rust/src/world_hull.rs reads.
 *
 * Hull selection (SV_HullForEntity) reads an edict's progs fields, the
 * server's model table and a model's hulls.  Every one of those lives in a
 * struct whose layout is the target's own: qmodel_t is a different struct in
 * gl_model.h (the GL client), model.h (the software renderer) and sv_model.h
 * (h2ded, hwsv), so its `hulls` member is at a different offset in each;
 * entvars_t is generated per protocol (engine/hexen2/progdefs.h,
 * engine/hexenworld/shared/progdefs.h); server_t is per protocol too.  The
 * archive is one object for all of them, so it cannot carry those offsets
 * itself.  engine/rust/world_target.c, which every target compiles with its
 * own headers, fills this table with offsetof and the header constants, and
 * the Rust reads through it -- the same arrangement net_udp_h2_target.c uses
 * for socket layouts, so there is no second copy of any layout to drift.
 *
 * Every field is a size_t offset into the named struct or an int constant.
 * Field order is the contract with world_hull.rs's WorldTargetFacts, and
 * engine/rust/tests/abi_layout.c checks it on every target, wasm32 included.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#ifndef WORLD_TARGET_H
#define WORLD_TARGET_H

#include <stddef.h>

typedef struct world_target_facts_s
{
	/* edict_t: where the progs fields (entvars_t v) begin */
	size_t	edict_v;
	/* entvars_t members, relative to v */
	size_t	v_solid, v_movetype, v_modelindex, v_hull, v_flags, v_spawnflags;
	size_t	v_origin, v_mins, v_maxs;
	/* qmodel_t members */
	size_t	model_type, model_hulls;
	/* sizeof (hull_t), so a hull index steps the way &model->hulls[i] does */
	size_t	hull_size;
	/* header constants */
	int	solid_bsp, movetype_push, mod_brush, fl_monster;
} world_target_facts_t;

extern const world_target_facts_t	World_TargetFacts;

/* sv.models and sv.edicts, read at call time: the server reallocates both on
 * every map change.  sv itself is per protocol, so these are C. */
struct qmodel_s	**World_SvModels (void);
struct edict_s	*World_SvEdicts (void);

#endif	/* WORLD_TARGET_H */
