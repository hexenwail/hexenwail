/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of sv_phys.c's base helpers, as one translation
 * unit, for the differential harness.  run_diff_harness.sh compiles it with
 * -DSV_FIXTURE naming sv_phys_base_h2.c or sv_phys_base_hw.c, and with every
 * function renamed to c_*, so it and the Rust link into one binary.  Its
 * callees are not renamed: SV_Move and SV_LinkEdict are the already-ported
 * Rust, and the console, progs and error functions are the harness's.
 *
 * As in the earlier slices' c_ref.c: the fixtures are sv_phys.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed statics link like the functions.
 */

#include "quakedef.h"

/* sv_phys.c's cvars, which stay sv_phys.c's storage; the harness defines them */
extern	cvar_t	sv_maxvelocity;
#ifndef H2W
extern	cvar_t	sv_gravity;
extern	cvar_t	sv_gameplayfix_elevators;
extern	cvar_t	sv_nostep;
extern	cvar_t	sv_freezenonclients;
#endif

#define static
#include SV_FIXTURE
#undef static
