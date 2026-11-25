#!/usr/bin/env bash
#
# bloom-diff.sh -- the checks for tools/sweep-bloom-gpu.steps: GL's frames
# against the SDL_GPU backend's, bloom (r_bloom) and HDR (r_hdr) (#340
# P4e).  See the steps file's header for how to take the two runs and where
# the thresholds come from.
#
# Usage:
#   bloom-diff.sh <gl-dir> <gpu-dir>
#
# The mini status bar's health (210x24+100+576, as in tools/pp-diff.sh) is
# masked everywhere, and:
#
#   0. Both sides have every frame, each 800x600.
#   1. Every frame: PSNR against GL >= PSNR_MIN (default 42); a frame with
#      bloom on >= BLOOM_PSNR_MIN (default 36), since bloom spreads the
#      bright pass's per-texel differences (below) over its whole halo --
#      the Tesla P4's graveyard sky at threshold 0.3 and intensity 2 reads
#      39.8.  On bloom frames this catches only gross faults (a placeholder
#      or zeros in a pass, 1-9 dB); check 2 is the bloom's own.
#   2. Bloom is there and is GL's, frame by frame.  Its contribution on each
#      side is the bloom frame minus its partner with bloom off (the same
#      view, the same r_hdr and exposure), and:
#        count   it changes at least MIN_CHANGED (default 2000) pixels on each
#                side, the two counts apart by at most COUNT_TOL (default
#                0.10) of the larger or COUNT_BUDGET (default 2000) pixels,
#                whichever is more;
#        energy  its sum on -gpu over GL's is within ENERGY_LO .. ENERGY_HI
#                (default 0.45 .. 2.2);
#        shape   averaged over 16x16 blocks, the RMS of the two sides'
#                difference over the RMS of GL's is at most SHAPE_MAX
#                (default 0.65).
#      Pixel by pixel the two sides cannot agree closely: the bright pass
#      samples the NEAREST-filtered native target at its texel corners,
#      where each implementation breaks the tie its own way, and the LDR
#      pyramid rounds faint glow to 8 bits; the block average and the sum
#      are what that leaves alone.  An upsample that overwrites instead of
#      adding loses the wide halo (energy), a pyramid level drawn at the
#      wrong size moves it (shape), a pass reading zeros or a placeholder
#      floods or drops it (every one).
#  2i. r_bloom_intensity is applied: on each side the contribution at
#      intensity 2 over the same view's at 1 (g05/g03, t04/t03, t05/t06) is
#      within INTENSITY_LO .. INTENSITY_HI (default 1.5 .. 2.1; under 2 where
#      the sum clips at 1), and the two sides' ratios within INTENSITY_TOL
#      (default 0.15) of each other.  An intensity ignored reads 1.0.
#   3. A threshold above the brightest pixel is bloom off: nothing in an
#      RGBA8 target is above 1.0, so at threshold 1.0 in LDR the bright pass
#      keeps nothing and the composite adds 0, and nothing in the graveyard
#      is above 0.8 (its brightest luminance is 0.61, the clouds).  On each
#      side g02 and g04 equal g01, and t02 equals t01, exactly (01 is the
#      chain up with no effect: r_waterwarp 1 above water).
#   4. HDR is there: the ACES frame (g06, t07) changes at least MIN_CHANGED
#      pixels against the chain with no effect (01), and exposure 2 (g07,
#      t08) against exposure 1, on each side, the two sides' counts within
#      COUNT_TOL.
#   5. Nothing is left behind: on each side g09-after (bloom and HDR back
#      off) equals g00-off exactly.  The upsample's GL_ONE, GL_ONE once
#      outlived the frame (#547), and r_hdr 0 must rebuild the RGBA8 chain.
#   6. When <gpu-dir>/qconsole.log is there (tools/headless-drive.sh writes
#      it): every REPORT-<name>-BEGIN / -END section has both gpu_unrouted
#      counts at 0, and neither it nor <gpu-dir>/engine.stdout has a
#      "Validation Error" (+gpu_debug 1 and the validation layers).
#
# FRAMES=<names> narrows the checks to those frames (a check whose frames
# are not all in it is skipped).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,67p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
BLOOM_PSNR_MIN=${BLOOM_PSNR_MIN:-36}
MIN_CHANGED=${MIN_CHANGED:-2000}
COUNT_TOL=${COUNT_TOL:-0.10}
COUNT_BUDGET=${COUNT_BUDGET:-2000}
ENERGY_LO=${ENERGY_LO:-0.45}
ENERGY_HI=${ENERGY_HI:-2.2}
SHAPE_MAX=${SHAPE_MAX:-0.65}
INTENSITY_LO=${INTENSITY_LO:-1.5}
INTENSITY_HI=${INTENSITY_HI:-2.1}
INTENSITY_TOL=${INTENSITY_TOL:-0.15}

