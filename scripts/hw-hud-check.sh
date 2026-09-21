#!/usr/bin/env bash
#
# hw-hud-check.sh -- HexenWorld HUD status icons, checked on screen (GitHub #211)
#
# Runs an hwsv (deathmatch 1) and the integrated GL client under Xvfb, joins
# with `connect hw://`, screenshots the HUD and scores two icons against the
# pics in the player's own paks (no game art is committed):
#
#   net   gfx.wad "net", the disconnected-network icon SCR_DrawNet draws when
#         no server message arrived for 0.3 s.
#   rook  gfx/durshd1..16.lmp, the Icon of the Defender DrawActiveArtifacts
#         spins while ART_INVINCIBILITY is set.  HW deathmatch spawns every
#         player with 3 s of it (client.hc: invincible_time = time + 3).
#
# Assertions, each with its positive control so none can pass by looking at
# the wrong pixels:
#
#   1. During spawn the rook is drawn at least once (control), and 15 s after
#      connecting it is gone -- spawn protection expired and the client saw
#      artifact_active clear.
#   2. 15 s after connecting, on a live session, the net icon is absent.
#      Before the #211 fix it was drawn for the whole session: only the
#      Hexen II qsocket loop set cl.last_received_message.
#   3. With the server SIGSTOPped for ~3 s the net icon IS drawn (control:
#      the icon still means what it says), and after SIGCONT it clears.
#
# Requires what tools/headless-drive.sh needs (Xvfb, xdotool, ImageMagick 7,
# bubblewrap), plus util-linux script and python3, and a retail install:
# data1/pak0.pak + pak1.pak and hw/pak4.pak + hw/hwprogs.dat under --basedir.
# Without HexenWorld data it prints SKIP and exits 77.
#
# Usage:
#   nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
#     nixpkgs#bubblewrap nixpkgs#python3 nixpkgs#util-linux --command \
#     scripts/hw-hud-check.sh --basedir ~/hexen2 \
#       [--client result/bin/glhexen2] [--server result-1/bin/hwsv] \
#       [--port 26970] [--map hwdm1] [--keep DIR]
#
# Exit status 0 on pass, 1 on any failed assertion, 2 on bad usage, 77 skip.
# The frames and scores stay in --keep DIR (default: a temp dir, removed).
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BASEDIR=""
CLIENT="$ROOT/result/bin/glhexen2"
SERVER="$ROOT/result-1/bin/hwsv"   # 2nd link of: nix build .#default .#hwsv-bundled
PORT=26970
MAP=hwdm1
KEEP=""

# Screen geometry the icon boxes below are measured for: headless-drive's
# default 800x600 window, where the 2D canvas scale is 1.25.  SCR_DrawNet
# puts the 32x32 net pic at canvas (64,0); DrawActiveArtifacts puts the
# 48x32 rook at canvas (vid.width - 50, 1) when no ring is active.
NET_BOX="80 0 40 40"
ROOK_BOX="738 1 60 40"
PRESENT=0.5     # measured: net 0.82, rook 0.76 when drawn
ABSENT=0.35     # measured: net 0.02, rook <= 0.15 when not

usage() { sed -n '/^# Usage:/,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
	--basedir) BASEDIR="$2"; shift 2 ;;
	--client) CLIENT="$2"; shift 2 ;;
	--server) SERVER="$2"; shift 2 ;;
	--port) PORT="$2"; shift 2 ;;
	--map) MAP="$2"; shift 2 ;;
	--keep) KEEP="$2"; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) echo "hw-hud-check: unknown option $1" >&2; usage >&2; exit 2 ;;
	esac
done

