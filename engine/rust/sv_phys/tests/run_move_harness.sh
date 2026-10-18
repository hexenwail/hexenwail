#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_phys movetype-dispatch differential harness for both C originals and run it.
#
#   VARIANT=h2  sv_phys_move_h2.c (hexen2/sv_phys.c's movetype dispatch), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_phys_move;
#   VARIANT=hw  sv_phys_move_hw.c (hexenworld/server/sv_phys.c's), -DH2W
#               -DSERVERONLY as hwsv, against sv_phys_move + world_trace_hw.
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
		fixture=sv_phys_move_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DSV_HW)
		fixture=sv_phys_move_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_Physics_None SV_Physics_Noclip SV_CheckWaterTransition \
		SV_Physics_Toss SV_Physics_Step SV_Physics SV_ProgStartFrame \
		SV_RunEntity SV_RunNewmis; do
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
		-c "$here/c_ref_move.c" -o "$OUT/$name/c_ref.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/phys_target.c" -o "$OUT/$name/phys_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/move_harness.c" -o "$OUT/$name/diff_harness.o"
	cc "$@" -o "$OUT/$name/diff_harness" "$OUT/$name/diff_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/world_target.o" "$OUT/$name/phys_target.o" \
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

# One line of Hexen II's original is exempt, by position, and it is NOT
# unreachable, only out of reach: the wrap of SV_Physics' walk counter,
# `walks = 1;', which runs once 2^31 frames have passed (about a year at
# 72 Hz).  The counter is a function-local static, which nothing outside
# SV_Physics can set, so no harness reaches it short of running that many
# frames.  The Rust's wrap (sv_phys_move.rs) is the same constant, read
# against this text.
#
# The block is pinned by sha256: an edit to it fails here until someone
# re-reads this and re-pins.
exempt=()
if [ "$VARIANT" = h2 ]; then
	mapfile -t from < <(grep -nF -- 'static unsigned int	walks;' "$here/$fixture" | cut -d: -f1)
	mapfile -t to < <(grep -nF -- 'sv_physics_walk = (int) walks;' "$here/$fixture" | cut -d: -f1)
	if [ "${#from[@]}" -ne 1 ] || [ "${#to[@]}" -ne 1 ]; then
		echo "FAIL: $fixture no longer has the walk counter block this exemption is about" >&2
		exit 1
	fi
	span_sha=$(sed -n "${from[0]},${to[0]}p" "$here/$fixture" | sha256sum | cut -d' ' -f1)
	if [ "$span_sha" != a27acead213066540fd52b0358f51522f2b8de34e4beb4e9d8fa5f785468f5c1 ]; then
		echo "FAIL: the walk counter block in $fixture changed (sha256 $span_sha); re-read the exemption and re-pin" >&2
		exit 1
	fi
	exempt=("$((to[0] - 1))")
	[ "$(sed -n "${exempt[0]}p" "$here/$fixture" | tr -d '\t ' | cut -c1-10)" = 'walks=1;/*' ] || {
		echo "FAIL: the exempt line of $fixture is not the walk counter's wrap" >&2
		exit 1
	}
fi
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v list=" ${exempt[*]} " '{ n = $2 + 0; if (index(list, " " n " ") == 0) print }' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
for e in "${exempt[@]}"; do
	grep -qE "^ *#####: *$e:" "$gcov_file" || {
		echo "FAIL: the exempt line $e of $fixture ran; the proof is wrong, drop the exemption" >&2
		exit 1
	}
done
if [ "${#exempt[@]}" -gt 0 ]; then
	echo "coverage: every executable line of $fixture ran ($lines), except the walk counter's wrap at 2^31 frames"
else
	echo "coverage: every executable line of $fixture ran ($lines)"
fi
echo "RESULT: PASS"
