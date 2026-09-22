#!/usr/bin/env bash
#
# check-rust-crc.sh -- gate for the Rust port of common/crc.c.
#
# The differential half proves the CCITT CRC is bit-identical to the C
# original: every (running value, byte) step exhaustively, CRC_Value over all
# 2^16 inputs, a published check value, and seeded random blocks at unaligned
# offsets.  The CMake half proves the consolidated engine archive exports it
# and the engine build every gate shares (scripts/lib/rust-gate.sh) links it
# exactly once per binary with no C object left in the build.
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
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(CRC_Init CRC_ProcessByte CRC_ProcessBlock CRC_Value CRC_Block)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/crc/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|differential harness:' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status" >&2
	cat "$diff_log" >&2
	exit 1
fi
grep -q '^RESULT: PASS$' "$diff_log" || {
	echo "FAIL: harness did not print 'RESULT: PASS'" >&2
	cat "$diff_log" >&2
	exit 1
}
# The PASS line carries the case count, and RESULT alone would still pass if the
# enumeration were cut down to nothing -- so floor the count at eight digits (ten
# million cases) rather than pinning it, which would mean editing the gate every
# time the enumeration is deliberately widened.
# Observed today: 16,884,495 cases, and the count is deterministic.  The floor
# is one decimal place below it.
count_line=$(grep -E '^crc differential harness: PASS \(' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 8 ]; then
	echo "FAIL: harness case count is below the ten-million floor" >&2
	echo "      observed: ${count_line:-<no 'crc differential harness: PASS (N cases)' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. exactly one crc definition per binary =="
# The archive must export the whole C ABI.  In a linked binary, CRC_Value has
# no engine caller, so whether it is present depends on how rustc partitioned
# codegen units; require it at most once there, and every called symbol
# exactly once.
archive="$engine_build/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} symbols exported exactly once"
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$sym" = CRC_Value ]; then
			[ "$n" -le 1 ] && continue
		elif [ "$n" -eq 1 ]; then
			continue
		fi
		echo "FAIL: $bin: $sym has $n definitions" >&2
		exit 1
	done
	echo "  $bin: called crc symbols exactly once, CRC_Value at most once"
done
stray=$(find "$engine_build" -name 'crc.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray crc.c.o object(s) in the engine build" >&2
	exit 1
}

echo "PASS: Rust crc -- differential harness green, the consolidated archive"
echo "      links exactly one symbol per target, and no crc.c object is left"
echo "      in the engine build."
