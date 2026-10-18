/*
 * world.c -- world query functions
 *
 * entities never clip against themselves, or their owner
 * line of sight checks trace->crosscontent, but bullets don't
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
 * Everything world.c did is Rust now (#455, #294), in four modules:
 *
 *   engine/rust/src/world_trace.rs  SV_HullPointContents, SV_RecursiveHullCheck
 *   engine/rust/src/world_hull.rs   the box hull, SV_HullForEntity
 *   engine/rust/src/world_link.rs   SV_ClearWorld, SV_LinkEdict, SV_UnlinkEdict
 *                                   and the touch and leaf walks under them
 *   engine/rust/src/world_move.rs   SV_Move and its clipping, SV_PointContents,
 *                                   SV_TestEntityPosition
 *
 * Each reads this target's layouts through engine/rust/world_target.c.  The C
 * that was here is the reference half of each port's differential harness,
 * moved verbatim to engine/rust/world_{trace,hull,link,move}/tests/, along
 * with SV_TestPlayerPosition, which was under `#if 0` (marked "no callers")
 * and is not ported.
 *
 * What stays is storage: the area nodes, which the Rust ports read, and so
 * does sv_user.c.
 */
areanode_t	sv_areanodes[AREA_NODES];
