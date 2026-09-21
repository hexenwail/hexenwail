#!/usr/bin/env bash
#
# hw-cmd-check.sh -- console commands reach a HexenWorld server (GitHub #212)
#
# Runs an hwsv (deathmatch 1) and the integrated GL client under Xvfb, joins
# with `connect hw://`, then types commands into the client console with real
# key events (tools/headless-drive.sh) and checks the server acted on them:
#
#   say TOKEN          Host_Say -> Cmd_ForwardToServer
#   cmd say TOKEN2     Cmd_ForwardToServer_f (the "cmd" command)
#   kill               Host_Kill_f -> Cmd_ForwardToServer, seen as the -2
#                      frags ClientKill charges, read from the server's status
#   hwflood x50        ~9500 bytes of say in one frame, past the netchan's
#                      7500: the client must disconnect and say why, not go
#                      silent until the server times it out
#
# Before the fix all three went into cls.message, which nothing sends while
# HexenWorld owns the connection, so the server saw none of them.
#
# Positive control: a server-console `say` must reach the client, which
# proves the session and the logs are live, so a missing token means the
# client dropped it rather than that nothing was connected.
#
# Requires what tools/headless-drive.sh needs (Xvfb, xdotool, ImageMagick 7,
# bubblewrap), plus util-linux script, and a retail install: data1/pak0.pak +
# pak1.pak and hw/pak4.pak + hw/hwprogs.dat under --basedir.  Without
# HexenWorld data it prints SKIP and exits 77.
#
# Usage:
#   nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
#     nixpkgs#bubblewrap nixpkgs#util-linux --command \
#     scripts/hw-cmd-check.sh --basedir ~/hexen2 \
#       [--client result/bin/glhexen2] [--server result-1/bin/hwsv] \
#       [--port 26972] [--map hwdm1] [--keep DIR]
#
# Exit status 0 on pass, 1 on any failed assertion, 2 on bad usage, 77 skip.
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BASEDIR=""
CLIENT="$ROOT/result/bin/glhexen2"
SERVER="$ROOT/result-1/bin/hwsv"   # 2nd link of: nix build .#default .#hwsv-bundled
PORT=26972
MAP=hwdm1
KEEP=""

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
	*) echo "hw-cmd-check: unknown option $1" >&2; usage >&2; exit 2 ;;
	esac
done

