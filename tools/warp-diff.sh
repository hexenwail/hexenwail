#!/usr/bin/env bash
#
# warp-diff.sh -- the checks for tools/sweep-warp-gpu.steps: GL's frames
# against the SDL_GPU backend's, the underwater screen warp and motion blur
# (#340 P4c).  See the steps file's header for how to take the two runs and
# where the thresholds come from.
#
# Usage:
#   warp-diff.sh <gl-dir> <gpu-dir>
#
# The mini status bar's health (210x24+100+576, as in tools/pp-diff.sh) is
# masked everywhere, and:
#
#   0. Both sides have every frame, each 800x600.
#   1. Every frame but the two turning ones (08): PSNR against GL >=
#      PSNR_MIN (default 42).
#   2. The warp is the shader's warp, on each side: tools/warp-model.py
#      predicts each warped frame from its unwarped partner (02, 03 and 09
#      from 01, 04 from 04-under-tint-nowarp, 05 from 05-under-scale-off at
#      r_scale 0.5, 10-wall-warp1 from 10-wall-off; 10 faces the pool wall,
#      where gl_rsurf.c's legacy vertex wobble would show)
#      with r_waterwarp and r_pp_time as the steps set them, and at most
#      MODEL_MAX (default 0.001) of the counted pixels disagree.  A warp
#      flipped in y, applied twice, at the wrong time, at the wrong
#      strength, missing, or replaced by the legacy vertex wobble (which
#      moves the world, not the image) is far off; NEAREST at a rounding
#      boundary is the honest remainder.
#   3. The warp is there: on each side 02 differs from 01 in at least
#      MIN_CHANGED (default 5000) pixels, and 09 (r_pp_time 3.7) from 02.
#   4. Above the water the warp is the identity: on -gpu 06-above-warp
#      equals 06-above-off exactly (r_waterwarp 1 wakes the chain, which
#      must then change nothing).  GL's two differ in some 2,500 scattered
#      texels by at most 15/255, its default framebuffer and its FBO
#      rounding apart (tools/pp-diff.sh check 2, the same on master), so
#      GL's count is printed, not checked.
#   5. Motion blur at rest is the identity within a rounding step: on each
#      side 07-above-blur differs from 06-above-warp (the chain up, no
#      effect) by at most 1 in any channel (its five taps' weights sum to
#      1.0).
#   6. Motion blur while turning blurs along the turn: on each side
#      08-turn-blur's anisotropy (the mean step between horizontal
#      neighbours over that between vertical ones) is at most BLUR_RATIO
#      (default 0.85) of 08-turn-noblur's.  Both are one still view: paused,
#      V_RenderView skips V_CalcRefdef (view.c), so the rendered view stays
#      where setpos put it, while +right keeps turning cl.viewangles, which
#      is what the blur's viewdelta reads.  The blur's length is the turn
#      per frame, so it follows the frame rate: structural, not frame
#      against frame.
#   7. When <gpu-dir>/qconsole.log is there (tools/headless-drive.sh writes
#      it): every REPORT-<name>-BEGIN / -END section has both gpu_unrouted
#      counts at 0, and neither it nor <gpu-dir>/engine.stdout has a
#      "Validation Error" (+gpu_debug 1 and the validation layers).
#
# FRAMES=<names> narrows the checks to those frames (a check whose frames
# are not all in it is skipped).
#
# Exit status is 1 if any check fails.  Requires ImageMagick and python3.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,50p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
MODEL_MAX=${MODEL_MAX:-0.001}
MIN_CHANGED=${MIN_CHANGED:-5000}
BLUR_RATIO=${BLUR_RATIO:-0.85}
HUD=210x40+100+560	# the mini status bar, its mana icons included (the model only)

ALL="01-under-off 02-under-warp1 03-under-warp2 09-under-time37 04-under-tint 04-under-tint-nowarp
05-under-scale-off 05-under-scale 10-wall-off 10-wall-warp1 06-above-off 06-above-warp 07-above-blur 08-turn-noblur
08-turn-blur"
FRAMES=${FRAMES:-$ALL}
MODEL=$(dirname "$0")/warp-model.py

