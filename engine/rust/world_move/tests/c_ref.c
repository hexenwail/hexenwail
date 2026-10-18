/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of SV_Move and the rest of world.c, as one
 * translation unit, for the differential harness.  run_diff_harness.sh
 * compiles it with -DWT_FIXTURE naming world_move_h2.c or world_move_hw.c,
 * and with every function and move_type renamed to c_*, so it and the Rust
 * link into one binary.  sv_areanodes is not renamed: it is world.c's
 * storage, which both read, as in the engine.
 *
 * As in the earlier slices' c_ref.c: the fixtures are world.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed statics link like the functions.
 */

#include "quakedef.h"

#define static
#include WT_FIXTURE
#undef static
