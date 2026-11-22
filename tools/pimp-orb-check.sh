#!/usr/bin/env bash
#
# pimp-orb-check.sh -- live check of where a misc_modelpimp's glow orb draws
# (GitHub #518) and where its cast light comes from (#522).
#
#   ./tools/pimp-orb-check.sh [basedir] [map] [model]
#     basedir  a Hexen II install with Storm over Thyrion in sot/ (default ~/hexen2)
#     map      a SoT map                                    (default courtyard)
#     model    an alias model the map does not already use  (default models/pot1.mdl)
#
# Why it needs SoT: misc_modelpimp is mod gamecode (Inky's), not in ours, so
# only a mod's progs.dat reaches PF_pimpmodel.  The unit side -- the side list
# CL_RelinkEntities builds and R_DrawAllGlows walks -- is
# tools/test_modelpimp_selforb.py; this is the whole chain: spawnflags ->
# PF_pimpmodel -> pimp->self_orb -> R_AddPimpSelfOnly -> R_DrawAllGlows, and
# for the light PF_pimpmodel -> pimp->self_light -> CL_PimpSelfLights.  The
# light's unit side is tools/test_modelpimp_selflight.py.
#
# Every misc_modelpimp controller has effects exactly EF_NODRAW, which the
# server never sends, so before #518 a self-only orb (spawnflags 4+16) sat on
# an entity the client never had and no orb appeared anywhere.  A shared orb
# (4 alone) is copied onto the model, so the siblings draw it and the
# controller must not add one of its own.  The cast light is the same story:
# before #522 a self-only light (8+16) was never allocated.
#
# Method: the map's entity lump is written out as an external .ent in the
# run's userdir (which outranks the gamedir, so Mod_LoadEntities takes it),
# with two plain misc_model siblings of MODEL in front of the first player
# start, in three versions:
#   control  the siblings alone
#   self     plus a misc_modelpimp for MODEL between them, spawnflags 20 (4+16)
#   shared   plus the same misc_modelpimp, spawnflags 4
#   selflight    plus a misc_modelpimp casting a light, spawnflags 24 (8+16)
#   sharedlight  the same light controller, spawnflags 8
# Each run loads the map offscreen, waits, and prints r_glowinfo (the orbs the
# last frame drew) and r_dlightinfo (the live dynamic lights).  PASS needs:
#   control  no orb on MODEL
#   self     exactly one orb on MODEL: at the controller, on an edict that is
#            neither sibling, with the controller's colour and radius; none on
#            either sibling
#   shared   one orb on each sibling and none at the controller
#   and, in both pimped runs, every other model's orbs equal to the control's.
#   control      no dlight at the controller or either sibling
#   selflight    exactly one dlight at the controller, keyed to its edict
#                (which the client has no entity for), with its colour and
#                max_health radius; none keyed to or at either sibling
#   sharedlight  one dlight at each sibling, keyed to it; none at the
#                controller (spawnflag 8 alone, unchanged)
#   and, in both light runs, every other dlight's origin equal to the control's.
#
# Engine: ENGINE (default ./result/bin/hexenwail).  Runs offscreen; on a box
# without a GPU set SDL_VIDEODRIVER=x11 under Xvfb instead.  KEEP=<dir> keeps
# the userdirs and logs there instead of deleting them.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-courtyard}
MODEL=${3:-models/pot1.mdl}
ENGINE=${ENGINE:-./result/bin/hexenwail}
WAITS=${WAITS:-150}
BSP="$BASEDIR/sot/maps/$MAP.bsp"

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
[ -f "$BSP" ] || { echo "no $BSP: this check needs Storm over Thyrion's maps" >&2; exit 2; }
[ -f "$BASEDIR/sot/progs.dat" ] || { echo "no $BASEDIR/sot/progs.dat" >&2; exit 2; }

if [ -n "${KEEP:-}" ]; then
	work=$KEEP
	mkdir -p "$work"
else
	work=$(mktemp -d "${TMPDIR:-/tmp}/pimp-orb.XXXXXX")
	trap 'rm -rf "$work"' EXIT
fi

