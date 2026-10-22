#!/usr/bin/env bash
#
# check-rust-info-str.sh -- gate for the Rust port of hexenworld/shared/info_str.c.
#
# The fixture half replays expected values frozen from the C original, compiled
# exactly as hwsv does (-DH2W -DSERVERONLY, hexenworld/shared ahead of
# h2shared), against the Rust module with no C compiler: empty and malformed
# info strings, missing, duplicate and valueless keys, the 63/64 and >= maxsize
# rejections, the ascii and high-bit filtering including the `c > 13` retention,
# both sv_highchars states and the pre-registration one, the four-buffer
# rotation by pointer class and by the text of every held slot, and the in-place
# rewrites of Info_RemoveKey and Info_RemovePrefixedKeys down to the stale bytes
# memmove leaves.  The same crate checks the port against an independent
# reference model of the info-string semantics: every string over { \ a b * } up
# to length 8 through ten operations, seeded random operation sequences on one
# evolving buffer, and a key/value length x maxsize x filter-mode grid, all in
# guard zones and byte for byte.  tests/gen_fixtures.sh --check (step 1b, needs
# cc) re-proves the fixture against hexenworld/shared/info_str.c.
#
# The CMake half checks the engine build every gate shares
# (scripts/lib/rust-gate.sh), and covers the shape this port introduced: info_str.c is
# compiled into HWSV_SOURCES alone, so the C symbols exist in hwsv and nowhere
# else, while the Rust archive is linked into all three targets.  hwsv must
# therefore define each of the six exactly once, and glhexen2 and h2ded at most
# once -- whether the unreferenced member is pulled in depends on the linker.
#
# There is deliberately no --engine smoke arm.  The hwsv smoke CI runs dies at
# FS_Init's data gate before any client connects, so no Info_ call is reached;
# an arm that cannot reach the code under test would only look like evidence.
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
symbols=(
	Info_ValueForKey Info_RemoveKey Info_RemovePrefixedKeys
	Info_SetValueForKey Info_SetValueForStarKey Info_Print
)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. fixture replay and reference model: the frozen info_str.c oracle, cargo test --release =="
# The differential harness against the C original was retired (#475).  Its
# expected values are frozen in engine/rust/info_str/tests/fixtures/info_str.txt
# and replayed against src/info_str.rs with no C compiler.  The fixture is
# re-proved against C in step 1b by tests/gen_fixtures.sh --check, and shown
# able to fail by tests/mutants.sh, which rewrites a tracked source and so is
# not run here (docs/rust-harness-retirement.md).  The replay runs --release:
# the engine ships release with panic=abort, so the replay must too.
replay_log="$work/replay.log"
if ! cargo test --release --offline --manifest-path "$crate/info_str/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: info_str fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that the replay tests themselves ran, so
# a renamed or filtered-out test cannot read as green.
for t in replay_c_fixtures sweep_digest_matches_c sweep_matches_reference_model \
         random_sequences_match_reference_model boundary_grid_matches_reference_model; do
	grep -q "^test $t \\.\\.\\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/info_str/tests/fixtures/info_str.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 800 ]; then
	echo "FAIL: info_str fixture has $records records, below the 800 floor" >&2
	exit 1
fi
echo "  5/5 replay tests ok ($records records; per-kind floors live in replay.rs)"

echo
echo "== 1b. the fixture still matches info_str.c (tests/gen_fixtures.sh --check) =="
# Nothing else re-proves the frozen values against C: the replay only shows
# the Rust agrees with the fixture.  Drift in the C or an edited fixture fails
# the gate here.  Needs cc.
if ! check_log=$(bash "$crate/info_str/tests/gen_fixtures.sh" --check 2>&1); then
	echo "FAIL: info_str fixture no longer matches the C original" >&2
	echo "$check_log" >&2
	exit 1
fi
echo "  $check_log"

echo
echo "== 2. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 3. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/hwsv/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
# The cvar_t layout accessors that check-rust-abi.sh compares with the real
# struct.
for sym in CvarC_sizeof CvarC_offsetof_integer; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} symbols and 2/2 accessors once"

# This is the invariant that made the port read sv_highchars through
# Cvar_FindVar instead of the global.  The consolidated archive is one object
# member, so any target pulling it for any other port inherits every undefined
# symbol in it -- and only hwsv defines sv_highchars.  A direct reference here
# is a link error in glhexen2 and h2ded, so it must never come back.
stray_hwsv_only=$(nm -u "$archive" 2>/dev/null | grep -cE " U sv_highchars\$" || true)
if [ "$stray_hwsv_only" -ne 0 ]; then
	echo "FAIL: libengine_rs.a references the hwsv-only global sv_highchars;" >&2
	echo "      glhexen2 and h2ded do not define it and will not link." >&2
	exit 1
fi
echo "  libengine_rs.a: no reference to the hwsv-only sv_highchars global"

echo
echo "== 4. exactly one info_str definition where it is reachable =="
# All six are called from hexenworld/server, so hwsv must define each exactly
# once.  The Hexen II targets contain no caller: a definition there is the
# archive member being pulled in for another port, which is allowed but must
# never be more than one.
for sym in "${symbols[@]}"; do
	n=$(nm "$engine_build/bin/hwsv" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: hwsv: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  hwsv: ${#symbols[@]}/${#symbols[@]} symbols exactly once"
for bin in glhexen2 h2ded; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -gt 1 ]; then
			echo "FAIL: $bin: $sym is defined $n times, expected at most 1" >&2
			exit 1
		fi
	done
	echo "  $bin: every info_str symbol at most once (no caller in this target)"
done
stray=$(find "$engine_build" -name 'info_str.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray info_str.c.o object(s) in the engine build" >&2
	exit 1
}

echo
echo "PASS: Rust info_str -- fixture replay and reference model green, the"
echo "      fixture still matches the C as hwsv compiles it, the consolidated"
echo "      archive links exactly one definition where the code is reachable,"
echo "      and no info_str.c object is left in the engine build."