/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_connect.c -- the C original of sv_main.c's connect and signon
 * functions, as one translation unit, for the differential harness.
 * run_connect_harness.sh compiles it with -DSV_FIXTURE naming
 * sv_main_connect_h2.c, and with every ported function renamed to c_*, so
 * it and the Rust link into one binary.  Its callees are not renamed: the
 * MSG_Write* and SZ_* are the already-ported Rust, SV_GetLevelname is the
 * fixture's own supporting copy, and CON_Printf, Sys_Error, Host_Error and
 * the NET_* are the harness's.
 *
 * As in the other slices' c_ref.c: the fixture is sv_main.c's text moved
 * verbatim, `static` included, and `static` is defined away around the
 * include and nowhere else, so the renamed SV_ConnectClient and friends
 * link like the rest.  Nothing in the fixture has a function-local static.
 */

#include "quakedef.h"

#define static
#include SV_FIXTURE
#undef static
