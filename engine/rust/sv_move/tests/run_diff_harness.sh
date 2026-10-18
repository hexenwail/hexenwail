#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_move differential harness for both C originals and run it.
#
#   VARIANT=h2  sv_move_h2.c (hexen2/sv_move.c's), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_move;
#   VARIANT=hw  sv_move_hw.c (hexenworld/server/sv_move.c's), -DH2W
#               -DSERVERONLY as hwsv, against sv_move + world_trace_hw.
#
# RUST_LIB names the archive for that variant.  The binary also compiles the
# real engine/rust/world_target.c with the variant's headers.  The C original
# goes in once, as plain IEEE C (see world_trace.rs on the engine's
# -ffast-math build), and the run is repeated with --coverage, where every
# line of the original must run, but the lines proven unreachable below.
# Both runs are under a timeout.
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
		fixture=sv_move_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DSV_HW)
		fixture=sv_move_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_CheckBottom set_move_trace SV_movestep SV_StepDirection \
		SV_FixCheckBottom SV_NewChaseDir SV_CloseEnough SV_MoveToGoal; do
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
		-DSV_FIXTURE="\"$here/$fixture\"" \
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

# Hexen II's original has one line no input reaches, exempt by position: the
# FL_HUNTFACE `dz +=' in SV_movestep's flier branch.  The branch is entered
# only when `!((int)ent->v.flags & FL_HUNTFACE)'; nothing between that test
# and the line writes the flags (a VectorAdd into neworg and pointer reads);
# `(int)' of the same stored float gives the same int both times, and for
# NaN or out-of-range flags x86 gives INT_MIN, which has bit 16 clear.  The
# C's own comment calls the line "utterly broken".
#
# The proof is about exact text, so the text is pinned: the span from the
# flier test's first line to the exempt line, hashed.  An edit anywhere in it
# -- a flags store, a `||', different nesting -- fails here until someone
# re-reads the proof and re-pins the hash.  The fixture is a verbatim move of
# engine/hexen2/sv_move.c, so this span is not expected to change.
exempt=()
if [ "$VARIANT" = h2 ]; then
	anchor='dz += PROG_TO_EDICT(ent->v.enemy)->v.view_ofs[2];'
	mapfile -t at < <(grep -nF -- "$anchor" "$here/$fixture" | cut -d: -f1)
	mapfile -t test_at < <(grep -nF -- 'if ( ((int)ent->v.flags & (FL_SWIM|FL_FLY))' "$here/$fixture" | cut -d: -f1)
	if [ "${#at[@]}" -ne 1 ] || [ "${#test_at[@]}" -ne 1 ]; then
		echo "FAIL: the exemption anchors occur ${#at[@]}/${#test_at[@]} times in $fixture; expected 1/1" >&2
		exit 1
	fi
	span_sha=$(sed -n "${test_at[0]},${at[0]}p" "$here/$fixture" | sha256sum | cut -d' ' -f1)
	if [ "$span_sha" != d1bb66cf22cd74a7700d4ab6a769133f6fcb8c7dbe51045027166540cc72d80f ]; then
		echo "FAIL: the text between $fixture's flier test and the exempt FL_HUNTFACE line changed (sha256 $span_sha); re-check the proof above and re-pin" >&2
		exit 1
	fi
	exempt=("${at[0]}")
fi
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v a="${exempt[0]:-0}" '{ if ($2 + 0 != a) print }' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
if [ "${#exempt[@]}" -gt 0 ]; then
	grep -qE "^ *#####: *${exempt[0]}:" "$gcov_file" || {
		echo "FAIL: the exempt line ${exempt[0]} of $fixture ran; the proof is wrong, drop the exemption" >&2
		exit 1
	}
	echo "coverage: every executable line of $fixture ran ($lines), except the unreachable FL_HUNTFACE dz line"
else
	echo "coverage: every executable line of $fixture ran ($lines)"
fi
echo "RESULT: PASS"
