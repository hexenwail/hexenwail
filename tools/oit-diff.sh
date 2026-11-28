#!/usr/bin/env bash
#
# oit-diff.sh -- the checks for tools/sweep-oit-gpu.steps: GL's frames
# against the SDL_GPU backend's, order-independent transparency under -gpu
# (#340 P6c).  See the steps file's header for how to take the two runs.
#
# Usage:
#   oit-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (210x24+100+576 at 800x600; at any other size, which the Tesla P4's
# offscreen runs come out at, 640x480, the bottom 24 rows), and:
#
#   0. Both sides have every frame, all of one size.
#   1. Every frame but the bloom pair: PSNR against GL >= PSNR_MIN.  14-15
#      (bloom) are printed, not checked: bloom under -gpu differs from GL's
#      on master already (the P5f sweep's finding), and 3 checks what OIT
#      adds to them instead.
#   2. Each side's OIT is there: 08 (r_oit 1) against 07 (r_oit 0), the
#      overlapping sprites and sheep, and 09 (r_oit 2) against 08 each
#      change at least MIN_CHANGED pixels.  Two sides that both lost OIT
#      would agree in 1.
#   3. What OIT adds, the same on both sides: for each (r_oit 1, r_oit 0)
#      pair -- 02/01, 05/04, 08/07, 11/10, 15/14, 21/22 -- and each (r_oit 2,
#      r_oit 1) pair -- 03/02, 06/05, 09/08, 12/11 -- the pixels that
#      change, and the summed change (every channel's |a - b|, in 1/255),
#      on -gpu within REL (default 5%) of GL's, or within ABS_PX pixels /
#      ABS_SUM when GL's is that small (one translucent layer resolves to
#      what sorted blending gives, so a pair can change next to nothing).
#      17/18 (map_checks 1, r_oit 1 against 0) is printed, not checked:
#      map_checks makes OIT_Active false, which picks the plain programs,
#      but OIT_BeginTranslucency asks r_oit alone, so the pass still runs
#      into both targets with only location 0 written -- revealage blends
#      an unwritten output, undefined on GL and Vulkan alike (a GL bug of
#      its own; 0 pixels on llvmpipe, 899 on the Tesla P4, both backends).
#   4. Frames that must be identical, on each side: 19 (r_oit 2 -> 1 -> 0)
#      = 01; 20 (r_oit 2 again) = 03.
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,36p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
MIN_CHANGED=${MIN_CHANGED:-2000}
REL=${REL:-0.05}
ABS_PX=${ABS_PX:-300}
ABS_SUM=${ABS_SUM:-3000}

FRAMES="01-pools-oit0 02-pools-oit1 03-pools-oit2 04-pools-wa06-oit0 05-pools-wa06-oit1 06-pools-wa06-oit2
07-trans-oit0 08-trans-oit1 09-trans-oit2 10-part-oit0 11-part-oit1 12-part-oit2 13-pools-oit1-scale05
14-pools-bloom-oit0 15-pools-bloom-oit1 16-pools-oit1-softemu2 17-pools-checks-oit1 18-pools-checks-oit0
19-pools-oit-back0 20-pools-oit2-again 21-pools-vm-oit1 22-pools-vm-oit0"

command -v magick >/dev/null || { echo "oit-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

SIZE=$(magick identify -format '%wx%h' "$GL/01-pools-oit0.png" 2>/dev/null)
case $SIZE in
800x600) HUD="rectangle 100,576 309,599" ;;
*x*)     HUD="rectangle 0,$(( ${SIZE#*x} - 24 )) $(( ${SIZE%x*} - 1 )),$(( ${SIZE#*x} - 1 ))" ;;
*)       echo "oit-diff: no $GL/01-pools-oit0.png"; exit 1 ;;
esac
mask() { magick "$1" -alpha off -fill black -draw "$HUD" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# pixels that differ at all, in any channel, masked
changed() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off \
		-separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
# every channel's |a - b| summed, in 1/255
summed() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off \
		-format '%[fx:int(mean*w*h*3*255+0.5)]' info:
}
# a within REL of b, or both within abs
near() { awk -v a="$1" -v b="$2" -v r="$REL" -v m="$3" 'BEGIN {
	d = a - b; if (d < 0) d = -d; exit !(d <= m || d <= r * b) }'; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-34s OK    (%s)\n' "$1" "$3"
	else printf '%-34s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

