/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_inv.c -- the C original of sv_main.c's extended inventory, worldspawn
 * sky, level name and savegame helpers, as one translation unit, for the
 * differential harness.  run_inv_harness.sh compiles it with -DSV_FIXTURE
 * naming sv_main_inv_h2.c, and with every ported function renamed to c_*, so
 * it and the Rust link into one binary.  Its callees are not renamed:
 * COM_Parse and q_strlcpy are the harness's logging wrappers around the
 * engine's own C (extracted from engine/h2shared/common.c, and
 * common/strlcpy.c), and Host_GetString, PR_GetString and Host_Error are the
 * harness's.
 *
 * As in the other slices' c_ref.c: the fixture is sv_main.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed SV_ParseWorldspawnSky links like
 * the rest.  Nothing in the fixture has a function-local static.
 *
 * One more thing is defined around the include, and only for the C side: the
 * fixture's fscanf and fgetc go through inv_harness.c's observers, which call
 * the C library's vfscanf and fgetc and return what they return.  They exist
 * to stop the C at the one place SV_LoadInventory's behaviour becomes
 * undefined -- an item loop about to index past MAX_INVENTORY_EX -- before it
 * writes past the page, so the harness can compare that stop with the Rust's
 * bounds-check abort (see inv_harness.c).  They change no byte the fixture
 * reads or writes.
 */

#include "quakedef.h"

int	inv_harness_c_fscanf (FILE *f, const char *fmt, ...);
int	inv_harness_c_fgetc (FILE *f);

#define fscanf	inv_harness_c_fscanf
#define fgetc	inv_harness_c_fgetc
#define static
#include SV_FIXTURE
#undef static
#undef fgetc
#undef fscanf
