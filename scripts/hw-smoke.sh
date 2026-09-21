#!/usr/bin/env bash
#
# hw-smoke.sh -- headless HexenWorld integration smoke test (uhexen2-6g8q.14)
#
# Brings up an hwsv against a real Hexen II + HexenWorld data install and walks
# the same ground the old client network tests did, without a display:
#
#   0. Plaque on a failed connect (Hexen II data1 only, no server): connect to
#      a dead port and disconnect before the 5 second connect retry.  Only
#      HWCL_Disconnect can drop the loading plaque there -- no download ever
#      starts and no retry fires -- so a missing drop prints "load timeout.".
#   1. hwsv boots, mounts hw/ on top of data1, spawns a map.
#   2. A tiny UDP fixture sends client A a syntactically valid H2 acceptance
#      and then stays silent. The bounded post-accept timeout must still select
#      HW; client B uses the explicit hw:// override. Each signs on, mounts the
#      server's gamedir, and renders the world (issue #50).
#      Both must reach "signon complete" with zero unknown/malformed protocol
#      messages and with the loading plaque dismissed.
#   3. A server broadcast is delivered to both (say smoke-<token>).
#   4. A live map change is issued from the server console; both clients see
#      "Changing map..." / "reconnecting..." and complete a SECOND signon
#      without aborting the reader, and the static-entity list does not
#      overflow ("Too many HexenWorld static entities").  Neither client may
#      hit the loading plaque's "load timeout.".
#   5. When Siege is installed, a Siege client renders successfully, while a
#      client with data1+hw but no Siege map aborts before signon with an
#      actionable maps/siege.bsp error.
#   6. Mod + download: a second hwsv runs a throwaway mod gamedir holding a
#      LOOSE map (hwsv refuses to send maps that live in a pak), extracted from
#      hw/pak4.pak under a name no install has.  A client with a fresh userdir
#      must download it, mount hw beneath the mod (no hw asset is downloaded,
#      no "can't find sound/misc/talk.wav"), and complete signon.  A second
#      client launched with "-game <mod>" -- which names the mod before any
#      HexenWorld connection, without hw beneath it -- must get the same hw
#      mount.
#   7. Missing world: the same server with allow_download_maps 0.  A fresh
#      client must disconnect with the "could not be found or downloaded"
#      message instead of signing on with no world, and must not freeze.
#
# Requires:
#   - Hexen II data1 (pak0/pak1) under --basedir for phase 0; without it the
#     script prints SKIP and exits 77.
#   - A HexenWorld gamedir (hw/pak4.pak, hw/hwprogs.dat) for phases 1-7.
#     Registered data cannot ship in CI, so this runs on machines/installs
#     where it exists; CI keeps its data-less hwsv boot smoke.  Without it,
#     phase 0 still runs and the script exits 77 (skip) if phase 0 passed.
#   - Xvfb for the GL clients (pass --xvfb if it is not on PATH), or
#     --offscreen to use SDL's offscreen video driver instead; --client /
#     --server for the binaries.  Prefer Xvfb: phase 2 runs two clients at
#     once, and SDL's offscreen driver gets one EGL surface per device on at
#     least the NVIDIA driver -- the second client dies with "Couldn't create
#     gl context: ... EGL_BAD_ALLOC" and phases 2-4 all fail.  --offscreen is
#     fine for phase 0 and, with --skip-base, for phases 6-7, which never run
#     two clients at the same time.
#   - Python 3 for the false-H2-accept UDP fixture.
#   - coreutils od/dd (the pak reader), util-linux script (the server pty).
#
# Usage:
#   hw-smoke.sh --basedir DIR [--client BIN] [--server BIN] [--xvfb BIN]
#                [--offscreen] [--display :99] [--map hwdm1]
#                [--second-map hwdm2] [--mod-source-map hwdm1] [--skip-base]
#
# Exit status 0 on pass, 1 on any failed assertion, 2 on bad usage, 77 when
# the data for a phase is not available (skip).
#

set -u

