#!/usr/bin/env bash
#
# demo-playback-smoke.sh -- headless proof that a demo plays with no qsocket.
#
# The regression this guards (GitHub #209, introduced by 30ea43b17): a guard
# written for HexenWorld's socket ownership returned early from
# CL_ReadFromServer whenever cls.netcon was NULL.  A demo has no qsocket, so
# every demo froze on the loading plaque until the 25 second "load timeout".
# Mods that drive their whole front end from `startdemos` -- Storm of the
# Thyrion, Shadows of Chaos -- came up with no menu at all, which is how it was
# reported: "mod loading broken".
#
# TWO PHASES, AND THE SECOND ONE IS THE TEST.  A demo played back in the same
# session that recorded it does NOT reproduce the bug: `record <name> <map>`
# spins up a listen server, so cls.netcon is still set when playback starts and
# the guard lets it through.  Measured on the broken build: the single-session
# record-then-playdemo run reaches the end of the demo and looks green.  The
# failure needs a demo played with no connection ever made -- which is exactly
# what `startdemos` at boot does, and what a mod's autoexec.cfg does.
#
#   phase 1  record a demo of demo1, copy the .dem out of the sandbox
#   phase 2  boot a fresh engine straight into `playdemo`, no connection first
#
# THE VERDICT IS A LOG LINE, NOT A SCREENSHOT.  CL_PlayDemo_f execs
# "<name>start.cfg" when playback begins and CL_StopPlayback execs
# "<name>end.cfg" when it finishes, so only the *end* line proves the demo
# advanced.  A frozen client still prints "Playing demo from ..." and the
# start.cfg line, so grepping for either passes on a broken engine.
#
# Needs no retail data and no .dem in the tree: .#demodata is enough, because
# phase 1 makes the demo.  ~2 minutes.
#
# Usage:
#   nix build .#default   -o result
#   nix build .#demodata  -o result-demodata
#   nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
#             nixpkgs#bubblewrap --command ./scripts/demo-playback-smoke.sh
#
# Environment: ENGINE (default ./result/bin/glhexen2),
#              DEMODATA (default ./result-demodata/share/hexenwail),
#              OUT (default a mktemp dir, kept on failure).

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENGINE=${ENGINE:-$ROOT/result/bin/glhexen2}
DEMODATA=${DEMODATA:-$ROOT/result-demodata/share/hexenwail}
DRIVE=$ROOT/tools/headless-drive.sh
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/demo-playback.XXXXXX")}

DEMO=smoketest

fail() { echo "demo-playback-smoke: FAIL -- $*" >&2; echo "artifacts in $OUT" >&2; exit 1; }

for dep in Xvfb xdotool bwrap import; do
	command -v "$dep" >/dev/null || fail "missing required tool: $dep (see the nix shell line in this file's header)"
done
[ -x "$ENGINE" ] || fail "no engine at $ENGINE -- nix build .#default -o result"
[ -d "$DEMODATA/data1" ] || fail "no demo data at $DEMODATA -- nix build .#demodata -o result-demodata"

mkdir -p "$OUT"

# ---------------------------------------------------------------- phase 1 ---
# Record a demo of demo1.  `record <name> <map>` is the only form that works
# from the menu: plain `record <name>` while connected is refused with "Can not
# record - already connected to server".
cat > "$OUT/record.steps" <<EOF
sleep 6
cmd record $DEMO demo1
sleep 30
cmd stop
EOF

STEPS=$OUT/record.steps ENGINE=$ENGINE \
	"$DRIVE" script "$OUT/record" "$DEMODATA" >"$OUT/record.stdout" 2>&1 \
	|| fail "phase 1 driver exited nonzero; see $OUT/record.stdout"

grep -q "^recording to " "$OUT/record/qconsole.log" \
	|| fail "phase 1 never started recording; see $OUT/record/qconsole.log"

# headless-drive.sh wipes its sandboxed HOME on start and does not copy the
# userdir out, so lift the .dem before it is gone.
SRC=$(find "$OUT/record/work/home" -name "$DEMO.dem" -print -quit 2>/dev/null)
[ -n "$SRC" ] || fail "phase 1 recorded no $DEMO.dem under $OUT/record/work/home"

# ---------------------------------------------------------------- phase 2 ---
# A writable basedir, because the demo has to sit in data1 where a fresh engine
# finds it, and $DEMODATA is a read-only store path.
BASE=$OUT/base
mkdir -p "$BASE"
cp -rL "$DEMODATA/." "$BASE/"
chmod -R u+w "$BASE"
cp "$SRC" "$BASE/data1/$DEMO.dem"

# +playdemo goes through stuffcmds at the end of hexen.rc, the same path
# startdemos takes: nothing has connected, so cls.netcon is NULL.
printf 'sleep 45\nshot playback\n' > "$OUT/play.steps"

STEPS=$OUT/play.steps ENGINE=$ENGINE \
	"$DRIVE" script "$OUT/play" "$BASE" +playdemo "$DEMO" >"$OUT/play.stdout" 2>&1 \
	|| fail "phase 2 driver exited nonzero; see $OUT/play.stdout"

LOG=$OUT/play/qconsole.log
grep -q "Playing demo from $DEMO.dem" "$LOG" \
	|| fail "phase 2 never started the demo; see $LOG"

if grep -q "load timeout" "$LOG"; then
	fail "demo froze on the loading plaque -- CL_ReadFromServer returned early with no qsocket (see $LOG)"
fi

grep -q "couldn't exec ${DEMO}end.cfg" "$LOG" \
	|| fail "demo started but never reached its end -- playback is not advancing (see $LOG)"

echo "demo-playback-smoke: PASS -- $DEMO.dem played to completion from a cold boot, no qsocket."
rm -rf "$OUT"
