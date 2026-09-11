#!/usr/bin/env bash
#
# Build and run the hashindex differential harness.
#
# Links the Rust staticlib and the C original into ONE binary, by compiling
# hashindex.c a second time with its exported symbols renamed to c_Hash_*.  The
# header is not modified for this: -D renames both the definitions in the .c and
# the prototypes it sees in hashindex.h.
#
# Usage:
#   tests/run_diff_harness.sh                 # uses target/release staticlib
#   RUST_LIB=/path/to/libhashindex_rs.a tests/run_diff_harness.sh
#
# The crate must have been built first (cargo build --release).
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/.." && pwd)"
engine="$(cd "$crate/../.." && pwd)"
root="$(cd "$engine/.." && pwd)"

RUST_LIB="${RUST_LIB:-$crate/target/release/libhashindex_rs.a}"
OUT="${OUT:-$(mktemp -d)}"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first:  cargo build --release --manifest-path $crate/Cargo.toml" >&2
	exit 2
fi

# Same include paths the engine's own CMake build uses for this file.
INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Rename only the five exported symbols.  Hash_First/Hash_Next and the two key
# generators are `static inline` in the header and have no symbols to rename.
RENAME=(
	-DHash_Allocate=c_Hash_Allocate
	-DHash_Free=c_Hash_Free
	-DHash_Add=c_Hash_Add
	-DHash_Remove=c_Hash_Remove
	-DHash_Clear=c_Hash_Clear
)

echo "== compiling the C original with renamed symbols =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$engine/h2shared/hashindex.c" -o "$OUT/hashindex_c.o"

echo "== compiling the differential harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/hashindex_c.o" "$RUST_LIB"

echo "== running =="
"$OUT/diff_harness"
