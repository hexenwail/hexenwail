#!/usr/bin/env bash
#
# hw-smoke.sh -- headless HexenWorld integration smoke test (uhexen2-6g8q.14)
#
# Brings up an hwsv against a real Hexen II + HexenWorld data install and walks
# the same ground the old client network tests did, without a display:
#
#   1. hwsv boots, mounts hw/ on top of data1, spawns a map.
#   2. Two GL clients connect by plain address -- no hw:// scheme -- so the
#      automatic H2-then-HW negotiation is what picks the protocol (issue #50).
#      Each signs on, mounts the server's gamedir, and renders the world.
#      Both must reach "signon complete" with zero unknown/malformed protocol
#      messages and with the loading plaque dismissed.
#   3. A server broadcast is delivered to both (say smoke-<token>).
#   4. A live map change is issued from the server console; both clients see
#      "Changing map..." / "reconnecting..." and complete a SECOND signon
#      without aborting the reader, and the static-entity list does not
#      overflow ("Too many HexenWorld static entities").
#   5. When Siege is installed, a Siege client renders successfully, while a
#      client with data1+hw but no Siege map aborts before signon with an
#      actionable maps/siege.bsp error.
#
# Requires:
#   - Hexen II data1 (pak0/pak1) and a HexenWorld gamedir (hw/pak4.pak,
#     hw/hwprogs.dat) under --basedir.  Registered data cannot ship in CI, so
#     this runs on machines/installs where it exists; CI keeps its data-less
#     hwsv boot smoke.
#   - Xvfb for the GL clients; pass the store path with --xvfb if Xvfb is not
#     on PATH, and --client / --server for the binaries.
#
# Usage:
#   hw-smoke.sh --basedir DIR [--client BIN] [--server BIN] [--xvfb BIN]
#                [--display :99] [--map hwdm1] [--pause-on-map-change N]
#
# Exit status 0 on pass, 1 on any failed assertion.
#

set -u

BASEDIR=""
CLIENT=""
SERVER=""
XVFB=""
DISPNUM=":99"
MAP="hwdm1"
SECOND_MAP="hwdm2"
MAX_WAIT=180

while [ $# -gt 0 ]; do
	case "$1" in
	--basedir) BASEDIR="$2"; shift 2 ;;
	--client) CLIENT="$2"; shift 2 ;;
	--server) SERVER="$2"; shift 2 ;;
	--xvfb) XVFB="$2"; shift 2 ;;
	--display) DISPNUM="$2"; shift 2 ;;
	--map) MAP="$2"; shift 2 ;;
	--second-map) SECOND_MAP="$2"; shift 2 ;;
	*) echo "hw-smoke: unknown option $1" >&2; exit 2 ;;
	esac
done

[ -n "$BASEDIR" ] || { echo "hw-smoke: --basedir is required" >&2; exit 2; }
[ -d "$BASEDIR/data1" ] || { echo "hw-smoke: $BASEDIR/data1 not found" >&2; exit 2; }
[ -d "$BASEDIR/hw" ] || { echo "hw-smoke: $BASEDIR/hw not found" >&2; exit 2; }

command -v "$CLIENT" >/dev/null 2>&1 || CLIENT="${CLIENT:-glhexen2}"
command -v "$SERVER" >/dev/null 2>&1 || SERVER="${SERVER:-hwsv}"
command -v "$XVFB" >/dev/null 2>&1 || XVFB="${XVFB:-Xvfb}"

for bin in "$CLIENT" "$SERVER" "$XVFB"; do
	command -v "$bin" >/dev/null 2>&1 || { echo "hw-smoke: cannot find $bin" >&2; exit 2; }
done

WORK=$(mktemp -d /tmp/hw-smoke.XXXXXX)
SLOG="$WORK/server.log"
ALOG="$WORK/client-a.log"
BLOG="$WORK/client-b.log"
FIFO="$WORK/console"
FAILURES=0
# Initialised before the trap: cleanup runs under `set -u`, so an early exit
# (Xvfb refused to start, an unusable basedir) must not trip over an unset PID.
SRV_PID=""; CA_PID=""; CB_PID=""; CP_PID=""; SIEGE_PID=""; XV_PID=""

