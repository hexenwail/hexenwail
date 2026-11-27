#!/usr/bin/env bash
#
# hiz-diff.sh -- the checks for tools/sweep-hiz-gpu.steps: the Hi-Z pyramid
# under -gpu against GL (#340 P5d).  See the steps file's header for how to
# take the runs.
#
# Usage:
#   hiz-diff.sh <gl-dir> <gpu-dir> [<master-gpu-dir>]
#
# <master-gpu-dir> is the same sweep from a -gpu binary that holds Hi-Z off
# (P5c and before): culling is invisible, so the frames must not move.
#
# The full status bar's health (61x41+288+552) is masked everywhere -- the
# player takes a run-dependent knock before god is on -- and:
#
#   0. Both sides have every frame.
#   1. Every frame: PSNR against GL >= PSNR_MIN.
#   2. With <master-gpu-dir>: every frame AE <= AE_MAX (default 0) against
#      master's -gpu frame.  Hi-Z culls only what the chain pass draws over.
#   3. The counters: in every STATS section the set of distinct hiz_stats
#      lines is one line, the same on both sides; STATS-01, -02, -03 and
#      -04 sweep-worldcull.steps' village1 baseline exactly (YAW90,
#      YAW90_OFF, YAW180, YAW270).
#  3r. The relations: STATS-11 (after the post chain went up and down) is
#      STATS-01, STATS-11A is STATS-01 and STATS-11C (down) is STATS-11B (up),
#      hiz-toggle-check.sh's; STATS-10's hiz (r_mirroralpha 0.5) is above 0
#      and at most STATS-10A's (r_mirroralpha 1) plus SLACK,
#      hiz-mirror-check.sh's.  On each side.
#  3v. worldcull_verify in every STATS section that runs it: on each side 0
#      buckets disagree on anything, at least one surface accepted, and the
#      -gpu line is GL's; with Hi-Z on, 0 pyramid texels off the reduce of
#      the level below, and the marksurfaces the CPU's Hi-Z test culls are the
#      section's hiz= count, give or take those too close to call.  JUMP: both
#      verifies 0 off, and different from each other (the jump changed what
#      is drawn).
#   4. The -gpu run: in each REPORT section 0 routed calls the backend did
#      not handle and 0 dispatches dropped; in REPORT-OFF levels written
#      through a scratch; REPORT-MIRROR (counted from before the map change)
#      no call into the null GL table but glGetError and glGetString; no
#      "Validation Error", and Validation Warnings > 0 (the layers loaded).
#      The fence waits of the last frame are printed, not gated (P5f).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# = 2 ] || [ $# = 3 ] || { sed -n '2,/^# Exit status/p' "$0"; exit 2; }
GL=$1; GPU=$2; MGPU=${3:-}
PSNR_MIN=${PSNR_MIN:-40}
AE_MAX=${AE_MAX:-0}
SLACK=${SLACK:-5}
YAW90=${YAW90:-"total=5572 pvs=4953 flags=46 frust=2 hiz=118 accepted=390 dedup=63"}
YAW90_OFF=${YAW90_OFF:-"total=5572 pvs=4953 flags=46 frust=2 hiz=0 accepted=503 dedup=68"}
YAW180=${YAW180:-"total=5572 pvs=4953 flags=46 frust=314 hiz=147 accepted=88 dedup=24"}
YAW270=${YAW270:-"total=5572 pvs=4953 flags=46 frust=469 hiz=0 accepted=88 dedup=16"}
command -v magick >/dev/null || { echo "hiz-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}
HP="rectangle 288,552 348,592"
mask() { magick "$1" -alpha off -fill black -draw "$HP" "$2"; }
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
stats() { section "$1" "STATS-$2" | grep -a "^hiz_stats:" | sed 's/^hiz_stats: //' | sort -u; }
hizof() { sed -n 's/.* hiz=\([0-9]*\) .*/\1/p' <<<"$1"; }

FRAMES="01-yaw090 02-yaw090-hiz-off 03-yaw180 04-yaw270 05-yaw090-pp 06-yaw090-pp-hiz-off 07-yaw180-pp
08-yaw270-pp 09-yaw090-scale 10-mirror-floor 11-toggle-yaw090"
SECTIONS="01 02 03 04 05 06 07 08 09 10A 10 11A 11B 11C 11"
VERIFIED="01 02 03 04 05 06 07 08 09 10A 10 11"

# 0, 1, 2
missing=0
for n in $FRAMES; do
	for d in "$GL" "$GPU" $MGPU; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; missing=1; continue 2; }
	done
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	if [ -n "$MGPU" ]; then
		a=$(ae "$MGPU/$n.png" "$GPU/$n.png")
		ok=1; atleast "$p" "$PSNR_MIN" && [ "$a" -le "$AE_MAX" ] && ok=0
		report "$n" $ok "PSNR $p dB, min $PSNR_MIN; $a pixels from master -gpu, max $AE_MAX"
	else
		atleast "$p" "$PSNR_MIN"; report "$n" $? "PSNR $p dB, min $PSNR_MIN"
	fi
done
[ "$missing" = 0 ] || { echo "hiz-diff: frames missing"; exit 1; }

# 3
for n in $SECTIONS; do
	stats "$GL" "$n" > "$T/sgl"; stats "$GPU" "$n" > "$T/sgpu"
	want=
	case $n in 01) want=$YAW90 ;; 02) want=$YAW90_OFF ;; 03) want=$YAW180 ;; 04) want=$YAW270 ;; esac
	ok=1
	if [ "$(wc -l < "$T/sgl")" = 1 ] && cmp -s "$T/sgl" "$T/sgpu"; then
		ok=0
		[ -n "$want" ] && [ "$(cat "$T/sgpu")" != "$want" ] && ok=1
	fi
	report "hiz_stats STATS-$n" $ok "-gpu $(paste -sd '|' "$T/sgpu"); GL $(cmp -s "$T/sgl" "$T/sgpu" && echo same || paste -sd '|' "$T/sgl")"
