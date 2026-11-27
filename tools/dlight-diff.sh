#!/usr/bin/env bash
#
# dlight-diff.sh -- the checks for tools/sweep-dlight-gpu.steps: clustered
# dynamic lights and GPU lightstyles under -gpu against GL (#340 P5b).  See
# the steps file's header for how to take the runs.
#
# Usage:
#   dlight-diff.sh <gl-dir> <gpu-dir> [<gpu-dir-before-p5b>]
#
# The mini status bar's health (210x24+100+576, as in tools/pp-diff.sh) is
# masked everywhere, and:
#
#   0. Both sides have every frame.
#   1. Every frame: PSNR against GL >= PSNR_MIN.
#   2. The grid lights what the CPU path lit: on each side 02 (the grid)
#      differs from 01 (the CPU path) by at least MIN_CHANGED pixels, and
#      the -gpu count is within COUNT_TOL of GL's.  Both paths light the
#      torch's wall, by different maths (per pixel, against the CPU's
#      luxels), and that difference is the same on both backends.
#   3. The alias consumer: on each side 03 (r_lightclusters_models 1)
#      differs from 04 (0) by at least MIN_ALIAS pixels, the -gpu count
#      within COUNT_TOL of GL's; and the same for 03g / 04g, the instanced
#      alias stages (r_alias_gpu 1, the desktop default).
#   4. GPU lightstyles: on each side 05 (the styles out of LightBuffer)
#      differs from 05f (flat styles) by at least MIN_STYLE pixels -- the
#      camera sees styled light -- and 05 against 06 (the styles baked on
#      the CPU) changes at most STYLE_SLACK more pixels on -gpu than on GL.
#      Styles read as zeros would make 05 far darker than 06.
#   5. An empty grid is no light: 07 (r_dynamic 0, with the lightmaps and
#      the models lit on the CPU) is the third directory's 07 exactly, when
#      one is given (the binary before P5b, whose world takes no dynamic
#      light at r_dynamic 0 either).
#   6. THE TRAP: on -gpu 12 (r_dlight_occlusion 2) equals 02 exactly: the
#      cube shadow maps are P6, R_Shadow_Prepare returns under -gpu, and the
#      frame's DlShadow block names no map.  A gate lost draws into cube
#      maps -gpu cannot render, and the frame changes.
#  6l. No light through a wall: 11a and 11b are tools/dlight-leak-check.sh's
#      scenes 1 and 3 with the torch named (its "repro"), and the mean
#      luminance of that check's box on each is within LEAK_TOL (default
#      1.0, the check's own TOL) of GL's.  GL passes the check itself (it
#      compares against the torch unnamed); the check cannot run under -gpu
#      as it stands, its `screenshot` reading through glReadPixels, which
#      -gpu does not route yet (P7), so this is how -gpu is held to it.
#   7. lightcluster_verify: each side prints the same lines (lights,
#      occupied froxels, light references, disagreeing words) in REPORT-LIT
#      (real lights, then 64 synthetic) and REPORT-MAP (64 after the map
#      change), every one with 0 words disagreeing.
#   8. The -gpu run: every REPORT section has 0 routed calls the backend did
#      not handle (an unbound LightBuffer on a frame with a grid is one);
#      REPORT-CPU, -LIT, -ALL and -MAP no call into the null GL table but
#      glGetError (LM_BuildStyleLayers' error drain, each time GPU
#      lightstyles come on or off) and glGetString (renderer_status's),
#      REPORT-BOOT's are printed (the boot's buffer and query residue, on
#      master too); in REPORT-LIT the last frame ran 1 compute pass with 0
#      fence waits (the pass samples nothing, so it never waits) and bound
#      every storage slot to a buffer, none to the stand-in (the world's
#      LightBuffer is lc_ssbo); REPORT-ALL no dispatch
#      dropped; no "Validation Error", and Validation Warnings > 0 (the
#      layers loaded).
#
# THE NUMBERS -- see the steps file's header, and the PR, for the runs
# behind them.  The broken builds each check catches are in the PR.
#
# Exit status is 1 if any check fails.  Requires ImageMagick and python3.
set -uo pipefail

