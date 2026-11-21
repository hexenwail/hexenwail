#!/usr/bin/env bash
#
# fog-diff.sh -- the checks for tools/sweep-fog-gpu.steps: GL's frames against
# the SDL_GPU backend's (#340 P3c).  See the steps file's header for how to
# take the two runs and where the thresholds come from.
#
# Usage:
#   fog-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (200x24+100+576, as in tools/sweep-world-gpu.steps), and:
#
#   0. Both sides have every frame the steps file shoots, each 800x600, the
#      size the mask and thresholds are for.
#   1. Every frame but the torch room's: PSNR against GL >= PSNR_MIN
#      (default 42), as tools/shot-diff.sh's PSNR_MIN mode.  34-square too,
#      whole-frame, because fog A damps the square particles enough that
#      Mesa's one-column point offset (tools/particles-diff.sh's 2) stays
#      above it; and as particles-diff.sh does, the pixels the square
#      particles change against 30-noparticles and the summed change, -gpu's
#      within SQUARE_TOL (default 10%) of GL's.  Under fog the summed change
#      is what says the points are fogged.
#   2. The torch room (44-glows .. 47-noglows-fogb): PSNR >= SCENE_MIN
#      (default 28), a floor alone, because its flames animate and a pause
#      catches them at a frame of its own each run (tools/particles-diff.sh's
#      06/07).
#   3. What one consumer adds, so that a consumer drawn unfogged cannot hide
#      in a frame the rest of which is right: on each side the difference
#      between a frame with it and the same paused frame without it, and the
#      two sides' differences compared by PSNR >= ADD_MIN (default 55).
#      31-particles over 30-noparticles (round particles, spart_frag) and
#      40-sprites over 43-nosprites (the sprites and every entity in view,
#      salias_frag), both under fog A.  And what the fog itself adds, a
#      fogged frame over the same camera's unfogged one (01/05 classic sky,
#      10/12 pools, 20/22 alias, 26/27 tombs, 31/33 particles, 40/42
#      sprites, 60/64 and 65/69 the skybox): >= FOGADD_MIN (default 42).
#   4. 45-glows-fogb against 47-noglows-fogb: what the glows add under fog B,
#      after their CPU-side fog attenuation, on each side; PSNR >= GLOW_MIN
#      (default 60) over the pixels where the two sides' no-glow frames agree
#      within 2%, at most GLOW_EXCLUDE_MAX (default 20000) pixels left out --
#      tools/particles-diff.sh's glow check, under fog.
#
# FRAMES=<names> narrows every check to those frames (a check whose frames
# are not all in it is skipped): the "-game skel" run of the steps file
# compares FRAMES="20-alias ... 28-tombs-inst-fogb" alone.
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,50p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
SCENE_MIN=${SCENE_MIN:-28}
ADD_MIN=${ADD_MIN:-55}
FOGADD_MIN=${FOGADD_MIN:-42}
SQUARE_TOL=${SQUARE_TOL:-0.10}
GLOW_MIN=${GLOW_MIN:-60}
GLOW_EXCLUDE_MAX=${GLOW_EXCLUDE_MAX:-20000}

