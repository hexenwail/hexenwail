#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/h2shared/wad.c the way glhexen2 compiles it -- -DGLQUAKE, with
# engine/hexen2 and engine/h2shared ahead of common/ on the include path -- with
# every exported name and all three exported globals renamed so it can share a
# binary with the Rust wad module, and compare the two byte for byte.
#
# Only one C variant exists to compare against: wad.c is in COMMON_SOURCES and
# is removed again for h2ded, and HWSV_SOURCES never lists it, so glhexen2 is
# the only target that compiles it.  The file has no #ifdefs of its own.
#
# The Rust staticlib must have been built with the wad feature.  FS_LoadZoneFile,
# Z_Free and Sys_Error -- the engine symbols the module calls -- are defined by
# diff_harness.c.
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features wad" >&2
	exit 2
fi

# The Hexen II include order, as the client uses it.
INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol wad.c exports, renamed -- including the three globals, which are
# part of the header's ABI.
RENAME=(
	-DW_LoadWadFile=c_W_LoadWadFile
	-DW_GetLumpinfo=c_W_GetLumpinfo
	-DW_GetLumpName=c_W_GetLumpName
	-DSwapPic=c_SwapPic
	-Dwad_numlumps=c_wad_numlumps
	-Dwad_lumps=c_wad_lumps
	-Dwad_base=c_wad_base
)

echo "== compiling the C original as glhexen2 does (-DGLQUAKE) =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$engine/h2shared/wad.c" -o "$OUT/wad_c.o"

echo "== compiling the differential harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/wad_c.o" \
	"$RUST_LIB" -lm

echo "== running =="
# The timeout is load-bearing, not defensive habit.
#
# The lump table is walked `while (i < wad_numlumps)`, and wad_numlumps comes
# out of the mapped header through LittleLong.  An implementation that byte
# swaps when it should not reads the header's {1,0,0,0} as 0x01000000 and walks
# sixteen million table entries off the end of a 4 KB zone instead of failing.
# A gate that hangs is worse than one that fails: CI cannot afford a job that
# never returns, and a hang looks like a slow machine rather than a broken port.
harness_timeout="${HARNESS_TIMEOUT:-60}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	echo "      That is what a lump count read with the wrong byte order looks" >&2
	echo "      like: the table walk never reaches wad_numlumps." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"
