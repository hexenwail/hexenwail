#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
engine="$(cd "$here/../../.." && pwd)"
root="$(cd "$engine/.." && pwd)"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
lib="${RUST_LIB:-$engine/rust/target/release/libengine_rs.a}"
inc=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
flags=(-std=gnu11 -O1 -g -Wall -Wno-unused-function -DGLQUAKE -D_GNU_SOURCE=1)
renames=()
for fn in Init Shutdown Listen SearchForHosts Connect CheckNewConnections GetMessage SendMessage SendUnreliableMessage CanSendMessage CanSendUnreliableMessage Close; do
  renames+=("-DLoop_${fn}=c_Loop_${fn}")
done
for fn in Init Shutdown Listen OpenSocket CloseSocket Connect CheckNewConnections Read Write Broadcast AddrToString StringToAddr GetSocketAddr GetNameFromAddr GetAddrFromName AddrCompare GetSocketPort SetSocketPort; do
  renames+=("-DUDP_${fn}=c_UDP_${fn}")
done
cc "${inc[@]}" "${flags[@]}" "${renames[@]}" -c "$engine/hexen2/net_loop.c" -o "$work/loop.o"
cc "${inc[@]}" "${flags[@]}" "${renames[@]}" -c "$engine/hexen2/net_udp.c" -o "$work/udp.o"
cc "${inc[@]}" "${flags[@]}" "${renames[@]}" \
  -Dnet_drivers=c_net_drivers -Dnet_landrivers=c_net_landrivers \
  -Dnet_numdrivers=c_net_numdrivers -Dnet_numlandrivers=c_net_numlandrivers \
  -c "$engine/hexen2/net_bsd.c" -o "$work/bsd.o"
cc "${inc[@]}" "${flags[@]}" -c "$here/diff_harness.c" -o "$work/harness.o"
cc -o "$work/diff_harness" "$work/harness.o" "$work/loop.o" "$work/udp.o" "$work/bsd.o" "$lib" -lm
timeout -k 5 60 "$work/diff_harness"
