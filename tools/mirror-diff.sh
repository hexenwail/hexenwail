#!/usr/bin/env bash
#
# mirror-diff.sh -- the checks for tools/sweep-mirror-gpu.steps: GL's frames
# against the SDL_GPU backend's (#340 P3d).  See the steps file's header for
# how to take the two runs.
#
# Usage:
#   mirror-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (220x24+100+576: tools/sweep-world-gpu.steps' 200x24 and 20 more, since
# the digits reach x 315 here), the wall frames with the reflection's
# animated candle flames masked too (FLAMES below, five boxes, 1.3% of the
# frame), and:
#
#   0. Both sides have every frame the steps file shoots, each 800x600, the
#      size the mask and thresholds are for.
#   1. Every frame: PSNR against GL >= PSNR_MIN (default 42), as
#      tools/shot-diff.sh's PSNR_MIN mode.
#   2. What the mirror adds, so that a reflection drawn wrong cannot hide in
#      a frame the rest of which is right: on each side the difference
#      between the mirror frame and the same paused frame at r_mirroralpha
#      1 (no mirror), and the two sides' differences compared by PSNR >=
#      ADD_MIN (default 50).  01, 02 (r_mirroralpha 0.25) and 05 (fog)
#      against 03 / 06 on the wall, 10 against 11 on the floor.
#   3. What else a frame adds over 01 / 10, compared the same way: gl_cull 0
#      (04 over 01, 12 over 10 -- the back faces the reflected view culls,
#      so a reflected view culling the wrong side differs in both) and the
#      particles (07 over 01, drawn in the view and in the reflection),
#      PSNR >= ADD_MIN.
#   4. Each side's mirror really is there: what the mirror adds (2) changes
#      at least MIN_CHANGED (default 2000) pixels on GL and on -gpu.  Two
#      sides that both lost the mirror would agree in 1-3; and on GL alone,
#      the frames this checks are not the ones the thresholds were measured
#      on.
#
# THE THRESHOLDS are tabled in tools/sweep-mirror-gpu.steps' header: the
# honest numbers they were set from (measured 2026-11-21, 800x600, llvmpipe
# GL against lavapipe -gpu, three runs a side) and the broken builds that
# fail them.
#
# FRAMES=<names> narrows every check to those frames (a check whose frames
# are not all in it is skipped).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,45p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
ADD_MIN=${ADD_MIN:-50}
MIN_CHANGED=${MIN_CHANGED:-2000}

FRAMES=${FRAMES:-"01-wall 02-wall-a025 03-wall-a1 04-wall-nocull 05-wall-fog 06-wall-fog-a1 07-wall-parts
10-floor 11-floor-a1 12-floor-nocull"}

command -v magick >/dev/null || { echo "mirror-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

# The HUD's health, and on the wall frames (01-07) the candle flames on the
# altar the panel reflects: they animate, and a pause catches each at a frame
# of its own on every run -- GL against GL differs there and nowhere else.
# The boxes are the union of |run - run| over three runs a side, padded; they
# belong to the camera at "setpos -312 -2128 284 0 0 0", so moving it means
# re-deriving them.
FLAMES="rectangle 98,295 121,341 rectangle 280,304 301,341 rectangle 122,376 152,430 rectangle 305,357 331,426 rectangle 274,419 308,444"
mask() {
	case $(basename "$1") in
	0*-wall*) magick "$1" -alpha off -fill black -draw "rectangle 100,576 319,599 $FLAMES" "$2" ;;
	*)        magick "$1" -alpha off -fill black -draw "rectangle 100,576 319,599" "$2" ;;
	esac
}
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1}'
}
# |on - off|, masked
dif() { mask "$1" "$T/x.png"; mask "$2" "$T/y.png"; magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off "$3"; }
# "inf" or a number, >= min
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# pixels that differ at all, in any channel
changed() {
	dif "$1" "$2" "$T/c.png"
	magick "$T/c.png" -separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
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
[ $rc = 0 ] || { echo "mirror-diff: FRAMES MISSING OR THE WRONG SIZE"; exit 1; }

for n in $FRAMES; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	[ "$p" = 120 ] && p=inf
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done

for pr in "01-wall 03-wall-a1 mirror" "02-wall-a025 03-wall-a1 mirror" "05-wall-fog 06-wall-fog-a1 mirror" \
	  "10-floor 11-floor-a1 mirror" "04-wall-nocull 01-wall cull" "12-floor-nocull 10-floor cull" \
	  "07-wall-parts 01-wall particles"; do
	set -- $pr
	has "$1" "$2" || continue
	p=$(addpsnr "$1" "$2")
	atleast "$p" "$ADD_MIN"; report "$3 $1" $? "what $1 adds over $2: PSNR $p dB, min $ADD_MIN"
	if [ "$3" = mirror ]; then
		for side in GL GPU; do
			eval d=\$$side
			c=$(changed "$d/$1.png" "$d/$2.png"); c=${c:-0}
			[ "$c" -ge "$MIN_CHANGED" ]; report "mirror present $1 $side" $? "$side: $c pixels the mirror changes, min $MIN_CHANGED"
		done
	fi
done

[ $rc -eq 0 ] && echo "mirror-diff: all checks pass" || echo "mirror-diff: DIFFERENCES FOUND"
exit $rc
