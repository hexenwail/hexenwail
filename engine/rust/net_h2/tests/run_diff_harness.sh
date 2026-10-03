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
# An extra -I for a platform that keeps headers outside the compiler's default
# search path.  FreeBSD is the case in hand: quakeinc.h reaches glheader.h from
# quakedef.h, libglvnd installs GL/gl.h under /usr/local/include, and base
# clang does not search that directory.  CPPFLAGS is the conventional carrier,
# so a caller sets it rather than every script growing a platform path.
if [ -n "${CPPFLAGS:-}" ]; then
  # shellcheck disable=SC2206  # deliberate: CPPFLAGS is a whitespace-separated flag list
  flags+=(${CPPFLAGS})
fi
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
cc "${inc[@]}" "${flags[@]}" -c "$engine/rust/net_udp_h2_target.c" -o "$work/udp_target.o"
cc "${inc[@]}" "${flags[@]}" -c "$here/diff_harness.c" -o "$work/harness.o"
cc -o "$work/diff_harness" "$work/harness.o" "$work/loop.o" "$work/udp.o" "$work/bsd.o" \
  "$work/udp_target.o" "$lib" -lm
# A watchdog, in case a fixture deadlocks on a socket read.  timeout(1) is GNU
# coreutils and macOS ships no /usr/bin/timeout, which is how the first macOS
# run of this harness died with 127 after compiling everything -- Homebrew's
# coreutils installs the same program as gtimeout.  Neither present is not
# fatal; it just means the caller's own time limit is the only one.
if command -v timeout >/dev/null 2>&1; then
  timeout -k 5 60 "$work/diff_harness"
elif command -v gtimeout >/dev/null 2>&1; then
  gtimeout -k 5 60 "$work/diff_harness"
else
  echo 'warning: neither timeout(1) nor gtimeout(1) on PATH; running the fixture unbounded' >&2
  "$work/diff_harness"
fi
