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

# The harness forks twice per case, and fork() gets slower the longer one
# process keeps forking: measured here, 500 cases take 3s, 2000 take 42s and
# the 12000-case enumeration in one process runs past the 900s bound (on
# smaller machines fork() fails with ENOMEM outright).  So the enumeration is
# tiled over processes of CHUNK cases each, with disjoint seeds (the harness's
# HARNESS_CASE_OFFSET / HARNESS_KIND), plus one process for the directed
# edges.  The union is exactly the single-process enumeration; counts are
# summed into one PASS line, and the coverage run's .gcda files merge.
TOTAL=12000
CHUNK=500
# loop kind (the harness's own numbering), its share of TOTAL
KINDS=("2 $((TOTAL * 2 / 3))" "1 $((TOTAL / 6))" "0 $((TOTAL / 6))")

run_chunk() { # dir, then env assignments and the harness's target argument
	local dir=$1 status=0; shift
	(cd "$dir" && env "$@" timeout -k 10 "$LIMIT" ./datagram_harness "$ARG") || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s)" >&2
	fi
	return "$status"
}

run() { # dir
	local dir=$1 cases=0 pass=0 kind share off line n m
	local log="$dir/chunk.log" layouts=""
	# the loops derive their counts from the argument: kind 2 runs 2/3 of it,
	# kinds 1 and 0 one sixth, so each chunk asks for the argument that
	# yields exactly CHUNK (or the remainder) cases of its kind
	local -a todo=()
	for kk in "${KINDS[@]}"; do
		read -r kind share <<<"$kk"
		for ((off = 0; off < share; off += CHUNK)); do
			n=$((share - off < CHUNK ? share - off : CHUNK))
			case $kind in 2) ARG=$((n * 3 / 2 + 1)) ;; *) ARG=$((n * 6)) ;; esac
			todo+=("$kind $off $ARG")
		done
	done
	todo+=("9 0 0")	# no loop runs: the directed edges alone
	for t in "${todo[@]}"; do
		read -r kind off ARG <<<"$t"
		local -a env_=(HARNESS_CASE_OFFSET="$off" HARNESS_KIND="$kind")
		[ "$kind" = 9 ] || env_+=(HARNESS_SKIP_DIRECTED=1)
		if ! run_chunk "$dir" "${env_[@]}" >"$log" 2>&1; then
			cat "$log"
			echo "FAIL: chunk kind=$kind offset=$off" >&2
			return 1
		fi
		line=$(grep -E '^sv_main_datagram differential harness \(h2\): PASS \(' "$log" || true)
		n=$(printf '%s\n' "$line" | sed -n 's/.*PASS (\([0-9]*\) cases, \([0-9]*\) pass).*/\1/p')
		m=$(printf '%s\n' "$line" | sed -n 's/.*PASS (\([0-9]*\) cases, \([0-9]*\) pass).*/\2/p')
		if [ -z "$n" ] || [ "$n" != "$m" ]; then
			cat "$log"
			echo "FAIL: chunk kind=$kind offset=$off did not report PASS" >&2
			return 1
		fi
		cases=$((cases + n)); pass=$((pass + m))
		[ -n "$layouts" ] || layouts=$(grep -E '^layouts:' "$log" || true)
	done
	[ -z "$layouts" ] || echo "$layouts"
	echo "sv_main_datagram differential harness (h2): PASS ($cases cases, $pass pass)"
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
#    sites in SV_WriteClientdataToMessage, all with a literal false
#    (verified by inspection; if the fixture gains a call site, re-check).
# 2. SV_ClientInventoryPage's out-of-pages `return NULL': the backing is 32
#    pages and the slice's ids are 1..15, so one page holds every id the
#    slice ever creates; allocation from the backing cannot exhaust it.
#    (The harness's own randomized pages are not the backing.)
# 3. SV_ModelIndex's trailing `return i;': dead code.  The for loop exits
#    iff i == MAX_MODELS or !sv.model_precache[i], which is exactly the
#    `if' guarding the `return 0' above it, so the trailing return is
#    unreachable.
#
# The pins are line numbers in $fixture; the anchor patterns are chosen to be
# unique (indentation-sensitive), so an edit that moves them fails the count
# check below until someone re-reads the proof and re-pins.
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
# SV_ModelIndex's dead trailing `return i;': function scope, a single tab;
# the found-match `return i' inside the loop is indented one more and runs.
mapfile -t at < <(grep -n $'^\treturn i;$' "$here/$fixture" | cut -d: -f1)
for ln in "${at[@]}"; do
	exempt_lines="$exempt_lines $ln"
done
# The += line occurs twice (found-slot and new-slot arms); each return once.
[ "$(echo "$exempt_lines" | wc -w)" -eq 4 ] || {
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
echo "coverage: every executable line of $fixture ran ($lines), except the 4 pinned exemptions"
echo "RESULT: PASS"
