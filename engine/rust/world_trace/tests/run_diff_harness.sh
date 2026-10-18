#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the hull-trace differential harness for both C originals and run it.
#
#   VARIANT=h2  world_trace_h2.c (hexen2/world.c's), -DSERVERONLY as h2ded
#               compiles it, against an archive built with world_trace;
#   VARIANT=hw  world_trace_hw.c (hexenworld/server/world.c's), -DH2W
#               -DSERVERONLY as hwsv compiles it, against world_trace_hw.
#
# RUST_LIB names the archive for that variant.  The C original is compiled
# twice into each binary: as plain IEEE C (c_*), and with the flags every
# engine target ships with (f_*; ENGINE_CFLAGS below, from
# engine/CMakeLists.txt).  The port is held to the IEEE build; the
# -ffast-math one is compared and counted, never held (see diff_harness.c).
#
# Each variant runs twice: at -O2, and with --coverage on the IEEE build,
# where the same run must also execute every line of the C original -- the
# evidence that the generated cases reach the backup loop, the "backup past
# 0" exit and the fatal calls, rather than an assertion that they do.  Both
# runs are under a timeout: the port must never hang, so a hang fails here
# rather than stalling the gate.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:?set VARIANT=h2 or VARIANT=hw}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built for this variant}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }

case "$VARIANT" in
	h2)
		INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
		CDEFS=(-DSERVERONLY)
		fixture=world_trace_h2.c ;;
	hw)
		# hwsv's include order: hexenworld/server, hexenworld/shared, h2shared, common.
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common")
		CDEFS=(-DH2W -DSERVERONLY -DWT_HW)
		fixture=world_trace_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	echo "-DSV_HullPointContents=$1SV_HullPointContents" \
	     "-DSV_RecursiveHullCheck=$1SV_RecursiveHullCheck" \
	     "-DWackyBugFixer=$1WackyBugFixer"
}
# engine/CMakeLists.txt's add_compile_options for every target.
ENGINE_CFLAGS=(-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer)
WARN=(-Wall -Wno-unused-function)
# Generous: a full run takes well under a minute.  Only a hang reaches it.
LIMIT=${HARNESS_TIMEOUT:-600}

build() { # name ieee-cflags...
	local name=$1; shift
	mkdir -p "$OUT/$name"
	# shellcheck disable=SC2046  # rename's words are meant to split
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" "$@" $(rename c_) \
		-DWT_FIXTURE="\"$here/$fixture\"" \
		-c "$here/c_ref.c" -o "$OUT/$name/c_ref.o"
	# shellcheck disable=SC2046
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" "${ENGINE_CFLAGS[@]}" $(rename f_) \
		-DWT_FIXTURE="\"$here/$fixture\"" \
		-c "$here/c_ref.c" -o "$OUT/$name/c_ref_engine.o"
	# The harness itself is IEEE: it tests for NaN.
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/diff_harness.c" -o "$OUT/$name/diff_harness.o"
	cc "$@" -o "$OUT/$name/diff_harness" "$OUT/$name/diff_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/c_ref_engine.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$1" && timeout -k 10 "$LIMIT" ./diff_harness) || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s) -- the port must always return" >&2
	fi
	return "$status"
}

echo "== $VARIANT: the C original (IEEE and engine flags) against the Rust archive =="
build opt -O2 -g
run "$OUT/opt"

echo "== $VARIANT: the same run with C line coverage =="
build cov -O0 -g --coverage
run "$OUT/cov" >/dev/null
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
# gcov marks a line that never ran "#####" (and "=====" for one reached only
# through an exceptional path); a "-" line has no code.
#
# One line is exempt, by its exact text, because no input reaches it: the
# clamp's `else if (frac > 1) frac = 1;`.  With side 0 (t1 >= 0 > t2) the
# denominator fl(t1 - t2) is >= t1, so (t1 - DIST_EPSILON)/den < 1.  With
# side 1 (t1 < 0 <= t2), t1 is representable and the exact t1 - t2 is <= t1,
# so round-to-nearest gives den <= t1 < 0; then either the numerator
# t1 + DIST_EPSILON is positive and frac < 0, or |t1 + DIST_EPSILON| < |t1|
# <= |den| and frac < 1.  Rounding the double quotient to float can reach 1.0,
# never pass it, and NaN fails both comparisons.  Each original has the line
# once, and the exemption must match it exactly once, so it cannot hide
# anything else.
dead_line=$'\t\tfrac = 1;'
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" || true)
exempt=$(printf '%s\n' "$unrun" | awk -F: -v d="$dead_line" '{ t = $0; sub(/^[^:]*:[^:]*:/, "", t); if (t == d) n++ } END { print n + 0 }')
if [ "$exempt" -gt 1 ]; then
	echo "FAIL: the dead-clamp exemption matched $exempt lines of $fixture; expected at most 1" >&2
	exit 1
fi
unrun=$(printf '%s\n' "$unrun" | awk -v d="$dead_line" '{ t = $0; sub(/^[^:]*:[^:]*:/, "", t); if (t != d && $0 != "") print }')
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines), except $exempt: the unreachable frac > 1 clamp"
echo "RESULT: PASS"
