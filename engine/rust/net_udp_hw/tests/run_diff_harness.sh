#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/hexenworld/shared/net_udp.c the way hwsv compiles it -- -DH2W
# -DSERVERONLY with hexenworld/server and hexenworld/shared ahead of h2shared on
# the include path, so "quakedef.h" is the HexenWorld one -- with every exported
# name and all four exported globals renamed so it can share a binary with the
# Rust module, and compare the two.
#
# One C variant: H2W_INTEGRATED is what renames this file's symbols for the
# *client*, and that renaming is a header macro, not different code, so the
# behaviour under test is the same one hwsv builds.  The port's own gate checks
# that both name sets come out right in the two targets.
#
# The Rust staticlib must have been built with net_udp_hw, sizebuf and huffman:
# the sizebuf and the codec are the Rust ports, and they are the same functions
# the C original calls, which is the substrate both arms share.
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features net_udp_hw,sizebuf,huffman" >&2
	exit 2
fi

# hwsv's include order: hexenworld/server, hexenworld/shared, h2shared, common.
INCS=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
      -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)
CDEFS=(-DH2W -DSERVERONLY)

RENAME=(
	-DNET_Init=c_NET_Init
	-DNET_Shutdown=c_NET_Shutdown
	-DNET_GetPacket=c_NET_GetPacket
	-DNET_SendPacket=c_NET_SendPacket
	-DNET_CheckReadTimeout=c_NET_CheckReadTimeout
	-DNET_CompareAdr=c_NET_CompareAdr
	-DNET_CompareBaseAdr=c_NET_CompareBaseAdr
	-DNET_AdrToString=c_NET_AdrToString
	-DNET_BaseAdrToString=c_NET_BaseAdrToString
	-DNET_StringToAdr=c_NET_StringToAdr
	-Dnet_local_adr=c_net_local_adr
	-Dnet_loopback_adr=c_net_loopback_adr
	-Dnet_from=c_net_from
	-Dnet_message=c_net_message
	-DLastCompMessageSize=c_LastCompMessageSize
)

echo "== compiling the C original as hwsv does (-DH2W -DSERVERONLY) =="
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" "${RENAME[@]}" \
	-c "$engine/hexenworld/shared/net_udp.c" -o "$OUT/net_udp_c.o"

echo "== compiling the per-target shim and the harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/net_udp_hw_target.c" -o "$OUT/net_udp_target.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/net_udp_c.o" \
	"$OUT/net_udp_target.o" "$RUST_LIB" -lm

echo "== running =="
# The timeout is load-bearing: the round-trip case talks to a real socket, and
# a port that fails to make it non-blocking would sit in recvfrom until the
# gate's own timeout took the job down.  The peer sets SO_RCVTIMEO as well, so
# a lost datagram fails the case rather than hanging it.
harness_timeout="${HARNESS_TIMEOUT:-60}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	echo "      That is what a socket left in blocking mode looks like: the" >&2
	echo "      receive path waits for a packet that is not coming." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"