BASEDIR=""
CLIENT=""
SERVER=""
XVFB=""
OFFSCREEN=0
DISPNUM=":99"
MAP="hwdm1"
SECOND_MAP="hwdm2"
MOD_SOURCE_MAP=""
SKIP_BASE=0
MAX_WAIT=180

# Names nothing ships with, so the client cannot already have them.
MOD="hwsmkmod"
MODMAP="hwsmkdl"
MOD_PORT=26960
# Nothing listens here; the discard port.
DEAD_PORT=9

usage() {
	sed -n '/^# Usage:/,/(skip)\.$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
	--basedir) BASEDIR="$2"; shift 2 ;;
	--client) CLIENT="$2"; shift 2 ;;
	--server) SERVER="$2"; shift 2 ;;
	--xvfb) XVFB="$2"; shift 2 ;;
	--offscreen) OFFSCREEN=1; shift ;;
	--display) DISPNUM="$2"; shift 2 ;;
	--map) MAP="$2"; shift 2 ;;
	--second-map) SECOND_MAP="$2"; shift 2 ;;
	--mod-source-map) MOD_SOURCE_MAP="$2"; shift 2 ;;
	--skip-base) SKIP_BASE=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) echo "hw-smoke: unknown option $1" >&2; usage >&2; exit 2 ;;
	esac
done
[ -n "$MOD_SOURCE_MAP" ] || MOD_SOURCE_MAP="$MAP"

[ -n "$BASEDIR" ] || { echo "hw-smoke: --basedir is required" >&2; usage >&2; exit 2; }
if [ ! -d "$BASEDIR/data1" ]; then
	echo "hw-smoke: SKIP: no Hexen II data under $BASEDIR (need data1/)."
	exit 77
fi
BASEDIR=$(cd "$BASEDIR" && pwd)
HW_DATA=0
[ -f "$BASEDIR/hw/pak4.pak" ] && HW_DATA=1
if [ "$HW_DATA" -eq 1 ] && [ -e "$BASEDIR/$MOD" ]; then
	echo "hw-smoke: $BASEDIR/$MOD exists; the download test needs a client without it" >&2
	exit 2
fi

command -v "$CLIENT" >/dev/null 2>&1 || CLIENT="${CLIENT:-glhexen2}"
command -v "$SERVER" >/dev/null 2>&1 || SERVER="${SERVER:-hwsv}"
command -v "$XVFB" >/dev/null 2>&1 || XVFB="${XVFB:-Xvfb}"

NEED="$CLIENT"
[ "$OFFSCREEN" -eq 1 ] || NEED="$NEED $XVFB"
[ "$HW_DATA" -eq 1 ] && NEED="$NEED $SERVER od dd script"
for bin in $NEED; do
	command -v "$bin" >/dev/null 2>&1 || { echo "hw-smoke: cannot find $bin" >&2; exit 2; }
done

WORK=$(mktemp -d /tmp/hw-smoke.XXXXXX)
SLOG="$WORK/server.log"
ALOG="$WORK/client-a.log"
BLOG="$WORK/client-b.log"
MSLOG="$WORK/server-mod.log"
CLOG="$WORK/client-download.log"
ELOG="$WORK/client-gamearg.log"
DLOG="$WORK/client-noworld.log"
PLOG="$WORK/client-deadport.log"
FIFO="$WORK/console"
MFIFO="$WORK/console-mod"
FAILURES=0
# Initialised before the trap: cleanup runs under `set -u`, so an early exit
# (Xvfb refused to start, an unusable basedir) must not trip over an unset PID.
SRV_PID=""; CA_PID=""; CB_PID=""; CP_PID=""; SIEGE_PID=""; XV_PID=""; FAKE_PID=""
MSRV_PID=""; CC_PID=""; CD_PID=""; CE_PID=""
LAST_PID=""

cleanup() {
	for pid in "$SRV_PID" "$CA_PID" "$CB_PID" "$CP_PID" "$SIEGE_PID" \
			"$FAKE_PID" "$MSRV_PID" "$CC_PID" "$CD_PID" "$CE_PID" "$XV_PID"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	exec 3>&- 4>&-
	rm -rf "$WORK"
}
trap cleanup EXIT

