#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_phys pusher differential harness for both C originals and run it.
#
#   VARIANT=h2  sv_phys_push_h2.c (hexen2/sv_phys.c's pushers), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_phys_push;
#   VARIANT=hw  sv_phys_push_hw.c (hexenworld/server/sv_phys.c's), -DH2W
#               -DSERVERONLY as hwsv, against sv_phys_push + world_trace_hw.
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
		fixture=sv_phys_push_h2.c ;;
	hw)
		INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
		      -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
		CDEFS=(-DH2W -DSERVERONLY -DSV_HW)
		fixture=sv_phys_push_hw.c ;;
	*) echo "error: VARIANT must be h2 or hw" >&2; exit 2 ;;
esac

rename() { # prefix
	local s
	for s in SV_Push SV_PushMove SV_PushRotate SV_Physics_Pusher; do
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
		-c "$here/push_harness.c" -o "$OUT/$name/diff_harness.o"
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

# Lines exempt because no input reaches them: the bodies of the live
# SV_PushRotate's three compiled `for (i = 0; i < slaves_moved; i++)' loops
# (a fourth sits in its `#   if 0' debug dump, which gcov does not count).  The block
# that would fill the slave list is commented out, so slaves_moved is set to
# 0 and never written again: the loops run no times.
#
# Checked, not assumed, on every run:
#   - the live SV_PushRotate (the one headed "NEW"; the other is #if 0) is
#     pinned by sha256, so any edit to it fails here until the proof is
#     re-read and re-pinned;
#   - in that function with its comments stripped (cc -fpreprocessed), the
#     only write to slaves_moved is `slaves_moved = 0;';
#   - every exempt line lies inside one of those three loops' bodies, found
#     by brace matching from the `for' line, and is a line gcov never ran.
# The fixtures are verbatim moves, so the pin is not expected to change.
case "$VARIANT" in
	h2) want_sha=a27db8319db7ef517a0b6ce19c6f6eacb90f5f313490d20f27b6882b4fb42d18 ;;
	hw) want_sha=4b65c4ca02688f16abe9029732ac60f66f3567764a62cf4810fe8f44b9e26a7d ;;
esac
fn_from=$(awk '/^SV_PushRotate$/ { getline n; if (n == "NEW") { print NR; exit } }' "$here/$fixture")
[ -n "$fn_from" ] || { echo "FAIL: cannot find the live SV_PushRotate in $fixture" >&2; exit 1; }
fn_to=$(awk -v a="$fn_from" 'NR > a && /^}$/ { print NR; exit }' "$here/$fixture")
span_sha=$(sed -n "${fn_from},${fn_to}p" "$here/$fixture" | sha256sum | cut -d' ' -f1)
if [ "$span_sha" != "$want_sha" ]; then
	echo "FAIL: the live SV_PushRotate in $fixture changed (sha256 $span_sha); re-check the slaves_moved proof and re-pin" >&2
	exit 1
fi
writes=$(sed -n "${fn_from},${fn_to}p" "$here/$fixture" | cc -fpreprocessed -E -P -x c - 2>/dev/null |
	grep -E 'slaves_moved[[:space:]]*(\+\+|--|[-+*/|&^]?=[^=])' | tr -d '\t ' || true)
if [ "$writes" != 'slaves_moved=0;' ]; then
	echo "FAIL: slaves_moved is written other than once to 0 in $fixture's SV_PushRotate: $writes" >&2
	exit 1
fi
# the lines inside the three loops' bodies
mapfile -t loop_lines < <(awk -v a="$fn_from" -v b="$fn_to" '
	NR < a || NR > b { next }
	inloop { depth += gsub(/\{/, "{"); depth -= gsub(/\}/, "}"); if (NR > start) print NR; if (depth <= 0 && seen) inloop = 0; if (depth > 0) seen = 1; next }
	/for \(i = 0; i < slaves_moved; i\+\+\)/ { inloop = 1; start = NR; depth = 0; seen = 0; nloops++ }
	END { if (nloops != 4) print "LOOPS " nloops }' "$here/$fixture")
if [ "${#loop_lines[@]}" -eq 0 ] || printf '%s\n' "${loop_lines[@]}" | grep -q '^LOOPS'; then
	echo "FAIL: $fixture's SV_PushRotate does not have the four slaves_moved loops (three compiled, one under #if 0) the proof is about" >&2
	exit 1
fi
mapfile -t exempt < <(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: '{ print $2 + 0 }' |
	grep -Fxf <(printf '%s\n' "${loop_lines[@]}") || true)
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
echo "coverage: every executable line of $fixture ran ($lines), except the ${#exempt[@]} in SV_PushRotate's empty slave loops"
echo "RESULT: PASS"
