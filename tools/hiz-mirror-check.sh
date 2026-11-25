#!/usr/bin/env bash
#
# hiz-mirror-check.sh -- live check that the Hi-Z cull compares in the depth
# range the view drew into at r_mirroralpha != 1 (GitHub #533).
#
#   ./tools/hiz-mirror-check.sh [basedir]
#     basedir  a Hexen II install whose data1 holds demo1  (default ~/hexen2)
#
# At r_mirroralpha != 1, on any map, R_Clear (gl_rmain.c) puts the view
# into half the window-Z range ([0.5, 1] under reversed-Z, [0, 0.5]
# forward) and R_Mirror draws the reflection into the other half.  The Hi-Z pyramid is built from
# that buffer.  cull_mark_comp used to compare each surface's unscaled [0, 1]
# depth against it.  Under reversed-Z a surface's depth then sits below the
# world's (all >= 0.5) wherever no sky or reflection is in its tile, and
# forward-Z (all <= 0.5) is the mirror image: the cull rejected surfaces in
# view (DrawTextureChains drew them again, so only the saving was lost).
# Master, 800x600 on the Tesla P4, hiz of post-frustum at r_mirroralpha 1
# against 0.5:
#
#                       reversed-Z           forward-Z
#   demo1  (wall)       121 vs 122 of 399    115 vs 122 of 399
#   demo1f (floor)      164 vs 306 of 493    164 vs 306 of 493
#
# and with the fix 121 vs 115, 164 vs 164, 115 vs 115, 164 vs 164.  The
# wall panel fills most of the view 96 units ahead, so nearly every tile
# there holds reflection depth and master barely over-rejects; the floor
# camera is the one that fails it.
#
# Each run loads one map of tools/mk-mirror-testgame.py's game, in each
# depth convention (gl_reversed_z 1 and 0, then vid_restart; a run that
# comes up in the other one fails), stands at the
# camera tools/sweep-mirror-gpu.steps shoots from, and samples gl_hiz_stats
# at r_mirroralpha 1 and then 0.5.  The view draws the same scene both ways;
# at 0.5 the mirror's own surfaces are left out of its depth and the
# reflection behind them sits in the far half, which never occludes.  So the
# check, on relations rather than counts (they depend on the resolution):
#   * at 0.5, hiz is no more than at 1 plus SLACK (default 5) surfaces, the
#     few the D24 depth buffer's 23 bits in half the range can round across;
#   * at 1 and at 0.5, hiz is not 0 (the cull runs, and a "fix" that turns
#     Hi-Z off at 0.5 does not pass).
# Every sampled frame of a phase must agree (the camera is static): one
# that varies fails.  Three runs a side on the P4 never varied.
#
# KEEP=1 keeps the work directory (logs) and prints its path.
#
# Engine: ENGINE (default ./result/bin/hexenwail), desktop GL.  Commands go
# through an exec'd cfg, as tools/hiz-toggle-check.sh does.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
ENGINE=${ENGINE:-./result/bin/hexenwail}
SLACK=${SLACK:-5}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/hiz-mirror.XXXXXX")
[ -n "${KEEP:-}" ] && echo "keeping $work" >&2 || trap 'rm -rf "$work"' EXIT
mkdir -p "$work/base"
ln -s "$BASEDIR/data1" "$work/base/data1"
python3 "$HERE/mk-mirror-testgame.py" "$work/base" >/dev/null

w() { local i; for i in $(seq "$1"); do echo wait; done; }
st() { echo "echo PHASE-$1"; echo "gl_hiz_stats 1"; w 30; echo "gl_hiz_stats 0"; w 5; }

# run <map> <setpos> <reversed_z>: prints "phase hiz" lines, a1 then a05,
# and "zmode reversed|forward" for the depth convention the run really took
run() {
	local ud="$work/u-$1-z$3"
	mkdir -p "$ud/mirror"
	{
		w 400; echo "echo ZMODE-BEGIN"; echo "gl_reversed_z $3"; echo vid_restart; w 200
		echo god; echo notarget; echo noclip
		echo "r_drawviewmodel 0"; echo "gl_cull 1"
		echo "setpos $2"; echo "r_mirroralpha 1"; w 60; st a1
		echo "r_mirroralpha 0.5"; w 60; st a05
		echo quit
	} >"$ud/mirror/probe.cfg"
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$work/base" -userdir "$ud" \
		-width 800 -height 600 -window -fsaa 0 -game mirror +gl_hiz_cull 1 \
		+map "$1" +exec probe.cfg >"$ud/stdout" 2>&1 || true
	local log
	log=$(find "$ud" -name qconsole.log -print -quit)
	[ -n "$log" ] || return 0
	awk '/^PHASE-/ {p = substr($1, 7)}
	     /^hiz_stats:/ {for (i = 2; i <= NF; i++) if ($i ~ /^hiz=/) h[p] = h[p] " " substr($i, 5)}
	     /^ZMODE-BEGIN/ {z = 1} z && /^Reversed-Z depth buffer enabled/ {rz = 1}
	     END {print "zmode", (rz ? "reversed" : "forward"); for (k in h) {n = split(h[k], a, " "); u = a[1]; for (i = 2; i <= n; i++) if (a[i] != u) u = "varies"; print k, u}}' \
		"$log" | sort
}

fail=0
for z in 1 0; do
for cam in "demo1|-312 -2128 284 0 0 0" "demo1f|-860 -1990 30 10 40 0"; do
	map=${cam%%|*}; pos=${cam##*|}
	declare -A h=()
	while read -r p v; do h[$p]=$v; done < <(run "$map" "$pos" "$z")
	a1=${h[a1]:-?}; a05=${h[a05]:-?}
	want=$([ "$z" = 1 ] && echo reversed || echo forward)
	name="$map/$want-Z"
	bad=
	for v in "$a1" "$a05"; do
		case "$v" in ''|'?'|varies) bad="no steady hiz (a1=$a1 a05=$a05)";; esac
	done
	[ "${h[zmode]:-?}" = "$want" ] || bad="${bad:+$bad; }ran ${h[zmode]:-?}-Z, wanted $want (no ARB_clip_control?)"
	if [ -z "$bad" ]; then
		[ "$a1" -gt 0 ] || bad="hiz=0 at r_mirroralpha 1: the cull is not running"
		[ "$a05" -gt 0 ] || bad="${bad:+$bad; }hiz=0 at r_mirroralpha 0.5: Hi-Z is off with the mirror"
		[ "$a05" -le $((a1 + SLACK)) ] || bad="${bad:+$bad; }r_mirroralpha 0.5 culls $a05 against $a1 at 1: compared outside the view's depth range"
	fi
	if [ -n "$bad" ]; then
		echo "FAIL $name: hiz at 1 = $a1, at 0.5 = $a05 -- $bad"; fail=1
	else
		echo "ok   $name: hiz at 1 = $a1, at 0.5 = $a05"
	fi
	unset h
done
done

[ "$fail" -eq 0 ] && echo "PASS: Hi-Z at r_mirroralpha 0.5 compares in the view's depth range"
exit "$fail"
