/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of the box hull and SV_HullForEntity, as one
 * translation unit, for the differential harness.  run_diff_harness.sh
 * compiles it with -DWT_FIXTURE naming world_hull_h2.c or world_hull_hw.c,
 * and with every exported name and the three box statics renamed (c_* for the
 * IEEE build, f_* for the engine-flags one), so both builds and the Rust link
 * into one binary.
 *
 * As in engine/rust/world_trace/tests/c_ref.c: the fixtures are world.c's text
 * moved verbatim, `static` included, and world.h now declares SV_InitBoxHull
 * and SV_HullForEntity for their Rust definitions, so `static` is defined away
 * around the include and nowhere else.  It changes linkage, not code.
 */

#include "quakedef.h"

#define static
#include WT_FIXTURE
#undef static
