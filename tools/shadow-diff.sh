#!/usr/bin/env bash
#
# shadow-diff.sh -- the checks for tools/sweep-shadow-gpu.steps: the dynamic
# lights' cube shadow maps under -gpu against GL (#340 P6b).  See the steps
# file's header for how to take the runs.
#
# Usage:
#   shadow-diff.sh <gl-dir> <gpu-dir> [<gpu-dir-before-p6b>]
#
# The mini status bar's health (210x24+100+576) is masked everywhere, and:
#
#   0. Both sides have every frame.
#   1. Every frame: PSNR against GL >= PSNR_MIN; 13 (r_shadow_debug 1, the
#      cube faces unwrapped on screen: each face's orientation and row
#      order) >= DEBUG_MIN.
#   2. The leak check's boxes (tools/dlight-leak-check.sh's, as fractions of
#      the frame), the mean luminance of each within LEAK_TOL of GL's: lit
#      on 01, scene 2 and alias on 02, cast on 04, 05 and 06.
#   3. The maps stop what they must, on each side: scene 2's box on 02o
#      (r_dlight_occlusion 1, the light through the wall) at least LEAKMIN
#      brighter than on 02; and the cast scene as the leak check has it,
#      05 (r_shadow_casters 1) within LEAK_TOL of 06 (the barrel gone) and
#      04 (casters 2) at least SHADOWMIN darker than 06.
#   4. r_shadowinfo after every frame (SI-<frame>): each line the same on
#      both backends -- the lights chosen, their slots, keys, origins, radii,
#      ages and alias counts are CPU code, so any difference is a bug.  Bar
#      one field: the age of a slot no light took this frame (no "in use")
#      counts the frames rendered since, which is the harness's pace, not
#      the backend's; it is masked.  An in-use slot's age is compared.
#   5. The cache is invisible: on each side 12 (r_shadow_cache 0, every map
#      redrawn each frame) is 01 exactly, and 08 (r_shadow_filter back at 1)
#      is 04 exactly.
#   6. The passes: at 12 on -gpu, renderer_status's depth-only passes on a
#      layer equal 6 x the maps r_shadowinfo says that frame drew, and that
#      is more than 0.  (The storage stand-ins that frame are printed: each
#      alias caster binds one of sshadow_alias_vert's three pose buffers,
#      and the pose rule excuses the other two, so they are expected; an
#      unexcused one is check 7's routed call.)
#   7. The -gpu run: REPORT-ALL and REPORT-MAP each 0 routed calls the
#      backend did not handle (an alias caster without the pose rule, a
#      dropped multi-draw, a refused layer would each be one) and no call
#      into the null GL table but glGetError and glGetString; no
#      "Validation Error", and Validation Warnings > 0 (the layers loaded).
#   8. With a third directory (the binary before P6b, -gpu): 18
#      (r_dlight_occlusion 1) is its 18 exactly, and its 04 is its 18
#      exactly (the gate it had: occlusion 2 drew nothing there).
#
# Exit status is 1 if any check fails.  Requires ImageMagick and python3.
set -uo pipefail

[ $# -ge 2 ] && [ $# -le 3 ] || { sed -n '2,/^# Exit status/p' "$0"; exit 2; }
GL=$1; GPU=$2; OLD=${3:-}
PSNR_MIN=${PSNR_MIN:-42}
DEBUG_MIN=${DEBUG_MIN:-50}
LEAK_TOL=${LEAK_TOL:-1.0}
LEAKMIN=${LEAKMIN:-4.0}
SHADOWMIN=${SHADOWMIN:-2.0}
command -v magick >/dev/null || { echo "shadow-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-34s OK    (%s)\n' "$1" "$3"
	else printf '%-34s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
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
boxlum() {	# png x0 y0 x1 y1 (fractions of the frame): mean Rec.601 luminance, 0..255
	magick "$1" -alpha off \
		-crop "%[fx:round(($4-$2)*w)]x%[fx:round(($5-$3)*h)]+%[fx:round($2*w)]+%[fx:round($3*h)]" +repage \
		-grayscale Rec601Luma -format '%[fx:mean*255]' info:
}
within() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }

FRAMES="01-lit 02-scene2 03-scene2-casters1 02o-scene2-occl1 04-cast2 05-cast1 06-castbare 07-filter0
08-filter1 09-res64 10-res512 11-maxlights1 13-debug 14-aliasgpu 17-scale050 18-occl1 12-nocache
15-md5cast"

# 0, 1
missing=0
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; missing=1; continue 2; }
	done
	min=$PSNR_MIN; [ "$n" = 13-debug ] && min=$DEBUG_MIN
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$min"; report "$n" $? "PSNR $p dB, min $min"
done
[ "$missing" = 0 ] || { echo "shadow-diff: frames missing"; exit 1; }

# 2: tools/dlight-leak-check.sh's boxes
BOX_LIT="0.55 0.40 0.75 0.60"
BOX_S2="0.475 0.25 0.515 0.55"
BOX_ALIAS="0.575 0.54 0.615 0.64"
BOX_CAST="0.47 0.597 0.535 0.635"
declare -A L
for side in GL GPU; do
	eval d=\$$side
	L[$side,lit]=$(boxlum "$d/01-lit.png" $BOX_LIT)
	L[$side,s2]=$(boxlum "$d/02-scene2.png" $BOX_S2)
	L[$side,s2o]=$(boxlum "$d/02o-scene2-occl1.png" $BOX_S2)
	L[$side,alias]=$(boxlum "$d/02-scene2.png" $BOX_ALIAS)
	L[$side,cast2]=$(boxlum "$d/04-cast2.png" $BOX_CAST)
	L[$side,cast1]=$(boxlum "$d/05-cast1.png" $BOX_CAST)
	L[$side,bare]=$(boxlum "$d/06-castbare.png" $BOX_CAST)
