#!/usr/bin/env bash
# Live check that the selected artifact survives a savegame (GitHub #159).
#
# "carry the selected artifact through a savegame" (uhexen2-1knr, first in
# 0.8.0-beta.r11) appends the selected artifact id as the last line of
# info.dat.  It was verified by reading the code; this runs it.  Each case
# is a separate engine process driven by a cfg of console commands, so no
# X server or key injection is needed.  Each run reads the engine's own
# view back by saving again: SB_GetSelectedArtifact reports the live
# selection, so the next save's last line is what the engine believes is
# selected.
#
#   1  select an artifact and save: the last line is its id, not -1
#   1b select a different one: the id follows the selection
#   2  relaunch and load: the same artifact is selected afterwards
#   2c control, relaunch and load the save with the line stripped (the
#      pre-r11 format): the selection is NOT restored, so case 2 is the
#      field working and not something else
#   3  mid-session, with another artifact selected, load the stripped save:
#      the running selection is kept
#   5  under protocol 21 (-protocol 21) the field is -1, because that
#      protocol selects by ex_inventory item ids, a different id space
#   4  with OLD_ENGINE set to a build from before the field: that build
#      loads a new-format save and plays on
#
# Usage:
#   tools/artifact-save-check.sh <basedir with retail data1> [outdir]
# Env:
#   ENGINE      engine binary (default ./result/bin/glhexen2)
#   OLD_ENGINE  optional pre-r11 binary for case 4
#   MAP         map to start on (default demo1)
#
# Runs on SDL's offscreen driver, so it needs a GPU EGL can reach (the
# Tesla P4 here).  Case 3 loads a save over a running map; if that crashes
# on your box, see GitHub #204.
set -u

BASE=$(readlink -f "${1:?usage: $0 <basedir> [outdir]}")
OUT=${2:-/tmp/artifact-save-check}
ENGINE=$(readlink -f "${ENGINE:-./result/bin/glhexen2}")
MAP=${MAP:-demo1}
USERDIR=$OUT/userdir
SAVES=$USERDIR/data1

rm -rf "$OUT"
mkdir -p "$SAVES"
fail=0

# A cfg is read into the command buffer whole, and a few thousand "wait"
# lines overflow it (Cbuf_AddText: overflow, and the rest is dropped).  So
# waits come in exec'd pages of 100: each page enters the buffer only when it
# runs.
waits() {
  local _
  for _ in $(seq $(( $1 / 100 ))); do echo "exec wait100.cfg"; done
  for _ in $(seq $(( $1 % 100 ))); do echo wait; done
}
page() { local _; for _ in $(seq 100); do echo wait; done; }
page > "$SAVES/wait100.cfg"

# Map load to "player entered the game" takes a few hundred frames; a
# command sent before signon is dropped without a word.
ingame() { echo "map $MAP"; waits 600; echo "impulse 43"; waits 30; }   # 43: every artifact
# A selection is committed (Inv_Update writes sb_sticky_artifact) only when
# the bar times out: INVENTORY_DISPLAY_TIME, 4 s, in sbar.c.  At the default
# host_maxfps 72 that is ~288 frames, so wait 500 the way a player lets the
# bar close.  Loading inside that window loses the change.
select_n() { local _; for _ in $(seq "$1"); do echo invright; waits 5; done; waits 500; }
save() { echo "save $1"; waits 60; }

run() {   # run <name> <engine> <extra args> -- cfg lines on stdin
  local name=$1 engine=$2 extra=$3
  cat > "$SAVES/$name.cfg"
  echo "quit" >> "$SAVES/$name.cfg"
  # shellcheck disable=SC2086  # $extra is deliberately split
  env -u LD_LIBRARY_PATH SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
    timeout -k 5 180 "$engine" -basedir "$BASE" -userdir "$USERDIR" \
    -condebug $extra +skill 1 +exec "$name.cfg" >"$OUT/$name.stdout" 2>&1
  local rc=$?
  cp "$USERDIR/qconsole.log" "$OUT/$name.log" 2>/dev/null
  [ $rc = 0 ] || { echo "FAIL $name: engine exited $rc (see $OUT/$name.log)"; fail=1; }
}

field() { [ -f "$SAVES/$1/info.dat" ] && tail -n 1 "$SAVES/$1/info.dat" || echo missing; }

check() { # check <case> <description> <ok?>
  if [ "$3" = 1 ]; then echo "PASS $1: $2"; else echo "FAIL $1: $2"; fail=1; fi
}

