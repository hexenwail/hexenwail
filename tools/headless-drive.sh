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

# Resolve through symlinks: ./result is a symlink into /nix/store, and the
# store path is outside $HOME, which the bwrap below replaces wholesale.
ENGINE=$(readlink -f "$ENGINE")
BASEDIR=$(cd "$BASEDIR" && pwd)
ENGINE_DIR=$(dirname "$ENGINE")

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
#
# The engine binary needs the same treatment for the same reason: a source
# checkout under $HOME puts ./result there too, and without this the sandbox
# hides the very binary it is about to exec.  Harmless when ENGINE_DIR is
# already outside $HOME (a /nix/store path binds over itself).
bwrap --dev-bind / / \
      --bind "$WORK/home" "$HOME" \
      --ro-bind "$BASEDIR" "$BASEDIR" \
      --ro-bind "$ENGINE_DIR" "$ENGINE_DIR" \
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
# Like shot(), minus the settling sleep: for sampling a lightstyle as a TIME
# SERIES rather than at 2s intervals, where a 10Hz style aliases into noise.
shotf() { import -window root "$OUT/$1.png" 2>/dev/null; }
key()   { xdotool key --clearmodifiers "$1"; sleep 0.7; }
keyn()  { for _ in $(seq 1 "$2"); do xdotool key --clearmodifiers "$1"; sleep 0.5; done; }
# type without submitting -- needed to test TAB, where pressing Return would
# run whatever just got completed
typn()  { xdotool type --clearmodifiers --delay 40 "$1"; sleep 0.5; }
typ()   { typn "$1"; xdotool key Return; sleep 0.8; }
# End first: the cursor can be mid-line (that is the point of the TAB splice
# tests) and BackSpace only eats backwards, so without it the tail survives
# into the next case and every later shot carries stale text.
wipe()  { xdotool key --clearmodifiers End; sleep 0.2
          for _ in $(seq 1 60); do xdotool key --clearmodifiers BackSpace; done; sleep 0.5; }

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

  # TAB completion in a live console (uhexen2-q6ap).  The point is that these
  # are real X key events, so Key_Event -> CompleteCommand runs exactly as it
  # does under a human; nothing here simulates the console.
  #
  # Never press Return: the whole line would execute, and half of these
  # complete to a "map" that would then load.  Read the result off the shots,
  # and the ambiguous-match listings out of qconsole.log (pass -condebug).
  #
  # Map names assumed are stock data1 (42 maps): eidolon is the only "eido*",
  # romeric1..7 are the only "rom*", nothing matches "zzz".
  # MUST be run with "+map demo1" (or any map) as an extra engine arg. The
  # console cannot be opened from the main menu: Escape closes it, but with
  # nothing connected and no demo loop the engine puts it straight back, and a
  # menu eats grave -- the run then screenshots the untouched menu eight times
  # and looks like a pass. Being in a map puts key_dest at key_game, where
  # toggleconsole binds.
  console_tab)
    sleep 25                   # map load
    key grave;  sleep 2
    shot 00-console-open

    # 1. unique prefix completes fully AND appends a trailing space
    typn "map eido";      key Tab; shot 01-unique-arg
    wipe

    # 2. ambiguous prefix completes to the common stem and lists candidates
    typn "map rom";       key Tab; shot 02-ambiguous-arg
    wipe

    # 3. no match leaves the line exactly as typed (must not eat characters)
    typn "map zzz";       key Tab; shot 03-no-match
    wipe

    # 4. cursor mid-line: completes only up to the cursor, splices the tail
    #    back. Type the tail, walk left over it, then TAB.
    typn "map eidoXY";    key Left; key Left; sleep 0.3
    key Tab; shot 04-midline-splice
    wipe

    # 5. the command word itself (argno 0) still completes
    typn "sv_altnoc";     key Tab; shot 05-command-word
    wipe

    # 6. second argument slot: record takes <demo> then <map>
    typn "record foo eido"; key Tab; shot 06-arg2-maps
    wipe

    # 7. sky in stock data1, which has no gfx/env: must do nothing rather
    #    than misbehave
    typn "sky ";          key Tab; shot 07-sky-empty
    wipe

    # 8. a completion AFTER several others: every row's cleanup is
    #    FS_FreeNameList over a shared single-owner list, so a leaked one
    #    breaks the NEXT completion rather than its own. Repeat case 1 last;
    #    it must still behave identically to shot 01.
    typn "map eido";      key Tab; shot 08-repeat-after-many
    wipe
    ;;

  # uhexen2-ayrn: Ironwail's alias light-trace cache (e2f39505).  Two claims to
  # confirm, both about the CACHE HIT path in R_LightPointColor (gl_rlight.c:682):
  #   1. cache->spot is copied back into lightspot BEFORE InterpolateLightmap
  #      runs, so GL_DrawAliasShadow still lands the shadow on the floor rather
  #      than at the model's origin or a stale height.
  #   2. InterpolateLightmap (gl_rlight.c:550) re-reads d_lightstylevalue[] on
  #      every hit, so a model that never moves -- and therefore hits the cache
  #      every single frame -- keeps reacting to a flickering torch instead of
  #      freezing at the brightness of its first trace.
  #
  # The player in chase cam IS the stationary alias model under test: it is a
  # normal entity, so it lights through AliasModelGetLightInfo -> the cached
  # path, not the R_DrawViewModel special case.
  #
  # SEND NO INPUT during the burst.  Any movement invalidates the cache via the
  # fabs(cache->pos - adjust_origin) test, which would re-trace every frame and
  # make the run pass for the wrong reason -- it would prove nothing about the
  # hit path, which is the only thing this bead is about.
  ayrn_lightcache)
    sleep 25
    key grave; sleep 1
    typ "god"; typ "notarget"
    typ "r_shadows 1"
    typ "chase_active 1"
    key grave; sleep 3
    shot 30-chase-r_shadows1
    # Burst with zero input in between: the player entity's lightcache is hit
    # on every frame of all of these.
    for i in 1 2 3 4 5 6 7 8 9 10; do shot "31-still-$i"; done
    # Control: same scene with shadows off, to tell a shadow apart from a dark
    # lightmap patch when reading 30-chase-r_shadows1.
    key grave; sleep 1
    typ "r_shadows 0"
    key grave; sleep 3
    shot 32-chase-r_shadows0
    ;;

  # uhexen2-ayrn, part 2: the lightstyle-liveness half, run somewhere it can
  # actually fail.  The first pass (ayrn_lightcache) put the player at demo1's
  # spawn and proved nothing: a frame-to-frame diff there was pin-black over the
  # whole world, because every floor face under the spawn carries style 0.  With
  # nothing animating, a cache that correctly re-interpolates and a cache frozen
  # at its first trace render identically.
  #
  # So teleport onto a floor face that a BSP scan says is lit by style 4 --
  # "mamamamamama", the fast strobe, which swings between full and black and is
  # therefore the largest signal available in this map.  Face centroid
  # (-1612.5, 1548.0, 67.3) in maps/demo1.bsp, the biggest style-4 floor in the
  # map; 54 floor faces there carry a style in 1..11, and only those 11 styles
  # animate (32+ are switchable lights, static until triggered).
  #
  # Land, let the player settle, then send NOTHING.  Same reasoning as part 1:
  # any movement re-traces instead of hitting the cache, and would test the
  # wrong path.
  ayrn_strobe)
    sleep 25
    key grave; sleep 1
    typ "god"; typ "notarget"
    typ "r_shadows 1"
    typ "chase_active 1"
    typ "setpos -1612 1548 107"
    key grave; sleep 5      # land and settle before the burst
    shot 40-strobe-landed
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do shot "41-strobe-$i"; done
    ;;

  # uhexen2-ayrn, part 3.  Parts 1 and 2 were both inconclusive, for opposite
  # reasons, and both failures are easy to repeat by accident:
  #   - part 1 (ayrn_lightcache) sat at demo1's spawn, where every floor face is
  #     style 0.  Nothing in the world could animate, so a frozen cache and a
  #     live one render identically.  A frame diff there is pin-black except for
  #     the scrolling sky and the model's own idle animation.
  #   - part 2 (ayrn_strobe) reached a style-4 floor but sampled at 2s
  #     intervals, which aliases a 10Hz strobe into noise, and its floor
  #     reference patch was not on the animated surface -- it pinned to one
  #     exact value for 7 consecutive frames once the player stopped settling.
  #
  # There is a third failure mode underneath both of those, and it invalidated
  # the targets they used: dface_t puts styles[] at byte 12 and lightofs at 16,
  # and the scan that picked those spots read them at 14 and 18.  Every style
  # number it reported was two bytes off -- it was reading styles[2], styles[3]
  # and half of lightofs.  Corrected, demo1 has ZERO floor faces carrying an
  # animated style, so no spot in that map can test this claim at all, and the
  # "style 4" and "style 2" floors parts 1-2 aimed at are both plain style 0.
  #
  # So switch map.  A corrected scan of all 42 stock maps ranks eidolon first:
  # 21 animated-style floor faces, the largest being face #24 at centroid
  # (1817.6, 32.0, 0.0), styles[] = [2, 0, 255, 255].  Style 2 is
  # "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba" -- 50 frames at 10Hz,
  # a 5-second full sweep from black to full bright: the largest amplitude
  # available and slow enough to sample honestly.
  #
  # MUST be run with "+map eidolon", not demo1.
  #
  # Two things part 2 lacked:
  #   - `viewpos` into the console log, so the position the player ACTUALLY
  #     settled at can be pushed back through a BSP downtrace offline and the
  #     surface under its feet identified by style.  Otherwise "did we land on
  #     the animated face" is guesswork.
  #   - a real time series: 30 back-to-back frames, no settling sleep, covering
  #     roughly one full style-2 cycle.
  ayrn_pulse)
    sleep 25
    key grave; sleep 1
    typ "god"; typ "notarget"
    typ "r_shadows 1"
    typ "chase_active 1"
    typ "setpos 1817 32 40"
    sleep 5
    typ "viewpos"          # lands in qconsole.log; read it back offline
    key grave; sleep 5     # settle fully -- a moving player re-traces
    shot 50-pulse-landed
    for i in $(seq -w 1 30); do shotf "51-pulse-$i"; sleep 0.15; done
    echo "  fast burst done"
    ;;

  *)
    echo "headless-drive: unknown scenario '$SCEN'" >&2
    exit 2
    ;;

esac

sleep 2
cp "$WORK/home/.hexen2/qconsole.log" "$OUT/qconsole.log" 2>/dev/null
echo "done -> $OUT"
