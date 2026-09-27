#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/h2shared/zone.c with every exported name renamed, link it beside
# the Rust zone module, and compare the two -- twice, because zone.c has two
# variants.
#
# zone.c is compiled into all three engine targets but not identically: under
# SERVERONLY the zone default is 1 MB (against 2 MB), the secondary zone does
# not exist, and the whole cache API is compiled out.  The consolidated Rust
# archive is one object linked into all of them, so the difference is carried by
# engine/rust/zone_target.c -- the per-target shim decided on #233 -- which this
# script compiles with the arm's defines, exactly as each engine target compiles
# it.  That is the claim under test: one Rust implementation, two behaviours,
# each matching the C the target was built against.
#
# The cache scenarios exist only in the client arm, because the SERVERONLY C
# original has no Cache_* symbols at all (zone.h does not even declare them
# there).  The SERVERONLY arm proves the rest of the surface still matches and
# that the Rust module's cache machinery -- present in both, because the archive
# is one object -- stays inert where the C compiled it away.
#
# The Rust staticlib must have been built with the zone feature.  The harness
# defines CON_Printf, Sys_Error, Cmd_AddCommand, COM_CheckParm and host_parms,
# and compiles common/strlcpy.c for q_strlcpy (which is the Rust strlcpy port in
# the engine, but the C original here, so both implementations call the same
# code and the comparison is about zone, not about strlcpy).
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features zone" >&2
	exit 2
fi

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol zone.c exports, renamed.  Its statics -- the zonelist, the hunk
# counters, the cache sentinel -- are internal to each implementation and stay
# unrenamed: each builds its own.
RENAME=(
	-DMemory_Init=c_Memory_Init
	-DZ_Free=c_Z_Free
	-DZ_Malloc=c_Z_Malloc
	-DZ_Realloc=c_Z_Realloc
	-DZ_Strdup=c_Z_Strdup
	-DHunk_Alloc=c_Hunk_Alloc
	-DHunk_AllocName=c_Hunk_AllocName
	-DHunk_HighAllocName=c_Hunk_HighAllocName
	-DHunk_Strdup=c_Hunk_Strdup
	-DHunk_TempAlloc=c_Hunk_TempAlloc
	-DHunk_LowMark=c_Hunk_LowMark
	-DHunk_FreeToLowMark=c_Hunk_FreeToLowMark
	-DHunk_HighMark=c_Hunk_HighMark
	-DHunk_FreeToHighMark=c_Hunk_FreeToHighMark
	-DHunk_Check=c_Hunk_Check
	-DCache_Flush=c_Cache_Flush
	-DCache_Check=c_Cache_Check
	-DCache_Free=c_Cache_Free
	-DCache_Alloc=c_Cache_Alloc
	-DCache_Report=c_Cache_Report
)

build_arm() {
	local arm=$1
	shift
	local defines=("$@")

	echo "== compiling the C original, the shim and the harness ($arm arm) =="
	cc "${INCS[@]}" "${CFLAGS[@]}" "${defines[@]}" "${RENAME[@]}" \
		-c "$engine/h2shared/zone.c" -o "$OUT/zone_c_$arm.o"
	cc "${INCS[@]}" "${CFLAGS[@]}" "${defines[@]}" \
		-c "$rust_root/zone_target.c" -o "$OUT/zone_target_$arm.o"
	cc "${INCS[@]}" "${CFLAGS[@]}" "${defines[@]}" \
		-c "$here/diff_harness.c" -o "$OUT/harness_$arm.o"
	cc "${INCS[@]}" "${CFLAGS[@]}" \
		-c "$root/common/strlcpy.c" -o "$OUT/strlcpy_$arm.o"

	echo "== linking both implementations into one binary ($arm arm) =="
	cc -o "$OUT/harness_$arm" "$OUT/harness_$arm.o" "$OUT/zone_c_$arm.o" \
		"$OUT/zone_target_$arm.o" "$OUT/strlcpy_$arm.o" "$RUST_LIB" -lm
}

run_arm() {
	local arm=$1
	local log="$OUT/run_$arm.log"

	echo
	echo "== running the $arm arm =="
	# The timeout is load-bearing, not defensive habit.  The zone's block list
	# is a circular list walked by a rover, and the cache has an LRU list plus
	# an address-ordered list over the same records; a port that gets one of
	# those links wrong loops forever instead of failing, and a gate that hangs
	# is worse than one that fails.
	local harness_timeout="${HARNESS_TIMEOUT:-120}"
	set +e
	timeout -k 5 "$harness_timeout" "$OUT/harness_$arm" >"$log" 2>&1
	local status=$?
	set -e
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the $arm harness did not finish within ${harness_timeout}s" >&2
		tail -20 "$log" >&2
		exit 1
	fi
	cat "$log"
	if [ "$status" -ne 0 ]; then
		echo "FAIL: the $arm harness exited $status" >&2
		exit 1
	fi
	grep -q '^RESULT: PASS$' "$log" || {
		echo "FAIL: the $arm harness did not print 'RESULT: PASS'" >&2
		exit 1
	}
}

build_arm client -DGLQUAKE
build_arm serveronly -DSERVERONLY

run_arm client
run_arm serveronly

echo
echo "== benchmark (client arm) =="
"$OUT/harness_client" --bench

echo "RESULT: PASS"
