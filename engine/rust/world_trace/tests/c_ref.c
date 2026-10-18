/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref.c -- the C original of the hull traces, as one translation unit, for
 * the differential harness.  run_diff_harness.sh compiles this file with
 * -DWT_FIXTURE naming world_trace_h2.c or world_trace_hw.c and with every
 * exported name renamed to c_*, so both implementations link into one binary.
 *
 * The fixtures are world.c's text moved verbatim, `static` included.  world.h
 * now declares SV_HullPointContents for its Rust definition, and a static
 * definition after a non-static declaration does not compile, so `static` is
 * defined away around the include -- and only there.  That is the one liberty
 * taken with the original, and it changes linkage, not code.
 */

#include "quakedef.h"

#define static
#include WT_FIXTURE
#undef static
