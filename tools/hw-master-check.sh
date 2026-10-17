#!/usr/bin/env bash
#
# hw-master-check.sh -- HexenWorld's server-list path, end to end (GitHub #38).
#
# Usage: hw-master-check.sh BASEDIR [OUTDIR]
#
#   BASEDIR  a Hexen II install with retail data1/pak0.pak + pak1.pak and
#            hw/pak4.pak.  hwsv refuses the demo data, so this cannot run in
#            CI, which carries no game data; it is a local harness.
#   OUTDIR   where the logs go (default: a fresh mktemp dir, printed).
#
# Starts hwmaster and two hwsv instances on one box, each server pointed at the
# master with `setmaster`, then asserts through the shipped tools:
#
#   1. hwmquery lists exactly both servers, by port   (S2M_HEARTBEAT)
#   2. after `hwrcon ... quit` to one of them, the list holds only the other
#                                                      (S2M_SHUTDOWN)
#
# Ports are random in 40000-45999 so a run never collides with a real server
# on 26900/26950.  `setmaster` zeroes svs.last_heartbeat, so the first
# heartbeat goes out on the next frame rather than after HEARTBEAT_SECONDS.
#
# Environment:
#   ENGINE  hwsv to run          (default: nix build .#hwsv-bundled, which
#                                 brings its own hw/hwprogs.dat, #37)
#   UTILS   dir holding bin/hwmaster, bin/hwmquery, bin/hwrcon
#                                (default: nix build .#utils)
#
# Exit 0 when both assertions hold.  hwsv prints nothing on a non-tty stdout,
# so each runs under `script` for a pty; its transcript is OUTDIR/svPORT.log.
# Every process started here is killed by pid on exit -- never by pattern.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	sed -n '2,/^# SPDX/p' "$0" >&2
	exit 2
fi
basedir=$(readlink -f "$1")
out=${2:-$(mktemp -d)}
mkdir -p "$out"
out=$(readlink -f "$out")

for f in data1/pak0.pak data1/pak1.pak hw/pak4.pak; do
	[ -f "$basedir/$f" ] || { echo "error: $basedir/$f not found (retail HexenWorld data needed)" >&2; exit 2; }
done
command -v script >/dev/null || { echo "error: script(1) is required" >&2; exit 2; }

repo=$(cd "$(dirname "$0")/.." && pwd)
engine=${ENGINE:-$(nix build "$repo#hwsv-bundled" --no-link --print-out-paths)/bin/hwsv}
utils=${UTILS:-$(nix build "$repo#utils" --no-link --print-out-paths)}
for t in hwmaster hwmquery hwrcon; do
	[ -x "$utils/bin/$t" ] || { echo "error: $utils/bin/$t missing" >&2; exit 2; }
done

master_port=$((40000 + RANDOM % 5000))
port_a=$((master_port + 501))
port_b=$((master_port + 502))
password="check$RANDOM"

pids=()
cleanup() {
	local p
	for p in "${pids[@]}"; do
		kill "$p" 2>/dev/null || true
	done
	wait 2>/dev/null || true
}
trap cleanup EXIT

# The nix-built tools die on this box's exported LD_LIBRARY_PATH.
unset LD_LIBRARY_PATH

"$utils/bin/hwmaster" -port "$master_port" </dev/null >"$out/master.log" 2>&1 &
pids+=($!)

for p in "$port_a" "$port_b"; do
	timeout -k 3 120 script -qec "$engine -basedir $basedir -userdir $out/ud$p -port $p \
+rcon_password $password +setmaster 127.0.0.1:$master_port +map demo1" /dev/null \
		</dev/null >"$out/sv$p.log" 2>&1 &
	pids+=($!)
done

# Poll the master until it settles on `want` (space-separated ports), or fail.
listed() {
	"$utils/bin/hwmquery" "127.0.0.1:$master_port" 2>/dev/null |
		sed -n 's/^[0-9.]*:\([0-9][0-9]*\)$/\1/p' | sort -n | tr '\n' ' ' | sed 's/ $//'
}
expect() {
	local want=$1 what=$2 got="" i
	for i in $(seq 1 30); do
		got=$(listed || true)
		[ "$got" = "$want" ] && { echo "PASS: $what -- master lists [$got]"; return 0; }
		sleep 1
	done
	echo "FAIL: $what -- master lists [$got], wanted [$want]; logs in $out" >&2
	return 1
}

expect "$port_a $port_b" "two hwsv heartbeat to hwmaster"

"$utils/bin/hwrcon" "127.0.0.1:$port_a" "$password" quit >"$out/rcon.log" 2>&1 || true
# The master's only other way to drop a server is SV_TIMEOUT, 450 s without a
# heartbeat, so a drop inside expect's 30 s window is the shutdown packet.
# (hwmaster's own "S2M_SHUTDOWN" line is block-buffered into master.log and
# may not have reached it yet, so it is no evidence either way.)
expect "$port_b" "a server that quits is dropped (S2M_SHUTDOWN)"

echo "PASS: hw-master-check (logs in $out)"
