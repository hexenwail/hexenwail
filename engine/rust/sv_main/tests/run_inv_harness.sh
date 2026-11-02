#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main extended-inventory/savegame differential harness and run
# it (#469 slice E, unit E1).
#
#   VARIANT=h2  sv_main_inv_h2.c (hexen2/sv_main.c's), -DSERVERONLY as h2ded
#               compiles it, against an archive built with sv_main_inv.
#               Hexen II only: HexenWorld's server has its own C
#               SV_GetLevelname and SV_SaveSpawnparms and none of the rest.
#
# RUST_LIB names the archive.  The binary also compiles the real
# engine/rust/world_target.c and engine/rust/sv_main_target.c with Hexen II's
# headers, the engine's own COM_Parse (extracted from
# engine/h2shared/common.c, below) and common/strlcpy.c's q_strlcpy, each
# renamed so the harness can log the calls around it.  sv_main_target.c is
# compiled sectioned and linked with --gc-sections, as every sv_main runner
# is: it carries every slice's accessors, and this harness defines only the
# globals its own slice's accessors read.
#
# The C original is compiled with -ftrivial-auto-var-init=zero: where
# SV_LoadInventory reads an int no fscanf assigned, the C's value is
# unspecified (its address was taken, so not undefined) and the port's is 0;
# this makes the C's 0 too, so those inputs are compared instead of skipped
# (see sv_main_inv.rs).  For input where every int is assigned before it is
# read, the flag changes nothing.  The run is
# repeated with --coverage, where every line of the original must run.  Both
# runs are under a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_inv}"
OUT="${OUT:-$(mktemp -d)}"
CASES="${HARNESS_CASES:-}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
CDEFS=(-DSERVERONLY)
fixture=sv_main_inv_h2.c

rename() { # prefix
	local s
	for s in SV_ParseWorldspawnSky SV_ClientInventoryPage INV_UpdateExItem \
		SV_GetLevelname SV_SaveSpawnparms INV_WritePage SV_LoadInventory \
		INV_SavePages; do
		printf -- '-D%s=%s%s ' "$s" "$1" "$s"
	done
}
# -Wno-comment: quakefs.h:175's "gfx/env/*_rt" inside a comment.
WARN=(-Wall -Wno-unused-function -Wno-comment)
LIMIT=${HARNESS_TIMEOUT:-900}

# COM_Parse and com_token, verbatim from common.c: from the com_token
# definition to the end of the function after it.  The anchors are checked,
# so a move in common.c fails here rather than testing something else.
com_parse="$OUT/com_parse.c"
{
	echo '#include "quakedef.h"'
	awk '/^char[ \t]+com_token\[1024\];$/ {p = 1} p {print} p && /^}$/ {exit}' \
		"$engine/h2shared/common.c"
} >"$com_parse"
grep -q '^const char \*COM_Parse (const char \*data)$' "$com_parse" &&
	[ "$(grep -c '^}$' "$com_parse")" -eq 1 ] || {
	echo "FAIL: could not extract COM_Parse from engine/h2shared/common.c" >&2
	exit 1
}

build() { # name cflags...
	local name=$1; shift
	mkdir -p "$OUT/$name"
	# shellcheck disable=SC2046  # rename's words are meant to split
	# -Wno-unused-variable: the verbatim INV_UpdateExItem declares an unused j
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -Wno-unused-variable "$@" $(rename c_) \
		-ftrivial-auto-var-init=zero \
		-DSV_FIXTURE="\"$here/$fixture\"" \
		-c "$here/c_ref_inv.c" -o "$OUT/$name/c_ref.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g -DCOM_Parse=real_COM_Parse \
		-c "$com_parse" -o "$OUT/$name/com_parse.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g -Dq_strlcpy=real_q_strlcpy \
		-c "$root/common/strlcpy.c" -o "$OUT/$name/strlcpy.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-ffunction-sections -fdata-sections -c "$rust_root/sv_main_target.c" -o "$OUT/$name/sv_main_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/inv_harness.c" -o "$OUT/$name/inv_harness.o"
	cc "$@" -o "$OUT/$name/inv_harness" "$OUT/$name/inv_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/com_parse.o" "$OUT/$name/strlcpy.o" \
		"$OUT/$name/world_target.o" "$OUT/$name/sv_main_target.o" \
		"$RUST_LIB" -lm -Wl,--gc-sections
}

run() { # dir
	local status=0
	# shellcheck disable=SC2086  # CASES is one word or none
	(cd "$1" && timeout -k 10 "$LIMIT" ./inv_harness $CASES) || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s)" >&2
	fi
	return "$status"
}

echo "== $VARIANT: the C original against the Rust archive =="
build opt -O2 -g
run "$OUT/opt" | tee "$OUT/opt.log"
[ "${PIPESTATUS[0]}" -eq 0 ] || exit 1

# Each way a call can end must have been compared, not merely be possible:
# the Host_Error of a bad page count, the undefined-behaviour stop of an
# overlong or EOF-cut item line (the C's stop, the port's Host_Error),
# and INV_UpdateExItem's spin.
floor() { # kind column minimum
	local n
	n=$(awk -v k="$1" -v c="$2" '$1 == k && NF == 6 {print $c}' "$OUT/opt.log")
	[ -n "$n" ] && [ "$n" -ge "$3" ] || {
		echo "FAIL: outcome floor: $1 column $2 is ${n:-missing}, need >= $3" >&2
		exit 1
	}
}
for kind in sky page update levelname spawnparms writepage load savepages roundtrip; do
	floor "$kind" 2 100
done
floor load 3 100	# Host_Error: a bad page count
floor load 4 100	# the C overruns item_id[], the port refuses
floor load 5 100	# EOF inside a line: the C loops, the port refuses
floor update 6 3	# INV_UpdateExItem's spin, on both sides

[ -n "${HARNESS_SKIP_COVERAGE:-}" ] && { echo "RESULT: PASS (coverage skipped)"; exit 0; }

echo "== $VARIANT: the same run with C line coverage =="
build cov -O0 -g --coverage
# The children use exit(), not _exit(), so libgcov's exit handler dumps
# and merges the per-process coverage automatically.
# shellcheck disable=SC2086  # CASES is one word or none
(cd "$OUT/cov" && ./inv_harness $CASES >/dev/null 2>&1) || true
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines)"

echo "RESULT: PASS"
