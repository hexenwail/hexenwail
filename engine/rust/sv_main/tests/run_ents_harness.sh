#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main entity-update differential harness and run it.
#
#   VARIANT=h2  sv_main_ents_h2.c (hexen2/sv_main.c's), -DSERVERONLY as
#               h2ded compiles it, against an archive built with
#               sv_main_ents.  Hexen II only: HexenWorld's entity updates are
#               its sv_ents.c (#469 slice G); this is slice B.
#
# RUST_LIB names the archive.  The binary also compiles the real
# engine/rust/world_target.c and engine/rust/sv_main_target.c with Hexen II's
# headers.  The C original goes in once, as plain IEEE C (see world_trace.rs
# on the engine's -ffast-math build), and the run is repeated with
# --coverage, where every line of the original must run.  Both runs are
# under a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_ents}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
CDEFS=(-DSERVERONLY)
fixture=sv_main_ents_h2.c

rename() { # prefix
	local s
	for s in SV_AddToFatPVS SV_FatPVS SV_EntityUpdateSize SV_EntityPriority \
		SV_PrepareClientEntities SV_CleanupEnts fatbytes fatpvs; do
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
		-c "$here/c_ref_ents.c" -o "$OUT/$name/c_ref_ents.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/sv_main_target.c" -o "$OUT/$name/sv_main_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/ents_harness.c" -o "$OUT/$name/ents_harness.o"
	cc "$@" -o "$OUT/$name/ents_harness" "$OUT/$name/ents_harness.o" \
		"$OUT/$name/c_ref_ents.o" "$OUT/$name/world_target.o" \
		"$OUT/$name/sv_main_target.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$1" && timeout -k 10 "$LIMIT" ./ents_harness) || status=$?
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
(cd "$OUT/cov" && gcov -o . c_ref_ents.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
# One line no input reaches, exempt by position: the clamp
# `NumToRemove = MAX_CLEAR_EDICTS_PER_MSG;' after the entity loop.  From
# `restart:', which sets NumToRemove to 0, to the clamp, the only write to it
# is the `NumToRemove++' inside `if (NumToRemove < MAX_CLEAR_EDICTS_PER_MSG)',
# so it never exceeds 255 and the clamp's test is never true.  The C's own
# comment calls the clamp a guard "so the two writes can never disagree".
#
# The proof is about exact text, so the text is pinned: the span from
# `restart:' to the clamp, hashed.  An edit anywhere in it fails here until
# someone re-reads the proof and re-pins the hash.
anchor='		NumToRemove = MAX_CLEAR_EDICTS_PER_MSG;'
mapfile -t at < <(grep -nxF -- "$anchor" "$here/$fixture" | cut -d: -f1)
mapfile -t start_at < <(grep -nxF -- 'restart:' "$here/$fixture" | cut -d: -f1)
if [ "${#at[@]}" -ne 1 ] || [ "${#start_at[@]}" -ne 1 ]; then
	echo "FAIL: the exemption anchors occur ${#at[@]}/${#start_at[@]} times in $fixture; expected 1/1" >&2
	exit 1
fi
span_sha=$(sed -n "${start_at[0]},${at[0]}p" "$here/$fixture" | sha256sum | cut -d' ' -f1)
if [ "$span_sha" != 89e27561e6410fbdf2af58fb497072178e5165d67d7afe400d8d9ced2619865d ]; then
	echo "FAIL: the text between $fixture's restart: and the NumToRemove clamp changed (sha256 $span_sha); re-check the proof above and re-pin" >&2
	exit 1
fi
exempt=${at[0]}
grep -qE "^ *#####: *${exempt}:" "$gcov_file" || {
	echo "FAIL: the exempt line $exempt of $fixture ran; the proof is wrong, drop the exemption" >&2
	exit 1
}
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: -v a="$exempt" '{ if ($2 + 0 != a) print }' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines), except the unreachable NumToRemove clamp"
echo "RESULT: PASS"