# 0
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		if [ ! -f "$d/$n.png" ]; then printf '%-34s MISSING in %s\n' "$n" "$d"; rc=1; continue; fi
		sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
		[ "$sz" = "$SIZE" ] || { printf '%-34s DIFF  (size %s in %s)\n' "$n" "${sz:-unreadable}" "$d"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "oit-diff: frames missing"; exit 1; }

# 1
for n in $FRAMES; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	case $n in
	14-*|15-*) printf '%-34s INFO  (PSNR %s dB, bloom: not checked)\n' "$n" "$p" ;;
	*) atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN" ;;
	esac
done

# 2
for side in GL GPU; do
	eval d=\$$side
	c=$(changed "$d/08-trans-oit1.png" "$d/07-trans-oit0.png"); c=${c:-0}
	[ "$c" -ge "$MIN_CHANGED" ]; report "oit1 effect $side" $? "$side: 08 against 07, $c pixels, min $MIN_CHANGED"
	c=$(changed "$d/09-trans-oit2.png" "$d/08-trans-oit1.png"); c=${c:-0}
	[ "$c" -ge "$MIN_CHANGED" ]; report "oit2 effect $side" $? "$side: 09 against 08, $c pixels, min $MIN_CHANGED"
done

# 3
for pair in 02-pools-oit1:01-pools-oit0 05-pools-wa06-oit1:04-pools-wa06-oit0 08-trans-oit1:07-trans-oit0 \
	    11-part-oit1:10-part-oit0 15-pools-bloom-oit1:14-pools-bloom-oit0 21-pools-vm-oit1:22-pools-vm-oit0 \
	    03-pools-oit2:02-pools-oit1 06-pools-wa06-oit2:05-pools-wa06-oit1 09-trans-oit2:08-trans-oit1 \
	    12-part-oit2:11-part-oit1; do
	a=${pair%%:*}; b=${pair##*:}
	cg=$(changed "$GL/$a.png" "$GL/$b.png"); cv=$(changed "$GPU/$a.png" "$GPU/$b.png")
	sg=$(summed "$GL/$a.png" "$GL/$b.png"); sv=$(summed "$GPU/$a.png" "$GPU/$b.png")
	near "${cv:-0}" "${cg:-0}" "$ABS_PX"; report "adds px ${a%%-*}-${b%%-*}" $? "GL $cg, -gpu $cv pixels"
	near "${sv:-0}" "${sg:-0}" "$ABS_SUM"; report "adds sum ${a%%-*}-${b%%-*}" $? "GL $sg, -gpu $sv"
done

c=$(changed "$GL/17-pools-checks-oit1.png" "$GL/18-pools-checks-oit0.png")
v=$(changed "$GPU/17-pools-checks-oit1.png" "$GPU/18-pools-checks-oit0.png")
printf '%-34s INFO  (GL %s, -gpu %s pixels; undefined, not checked)\n' "map_checks 17-18" "$c" "$v"

# 4
for side in GL GPU; do
	eval d=\$$side
	for pair in 19-pools-oit-back0:01-pools-oit0 20-pools-oit2-again:03-pools-oit2; do
		a=${pair%%:*}; b=${pair##*:}
		c=$(changed "$d/$a.png" "$d/$b.png")
		[ "${c:-1}" = 0 ]; report "same ${a%%-*}=${b%%-*} $side" $? "$side: $c pixels differ, want 0"
	done
done

[ $rc -eq 0 ] && echo "oit-diff: all checks pass" || echo "oit-diff: DIFFERENCES FOUND"
exit $rc
