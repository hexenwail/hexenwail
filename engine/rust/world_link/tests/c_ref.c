/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of world.c's area linking, as one translation
 * unit, for the differential harness.  run_diff_harness.sh compiles it with
 * -DWT_FIXTURE naming world_link_h2.c or world_link_hw.c, and with every
 * function and private static renamed to c_*, so it and the Rust link into
 * one binary.  sv_areanodes is not renamed: it is world.c's storage, which
 * the engine keeps in C, and both implementations share it here as they would
 * share it there.
 *
 * As in engine/rust/world_trace/tests/c_ref.c: the fixtures are world.c's text
 * moved verbatim, `static` included.  `static` is defined away around the
 * include and nowhere else, so the renamed statics link like the functions;
 * it changes linkage, not code.
 */

#include "quakedef.h"

#define static
#include WT_FIXTURE
#undef static
