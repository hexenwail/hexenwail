#!/usr/bin/env bash
#
# hw-drive.sh -- run hwsv and a real client connected to it, headless, and
# hand back screenshots of the HexenWorld session.
#
#   STEPS=<stepfile> ./tools/hw-drive.sh <outdir> [extra client args...]
#
# WHY THIS EXISTS.  The client's HexenWorld reader (engine/hexen2/cl_hw.c) is
# reached only through `connect hw://`, so neither single player nor
# netplay-drive.sh (h2ded, Hexen II protocol) ever runs it.  Issue #309 sat in
# that gap: svc_particle2/3/4 drew the wrong effect for years.  This starts
# hwsv on loopback, boots the client under headless-drive.sh, connects it, and
# then runs your steps with the client already in the HexenWorld game.
#
# Environment:
#   STEPS    step file, headless-drive.sh `script` verbs (required)
#   BASEDIR  RETAIL game data             (default: ~/hexen2)
#   SERVER   hwsv binary                  (default: ./result-hwsv/bin/hwsv)
#   ENGINE   client binary                (default: ./result/bin/glhexen2)
#   MAP      map both sides load          (default: demo1)
#   PORT     server UDP port              (default: 26950)
#   SVARGS   server command line tail     (default: +deathmatch 0 +coop 0 +skill 0)
#   LOAD     seconds for the client's own map load before connecting (default: 25)
#   JOIN     seconds from `connect` to your first step               (default: 15)
#   TIMEOUT  hard kill for the server, seconds                       (default: 600)
#   W, H     client window size, passed through to headless-drive.sh
#
# Output in <outdir>: server.log (the server console), client/ (everything
# headless-drive.sh writes: PNGs, qconsole.log), steps.txt (your steps with the
# connect preamble in front).  Exit 0 only if the client log shows the
# HexenWorld signon.  OPEN THE PNGs: a signon proves the connection, not that
# your steps did what you meant.
#
# Traps worth knowing:
#   - hwsv refuses the demo data ("You must have the HexenWorld data
#     installed").  BASEDIR needs retail data1/pak0.pak + pak1.pak and
#     hw/pak4.pak.  hw/hwprogs.dat comes from the gamedir if it has one,
#     else from beside the engine: an ENGINE from .#hwsv-bundled carries it.
#   - hwsv prints nothing on a non-tty stdout, so it runs under `script` for a
#     pty.  server.log is that pty's transcript.
#   - The gamecode's cheat impulses return early when deathmatch or coop is
#     set, and hwsv defaults to deathmatch 1.  The default SVARGS turns both
#     off; override it to test a real deathmatch.
#   - `cmd` steps need key_dest == key_game, so the client loads MAP locally
#     first and connects from its console.  `+connect` on the command line
#     connects too but leaves no console to type into.
#   - The server is killed by this script on the way out.  It never touches
#     another hwsv: it kills its own process group, not a name.
#
# Requires: script (util-linux) plus everything headless-drive.sh needs.  Run as
#   nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
#     env STEPS=tools/hw-bluestep.steps ./tools/hw-drive.sh /tmp/out-hw
set -u

usage() { sed -n '3,6p' "$0" >&2; exit 2; }
[ $# -ge 1 ] || usage
OUT=$1; shift
[ -n "${STEPS:-}" ] && [ -f "$STEPS" ] || { echo "hw-drive: set STEPS= to a step file" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
BASEDIR=${BASEDIR:-$HOME/hexen2}
SERVER=${SERVER:-./result-hwsv/bin/hwsv}
MAP=${MAP:-demo1}
PORT=${PORT:-26950}
SVARGS=${SVARGS:-+deathmatch 0 +coop 0 +skill 0}
LOAD=${LOAD:-25}
JOIN=${JOIN:-15}
TIMEOUT=${TIMEOUT:-600}

command -v script >/dev/null 2>&1 || { echo "hw-drive: missing required tool: script" >&2; exit 3; }
SERVER=$(readlink -f "$SERVER") || exit 3
BASEDIR=$(readlink -f "$BASEDIR") || exit 3
[ -x "$SERVER" ] || { echo "hw-drive: no hwsv at $SERVER (set SERVER=)" >&2; exit 3; }
[ -f "$BASEDIR/hw/pak4.pak" ] || {
  echo "hw-drive: no hw/pak4.pak under $BASEDIR -- hwsv needs retail data (set BASEDIR=)" >&2; exit 3; }

rm -rf "$OUT"; mkdir -p "$OUT/srv"
OUT=$(readlink -f "$OUT")

{
  echo "sleep $LOAD"
  echo "cmd connect hw://127.0.0.1:$PORT"
  echo "sleep $JOIN"
  cat "$STEPS"
} > "$OUT/steps.txt"

SPID=""
cleanup() {
  trap - EXIT INT TERM HUP
  [ -n "$SPID" ] && { kill -- "-$SPID" 2>/dev/null; sleep 1; kill -9 -- "-$SPID" 2>/dev/null; }
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# SVARGS is split on purpose: it is a command line tail.
# shellcheck disable=SC2086
SVCMD=$(printf '%q ' "$SERVER" -basedir "$BASEDIR" -userdir "$OUT/srv" -port "$PORT" $SVARGS +map "$MAP")
# setsid makes the server its own process group, so cleanup can kill exactly
# this run's script + hwsv and nothing else.
setsid timeout -k 5 "$TIMEOUT" script -qfc "$SVCMD" "$OUT/server.log" >/dev/null 2>&1 &
SPID=$!

# Spawning the map parses progs and builds the PHS; connecting into the middle
# of that just fails.
i=0
until grep -aq 'Building PHS' "$OUT/server.log" 2>/dev/null; do
  kill -0 "$SPID" 2>/dev/null || { echo "hw-drive: hwsv exited early; see $OUT/server.log" >&2; exit 4; }
  i=$((i+1)); [ $i -le 60 ] || { echo "hw-drive: hwsv did not spawn $MAP in 30s; see $OUT/server.log" >&2; exit 4; }
  sleep 0.5
done
sleep 2
echo "hwsv up on 127.0.0.1:$PORT, map $MAP"

STEPS="$OUT/steps.txt" "$HERE/headless-drive.sh" script "$OUT/client" "$BASEDIR" +map "$MAP" "$@"
rc=$?

JOINED=$(grep -ac 'player joined the game\|entered the game' "$OUT/server.log" 2>/dev/null) || true
SIGNON=$(grep -ac 'HexenWorld signon complete' "$OUT/client/qconsole.log" 2>/dev/null) || true
printf 'client_rc=%s  server saw joins: %s  client signons: %s\n' "$rc" "${JOINED:-0}" "${SIGNON:-0}"
[ "$rc" = 0 ] || exit "$rc"
[ "${SIGNON:-0}" -ge 1 ] || { echo "hw-drive: the client never signed on; see $OUT/client/qconsole.log" >&2; exit 5; }
exit 0
