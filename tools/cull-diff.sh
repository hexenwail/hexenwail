#!/usr/bin/env bash
#
# cull-diff.sh -- the checks for tools/sweep-cull-gpu.steps: the GPU world
# cull and its indirect draws under -gpu against GL (#340 P5c).  See the
# steps file's header for how to take the runs.
#
# Usage:
#   cull-diff.sh <gl-dir> <gpu-dir>
#
# The mini status bar's health (210x24+100+576, as in tools/pp-diff.sh) and
# the full bar's (61x41+288+552: village1's frames are at viewsize 100, as
# sweep-worldcull.steps' counts were taken) are masked everywhere -- the
# player takes a run-dependent knock before god is on -- and:
#
#   0. Both sides have every frame.
#   1. Every frame: PSNR against GL >= PSNR_MIN.
#   2. The cull is invisible beside the chain pass: on -gpu 01 (r_gpucull 1)
#      against 07 (r_gpucull 0) changes at most GL's own count plus
#      PAIR_SLACK pixels.  The two passes draw the same opaque surfaces, and
#      only depth-equal ties move (the compaction order is not fixed).
#   3. The counters: in every STATS-nn section the set of distinct
#      hiz_stats lines is one line, the same on both sides, with hiz=0 (Hi-Z
#      is off); STATS-10 is the yaw-90 baseline exactly (YAW90).
#  3v. worldcull_verify, in REPORT-CULL, -MIRROR and STATS-10..13: on each
#      side 0 buckets disagree on the count, the first index, the rest of the
#      command or the indices, at least one surface accepted, and the -gpu
#      line is GL's.
#   4. The indirect draws: in REPORT-CULL, -MIRROR and -VILLAGE the last
#      frame's indirect draws are the map's buckets with a texture (its
#      "GPU world cull: ... (N with a texture, drawn)" boot line, the latest
#      before the section), 0 dispatches dropped; REPORT-NOCULL (gl_cull 0
#      on a mirror map, where the cull stays off) 0 indirect draws.
#   5. The -gpu run: every REPORT section has 0 routed calls the backend did
#      not handle, REPORT-VILLAGE (counted from before the map change, so
#      R_FreeWorldCull's deletes are in it) no call into the null GL table
#      but glGetError and glGetString, the asynchronous readbacks were all collected (none
#      refused or failed), no "Validation Error", and Validation Warnings
#      > 0 (the layers loaded).
#
# THE NUMBERS -- see the steps file's header, and the PR, for the runs
# behind them.  The broken builds each check catches are in the PR.
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# = 2 ] || { sed -n '2,/^# Exit status/p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-40}
PAIR_SLACK=${PAIR_SLACK:-200}
YAW90=${YAW90:-"total=5572 pvs=4953 flags=46 frust=2 hiz=0 accepted=503 dedup=68"}
command -v magick >/dev/null || { echo "cull-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}
HUD="rectangle 100,576 309,599"
HP="rectangle 288,552 348,592"
mask() { magick "$1" -alpha off -fill black -draw "$HUD" -draw "$HP" "$2"; }
psnr() {
	mask "$1" "$T/a.png"; mask "$2" "$T/b.png"
	magick compare -metric PSNR "$T/a.png" "$T/b.png" null: 2>&1 | awk '{print $1 == 120 ? "inf" : $1}'
}
atleast() { awk -v p="$1" -v m="$2" 'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; }
ae() {
	mask "$1" "$T/x.png"; mask "$2" "$T/y.png"
	magick compare -metric AE "$T/x.png" "$T/y.png" null: 2>&1 | awk '{print int($1)}'
}
section() { sed -n "/^$2-BEGIN/,/^$2-END/p" "$1/qconsole.log" 2>/dev/null; }

FRAMES="01-statue-sky 02-arch 03-tree 04-cross 05-wall 06-courtyard 07-statue-sky-nocull
08-mirror 09-mirror-gl-cull-0 10-yaw090 11-yaw180 12-yaw270 13-yaw000"

# 0, 1
missing=0
for n in $FRAMES; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; missing=1; continue 2; }
	done
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
done
[ "$missing" = 0 ] || { echo "cull-diff: frames missing"; exit 1; }

# 2
g=$(ae "$GL/01-statue-sky.png" "$GL/07-statue-sky-nocull.png")
v=$(ae "$GPU/01-statue-sky.png" "$GPU/07-statue-sky-nocull.png")
[ "$v" -le $((g + PAIR_SLACK)) ]; report "cull invisible (01 vs 07)" $? "GL $g, -gpu $v pixels; -gpu at most GL's + $PAIR_SLACK"

# 3
for n in 10 11 12 13; do
	section "$GL" "STATS-$n" | grep -a "^hiz_stats:" | sed 's/^hiz_stats: //' | sort -u > "$T/sgl"
	section "$GPU" "STATS-$n" | grep -a "^hiz_stats:" | sed 's/^hiz_stats: //' | sort -u > "$T/sgpu"
	ok=1
	if [ "$(wc -l < "$T/sgl")" = 1 ] && cmp -s "$T/sgl" "$T/sgpu" && grep -q " hiz=0 " "$T/sgpu"; then
		ok=0
		[ $n = 10 ] && [ "$(cat "$T/sgpu")" != "$YAW90" ] && ok=1
	fi
	report "hiz_stats STATS-$n" $ok "-gpu $(paste -sd '|' "$T/sgpu"); GL $(cmp -s "$T/sgl" "$T/sgpu" && echo same || paste -sd '|' "$T/sgl")"
done

# 3v
for sec in REPORT-CULL REPORT-MIRROR STATS-10 STATS-11 STATS-12 STATS-13; do
	vg=$(section "$GL" "$sec" | grep -a "^worldcull_verify:")
	vv=$(section "$GPU" "$sec" | grep -a "^worldcull_verify:")
	ok=1
	case "$vv" in
	*"surfaces accepted"*" 0 disagree on the count, 0 on the first index, 0 on the rest of the command, 0 on the indices")
		[ "$vv" = "$vg" ] && ! printf '%s' "$vv" | grep -q "^worldcull_verify: 0 surfaces" && ok=0 ;;
	esac
	report "worldcull_verify $sec" $ok "$(printf '%s' "${vv:-none}" | sed 's/worldcull_verify: //'); GL $([ "$vv" = "$vg" ] && echo same || echo "${vg:-none}")"