# Writes control/self/shared .ent and prints the controller's and the two
# siblings' origins.  They go 96 units in front of the first info_player_start
# along the way it faces, 40 apart, so all three are in its PVS and in view.
read -r ctl sib1 sib2 < <(python3 - "$BSP" "$MODEL" "$work" <<'PY'
import math, re, struct, sys
bsp, model, work = sys.argv[1:]
data = open(bsp, 'rb').read()
off, length = struct.unpack('<ii', data[4:12])
ents = data[off:off + length].rstrip(b'\0').decode('latin1')
if ('"%s"' % model) in ents:
    sys.exit("%s already places %s; pick a model it does not use" % (bsp, model))
start = None
for body in re.findall(r'\{([^{}]*)\}', ents):
    kv = dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', body))
    if kv.get('classname') == 'info_player_start':
        start = kv
        break
if not start:
    sys.exit("no info_player_start in " + bsp)
o = [float(v) for v in start['origin'].split()]
yaw = math.radians(float(start.get('angle', '0')))
fwd, side = (math.cos(yaw), math.sin(yaw)), (-math.sin(yaw), math.cos(yaw))
def at(f, s):
    return '%d,%d,%d' % (round(o[0] + fwd[0] * f + side[0] * s),
                         round(o[1] + fwd[1] * f + side[1] * s), round(o[2]))
c, a, b = at(96, 0), at(96, -40), at(96, 40)
key = lambda p: p.replace(',', ' ')
sib = ''.join('{\n"classname" "misc_model"\n"model" "%s"\n"origin" "%s"\n}\n'
              % (model, key(p)) for p in (a, b))
def pimp(sf):
    return ('{\n"classname" "misc_modelpimp"\n"model" "%s"\n"origin" "%s"\n'
            '"spawnflags" "%d"\n"glow_color" "255 0 0"\n"health" "12"\n}\n'
            % (model, key(c), sf))
def light(sf):
    return ('{\n"classname" "misc_modelpimp"\n"model" "%s"\n"origin" "%s"\n'
            '"spawnflags" "%d"\n"glow_color" "255 128 64"\n"max_health" "150"\n}\n'
            % (model, key(c), sf))
for name, extra in (('control', ''), ('self', pimp(20)), ('shared', pimp(4)),
                    ('selflight', light(24)), ('sharedlight', light(8))):
    open('%s/%s.ent' % (work, name), 'w', encoding='latin1').write(
        ents + '\n' + sib + extra)
print(c, a, b)
PY
)
[ -n "${sib2:-}" ] || { echo "FAIL: could not write the .ent files" >&2; exit 1; }

waits=$(printf '+wait %.0s' $(seq "$WAITS"))
run () {	# $1 = control|self|shared|selflight|sharedlight; prints the qconsole.log path
	local ud="$work/$1"
	mkdir -p "$ud/sot/maps"
	cp "$work/$1.ent" "$ud/sot/maps/$MAP.ent"
	# Missile orbs move and the monsters that fire them wake at random, so
	# they are turned off: everything left to compare stands still.
	# shellcheck disable=SC2086  # $waits is meant to split into +wait args
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-game sot +gl_glows 1 +gl_other_glows 1 +gl_missile_glows 0 +r_drawentities 1 \
		+map "$MAP" +notarget $waits +r_glowinfo +r_dlightinfo +quit >"$ud/stdout" 2>&1 || true
	find "$ud" -name qconsole.log | head -1
}
# One line per orb: "<model> <x,y,z> <ef> <r> <colour> <entnum>", from the
# last dump.
orbs () {
	awk '/^--- r_glowinfo:/ { n = 0; delete o } /^\[ *[0-9]+\] num=/ { o[n++] = $0 }
	     END { for (i = 0; i < n; i++) print o[i] }' "$1" |
	sed -E 's/.*num=(-?[0-9]+) +ef=([0-9a-f]+) r= *([0-9.]+) col=\(([^)]*)\) org=\(([^)]*)\) ent=(.*)$/\6 \5 \2 \3 \4 \1/'
}
mine () { orbs "$1" | awk -v m="$MODEL" '$1 == m'; }
others () { orbs "$1" | awk -v m="$MODEL" '$1 != m { print $1, $2 }' | sort | uniq -c; }
at () { mine "$1" | awk -v p="$2" '$2 == p' | wc -l; }
dumped () { grep -q '^--- end r_glowinfo' "$1"; }
# One line per live dlight: "<key> <x,y,z> <r> <colour> <ent>", from the last
# r_dlightinfo dump; <ent> is the client entity's model, or "-" for none.
lights () {
	awk '/^--- r_dlightinfo:/ { n = 0; delete o } /^\[ *[0-9]+\] key=/ { o[n++] = $0 }
	     END { for (i = 0; i < n; i++) print o[i] }' "$1" |
	sed -E 's/ ent=<no model>//; s/.*key=(-?[0-9]+) +r= *([0-9.]+) ttl= *[-0-9.]+ col=\(([^)]*)\) org=\(([^)]*)\)( ent=([^ ]+))?.*$/\1 \4 \2 \3 \6/' |
	awk '{ print $1, $2, $3, $4, (NF > 4 ? $5 : "-") }'
}
lit () { lights "$1" | awk -v p="$2" '$2 == p' | wc -l; }
ldumped () { grep -q '^--- [0-9]* active dlights ---' "$1"; }
# Every dlight away from our three positions, by origin (torch styles flicker
# the colour, so only where they are is compared).
otherlights () { lights "$1" | awk -v a="$ctl" -v b="$sib1" -v d="$sib2" '$2 != a && $2 != b && $2 != d { print $2 }' | sort | uniq -c; }

