#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Build strlcat.c with a renamed export and compare it with the consolidated
# Rust engine staticlib's strlcat feature.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
root="$(cd "$rust_root/../.." && pwd)"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: consolidated Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features hashindex,mathlib,sizebuf,crc,link_ops,strlcpy,strlcat" >&2
	exit 2
fi

CFLAGS=(-O1 -g -Wall -Wextra -Werror)

echo "== compiling the C original with renamed symbols =="
cc "${CFLAGS[@]}" -Dq_strlcat=c_q_strlcat \
	-c "$root/common/strlcat.c" -o "$OUT/strlcat_c.o"
echo "== compiling the differential harness =="
cc "${CFLAGS[@]}" \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"
echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/strlcat_c.o" "$RUST_LIB" -lm
echo "== running =="
"$OUT/diff_harness"
echo "RESULT: PASS"