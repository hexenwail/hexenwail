/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_ents.c -- the C original of sv_main.c's entity updates, as one
 * translation unit, for the differential harness.  run_ents_harness.sh
 * compiles it with -DSV_FIXTURE naming sv_main_ents_h2.c, and with every
 * function and file-scope static renamed to c_*, so it and the Rust link
 * into one binary.  Its callees are not renamed: MSG_Write* and SZ_GetSpace
 * are the already-ported Rust; EDICT_NUM, SV_ModelIndex, Mod_LeafPVS,
 * GetEdictFieldValue, PR_GetString, CON_Printf and Host_Error the harness's.
 *
 * As in the other slices' c_ref.c: the fixture is sv_main.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed statics link like the functions.
 * Nothing in the fixture has a function-local static.
 */

#include "quakedef.h"

/* sv_main.c's cvars, which the fixture reads; the harness defines them */
extern cvar_t	sv_update_player, sv_update_monsters, sv_update_missiles;
extern cvar_t	sv_update_misc, sv_netsort;

#define static
#include SV_FIXTURE
#undef static
