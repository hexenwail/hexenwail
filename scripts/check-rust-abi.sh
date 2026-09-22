#!/usr/bin/env bash
#
# check-rust-abi.sh -- the struct layouts the Rust engine archive shares with
# C must match what the target's C compiler produces.
#
# Usage: check-rust-abi.sh [--wasm]
#
#   (default)  build the archive for the host and run
#              engine/rust/tests/abi_layout.c natively.
#   --wasm     build it for wasm32-unknown-emscripten, compile the same C
#              with emcc and run it under node.  This is the target where it
#              matters: the WebAssembly client is ILP32, and the host-only
#              differential harnesses never see a 4-byte pointer.
#
# Requires cc, cargo, and for --wasm emcc, node and the
# wasm32-unknown-emscripten Rust target (rustup target add ...).
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

mode=host
for arg in "$@"; do
	case "$arg" in
		--wasm) mode=wasm ;;
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
engine="$root/engine"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"

# Every feature: this is the archive the engine links, so these are the
# layouts the engine depends on.
features=hashindex,mathlib,sizebuf,crc,link_ops,msg_io,info_str,strlcpy,strlcat,huffman,wad

extra_c=()
if [ "$mode" = wasm ]; then
	tools=(cargo emcc node)
	target=(--target wasm32-unknown-emscripten)
	lib="$work/target/wasm32-unknown-emscripten/release/libengine_rs.a"
	# The archive's exported data lives in C on wasm32; link it as the
	# engine does.  See engine/rust/wasm_globals.c.
	extra_c=("$crate/wasm_globals.c")
	CC=emcc
	exe="$work/abi_layout.js"
	run=(node "$exe")
else
	tools=(cargo cc)
	target=()
	lib="$work/target/release/libengine_rs.a"
	CC=cc
	exe="$work/abi_layout"
	run=("$exe")
fi
for tool in "${tools[@]}"; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH" >&2
		exit 2
	}
done

echo "== 1. build the engine archive ($mode) =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--target-dir "$work/target" \
	"${target[@]}" \
	--features "$features"

echo
echo "== 2. compile abi_layout.c with the target's C compiler and link it =="
# The HexenWorld include order: protocol.h must be hexenworld/shared's, whose
# usercmd_t is the one msg_io.rs mirrors.
"$CC" -std=gnu99 -O1 -Wall -Werror \
	-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common" \
	-DGLQUAKE -DH2W -DSERVERONLY \
	-o "$exe" "$crate/tests/abi_layout.c" "${extra_c[@]}" "$lib" -lm

echo
echo "== 3. run =="
"${run[@]}" | tee "$work/abi.log"
grep -q '^RESULT: PASS$' "$work/abi.log" || {
	echo "FAIL: abi_layout did not print 'RESULT: PASS'" >&2
	exit 1
}
echo "PASS: Rust/C struct layouts agree ($mode)"
