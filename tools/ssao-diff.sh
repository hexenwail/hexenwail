#!/usr/bin/env bash
#
# ssao-diff.sh -- the checks for tools/sweep-ssao-gpu.steps: r_ssao under
# -gpu against GL (#340 P5e).  See the steps file's header for how to take
# the runs.
#
# Usage:
#   ssao-diff.sh <gl-dir> <gpu-dir>
#
# The status bar (the bottom 40 rows: the player's health takes a
# run-dependent knock before god is on) is cut off everywhere, and:
#
#   0. Both sides have every frame.
#   1. Every frame: PSNR against GL >= PSNR_MIN.
#   2. Coverage: in 05 (r_ssao_debug 1, the occlusion multiplier, white
#      where nothing is occluded) the pixels below 250 grey are occluded
#      ones; the -gpu count is within OCC_TOL (default 0.05) of GL's, and
#      both are at least MIN_OCC.  A pass dropped or fed a placeholder is no
#      occlusion at all, or occlusion everywhere.
#   3. The receivers: in 06 (r_ssao_debug 2, white on an entity's own
#      pixels) the white pixel count on -gpu is within MASK_TOL (default
#      0.01) of GL's, and at least MIN_MASK.  The world/scene depth equality
#      decides it, so it moves with the mip pass's tile tags and rounding.
#   4. Nothing to add, nothing changed: 09 (r_ssao 2, no entity in view)
#      is 01n (the same camera at r_ssao 0) to within one level per channel,
#      on each side.  Not exactly: the AO is stored R8, a visibility of
#      254/255 still darkens a little, and GL's own 09 differs from its 01n
#      by one level in about 1600 pixels.  The count is printed.
#   5. SSAO changed the frame: 02, 03 and 04 (r_ssao 1, 2, 3) each differ
#      from 01 (r_ssao 0) by at least MIN_CHANGED pixels, on each side.
#   6. The -gpu run: in each REPORT section 0 routed calls the backend did
#      not handle and no dispatch dropped since boot (REPORT-ON's calls counted from
#      REPORT-OFF, so SSAO's targets made twice are in it); since boot no
#      texture made again for a storage use it was made without (the
#      pyramid's mip 0, prepared write-only and read back read-write, would
#      lose the prepared depth); REPORT-ON's last frame ran exactly PASSES
#      (default 5) more compute passes than REPORT-OFF's: the prepare, mip,
#      evaluate and two filters, one pass each (the composite is a draw).
#      Its fence waits and render segments are printed, not gated (P5f).
#      renderer_status' SSAO line says it is available and ran, on both
#      sides; no "Validation Error", and Validation Warnings > 0 (the layers
#      loaded).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# = 2 ] || { sed -n '2,/^# Exit status/p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-40}
OCC_TOL=${OCC_TOL:-0.05}
MASK_TOL=${MASK_TOL:-0.01}
MIN_OCC=${MIN_OCC:-500}
MIN_MASK=${MIN_MASK:-5000}
MIN_CHANGED=${MIN_CHANGED:-20}
PASSES=${PASSES:-5}
command -v magick >/dev/null || { echo "ssao-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}
# the status bar, whatever the frame's height
mask() { magick "$1" -alpha off -gravity south -chop 0x40 "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
# pixels that differ at all, and the largest difference of any channel (0-255)
diffpx() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick "$T/x.png" "$T/y.png" -compose difference -composite -separate -evaluate-sequence max \
		-threshold 0 -format "%[fx:int(w*h*mean+0.5)] " info:
	magick "$T/x.png" "$T/y.png" -compose difference -composite -separate -evaluate-sequence max \
		-format "%[fx:int(maxima*255+0.5)]" info:
}
# pixels of the frame, status bar cut off, whose grey is below / at least `level` (0-255)
below() { magick "$1" -alpha off -gravity south -chop 0x40 -colorspace Gray -format "%[fx:int(w*h*(1-mean)+0.5)]" \
	  -threshold "$(awk -v l="$2" 'BEGIN { printf "%.6f%%", (l - 0.5) * 100 / 255 }')" info:; }
white() { magick "$1" -alpha off -gravity south -chop 0x40 -colorspace Gray -format "%[fx:int(w*h*mean+0.5)]" \
	  -threshold 98% info:; }
near() {	# gl gpu min tol
	awk -v g="$1" -v v="$2" -v m="$3" -v t="$4" \
		'BEGIN { d = g - v; if (d < 0) d = -d; exit !(g >= m && v >= m && d <= t * g) }'
}
section() { sed -n "/^$2-BEGIN/,/^$2-END/p" "$1/qconsole.log" 2>/dev/null; }

FRAMES="01-ssao0 02-ssao1 03-ssao2 04-ssao3 05-debug1 06-debug2 07-scale050 08-fog 09-noent 01n-noent-ssao0"

# 0, 1
missing=0
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; missing=1; continue 2; }
	done
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done
[ "$missing" = 0 ] || { echo "ssao-diff: frames missing"; exit 1; }

