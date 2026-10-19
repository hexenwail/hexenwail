#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main datagram differential harness and run it.
#
#   VARIANT=h2  sv_main_datagram_h2.c (hexen2/sv_main.c's), -DSERVERONLY as
#               h2ded compiles it, against an archive built with
#               sv_main_datagram.  Hexen II only: HexenWorld's server frame
#               is its own sv_main.c (#469 slices G-I); this is slice C.
#
# RUST_LIB names the archive.  The binary also compiles the real
# engine/rust/world_target.c and engine/rust/sv_main_target.c with Hexen II's
# headers.  The C original goes in once, as plain IEEE C (see world_trace.rs
# on the engine's -ffast-math build), with the three exported functions
# renamed to c_*; the supporting functions the slice calls but does not
# port (SV_ClientInventoryPage, INV_UpdateExItem, SV_ModelIndex, the
# signon-buffer trio) keep their names, so both sides call the same C.
# The run is repeated with --coverage, where every line of the original
# must run except the pinned exemptions.  Both runs are under a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_datagram}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
CDEFS=(-DSERVERONLY)
fixture=sv_main_datagram_h2.c

# Only the three #[no_mangle] functions are renamed: the static ones
# (SV_SendClientDatagram, SV_UpdateToReliableMessages, SV_SendNop) cannot
# clash, and the supporting functions stay shared.  The #define-static
# trick is NOT used: SV_WriteClientdataToMessage's function-local statics
# (next_update, next_count) must keep their storage.
rename() { # prefix
	local s
	for s in SV_ClearDatagram SV_WriteClientdataToMessage SV_SendClientMessages; do
		printf -- '-D%s=%s%s ' "$s" "$1" "$s"
	done
}
WARN=(-Wall -Wno-unused-function)
LIMIT=${HARNESS_TIMEOUT:-900}

build() { # name ieee-cflags...
	local name=$1; shift
	mkdir -p "$OUT/$name"
	# shellcheck disable=SC2046  # rename's words are meant to split
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" "$@" $(rename c_) \
		-c "$here/$fixture" -o "$OUT/$name/${fixture%.c}.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/sv_main_target.c" -o "$OUT/$name/sv_main_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/datagram_harness.c" -o "$OUT/$name/datagram_harness.o"
	# The harness calls __gcov_dump through a weak declaration (libgcov on
	# some toolchains hides the older __gcov_flush name).  A weak reference
	# does not make the linker extract _gcov_dump.o from libgcov.a, so the
	# null check would always fail and no coverage would be written; force
	# the symbol for coverage builds.
	link_extra=()
	case " $* " in *" --coverage "*) link_extra=(-Wl,-u,__gcov_dump) ;; esac
	cc "$@" "${link_extra[@]}" -o "$OUT/$name/datagram_harness" "$OUT/$name/datagram_harness.o" \
		"$OUT/$name/${fixture%.c}.o" "$OUT/$name/world_target.o" \
		"$OUT/$name/sv_main_target.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$1" && timeout -k 10 "$LIMIT" ./datagram_harness) || status=$?
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
(cd "$OUT/cov" && gcov -o . "${fixture%.c}.o" >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
# Exemptions: lines no input reaches, each proven and pinned.
#
# 1. INV_UpdateExItem's `if (inc)' true arms (two sites): the slice calls it
#    only as INV_UpdateExItem(page, id, (int)cnt, false) -- fifteen call
#    sites in SV_WriteClientdataToMessage, all with a literal false.  The
#    proof is about exact text, so the call sites are pinned by hash below.
# 2. SV_ClientInventoryPage's out-of-pages `return NULL': the backing is 32
#    pages and the slice's ids are 1..15, so one page holds every id the
#    slice ever creates; allocation from the backing cannot exhaust it.
#    (The harness's own randomized pages are not the backing.)
#
# The pins are line numbers in $fixture; the text around each is hashed so
# an edit fails here until someone re-reads the proof and re-pins.
exempt_lines=""
for anchor in 'page->item_cnt[i] += inv_cnt;'; do
	mapfile -t at < <(grep -nF -- "$anchor" "$here/$fixture" | cut -d: -f1)
	for ln in "${at[@]}"; do
		exempt_lines="$exempt_lines $ln"
	done
done
# SV_ClientInventoryPage's out-of-pages `return NULL': the one at function
# scope (a single tab); the `!sv.ex_inventory_pages' early return is
# indented one more and does run, so it is not exempt.
mapfile -t at < <(grep -n $'^\treturn NULL;$' "$here/$fixture" | cut -d: -f1)
for ln in "${at[@]}"; do
	exempt_lines="$exempt_lines $ln"
done
# The += line occurs twice (found-slot and new-slot arms); return NULL once.
[ "$(echo "$exempt_lines" | wc -w)" -eq 3 ] || {
	echo "FAIL: exemption anchors moved in $fixture (found:$exempt_lines); re-check the proofs" >&2
	exit 1
}
for ln in $exempt_lines; do
	grep -qE "^ *#####: *${ln}:" "$gcov_file" || {
		echo "FAIL: exempt line $ln of $fixture ran; the proof is wrong, drop the exemption" >&2
		exit 1
	}
done
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" | awk -F: '{ print $2 + 0 }' | while read -r ln; do
	case " $exempt_lines " in *" $ln "*) ;; *) echo "$ln" ;; esac
done || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines), except the 3 pinned exemptions"
echo "RESULT: PASS"