say() { echo "hw-smoke: $*"; }
fail() { say "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

# --- launch_client LOG ARGS... : start a GL client in the background ---
launch_client() {
	local log="$1"
	shift
	if [ "$OFFSCREEN" -eq 1 ]; then
		SDL_VIDEODRIVER=offscreen stdbuf -oL -eL "$CLIENT" -window "$@" > "$log" 2>&1 &
	else
		DISPLAY="$DISPNUM" stdbuf -oL -eL "$CLIENT" "$@" > "$log" 2>&1 &
	fi
	LAST_PID=$!
}

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

# --- absent FILE GREPEXPR MESSAGE : fail when the pattern IS in the log ---
absent() {
	grep -qE "$2" "$1" 2>/dev/null && fail "$3"
}

# --- pak_extract PAK MEMBER OUT : copy one member out of a Quake/Hexen II pak ---
# Layout: "PACK", int32 dirofs, int32 dirlen; dirlen/64 entries of
# name[56] int32 filepos int32 filelen, all little-endian.  od reads host
# order, which is what every box this runs on (x86, arm64) uses.
pak_extract() {
	local pak="$1" want="$2" out="$3" dirofs dirlen count i name ofs len
	local dir="$WORK/pakdir.bin"

	[ "$(head -c 4 "$pak")" = "PACK" ] || { say "$pak is not a pak"; return 1; }
	read -r dirofs dirlen < <(od -An -v -t u4 -j 4 -N 8 "$pak")
	count=$((dirlen / 64))
	dd if="$pak" of="$dir" iflag=skip_bytes,count_bytes skip="$dirofs" \
		count="$dirlen" bs=65536 2>/dev/null || return 1
	for ((i = 0; i < count; i++)); do
		name=$(dd if="$dir" bs=64 skip="$i" count=1 2>/dev/null | head -c 56 | tr -d '\000')
		[ "$name" = "$want" ] || continue
		read -r ofs len < <(od -An -v -t u4 -j $((i * 64 + 56)) -N 8 "$dir")
		mkdir -p "$(dirname "$out")"
		dd if="$pak" of="$out" iflag=skip_bytes,count_bytes skip="$ofs" \
			count="$len" bs=65536 2>/dev/null || return 1
		[ "$(stat -c %s "$out")" = "$len" ] || { say "short extract of $want"; return 1; }
		return 0
	done
	say "$want not found in $pak"
	return 1
}

say "workdir: $WORK"
say "client : $CLIENT"
[ "$HW_DATA" -eq 1 ] && say "server : $SERVER"

# --- Xvfb ---
if [ "$OFFSCREEN" -eq 0 ]; then
	"$XVFB" "$DISPNUM" -screen 0 800x600x24 -nolisten tcp > "$WORK/xvfb.log" 2>&1 &
	XV_PID=$!
	sleep 1.5
fi

# ============================================================================
# Phase 0: the loading plaque on a connect that is abandoned before any reply.
# ============================================================================
# "connect" raises the plaque (Host_Connect_f -> Host_Reconnect_f); the
# "disconnect" runs in the same command-buffer pass, long before HWCL_Frame's
# 5 second retry, and afterwards the client is disconnected so no retry ever
# fires.  No server means no download.  HWCL_Disconnect is the only drop.
launch_client "$PLOG" -basedir "$BASEDIR" -userdir "$WORK/user-deadport" -nolan \
	-hwport 27005 +developer 1 +connect "hw://127.0.0.1:$DEAD_PORT" +disconnect
CP_PID=$LAST_PID
if ! wait_for "$PLOG" 'Connecting to HexenWorld server' 60; then
	fail "dead-port client never attempted the HexenWorld connect"
else
	sleep 32	# the plaque's timeout is 25 seconds after it was raised
	absent "$PLOG" 'load timeout\.' \
		"loading plaque survived disconnecting a pending HexenWorld connect"
	kill -0 "$CP_PID" 2>/dev/null || fail "dead-port client exited"
fi
kill "$CP_PID" 2>/dev/null; wait "$CP_PID" 2>/dev/null; CP_PID=""

if [ "$HW_DATA" -eq 0 ]; then
	if [ "$FAILURES" -eq 0 ]; then
		say "PASS: phase 0 (plaque on abandoned connect)."
		say "SKIP: phases 1-6 need HexenWorld data ($BASEDIR/hw/pak4.pak)."
		exit 77
	fi
	say "FAILED: $FAILURES assertion(s)."
	say "$(basename "$PLOG") tail:"; tail -12 "$PLOG"
	exit 1
fi

if [ "$SKIP_BASE" -eq 0 ]; then
# --- hwsv on a pty so the console accepts commands ---
mkfifo "$FIFO"
exec 3<>"$FIFO"		# keep a writer open so the reader never sees EOF

script -qefc "$SERVER -basedir $BASEDIR -game hw +map $MAP" "$SLOG" < "$FIFO" >/dev/null 2>&1 &
SRV_PID=$!

wait_for "$SLOG" 'Building PHS' || { tail -40 "$SLOG"; fail "server did not reach map spawn"; }

# A valid H2 control reply is only a provisional protocol match. Reproduce the
# field failure by accepting on H2's default port without ever sending signon.
cat > "$WORK/false-h2-accept.py" <<'PY'
import socket
import struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 26900))
while True:
    request, address = sock.recvfrom(8192)
    if len(request) >= 5 and request[4] == 1:  # CCREQ_CONNECT
        payload = bytes([0x81]) + struct.pack("<I", 26900)  # CCREP_ACCEPT
        header = struct.pack(">I", 0x80000000 | (4 + len(payload)))
        sock.sendto(header + payload, address)
