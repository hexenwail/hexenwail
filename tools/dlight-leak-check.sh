#!/usr/bin/env bash
#
# dlight-leak-check.sh -- live check that a dynamic light does not light the
# far side of a wall (GitHub #97).
#
#   ./tools/dlight-leak-check.sh [basedir]
#     basedir  a Hexen II install with the retail data1 paks (default ~/hexen2)
#
# Every tier lit both sides of a face's plane (R_AddDynamicLights' fabs(dist),
# ClusteredDlight's abs(pdist)), so a torch on one side of a wall lit the
# other.  tools/bsp_dlight_leak.py counts it offline; this sees it in frames.
#
# Stock wall torches cast no dynamic light: Init_Torch (torch.hc) calls
# makestatic, and a static entity never reaches CL_RelinkEntities.  But a torch
# with a targetname skips makestatic, stays a real entity, and its flame model
# (XF_TORCH_GLOW) gets the torch dlight, radius 150, 8 units up (cl_main.c).
# So the map's own entity lump is written out as an external .ent in the run's
# userdir (which outranks the paks, so Mod_LoadEntities takes it): once
# unchanged, the CONTROL, and once with "targetname" "leakrepro" added to one
# torch, the REPRO.  Each run loads demo2 offscreen, noclips to a camera,
# screenshots it, and the mean luminance of a box in the frame is compared.
#
#   scene  torch          what the box sees                        PASS when
#   1      -720 -88 224   the far side of the torch's wall, out   repro == control
#                         of the light's PVS (faces 4495/4496,
#                         the y=-64 wall, 24 units in front)
#   2      -758 482 192   a front-facing face in the light's PVS  repro == control
#                         with a wall between (face 4446, a strip
#                         at y=448 behind the torch wall's end)
#   3      -798 930 304   a back face in the light's PVS          repro == control
#                         (face 4981, a pillar at y=960)
#   lit    170 312 80     the torch's own wall                    repro > control
#   alias  -758 482 192   an alias model (an obj_barrel the .ent   repro == control
#                         adds at -818 462 136) in a door recess
#                         the torch's wall hides from it
#
# Face numbers are tools/bsp_dlight_leak.py's (--map demo2 --scenes N); every
# box was placed by projecting its face and checked on the frames.  Scene 2 is
# the case the one-sided test (P1) cannot fix: the face does face the light.
# The CPU path passes it since P2 (r_dlightvis.c); the GPU clustered path
# (r_lightclusters 1) fails it until the shadow maps (P4) land.
#
# The alias scene is P2's too: the CPU alias loops (R_DrawAliasModel, the
# instanced twin, the software R_PrepareAlias) took every light in
# range, through any wall.  It photographs scene 2's spot, with a barrel added
# to BOTH .ents -- so it has a control and a repro run of its own, and scene 2's
# frames do not change -- and the box is the barrel.  A third run takes the
# same view without the barrel, and the scene FAILs unless the box differs
# from it by PRESENT: a barrel that did not spawn would leave a wall P2
# already fixes, and the scene would pass without testing a model.  On r_lightclusters 1 the
# GPU shades alias models (ClusteredAliasDlight) and it FAILs until P4.
#
# Why scene 1 is not the torch at 170 312 80 that the design pass picked: the
# server sends the client only the entities in its PVS, and from in front of
# that face the torch is not in it, so the torch has no dlight to leak.  Most
# "out of the light's PVS" leaks are hidden that way for entity lights; they
# stay real for lights the server broadcasts regardless (explosions), and for
# a camera that sees both, which is what this one is.
#
# "==" is within TOL (default 1.0) of 255 levels of mean luminance; "lit" must
# gain at least LITMIN (default 4.0).  The r_dlightinfo dump taken with each
# frame must show the torch's dlight in the repro and not in the control, or
# the frame proves nothing and the scene FAILs as such.
#
# Exit status: 0 if every scene in SCENES passes, 1 if any fails (each failing
# scene is named), 2 if the check could not run.
#
# Environment:
#   ENGINE  engine binary            (default ./result/bin/hexenwail)
#   EXTRA   extra engine args, e.g. "+r_lightclusters 0 +r_dlight_occlusion 0"
#   SCENES  which scenes             (default "1 2 3 lit alias")
#   OUT     keep the PNGs and logs here (default: a temp dir, removed)
#   TOL, LITMIN, WAITS (frames after the map loads, default 150)
#   PRESENT  how far (mean per-pixel luminance) the alias scene's box must
#            differ from the same view without the barrel (a third, "bare" run)
#            for the barrel to count as there, default 2.0
#
# Runs offscreen; on a box without a GPU set SDL_VIDEODRIVER=x11 under Xvfb.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
ENGINE=${ENGINE:-./result/bin/hexenwail}
EXTRA=${EXTRA:-}
SCENES=${SCENES:-1 2 3 lit alias}
TOL=${TOL:-1.0}
LITMIN=${LITMIN:-4.0}
PRESENT=${PRESENT:-2.0}
WAITS=${WAITS:-150}
MAP=demo2

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
for p in pak0.pak pak1.pak; do
	[ -f "$BASEDIR/data1/$p" ] || { echo "no $BASEDIR/data1/$p" >&2; exit 2; }
