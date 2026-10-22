#!/usr/bin/env bash
#
# check-rust-hashindex.sh -- gate for the Rust port of hashindex.c.
#
# WHY THIS EXISTS
#
#   The Rust hashindex is the only one any engine target links; hashindex.c
#   is compiled by nothing in the engine build, only by the fixture generator.
#
#   The fixture replay is the half that matters.  A build gate alone proves the
#   crate compiles and links; it says nothing about whether the Rust
#   implementation still computes what the C original computed.
#
# WHAT IT CHECKS
#
#   1. the crate builds, offline, with no dependencies
#   2. the fixture replay passes -- the operation sequences frozen from the C
#      original (tests/fixtures/hashindex.txt: raw arrays, size/mask/NULL state
#      and per-bucket chain ORDER after every step, plus the fatal paths'
#      Sys_Error messages) are reproduced by the Rust port, and so is an
#      independent reference model over ~1M seeded operations.  Run with
#      `cargo test --release`, the profile the engine ships.  The differential
#      harness against hashindex.c was retired (#475)
#   2b. tests/gen_fixtures.sh --check: the fixture still matches what the
#      unmodified hashindex.c produces (needs cc).  tests/mutants.sh shows the
#      replay can fail; it rewrites a tracked source, so it is not run here
#   3. the engine build every gate shares (scripts/lib/rust-gate.sh)
#   4. exactly one definition of each of the five symbols in each binary; this
#      is the duplicate-symbol hazard the CMake source-list removal exists to
#      prevent, and the failure mode if that removal is ever lost
#
# Requires cc, cargo/rustc and cmake -- run it inside `nix develop`.
# No game data needed for the default checks: nothing here starts the engine.
#
#   ./scripts/check-rust-hashindex.sh            # fast: crate, replay, builds
#   OFF_BIN=... ./scripts/check-rust-hashindex.sh --engine
#       also run the real engine and diff it end to end against a C-only
#       reference binary (how to build one: scripts/lib/rust-gate.sh)
#
# --engine additionally resolves the demo data and Xvfb out of nix itself and
# runs engine/rust/hashindex/tests/run_engine_smoke.sh, so it needs network (or
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
crate="$root/engine/rust/hashindex"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(Hash_Allocate Hash_Free Hash_Add Hash_Remove Hash_Clear)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate (offline, no dependencies) =="
cargo build --release --offline --manifest-path "$crate/Cargo.toml"

echo
echo "== 2. fixture replay: the frozen engine/h2shared/hashindex.c oracle, cargo test --release =="
replay_log="$work/replay.log"
if ! cargo test --release --offline --manifest-path "$crate/replay/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: hashindex fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that the replay tests themselves ran, so
# a renamed or filtered-out test cannot read as green.
for t in replay_c_fixtures fatal_paths_call_sys_error fatal_child \
         model_matches_port_on_long_seeded_sequences; do
	grep -q "^test $t \.\.\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/tests/fixtures/hashindex.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 3000 ]; then
	echo "FAIL: hashindex fixture has $records records, below the 3000 floor" >&2
	exit 1
fi
echo "  replay ok ($records records)"

echo
echo "== 2b. the fixture still matches engine/h2shared/hashindex.c (tests/gen_fixtures.sh --check) =="
# Nothing else re-proves the frozen values against C: the replay only shows
# the Rust agrees with the fixture.  Drift in the C or an edited fixture fails
# the gate here.  Needs cc.
if ! check_log=$(bash "$crate/tests/gen_fixtures.sh" --check 2>&1); then
	echo "FAIL: hashindex fixture no longer matches the C original" >&2
	echo "$check_log" >&2
	exit 1
fi
echo "  ${check_log##*$'\n'}"

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
			echo "      If 0, the archive was not linked; if 2, hashindex.c is back in" >&2
			echo "      an engine source list in engine/CMakeLists.txt." >&2
			exit 1
		fi
	done
	echo "  $bin: 5/5 symbols exactly once"
done

# The C object must not have been compiled into the build either, or the
# single definitions above are luck rather than construction.
stray=$(find "$engine_build" -name 'hashindex.c.o' | wc -l)
if [ "$stray" -ne 0 ]; then
	echo "FAIL: $stray hashindex.c.o object(s) in the engine build" >&2
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
		"$crate/tests/run_engine_smoke.sh" hashindex

	echo
	echo "PASS: Rust hashindex -- fixture replay green; all three targets"
	echo "      link it with no duplicate symbol and no C object; and the real"
	echo "      engine produces the C-only reference's output on a pak load and a"
	echo "      map load."
else
	echo "PASS: Rust hashindex -- fixture replay green, and all three targets"
	echo "      link it with no duplicate symbol and no C object."
	echo "      (re-run with --engine and OFF_BIN=... to also drive the real engine)"
fi
