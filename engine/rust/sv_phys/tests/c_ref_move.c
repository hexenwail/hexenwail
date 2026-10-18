/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_move.c -- the C original of sv_phys.c's movetype dispatch, as one
 * translation unit, for the differential harness.  run_move_harness.sh
 * compiles it with -DSV_FIXTURE naming sv_phys_move_h2.c or sv_phys_move_hw.c,
 * and with every function renamed to c_*, so it and the Rust link into one
 * binary.
 *
 * Unlike the other slices' c_ref.c, `static' is NOT defined away here: both
 * SV_Physics keep a function-local static (Hexen II's walk counter,
 * HexenWorld's old_time), which `#define static' would turn into an
 * uninitialised automatic.  The static functions are reached through the
 * cw_* wrappers below instead.
 */

#include "quakedef.h"

/* sv_phys.c's cvars, which stay sv_phys.c's storage; the harness defines them */
extern	cvar_t	sv_maxvelocity;
#ifndef H2W
extern	cvar_t	sv_gravity;
extern	cvar_t	sv_freezenonclients;
#endif

#include SV_FIXTURE

void cw_SV_Physics_None (edict_t *ent)		{ c_SV_Physics_None (ent); }
void cw_SV_Physics_Noclip (edict_t *ent)	{ c_SV_Physics_Noclip (ent); }
void cw_SV_CheckWaterTransition (edict_t *ent)	{ c_SV_CheckWaterTransition (ent); }
void cw_SV_Physics_Step (edict_t *ent)		{ c_SV_Physics_Step (ent); }
#ifdef H2W
void cw_SV_RunEntity (edict_t *ent)		{ c_SV_RunEntity (ent); }
#endif
