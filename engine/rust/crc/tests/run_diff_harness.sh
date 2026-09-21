#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build crc.c with renamed exports and compare it with the consolidated Rust
# engine staticlib's crc feature.
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features hashindex,mathlib,sizebuf,crc" >&2
	exit 2
fi

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)
RENAME=(
	-DCRC_Init=c_CRC_Init
	-DCRC_ProcessByte=c_CRC_ProcessByte
	-DCRC_ProcessBlock=c_CRC_ProcessBlock
	-DCRC_Value=c_CRC_Value
	-DCRC_Block=c_CRC_Block
)

echo "== compiling the C original with renamed symbols =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$root/common/crc.c" -o "$OUT/crc_c.o"
echo "== compiling the differential harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"
echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/crc_c.o" "$RUST_LIB" -lm
echo "== running =="
"$OUT/diff_harness"
echo "RESULT: PASS"