PY
python3 "$WORK/false-h2-accept.py" > "$WORK/false-h2-accept.log" 2>&1 &
FAKE_PID=$!
sleep 1

# --- client A ---
# Plain address, not hw://: client A is the autodetect path, and the false-H2
# fixture above only proves anything if the client had to choose.  No -nolan
# either, for the same reason: it makes NET_Datagram_Init bail (net_dgrm.c),
# so there is no Hexen II attempt left to reject and every phase-2 assertion
# on client A fails.
launch_client "$ALOG" -basedir "$BASEDIR" -hwport 27001 +developer 1 \
	+connect "127.0.0.1"
CA_PID=$LAST_PID

wait_for "$ALOG" 'HexenWorld signon complete' || { fail "client A did not complete signon"; }
wait_for "$ALOG" 'HexenWorld server set the gamedir to hw' || fail "client A did not mount the hw gamedir"
wait_for "$ALOG" 'player 0 spawned' || fail "client A never saw its own spawn"
# A control-packet acceptance without real H2 traffic must remain provisional.
wait_for "$ALOG" 'Hexen II accepted but sent no signon data; trying HexenWorld' || \
	fail "client A committed to a false Hexen II acceptance"
kill "$FAKE_PID" 2>/dev/null; wait "$FAKE_PID" 2>/dev/null; FAKE_PID=""
# Signon has to dismiss the loading plaque and actually draw, or the player sits
# behind a frozen loading screen (the reported issue #50 failure).
wait_for "$ALOG" 'First world draw completed' || fail "client A never rendered the world"
grep -q 'HexenWorld missing world map' "$ALOG" && fail "client A reported a missing world map"
no_badness "$ALOG" || fail "client A hit a protocol abort"

# --- client B ---
sleep 2
launch_client "$BLOG" -basedir "$BASEDIR" -nolan -hwport 27002 +developer 1 \
	+connect "hw://127.0.0.1"
CB_PID=$LAST_PID

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
wait_count "$ALOG" 'HexenWorld signon complete' 2 180 || fail "client A did not complete a second signon (saw $(grep -c 'HexenWorld signon complete' "$ALOG" 2>/dev/null))"
wait_count "$BLOG" 'HexenWorld signon complete' 2 180 || fail "client B did not complete a second signon (saw $(grep -c 'HexenWorld signon complete' "$BLOG" 2>/dev/null))"

