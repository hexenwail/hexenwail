#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_phys client-movement differential harness for both C originals and run it.
#
#   VARIANT=h2  sv_phys_walk_h2.c (hexen2/sv_phys.c's client movement), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_phys_walk;
#   VARIANT=hw  sv_phys_walk_hw.c (hexenworld/server/sv_phys.c's), -DH2W
#               -DSERVERONLY as hwsv, against sv_phys_walk + world_trace_hw.
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
		fixture=sv_phys_walk_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DSV_HW)
		fixture=sv_phys_walk_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_CheckStuck SV_CheckWater SV_WallFriction SV_TryUnstick \
		SV_WalkMove SV_Physics_Client; do
		printf -- '-D%s=%s%s ' "$s" "$1" "$s"
	done
}
WARN=(-Wall -Wno-unused-function)
LIMIT=${HARNESS_TIMEOUT:-600}

build() { # name ieee-cflags...
	local name=$1; shift
	mkdir -p "$OUT/$name"
	# shellcheck disable=SC2046  # rename's words are meant to split
	# -ftrivial-auto-var-init=zero: SV_WalkMove's C reads its steptrace
	# uninitialised when neither SV_FlyMove stored a wall trace yet the clip
	# has bit 2 (both returned 3 or 7, or SV_TryUnstick's clip carries it);
	# the port reads zeros there (see sv_phys_walk.rs), and this gives the C
	# the same, so the comparison is defined.  Every other local in the
	# fixture is written before it is read, so it hides nothing else.
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" "$@" -ftrivial-auto-var-init=zero $(rename c_) \
		-DSV_FIXTURE="\"$here/$fixture\"" \
		-c "$here/c_ref.c" -o "$OUT/$name/c_ref.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/phys_target.c" -o "$OUT/$name/phys_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/walk_harness.c" -o "$OUT/$name/diff_harness.o"
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

exempt=()
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
echo "coverage: every executable line of $fixture ran ($lines)${exempt:+, except ${#exempt[@]} proven unreachable}"
echo "RESULT: PASS"
