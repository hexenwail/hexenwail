/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of sv_move.c's monster movement, as one
 * translation unit, for the differential harness.  run_diff_harness.sh
 * compiles it with -DSV_FIXTURE naming sv_move_h2.c or sv_move_hw.c, and with
 * every function renamed to c_*, so it and the Rust link into one binary.
 * Its callees are not renamed: SV_Move, SV_PointContents and SV_LinkEdict
 * are the already-ported Rust, and the sv_debugmovestep helpers and
 * PF_changeyaw are the harness's.
 *
 * As in the world.c slices' c_ref.c: the fixtures are sv_move.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed statics link like the functions.
 */

#include "quakedef.h"

#define static
#include SV_FIXTURE
#undef static
