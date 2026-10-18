#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_phys differential harness for both C originals and run it.
#
#   VARIANT=h2  sv_phys_base_h2.c (hexen2/sv_phys.c's base helpers), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_phys;
#   VARIANT=hw  sv_phys_base_hw.c (hexenworld/server/sv_phys.c's), -DH2W
#               -DSERVERONLY as hwsv, against sv_phys + world_trace_hw.
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
		fixture=sv_phys_base_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DSV_HW)
		fixture=sv_phys_base_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_CheckVelocity SV_RunThink SV_Impact ClipVelocity \
		SV_FlyMove SV_FlyExtras SV_AddGravity SV_PushEntity hoverinc; do
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
		-c "$rust_root/phys_target.c" -o "$OUT/$name/phys_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/diff_harness.c" -o "$OUT/$name/diff_harness.o"
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

# Two lines are exempt, by position, because no input reaches them: the body
# of SV_FlyMove's `if (numplanes >= MAX_CLIP_PLANES)' ("this shouldn't
# really happen").  numbumps is 4, so the loop body runs at most four times;
# numplanes starts at 0, is only ever reset to 0 or incremented once per
# pass (the one `numplanes++', after the guard), so at the guard it is at
# most 3, and MAX_CLIP_PLANES is 5.
#
# The proof is about exact text, so the text is pinned: the span from
# `numbumps = 4;' through the guard's body, hashed per fixture, plus the
# facts outside it -- MAX_CLIP_PLANES is 5, and numplanes is written three
# times in all: two resets to 0 and one `numplanes++', after the guard.  An edit fails here until someone re-reads the proof and
# re-pins.  The fixtures are verbatim moves, so this is not expected to
# change.
case "$VARIANT" in
	h2) want_sha=217e999b2aab77d4a9952864167e23befc0f589502446ed8d310d06e643de7dc ;;
	hw) want_sha=f1b3a1898cc2fe7b516542e2e442a3090e396b13e7ebd716f28b29e412713da6 ;;
esac
mapfile -t from < <(grep -nF -- 'numbumps = 4;' "$here/$fixture" | cut -d: -f1)
mapfile -t guard < <(grep -nF -- 'if (numplanes >= MAX_CLIP_PLANES)' "$here/$fixture" | cut -d: -f1)
mapfile -t inc < <(grep -nF -- 'numplanes++' "$here/$fixture" | cut -d: -f1)
# every write to numplanes in the fixture: the two resets and the increment
mapfile -t writes < <(grep -nE 'numplanes[[:space:]]*(\+\+|--|[-+*/|&^]?=[^=])' "$here/$fixture" | cut -d: -f1)
resets=$(grep -cE 'numplanes[[:space:]]*=[[:space:]]*0;' "$here/$fixture" || true)
if [ "${#from[@]}" -ne 1 ] || [ "${#guard[@]}" -ne 1 ] || [ "${#inc[@]}" -ne 1 ] ||
   [ "${#writes[@]}" -ne 3 ] || [ "$resets" -ne 2 ] ||
   [ "${inc[0]}" -le "$((guard[0] + 4))" ] ||
   ! grep -qE '^#define[[:space:]]+MAX_CLIP_PLANES[[:space:]]+5$' "$here/$fixture"; then
	echo "FAIL: $fixture no longer has the shape the MAX_CLIP_PLANES proof rests on" >&2
	exit 1
fi
span_sha=$(sed -n "${from[0]},$((guard[0] + 4))p" "$here/$fixture" | sha256sum | cut -d' ' -f1)
if [ "$span_sha" != "$want_sha" ]; then
	echo "FAIL: SV_FlyMove's text up to the MAX_CLIP_PLANES guard changed in $fixture (sha256 $span_sha); re-check the proof above and re-pin" >&2
	exit 1
fi
exempt=("$((guard[0] + 2))" "$((guard[0] + 3))")
if [ "$(sed -n "${exempt[0]}p" "$here/$fixture" | tr -d '\t ')" != 'VectorClear(ent->v.velocity);' ] ||
   [ "$(sed -n "${exempt[1]}p" "$here/$fixture" | tr -d '\t ')" != 'return3;' ]; then
	echo "FAIL: the exempt lines of $fixture are not the guard's VectorClear and return 3" >&2
	exit 1
fi
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v a="${exempt[0]}" -v b="${exempt[1]}" '{ n = $2 + 0; if (n != a && n != b) print }' || true)
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
echo "coverage: every executable line of $fixture ran ($lines), except the unreachable MAX_CLIP_PLANES guard (2 lines)"
echo "RESULT: PASS"
