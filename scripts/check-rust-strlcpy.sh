#!/usr/bin/env bash
#
# check-rust-strlcpy.sh -- gate for the Rust port of common/strlcpy.c.
#
# The fixture half replays expected values frozen from the C original (every
# source length 0..20 against every size 0..24, junk after the NUL, long
# sources around the size boundary, seeded random mixes) against the Rust port
# with guard bytes and unaligned offsets, with no C compiler.  The CMake half
# proves Hexen II and HexenWorld both link it: the engine build every gate
# shares (scripts/lib/rust-gate.sh) has it exactly once per binary with no C
# object left in the build.
#
# Requires cargo/rustc, cmake and nm -- run inside `nix develop`.
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
symbol=q_strlcpy

for tool in cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. fixture replay: the frozen common/strlcpy.c oracle, plain cargo test =="
# The differential harness against the C original was retired (#475).  Its
# expected values are frozen in engine/rust/strlcpy/tests/fixtures/strlcpy.txt
# and replayed against src/strlcpy.rs with no C compiler.  The fixture is
# audited against C by tests/gen_fixtures.sh --check, and shown able to fail by
# tests/mutants.sh; neither runs here (docs/rust-harness-retirement.md).
replay_log="$work/replay.log"
if ! cargo test --offline --manifest-path "$crate/strlcpy/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: strlcpy fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that the replay tests themselves ran, so
# a renamed or filtered-out test cannot read as green.
for t in replay_c_fixtures size_zero_leaves_a_real_destination_alone; do
	grep -q "^test $t \\.\\.\\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/strlcpy/tests/fixtures/strlcpy.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 600 ]; then
	echo "FAIL: strlcpy fixture has $records records, below the 600 floor" >&2
	exit 1
fi
echo "  replay_c_fixtures ok ($records records)"

echo
echo "== 2. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 3. exactly one q_strlcpy definition per binary =="
archive="$engine_build/rust/client/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
n=$(nm "$archive" 2>/dev/null | grep -cE " T $symbol\$" || true)
if [ "$n" -ne 1 ]; then
	echo "FAIL: libengine_rs.a: $symbol has $n definitions, expected exactly 1" >&2
	exit 1
fi
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	n=$(nm "$path" | grep -cE " [Tt] $symbol\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: $bin: $symbol has $n definitions, expected exactly 1" >&2
		exit 1
	fi
	echo "  $bin: $symbol exactly once"
done
stray=$(find "$engine_build" -name 'strlcpy.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray strlcpy.c.o object(s) in the engine build" >&2
	exit 1
}

echo "PASS: Rust strlcpy -- fixture replay green, the consolidated"
echo "      archive links exactly one definition per target, and no strlcpy.c"
echo "      object is left in the engine build."
