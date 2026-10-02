#!/usr/bin/env bash
#
# run.sh -- periodic render eval for GitHub #312 (liquid rim lifting off a
# sloped floor).  Not a gate: it needs ericw-tools, the demo data and Xvfb,
# and takes ~90 s.  tools/test_water_ripple.py is the gate.
#
# Builds a one-room map whose pool meets a 1:2 ramp off the subdivision grid,
# then renders four camera setups (two with the ripple exaggerated to +-40
# units, two at the shipping defaults), each as a flat reference
# (gl_waterripple 0) followed by 12 frames of a moving ripple, and counts
# pixels the rippled water covers that the flat water did not.  A ripple that
# never rises above the rest plane covers none.
#
# Usage (repo root):
#   nix shell nixpkgs#ericw-tools nixpkgs#xorg-server nixpkgs#xdotool \
#     nixpkgs#imagemagick nixpkgs#bubblewrap --command \
#     ./tools/water_ripple_eval/run.sh <outdir>
# Environment: ENGINE (default ./result/bin/glhexen2), LIMIT (see lift_metric.py)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
engine=$(readlink -f "${ENGINE:-./result/bin/glhexen2}")
demo=$(readlink -f "$(nix build .#demodata --no-link --print-out-paths)/share/hexenwail")

rm -rf "$out/map" "$out/base" "$out/shots"
mkdir -p "$out/map" "$out/base/data1/maps"
python3 "$here/genmap.py" "$out/map" "$demo/data1/pak0.pak"
( cd "$out/map" && qbsp -hexen2 ripple312.map >qbsp.log 2>&1 )
cp "$demo"/data1/{pak0.pak,progs.dat,default.cfg,hexen.rc,autoexec.cfg} "$out/base/data1/"
chmod u+w "$out"/base/data1/*
cp "$out/map/ripple312.bsp" "$out/base/data1/maps/"

steps="$out/steps.txt"
{
	echo "sleep 25"
	# r_wateralpha 1: the classifier needs opaque water.
	for c in god noclip "viewsize 120" "crosshair 0" "r_wateralpha 1" \
		"r_turbalpha 1" "gl_waterwarp_speed 4"; do echo "cmd $c"; done
	view() {	# name, setpos, gl_waterripple, gl_waterwarp_amount
		echo "cmd setpos $2"; echo "cmd gl_waterripple 0"
		echo "cmd gl_waterwarp_amount $4"; echo "sleep 2"; echo "shotf $1-flat"
		echo "cmd gl_waterripple $3"; echo "sleep 1"
		for i in $(seq -w 1 12); do echo "shotf $1-$i"; echo "sleep 0.15"; done
	}
	view hi  "-140 -150 130 22 35 0" 10 4
	view lo  "-60 -200 80 4 40 0"    10 4
	view dhi "-20 -120 90 25 40 0"   2 0.5
	view dlo "30 -60 68 3 30 0"      2 0.5
} >"$steps"

STEPS="$steps" ENGINE="$engine" "$here/../headless-drive.sh" script "$out/shots" "$out/base" +map ripple312
# A run whose commands never landed would compare identical flat frames.
# lift_metric.py also checks this from the pixels; this names the cause.
n=$(grep -c '^\]setpos' "$out/shots/qconsole.log" || true)
[ "$n" -eq 4 ] || { echo "FAIL: $n of 4 setpos commands reached the console"; exit 1; }
n=$(grep -c '^\]gl_waterripple' "$out/shots/qconsole.log" || true)
[ "$n" -eq 8 ] || { echo "FAIL: $n of 8 gl_waterripple commands reached the console"; exit 1; }
python3 "$here/lift_metric.py" "$out/shots" hi lo dhi dlo
