#!/usr/bin/env bash
#
# softemu-diff.sh -- the checks for tools/sweep-softemu-gpu.steps: r_softemu's
# palette emulation, GL's frames against the SDL_GPU backend's (#340 P4d).
# See the steps file's header for how to take the two runs and where the
# thresholds come from.
#
# Usage:
#   softemu-diff.sh <gl-dir> <gpu-dir> <game-dir>
#
# <game-dir> is the data1 the runs used: its gfx/palette.lmp is dumped with
# tools/softemu-palette.py.  Every frame is compared with the mini status
# bar's health masked (210x24+100+576: it reads 83 or 85 by run), and:
#
#   0. Both sides have every frame, each 800x600, the size the masks and
#      thresholds are for, and each run is the backend it is passed as: the
#      GL run's log says "gpu_unrouted: not in SDL_GPU mode", the -gpu run's
#      has renderer_status's "[RENDERER] SDL_GPU" (a -gpu run in both
#      places would agree with itself on every check).
#   1. PSNR against GL: the r_softemu-off frames (01, 11) >= PSNR_MIN
#      (default 42), every r_softemu frame >= SOFTEMU_MIN (default 40):
#      palette snapping turns a rasterizer's last-bit differences into whole
#      palette steps (sweep-pp-gpu.steps' 26-28 have the same gate).
#   2. Palette membership, on each side: every pixel of every r_softemu
#      frame (the final pass covers the whole window, HUD included) is one
#      of gfx/palette.lmp's entries 0-254, the colours the LUTs emit.  And
#      the off frames (x1) hold at least MIN_CHANGED pixels that are not,
#      or the check could not tell a snapped frame from an unsnapped one.
#   3. The metric is live, on each side (the graveyard, which repeats
#      exactly): 05 (naive) and 06 (Riemersma) each differ from 02 (OKLab,
#      r_softemu_metric -1) and from each other by at least MIN_METRIC
#      (default 500) pixels -- a rebuild that never reached the texture
#      leaves them equal -- and 07, back at -1, equals 02 exactly.
#   4. r_dither is live, on each side: 08 (r_dither 0) differs from 02
#      (0.5) by at least MIN_CHANGED (default 2000) pixels.
#   5. The dither follows r_scale, on each side: in 09 and 19 (r_softemu 1,
#      r_scale 0.5) every 2x2 block of the 3D view (the top 560 rows) is one
#      colour.  bayer16 reads gl_FragCoord * scale, so the dither cell is
#      the upscaled pixel; a dither at native resolution breaks the blocks.
#   6. The -gpu run's own reports (<gpu-dir>/qconsole.log): the GY and POOLS
#      REPORT-<name>-BEGIN / -END sections, reset ahead of each camera's first
#      shot so they span every shot state and both metric rebuilds, count 0
#      calls into the GL entry points and 0 routed calls in gpu_unrouted
#      (tools/softemu-palette.py reports: a section missing, unclosed or
#      without both counts fails).  No "Validation Error" in the log or
#      engine.stdout, and engine.stdout must show the layers were loaded:
#      at least one "Validation Warning" (SDL's 3D-texture barriers log
#      five a run, see the steps header), else a 0 would prove nothing.
#      REPORT-BOOT, the boot's own count, is printed, not checked.
#
# THE THRESHOLDS are tabled in tools/sweep-softemu-gpu.steps' header: the
# honest numbers they were set from and the broken builds that fail them.
#
# FRAMES=<names> narrows every check to those frames (a check whose frames
# are not all in it is skipped).
#
# Which LUT r_softemu 3 reads (the colormap one, 32^3) has no structural
# check: the frame is palette colours either way, and its world shaders band
# the lightmaps whatever the post-process reads.  Check 1 is what tells the
# two tables apart (see the steps header's broken builds).
#
# Exit status is 1 if any check fails.  Requires ImageMagick and python3.
set -uo pipefail