ALL="g00-off g01-chain g02-ldr-t10 g03-t03-i1 g04-t08-i1 g05-t03-i2 g06-hdr-e1 g07-hdr-e2 g08-hdr-bloom
g09-after t00-off t01-chain t02-ldr-t10 t03-t05-i1 t04-t05-i2 t05-t08-i2 t06-t08-i1 t07-hdr-e1 t08-hdr-e2
t09-hdr-bloom t10-hdr-t10"
FRAMES=${FRAMES:-$ALL}

command -v magick >/dev/null || { echo "bloom-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
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
# pixels that differ at all, in any channel
changed() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick "$T/x.png" "$T/y.png" -compose difference -composite -alpha off \
		-separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# what the bloom adds: on minus off (it only adds; in HDR through ACES, which
# is monotone), the status bar masked
contrib() {
	mask "$1" "$T/on.png"; mask "$2" "$T/off.png"
	magick "$T/on.png" "$T/off.png" -compose minus_src -composite -alpha off "$3"
}
msq() { magick "$1" -evaluate pow 2 -format '%[fx:mean]' info:; }
# 16x16 block averages of an 800x600 contribution
blocks() { magick "$1" -filter box -resize '50x38!' "$2"; }
# both counts >= MIN_CHANGED, and apart by at most COUNT_TOL of the larger
# or ${3:-0} pixels, whichever is more
agree() {
	awk -v a="$1" -v b="$2" -v m="$MIN_CHANGED" -v t="$COUNT_TOL" -v x="${3:-0}" 'BEGIN {
		hi = (a > b) ? a : b; lo = (a > b) ? b : a
		lim = t * hi; if (x > lim) lim = x
		exit !(lo >= m && hi > 0 && hi - lo <= lim) }'
}
spread() { awk -v a="$1" -v b="$2" 'BEGIN { hi = (a > b) ? a : b; printf "%.1f%%", hi ? 100 * (a > b ? a - b : b - a) / hi : 0 }'; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-26s OK    (%s)\n' "$1" "$3"
	else printf '%-26s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { printf '%-26s MISSING in %s\n' "$n" "$d"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "bloom-diff: FRAMES MISSING"; exit 1; }
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
		[ "$sz" = 800x600 ] || { printf '%-26s DIFF  (%s: size %s, want 800x600)\n' "$n" "$d" "${sz:-unreadable}"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "bloom-diff: WRONG FRAME SIZE"; exit 1; }

# 1. every frame against GL
for n in $FRAMES; do
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	min=$PSNR_MIN
	case $n in g0[358]-*|t0[3-6]-*|t09-*|t10-*) min=$BLOOM_PSNR_MIN ;; esac
	atleast "$p" "$min"; report "$n" $? "PSNR $p dB, min $min"
done

# 2. bloom against its partner without it, on each side
for pair in g03-t03-i1:g01-chain g05-t03-i2:g01-chain g08-hdr-bloom:g06-hdr-e1 \
	    t03-t05-i1:t01-chain t04-t05-i2:t01-chain t05-t08-i2:t01-chain t06-t08-i1:t01-chain \
	    t09-hdr-bloom:t07-hdr-e1 t10-hdr-t10:t07-hdr-e1; do
	on=${pair%%:*}; off=${pair##*:}
	want "$on" "$off" || continue
	cg=$(changed "$GL/$on.png" "$GL/$off.png"); cp=$(changed "$GPU/$on.png" "$GPU/$off.png")
	agree "$cg" "$cp" "$COUNT_BUDGET"
	report "bloom $on count" $? "pixels changed against $off: GL $cg / -gpu $cp ($(spread "$cg" "$cp") apart), min $MIN_CHANGED, tol $COUNT_TOL or $COUNT_BUDGET px"
	contrib "$GL/$on.png" "$GL/$off.png" "$T/eg.png"; contrib "$GPU/$on.png" "$GPU/$off.png" "$T/ep.png"
	sg=$(magick "$T/eg.png" -format '%[fx:mean]' info:); sp=$(magick "$T/ep.png" -format '%[fx:mean]' info:)
	e=$(awk -v g="$sg" -v p="$sp" 'BEGIN { printf "%.3f", (g > 0) ? p / g : 0 }')
	awk -v e="$e" -v lo="$ENERGY_LO" -v hi="$ENERGY_HI" 'BEGIN { exit !(e + 0 >= lo + 0 && e + 0 <= hi + 0) }'
	report "bloom $on energy" $? "-gpu over GL $e, want $ENERGY_LO .. $ENERGY_HI"
	blocks "$T/eg.png" "$T/bg.png"; blocks "$T/ep.png" "$T/bp.png"
	magick "$T/bg.png" "$T/bp.png" -compose difference -composite "$T/bd.png"
	sh=$(awk -v d="$(msq "$T/bd.png")" -v g="$(msq "$T/bg.png")" 'BEGIN { printf "%.3f", (g > 0) ? sqrt(d / g) : 99 }')
	awk -v s="$sh" -v m="$SHAPE_MAX" 'BEGIN { exit !(s + 0 <= m + 0) }'
	report "bloom $on shape" $? "16x16-block RMS apart over GL's $sh, max $SHAPE_MAX"
done

# 2i. r_bloom_intensity: intensity 2 adds about twice what 1 adds, on each side
for pair in g05-t03-i2:g03-t03-i1:g01-chain t04-t05-i2:t03-t05-i1:t01-chain t05-t08-i2:t06-t08-i1:t01-chain; do
	IFS=: read -r i2 i1 off <<<"$pair"
	want "$i2" "$i1" "$off" || continue
	res=""; ok=0; rs=""
	for side in "$GL" "$GPU"; do
		contrib "$side/$i2.png" "$side/$off.png" "$T/e2.png"; contrib "$side/$i1.png" "$side/$off.png" "$T/e1.png"
		r=$(awk -v a="$(magick "$T/e2.png" -format '%[fx:mean]' info:)" -v b="$(magick "$T/e1.png" -format '%[fx:mean]' info:)" \
			'BEGIN { printf "%.3f", (b > 0) ? a / b : 0 }')
		res="$res${res:+ / }$r"; rs="$rs $r"
		awk -v r="$r" -v lo="$INTENSITY_LO" -v hi="$INTENSITY_HI" 'BEGIN { exit !(r + 0 >= lo + 0 && r + 0 <= hi + 0) }' || ok=1
	done
	# shellcheck disable=SC2086
	set -- $rs
	awk -v a="$1" -v b="$2" -v t="$INTENSITY_TOL" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }' || ok=1
	report "intensity ${i2%%-*}/${i1%%-*}" $ok "contribution at intensity 2 over 1, GL / -gpu $res, want $INTENSITY_LO .. $INTENSITY_HI and within $INTENSITY_TOL"
