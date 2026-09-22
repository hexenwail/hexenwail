#!/usr/bin/env bash
#
# check-rust.sh -- the whole Rust gate, as CI runs it.
#
# The engine links engine/rust into every target, so the Rust ports are not a
# configuration to test on the side; they are the build.  This runs, in order:
#
#   1. toolchain parity: the nixpkgs rustc this shell provides must be the
#      version engine/rust/rust-toolchain.toml pins, which is what the
#      WebAssembly builds use.  A flake.lock bump that moves one and not the
#      other would have the web client compiling different Rust.
#   2. the removed switches stay removed: configuring with any old
#      -DUSE_*_RS=OFF must stop with the explanation, not silently link Rust.
#   3. one engine build of glhexen2, h2ded and hwsv, shared by every gate
#      below (it used to be two per gate: twenty-two builds of one tree).
#   4. each port's gate -- crate, differential harness against the C
#      original, symbol and stray-object checks on the shared build.
#   5. the Rust/C struct layouts on the host (scripts/check-rust-abi.sh; the
#      PWA job runs the same check for wasm32).
#
# Every check runs even after one fails, so a red run lists all of them.  The
# one exception is a failed shared build: the per-port gates all inspect it,
# so they are reported as skipped rather than run against nothing.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"

for tool in cc cargo rustc cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

failed=()

echo "== 1. toolchain parity =="
pinned=$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$root/engine/rust/rust-toolchain.toml")
have=$(rustc --version | awk '{print $2}')
if [ -z "$pinned" ]; then
	echo "FAIL: no channel in engine/rust/rust-toolchain.toml" >&2
	failed+=(toolchain)
elif [ "$have" != "$pinned" ]; then
	echo "FAIL: rustc is $have but engine/rust/rust-toolchain.toml pins $pinned." >&2
	echo "      Set the pin to the nixpkgs version, so the WebAssembly builds" >&2
	echo "      compile the same Rust as every other target." >&2
	failed+=(toolchain)
else
	echo "  rustc $have matches the pin"
fi

echo
echo "== 2. the removed USE_*_RS switches refuse to configure =="
if cmake -B "$work/removed-flag" -S "$root/engine" -DUSE_CRC_RS=OFF \
		>"$work/removed-flag.log" 2>&1; then
	echo "FAIL: -DUSE_CRC_RS=OFF configured; it must stop with an error" >&2
	failed+=(removed-flag)
elif ! grep -q 'USE_CRC_RS=OFF is no longer supported' "$work/removed-flag.log"; then
	echo "FAIL: -DUSE_CRC_RS=OFF failed, but not with the removed-switch message:" >&2
	tail -20 "$work/removed-flag.log" >&2
	failed+=(removed-flag)
else
	echo "  -DUSE_CRC_RS=OFF stops at configure with the explanation"
fi

echo
echo "== 3. the shared engine build =="
ports=(hashindex mathlib sizebuf crc link-ops msg-io info-str strlcpy strlcat huffman wad)
if ENGINE_BUILD=$(rust_gate_engine_build "$work"); then
	export ENGINE_BUILD
	echo "  $ENGINE_BUILD"
else
	# Every per-port gate checks symbols in this build, so none can run; the
	# layout check below does not need it and still does.
	failed+=(engine-build "${ports[@]/#/skipped:}")
	ports=()
fi

echo
echo "== 4. per-port gates =="
for port in "${ports[@]}"; do
	echo
	echo "---- $port"
	if WORKDIR="$work/$port" bash "$root/scripts/check-rust-$port.sh"; then
		:
	else
		failed+=("$port")
	fi
done

echo
echo "== 5. Rust/C struct layouts (host) =="
if ! WORKDIR="$work/abi" bash "$root/scripts/check-rust-abi.sh"; then
	failed+=(abi)
fi

echo
if [ "${#failed[@]}" -ne 0 ]; then
	echo "FAIL: ${failed[*]}" >&2
	exit 1
fi
echo "PASS: every Rust gate -- toolchain pinned, the old switches rejected, 11"
echo "      ports agree with their C originals and link once, layouts match."
