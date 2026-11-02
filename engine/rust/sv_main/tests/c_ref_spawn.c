/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_spawn.c -- the C original of sv_main.c's SV_SpawnServer and its
 * map_checks readout, as one translation unit, for the differential harness
 * (E2 / slice E of #469).  run_spawn_harness.sh compiles it with -DSV_FIXTURE
 * naming sv_main_spawn_h2.c and every ported function renamed to c_*, so it
 * and the Rust link into one binary.  Its callees are not renamed: both
 * sides call the harness's recorders, and SV_PrintMapCheck, which stays C,
 * is engine/rust/sv_main_variadic.c's for both.
 *
 * Unlike the earlier slices' c_ref files, `static' is NOT defined away
 * around the include: SV_SpawnServer's `static char dummy[8]' is the
 * precache sentinel, and defining static away would put it on the stack.
 * The three static helpers are reached instead through the non-static
 * doors at the bottom, which are not part of the fixture.
 */

#include "quakedef.h"
#include "sv_main_target.h"

/* sv_main.c's file-scope declarations the fixture's text relies on: sv_main.c
 * defines these above the moved functions, or (scr_centertime_off) declares
 * them there. */
extern	char		localmodels[MAX_MODELS][8];
extern	qboolean	sv_protocol_userset;
extern	cvar_t		max_edicts;
extern	float		scr_centertime_off;
void SV_SendServerinfo (client_t *client);
void SV_CreateBaseline (void);
void SV_SendReconnect (void);

#include SV_FIXTURE

mapcheck_t c_ref_MapCheckThresh (int current, int target);
void c_ref_MapCheckTitle (char *out, size_t outsize, const char *in);
void c_ref_PrintMapChecklist (void);

mapcheck_t c_ref_MapCheckThresh (int current, int target)
{
	return SV_MapCheckThresh (current, target);
}

void c_ref_MapCheckTitle (char *out, size_t outsize, const char *in)
{
	SV_MapCheckTitle (out, outsize, in);
}

void c_ref_PrintMapChecklist (void)
{
	SV_PrintMapChecklist ();
}