[ -n "$BASEDIR" ] || { echo "hw-hud-check: --basedir is required" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
if [ ! -f "$BASEDIR/data1/pak0.pak" ] || [ ! -f "$BASEDIR/hw/pak4.pak" ]; then
	echo "hw-hud-check: SKIP: need data1/pak0.pak and hw/pak4.pak under $BASEDIR"
	exit 77
fi
for bin in "$CLIENT" "$SERVER"; do
	[ -x "$bin" ] || { echo "hw-hud-check: no executable at $bin" >&2; exit 2; }
done
for tool in Xvfb xdotool magick bwrap script python3; do
	command -v "$tool" >/dev/null 2>&1 ||
		{ echo "hw-hud-check: cannot find $tool on PATH" >&2; exit 2; }
done

if [ -n "$KEEP" ]; then
	mkdir -p "$KEEP" && WORK=$(cd "$KEEP" && pwd)
else
	WORK=$(mktemp -d /tmp/hw-hud-check.XXXXXX)
fi
SHOTS="$WORK/shots"         # headless-drive owns (and wipes) this one
FIFO="$WORK/console"
SLOG="$WORK/server.log"
STEPS="$WORK/steps.txt"
SRV_PID=""; HWSV_PID=""; DRIVE_PID=""
FAILURES=0

cleanup() {
	[ -n "$HWSV_PID" ] && kill -CONT "$HWSV_PID" 2>/dev/null
	for pid in "$DRIVE_PID" "$SRV_PID" "$HWSV_PID"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	exec 3>&-
	[ -n "$KEEP" ] || rm -rf "$WORK"
}
trap cleanup EXIT

say() { echo "hw-hud-check: $*"; }
fail() { say "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

# --- reference pics, straight from the install -----------------------------
REFS="$WORK/refs"
mkdir -p "$REFS"
python3 "$HERE/wadpic2ppm.py" "$BASEDIR" net "$REFS/net.ppm" || exit 1
for k in $(seq 1 16); do
	python3 "$HERE/wadpic2ppm.py" "$BASEDIR" "gfx/durshd$k.lmp" \
		"$REFS/durshd$k.ppm" || exit 1
done

# score SHOT KIND -> prints the best similarity for that icon
score() {
	local shot="$1" kind="$2" best=-1 v k
	if [ "$kind" = net ]; then
		# shellcheck disable=SC2086
		python3 "$HERE/hudicon-score.py" "$shot" "$REFS/net.ppm" $NET_BOX
		return
	fi
	# The rook spins through 16 frames; whichever one was on screen wins.
	for k in $(seq 1 16); do
		# shellcheck disable=SC2086
		v=$(python3 "$HERE/hudicon-score.py" "$shot" "$REFS/durshd$k.ppm" $ROOK_BOX)
		best=$(awk -v a="$best" -v b="$v" 'BEGIN { print (b > a) ? b : a }')
	done
	echo "$best"
}
above() { awk -v v="$1" -v t="$2" 'BEGIN { exit !(v >= t) }'; }

# --- server ------------------------------------------------------------------
rm -f "$FIFO"; mkfifo "$FIFO"
# `exec` makes hwsv the pty child of script, so its pid can be stopped and
# continued without matching a pattern against every process on the box.
# script runs the string through $SHELL, so every path is quoted for it (an
# install under "~/Hexen II" must still launch).  %q quotes for bash, so
# $SHELL is pinned to bash rather than the user's login shell.
SRV_CMD=$(printf 'exec %q -basedir %q -userdir %q -game hw -port %q +deathmatch 1 +map %q' \
	"$SERVER" "$BASEDIR" "$WORK/srvuser" "$PORT" "$MAP")
SHELL=$(command -v bash) script -qefc "$SRV_CMD" "$SLOG" < "$FIFO" >/dev/null 2>&1 &
SRV_PID=$!
exec 3> "$FIFO"
for _ in $(seq 1 50); do
	HWSV_PID=$(pgrep -P "$SRV_PID" -x hwsv 2>/dev/null | head -n1)
	[ -n "$HWSV_PID" ] && break
	sleep 0.2
done
[ -n "$HWSV_PID" ] || { say "hwsv did not start; log:"; cat "$SLOG"; exit 1; }
sleep 3

# --- client script -------------------------------------------------------------
{
	echo "sleep 25"
	echo "cmd connect hw://127.0.0.1:$PORT"
	# The spawn window: sampled every 0.5 s from 2 s to 11 s after connect,
	# which brackets load time plus the 3 s of spawn protection.
	echo "sleep 2"
	for i in $(seq -w 0 18); do
		echo "shotf spawn$i"
		echo "sleep 0.5"
	done
	echo "sleep 4"
	echo "shot 01-connected"
	# The watcher below stops the server as soon as 01 exists.
	echo "sleep 3"
	echo "shotf 02-server-stalled"
	# ... and continues it once 02 exists.
	echo "sleep 4"
	echo "shot 03-resumed"
} > "$STEPS"

ENGINE="$CLIENT" STEPS="$STEPS" "$ROOT/tools/headless-drive.sh" script \
	"$SHOTS" "$BASEDIR" > "$WORK/drive.log" 2>&1 &
DRIVE_PID=$!

wait_file() {
	local f="$1" n=0
	while [ ! -f "$f" ]; do
		kill -0 "$DRIVE_PID" 2>/dev/null || return 1
		sleep 0.2
		n=$((n + 1))
		[ "$n" -lt 900 ] || return 1
	done
}

if wait_file "$SHOTS/01-connected.png"; then
	kill -STOP "$HWSV_PID"
	say "server stopped after 01-connected"
	wait_file "$SHOTS/02-server-stalled.png"
	kill -CONT "$HWSV_PID"
	say "server continued after 02-server-stalled"
fi
wait "$DRIVE_PID"
DRIVE_PID=""

LOG="$SHOTS/qconsole.log"
if ! grep -q "HexenWorld signon complete" "$LOG" 2>/dev/null; then
	fail "client never completed HexenWorld signon"
	tail -30 "$WORK/drive.log"
	exit 1
fi
for f in 01-connected 02-server-stalled 03-resumed; do
	[ -f "$SHOTS/$f.png" ] || { fail "no $f.png"; exit 1; }
done

# --- assertions ----------------------------------------------------------------
: > "$WORK/scores.txt"
rook_seen=""
for f in "$SHOTS"/spawn*.png; do
	s=$(score "$f" rook)
	echo "$(basename "$f" .png) rook $s" >> "$WORK/scores.txt"
	above "$s" "$PRESENT" && rook_seen="$(basename "$f" .png) ($s)"
done
for f in 01-connected 02-server-stalled 03-resumed; do
	for kind in net rook; do
		echo "$f $kind $(score "$SHOTS/$f.png" "$kind")" >> "$WORK/scores.txt"
	done
done
get() { awk -v f="$1" -v k="$2" '$1 == f && $2 == k { print $3 }' "$WORK/scores.txt"; }

if [ -n "$rook_seen" ]; then
	say "ok: spawn-protection rook drawn during spawn, e.g. $rook_seen"
else
	fail "rook never drawn during spawn -- the box or the scorer is wrong, so the rook-clear check below proves nothing"
fi
s=$(get 01-connected rook)
if above "$s" "$ABSENT"; then
	fail "rook still drawn 15 s after connecting ($s): artifact_active never cleared"
else
	say "ok: rook cleared after spawn protection ($s)"
fi
s=$(get 01-connected net)
if above "$s" "$ABSENT"; then
	fail "net icon drawn on a live HexenWorld session ($s) -- GitHub #211"
else
	say "ok: no net icon on a live session ($s)"
fi
s=$(get 02-server-stalled net)
if above "$s" "$PRESENT"; then
	say "ok: net icon drawn while the server was stopped ($s)"
else
	fail "net icon not drawn with the server stopped ($s) -- the check cannot see the icon"
fi
s=$(get 03-resumed net)
if above "$s" "$ABSENT"; then
	fail "net icon still drawn after the server resumed ($s)"
else
	say "ok: net icon cleared once the server resumed ($s)"
fi

[ -n "$KEEP" ] && say "frames and scores in $WORK"
if [ "$FAILURES" -gt 0 ]; then
	say "$FAILURES assertion(s) failed"
	exit 1
fi
say "PASS"
exit 0
