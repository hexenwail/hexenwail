#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/hexenworld/shared/net_chan.c the way hwsv compiles it -- -DH2W
# -DSERVERONLY with hexenworld/server and hexenworld/shared ahead of h2shared on
# the include path -- with every exported name and net_drop renamed so it can
# share a binary with the Rust module, and compare the two.
#
# One C variant: H2W_INTEGRATED is what renames this file's symbols for the
# *client*, and that renaming is a header macro rather than different code, so
# the behaviour under test is the one hwsv builds.  The port's own gate checks
# that both name sets come out right in the two targets that build it.
#
# The per-target shim net_chan_target.c is compiled in, as the transport
# harness compiles its own: it is what provides net_drop, the
# NOT_DEMOPLAYBACK predicate and the variadic Netchan_OutOfBandPrint, so the
# Rust arm is exercised exactly as the engine builds it.
#
# The Rust staticlib must have been built with net_chan, sizebuf and msg_io:
# the sizebuf and the message reader are Rust ports, and they are the same
# functions the C original calls -- the substrate both arms share.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: consolidated Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features net_chan,sizebuf,msg_io" >&2
	exit 2
fi

# hwsv's include order: hexenworld/server, hexenworld/shared, h2shared, common.
INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
      -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)
CDEFS=(-DH2W -DSERVERONLY)

RENAME=(
	-DNetchan_Init=c_Netchan_Init
	-DNetchan_OutOfBand=c_Netchan_OutOfBand
	-DNetchan_OutOfBandPrint=c_Netchan_OutOfBandPrint
	-DNetchan_Setup=c_Netchan_Setup
	-DNetchan_CanPacket=c_Netchan_CanPacket
	-DNetchan_CanReliable=c_Netchan_CanReliable
	-DNetchan_Transmit=c_Netchan_Transmit
	-DNetchan_Process=c_Netchan_Process
	-Dnet_drop=c_net_drop
)

echo "== compiling the C original as hwsv does (-DH2W -DSERVERONLY) =="
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" "${RENAME[@]}" \
	-c "$engine/hexenworld/shared/net_chan.c" -o "$OUT/net_chan_c.o"

echo "== compiling the per-target shim and the harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/net_chan_target.c" -o "$OUT/net_chan_target.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

# The C's Netchan_OutOfBandPrint and the shim's variadic wrapper both format
# through q_vsnprintf, so the real qsnprint.c is linked in.
cc "${INCS[@]}" "${CFLAGS[@]}" -c "$root/common/qsnprint.c" -o "$OUT/qsnprint.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/net_chan_c.o" \
	"$OUT/net_chan_target.o" "$OUT/qsnprint.o" "$RUST_LIB" -lm

echo "== running =="
harness_timeout="${HARNESS_TIMEOUT:-60}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"
