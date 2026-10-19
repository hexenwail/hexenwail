#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main connect-and-signon differential harness and run it.
#
#   VARIANT=h2  sv_main_connect_h2.c (hexen2/sv_main.c's), -DSERVERONLY as
#               h2ded compiles it, against an archive built with
#               sv_main_connect.  Hexen II only: HexenWorld's server has no
#               counterpart of these functions (#469 slice D).
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
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_connect}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
CDEFS=(-DSERVERONLY)
fixture=sv_main_connect_h2.c

rename() { # prefix
	local s
	for s in SV_ProtocolName SV_SendServerinfo SV_ConnectClient \
		SV_CheckForNewClients SV_ModelIndex SV_SignonBufferCount \
		SV_SignonBufferSize SV_ReserveSignonSpace SV_SendSignonBuffer \
		SV_CreateBaseline SV_SendReconnect; do
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
		-c "$here/c_ref_connect.c" -o "$OUT/$name/c_ref.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/world_target.c" -o "$OUT/$name/world_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$rust_root/sv_main_target.c" -o "$OUT/$name/sv_main_target.o"
	cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
		-c "$here/connect_harness.c" -o "$OUT/$name/connect_harness.o"
	cc "$@" -o "$OUT/$name/connect_harness" "$OUT/$name/connect_harness.o" \
		"$OUT/$name/c_ref.o" "$OUT/$name/world_target.o" \
		"$OUT/$name/sv_main_target.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$1" && timeout -k 10 "$LIMIT" ./connect_harness) || status=$?
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
# The children use exit(), not _exit(), so libgcov's exit handler dumps
# and merges the per-process coverage automatically.
(cd "$OUT/cov" && ./connect_harness >/dev/null 2>&1) || true
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" || true)
# Exemption, proven and pinned: SV_ModelIndex's trailing `return i;` is
# unreachable dead code. The `for (i = 0; i < MAX_MODELS &&
# sv.model_precache[i]; i++)` loop exits only with i == MAX_MODELS or
# sv.model_precache[i] == NULL, and the `if (i == MAX_MODELS ||
# !sv.model_precache[i])` immediately after catches both arms (Con_Printf,
# return 0).
#
# The pin is the line number of the second `return i;` in $fixture; the
# anchor count is checked so an edit fails here until the proof is re-read.
mapfile -t _ri < <(grep -nF 'return i;' "$here/$fixture" | cut -d: -f1)
[ "${#_ri[@]}" -eq 2 ] && [ "${_ri[1]}" = "310" ] || {
	echo "FAIL: exemption anchor moved in $fixture (found:${_ri[*]}); re-check the proof" >&2
	exit 1
}
unrun=$(printf '%s\n' "$unrun" | grep -vE '^ *#####: *310:' || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines)"

echo "== $VARIANT: the listen-server arm of SV_SendReconnect =="
# The differential fixture is -DSERVERONLY, so SV_SendReconnect's
# `#if !defined(SERVERONLY)` listen arm is dead there by construction.
# test_listen_branch.c checks both sides of the runtime
# SvMain_IsDedicated() gate directly: dedicated sends only the
# stufftext; a listen server also runs Cmd_ExecuteString("reconnect\n").
mkdir -p "$OUT/listen"
cc "${INCS[@]}" "${CDEFS[@]}" "${WARN[@]}" -O2 -g \
	-c "$here/test_listen_branch.c" -o "$OUT/listen/test_listen_branch.o"
cc "$OUT/listen/test_listen_branch.o" "$OUT/opt/sv_main_target.o" \
	"$RUST_LIB" -lm -o "$OUT/listen/test_listen_branch"
(cd "$OUT/listen" && ./test_listen_branch)

echo "RESULT: PASS"