done
for k in lit s2 alias cast2 cast1 bare; do
	within "${L[GL,$k]}" "${L[GPU,$k]}" "$LEAK_TOL"
	report "box $k" $? "GL ${L[GL,$k]}, -gpu ${L[GPU,$k]} mean luminance; within $LEAK_TOL"
done

# 3
for side in GL GPU; do
	awk -v o="${L[$side,s2o]}" -v s="${L[$side,s2]}" -v m="$LEAKMIN" 'BEGIN { exit !(o - s >= m) }'
	report "scene 2 stopped $side" $? "02o ${L[$side,s2o]} vs 02 ${L[$side,s2]}: the maps take at least $LEAKMIN"
	within "${L[$side,cast1]}" "${L[$side,bare]}" "$LEAK_TOL"; a=$?
	awk -v c="${L[$side,cast2]}" -v b="${L[$side,bare]}" -v m="$SHADOWMIN" 'BEGIN { exit !(b - c >= m) }'; b=$?
	report "cast scene $side" $((a | b)) "casters 2 ${L[$side,cast2]}, 1 ${L[$side,cast1]}, bare ${L[$side,bare]}: 1 = bare within $LEAK_TOL, 2 at least $SHADOWMIN darker"
done

# 4
si() {	# log frame: the r_shadowinfo dump after SI-<frame> (echo leaves a space)
	awk -v m="SI-$2" '$1 == m && NF == 1 { on = 1; next } on && /^--- r_shadowinfo/ { p = 1; print; next }
		p && /^slot [0-9]+:/ { if ($0 !~ / in use$/) sub(/ age [0-9]+ /, " age - "); print; next }
		p { exit }' "$1"
}
n=0; bad=""
for f in 01 02 03 04 05 06 07 08 09 10 11 13 14 17 18 12 15; do
	si "$GL/qconsole.log" $f > "$T/sgl"; si "$GPU/qconsole.log" $f > "$T/sgpu"
	if [ ! -s "$T/sgl" ] || ! cmp -s "$T/sgl" "$T/sgpu"; then bad="$bad $f"; else n=$((n + 1)); fi
done
[ -z "$bad" ]; report "r_shadowinfo" $? "$n of 17 dumps identical${bad:+; differ or missing:$bad}"

# 5
for side in GL GPU; do
	eval d=\$$side
	a=$(ae "$d/12-nocache.png" "$d/01-lit.png"); b=$(ae "$d/08-filter1.png" "$d/04-cast2.png")
	ok=1; [ "$a" = 0 ] && [ "$b" = 0 ] && ok=0
	report "cache invisible $side" $ok "12 vs 01 $a, 08 vs 04 $b pixels; want 0, 0"
done

# 6, 7
LG=$GPU/qconsole.log
if [ ! -f "$LG" ]; then
	report "gpu reports" 1 "no $LG"
else
	drawn=$(si "$LG" 12 | sed -n 's/.* \([0-9]*\) drawn, .*/\1/p')
	st=$(awk '$1 == "STATUS-12" && NF == 1 { on = 1; next } on && /depth-only passes/ { print; exit }' "$LG")
	lay=$(printf '%s\n' "$st" | sed -n 's/.* \([0-9]*\) of them on a layer.*/\1/p')
	sl=$(awk '$1 == "STATUS-12" && NF == 1 { on = 1; next } on && /storage slots last frame/ { print; exit }' "$LG" |
		sed -n 's/.* \([0-9]*\) to the stand-in.*/\1/p')
	ok=1; [ -n "$drawn" ] && [ "$drawn" -gt 0 ] && [ "${lay:-x}" = $((drawn * 6)) ] && ok=0
	report "passes at 12" $ok "${drawn:-?} maps drawn, ${lay:-?} layer passes, want 6 x; ${sl:-?} slots on the stand-in"
	for sec in ALL MAP; do
		s=$(sed -n "/^REPORT-$sec-BEGIN/,/^REPORT-$sec-END/p" "$LG")
		[ -n "$s" ] || { report "report $sec" 1 "no REPORT-$sec"; continue; }
		r=$(printf '%s\n' "$s" | grep -a "routed calls the SDL_GPU backend did not handle" | tail -1)
		x=$(printf '%s\n' "$s" | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | grep -avE ' gl(GetError|GetString)$' | tr -s ' ' | paste -sd ';' -)
		ok=1
		case "$r" in "gpu_unrouted: 0 routed calls"*) [ -z "$x" ] && ok=0 ;; esac
		report "report $sec" $ok "${r:-no gpu_unrouted in it}; other null-table calls ${x:-none}"
	done
	v=$(cat "$LG" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
	w=$(grep -cs "Validation Warning" "$GPU/engine.stdout"); w=${w:-0}
	ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
	report "validation" $ok "$v Validation Error lines; $w Warnings, want > 0 (the layers loaded)"
fi

# 8
if [ -n "$OLD" ]; then
	if [ -f "$OLD/18-occl1.png" ] && [ -f "$OLD/04-cast2.png" ]; then
		a=$(ae "$GPU/18-occl1.png" "$OLD/18-occl1.png"); b=$(ae "$OLD/04-cast2.png" "$OLD/18-occl1.png")
		ok=1; [ "$a" = 0 ] && [ "$b" = 0 ] && ok=0
		report "18 = before P6b" $ok "18 vs its 18: $a pixels; its 04 vs its 18: $b; want 0, 0"
	else
		report "18 = before P6b" 1 "no 18-occl1.png / 04-cast2.png in $OLD"
	fi
fi

[ $rc -eq 0 ] && echo "shadow-diff: all checks pass" || echo "shadow-diff: DIFFERENCES FOUND"
exit $rc
