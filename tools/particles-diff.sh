#!/usr/bin/env bash
#
# particles-diff.sh -- the checks for tools/sweep-particles-gpu.steps: GL's
# frames against the SDL_GPU backend's (#340 P3b).  See the steps file's
# header for how to take the two runs and where the thresholds come from.
#
# Usage:
#   particles-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (200x24+100+576, as in tools/sweep-world-gpu.steps), and:
#
#   0. Every frame is 800x600, the size the mask and thresholds are for.
#   1. 01-noparticles, 02-round, 04-sprites, 05-sprites-wa1: PSNR against
#      GL >= PSNR_MIN (default 42), as tools/shot-diff.sh's PSNR_MIN mode;
#      06-glows and 07-noglows >= SCENE_MIN (default 28), a floor for the
#      torch room alone, whose flames animate (see 3).
#   2. 03-square, square particles: whole-frame PSNR cannot tell a point
#      drawn one pixel over from a point drawn at the wrong size, and Mesa's
#      GL rasterizes a 2-pixel point one column right of Vulkan (lavapipe and
#      NVIDIA alike).  So on each side count the pixels the particles change
#      against 01-noparticles (any channel, any amount), and sum the change;
#      -gpu's must be within SQUARE_TOL (default 10%) of GL's, both.  A point
#      drawn at 1 pixel changes a quarter of them.
#   3. 06-glows against 07-noglows: what the glows add on each side, PSNR >=
#      GLOW_MIN (default 60) over the pixels where the two sides' no-glow
#      frames agree within 2% -- the torch flames under the glows are
#      animated, and a pause catches them at a frame of its own each run.
#      At most GLOW_EXCLUDE_MAX (default 20000) pixels may be left out, so a
#      scene that is wrong everywhere cannot pass by excluding itself.
#   4. 04-sprites against 05-sprites-wa1 (r_wateralpha 0.33, then 1, so the
#      translucent sprite turns opaque): the pixels that change, counted as
#      in 2, on each side, nonzero and within SQUARE_TOL of each other.
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,35p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
GLOW_MIN=${GLOW_MIN:-60}
SCENE_MIN=${SCENE_MIN:-28}
GLOW_EXCLUDE_MAX=${GLOW_EXCLUDE_MAX:-20000}
SQUARE_TOL=${SQUARE_TOL:-0.10}

command -v magick >/dev/null || { echo "particles-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

mask() { magick "$1" -alpha off -fill black -draw "rectangle 100,576 299,599" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1}'
}
# |on - off|, masked
dif() { mask "$1" "$T/x.png"; mask "$2" "$T/y.png"; magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off "$3"; }
# pixels that differ at all, in any channel (compare's AE is not a count on
# IM 7.1.2: it weighs each pixel by how much it differs)
changed() {
	dif "$1" "$2" "$T/c.png"
	magick "$T/c.png" -separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
summed() { dif "$1" "$2" "$T/d.png"; magick "$T/d.png" -format '%[fx:mean*w*h*3]' info:; }
# "inf" or a number, >= min
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# b within tol of a, a nonzero
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { exit !(a > 0 && b >= a * (1 - t) && b <= a * (1 + t)) }'; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-24s OK    (%s)\n' "$1" "$3"
	else printf '%-24s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

for n in 01-noparticles 02-round 03-square 04-sprites 05-sprites-wa1 06-glows 07-noglows; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { printf '%-24s MISSING in %s\n' "$n" "$d"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "particles-diff: FRAMES MISSING"; exit 1; }
for f in "$GL"/0[1-7]-*.png "$GPU"/0[1-7]-*.png; do
	sz=$(magick identify -format '%wx%h' "$f" 2>/dev/null)
	[ "$sz" = 800x600 ] || { printf '%-24s DIFF  (size %s, want 800x600)\n' "$f" "${sz:-unreadable}"; rc=1; }
done
[ $rc = 0 ] || { echo "particles-diff: WRONG FRAME SIZE"; exit 1; }

for n in 01-noparticles 02-round 04-sprites 05-sprites-wa1; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done
for n in 06-glows 07-noglows; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$SCENE_MIN"; report "$n scene" $? "PSNR $p dB, min $SCENE_MIN"
done

cg=$(changed "$GL/03-square.png" "$GL/01-noparticles.png")
cp=$(changed "$GPU/03-square.png" "$GPU/01-noparticles.png")
sg=$(summed "$GL/03-square.png" "$GL/01-noparticles.png")
sp=$(summed "$GPU/03-square.png" "$GPU/01-noparticles.png")
near "$cg" "$cp" "$SQUARE_TOL" && near "$sg" "$sp" "$SQUARE_TOL"
report 03-square $? "pixels changed GL $cg / -gpu $cp, summed change $sg / $sp, tolerance $SQUARE_TOL"

dif "$GL/06-glows.png" "$GL/07-noglows.png" "$T/ga.png"
dif "$GPU/06-glows.png" "$GPU/07-noglows.png" "$T/gb.png"
mask "$GL/07-noglows.png" "$T/x.png"; mask "$GPU/07-noglows.png" "$T/y.png"
# white where the no-glow frames agree, dilated black where they do not
magick compare -fuzz 2% "$T/x.png" "$T/y.png" -compose src -highlight-color black -lowlight-color white \
	"$T/m.png" 2>/dev/null
magick "$T/m.png" -alpha off -negate -morphology dilate square:1 -negate "$T/m.png"
ex=$(magick "$T/m.png" -format '%[fx:int((1-mean)*w*h+0.5)]' info:)
magick "$T/ga.png" "$T/m.png" -compose multiply -composite -alpha off "$T/ga.png"
magick "$T/gb.png" "$T/m.png" -compose multiply -composite -alpha off "$T/gb.png"
p=$(magick compare -metric PSNR "$T/ga.png" "$T/gb.png" null: 2>&1 | awk '{print $1}')
[ "$p" = 120 ] && p=inf
atleast "$p" "$GLOW_MIN" && [ "$ex" -le "$GLOW_EXCLUDE_MAX" ]
report 06-glows $? "what the glows add: PSNR $p dB, min $GLOW_MIN, $ex pixels excluded, max $GLOW_EXCLUDE_MAX"

tg=$(changed "$GL/04-sprites.png" "$GL/05-sprites-wa1.png")
tp=$(changed "$GPU/04-sprites.png" "$GPU/05-sprites-wa1.png")
near "$tg" "$tp" "$SQUARE_TOL"
report r_wateralpha $? "the translucent sprite at 0.33 and 1: pixels changed GL $tg / -gpu $tp"

[ $rc -eq 0 ] && echo "particles-diff: all checks pass" || echo "particles-diff: DIFFERENCES FOUND"
exit $rc
