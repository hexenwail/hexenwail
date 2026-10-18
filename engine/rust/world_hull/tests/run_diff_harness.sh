#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the hull-selection differential harness for both C originals and run it.
#
#   VARIANT=h2  world_hull_h2.c (hexen2/world.c's), -DSERVERONLY as h2ded
#               compiles it, against an archive built with world_hull;
#   VARIANT=hw  world_hull_hw.c (hexenworld/server/world.c's), -DH2W
#               -DSERVERONLY as hwsv, against world_hull + world_trace_hw.
#
# RUST_LIB names the archive for that variant.  The binary also compiles the
# real engine/rust/world_target.c with the variant's headers: it is how the
# Rust learns this target's layouts, so it is under test here as well.  The
# C original goes in twice -- plain IEEE C (c_*), the contract, and with the
# engine's flags (f_*), counted only -- as in world_trace's harness.  The run
# is repeated with --coverage on the IEEE build, where every line of the
# original must run but the two proven unreachable below.  Both runs are
# under a timeout.
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
		fixture=world_hull_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DWT_HW)
		fixture=world_hull_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	echo "-DSV_InitBoxHull=$1SV_InitBoxHull" \
	     "-DSV_HullForBox=$1SV_HullForBox" \
	     "-DSV_HullForEntity=$1SV_HullForEntity" \
	     "-Dbox_hull=$1box_hull" \
	     "-Dbox_clipnodes=$1box_clipnodes" \
	     "-Dbox_planes=$1box_planes"
}
ENGINE_CFLAGS=(-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer)
WARN=(-Wall -Wno-unused-function)
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
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/diff_harness.c" -o "$OUT/$name/diff_harness.o"
	cc "$@" -o "$OUT/$name/diff_harness" "$OUT/$name/diff_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/c_ref_engine.o" "$OUT/$name/world_target.o" \
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

echo "== $VARIANT: the C original (IEEE and engine flags) against the Rust archive =="
build opt -O2 -g
run "$OUT/opt"

echo "== $VARIANT: the same run with C line coverage =="
build cov -O0 -g --coverage
run "$OUT/cov" >/dev/null
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }

# Two lines are exempt, by position, because no input reaches them: the body
# of SV_HullForEntity's `if (!hull)`, the Con_Printf and the hulls[0]
# assignment after it.  hull is &model->hulls[idx] with model already checked
# non-null, and the address of an array element is never null -- C does not
# let pointer arithmetic produce one, and GCC deletes the branch at -O2.  They
# are found from the Con_Printf's text, which must occur exactly once, so the
# exemption cannot drift onto other lines; the hulls[0] line after it is the
# one other `hull = &model->hulls[0];` in the function, the point-hull rule,
# which must run and is not exempt.
anchor='Con_Printf ("ERROR: hull %d is null.\n", idx);'
mapfile -t at < <(grep -nF -- "$anchor" "$here/$fixture" | cut -d: -f1)
if [ "${#at[@]}" -ne 1 ]; then
	echo "FAIL: the exemption anchor occurs ${#at[@]} times in $fixture; expected 1" >&2
	exit 1
fi
dead=("${at[0]}" "$((at[0] + 1))")
[ "$(sed -n "${dead[1]}p" "$here/$fixture" | tr -d '\t ')" = 'hull=&model->hulls[0];' ] || {
	echo "FAIL: the line after the anchor in $fixture is not the null branch's hulls[0]" >&2
	exit 1
}
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v a="${dead[0]}" -v b="${dead[1]}" \
	'{ n = $2 + 0; if (n != a && n != b) print }' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines), except the 2 of the unreachable null-hull branch"
echo "RESULT: PASS"
