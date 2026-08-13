#!/usr/bin/env bash
#
# headless-drive.sh — Drive the engine's menus on a headless box.
#
# Runs the engine under Xvfb and sends *real X key events* with xdotool, so
# input arrives through SDL and Key_Event exactly as it would from a keyboard.
# Menu paths therefore actually execute; this is not a scripted-console
# simulation, and it is how uhexen2-uh5c's class-menu navigation was verified
# without a second machine.
#
# Usage:
#   headless-drive.sh <scenario> <outdir> <basedir> [extra engine args...]
#
# Environment:
#   ENGINE    path to the engine binary  (default: ./result/bin/glhexen2)
#   WORK      scratch dir for the sandboxed HOME (default: <outdir>/work)
#   DISPLAY_N X display number           (default: first free >= 99)
#   W, H      window size                (default: 800x600)
#
# Example:
#   nix build .#default
#   ./tools/headless-drive.sh noportals /tmp/out ~/hexen2
#
# Requires: Xvfb, xdotool, bwrap (bubblewrap), ImageMagick `import`.
#
# Scenarios below are the ones written for uhexen2-uh5c; they are examples.
# The reusable parts are the harness and the shot/key/keyn/typ helpers.
#
set -uo pipefail

usage() { sed -n '2,28p' "$0"; exit 2; }
[ $# -ge 3 ] || usage

SCEN="$1"; shift
OUT="$1"; shift
BASEDIR="$1"; shift

ENGINE=${ENGINE:-./result/bin/glhexen2}
W=${W:-800}; H=${H:-600}

for dep in Xvfb xdotool bwrap import; do
  command -v "$dep" >/dev/null 2>&1 || {
    echo "headless-drive: missing required tool: $dep" >&2; exit 3; }
done
[ -x "$ENGINE" ] || { echo "headless-drive: no engine at $ENGINE (set ENGINE=)" >&2; exit 3; }
[ -d "$BASEDIR" ] || { echo "headless-drive: no basedir at $BASEDIR" >&2; exit 3; }

ENGINE=$(cd "$(dirname "$ENGINE")" && pwd)/$(basename "$ENGINE")
BASEDIR=$(cd "$BASEDIR" && pwd)

rm -rf "$OUT"; mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=${WORK:-$OUT/work}
rm -rf "$WORK"; mkdir -p "$WORK/home"

# Pick a free display rather than assuming :99 is idle.
if [ -n "${DISPLAY_N:-}" ]; then
  DISP=":$DISPLAY_N"
else
  for n in $(seq 99 120); do
    [ -e "/tmp/.X11-unix/X$n" ] || { DISP=":$n"; break; }
  done
fi
echo "display $DISP, engine $ENGINE, basedir $BASEDIR"

Xvfb "$DISP" -screen 0 "${W}x${H}x24" -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
XPID=$!
sleep 2

cleanup() {
  kill $GPID 2>/dev/null; sleep 3; kill -9 $GPID 2>/dev/null
  kill $XPID 2>/dev/null; sleep 1; kill -9 $XPID 2>/dev/null
}
trap cleanup EXIT

# The engine writes config/savegames/qconsole.log under $HOME/.hexen2.  Bind a
# throwaway dir over $HOME so a run cannot touch the real one, then re-bind the
# game data read-only *on top* -- the basedir usually lives inside $HOME, so
# the order matters: the ro-bind must come after the HOME bind or it is hidden.
bwrap --dev-bind / / \
      --bind "$WORK/home" "$HOME" \
      --ro-bind "$BASEDIR" "$BASEDIR" \
  env DISPLAY="$DISP" HOME="$HOME" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  "$ENGINE" -basedir "$BASEDIR" -width "$W" -height "$H" -window -nosound "$@" \
  >"$OUT/engine.stdout" 2>&1 &
GPID=$!

export DISPLAY=$DISP
WID=""
for i in $(seq 1 90); do
  WID=$(xdotool search --name "." 2>/dev/null | tail -1)
  [ -n "$WID" ] && break
  kill -0 $GPID 2>/dev/null || { echo "engine exited early; see $OUT/engine.stdout" >&2; exit 4; }
  sleep 1
done
[ -n "$WID" ] || { echo "no window after 90s; see $OUT/engine.stdout" >&2; exit 4; }
echo "window id: [$WID] after ${i}s"
sleep 10

shot()  { sleep 2; import -window root "$OUT/$1.png" 2>/dev/null; echo "  shot $1"; }
key()   { xdotool key --clearmodifiers "$1"; sleep 0.7; }
keyn()  { for _ in $(seq 1 "$2"); do xdotool key --clearmodifiers "$1"; sleep 0.5; done; }
typ()   { xdotool type --clearmodifiers --delay 40 "$1"; sleep 0.4; xdotool key Return; sleep 0.8; }

# Shared in-game sequence: cheats, spawn ONE armor helmet (HelmetAC differs most
# between Paladin[4] and Demoness[8]), touch it, expand the lower bar, chase cam.
ingame() {
  local tag="$1"
  sleep 25
  shot "07-ingame-$tag"
  key grave; sleep 1
  typ "god"; typ "notarget"; typ "impulse 9"
  # Hexen II's default.cfg has no WASD bindings; bind explicitly so the
  # xdotool key holds actually move the player.
  typ "bind w +forward"
  typ "bind s +back"
  typ "create item_armor_helmet"
  typ "path"
  key grave; sleep 2
  # the helmet spawns 80 units along the view vector (host_cmd.c Host_Create_f),
  # so sweep forward and back over that spot to trigger armor_touch
  for pass in 1 2 3; do
    xdotool keydown w; sleep 0.5; xdotool keyup w; sleep 1.2
    xdotool keydown s; sleep 0.4; xdotool keyup s; sleep 1.2
  done
  shot "08-$tag-armor-picked-up"
  key grave; sleep 1
  typ "+showinfo"
  key grave; sleep 3
  shot "09-lowerbar-$tag"
  key grave; sleep 1
  typ "chase_active 1"
  key grave; sleep 3
  shot "10-chasecam-$tag"
}

xdotool mousemove $((W/2)) $((H-1)); sleep 1   # park pointer below every menu item

case "$SCEN" in

  oldmission_demoness)
    shot 01-main
    key Return            # SINGLE PLAYER
    shot 02-singleplayer-cursor0
    keyn Down 3           # cursor -> 3 == OLD MISSION
    shot 03-singleplayer-cursor3-OLDMISSION
    key Return            # -> class menu with m_enter_portals == 0
    shot 04-class-menu-OLDMISSION
    keyn Down 4           # cursor -> 4 == DEMONESS
    shot 05-class-demoness-selected
    key Return
    shot 06-difficulty-demoness
    key Return            # skill 0 -> map demo1
    ingame demoness
    ;;

  oldmission_paladin)
    key Return            # SINGLE PLAYER
    keyn Down 3           # OLD MISSION
    key Return            # class menu, cursor 0 == PALADIN
    shot 04-class-menu
    key Return
    shot 06-difficulty-paladin
    key Return
    ingame paladin
    ;;

  # OLD MISSION -> Demoness -> demo1 -> force R_TranslatePlayerSkin via "color"
  demoness_skin)
    key Return            # SINGLE PLAYER
    keyn Down 3           # OLD MISSION
    key Return            # class menu
    keyn Down 4           # DEMONESS
    key Return
    key Return            # skill 0 -> map demo1
    sleep 25
    key grave; sleep 1
    typ "god"; typ "notarget"
    typ "chase_active 1"
    key grave; sleep 3
    shot 20-chase-default-colors
    key grave; sleep 1
    typ "color 3 6"       # -> Host_Color_f -> svc_updatecolors -> CL_NewTranslation
    key grave; sleep 3    #    -> R_TranslatePlayerSkin (gl_rmisc.c:465 clamp)
    shot 21-chase-after-color-3-6
    key grave; sleep 1
    typ "color 11 4"
    key grave; sleep 3
    shot 22-chase-after-color-11-4
    ;;

  newmission)
    shot 01-main
    key Return            # SINGLE PLAYER
    key Return            # cursor 0 == NEW MISSION
    shot 02-class-menu-NEWMISSION
    ;;

  noportals)
    shot 01-main
    key Return            # SINGLE PLAYER
    shot 02-singleplayer
    key Return            # NEW GAME
    shot 03-class-menu
    keyn Down 6           # walk past the 4th entry; must wrap at 4, never reach a 5th
    shot 04-class-menu-after-6-down
    ;;

  *)
    echo "headless-drive: unknown scenario '$SCEN'" >&2
    exit 2
    ;;

esac

sleep 2
cp "$WORK/home/.hexen2/qconsole.log" "$OUT/qconsole.log" 2>/dev/null
echo "done -> $OUT"