done

# 3. a threshold above the brightest pixel is bloom off
for pair in g02-ldr-t10:g01-chain g04-t08-i1:g01-chain t02-ldr-t10:t01-chain; do
	on=${pair%%:*}; off=${pair##*:}
	want "$on" "$off" || continue
	cg=$(changed "$GL/$on.png" "$GL/$off.png"); cp=$(changed "$GPU/$on.png" "$GPU/$off.png")
	[ "$cg" = 0 ] && [ "$cp" = 0 ]
	report "bloom off ${on%%-*}" $? "pixels changed against $off: GL $cg / -gpu $cp, want 0"
done

# 4. HDR is there
for pair in g06-hdr-e1:g01-chain g07-hdr-e2:g06-hdr-e1 t07-hdr-e1:t01-chain t08-hdr-e2:t07-hdr-e1; do
	on=${pair%%:*}; off=${pair##*:}
	want "$on" "$off" || continue
	cg=$(changed "$GL/$on.png" "$GL/$off.png"); cp=$(changed "$GPU/$on.png" "$GPU/$off.png")
	agree "$cg" "$cp"
	report "hdr $on" $? "pixels changed against $off: GL $cg / -gpu $cp ($(spread "$cg" "$cp") apart), min $MIN_CHANGED, tol $COUNT_TOL"
done

# 5. nothing left behind
if want g09-after g00-off; then
	cg=$(changed "$GL/g09-after.png" "$GL/g00-off.png"); cp=$(changed "$GPU/g09-after.png" "$GPU/g00-off.png")
	[ "$cg" = 0 ] && [ "$cp" = 0 ]
	report "g09 after bloom and HDR" $? "pixels changed against g00-off: GL $cg / -gpu $cp, want 0"
fi

# 6. the -gpu run's own reports
if [ -f "$GPU/qconsole.log" ]; then
	bad=$(awk '
		/^REPORT-.*-BEGIN/ { if (sec != "") print sec ": no -END"; sec = $1; sub(/-BEGIN$/, "", sec); n++; calls = routed = 0 }
		/^REPORT-.*-END/ && sec != "" {
			if (!calls || !routed) print sec ": no gpu_unrouted count"
			sec = ""
		}
		sec != "" && /^gpu_unrouted: [0-9]+ calls into/ { calls = 1; if ($2 != 0) print sec ": " $0 }
		sec != "" && /^gpu_unrouted: [0-9]+ routed calls/ { routed = 1; if ($2 != 0) print sec ": " $0 }
		END { if (!n) print "no REPORT sections"; if (sec != "") print sec ": no -END" }' "$GPU/qconsole.log")
	ok=0; [ -z "$bad" ] || ok=1
	report "gpu_unrouted" $ok "${bad:-0 in every REPORT section}"
	v=$(cat "$GPU/qconsole.log" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
	ok=0; [ "$v" = 0 ] || ok=1
	report "validation" $ok "$v Validation Error lines"
else
	printf '%-26s SKIP  (no %s/qconsole.log: gpu_unrouted and validation not checked)\n' "reports" "$GPU"
fi

[ $rc -eq 0 ] && echo "bloom-diff: all checks pass" || echo "bloom-diff: DIFFERENCES FOUND"
exit $rc
