#!/usr/bin/env bash
#
# ride-lift.sh -- drive a Hexen II pusher (func_plat, func_train, func_door)
# with a player standing on it, headless, and print the player's z over time.
#
#   ./tools/ride-lift.sh <outdir> <elevators> <map> <x> <y> <z> [basedir]
#
# <elevators> is the value for sv_gameplayfix_elevators (0 = off, 1 = players
# only, 2 = all entities).  <x> <y> <z> is where to drop the player: put it a
# little ABOVE the pusher's resting surface and let gravity settle it.
#
# WHY THIS EXISTS.  "Does a rider stall on a lift" reads like something only a
# human at a keyboard can answer, and uhexen2-a5nn.7 sat blocked on exactly
# that.  It is not a look, it is a coordinate: if the rider stalls, its z stops
# tracking the platform's.  This turns the question into a number, so the
# elevators fix can be regression-tested instead of re-eyeballed.
#
# NO X SERVER IS INVOLVED -- SDL offscreen plus llvmpipe.  Unlike
# headless-drive.sh this needs neither Xvfb nor xdotool, only bwrap, because
# it drives the engine through Cmd_StartupScript ('+arg' runs 'arg' as a
# console command) rather than through synthesized key events.
#
# Two things that will waste your time if you write your own:
#   - '+setpos 888 -1168 -50' SETS NOTHING.  Cmd_StartupScript stops collecting
#     arguments at the first token starting with '-', so every negative
#     coordinate silently truncates the command.  Everything positional goes
#     through the generated lift.cfg below for that reason.
#   - 'quit' while connected opens the confirmation MENU and the run hangs to
#     your timeout.  The cfg disconnects first.
#
# Finding a pusher to stand on, given only .bsp files: a brush entity's
# position is in its submodel's bbox (LUMP_MODELS, index 14), not in an origin
# key -- brush ents normally have origin "0 0 0".  An untargeted func_plat
# spawns at its BOTTOM (pos2_z = origin_z - height, or - size_z + 8 when there
# is no height key), so aim at the bbox top MINUS that travel.  One carrying a
# targetname spawns at the TOP and does nothing until triggered -- do not
# bother teleporting under it.
#
# Requires: bwrap.  Run it as
#   nix shell nixpkgs#bubblewrap --command ./tools/ride-lift.sh ...
set -u

[ $# -ge 6 ] || { sed -n '3,8p' "$0" >&2; exit 2; }
OUT=$1; EV=$2; MAP=$3; PX=$4; PY=$5; PZ=$6
BASEDIR=${7:-${BASEDIR:-$HOME/hexen2}}
ENGINE=${ENGINE:-./result/bin/glhexen2}
SAMPLES=${SAMPLES:-60}          # viewpos samples
STRIDE=${STRIDE:-4}             # frames between samples (host_framerate 0.05)
TIMEOUT=${TIMEOUT:-300}

command -v bwrap >/dev/null 2>&1 || {
  echo "ride-lift: missing bwrap -- run under 'nix shell nixpkgs#bubblewrap --command'" >&2
  exit 3; }

# bwrap cannot --ro-bind through a symlink into the read-only store, and
# ./result is exactly such a symlink.  Same trap headless-drive.sh documents.
ENGINE=$(readlink -f "$ENGINE") || exit 3
BASEDIR=$(readlink -f "$BASEDIR") || exit 3
[ -x "$ENGINE" ]  || { echo "ride-lift: no engine at $ENGINE (set ENGINE=)" >&2; exit 3; }
[ -d "$BASEDIR" ] || { echo "ride-lift: no basedir at $BASEDIR" >&2; exit 3; }
EDIR=$(dirname "$ENGINE")

rm -rf "$OUT"
mkdir -p "$OUT/home/.local/share/hexen2/data1"

{
  echo "developer 1"
  # Pin the frame time so two runs are comparable sample-for-sample; without
  # it the traces drift apart on timing jitter alone and every diff is noise.
  echo "host_framerate 0.05"
  echo "sv_gameplayfix_elevators $EV"
  echo "setpos $PX $PY $PZ"
  i=0
  while [ $i -lt "$SAMPLES" ]; do
    j=0; while [ $j -lt "$STRIDE" ]; do echo "wait"; j=$((j+1)); done
    echo "viewpos"
    i=$((i+1))
  done
  echo "disconnect"; echo "wait"; echo "quit"
} > "$OUT/home/.local/share/hexen2/data1/lift.cfg"

# ~40 waits is enough for a map to finish connecting; ~250 overflows Cbuf.
CONNECT=""
i=0; while [ $i -lt 40 ]; do CONNECT="$CONNECT +wait"; i=$((i+1)); done

# shellcheck disable=SC2086
timeout -k 5 "$TIMEOUT" \
bwrap --dev-bind / / --die-with-parent \
      --bind "$OUT/home" "$HOME" \
      --ro-bind "$BASEDIR" "$BASEDIR" \
      --ro-bind "$EDIR" "$EDIR" \
  env HOME="$HOME" XDG_DATA_HOME="$HOME/.local/share" \
      SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  "$ENGINE" -basedir "$BASEDIR" -width 320 -height 240 -window -nosound \
      +map "$MAP" $CONNECT +exec lift.cfg \
  >"$OUT/engine.stdout" 2>&1
rc=$?

# The engine always writes this; it is the only output on a non-tty stdout.
cp "$OUT/home/.local/share/hexen2/qconsole.log" "$OUT/qconsole.log" 2>/dev/null

printf 'map=%s elevators=%s rc=%s\n' "$MAP" "$EV" "$rc"
# grep -c prints 0 AND exits 1 when it finds nothing, so this must not have an
# `|| echo 0` fallback -- that reports the count twice.
NUDGES=$(grep -c 'nudged' "$OUT/qconsole.log" 2>/dev/null) || true
printf 'nudges=%s   (sv_gameplayfix_elevators fired this many times)\n' "${NUDGES:-0}"
printf 'z: '
grep -o 'Viewpos: ([^)]*)' "$OUT/qconsole.log" 2>/dev/null \
  | awk '{print $4}' | tr -d ')' | tr '\n' ' '
echo
[ "$rc" = 0 ] || echo "see $OUT/engine.stdout" >&2
exit $rc