# statics: either the map has some (and they are parsed) or none; overflow is
# always a failure.
grep -q 'Too many HexenWorld static entities' "$ALOG" && fail "static entity list overflowed"
grep -q 'Too many HexenWorld static entities' "$BLOG" && fail "static entity list overflowed"
nsa=$(grep -c 'HexenWorld static ' "$ALOG" 2>/dev/null || true)
nsb=$(grep -c 'HexenWorld static ' "$BLOG" 2>/dev/null || true)
say "static records parsed: A=$((nsa + 0)) B=$((nsb + 0))"

no_badness "$ALOG" || fail "client A aborted during/after the map change"
no_badness "$BLOG" || fail "client B aborted during/after the map change"
# Both clients have been up well past the plaque's 25 second timeout by now,
# so a plaque that signon never dropped has printed this.
absent "$ALOG" 'load timeout\.' "client A's loading plaque was never dropped (load timeout)"
absent "$BLOG" 'load timeout\.' "client B's loading plaque was never dropped (load timeout)"

# --- optional Siege leg (issue #50) ---
# Siege is an HW mod, not a third protocol: the server advertises gamedir
# "siege" and the client must layer siege/ above the hw/ base.  Only runs when
# the mod is actually installed, since the base hw install has no siege.bsp.
if [ -d "$BASEDIR/siege" ] && [ -f "$BASEDIR/siege/maps/siege.bsp" ]; then
	# Not CLOG: the mod/download phase below owns that name.
	SIEGELOG="$WORK/client-siege.log"
	SFIFO="$WORK/console-siege"
	mkfifo "$SFIFO"
	exec 4<>"$SFIFO"
	script -qefc "$SERVER -basedir $BASEDIR -userdir $WORK/usr-siege -game siege -port 27003 +map siege" \
		"$WORK/server-siege.log" < "$SFIFO" >/dev/null 2>&1 &
	SIEGE_PID=$!

	wait_for "$WORK/server-siege.log" 'Building PHS' || fail "siege server did not spawn its map"

	launch_client "$SIEGELOG" -basedir "$BASEDIR" -userdir "$WORK/user-siege" \
		-nolan -hwport 27004 +developer 1 +connect "127.0.0.1:27003"
	CP_PID=$LAST_PID

	wait_for "$SIEGELOG" 'HexenWorld server set the gamedir to siege' 120 || fail "siege client did not mount the siege gamedir"
	wait_for "$SIEGELOG" 'HexenWorld signon complete: siege' 180 || fail "siege client did not complete signon"
	wait_for "$SIEGELOG" 'First world draw completed' 120 || fail "siege client never rendered the world"
	absent "$SIEGELOG" 'could not be found or downloaded' "siege client reported a missing world map"
	no_badness "$SIEGELOG" || fail "siege client hit a protocol abort"

	kill "$CP_PID" 2>/dev/null; wait "$CP_PID" 2>/dev/null; CP_PID=""

	# The same server against a client install with data1+hw but no Siege map.
	# This used to abort at signon; with downloads wired up, siege.bsp is a
	# loose file on the server and allow_download_maps defaults to 1, so the
	# client must now fetch it and sign on.  Real mod data, unlike the
	# synthetic gamedir the download phase below builds.  Symlinks avoid
	# copying proprietary data into the test workdir; the fresh userdir keeps
	# the download out of the developer's real ~/.hexen2.
	MISSING_BASE="$WORK/missing-base"
	MLOG="$WORK/client-missing-siege.log"
	mkdir -p "$MISSING_BASE/siege"
	ln -s "$BASEDIR/data1" "$MISSING_BASE/data1"
	ln -s "$BASEDIR/hw" "$MISSING_BASE/hw"
	launch_client "$MLOG" -basedir "$MISSING_BASE" -userdir "$WORK/user-missing-siege" \
		-nolan -hwport 27005 +developer 1 +connect "127.0.0.1:27003"
	CP_PID=$LAST_PID
	wait_for "$MLOG" 'Downloading maps/siege\.bsp' 180 || \
		fail "missing-Siege client never requested the map it lacks"
	wait_for "$MLOG" 'HexenWorld signon complete: siege' 240 || \
		fail "missing-Siege client did not sign on after downloading the map"
	# hw lives in its own basedir here, so nothing under hw/ may be fetched.
	absent "$MLOG" "Downloading sound/" "missing-Siege client downloaded an hw asset (hw not mounted)"
	absent "$MLOG" 'could not be found or downloaded' "missing-Siege client gave up on the world"
	no_badness "$MLOG" || fail "missing-Siege client hit a protocol abort"
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
fi	# SKIP_BASE