[ $# -ge 2 ] && [ $# -le 3 ] || { sed -n '2,/^# Exit status/p' "$0"; exit 2; }
GL=$1; GPU=$2; OLD=${3:-}
PSNR_MIN=${PSNR_MIN:-42}
MIN_CHANGED=${MIN_CHANGED:-1000}
MIN_ALIAS=${MIN_ALIAS:-100}
MIN_STYLE=${MIN_STYLE:-2000}
COUNT_TOL=${COUNT_TOL:-0.10}
STYLE_SLACK=${STYLE_SLACK:-500}
LEAK_TOL=${LEAK_TOL:-1.0}
command -v magick >/dev/null || { echo "dlight-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}
HUD="rectangle 100,576 309,599"
mask() { magick "$1" -alpha off -fill black -draw "$HUD" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
ae() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick compare -metric AE "$T/x.png" "$T/y.png" null: 2>&1 | awk '{print int($1)}'
}
# within COUNT_TOL of GL's count, both at least a minimum
near() {	# gl gpu min
	awk -v g="$1" -v v="$2" -v m="$3" -v t="$COUNT_TOL" \
		'BEGIN { d = g - v; if (d < 0) d = -d; exit !(g >= m && v >= m && d <= t * g) }'
}

FRAMES="01-lit-cpu 02-lit-gpu 03-cast-models 04-cast-nomodels 03g-cast-models-inst 04g-cast-nomodels-inst 05-styles-gpu 05f-styles-flat
06-styles-cpu 07-lit-dynamic0 08-lit-flashblend 09-lit-fullbright 10-lit-scale050 11a-leak1
11b-leak3 12-lit-occl2"

# 0, 1
missing=0
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; missing=1; continue 2; }
	done
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done
[ "$missing" = 0 ] || { echo "dlight-diff: frames missing"; exit 1; }

# 2, 3
pair() {	# name a b min
	local g v
	g=$(ae "$GL/$2.png" "$GL/$3.png"); v=$(ae "$GPU/$2.png" "$GPU/$3.png")
	near "$g" "$v" "$4"; report "$1" $? "GL $g, -gpu $v pixels changed; min $4, within $COUNT_TOL of GL's"
}
pair "grid lights (02 vs 01)" 02-lit-gpu 01-lit-cpu "$MIN_CHANGED"
pair "alias consumer (03 vs 04)" 03-cast-models 04-cast-nomodels "$MIN_ALIAS"
pair "instanced alias (03g vs 04g)" 03g-cast-models-inst 04g-cast-nomodels-inst "$MIN_ALIAS"

# 4
for side in GL GPU; do
	eval d=\$$side
	s=$(ae "$d/05-styles-gpu.png" "$d/05f-styles-flat.png")
	[ "$s" -ge "$MIN_STYLE" ]; report "styled light seen $side" $? "$side: 05 vs 05f $s pixels, min $MIN_STYLE"
done
g=$(ae "$GL/05-styles-gpu.png" "$GL/06-styles-cpu.png"); v=$(ae "$GPU/05-styles-gpu.png" "$GPU/06-styles-cpu.png")
[ "$v" -le $((g + STYLE_SLACK)) ]; report "GPU styles vs CPU (05 vs 06)" $? "GL $g, -gpu $v pixels; -gpu at most GL's + $STYLE_SLACK"

# 5
if [ -n "$OLD" ]; then
	if [ -f "$OLD/07-lit-dynamic0.png" ]; then
		a=$(ae "$GPU/07-lit-dynamic0.png" "$OLD/07-lit-dynamic0.png")
		[ "$a" = 0 ]; report "07 = before P5b" $? "$a pixels differ, want 0"
	else
		report "07 = before P5b" 1 "no $OLD/07-lit-dynamic0.png"
	fi
fi

# 6
a=$(ae "$GPU/12-lit-occl2.png" "$GPU/02-lit-gpu.png")
[ "$a" = 0 ]; report "trap: 12 = 02 on -gpu" $? "$a pixels differ, want 0"
printf '%-32s INFO  (GL: %s pixels -- its cube maps draw, and nothing in this view stands between the torch and its wall)\n' \
	"12 vs 02 GL" "$(ae "$GL/12-lit-occl2.png" "$GL/02-lit-gpu.png")"

# 6l
boxlum() {	# png x0 y0 x1 y1 (fractions of the frame): mean Rec.601 luminance, 0..255
	magick "$1" -alpha off -set option:fw '%w' -set option:fh '%h' \
		-crop "%[fx:round(($4-$2)*w)]x%[fx:round(($5-$3)*h)]+%[fx:round($2*w)]+%[fx:round($3*h)]" +repage \
		-grayscale Rec601Luma -format '%[fx:mean*255]' info:
}
for pair in "11a-leak1 0.36 0.25 0.55 0.50" "11b-leak3 0.49 0.25 0.515 0.52"; do
	set -- $pair
	g=$(boxlum "$GL/$1.png" $2 $3 $4 $5); v=$(boxlum "$GPU/$1.png" $2 $3 $4 $5)
	awk -v g="$g" -v v="$v" -v t="$LEAK_TOL" 'BEGIN { d = g - v; if (d < 0) d = -d; exit !(d <= t) }'
	report "leak box ${1%%-*}" $? "GL $g, -gpu $v mean luminance; within $LEAK_TOL"
done

# 7
verify() { sed -n "/^REPORT-$2-BEGIN/,/^REPORT-$2-END/p" "$1/qconsole.log" 2>/dev/null |
	grep -a "^lightcluster_verify: [0-9]* lights"; }
for sec in LIT MAP; do
	verify "$GL" $sec > "$T/vgl"; verify "$GPU" $sec > "$T/vgpu"
	want=2; [ $sec = MAP ] && want=1
	n=$(wc -l < "$T/vgl"); bad=$(grep -vc " 0 of [0-9]* mask words disagree" "$T/vgl" "$T/vgpu" | awk -F: '{s+=$2} END {print s}')
	ok=1; [ "$n" = "$want" ] && [ "$bad" = 0 ] && cmp -s "$T/vgl" "$T/vgpu" && ok=0
	report "lightcluster_verify $sec" $ok "$(sed 's/lightcluster_verify: //; s/ mask words disagree with the CPU//' "$T/vgpu" | paste -sd ';' -); GL $(diff -q "$T/vgl" "$T/vgpu" >/dev/null && echo same || echo different)"
done

# 8
L=$GPU/qconsole.log
if [ ! -f "$L" ]; then
	report "gpu reports" 1 "no $L"
else
	for sec in BOOT CPU LIT ALL MAP; do
		s=$(sed -n "/^REPORT-$sec-BEGIN/,/^REPORT-$sec-END/p" "$L")
		[ -n "$s" ] || { report "report $sec" 1 "no REPORT-$sec"; continue; }
		r=$(printf '%s\n' "$s" | grep -a "routed calls the SDL_GPU backend did not handle")
		all=$(printf '%s\n' "$s" | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | tr -s ' ' | paste -sd ';' -)
		x=$(printf '%s\n' "$s" | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | grep -avE ' gl(GetError|GetString)$' | tr -s ' ' | paste -sd ';' -)
		ok=1
		case "$r" in "gpu_unrouted: 0 routed calls"*) [ -z "$x" ] || [ $sec = BOOT ] && ok=0 ;; esac
		report "report $sec" $ok "${r:-no gpu_unrouted in it}; null-table calls ${all:-none}"
	done
	lit=$(sed -n '/^REPORT-LIT-BEGIN/,/^REPORT-LIT-END/p' "$L" | sed -n 's/.*storage slots last frame: \([0-9]*\) bound to a buffer, \([0-9]*\) to the stand-in.*/\1 \2/p')
	cl=$(sed -n '/^REPORT-LIT-BEGIN/,/^REPORT-LIT-END/p' "$L" | grep -a "compute last frame:" | sed 's/.*compute last frame: //; s/;.*//')
	set -- $lit
	ok=1; [ $# = 2 ] && [ "$1" -gt 0 ] && [ "$2" = 0 ] && [ "$cl" = "1 passes, 0 readbacks, 0 fence waits" ] && ok=0
	report "LightBuffer bound, no wait" $ok "grid frame: ${1:-?} storage slots bound, ${2:-?} to the stand-in; compute ${cl:-none}"
	d=$(sed -n '/^REPORT-ALL-BEGIN/,/^REPORT-ALL-END/p' "$L" | sed -n 's/.*since boot: \([0-9]*\) dispatches ([0-9]* images cycled), \([0-9]*\) dropped.*/\1 \2/p')
	set -- $d
	ok=1; [ $# = 2 ] && [ "$1" -gt 0 ] && [ "$2" = 0 ] && ok=0
	report "dispatches" $ok "${1:-?} since boot, ${2:-?} dropped"
	v=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
	w=$(grep -cs "Validation Warning" "$GPU/engine.stdout"); w=${w:-0}
	ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
	report "validation" $ok "$v Validation Error lines; $w Warnings, want > 0 (the layers loaded)"
fi

[ $rc -eq 0 ] && echo "dlight-diff: all checks pass" || echo "dlight-diff: DIFFERENCES FOUND"
exit $rc
