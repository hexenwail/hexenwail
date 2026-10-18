#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the SV_Move differential harness for both C originals and run it.
#
#   VARIANT=h2  world_move_h2.c (hexen2/world.c's), -DSERVERONLY as h2ded
#               compiles it, against an archive built with world_move;
#   VARIANT=hw  world_move_hw.c (hexenworld/server/world.c's), -DH2W
#               -DSERVERONLY as hwsv, against world_move + world_trace_hw.
#
# RUST_LIB names the archive for that variant.  The binary also compiles the
# real engine/rust/world_target.c with the variant's headers.  The C original
# goes in once, as plain IEEE C (see diff_harness.c for why no engine-flags
# build), and the run is repeated with --coverage, where every line of the
# original must run, but the one line proven unreachable below.  Both runs
# are under a timeout.
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
		INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DSERVERONLY)
		fixture=world_move_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DWT_HW)
		fixture=world_move_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_PointContents SV_TruePointContents SV_TestEntityPosition \
		SV_ClipMoveToEntity SV_ClipToLinks SV_MoveBounds SV_Move \
		SV_TestPlayerPosition move_type; do
		printf -- '-D%s=%s%s ' "$s" "$1" "$s"
	done
}
WARN=(-Wall -Wno-unused-function)
LIMIT=${HARNESS_TIMEOUT:-600}

build() { # name ieee-cflags...
	local name=$1; shift
	mkdir -p "$OUT/$name"
	# shellcheck disable=SC2046  # rename's words are meant to split
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" "$@" $(rename c_) \
		-DWT_FIXTURE="\"$here/$fixture\"" \
		-c "$here/c_ref.c" -o "$OUT/$name/c_ref.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/diff_harness.c" -o "$OUT/$name/diff_harness.o"
	cc "$@" -o "$OUT/$name/diff_harness" "$OUT/$name/diff_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/world_target.o" \
		"$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$1" && timeout -k 10 "$LIMIT" ./diff_harness) || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s)" >&2
	fi
	return "$status"
}

echo "== $VARIANT: the C original against the Rust archive =="
build opt -O2 -g
run "$OUT/opt"

echo "== $VARIANT: the same run with C line coverage =="
build cov -O0 -g --coverage
run "$OUT/cov" >/dev/null
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }

# One line is exempt, by position, because no input reaches it: the body of
# SV_ClipToLinks's `else if (trace.startsolid)`.  The `if` just before it
# takes every trace with allsolid, startsolid or a nearer fraction, so a trace
# reaching the `else` has startsolid clear.  The line is found from its own
# text inside that else-if, which must occur exactly once.
anchor_if='else if (trace.startsolid)'
mapfile -t at_if < <(grep -nF -- "$anchor_if" "$here/$fixture" | cut -d: -f1)
if [ "${#at_if[@]}" -ne 1 ]; then
	echo "FAIL: the exemption anchor occurs ${#at_if[@]} times in $fixture; expected 1" >&2
	exit 1
fi
at=("$((at_if[0] + 1))")
[ "$(sed -n "${at[0]}p" "$here/$fixture" | tr -d '\t ')" = 'clip->trace.startsolid=true;' ] || {
	echo "FAIL: the line after the anchor in $fixture is not the dead startsolid store" >&2
	exit 1
}
grep -B1 -F -- "$anchor_if" "$here/$fixture" | head -1 | grep -qF 'trace.allsolid || trace.startsolid' || \
	grep -B12 -F -- "$anchor_if" "$here/$fixture" | grep -qF 'if (trace.allsolid || trace.startsolid || trace.fraction < clip->trace.fraction)' || {
	echo "FAIL: the test before the anchor in $fixture no longer takes startsolid; the proof no longer holds" >&2
	exit 1
}
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v a="${at[0]}" '{ if ($2 + 0 != a) print }' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines), except the unreachable startsolid store"
echo "RESULT: PASS"