done

if [ -n "${OUT:-}" ]; then
	mkdir -p "$OUT"; work=$(cd "$OUT" && pwd)
else
	work=$(mktemp -d "${TMPDIR:-/tmp}/dlight-leak.XXXXXX")
	trap 'rm -rf "$work"' EXIT
fi

# scene -> torch origin | camera (setpos x y z pitch yaw roll) | box (fractions
# of the frame: x0 y0 x1 y1) | kind
declare -A TORCH CAM BOX KIND ADD
TORCH[1]="-720 -88 224";  CAM[1]="-830 32 150 0 -90 0";   BOX[1]="0.36 0.25 0.55 0.50"; KIND[1]=dark
TORCH[2]="-758 482 192";  CAM[2]="-801 608 184 17 -90 0"; BOX[2]="0.475 0.25 0.515 0.55"; KIND[2]=dark
TORCH[3]="-798 930 304";  CAM[3]="-811 1088 247 17 -90 0"; BOX[3]="0.49 0.25 0.515 0.52"; KIND[3]=dark
TORCH[lit]="170 312 80";  CAM[lit]="210 200 99 0 90 0";   BOX[lit]="0.55 0.40 0.75 0.60"; KIND[lit]=lit
TORCH[alias]="-758 482 192"; CAM[alias]="-801 608 184 17 -90 0"; BOX[alias]="0.52 0.60 0.58 0.73"; KIND[alias]=dark
ADD[alias]='{
"classname" "obj_barrel"
"origin" "-818 462 136"
}'

for s in $SCENES; do
	[ -n "${TORCH[$s]:-}" ] || { echo "unknown scene '$s' (have: 1 2 3 lit alias)" >&2; exit 2; }
done

