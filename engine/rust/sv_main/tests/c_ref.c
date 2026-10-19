/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of sv_main.c's sound and particle messages, as
 * one translation unit, for the differential harness.  run_diff_harness.sh
 * compiles it with -DSV_FIXTURE naming sv_main_sound_h2.c, and with every
 * function renamed to c_*, so it and the Rust link into one binary.  Its
 * callees are not renamed: the MSG_Write* and SZ_GetSpace are the
 * already-ported Rust, NUM_FOR_EDICT, q_strcasecmp, CON_Printf and
 * Host_Error the harness's.
 *
 * As in the other slices' c_ref.c: the fixture is sv_main.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed SV_SoundEntityFits links like the
 * rest.  Nothing in the fixture has a function-local static.
 */

#include "quakedef.h"

#define static
#include SV_FIXTURE
#undef static
