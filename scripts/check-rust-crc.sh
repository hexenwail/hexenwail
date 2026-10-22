#!/usr/bin/env bash
#
# check-rust-crc.sh -- gate for the Rust port of common/crc.c.
#
# The fixture half replays expected values frozen from the C original
# (every CRC table entry, directed and seeded byte steps, CRC_Value, the
# published check value, and blocks of every length 0..40 plus longer ones at
# unaligned offsets) against the Rust port, with no C compiler.  The replay
# also compares the port with an independent bit-by-bit CRC-CCITT over all 2^24
# (crc, byte) steps, all 2^16 CRC_Value inputs and 5000 seeded blocks of up to
# 8192 bytes at offsets 0..15.  tests/gen_fixtures.sh --check (step 1b, needs
# cc) re-proves the fixture against common/crc.c.  The CMake
# half proves the consolidated engine archive exports it and the engine build
# every gate shares (scripts/lib/rust-gate.sh) links it exactly once per
# binary with no C object left in the build.
#
# Requires cargo/rustc, cc, cmake and nm -- run inside `nix develop`.
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

for tool in cargo cc cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. fixture replay: the frozen common/crc.c oracle, cargo test --release =="
# The differential harness against the C original was retired (#475).  Its
# expected values are frozen in engine/rust/crc/tests/fixtures/crc.txt and
# replayed against src/crc.rs with no C compiler.  The fixture is
# re-proved against C in step 1b by tests/gen_fixtures.sh --check, and shown
# able to fail by tests/mutants.sh, which rewrites a tracked source and so is not run here
# (docs/rust-harness-retirement.md).  The replay runs --release: the engine
# ships release with panic=abort, so the replay must too.
replay_log="$work/replay.log"
if ! cargo test --release --offline --manifest-path "$crate/crc/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: crc fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that the replay test itself ran, so a
# renamed or filtered-out test cannot read as green.
for t in replay_c_fixtures reference_pins_the_published_check_value \
         every_crc_byte_step_matches_the_reference \
         crc_value_matches_the_reference_for_every_input \
         seeded_blocks_up_to_8192_bytes_at_every_offset_match_the_reference; do
	grep -q "^test $t \.\.\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/crc/tests/fixtures/crc.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 400 ]; then
	echo "FAIL: crc fixture has $records records, below the 400 floor" >&2
	exit 1
fi
echo "  replay_c_fixtures ok ($records records)"

echo
echo "== 1b. the fixture still matches common/crc.c (tests/gen_fixtures.sh --check) =="
# Nothing else re-proves the frozen values against C: the replay only shows
# the Rust agrees with the fixture.  Drift in the C or an edited fixture fails
# the gate here.  Needs cc.
if ! check_log=$(bash "$crate/crc/tests/gen_fixtures.sh" --check 2>&1); then
	echo "FAIL: crc fixture no longer matches the C original" >&2
	echo "$check_log" >&2
	exit 1
fi
echo "  $check_log"

echo
echo "== 2. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 3. exactly one crc definition per binary =="
# The archive must export the whole C ABI.  In a linked binary, CRC_Value has
# no engine caller, so whether it is present depends on how rustc partitioned
# codegen units; require it at most once there, and every called symbol
# exactly once.
archive="$engine_build/rust/client/libengine_rs.a"
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

echo "PASS: Rust crc -- fixture replay green, the consolidated archive"
echo "      links exactly one symbol per target, and no crc.c object is left"
echo "      in the engine build."