# ============================================================================
# Mod gamedir + map download, then the missing-world refusal.
# ============================================================================

SRVBASE="$WORK/srvbase"
mkdir -p "$SRVBASE/$MOD/maps" "$WORK/srvuser"
ln -s "$BASEDIR/data1" "$SRVBASE/data1"
ln -s "$BASEDIR/hw" "$SRVBASE/hw"
[ -d "$BASEDIR/portals" ] && ln -s "$BASEDIR/portals" "$SRVBASE/portals"
MAPFILE="$SRVBASE/$MOD/maps/$MODMAP.bsp"

if ! pak_extract "$BASEDIR/hw/pak4.pak" "maps/$MOD_SOURCE_MAP.bsp" "$MAPFILE"; then
	fail "could not extract maps/$MOD_SOURCE_MAP.bsp from hw/pak4.pak for the download test"
else
	say "mod map: $MAPFILE ($(stat -c %s "$MAPFILE") bytes)"

	mkfifo "$MFIFO"
	exec 4<>"$MFIFO"
	script -qefc "$SERVER -basedir $SRVBASE -userdir $WORK/srvuser -port $MOD_PORT -game $MOD +map $MODMAP" \
		"$MSLOG" < "$MFIFO" >/dev/null 2>&1 &
	MSRV_PID=$!

	if ! wait_for "$MSLOG" 'Building PHS'; then
		tail -40 "$MSLOG"
		fail "mod server did not reach map spawn"
	else
		# --- client C: fresh userdir, must download the map ---
		launch_client "$CLOG" -basedir "$BASEDIR" -userdir "$WORK/user-dl" \
			-nolan -hwport 27010 +developer 1 +connect "hw://127.0.0.1:$MOD_PORT"
		CC_PID=$LAST_PID

		wait_for "$CLOG" "HexenWorld server set the gamedir to $MOD" 90 \
			|| fail "download client did not mount the $MOD gamedir"
		wait_for "$CLOG" "Downloading maps/$MODMAP\.bsp" 120 \
			|| fail "download client never started downloading maps/$MODMAP.bsp"
		# 1024-byte blocks, one round trip each: a few MB takes a while.
		wait_for "$CLOG" 'HexenWorld signon complete' 600 \
			|| fail "download client did not complete signon"
		absent "$CLOG" 'world model unavailable|could not be found or downloaded' \
			"download client reported the world model missing"
		absent "$CLOG" "can't find sound/misc/talk\.wav" \
			"hw is not mounted beneath the mod (talk.wav missing)"
		absent "$CLOG" 'Downloading (sound|models|gfx)/' \
			"download client fetched hw assets, so hw is not beneath the mod"
		no_badness "$CLOG" || fail "download client hit a protocol abort"

		GOT="$WORK/user-dl/$MOD/maps/$MODMAP.bsp"
		if [ ! -f "$GOT" ]; then
			fail "downloaded map not at $GOT"
		elif ! cmp -s "$GOT" "$MAPFILE"; then
			fail "downloaded map differs from the server's copy"
		fi
		[ -e "$GOT.tmp" ] && fail "download left $GOT.tmp behind"

		sleep 27	# outlive the plaque's 25 second timeout
		absent "$CLOG" 'load timeout\.' "download client's loading plaque was never dropped (load timeout)"

		kill "$CC_PID" 2>/dev/null; wait "$CC_PID" 2>/dev/null; CC_PID=""

		# --- client E: "-game <mod>", same userdir so the map is present ---
		# FS_Init mounts the mod on data1 before any connection exists and
		# the server's gamedir then has the same name; hw must still end up
		# beneath it.  With the map already downloaded, any download here is
		# an hw asset the client failed to find.
		launch_client "$ELOG" -basedir "$BASEDIR" -userdir "$WORK/user-dl" -game "$MOD" \
			-nolan -hwport 27011 +developer 1 +connect "hw://127.0.0.1:$MOD_PORT"
		CE_PID=$LAST_PID

		wait_for "$ELOG" 'HexenWorld signon complete' 300 \
			|| fail "-game client did not complete signon"
		# Player skins are the exception: HWCL_SkinNextDownload asks for
		# skins/<name>.pcx during signon (8ce93dce8), and no pak ships a
		# skins/ directory, so every client requests base.pcx whatever is
		# mounted.  Any other download is an hw asset it failed to find.
		# Captured, not "grep -qv": ugrep, grep on some boxes, gets -qv wrong.
		if [ -n "$(grep -E '^Downloading ' "$ELOG" 2>/dev/null | grep -v '^Downloading skins/')" ]; then
			fail "-game client downloaded files, so hw is not beneath the mod"
		fi
		absent "$ELOG" "can't find sound/misc/talk\.wav" \
			"-game client: hw is not mounted beneath the mod (talk.wav missing)"
		absent "$ELOG" 'world model unavailable|could not be found or downloaded' \
			"-game client reported the world model missing"
		no_badness "$ELOG" || fail "-game client hit a protocol abort"

		kill "$CE_PID" 2>/dev/null; wait "$CE_PID" 2>/dev/null; CE_PID=""

		# --- client D: fresh userdir, server refuses the map ---
		printf 'allow_download_maps 0\n' >&4
		sleep 1
		launch_client "$DLOG" -basedir "$BASEDIR" -userdir "$WORK/user-nodl" \
			-nolan -hwport 27012 +developer 1 +connect "hw://127.0.0.1:$MOD_PORT"
		CD_PID=$LAST_PID

		wait_for "$DLOG" "could not be found or downloaded \(gamedir $MOD\)" 180 \
			|| fail "missing-world client did not disconnect with the missing-map message"
		wait_for "$DLOG" "Server cannot send maps/$MODMAP\.bsp" 5 \
			|| fail "missing-world client did not see the server refuse the map"
		sleep 27	# a hang or frozen plaque shows up past the 25 second mark
		absent "$DLOG" 'HexenWorld signon complete|requesting signon' \
			"missing-world client continued signon with no world"
		absent "$DLOG" 'load timeout\.' "missing-world client froze on the loading plaque"
		[ -e "$WORK/user-nodl/$MOD/maps/$MODMAP.bsp" ] && fail "refused map was written anyway"
		[ -e "$WORK/user-nodl/$MOD/maps/$MODMAP.bsp.tmp" ] && fail "refused map left a .tmp behind"
		kill -0 "$CD_PID" 2>/dev/null || fail "missing-world client exited (crash?) instead of returning to the console"
		no_badness "$DLOG" || fail "missing-world client hit a protocol abort"

		kill "$CD_PID" 2>/dev/null; wait "$CD_PID" 2>/dev/null; CD_PID=""
	fi

	printf 'quit\n' >&4
	sleep 2
	wait "$MSRV_PID" 2>/dev/null; MSRV_PID=""
fi

if [ "$FAILURES" -eq 0 ]; then
	say "PASS: HexenWorld smoke (autodetect, 2 clients, broadcast, map change, optional Siege, mod download, -game mod, missing world)."
	exit 0
fi
say "FAILED: $FAILURES assertion(s).  Logs: $WORK (removed on exit)."
for log in "$PLOG" "$ALOG" "$BLOG" "$SLOG" "$CLOG" "$ELOG" "$DLOG" "$MSLOG" \
		"${SIEGELOG:-}" "${MLOG:-}"; do
	[ -f "$log" ] || continue
	say "$(basename "$log") tail:"; tail -8 "$log"
done
exit 1