{ ingame; select_n 3; save sel3; } | run c1 "$ENGINE" ""
{ ingame; select_n 5; save sel5; } | run c1b "$ENGINE" ""
A=$(field sel3); B=$(field sel5)
check 1 "selected artifact written to info.dat (got $A)" "$([ "$A" != -1 ] && [ "$A" != missing ] && echo 1)"
check 1b "a different selection writes a different id ($B vs $A)" "$([ "$B" != "$A" ] && [ "$B" != -1 ] && [ "$B" != missing ] && echo 1)"

# The pre-r11 format: the same save without its last line.
cp -r "$SAVES/sel3" "$SAVES/old3"
sed -i '$d' "$SAVES/old3/info.dat"

{ echo "load sel3"; waits 600; save after2; } | run c2 "$ENGINE" ""
C=$(field after2)
check 2 "relaunch + load restores the selection ($C, saved $A)" "$([ "$C" = "$A" ] && echo 1)"

{ echo "load old3"; waits 600; save after2c; } | run c2c "$ENGINE" ""
D=$(field after2c)
check 2c "control: the stripped save does not restore it ($D, saved $A)" "$([ "$D" != "$A" ] && [ "$D" != missing ] && echo 1)"

{ ingame; select_n 3; echo "load old3"; waits 600; save after3; } | run c3 "$ENGINE" ""
E=$(field after3)
# Running selection is case 1's ($A).  It only proves anything if a reset
# would look different, and case 2c measured what a reset looks like ($D).
check 3 "an old save loaded mid-session keeps the running selection ($E, running $A, reset gives $D)" \
  "$([ "$E" = "$A" ] && [ "$A" != "$D" ] && echo 1)"

{ ingame; select_n 3; save p21; } | run c5 "$ENGINE" "-protocol 21"
F=$(field p21)
grep -q "protocol 21" "$OUT/c5.log" || echo "note: c5.log does not name protocol 21"
check 5 "protocol 21 writes -1 ($F)" "$([ "$F" = -1 ] && echo 1)"

if [ -n "${OLD_ENGINE:-}" ]; then
  # Builds that old have no -userdir and take the passwd home, not $HOME, so
  # the only way to keep them off the real ~/.hexen2 is to bind a throwaway
  # directory over it.
  OLDHOME=$OUT/oldhome
  mkdir -p "$OLDHOME/.hexen2/data1"
  cp -r "$SAVES/sel3" "$SAVES/wait100.cfg" "$OLDHOME/.hexen2/data1/"
  { echo "load sel3"; waits 600; save old4; echo quit; } > "$OLDHOME/.hexen2/data1/c4.cfg"
  env -u LD_LIBRARY_PATH SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
    bwrap --dev-bind / / --bind "$OLDHOME" "$HOME" --ro-bind "$BASE" "$BASE" --die-with-parent \
    timeout -k 5 180 "$(readlink -f "$OLD_ENGINE")" -basedir "$BASE" \
    -condebug +skill 1 +exec c4.cfg >"$OUT/c4.stdout" 2>&1
  rc=$?
  cp "$OLDHOME/.hexen2/qconsole.log" "$OUT/c4.log" 2>/dev/null
  O=$OLDHOME/.hexen2/data1/old4/info.dat
  # The old engine stops reading at info_mask2 and writes no trailing line,
  # so its save is one line shorter than the one it loaded.
  # Builds that old can hang on quit after "Client player removed" (GitHub
  # #204); the timeout then reaps them.  That is not this check's business,
  # so rc 124 passes when the save itself landed and the hang is that one.
  quit_ok=0
  if [ $rc = 0 ]; then quit_ok=1
  elif [ $rc = 124 ] && grep -q "Client player removed" "$OUT/c4.log"; then
    quit_ok=1; echo "note 4: old engine hung on quit (GitHub #204), reaped by timeout"
  fi
  check 4 "a pre-r11 engine loads the new format and saves again (rc $rc, $(wc -l < "$SAVES/sel3/info.dat") -> $(wc -l < "$O" 2>/dev/null || echo 0) lines)" \
    "$([ $quit_ok = 1 ] && grep -q "Loading game" "$OUT/c4.log" && [ -f "$O" ] && [ "$(wc -l < "$O")" -eq $(( $(wc -l < "$SAVES/sel3/info.dat") - 1 )) ] && echo 1)"
else
  echo "SKIP 4: set OLD_ENGINE to a pre-r11 build"
fi

[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED (logs in $OUT)"
exit $fail
