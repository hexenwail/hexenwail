#!/usr/bin/env bash
#
# softpart-diff.sh -- the checks for tools/sweep-softpart-gpu.steps: GL's
# frames against the SDL_GPU backend's, soft particles (#340 P4b).  See the
# steps file's header for how to take the two runs and where the thresholds
# come from.
#
# Usage:
#   softpart-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (210x24+100+576, as in tools/pp-diff.sh), and:
#
#   0. Both sides have every frame, each 800x600, the size the mask and
#      thresholds are for.
#   1. Every frame: PSNR against GL >= PSNR_MIN (default 42), as
#      tools/shot-diff.sh's PSNR_MIN mode.
#   2. The fade is there, and the same size, on each side: for each frame
#      with r_softparticles 1 and its partner at 0 (02 and 03 against 01,
#      04, 05 and 07 against their -off frames, 08 against 07-off) count the
#      pixels that differ (any channel, any amount) and sum the
#      difference; on each side the count is at least MIN_CHANGED (default
#      50), and -gpu's count and sum are each within FADE_TOL (default 10%)
#      of GL's.  Two sides that both lost the fade would agree in 1, and a
#      fade at the wrong distance can hide in a whole-frame PSNR; this
#      cannot.
#   3. r_softparticles_scale 24 fades more than 8: on each side 03's summed
#      change against 01 is above 02's.
#   4. Sprites with nothing within the fade distance are untouched: on each
#      side 06-clear equals 06-clear-off exactly (0 pixels differ).  A fade
#      against the wrong depth -- a snapshot nothing wrote, a linearisation
#      off by a convention -- shows here first.
#   5. When <gpu-dir>/qconsole.log is there (tools/headless-drive.sh writes
#      it): every REPORT-<name>-BEGIN / -END section has both gpu_unrouted
#      counts, at 0 calls and 0 unhandled routed calls, and neither it nor
#      <gpu-dir>/engine.stdout has a "Validation Error" (run with the
#      validation layers and +gpu_debug 1).
#
# FRAMES=<names> narrows checks 1-4 to those frames (a check whose frames are
# not all in it is skipped).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,40p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
MIN_CHANGED=${MIN_CHANGED:-50}
FADE_TOL=${FADE_TOL:-0.10}

ALL="01-off 02-soft 03-scale24 04-rscale 04-rscale-off 05-nopp 05-nopp-off 06-clear 06-clear-off
07-mirror 08-mirror24 07-mirror-off"
FRAMES=${FRAMES:-$ALL}

command -v magick >/dev/null || { echo "softpart-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

want() {	# every named frame is in FRAMES
	local n
	for n in "$@"; do
		case " ${FRAMES//$'\n'/ } " in *" $n "*) ;; *) return 1 ;; esac
	done
}
mask() { magick "$1" -alpha off -fill black -draw "rectangle 100,576 309,599" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1}'
}
dif() { mask "$1" "$T/x.png"; mask "$2" "$T/y.png"; magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off "$3"; }
# pixels that differ at all, in any channel (compare's AE is not a count on
# IM 7.1.2: it weighs each pixel by how much it differs)
changed() {
	dif "$1" "$2" "$T/c.png"
	magick "$T/c.png" -separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
summed() { dif "$1" "$2" "$T/d.png"; magick "$T/d.png" -format '%[fx:mean*w*h*3]' info:; }
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# b within tol of a, a nonzero
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { exit !(a > 0 && b >= a * (1 - t) && b <= a * (1 + t)) }'; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-26s OK    (%s)\n' "$1" "$3"
	else printf '%-26s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { printf '%-26s MISSING in %s\n' "$n" "$d"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "softpart-diff: FRAMES MISSING"; exit 1; }
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
		[ "$sz" = 800x600 ] || { printf '%-26s DIFF  (%s: size %s, want 800x600)\n' "$n" "$d" "${sz:-unreadable}"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "softpart-diff: WRONG FRAME SIZE"; exit 1; }

# 1. every frame against GL
for n in $FRAMES; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done

# 2. the fade, on each side
for pair in 02-soft:01-off 03-scale24:01-off 04-rscale:04-rscale-off 05-nopp:05-nopp-off \
	    07-mirror:07-mirror-off 08-mirror24:07-mirror-off; do
	on=${pair%%:*}; off=${pair##*:}
	want "$on" "$off" || continue
	cg=$(changed "$GL/$on.png" "$GL/$off.png");  cp=$(changed "$GPU/$on.png" "$GPU/$off.png")
	sg=$(summed "$GL/$on.png" "$GL/$off.png");   sp=$(summed "$GPU/$on.png" "$GPU/$off.png")
	[ "$cg" -ge "$MIN_CHANGED" ] && [ "$cp" -ge "$MIN_CHANGED" ] && near "$cg" "$cp" "$FADE_TOL" && near "$sg" "$sp" "$FADE_TOL"
	report "fade $on" $? "pixels changed GL $cg / -gpu $cp, summed $sg / $sp, min $MIN_CHANGED, tolerance $FADE_TOL"
done

# 3. a longer fade distance fades more
if want 01-off 02-soft 03-scale24; then
	s8g=$(summed "$GL/02-soft.png" "$GL/01-off.png");    s24g=$(summed "$GL/03-scale24.png" "$GL/01-off.png")
	s8p=$(summed "$GPU/02-soft.png" "$GPU/01-off.png");  s24p=$(summed "$GPU/03-scale24.png" "$GPU/01-off.png")
	awk -v a="$s8g" -v b="$s24g" -v c="$s8p" -v d="$s24p" 'BEGIN { exit !(b > a && d > c) }'
	report "scale 24 > 8" $? "summed change at 8 / 24: GL $s8g / $s24g, -gpu $s8p / $s24p"
fi

# 4. nothing near: nothing fades
if want 06-clear 06-clear-off; then
	cg=$(changed "$GL/06-clear.png" "$GL/06-clear-off.png"); cp=$(changed "$GPU/06-clear.png" "$GPU/06-clear-off.png")
	[ "$cg" = 0 ] && [ "$cp" = 0 ]
	report "06 clear of geometry" $? "pixels changed GL $cg / -gpu $cp, want 0 / 0"
fi

# 5. the -gpu run's own reports
if [ -f "$GPU/qconsole.log" ]; then
	bad=$(awk '
		/^REPORT-.*-BEGIN/ { sec = $1; sub(/-BEGIN$/, "", sec); n++; calls = routed = 0 }
		/^REPORT-.*-END/ && sec != "" {
			if (!calls || !routed) print sec ": no gpu_unrouted count"
			sec = ""
		}
		sec != "" && /^gpu_unrouted: [0-9]+ calls into/ { calls = 1; if ($2 != 0) print sec ": " $0 }
		sec != "" && /^gpu_unrouted: [0-9]+ routed calls/ { routed = 1; if ($2 != 0) print sec ": " $0 }
		END { if (!n) print "no REPORT sections" }' "$GPU/qconsole.log")
	ok=0; [ -z "$bad" ] || ok=1
	report "gpu_unrouted" $ok "${bad:-0 in every REPORT section}"
	v=$(cat "$GPU/qconsole.log" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
	ok=0; [ "$v" = 0 ] || ok=1
	report "validation" $ok "$v Validation Error lines"
fi

[ $rc -eq 0 ] && echo "softpart-diff: all checks pass" || echo "softpart-diff: DIFFERENCES FOUND"
exit $rc
