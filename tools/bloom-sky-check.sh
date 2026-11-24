#!/usr/bin/env bash
#
# bloom-sky-check.sh -- live check that a frame of r_bloom leaves the sky as
# it found it (GitHub #547).
#
#   ./tools/bloom-sky-check.sh [basedir]
#     basedir  a Hexen II install with demo1     (default ~/hexen2)
#
# The bloom upsample blends GL_ONE, GL_ONE and left that pair behind.  The
# sky's front layer at r_skyalpha < 1 (the default 0.67) turns blending on and
# sets no func of its own, so from the first bloom frame on it was added to
# the back layer instead of laid over it: demo1's graveyard sky went from a
# mean of 0.26 to 0.49 and stayed there with r_bloom back at 0, until restart.
# Nothing else is needed to set it off (the report's r_softemu frames were
# incidental).
#
# Two runs from an empty -userdir, same camera (the graveyard, looking up at
# the sky over the north wall), screenshotted offscreen:
#   fresh    r_bloom 0 throughout
#   toggle   r_bloom 1 (threshold 0) for a few hundred frames and a shot,
#            then r_bloom 0 (threshold 1) and a second shot
# The bloom shot must differ from fresh in at least BLOOM_MIN pixels (default
# 2000), or bloom never ran and the rest proves nothing.  The second shot must
# equal the fresh run's: the sky crop's mean
# within SKY_TOL (default 0.002) and the 3D view (the frame above its bottom
# tenth, which holds the status bar) within AE_MAX differing pixels (default
# 0).  The status bar is left out because its health reads differently from
# run to run (as tools/pp-diff.sh masks it), one run in two or three here.
# Measured on a Tesla P4: fresh 0.2611, toggle 0.2611, AE 0; on master the
# toggle run's sky is 0.4945, 13602 px differ, and the check fails.
#
# Everything that could move between two runs is off: entities, particles,
# dynamic light, light styles, sky scroll.
#
# Needs ImageMagick (`magick`): run under
#   nix shell nixpkgs#imagemagick --command ./tools/bloom-sky-check.sh
# Engine: ENGINE (default ./result/bin/hexenwail), a desktop GL build.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
ENGINE=${ENGINE:-./result/bin/hexenwail}
WAITS=${WAITS:-300}
SKY_TOL=${SKY_TOL:-0.002}
AE_MAX=${AE_MAX:-0}
BLOOM_MIN=${BLOOM_MIN:-2000}
# 200x50 at (100,0): sky only on this camera, at 640x480 (what the offscreen
# driver comes up at) and 800x600 alike.
SKY_CROP=200x50+100+0

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
command -v magick >/dev/null || { echo "no magick: run under nix shell nixpkgs#imagemagick" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/bloom-sky.XXXXXX")
# KEEP=1 leaves the shots and logs behind for a look.
if [ "${KEEP:-0}" = 1 ]; then echo "work dir: $work"; else trap 'rm -rf "$work"' EXIT; fi

waits=$(printf '+wait %.0s' $(seq "$WAITS"))
still="+r_skyspeed_back 0 +r_skyspeed_front 0 +r_skyalpha 0.67 +r_flatlightstyles 1
	+r_dynamic 0 +r_particles 0 +r_drawentities 0 +r_drawviewmodel 0 +fog 0
	+gl_glows 0 +r_ssao 0 +r_oit 0 +gamma 1 +contrast 1 +r_bloom 0
	+con_notifytime 0 +crosshair 0 +viewsize 120 +showpause 0"
# Through a cfg: the command line ends a +command at the first argument that
# starts with '-', which is most of setpos's.
camera="+exec bloomsky_cam.cfg"

# shoot <case> <args...>: runs the engine, prints the paths of its shots in
# the order taken.  Fails the run if the camera cfg did not execute: on any
# other view SKY_CROP is not sky.
shoot() {
	local ud="$work/$1"; shift
	mkdir -p "$ud/data1"
	echo "god; notarget; noclip; setpos -918 -2034 0 -20 45 0" >"$ud/data1/bloomsky_cam.cfg"
	# shellcheck disable=SC2086  # $still, $waits, $camera split into args
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-width 800 -height 600 -window -fsaa 0 $still +map demo1 $waits $camera \
		"$@" $waits +screenshot +quit >"$ud/stdout" 2>&1 || true
	if ! grep -qs "noclip ON" "$ud/data1/qconsole.log" "$ud/stdout"; then
		echo "FAIL $1: the camera cfg never ran (no \"noclip ON\")" >&2; return 1
	fi
	# C collation: a second shot in the same second is X_0000.png, which a
	# UTF-8 locale sorts before X.png.
	find "$ud" -path '*/shots/*.png' | LC_ALL=C sort
}

skymean() { magick "$1" -crop "$SKY_CROP" +repage -colorspace Gray -format '%[fx:mean]' info:; }
view() { magick "$1" -gravity north -crop 100%x90%+0+0 +repage "$2"; }
# viewae <a> <b>: differing pixels between the 3D views, or nothing on error
viewae() {
	view "$1" "$work/a.png" && view "$2" "$work/b.png" || return 0
	# compare exits 1 when the images differ: an answer, not an error
	{ magick compare -metric AE "$work/a.png" "$work/b.png" null: 2>&1 || true; } |
		awk '$1 ~ /^[0-9.e+-]+$/ {print $1}'
}

mapfile -t f < <(shoot fresh)
# The toggle run shoots with bloom on first (threshold 0, so it visibly
# blooms: at 0.5 this dark view changes in some 30 px), then with bloom back off: the bloom frame must differ from fresh,
# or bloom never ran and the check below would pass vacuously.
# shellcheck disable=SC2086
mapfile -t t < <(shoot toggle +r_bloom 1 +r_bloom_threshold 0 $waits +screenshot \
	$waits +r_bloom 0 +r_bloom_threshold 1)
if [ "${#f[@]}" -ne 1 ] || [ "${#t[@]}" -ne 2 ]; then
	echo "FAIL: ${#f[@]} + ${#t[@]} screenshots, want 1 + 2 (never got in-game?)"; exit 1
fi
fresh=${f[0]} bloom=${t[0]} toggle=${t[1]}

fail=0
ab=$(viewae "$fresh" "$bloom")
if [ -z "$ab" ]; then
	echo "FAIL bloom: could not compare the bloom frame"; fail=1
elif awk -v a="$ab" -v m="$BLOOM_MIN" 'BEGIN {exit !(a + 0 >= m)}'; then
	echo "ok   bloom: r_bloom 1 changes $ab px (min $BLOOM_MIN)"
else
	echo "FAIL bloom: r_bloom 1 changes $ab px (min $BLOOM_MIN): bloom never ran, the rest proves nothing"; fail=1
fi

sf=$(skymean "$fresh")
st=$(skymean "$toggle")
if awk -v a="$sf" -v b="$st" -v t="$SKY_TOL" 'BEGIN {d = a - b; if (d < 0) d = -d; exit !(d <= t)}'; then
	echo "ok   sky: fresh $sf, after r_bloom 1 -> 0 $st"
else
	echo "FAIL sky: fresh $sf, after r_bloom 1 -> 0 $st (tolerance $SKY_TOL)"; fail=1
fi

ae=$(viewae "$fresh" "$toggle")
if [ -z "$ae" ]; then
	echo "FAIL 3D view: could not compare"; fail=1
elif awk -v a="$ae" -v m="$AE_MAX" 'BEGIN {exit !(a + 0 <= m)}'; then
	echo "ok   3D view: $ae px differ (max $AE_MAX)"
else
	echo "FAIL 3D view: $ae px differ (max $AE_MAX)"; fail=1
fi

[ "$fail" -eq 0 ] && echo "PASS: a frame of r_bloom leaves the sky as it found it"
exit "$fail"
