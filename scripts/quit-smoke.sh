#!/usr/bin/env bash
#
# quit-smoke.sh -- headless proof that `quit` actually quits.
#
# The regression this guards (GitHub #204): the confirmation prompt used to be
# raised from Host_Quit_f, for every `quit` issued while connected with the
# console closed.  That is not only the player pressing a bound key -- it is
# also `quit` in a cfg, in an alias, or on the command line, which is how every
# headless harness ends its run.  A headless engine cannot answer that prompt:
# no key events arrive, and the console cannot be opened to retry, because with
# a connection live the menu eats the grave key.  So the engine kept rendering
# frames forever and every harness run ended on its own timeout.
#
# WHAT IT LOOKED LIKE IS WHY IT WAS MIS-DIAGNOSED.  The run's last two console
# lines were "Sending clc_disconnect" and "Client player removed", which reads
# as a shutdown that got stuck half way.  Those lines are printed by the SIGTERM
# the harness timeout sends: SDL turns it into SDL_EVENT_QUIT, and in_sdl.c
# answers that with CL_Disconnect + Sys_Quit.  They come AFTER the hang.  The
# shutdown never started; nothing was ever blocked.
#
# THE ASSERTION IS THE EXIT STATUS, NOT A LOG LINE.  A hung engine still prints
# everything up to and including the `quit`, so grepping the log passes on a
# broken build.  The log is only used to prove the run got far enough to issue
# the quit at all, so that a slow map load cannot be mistaken for a pass.
#
# Needs no X and no retail data: SDL's offscreen video driver plus .#demodata.
# ~60 s.
#
# Usage:
#   nix build .#default   -o result
#   nix build .#demodata  -o result-demodata
#   ./scripts/quit-smoke.sh
#
# Environment: ENGINE   (default ./result/bin/glhexen2)
#              DEMODATA (default ./result-demodata/share/hexenwail)
#              LIMIT    seconds to allow for the whole run (default 120)
#              OUT      work dir (default a mktemp dir, kept on failure)

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENGINE=${ENGINE:-$ROOT/result/bin/glhexen2}
DEMODATA=${DEMODATA:-$ROOT/result-demodata/share/hexenwail}
LIMIT=${LIMIT:-120}
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/quit-smoke.XXXXXX")}

# ./result is a symlink into the read-only store; resolve both the way the other
# drivers here do, so a caller passing `-o` output links gets the same result.
ENGINE=$(readlink -f "$ENGINE")
DEMODATA=$(readlink -f "$DEMODATA")

[ -x "$ENGINE" ]   || { echo "quit-smoke: no engine at $ENGINE (set ENGINE=)" >&2; exit 3; }
[ -d "$DEMODATA" ] || { echo "quit-smoke: no game data at $DEMODATA (set DEMODATA=)" >&2; exit 3; }

USERDIR=$OUT/userdir
mkdir -p "$USERDIR/data1"

# `wait` is one frame and there is no sleep in the console, so the w5/w25/w125
# ladder is how a cfg gets a multi-second delay.  375 frames is ~5 s at the
# default 72 Hz, long after the map has spawned the player.
cat > "$USERDIR/data1/quit-smoke.cfg" <<'EOF'
developer 1
map demo1
alias w5 "wait;wait;wait;wait;wait"
alias w25 "w5;w5;w5;w5;w5"
alias w125 "w25;w25;w25;w25;w25"
alias bye "echo QUITSMOKE_ISSUING_QUIT; quit"
alias go "w125; w125; w125; bye"
go
EOF

LOG=$USERDIR/qconsole.log

echo "quit-smoke: $ENGINE (limit ${LIMIT}s, work dir $OUT)"
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
timeout -k 5 "$LIMIT" \
  "$ENGINE" -basedir "$DEMODATA" -userdir "$USERDIR" \
            -width 320 -height 240 -window -nosound -condebug \
            +exec quit-smoke.cfg >"$OUT/engine.stdout" 2>&1
rc=$?

if ! grep -q QUITSMOKE_ISSUING_QUIT "$LOG" 2>/dev/null; then
	echo "quit-smoke: FAIL -- the run never reached the quit." >&2
	echo "            Raise LIMIT if the map load is just slow here." >&2
	tail -20 "$LOG" 2>/dev/null >&2
	echo "            work dir kept: $OUT" >&2
	exit 1
fi

if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	echo "quit-smoke: FAIL -- engine still running ${LIMIT}s after \`quit\` (#204)." >&2
	tail -5 "$LOG" >&2
	echo "            work dir kept: $OUT" >&2
	exit 1
fi

if [ "$rc" -ne 0 ]; then
	echo "quit-smoke: FAIL -- engine exited $rc, expected 0." >&2
	tail -20 "$LOG" >&2
	echo "            work dir kept: $OUT" >&2
	exit 1
fi

rm -rf "$OUT"
echo "quit-smoke: PASS -- \`quit\` from a script exits cleanly (status 0)."