[ -n "$BASEDIR" ] || { echo "hw-cmd-check: --basedir is required" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
if [ ! -f "$BASEDIR/data1/pak0.pak" ] || [ ! -f "$BASEDIR/hw/pak4.pak" ]; then
	echo "hw-cmd-check: SKIP: need data1/pak0.pak and hw/pak4.pak under $BASEDIR"
	exit 77
fi
for bin in "$CLIENT" "$SERVER"; do
	[ -x "$bin" ] || { echo "hw-cmd-check: no executable at $bin" >&2; exit 2; }
done
for tool in Xvfb xdotool bwrap script; do
	command -v "$tool" >/dev/null 2>&1 ||
		{ echo "hw-cmd-check: cannot find $tool on PATH" >&2; exit 2; }
done

if [ -n "$KEEP" ]; then
	mkdir -p "$KEEP" && WORK=$(cd "$KEEP" && pwd)
else
	WORK=$(mktemp -d /tmp/hw-cmd-check.XXXXXX)
fi
SHOTS="$WORK/drive"         # headless-drive owns (and wipes) this one
FIFO="$WORK/console"
SLOG="$WORK/server.log"
STEPS="$WORK/steps.txt"
SRV_PID=""; DRIVE_PID=""
FAILURES=0

cleanup() {
	for pid in "$DRIVE_PID" "$SRV_PID"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	exec 3>&-
	[ -n "$KEEP" ] || rm -rf "$WORK"
}
trap cleanup EXIT

say() { echo "hw-cmd-check: $*"; }
fail() { say "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

# Tokens nothing else prints, lowercase letters only so console key events
# type them exactly.
TOK=$(tr -dc 'a-z' < /dev/urandom | head -c 8)
T_SAY="saytok$TOK"
T_CMD="cmdtok$TOK"
T_SRV="srvtok$TOK"
# 180 letters: 50 of these in one frame (the hwflood10 x5 below) queue
# ~9500 bytes of reliable data, past the netchan's 7500 (HWNET_MAX_MSGLEN).
FLOOD="flood$(printf 'x%.0s' $(seq 1 175))"

# --- server ------------------------------------------------------------------
rm -f "$FIFO"; mkfifo "$FIFO"
SRV_CMD=$(printf 'exec %q -basedir %q -userdir %q -game hw -port %q +deathmatch 1 +map %q' \
	"$SERVER" "$BASEDIR" "$WORK/srvuser" "$PORT" "$MAP")
SHELL=$(command -v bash) script -qefc "$SRV_CMD" "$SLOG" < "$FIFO" >/dev/null 2>&1 &
SRV_PID=$!
exec 3> "$FIFO"
sleep 3

# --- client script -------------------------------------------------------------
cat > "$STEPS" <<EOF
sleep 25
cmd connect hw://127.0.0.1:$PORT
sleep 15
cmd say $T_SAY
sleep 2
cmd cmd say $T_CMD
sleep 2
cmd kill
sleep 4
shot 01-after-commands
cmd alias hwflood "say $FLOOD"
cmd alias hwflood10 "hwflood;hwflood;hwflood;hwflood;hwflood;hwflood;hwflood;hwflood;hwflood;hwflood"
cmd hwflood10;hwflood10;hwflood10;hwflood10;hwflood10
sleep 4
shot 02-after-flood
EOF

ENGINE="$CLIENT" STEPS="$STEPS" "$ROOT/tools/headless-drive.sh" script \
	"$SHOTS" "$BASEDIR" > "$WORK/drive.log" 2>&1 &
DRIVE_PID=$!

# headless-drive copies qconsole.log out only when the run ends; this is the
# file the sandboxed engine is writing right now.
LIVE="$SHOTS/work/home/.local/share/hexen2/qconsole.log"
wait_live() {
	local n=0
	until grep -qF -- "$1" "$LIVE" 2>/dev/null; do
		kill -0 "$DRIVE_PID" 2>/dev/null || return 1
		sleep 0.5; n=$((n + 1)); [ "$n" -lt 240 ] || return 1
	done
}

# frags of the one connected player, from the server console's `status`.
# Rows are "%5i %6i <address> <name>"; MARK brackets each dump in the log.
frags_now() {
	local mark="$1"
	# One command per write, spaced out: hwsv's stdin reader runs lines
	# that arrive in a single read together into one command.
	printf 'echo %s-begin\n' "$mark" >&3; sleep 0.7
	printf 'status\n' >&3; sleep 0.7
	printf 'echo %s-end\n' "$mark" >&3; sleep 1
	sed -n "/$mark-begin/,/$mark-end/p" "$SLOG" | tr -d '\r' |
		awk '$3 ~ /^127\.0\.0\.1/ { print $1; exit }'
}

FRAGS_BEFORE=""; FRAGS_AFTER=""
if wait_live "HexenWorld signon complete"; then
	sleep 2
	# Positive control: the session and both logs are live.
	printf 'say %s\n' "$T_SRV" >&3
	FRAGS_BEFORE=$(frags_now "fragsa$TOK")
	if wait_live "]kill"; then
		sleep 2
		FRAGS_AFTER=$(frags_now "fragsb$TOK")
	fi
fi

wait "$DRIVE_PID"
DRIVE_PID=""
printf 'quit\n' >&3
sleep 1

CLOG="$SHOTS/qconsole.log"
grep -q "HexenWorld signon complete" "$CLOG" 2>/dev/null ||
	{ fail "client never completed HexenWorld signon"; tail -30 "$WORK/drive.log"; exit 1; }

if grep -q "$T_SRV" "$CLOG"; then
	say "ok: server console say reached the client (session is live)"
else
	fail "server console say never reached the client -- nothing below can be trusted"
fi
if grep -q "$T_SAY" "$SLOG"; then
	say "ok: client 'say' reached the server"
else
	fail "client 'say' never reached the server -- GitHub #212"
fi
if grep -q "$T_CMD" "$SLOG"; then
	say "ok: client 'cmd say' reached the server"
else
	fail "client 'cmd say' never reached the server -- GitHub #212"
fi
# HW's ClientKill costs 2 frags (client.hc), which the server's `status`
# shows directly.  The suicide notice itself is an indexed print, which the
# client does not render yet, so it cannot be the evidence here.
if [ -z "$FRAGS_BEFORE" ]; then
	fail "could not read the player's frags from the server's status (before kill)"
elif [ -z "$FRAGS_AFTER" ]; then
	fail "could not read the player's frags from the server's status (after kill)"
elif [ "$FRAGS_AFTER" -lt "$FRAGS_BEFORE" ]; then
	say "ok: client 'kill' made the player suicide (frags $FRAGS_BEFORE -> $FRAGS_AFTER)"
else
	fail "client 'kill' had no effect (frags $FRAGS_BEFORE -> $FRAGS_AFTER) -- GitHub #212"
fi

# Flooding the reliable stream must end the session with a reason, not
# leave it silently dead until the server times it out.
if grep -q "too many commands queued for the server" "$CLOG"; then
	say "ok: an overflowed command queue disconnects with a message"
else
	fail "flooding forwarded commands did not report the netchan overflow"
fi

[ -n "$KEEP" ] && say "logs in $WORK"
if [ "$FAILURES" -gt 0 ]; then
	say "$FAILURES assertion(s) failed"
	exit 1
fi
say "PASS"
exit 0