command -v magick >/dev/null || { echo "warp-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
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
# pixels that differ at all, in any channel
changed() {
	dif "$1" "$2" "$T/c.png"
	magick "$T/c.png" -separate -evaluate-sequence max -threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:
}
# the largest difference in any channel, 0-255
maxdiff() { dif "$1" "$2" "$T/m.png"; magick "$T/m.png" -format '%[fx:int(maxima*255+0.5)]' info:; }
# the mean absolute difference between horizontal neighbours over that
# between vertical ones, grey, in the top 560 rows (above the HUD): a yaw
# blur smears along rows and lowers it
aniso() {
	local h v
	magick "$1" -alpha off -crop 800x560+0+0 +repage -colorspace gray "$T/g.png"
	h=$(magick "$T/g.png" \( +clone -roll +1+0 \) -compose difference -composite -crop 799x560+1+0 -format '%[fx:mean]' info:)
	v=$(magick "$T/g.png" \( +clone -roll +0+1 \) -compose difference -composite -crop 800x559+0+1 -format '%[fx:mean]' info:)
	awk -v h="$h" -v v="$v" 'BEGIN { printf "%.4f", (v > 0) ? h / v : 0 }'
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-26s OK    (%s)\n' "$1" "$3"
	else printf '%-26s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { printf '%-26s MISSING in %s\n' "$n" "$d"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "warp-diff: FRAMES MISSING"; exit 1; }
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		sz=$(magick identify -format '%wx%h' "$d/$n.png" 2>/dev/null)
		[ "$sz" = 800x600 ] || { printf '%-26s DIFF  (%s: size %s, want 800x600)\n' "$n" "$d" "${sz:-unreadable}"; rc=1; }
	done
done
[ $rc = 0 ] || { echo "warp-diff: WRONG FRAME SIZE"; exit 1; }

# 1. every static frame against GL
for n in $FRAMES; do
	case $n in 08-*) continue ;; esac
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done

# 2. the warp against its model, on each side
for spec in 02-under-warp1:01-under-off:1:10:1 03-under-warp2:01-under-off:2:10:1 \
	    09-under-time37:01-under-off:1:3.7:1 04-under-tint:04-under-tint-nowarp:1:10:1 \
	    05-under-scale:05-under-scale-off:1:10:0.5 10-wall-warp1:10-wall-off:1:10:1; do
	IFS=: read -r on off w t s <<<"$spec"
	want "$on" "$off" || continue
	res=""; ok=0
	for side in "$GL" "$GPU"; do
		f=$(python3 "$MODEL" "$side/$off.png" "$side/$on.png" --warp "$w" --time "$t" --scale "$s" --mask "$HUD" | awk '{print $3}')
		res="$res${res:+ / }${f:-error}"
		awk -v f="$f" -v m="$MODEL_MAX" 'BEGIN { exit !(f ~ /^[0-9.]+$/ && f + 0 <= m + 0) }' || ok=1
	done
	report "model $on" $ok "mismatched fraction GL / -gpu $res, max $MODEL_MAX (warp $w, time $t, scale $s)"
done

# 3. the warp is there, and follows r_pp_time
for pair in 02-under-warp1:01-under-off 09-under-time37:02-under-warp1; do
	a=${pair%%:*}; b=${pair##*:}
	want "$a" "$b" || continue
	cg=$(changed "$GL/$a.png" "$GL/$b.png"); cp=$(changed "$GPU/$a.png" "$GPU/$b.png")
	[ "$cg" -ge "$MIN_CHANGED" ] && [ "$cp" -ge "$MIN_CHANGED" ]
	report "$a vs ${b%%-*}" $? "pixels changed GL $cg / -gpu $cp, min $MIN_CHANGED"
done

# 4. above the water: identity
if want 06-above-warp 06-above-off; then
	cp=$(changed "$GPU/06-above-warp.png" "$GPU/06-above-off.png")
	[ "$cp" = 0 ]; report "06 above water" $? "-gpu: pixels changed $cp, want 0"
	cg=$(changed "$GL/06-above-warp.png" "$GL/06-above-off.png")
	printf '%-26s INFO  (GL: pixels changed %s, not checked)\n' "06 above water GL" "$cg"
fi

# 5. motion blur at rest: identity within 1
if want 07-above-blur 06-above-warp; then
	mg=$(maxdiff "$GL/07-above-blur.png" "$GL/06-above-warp.png"); mp=$(maxdiff "$GPU/07-above-blur.png" "$GPU/06-above-warp.png")
	cg=$(changed "$GL/07-above-blur.png" "$GL/06-above-warp.png"); cp=$(changed "$GPU/07-above-blur.png" "$GPU/06-above-warp.png")
	[ "$mg" -le 1 ] && [ "$mp" -le 1 ]
	report "07 blur at rest" $? "largest channel step GL $mg / -gpu $mp, max 1 ($cg / $cp pixels)"
fi

# 6. motion blur while turning
if want 08-turn-blur 08-turn-noblur; then
	ok=0; res=""
	for side in "$GL" "$GPU"; do
		ab=$(aniso "$side/08-turn-blur.png"); an=$(aniso "$side/08-turn-noblur.png")
		r=$(awk -v b="$ab" -v n="$an" 'BEGIN { printf "%.3f", (n > 0) ? b / n : 9 }')
		res="$res${res:+ / }$r ($ab / $an)"
		awk -v r="$r" -v m="$BLUR_RATIO" 'BEGIN { exit !(r + 0 <= m + 0) }' || ok=1
	done
	report "08 blur while turning" $ok "anisotropy blur / no blur, GL / -gpu: $res, max $BLUR_RATIO"
fi

# 7. the -gpu run's own reports
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

[ $rc -eq 0 ] && echo "warp-diff: all checks pass" || echo "warp-diff: DIFFERENCES FOUND"
exit $rc
