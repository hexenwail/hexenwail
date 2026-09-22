#!/usr/bin/env bash
#
# check-rust-hashindex.sh -- gate for the Rust port of hashindex.c.
#
# WHY THIS EXISTS
#
#   The Rust hashindex is the only one any engine target links; hashindex.c
#   is compiled by the differential harness below and by nothing else.
#
#   The differential harness is the half that matters.  A build gate alone
#   proves the crate compiles and links; it says nothing about whether the Rust
#   implementation still computes what the C original computes.
#
# WHAT IT CHECKS
#
#   1. the crate builds, offline, with no dependencies
#   2. the differential harness passes -- Rust and C agree on the raw array
#      contents AND on per-bucket chain ORDER, over 1.2M operations, plus
#      abort parity on the four fatal paths
#   3. the engine build every gate shares (scripts/lib/rust-gate.sh)
#   4. exactly one definition of each of the five symbols in each binary; this
#      is the duplicate-symbol hazard the CMake source-list removal exists to
#      prevent, and the failure mode if that removal is ever lost
#
# Requires cc, cargo/rustc and cmake -- run it inside `nix develop`.
# No game data needed for the default checks: nothing here starts the engine.
#
#   ./scripts/check-rust-hashindex.sh            # fast: crate, harness, builds
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
rust_lib="$crate/target/release/libhashindex_rs.a"
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
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$rust_lib" OUT="$work/diff" "$crate/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checks run:' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status (a divergence)" >&2
	sed -n '/FAIL /p' "$diff_log" | head -20 >&2
	exit 1
fi
# Re-assert the summary line too: a harness that somehow exited 0 without
# having run anything must not pass as green.
grep -q '^RESULT: PASS$' "$diff_log" || {
	echo "FAIL: harness did not print 'RESULT: PASS'" >&2
	exit 1
}
# RESULT: PASS reflects the failure count alone, so it stays green however little
# ran -- floor the check count at six digits (a hundred thousand checks) rather
# than pinning it, which would mean editing the gate every time the enumeration
# is deliberately widened.
# Observed today: 342,295 checks.  The floor is one decimal place below it.
count_line=$(grep -E '^checks run: [0-9]+' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 6 ]; then
	echo "FAIL: harness check count is below the hundred-thousand floor" >&2
	echo "      observed: ${count_line:-<no 'checks run: N   failures: M' line>}" >&2
	exit 1
fi

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
	echo "PASS: Rust hashindex -- differential harness green; all three targets"
	echo "      link it with no duplicate symbol and no C object; and the real"
	echo "      engine produces the C-only reference's output on a pak load and a"
	echo "      map load."
else
	echo "PASS: Rust hashindex -- differential harness green, and all three targets"
	echo "      link it with no duplicate symbol and no C object."
	echo "      (re-run with --engine and OFF_BIN=... to also drive the real engine)"
fi