FRAMES=${FRAMES:-"01-sky 02-sky-skyfog0 03-sky-skyfog1 04-sky-fogb 05-sky-nofog 06-sky-twopass 07-sky-fast
10-pools 11-pools-fogb 12-pools-nofog 13-lowlight 14-lowlight-fogb 15-under 16-under-fogb
20-alias 21-alias-fogb 22-alias-nofog 23-alias-inst 24-alias-inst-fogb
25-tombs 26-tombs-fogb 27-tombs-nofog 28-tombs-inst-fogb
30-noparticles 31-particles 32-particles-fogb 33-particles-nofog 34-square
40-sprites 41-sprites-fogb 42-sprites-nofog 43-nosprites
44-glows 45-glows-fogb 46-glows-nofog 47-noglows-fogb
50-c4-lava 51-c4-lava-la05 52-c4-lava-la05-fogb
60-box-north 61-box-north-skyfog0 62-box-north-skyfog1 63-box-north-fogb 64-box-north-nofog
65-box-battlement 66-box-battlement-skyfog0 67-box-battlement-skyfog1 68-box-battlement-fogb
69-box-battlement-nofog"}

command -v magick >/dev/null || { echo "fog-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
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
# "inf" or a number, >= min
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# pixels that differ at all, in any channel; and the summed difference
changed() {
	dif "$1" "$2" "$T/c.png"
	magick "$T/c.png" -separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
summed() { dif "$1" "$2" "$T/d.png"; magick "$T/d.png" -format '%[fx:mean*w*h*3]' info:; }
# b within tol of a, a nonzero
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { exit !(a > 0 && b >= a * (1 - t) && b <= a * (1 + t)) }'; }
# every named frame is in FRAMES
has() { for f in "$@"; do case " $(echo $FRAMES) " in *" $f "*) ;; *) return 1 ;; esac; done; }
# the two sides' |on - off| against each other: PSNR
addpsnr() {
	dif "$GL/$1.png" "$GL/$2.png" "$T/da.png"
	dif "$GPU/$1.png" "$GPU/$2.png" "$T/db.png"
	magick compare -metric PSNR "$T/da.png" "$T/db.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-30s OK    (%s)\n' "$1" "$3"
	else printf '%-30s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		if [ ! -f "$d/$n.png" ]; then
			printf '%-30s MISSING in %s\n' "$n" "$d"; rc=1
		else
			sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
			[ "$sz" = 800x600 ] || { printf '%-30s DIFF  (size %s in %s, want 800x600)\n' "$n" "${sz:-unreadable}" "$d"; rc=1; }
		fi
	done
done
[ $rc = 0 ] || { echo "fog-diff: FRAMES MISSING OR THE WRONG SIZE"; exit 1; }

for n in $FRAMES; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	[ "$p" = 120 ] && p=inf
	case $n in
	4[4-7]-*glows*) atleast "$p" "$SCENE_MIN"; report "$n" $? "PSNR $p dB, scene floor $SCENE_MIN" ;;
	*)              atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN" ;;
	esac
done

if has 34-square 30-noparticles; then
	cg=$(changed "$GL/34-square.png" "$GL/30-noparticles.png")
	cp=$(changed "$GPU/34-square.png" "$GPU/30-noparticles.png")
	sg=$(summed "$GL/34-square.png" "$GL/30-noparticles.png")
	sp=$(summed "$GPU/34-square.png" "$GPU/30-noparticles.png")
	near "$cg" "$cp" "$SQUARE_TOL" && near "$sg" "$sp" "$SQUARE_TOL"
	report "square particles under fog" $? "pixels changed GL $cg / -gpu $cp, summed change $sg / $sp, tolerance $SQUARE_TOL"
fi

for pr in "31-particles 30-noparticles particles" "40-sprites 43-nosprites sprites"; do
	set -- $pr
	has "$1" "$2" || continue
	p=$(addpsnr "$1" "$2")
	atleast "$p" "$ADD_MIN"; report "$3 under fog" $? "what $1 adds over $2: PSNR $p dB, min $ADD_MIN"
done

for pr in 01-sky:05-sky-nofog 10-pools:12-pools-nofog 20-alias:22-alias-nofog 26-tombs-fogb:27-tombs-nofog \
	  31-particles:33-particles-nofog 40-sprites:42-sprites-nofog 60-box-north:64-box-north-nofog \
	  65-box-battlement:69-box-battlement-nofog; do
	a=${pr%%:*}; b=${pr#*:}
	has "$a" "$b" || continue
	p=$(addpsnr "$a" "$b")
	atleast "$p" "$FOGADD_MIN"; report "fog over $b" $? "what the fog adds, $a over $b: PSNR $p dB, min $FOGADD_MIN"
done

if has 45-glows-fogb 47-noglows-fogb; then

dif "$GL/45-glows-fogb.png" "$GL/47-noglows-fogb.png" "$T/ga.png"
dif "$GPU/45-glows-fogb.png" "$GPU/47-noglows-fogb.png" "$T/gb.png"
mask "$GL/47-noglows-fogb.png" "$T/x.png"; mask "$GPU/47-noglows-fogb.png" "$T/y.png"
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
report "glows under fog" $? "what the glows add: PSNR $p dB, min $GLOW_MIN, $ex pixels excluded, max $GLOW_EXCLUDE_MAX"
fi

[ $rc -eq 0 ] && echo "fog-diff: all checks pass" || echo "fog-diff: DIFFERENCES FOUND"
exit $rc