# 2
g=$(below "$GL/05-debug1.png" 250); v=$(below "$GPU/05-debug1.png" 250)
near "$g" "$v" "$MIN_OCC" "$OCC_TOL"; report "occluded pixels (05)" $? "GL $g, -gpu $v, tolerance $OCC_TOL, min $MIN_OCC"

# 3
g=$(white "$GL/06-debug2.png"); v=$(white "$GPU/06-debug2.png")
near "$g" "$v" "$MIN_MASK" "$MASK_TOL"; report "receiver mask (06)" $? "GL $g white, -gpu $v, tolerance $MASK_TOL"

# 4, 5
for side in GL GPU; do
	d=${!side}
	read -r n m <<<"$(diffpx "$d/09-noent.png" "$d/01n-noent-ssao0.png")"
	ok=1; [ -n "$m" ] && [ "$m" -le 1 ] && ok=0
	report "09 = 01n $side" $ok "${n:-?} pixels differ, by at most ${m:-?} level(s)"
	for n in 02-ssao1 03-ssao2 04-ssao3; do
		read -r a m <<<"$(diffpx "$d/$n.png" "$d/01-ssao0.png")"
		[ "${a:-0}" -ge "$MIN_CHANGED" ]; report "$n vs 01 $side" $? "$a pixels differ, min $MIN_CHANGED"
	done
done

# 6
L=$GPU/qconsole.log
for sec in REPORT-OFF REPORT-ON REPORT-MAP; do
	r=$(section "$GPU" "$sec" | grep -a "routed calls the SDL_GPU backend did not handle")
	d=$(section "$GPU" "$sec" | sed -n 's/.*SDL_GPU last frame: [0-9]* indirect draws, \([0-9]*\) dispatches dropped.*/\1/p')
	ok=1; case "$r" in "gpu_unrouted: 0 routed calls"*) [ "$d" = 0 ] && ok=0 ;; esac
	report "unrouted $sec" $ok "${r:-no gpu_unrouted in it}; ${d:-?} dispatches dropped"
done
s=$(section "$GPU" REPORT-MAP | sed -n 's/.*SDL_GPU textures: .* re-creates, \([0-9]*\) for storage usage).*/\1/p')
[ "${s:-x}" = 0 ]; report "storage re-creates" $? "${s:-?} textures made again for a storage use since boot"
cpass() { section "$GPU" "$1" | sed -n 's/.*compute last frame: \([0-9]*\) passes, [0-9]* readbacks, \([0-9]*\) fence waits.*/\1 \2/p'; }
segs() { section "$GPU" "$1" | sed -n 's/.*SDL_GPU frame: .* draws (.* from buffers) in \([0-9]*\) segments.*/\1/p'; }
read -r p0 w0 <<<"$(cpass REPORT-OFF)"; read -r p1 w1 <<<"$(cpass REPORT-ON)"
ok=1; [ -n "${p0:-}" ] && [ -n "${p1:-}" ] && [ $((p1 - p0)) = "$PASSES" ] && ok=0
report "compute passes" $ok "r_ssao 0: ${p0:-?} passes, ${w0:-?} fence waits, $(segs REPORT-OFF) segments; r_ssao 2: ${p1:-?} passes, ${w1:-?} fence waits, $(segs REPORT-ON) segments; want +$PASSES passes"
for side in GL GPU; do
	d=${!side}
	s=$(section "$d" REPORT-ON | grep -a "^\[RENDERER\] SSAO:")
	ok=1; case "$s" in *", available; since boot "[1-9]*" scenes with it, 0 held off"*) ok=0 ;; esac
	report "SSAO status $side" $ok "${s:-no SSAO line in REPORT-ON}"
done
v=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
w=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Warning")
ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
report "validation" $ok "$v errors, $w warnings (the layers loaded)"

exit $rc
