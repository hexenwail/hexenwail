#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/hexenworld/shared/info_str.c the way hwsv compiles it -- -DH2W
# -DSERVERONLY, with hexenworld/shared ahead of h2shared on the include path --
# with all six exported names renamed so it can share a binary with the Rust
# info_str module, and compare the two byte for byte across successive calls.
#
# Only one C variant exists to compare against: info_str.c is compiled into
# HWSV_SOURCES alone, hwsv is the only SERVERONLY target, and every Hexen II
# use of Info_* is inside `#if defined(H2W)`.  The `#ifndef SERVERONLY` client
# variant is therefore never built here, deliberately -- it is code no binary in
# this tree contains.
#
# The Rust staticlib must have been built with the info_str feature.  The cvar
# the C reads (sv_highchars) and the lookup the Rust uses (Cvar_FindVar) are
# both defined by diff_harness.c, which also defines the engine symbols the
# other ports in the archive reference.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: consolidated Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features hashindex,mathlib,sizebuf,crc,link_ops,msg_io,info_str" >&2
	exit 2
fi

# The hwsv include order: hexenworld/server and hexenworld/shared shadow
# h2shared, which is where info_str.c's quakedef.h comes from.
INCS_H2W=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol info_str.c exports, renamed.
RENAME=(
	-DInfo_ValueForKey=c_Info_ValueForKey
	-DInfo_RemoveKey=c_Info_RemoveKey
	-DInfo_RemovePrefixedKeys=c_Info_RemovePrefixedKeys
	-DInfo_SetValueForKey=c_Info_SetValueForKey
	-DInfo_SetValueForStarKey=c_Info_SetValueForStarKey
	-DInfo_Print=c_Info_Print
)

echo "== compiling the C original as hwsv does (-DH2W -DSERVERONLY) =="
cc "${INCS_H2W[@]}" "${CFLAGS[@]}" -DGLQUAKE -DH2W -DSERVERONLY "${RENAME[@]}" \
	-c "$engine/hexenworld/shared/info_str.c" -o "$OUT/info_str_h2w.o"

echo "== compiling the differential harness =="
# -DH2W -DSERVERONLY too: the harness includes the same quakedef.h and must
# take the hwsv branch of it, and it uses qboolean-sized ints where the C does.
cc "${INCS_H2W[@]}" "${CFLAGS[@]}" -DGLQUAKE -DH2W -DSERVERONLY \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/info_str_h2w.o" \
	"$RUST_LIB" -lm

echo "== running =="
# The timeout is load-bearing, not defensive habit.
#
# Info_RemovePrefixedKeys is a `while (1)` that restarts from `start` after
# every removal, so an implementation whose Info_RemoveKey does not actually
# remove anything never makes progress -- it spins instead of failing, and the
# C original pulls the same trick on the Rust module's behalf.  A gate that
# hangs is worse than one that fails: CI cannot afford a job that never
# returns, and a hang looks like a slow machine rather than a broken port.  So
# a non-progressing run is reported as the failure it is.
harness_timeout="${HARNESS_TIMEOUT:-60}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	echo "      That is what a removal that does not take effect looks like:" >&2
	echo "      Info_RemovePrefixedKeys restarts after each removal and spins" >&2
	echo "      forever when the removal is a no-op." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"