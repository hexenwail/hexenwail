#!/usr/bin/env bash
#
# pimp-nodlight-check.sh -- live check that misc_modelpimp spawnflag 64 turns a
# model's dynamic lights off, end to end (GitHub #140).
#
#   ./tools/pimp-nodlight-check.sh [basedir] [map] [model]
#     basedir  a Hexen II install with Storm over Thyrion in sot/ (default ~/hexen2)
#     map      a SoT map that lights that model         (default courtyard)
#     model    a model casting dynamic lights there      (default models/flame7.mdl)
#
# Why it needs SoT: misc_modelpimp is mod gamecode (Inky's), not in ours, so
# only a mod's progs.dat reaches PF_pimpmodel.  The unit side -- the dlight
# gate in CL_RelinkEntities -- is tools/test_modelpimp_nodlight.py; this is the
# chain above it that no gate can see: spawnflag 64 -> PF_pimpmodel ->
# pimp->ex_flags / mod->ex_flags XF_NO_DLIGHT -> R_GetPimpFlags -> no dlight.
#
# Method: the map's own entity lump is written out twice as an external .ent
# in the run's userdir (which outranks the gamedir, so Mod_LoadEntities takes
# it) -- once unchanged (the control: the model must cast lights) and once
# with one extra misc_modelpimp for the model, spawnflags 64.  Each run loads
# the map offscreen, waits, and prints r_dlightinfo and mcache.  PASS needs:
#   control: the model owns >= 1 dlight and mcache does not say NO_DLIGHT
#   pimped:  the model owns 0 dlights, mcache says NO_DLIGHT, and every other
#            model's dlight count is unchanged (the flag reaches no one else)
#
# Engine: ENGINE (default ./result/bin/glhexen2).  Runs offscreen; on a box
# without a GPU set SDL_VIDEODRIVER=x11 under Xvfb instead.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-courtyard}
MODEL=${3:-models/flame7.mdl}
ENGINE=${ENGINE:-./result/bin/glhexen2}
WAITS=${WAITS:-150}
BSP="$BASEDIR/sot/maps/$MAP.bsp"

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
[ -f "$BSP" ] || { echo "no $BSP: this check needs Storm over Thyrion's maps" >&2; exit 2; }
[ -f "$BASEDIR/sot/progs.dat" ] || { echo "no $BASEDIR/sot/progs.dat" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/pimp-nodlight.XXXXXX")
trap 'rm -rf "$work"' EXIT

# The entity lump: Quake/Hexen II BSP29 and BSP2 both keep it as lump 0,
# right after the 4-byte version.
python3 - "$BSP" "$MODEL" "$work" <<'PY'
import struct, sys
bsp, model, work = sys.argv[1:]
data = open(bsp, 'rb').read()
off, length = struct.unpack('<ii', data[4:12])
ents = data[off:off + length].rstrip(b'\0').decode('latin1')
pimp = ('{\n"classname" "misc_modelpimp"\n"model" "%s"\n'
        '"spawnflags" "64"\n"origin" "0 0 0"\n}\n' % model)
open(work + '/control.ent', 'w', encoding='latin1').write(ents + '\n')
open(work + '/pimped.ent', 'w', encoding='latin1').write(ents + '\n' + pimp)
PY

waits=$(printf '+wait %.0s' $(seq "$WAITS"))
run () {	# $1 = control|pimped; prints the qconsole.log path
	local ud="$work/$1"
	mkdir -p "$ud/sot/maps"
	cp "$work/$1.ent" "$ud/sot/maps/$MAP.ent"
	# shellcheck disable=SC2086  # $waits is meant to split into +wait args
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-game sot +map "$MAP" $waits +r_dlightinfo +mcache +quit >"$ud/stdout" 2>&1 || true
	find "$ud" -name qconsole.log | head -1
}
count () { grep -c "ent=$2 " "$1" || true; }
others () { grep -oE 'ent=[^ ]+' "$1" | grep -vF "ent=$2" | sort | uniq -c; }
flagged () { grep -F "$2" "$1" | grep -q NO_DLIGHT && echo yes || echo no; }

c=$(run control); p=$(run pimped)
[ -n "$c" ] && [ -n "$p" ] || { echo "FAIL: an engine run left no qconsole.log" >&2; exit 1; }

fail=0
cn=$(count "$c" "$MODEL"); pn=$(count "$p" "$MODEL")
printf 'control: %s owns %s dlight(s), NO_DLIGHT=%s\n' "$MODEL" "$cn" "$(flagged "$c" "$MODEL")"
printf 'pimped:  %s owns %s dlight(s), NO_DLIGHT=%s\n' "$MODEL" "$pn" "$(flagged "$p" "$MODEL")"
[ "$cn" -ge 1 ] || { echo "FAIL: control: $MODEL casts no dlight on $MAP; pick a model that does"; fail=1; }
[ "$(flagged "$c" "$MODEL")" = no ] || { echo "FAIL: control: $MODEL is already NO_DLIGHT"; fail=1; }
[ "$pn" -eq 0 ] || { echo "FAIL: pimped: $MODEL still owns $pn dlight(s)"; fail=1; }
[ "$(flagged "$p" "$MODEL")" = yes ] || { echo "FAIL: pimped: mcache does not mark $MODEL NO_DLIGHT"; fail=1; }
[ "$(others "$c" "$MODEL")" = "$(others "$p" "$MODEL")" ] || {
	echo "FAIL: the flag changed other models' dlights:"; diff <(others "$c" "$MODEL") <(others "$p" "$MODEL") || true; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS: misc_modelpimp spawnflag 64 turns off $MODEL's dynamic lights on $MAP, and nothing else's"
exit "$fail"
