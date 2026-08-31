#!/usr/bin/env bash
#
# netplay-drive.sh -- run h2ded and a real client against it over UDP loopback,
# headless, and hand back both console logs.
#
#   ./tools/netplay-drive.sh <outdir> <map> [server-cvars...]
#
# WHY THIS EXISTS.  Single player never touches the network code.  The loop
# driver hands the message to the client in place, so every bound it checks is
# NET_MAXMESSAGE and every path that only misbehaves at MAX_DATAGRAM is
# invisible.  uhexen2-xz8b was exactly that: the per-frame datagram was built
# eight times larger than the wire accepts and had been for years, because
# nobody had a way to run the datagram driver without two machines.  This is
# that way -- two bwrap sandboxes on one box, separate HOMEs so their
# qconsole.logs do not collide, sharing the host network namespace so 127.0.0.1
# reaches between them.
#
# The server needs NO X AT ALL (h2ded has no video code); the client runs on
# SDL offscreen plus llvmpipe.  Both are driven through Cmd_StartupScript and
# generated .cfg files rather than synthesized input, so nothing here needs
# Xvfb or xdotool.
#
# Traps worth knowing:
#   - Both processes default to $HOME/.local/share/hexen2.  Point them at the
#     same HOME and the client truncates the server's log out from under it.
#   - '+arg' stops collecting at the first token starting with '-', so anything
#     with a negative number in it has to go through a cfg.  Same trap
#     ride-lift.sh documents.
#   - The server runs forever by design; it is killed by timeout, so a non-zero
#     rc from it is expected and not reported.  Only the client's rc is.
#   - Give the server a few seconds before connecting.  Spawning a map parses
#     progs and runs droptofloor for every item; connecting into the middle of
#     that just fails.
#
# Requires: bwrap.  Run it as
#   nix shell nixpkgs#bubblewrap --command ./tools/netplay-drive.sh ...
set -u

[ $# -ge 2 ] || { sed -n '3,6p' "$0" >&2; exit 2; }
OUT=$1; MAP=$2; shift 2
BASEDIR=${BASEDIR:-$HOME/hexen2}
SERVER=${SERVER:-./result-h2ded/bin/h2ded}
CLIENT=${CLIENT:-./result/bin/glhexen2}
FRAMES=${FRAMES:-250}           # client frames to sit connected for
BOOT=${BOOT:-6}                 # seconds to let the server spawn the map
TIMEOUT=${TIMEOUT:-45}

command -v bwrap >/dev/null 2>&1 || {
  echo "netplay-drive: missing bwrap -- run under 'nix shell nixpkgs#bubblewrap --command'" >&2
  exit 3; }

# bwrap cannot --ro-bind through a symlink into the read-only store, and
# ./result is exactly such a symlink.
SERVER=$(readlink -f "$SERVER") || exit 3
CLIENT=$(readlink -f "$CLIENT") || exit 3
BASEDIR=$(readlink -f "$BASEDIR") || exit 3
[ -x "$SERVER" ]  || { echo "netplay-drive: no h2ded at $SERVER (set SERVER=)" >&2; exit 3; }
[ -x "$CLIENT" ]  || { echo "netplay-drive: no client at $CLIENT (set CLIENT=)" >&2; exit 3; }
[ -d "$BASEDIR" ] || { echo "netplay-drive: no basedir at $BASEDIR" >&2; exit 3; }
SDIR=$(dirname "$SERVER"); CDIR=$(dirname "$CLIENT")

rm -rf "$OUT"
mkdir -p "$OUT/srv/.local/share/hexen2/data1" "$OUT/cli/.local/share/hexen2/data1"

{
  echo "developer 1"
  for cv in "$@"; do echo "$cv"; done
  echo "map $MAP"
} > "$OUT/srv/.local/share/hexen2/data1/srv.cfg"

{
  echo "developer 1"
  echo "connect 127.0.0.1"
  i=0; while [ $i -lt "$FRAMES" ]; do echo "wait"; i=$((i+1)); done
  # 'quit' while connected opens the confirmation MENU and hangs to the
  # timeout, so disconnect first -- as ride-lift.sh does.
  echo "disconnect"; echo "wait"; echo "quit"
} > "$OUT/cli/.local/share/hexen2/data1/cli.cfg"

timeout -k 5 "$TIMEOUT" \
bwrap --dev-bind / / --die-with-parent \
      --bind "$OUT/srv" "$HOME" \
      --ro-bind "$BASEDIR" "$BASEDIR" --ro-bind "$SDIR" "$SDIR" \
  env HOME="$HOME" XDG_DATA_HOME="$HOME/.local/share" \
  "$SERVER" -basedir "$BASEDIR" -dedicated 4 +exec srv.cfg \
  >"$OUT/server.stdout" 2>&1 &
SRV=$!

sleep "$BOOT"

timeout -k 5 $((TIMEOUT - BOOT)) \
bwrap --dev-bind / / --die-with-parent \
      --bind "$OUT/cli" "$HOME" \
      --ro-bind "$BASEDIR" "$BASEDIR" --ro-bind "$CDIR" "$CDIR" \
  env HOME="$HOME" XDG_DATA_HOME="$HOME/.local/share" \
      SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  "$CLIENT" -basedir "$BASEDIR" -width 320 -height 240 -window -nosound \
      +exec cli.cfg \
  >"$OUT/client.stdout" 2>&1
rc=$?

wait $SRV 2>/dev/null

cp "$OUT/srv/.local/share/hexen2/qconsole.log" "$OUT/server.log" 2>/dev/null
cp "$OUT/cli/.local/share/hexen2/qconsole.log" "$OUT/client.log" 2>/dev/null

printf 'map=%s client_rc=%s  cvars=%s\n' "$MAP" "$rc" "${*:-none}"
# grep -c prints 0 AND exits 1 when it finds nothing, so no '|| echo 0' here --
# that would report the count twice.  Same trap ride-lift.sh documents.
CONN=$(grep -c 'connected' "$OUT/server.log" 2>/dev/null) || true
printf 'clients connected: %s\n' "${CONN:-0}"
ERRS=$(grep -ci 'illegible\|Host_Error\|SZ_GetSpace\|overflow' "$OUT/client.log" 2>/dev/null) || true
printf 'client errors: %s\n' "${ERRS:-0}"
SHED=$(grep -c 'datagram full' "$OUT/server.log" 2>/dev/null) || true
printf 'frames shedding entities: %s\n' "${SHED:-0}"
grep -o 'nearest shed bin [0-9]*' "$OUT/server.log" 2>/dev/null \
  | awk '{print $NF}' | sort -n | head -1 \
  | awk 'NF {printf "best-priority entity shed: bin %s\n", $1}'

[ "$rc" = 0 ] || echo "see $OUT/client.stdout" >&2
exit $rc