[ $# -eq 3 ] || { sed -n '2,55p' "$0"; exit 2; }
GL=$1; GPU=$2; GAME=$3
PSNR_MIN=${PSNR_MIN:-42}
SOFTEMU_MIN=${SOFTEMU_MIN:-40}
MIN_CHANGED=${MIN_CHANGED:-2000}
MIN_METRIC=${MIN_METRIC:-500}
SECTIONS="GY POOLS"
HERE=$(cd "$(dirname "$0")" && pwd)

FRAMES=${FRAMES:-"01-gy-off 02-gy-se1 03-gy-se2 04-gy-se3 05-gy-se1-naive 06-gy-se1-riem 07-gy-se1-auto
08-gy-se1-dither0 09-gy-se1-scale050 11-pools-off 12-pools-se1 13-pools-se2 14-pools-se3 15-pools-se1-naive
16-pools-se1-riem 17-pools-se1-auto 18-pools-se1-dither0 19-pools-se1-scale050"}

command -v magick >/dev/null || { echo "softemu-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

python3 "$HERE/softemu-palette.py" dump "$GAME" > "$T/palette.txt" ||
	{ echo "softemu-diff: no palette from $GAME" >&2; exit 2; }

HUD="rectangle 100,576 309,599"
mask() { magick "$1" -alpha off -fill black -draw "$HUD" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
# "inf" or a number, >= min
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# pixels that differ at all, in any channel, masked
changed() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off \
		-separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
# pixels of the 3D view (top 560 rows) a 2x2 block disagrees on
blocky() {
	magick "$1" -alpha off -crop 800x560+0+0 +repage "$T/v.png"
	magick "$T/v.png" -sample 50% -sample 200% "$T/w.png"
	magick "$T/v.png" "$T/w.png" -compose difference -composite \
		-separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
# "<count> <examples>" of the pixels not among palette entries 0-254, unmasked:
# the HUD goes through the final pass too
outside() {
	magick "$1" -alpha off -depth 8 rgb:- | python3 "$HERE/softemu-palette.py" outside "$T/palette.txt" 800
}
has() { for f in "$@"; do case " $(echo $FRAMES) " in *" $f "*) ;; *) return 1 ;; esac; done; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}
sides() { echo GL GPU; }

for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		if [ ! -f "$d/$n.png" ]; then
			printf '%-32s MISSING in %s\n' "$n" "$d"; rc=1
		else
			sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
			[ "$sz" = 800x600 ] || { printf '%-32s DIFF  (size %s in %s, want 800x600)\n' "$n" "${sz:-unreadable}" "$d"; rc=1; }
		fi
	done
done
grep -qs "^gpu_unrouted: not in SDL_GPU mode" "$GL/qconsole.log" ||
	{ printf '%-32s DIFF  (%s)\n' "GL run" "$GL/qconsole.log does not say GL (no 'not in SDL_GPU mode')"; rc=1; }
grep -qs "^\[RENDERER\] SDL_GPU" "$GPU/qconsole.log" ||
	{ printf '%-32s DIFF  (%s)\n' "-gpu run" "$GPU/qconsole.log does not say SDL_GPU (no renderer_status line)"; rc=1; }
[ $rc = 0 ] || { echo "softemu-diff: FRAMES MISSING, THE WRONG SIZE OR THE WRONG BACKEND"; exit 1; }

# 1
for n in $FRAMES; do
	case $n in
	?1-*) min=$PSNR_MIN ;;
	*)    min=$SOFTEMU_MIN ;;
	esac
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$min"; report "$n" $? "PSNR $p dB, min $min"
done

# 2
for n in $FRAMES; do
	for side in $(sides); do
		eval d=\$$side
		o=$(outside "$d/$n.png"); c=${o%% *}
		case $n in
		?1-*) [ "${c:-0}" -ge "$MIN_CHANGED" ]
		      report "unsnapped ${n%%-*} $side" $? "$side: $c pixels off the palette with r_softemu 0, min $MIN_CHANGED" ;;
		*)    [ "${c:-1}" = 0 ]
		      report "palette ${n%%-*} $side" $? "$side: ${o:-?} pixels off the palette, want 0" ;;
		esac
	done
done

# 3
if has 02-gy-se1 05-gy-se1-naive 06-gy-se1-riem 07-gy-se1-auto; then
	for side in $(sides); do
		eval d=\$$side
		a=$(changed "$d/05-gy-se1-naive.png" "$d/02-gy-se1.png"); a=${a:-0}
		b=$(changed "$d/06-gy-se1-riem.png" "$d/02-gy-se1.png"); b=${b:-0}
		c=$(changed "$d/05-gy-se1-naive.png" "$d/06-gy-se1-riem.png"); c=${c:-0}
		ok=1; [ "$a" -ge "$MIN_METRIC" ] && [ "$b" -ge "$MIN_METRIC" ] && [ "$c" -ge "$MIN_METRIC" ] && ok=0
		report "metric rebuild $side" $ok "$side: naive/OKLab $a, Riemersma/OKLab $b, naive/Riemersma $c pixels differ, min $MIN_METRIC"
		e=$(changed "$d/07-gy-se1-auto.png" "$d/02-gy-se1.png")
		[ "${e:-1}" = 0 ]; report "metric back to -1 $side" $? "$side: 07 against 02, $e pixels differ, want 0"
	done
fi

# 4
if has 08-gy-se1-dither0 02-gy-se1; then
	for side in $(sides); do
		eval d=\$$side
		c=$(changed "$d/08-gy-se1-dither0.png" "$d/02-gy-se1.png"); c=${c:-0}
		[ "$c" -ge "$MIN_CHANGED" ]; report "dither $side" $? "$side: 08 against 02, $c pixels differ, min $MIN_CHANGED"
	done
fi

# 5
for n in 09-gy-se1-scale050 19-pools-se1-scale050; do
	has "$n" || continue
	for side in $(sides); do
		eval d=\$$side
		c=$(blocky "$d/$n.png")
		[ "${c:-1}" = 0 ]; report "2x2 blocks ${n%%-*} $side" $? "$side: $c pixels of the 3D view off their 2x2 block, want 0"
	done
done

# 6
if [ ! -f "$GPU/qconsole.log" ]; then
	report "gpu reports" 1 "no $GPU/qconsole.log"
else
	# shellcheck disable=SC2086 # SECTIONS is a word list
	bad=$(python3 "$HERE/softemu-palette.py" reports "$GPU/qconsole.log" $SECTIONS)
	ok=$?	# a parser that crashed printed nothing: its status still fails
	bad=$(printf '%s' "$bad" | paste -sd ';' -)
	report "gpu_unrouted" $ok "${bad:-0 in REPORT $SECTIONS}"
	b=$(sed -n '/^REPORT-BOOT-BEGIN/,/^REPORT-BOOT-END/p' "$GPU/qconsole.log" | grep '^gpu_unrouted: [0-9]* calls into')
	printf '%-32s INFO  (%s, not checked: see the steps header)\n' "gpu_unrouted boot" "${b:-no REPORT-BOOT}"
	v=$(cat "$GPU/qconsole.log" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
	w=$(grep -cs "Validation Warning" "$GPU/engine.stdout"); w=${w:-0}
	ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
	report "validation" $ok "$v Validation Error lines; $w Warnings, want > 0 (the layers loaded)"
fi

[ $rc -eq 0 ] && echo "softemu-diff: all checks pass" || echo "softemu-diff: DIFFERENCES FOUND"
exit $rc
