#!/usr/bin/env bash
#
# check-rust-mathlib.sh -- gate for the Rust port of mathlib.c.
#
# WHY THIS EXISTS
#
#   The Rust mathlib is the only one any engine target links; mathlib.c is
#   compiled by nothing in the engine build.
#
#   A build gate alone proves the crate compiles and links; it says nothing
#   about whether the Rust implementation still computes what the C original
#   computed.  The fixture replay is the half that does.
#
# WHAT IT CHECKS
#
#   1. the crate builds, offline, with no dependencies
#   2. the fixture replay passes -- the values frozen from the C original
#      (scalar results, vectors, matrices, plane classification, floor
#      division, struct layout) are reproduced by the Rust port, with a record
#      floor so a truncated fixture cannot read green.  No C compiler: the
#      differential harness against mathlib.c was retired (#475);
#      tests/gen_fixtures.sh --check audits the fixture against C and
#      tests/mutants.sh shows it can fail, neither run here
#   3. the engine build every gate shares (scripts/lib/rust-gate.sh)
#   4. exactly one definition of each of the 12 symbols in each binary; this
#      is the duplicate-symbol hazard the CMake source-list removal exists to
#      prevent, and the failure mode if that removal is ever lost
#
# Requires cargo/rustc, cmake and nm -- run it inside `nix develop`.
# No game data needed for the default checks: nothing here starts the engine.
#
#   ./scripts/check-rust-mathlib.sh            # fast: crate, replay, builds
#   OFF_BIN=... ./scripts/check-rust-mathlib.sh --engine
#       also run the real engine and diff it end to end against a C-only
#       reference binary (how to build one: scripts/lib/rust-gate.sh)
#
# --engine additionally resolves the demo data and Xvfb out of nix itself and
# runs the shared engine smoke, so it needs network (or
# a warm store) on a first run.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
crate="$root/engine/rust/mathlib"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(Q_isnan anglemod GreatestCommonDivisor NearestMultiple Q_log2 \
         Invert24To16 BOPS_Error FloorDivMod AngleVectors \
         R_ConcatRotations R_ConcatTransforms BoxOnPlaneSide)

for tool in cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate (offline, no dependencies) =="
cargo build --release --offline --manifest-path "$crate/Cargo.toml"

echo
echo "== 2. fixture replay: the frozen engine/h2shared/mathlib.c oracle, plain cargo test =="
replay_log="$work/replay.log"
if ! cargo test --offline --manifest-path "$crate/replay/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: mathlib fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that the replay tests themselves ran, so
# a renamed or filtered-out test cannot read as green.
for t in replay_c_fixtures angle_vectors_null_guards bops_bad_signbits_calls_sys_error \
         floordivmod_zero_denominator_calls_sys_error floordivmod_negative_denominator_calls_sys_error; do
	grep -q "^test $t \.\.\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/tests/fixtures/mathlib.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 1000 ]; then
	echo "FAIL: mathlib fixture has $records records, below the 1000 floor" >&2
	exit 1
fi
echo "  replay ok ($records records)"

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. exactly one definition of each symbol, in each binary =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1." >&2
			echo "      If 0, the archive was not linked; if 2, mathlib.c is back in" >&2
			echo "      an engine source list in engine/CMakeLists.txt." >&2
			exit 1
		fi
	done
	echo "  $bin: 12/12 symbols exactly once"
done

# The C object must not have been compiled into the build either, or the
# single definitions above are luck rather than construction.
stray=$(find "$engine_build" -name 'mathlib.c.o' | wc -l)
if [ "$stray" -ne 0 ]; then
	echo "FAIL: $stray mathlib.c.o object(s) in the engine build" >&2
	exit 1
fi

echo
if [ "$run_engine" -eq 1 ]; then
	echo "== 5. engine smoke: run the REAL engine against a C-only reference and diff =="
	reference=$(rust_gate_reference_bin)
	# Resolved out of nix rather than required from the caller: the store hash
	# in the demo path changes on every bump, and a check that is tedious to
	# run is a check that stops being run.
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$engine_build/bin/glhexen2" \
	OFF_BIN="$reference" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" mathlib

	echo
	echo "PASS: Rust mathlib -- fixture replay green; all three targets"
	echo "      link it with no duplicate symbol and no C object; and the real"
	echo "      engine produces the C-only reference's output on a pak load and a"
	echo "      map load."
else
	echo "PASS: Rust mathlib -- fixture replay green, and all three targets"
	echo "      link it with no duplicate symbol and no C object."
	echo "      (re-run with --engine and OFF_BIN=... to also drive the real engine)"
fi