c=$(run control); s=$(run self); h=$(run shared); sl=$(run selflight); hl=$(run sharedlight)
[ -n "$c" ] && [ -n "$s" ] && [ -n "$h" ] && [ -n "$sl" ] && [ -n "$hl" ] ||
	{ echo "FAIL: an engine run left no qconsole.log" >&2; exit 1; }

fail=0
for f in "$c" "$s" "$h" "$sl" "$hl"; do
	if grep -q 'Unknown command "r_glowinfo"' "$f"; then
		echo "FAIL: engine lacks r_glowinfo (built before #518?): $ENGINE"; fail=1; break
	fi
	dumped "$f" || { echo "FAIL: no r_glowinfo dump in $f"; fail=1; }
	ldumped "$f" || { echo "FAIL: no r_dlightinfo dump in $f"; fail=1; }
done
[ "$fail" -eq 0 ] || exit 1

printf 'controller at %s, siblings at %s and %s\n' "$ctl" "$sib1" "$sib2"
for r in control:"$c" self:"$s" shared:"$h"; do
	printf '%-8s %s orb(s) on %s:\n' "${r%%:*}" "$(mine "${r#*:}" | wc -l)" "$MODEL"
	mine "${r#*:}" | sed 's/^/           /'
done

[ "$(mine "$c" | wc -l)" -eq 0 ] || { echo "FAIL: control: $MODEL already has an orb"; fail=1; }

# self: one orb, the controller's: its glow_color (red) and health (radius
# 12), on an edict that is neither sibling (their numbers come from shared).
[ "$(mine "$s" | wc -l)" -eq 1 ] || { echo "FAIL: self: want exactly one orb on $MODEL"; fail=1; }
[ "$(at "$s" "$ctl")" -eq 1 ] || { echo "FAIL: self: no orb at the controller ($ctl)"; fail=1; }
read -r _ _ _ rad col num < <(mine "$s" | awk -v p="$ctl" '$2 == p' | head -1) || true
sibnums=$(mine "$h" | awk '{ print $6 }')
[ -n "${num:-}" ] && [ "$num" -gt 0 ] && ! grep -qx -- "$num" <<<"$sibnums" &&
	[ "$rad" = 12.0 ] && [ "$col" = 1.00,0.00,0.00 ] ||
	{ echo "FAIL: self: the orb at the controller is not the controller's (edict ${num:-?}, radius ${rad:-?}, colour ${col:-?})"; fail=1; }
[ "$(at "$s" "$sib1")" -eq 0 ] && [ "$(at "$s" "$sib2")" -eq 0 ] ||
	{ echo "FAIL: self: an unflagged sibling drew an orb"; fail=1; }

# shared: the siblings draw it, the controller does not.
[ "$(at "$h" "$sib1")" -eq 1 ] && [ "$(at "$h" "$sib2")" -eq 1 ] ||
	{ echo "FAIL: shared: want one orb on each sibling"; fail=1; }
[ "$(at "$h" "$ctl")" -eq 0 ] || { echo "FAIL: shared: the controller drew an orb of its own"; fail=1; }

for r in self:"$s" shared:"$h"; do
	[ "$(others "$c")" = "$(others "${r#*:}")" ] || {
		echo "FAIL: ${r%%:*}: other models' orbs changed:"
		diff <(others "$c") <(others "${r#*:}") || true; fail=1; }
done

for r in control:"$c" selflight:"$sl" sharedlight:"$hl"; do
	printf '%-11s dlights at the controller and siblings:\n' "${r%%:*}"
	lights "${r#*:}" | awk -v a="$ctl" -v b="$sib1" -v d="$sib2" '$2 == a || $2 == b || $2 == d' | sed 's/^/           /'
done

[ "$(lit "$c" "$ctl")" -eq 0 ] && [ "$(lit "$c" "$sib1")" -eq 0 ] && [ "$(lit "$c" "$sib2")" -eq 0 ] ||
	{ echo "FAIL: control: a dlight already sits at the controller or a sibling"; fail=1; }

# selflight: one light, the controller's: glow_color 255 128 64, max_health
# 150, keyed to an edict that is neither sibling and has no client entity.
sibkeys=$(lights "$hl" | awk -v b="$sib1" -v d="$sib2" '$2 == b || $2 == d { print $1 }')
[ "$(lit "$sl" "$ctl")" -eq 1 ] || { echo "FAIL: selflight: want exactly one dlight at the controller ($ctl)"; fail=1; }
read -r lkey _ lrad lcol lent < <(lights "$sl" | awk -v p="$ctl" '$2 == p' | head -1) || true
[ -n "${lkey:-}" ] && [ "$lkey" -gt 0 ] && ! grep -qx -- "$lkey" <<<"$sibkeys" &&
	[ "$lent" != "$MODEL" ] && [ "$lrad" = 150.0 ] && [ "$lcol" = 1.00,0.50,0.25 ] ||
	{ echo "FAIL: selflight: the dlight at the controller is not the controller's (key ${lkey:-?}, ent ${lent:-?}, radius ${lrad:-?}, colour ${lcol:-?})"; fail=1; }
[ -z "${lkey:-}" ] || [ "$(lights "$sl" | awk -v k="$lkey" '$1 == k' | wc -l)" -eq 1 ] ||
	{ echo "FAIL: selflight: the controller's key holds more than one dlight"; fail=1; }
[ "$(lit "$sl" "$sib1")" -eq 0 ] && [ "$(lit "$sl" "$sib2")" -eq 0 ] &&
	[ "$(lights "$sl" | awk -v m="$MODEL" '$5 == m' | wc -l)" -eq 0 ] ||
	{ echo "FAIL: selflight: an unflagged sibling cast a light"; fail=1; }

# sharedlight: each sibling lights itself, the controller does not.
[ "$(lights "$hl" | awk -v p="$sib1" -v m="$MODEL" '$2 == p && $5 == m' | wc -l)" -eq 1 ] &&
	[ "$(lights "$hl" | awk -v p="$sib2" -v m="$MODEL" '$2 == p && $5 == m' | wc -l)" -eq 1 ] ||
	{ echo "FAIL: sharedlight: want one dlight on each sibling"; fail=1; }
[ "$(lit "$hl" "$ctl")" -eq 0 ] || { echo "FAIL: sharedlight: the controller cast a light of its own"; fail=1; }

for r in selflight:"$sl" sharedlight:"$hl"; do
	[ "$(otherlights "$c")" = "$(otherlights "${r#*:}")" ] || {
		echo "FAIL: ${r%%:*}: other dlights moved:"
		diff <(otherlights "$c") <(otherlights "${r#*:}") || true; fail=1; }
done

[ "$fail" -eq 0 ] && echo "PASS: on $MAP, spawnflags 20 draws one orb at its controller and 4 still lights the siblings only; 24 casts one light from its controller and 8 still lights the siblings only"
exit "$fail"