done

# 4
L=$GPU/qconsole.log
buckets() {	# section: the "N with a texture" of the latest boot line before it
	awk -v sec="$1-BEGIN" '/^GPU world cull: / { l = $0 } index($0, sec) == 1 { print l; exit }' "$L" |
		sed -n 's/.*(\([0-9]*\) with a texture.*/\1/p'
}
lastframe() { section "$GPU" "$1" | sed -n 's/.*SDL_GPU last frame: \([0-9]*\) indirect draws, \([0-9]*\) dispatches dropped.*/\1 \2/p'; }
for sec in REPORT-CULL REPORT-MIRROR REPORT-VILLAGE REPORT-NOCULL; do
	want=$(buckets "$sec"); [ $sec = REPORT-NOCULL ] && want=0
	set -- $(lastframe "$sec")
	ok=1; [ $# = 2 ] && [ -n "$want" ] && [ "$1" = "$want" ] && [ "$2" = 0 ] && ok=0
	report "indirect draws $sec" $ok "${1:-?} indirect draws, want ${want:-?}; ${2:-?} dispatches dropped"
done

# 5
for sec in REPORT-CULL REPORT-MIRROR REPORT-NOCULL REPORT-VILLAGE; do
	r=$(section "$GPU" "$sec" | grep -a "routed calls the SDL_GPU backend did not handle")
	ok=1; case "$r" in "gpu_unrouted: 0 routed calls"*) ok=0 ;; esac
	report "unrouted $sec" $ok "${r:-no gpu_unrouted in it}"
done
x=$(section "$GPU" REPORT-VILLAGE | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | grep -avE ' gl(GetError|GetString)$' | tr -s ' ' | paste -sd ';' -)
[ -z "$x" ]; report "null table across the map" $? "${x:-no call into the null GL table but glGetError / glGetString}"
a=$(section "$GPU" REPORT-VILLAGE | sed -n 's/.*readbacks since boot \([0-9]*\) asked, \([0-9]*\) collected, \([0-9]*\) refused, \([0-9]*\) failed.*/\1 \2 \3 \4/p')
set -- $a
ok=1; [ $# = 4 ] && [ "$1" -gt 0 ] && [ "$3" = 0 ] && [ "$4" = 0 ] && [ $(($1 - $2)) -le 8 ] && ok=0
report "async readbacks" $ok "${1:-?} asked, ${2:-?} collected (at most 8 in flight), ${3:-?} refused, ${4:-?} failed"
v=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
w=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Warning")
ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
report "validation" $ok "$v errors, $w warnings (the layers loaded)"

exit $rc