cleanup() {
	[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
	[ -n "$CA_PID" ] && kill "$CA_PID" 2>/dev/null
	[ -n "$CB_PID" ] && kill "$CB_PID" 2>/dev/null
	[ -n "$CP_PID" ] && kill "$CP_PID" 2>/dev/null
	[ -n "$SIEGE_PID" ] && kill "$SIEGE_PID" 2>/dev/null
	[ -n "$XV_PID" ] && kill "$XV_PID" 2>/dev/null
	exec 3>&- 4>&-
	rm -rf "$WORK"
}
trap cleanup EXIT

say() { echo "hw-smoke: $*"; }
fail() { say "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

# --- wait_for FILE GREPEXPR [maxseconds] : polls until grep finds a line ---
wait_for() {
	local file="$1" expr="$2" max="${3:-$MAX_WAIT}" waited=0
	while [ "$waited" -lt "$max" ]; do
		if [ -f "$file" ] && grep -qE "$expr" "$file" 2>/dev/null; then
			return 0
		fi
		sleep 1
		waited=$((waited + 1))
	done
	return 1
}

# --- wait_count FILE GREPEXPR MIN [maxseconds] : polls until MIN matches seen ---
wait_count() {
	local file="$1" expr="$2" min="$3" max="${4:-$MAX_WAIT}" waited=0 n=0
	while [ "$waited" -lt "$max" ]; do
		n=$(grep -cE "$expr" "$file" 2>/dev/null || true)
		[ "$((n + 0))" -ge "$min" ] && return 0
		sleep 1
		waited=$((waited + 1))
	done
	return 1
}

# --- no_badness FILE : count protocol-abort markers; they are fatal ---
no_badness() {
	local file="$1" n
	n=$(grep -cE 'Malformed HexenWorld server message|awaits state adapter|FATAL ERROR' "$file" 2>/dev/null || true)
	[ "$((n + 0))" = "0" ]
}

say "workdir: $WORK"
say "server : $SERVER"
say "client : $CLIENT"

# --- Xvfb ---
"$XVFB" "$DISPNUM" -screen 0 800x600x24 -nolisten tcp > "$WORK/xvfb.log" 2>&1 &
XV_PID=$!
sleep 1.5

# --- hwsv on a pty so the console accepts commands ---
mkfifo "$FIFO"
exec 3<>"$FIFO"		# keep a writer open so the reader never sees EOF

script -qefc "$SERVER -basedir $BASEDIR -game hw +map $MAP" "$SLOG" < "$FIFO" >/dev/null 2>&1 &
SRV_PID=$!

wait_for "$SLOG" 'Building PHS' || { tail -40 "$SLOG"; fail "server did not reach map spawn"; }

# --- client A ---
DISPLAY="$DISPNUM" stdbuf -oL -eL "$CLIENT" -basedir "$BASEDIR" -hwport 27001 +developer 1 \
	+connect "127.0.0.1" > "$ALOG" 2>&1 &
CA_PID=$!

wait_for "$ALOG" 'HexenWorld signon complete' || { fail "client A did not complete signon"; }
wait_for "$ALOG" 'HexenWorld server set the gamedir to hw' || fail "client A did not mount the hw gamedir"
wait_for "$ALOG" 'player 0 spawned' || fail "client A never saw its own spawn"
# A plain address must reach HW by negotiation, never by the hw:// override.
wait_for "$ALOG" 'No Hexen II response; trying HexenWorld' || fail "client A did not autodetect HexenWorld"
# Signon has to dismiss the loading plaque and actually draw, or the player sits
# behind a frozen loading screen (the reported issue #50 failure).
wait_for "$ALOG" 'First world draw completed' || fail "client A never rendered the world"
grep -q 'HexenWorld missing world map' "$ALOG" && fail "client A reported a missing world map"
no_badness "$ALOG" || fail "client A hit a protocol abort"

# --- client B ---
sleep 2
DISPLAY="$DISPNUM" stdbuf -oL -eL "$CLIENT" -basedir "$BASEDIR" -hwport 27002 +developer 1 \
	+connect "hw://127.0.0.1" > "$BLOG" 2>&1 &
CB_PID=$!

wait_for "$BLOG" 'HexenWorld protocol 100.*player 1' || { fail "server did not give client B player slot 1"; }
wait_for "$BLOG" 'HexenWorld signon complete' || fail "client B did not complete signon"
wait_for "$BLOG" 'player 1 spawned' || fail "client B never saw its own spawn"
wait_for "$BLOG" 'First world draw completed' || fail "client B never rendered the world"
no_badness "$BLOG" || fail "client B hit a protocol abort"

# --- broadcast to both ---
TOKEN="smoke-$(date +%s)-$$"
printf 'say %s\n' "$TOKEN" >&3
sleep 5
grep -q "$TOKEN" "$ALOG" || fail "client A did not receive the server broadcast"
grep -q "$TOKEN" "$BLOG" || fail "client B did not receive the server broadcast"

# --- live map change ---
# Model/skin loading on a cold cache takes a while on llvmpipe boxen; the
# changing->reconnect->signon window is generous.
printf 'map %s\n' "$SECOND_MAP" >&3
wait_for "$ALOG" 'Changing map' 90 || fail "client A missed the changing broadcast"
wait_for "$BLOG" 'Changing map' 90 || fail "client B missed the changing broadcast"
wait_for "$ALOG" 'reconnecting' 90 || fail "client A did not re-arm signon"
wait_for "$BLOG" 'reconnecting' 90 || fail "client B did not re-arm signon"

# second signon = two occurrences (each is a full precache pass on llvmpipe,
# so this is the slowest leg).
wait_count "$ALOG" 'HexenWorld signon complete' 2 180 || fail "client A did not complete a second signon (saw $(grep -c 'HexenWorld signon complete' "$ALOG" 2>/dev/null || echo 0))"
wait_count "$BLOG" 'HexenWorld signon complete' 2 180 || fail "client B did not complete a second signon (saw $(grep -c 'HexenWorld signon complete' "$BLOG" 2>/dev/null || echo 0))"

# statics: either the map has some (and they are parsed) or none; overflow is
# always a failure.
grep -q 'Too many HexenWorld static entities' "$ALOG" && fail "static entity list overflowed"
grep -q 'Too many HexenWorld static entities' "$BLOG" && fail "static entity list overflowed"
nsa=$(grep -c 'HexenWorld static ' "$ALOG" 2>/dev/null || true)
nsb=$(grep -c 'HexenWorld static ' "$BLOG" 2>/dev/null || true)
say "static records parsed: A=$((nsa + 0)) B=$((nsb + 0))"

no_badness "$ALOG" || fail "client A aborted during/after the map change"
no_badness "$BLOG" || fail "client B aborted during/after the map change"

# --- optional Siege leg (issue #50) ---
# Siege is an HW mod, not a third protocol: the server advertises gamedir
# "siege" and the client must layer siege/ above the hw/ base.  Only runs when
# the mod is actually installed, since the base hw install has no siege.bsp.
if [ -d "$BASEDIR/siege" ] && [ -f "$BASEDIR/siege/maps/siege.bsp" ]; then
	CLOG="$WORK/client-siege.log"
	SFIFO="$WORK/console-siege"
	mkfifo "$SFIFO"
	exec 4<>"$SFIFO"
	script -qefc "$SERVER -basedir $BASEDIR -userdir $WORK/usr-siege -game siege -port 27003 +map siege" \
		"$WORK/server-siege.log" < "$SFIFO" >/dev/null 2>&1 &
	SIEGE_PID=$!

	wait_for "$WORK/server-siege.log" 'Building PHS' || fail "siege server did not spawn its map"

	DISPLAY="$DISPNUM" stdbuf -oL -eL "$CLIENT" -basedir "$BASEDIR" -hwport 27004 +developer 1 \
		+connect "127.0.0.1:27003" > "$CLOG" 2>&1 &
	CP_PID=$!

	wait_for "$CLOG" 'HexenWorld server set the gamedir to siege' 120 || fail "siege client did not mount the siege gamedir"
	wait_for "$CLOG" 'HexenWorld signon complete: siege' 180 || fail "siege client did not complete signon"
	wait_for "$CLOG" 'First world draw completed' 120 || fail "siege client never rendered the world"
	grep -q 'HexenWorld missing world map' "$CLOG" && fail "siege client reported a missing world map"
	no_badness "$CLOG" || fail "siege client hit a protocol abort"

	kill "$CP_PID" 2>/dev/null; wait "$CP_PID" 2>/dev/null; CP_PID=""

	# The same server against a client install with data1+hw but no Siege map
	# must abort before signon with the exact missing path. Symlinks avoid
	# copying proprietary data into the test workdir.
	MISSING_BASE="$WORK/missing-base"
	MLOG="$WORK/client-missing-siege.log"
	mkdir -p "$MISSING_BASE/siege"
	ln -s "$BASEDIR/data1" "$MISSING_BASE/data1"
	ln -s "$BASEDIR/hw" "$MISSING_BASE/hw"
	DISPLAY="$DISPNUM" stdbuf -oL -eL "$CLIENT" -basedir "$MISSING_BASE" -hwport 27005 +developer 1 \
		+connect "127.0.0.1:27003" > "$MLOG" 2>&1 &
	CP_PID=$!
	wait_for "$MLOG" 'Host_Error: HexenWorld missing world map: maps/siege\.bsp' 180 || \
		fail "missing-Siege client did not report the required world map"
	grep -q 'HexenWorld signon complete' "$MLOG" && fail "missing-Siege client completed signon"
	wait_for "$MLOG" 'Install hw/pak4\.pak.*siege mod assets' 10 || \
		fail "missing-Siege error was not actionable"
	kill "$CP_PID" 2>/dev/null; wait "$CP_PID" 2>/dev/null; CP_PID=""

	printf 'quit\n' >&4
	sleep 2
	wait "$SIEGE_PID" 2>/dev/null; SIEGE_PID=""
else
	say "siege: skipped ($BASEDIR/siege/maps/siege.bsp not installed)"
fi

# --- shut down ---
printf 'quit\n' >&3
sleep 2
kill "$CA_PID" "$CB_PID" 2>/dev/null
wait "$CA_PID" 2>/dev/null; wait "$CB_PID" 2>/dev/null
CA_PID=""; CB_PID=""
wait "$SRV_PID" 2>/dev/null; SRV_PID=""

if [ "$FAILURES" -eq 0 ]; then
	say "PASS: HexenWorld smoke (autodetect, 2 clients, map change, optional Siege/content checks)."
	exit 0
fi
say "FAILED: $FAILURES assertion(s).  Logs: $WORK (removed on exit)."
say "Client A tail:"; tail -8 "$ALOG" 2>/dev/null
say "Client B tail:"; tail -8 "$BLOG" 2>/dev/null
say "Server tail:"; tail -8 "$SLOG" 2>/dev/null
exit 1