done

# 3r
for d in "$GL" "$GPU"; do
	side=$([ "$d" = "$GL" ] && echo GL || echo -gpu)
	s01=$(stats "$d" 01); s11=$(stats "$d" 11); s11a=$(stats "$d" 11A)
	s11b=$(stats "$d" 11B); s11c=$(stats "$d" 11C)
	ok=1; [ -n "$s01" ] && [ "$s11" = "$s01" ] && [ "$s11a" = "$s01" ] && [ -n "$s11b" ] && [ "$s11c" = "$s11b" ] && ok=0
	report "toggle relations $side" $ok "11A $(hizof "$s11a") 11B $(hizof "$s11b") 11C $(hizof "$s11c") 11 $(hizof "$s11") against 01 $(hizof "$s01")"
	h1=$(hizof "$(stats "$d" 10A)"); h05=$(hizof "$(stats "$d" 10)")
	ok=1; [ -n "$h1" ] && [ -n "$h05" ] && [ "$h05" -gt 0 ] && [ "$h05" -le $((h1 + SLACK)) ] && ok=0
	report "mirror relation $side" $ok "hiz at r_mirroralpha 1 = ${h1:-?}, at 0.5 = ${h05:-?}"
done

# 3v
CLEAN=" 0 disagree on the count, 0 on the first index, 0 on the rest of the command, 0 on the indices"
for n in $VERIFIED; do
	for d in "$GL" "$GPU"; do
		section "$d" "STATS-$n" | grep -a "^worldcull_verify:" > "$T/v-$(basename "$d")"
	done
	vg=$(cat "$T/v-$(basename "$GL")"); vv=$(cat "$T/v-$(basename "$GPU")")
	main=$(head -1 <<<"$vv"); hz=$(sed -n 2p <<<"$vv")
	ok=1
	case "$main" in
	*"surfaces accepted"*"$CLEAN")
		[ "$vv" = "$vg" ] && ! grep -q "^worldcull_verify: 0 surfaces" <<<"$main" && ok=0 ;;
	esac
	h=$(hizof "$(stats "$GPU" "$n")")
	if [ -n "$hz" ]; then
		set -- $(sed -n 's/.*levels, \([0-9]*\) texels off .*; \([0-9]*\) marksurfaces culled by it on the CPU, \([0-9]*\) too close.*/\1 \2 \3/p' <<<"$hz")
		if [ $# = 3 ] && [ -n "$h" ]; then
			{ [ "$1" = 0 ] && [ "$h" -ge "$2" ] && [ "$h" -le $(($2 + $3)) ]; } || ok=1
		else
			ok=1
		fi
	elif [ "${h:-0}" != 0 ]; then
		ok=1	# Hi-Z culled, and the verify did not follow it
	fi
	report "worldcull_verify STATS-$n" $ok "$(sed 's/worldcull_verify: //' <<<"$vv" | paste -sd '|' -); GL $([ "$vv" = "$vg" ] && echo same || echo differs)"
done
for d in "$GL" "$GPU"; do
	side=$([ "$d" = "$GL" ] && echo GL || echo -gpu)
	section "$d" JUMP | grep -a "^worldcull_verify:" > "$T/j"
	ok=1
	[ "$(wc -l < "$T/j")" = 2 ] && [ "$(grep -c -- "$CLEAN\$" "$T/j")" = 2 ] && [ "$(sort -u "$T/j" | wc -l)" = 2 ] && ok=0
	report "jump verify $side" $ok "$(sed 's/worldcull_verify: //; s/ indices on the CPU.*//' "$T/j" | paste -sd '|' -)"
done

# 4
L=$GPU/qconsole.log
for sec in REPORT-OFF REPORT-PP REPORT-VILLAGE REPORT-MIRROR; do
	r=$(section "$GPU" "$sec" | grep -a "routed calls the SDL_GPU backend did not handle")
	d=$(section "$GPU" "$sec" | sed -n 's/.*SDL_GPU last frame: [0-9]* indirect draws, \([0-9]*\) dispatches dropped.*/\1/p')
	f=$(section "$GPU" "$sec" | sed -n 's/.*compute last frame: \([0-9]*\) passes, [0-9]* readbacks, \([0-9]*\) fence waits.*/\1 passes, \2 fence waits/p')
	ok=1; case "$r" in "gpu_unrouted: 0 routed calls"*) [ "$d" = 0 ] && ok=0 ;; esac
	report "unrouted $sec" $ok "${r:-no gpu_unrouted in it}; ${d:-?} dispatches dropped; last frame ${f:-?}"
done
s=$(section "$GPU" REPORT-OFF | sed -n 's/.*images cycled, \([0-9]*\) written through a scratch.*/\1/p')
ok=1; [ -n "$s" ] && [ "$s" -gt 0 ] && ok=0
report "scratch levels" $ok "${s:-?} levels written through a scratch since boot"
x=$(section "$GPU" REPORT-MIRROR | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | grep -avE ' gl(GetError|GetString)$' | tr -s ' ' | paste -sd ';' -)
[ -z "$x" ]; report "null table across the map" $? "${x:-no call into the null GL table but glGetError / glGetString}"
v=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Error")
w=$(cat "$L" "$GPU/engine.stdout" 2>/dev/null | grep -c "Validation Warning")
ok=1; [ "$v" = 0 ] && [ "$w" -gt 0 ] && ok=0
report "validation" $ok "$v errors, $w warnings (the layers loaded)"

exit $rc
