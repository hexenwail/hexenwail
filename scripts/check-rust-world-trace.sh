#!/usr/bin/env bash
#
# check-rust-world-trace.sh -- gate for the Rust port of the hull traces,
# SV_HullPointContents and SV_RecursiveHullCheck, out of engine/hexen2/world.c
# and engine/hexenworld/server/world.c (#455).
#
# The differential half runs engine/rust/world_trace/tests twice: against the
# Hexen II original with the archive's world_trace feature, and against the
# HexenWorld original with world_trace_hw, the feature hwsv's archive carries.
# Each run compares every return value, trace_t, console line and fatal call
# byte for byte (NaN payloads canonicalised), requires its outcome floors (startsolid, impacts, the
# "backup past 0" exit, hangs on NaN, fatal calls from both functions), and
# requires every line of its C original to have run under gcov, but for one
# clamp proven unreachable in run_diff_harness.sh.
#
# The CMake half proves the shared engine build links the port exactly once
# per binary, and that neither world.c object still defines either function.
# check-rust-archives.sh holds the error-name split: only hwsv's archive may
# reach SV_Error.
#
# USAGE
#
#   ./scripts/check-rust-world-trace.sh
#
# Requires cc, gcov, cargo/rustc, cmake and nm -- run inside `nix develop`.
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
symbols=(SV_HullPointContents SV_RecursiveHullCheck)

for tool in cc gcov cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

step=1
for variant in h2 hw; do
	if [ "$variant" = h2 ]; then feature=world_trace; else feature=world_trace_hw; fi
	echo "== $step. $variant: build the crate with $feature =="
	step=$((step + 1))
	cargo build --release --offline \
		--manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target-$variant" \
		--features "$feature"
	lib="$work/target-$variant/release/libengine_rs.a"

	echo
	echo "== $step. $variant: differential harness, Rust vs the C original =="
	step=$((step + 1))
	log="$work/diff-$variant.log"
	set +e
	VARIANT=$variant RUST_LIB="$lib" OUT="$work/diff-$variant" \
		bash "$crate/world_trace/tests/run_diff_harness.sh" >"$log" 2>&1
	status=$?
	set -e
	grep -E '^(outcomes:|          [a-z]|generated:|            [-0-9a-z%]|timed phase:|directed:)|^world_trace differential harness|^coverage:|^RESULT:' "$log" || true
	if [ "$status" -ne 0 ] || ! grep -q '^RESULT: PASS$' "$log"; then
		echo "FAIL: $variant differential harness (exit $status)" >&2
		grep -E 'MISMATCH|COVERAGE|FAIL|#####|error' "$log" >&2 || tail -40 "$log" >&2
		exit 1
	fi
	# RESULT alone would still pass with the enumeration cut to nothing, so
	# floor the case count at six digits.  Observed: 416,400 per variant.
	count_line=$(grep -E '^world_trace differential harness \([A-Za-z_]+\): PASS \(' "$log" || true)
	count=$(printf '%s\n' "$count_line" | sed -n 's/.*PASS (\([0-9]*\) cases.*/\1/p')
	if [ "${#count}" -lt 6 ]; then
		echo "FAIL: $variant harness case count is below the 100,000-case floor" >&2
		echo "      observed: ${count_line:-<no PASS line>}" >&2
		exit 1
	fi
	echo
done

echo "== $step. the engine build: glhexen2, h2ded and hwsv =="
step=$((step + 1))
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== $step. exactly one definition per binary, none left in world.c =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: ${symbols[*]} defined exactly once"
done
# Every world.c object the build made -- the client's and h2ded's hexen2 one,
# hwsv's hexenworld one -- must only reference the two symbols now.
objects=$(find "$engine_build" -name 'world.c.o' -o -name 'world.c.obj' | sort)
nobj=$(printf '%s\n' "$objects" | grep -c . || true)
if [ "$nobj" -lt 3 ]; then
	echo "FAIL: expected a world.c object per target (3), found $nobj" >&2
	exit 1
fi
while IFS= read -r obj; do
	for sym in "${symbols[@]}" WackyBugFixer; do
		if nm "$obj" | grep -qE " [Tt] $sym\$"; then
			echo "FAIL: ${obj#"$engine_build"/} still defines $sym" >&2
			exit 1
		fi
	done
done <<<"$objects"
echo "  $nobj world.c object(s): none defines the traces"
stray=$(find "$engine_build" -name 'world_trace_*.c.o' -o -name 'c_ref.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray harness fixture object(s) in the engine build" >&2
	exit 1
}

echo "PASS: Rust hull traces -- the harness agrees with both C originals, every"
echo "      line of them ran, each binary links the port once, and neither"
echo "      world.c defines it."