# The entity lump (BSP29: lump 0, right after the version) out of the pak, as
# control.ent, and one repro .ent per distinct torch.
torches=$(for s in $SCENES; do echo "${TORCH[$s]}"; done | sort -u | tr ' ' _ | tr '\n' ' ')
plain=(); added=()
for s in $SCENES; do if [ -n "${ADD[$s]:-}" ]; then added+=("$s"); else plain+=("$s"); fi; done
python3 - "$BASEDIR/data1" "$MAP" "$work" $torches <<'PY'
import os, re, struct, sys
data1, mapname, work, torches = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
bsp = None
for pak in ('pak0.pak', 'pak1.pak'):
    d = open(os.path.join(data1, pak), 'rb').read()
    _, ofs, ln = struct.unpack_from('<4sii', d, 0)
    for i in range(ln // 64):
        name, o, s = struct.unpack_from('<56sii', d, ofs + i * 64)
        if name.split(b'\0')[0].decode('latin1').lower() == 'maps/%s.bsp' % mapname:
            bsp = d[o:o + s]
if bsp is None:
    sys.exit('maps/%s.bsp is in neither pak' % mapname)
off, length = struct.unpack('<ii', bsp[4:12])
ents = bsp[off:off + length].split(b'\0')[0].decode('latin1')
open(os.path.join(work, 'control.ent'), 'w', encoding='latin1').write(ents)
for t in torches:
    origin = t.replace('_', ' ')
    pat = re.compile(r'\{[^{}]*"origin"\s*"%s"[^{}]*\}' % re.escape(origin))
    hits = [m for m in pat.finditer(ents) if '"light_torch' in m.group(0)]
    if len(hits) != 1:
        sys.exit('expected exactly one light_torch at "%s", found %d' % (origin, len(hits)))
    m = hits[0]
    block = m.group(0)[:-1] + '"targetname" "leakrepro"\n}'
    open(os.path.join(work, 'repro_%s.ent' % t), 'w', encoding='latin1').write(
        ents[:m.start()] + block + ents[m.end():])
PY

# $1 = run name, $2 = .ent file, rest = scenes to photograph.  Leaves
# $work/$1/qconsole.log and $work/$1/data1/shots/<scene>.png.
#
# The scene commands go through an exec'd cfg rather than +args: the command
# line parser takes the "-90" of "+setpos 210 464 99 17 -90 0" for an option.
run () {
	local name=$1 ent=$2; shift 2
	local ud="$work/$name" s i
	rm -rf "$ud"; mkdir -p "$ud/data1/maps"
	cp "$ent" "$ud/data1/maps/$MAP.ent"
	{
		for ((i = 0; i < WAITS; i++)); do echo wait; done
		echo god; echo notarget; echo noclip
		for s in "$@"; do
			echo "setpos ${CAM[$s]}"
			for ((i = 0; i < 60; i++)); do echo wait; done
			echo "cl_screenshotname shots/$s"
			echo r_dlightinfo
			echo screenshot
			echo wait
		done
		echo quit
	} >"$ud/data1/leakcheck.cfg"
	# shellcheck disable=SC2086  # EXTRA is meant to split
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-width 640 -height 480 -window \
		+r_flatlightstyles 1 +cl_bob 0 +cl_rollangle 0 +r_drawviewmodel 0 +crosshair 0 \
		+viewsize 120 +con_notifytime 0 +scr_centertime 0 $EXTRA \
		+map "$MAP" +exec leakcheck.cfg >"$ud/stdout" 2>&1 || true
	local log; log=$(find "$ud" -name qconsole.log | head -1)
	if [ -n "$log" ] && [ "$log" != "$ud/qconsole.log" ]; then cp "$log" "$ud/qconsole.log"; fi
	return 0
}

# Mean luminance (Rec. 601, 0..255) of a box given as frame fractions.
# With a third argument, a second PNG: the mean per-pixel |difference| instead.
lum () {
	python3 - "$1" $2 ${3:-} <<'PY'
import struct, sys, zlib
path, x0, y0, x1, y1 = sys.argv[1], *map(float, sys.argv[2:6])
other = sys.argv[6] if len(sys.argv) > 6 else None
def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    i, idat = 8, b''
    while i < len(d):
        n, typ = struct.unpack('>I4s', d[i:i + 8])
        body = d[i + 8:i + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        i += 12 + n
    assert depth == 8 and ctype in (2, 6), 'unsupported PNG (depth %d type %d)' % (depth, ctype)
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for k in range(stride):
            a = line[k - bpp] if k >= bpp else 0
            b = prev[k]; c = prev[k - bpp] if k >= bpp else 0
            if f == 1: line[k] = (line[k] + a) & 255
            elif f == 2: line[k] = (line[k] + b) & 255
            elif f == 3: line[k] = (line[k] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line); prev = line
    return w, h, bpp, rows
w, h, bpp, A = load(path)
B = load(other)[3] if other else None
lum = lambda r, q: 0.299 * r[q] + 0.587 * r[q + 1] + 0.114 * r[q + 2]
tot = cnt = 0
for y in range(int(y0 * h), int(y1 * h)):
    for x in range(int(x0 * w), int(x1 * w)):
        q = x * bpp
        tot += abs(lum(A[y], q) - lum(B[y], q)) if B else lum(A[y], q); cnt += 1
print('%.3f' % (tot / cnt))
PY
}

# $1 log, $2 scene, $3 torch origin: did the torch own a dlight when that
# scene's frame was taken?  Read from the r_dlightinfo dump just before the
# shot, not the whole log: a torch the camera's PVS does not hold is never sent
# to the client, so it can have a dlight in one scene and none in the next.
has_dlight () {
	local o x y z; read -r x y z <<<"$3"; o="org=($x,$y,$((z + 8)))"
	awk -v shot="Wrote shots/$2.png" '/^--- r_dlightinfo/ { buf = "" } { buf = buf $0 "\n" }
		index($0, shot) { printf "%s", buf; exit }' "$1" | grep -qF "$o" && echo yes || echo no
}

[ ${#plain[@]} -gt 0 ] && run control "$work/control.ent" ${plain[@]+"${plain[@]}"}
for t in $torches; do
	scn=(); for s in ${plain[@]+"${plain[@]}"}; do [ "${TORCH[$s]// /_}" = "$t" ] && scn+=("$s"); done
	[ ${#scn[@]} -gt 0 ] && run "repro_$t" "$work/repro_$t.ent" "${scn[@]}"
done
# a scene that adds an entity gets a control and a repro of its own, both with it
for s in ${added[@]+"${added[@]}"}; do
	t=${TORCH[$s]// /_}
	run "bare_$s" "$work/control.ent" "$s"	# the same view without the entity
	{ cat "$work/control.ent"; printf '%s\n' "${ADD[$s]}"; } >"$work/control_$s.ent"
	{ cat "$work/repro_$t.ent"; printf '%s\n' "${ADD[$s]}"; } >"$work/repro_${t}_$s.ent"
	run "control_$s" "$work/control_$s.ent" "$s"
	run "repro_${t}_$s" "$work/repro_${t}_$s.ent" "$s"
done

fail=0; failed=()
for s in $SCENES; do
	t=${TORCH[$s]// /_}; ctl="$work/control"; rep="$work/repro_$t"
	if [ -n "${ADD[$s]:-}" ]; then ctl="$work/control_$s"; rep="$work/repro_${t}_$s"; fi
	[ -f "$ctl/qconsole.log" ] || { echo "control run left no qconsole.log; see $ctl/stdout" >&2; exit 2; }
	c="$ctl/data1/shots/$s.png"; r="$rep/data1/shots/$s.png"
	if [ ! -f "$c" ] || [ ! -f "$r" ]; then
		echo "scene $s: FAIL (no screenshot; see $ctl/stdout, $rep/stdout)"; fail=1; failed+=("$s"); continue
	fi
	cd_=$(has_dlight "$ctl/qconsole.log" "$s" "${TORCH[$s]}")
	rd=$(has_dlight "$rep/qconsole.log" "$s" "${TORCH[$s]}")
	lc=$(lum "$c" "${BOX[$s]}"); lr=$(lum "$r" "${BOX[$s]}")
	delta=$(python3 -c "print('%.3f' % ($lr - $lc))")
	present=yes
	if [ -n "${ADD[$s]:-}" ]; then
		pd=$(lum "$c" "${BOX[$s]}" "$work/bare_$s/data1/shots/$s.png" 2>/dev/null || echo 0)
		python3 -c "import sys; sys.exit(0 if $pd >= $PRESENT else 1)" || present="no (box differs from the bare view by $pd)"
	fi
	if [ "$cd_" != no ] || [ "$rd" != yes ]; then
		verdict="FAIL (torch dlight: control $cd_, repro $rd -- the run proves nothing)"
	elif [ "$present" != yes ]; then
		verdict="FAIL (the added entity is not in the box: $present -- the run proves nothing)"
	elif [ "${KIND[$s]}" = lit ]; then
		python3 -c "import sys; sys.exit(0 if $delta >= $LITMIN else 1)" \
			&& verdict="PASS (the torch lights its own wall)" \
			|| verdict="FAIL (the torch no longer lights its own wall: gain < $LITMIN)"
	else
		python3 -c "import sys; sys.exit(0 if abs($delta) <= $TOL else 1)" \
			&& verdict="PASS (no light through the wall)" \
			|| verdict="FAIL (light leaks through the wall: |delta| > $TOL)"
	fi
	printf 'scene %-3s control %7s  repro %7s  delta %+8s  %s\n' "$s" "$lc" "$lr" "$delta" "$verdict"
	case $verdict in FAIL*) fail=1; failed+=("$s");; esac
done
[ "$fail" -eq 0 ] && echo "PASS: no dynamic light leaks through a wall on $MAP (scenes: $SCENES)" \
	|| echo "FAIL: scene(s) ${failed[*]}"
[ -n "${OUT:-}" ] && echo "frames and logs kept in $work"
exit "$fail"
