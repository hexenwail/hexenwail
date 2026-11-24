#!/usr/bin/env bash
#
# pp-diff.sh -- the checks for tools/sweep-pp-gpu.steps: GL's frames against
# the SDL_GPU backend's, the post-process chain on render targets (#340
# P4a-2).  See the steps file's header for how to take the two runs and where
# the thresholds come from.
#
# Usage:
#   pp-diff.sh <gl-dir> <gpu-dir>
#
# Every frame is compared with the mini status bar's health masked
# (210x24+100+576), the console frame with its blinking input cursor masked
# too, and:
#
#   0. Both sides have every frame, each 800x600, the size the masks and
#      thresholds are for.
#   1. Every frame but 20-gy-bloom (check 5's): PSNR against GL >= PSNR_MIN
#      (default 42), as tools/shot-diff.sh's PSNR_MIN mode; the r_softemu
#      frames 26-28 >= SOFTEMU_MIN (default 40), because snapping to the
#      palette turns a rasterizer's last-bit differences into whole palette
#      steps (Tesla P4 against llvmpipe: 51-52 dB before it, 41.7 after).
#   2. The identity chain: on -gpu, 02-gy-identity (r_waterwarp 1 above
#      water wakes the chain and changes nothing) equals 01-gy-ppoff exactly
#      -- the scene through pp_fbo, the blit into the native target, the 2D
#      and the final pass, all at identity, are the frame drawn straight.
#      GL's two differ in some 2,500 scattered texels by at most 15/255, the
#      same on the binary before #340 P4a-2 (its default framebuffer and its
#      FBO do not round alike), so GL's count is printed, not checked.
#   3. Each effect is there, on each side: 03 (gamma 0.7), 04 (gamma 0.85,
#      contrast 1.5), 05 (r_scale 0.5), 07 (FXAA) and 14 (gamma 0.9) each
#      change at least MIN_CHANGED (default 2000) pixels against 01.  Two
#      sides that both lost an effect would agree in 1.
#   4. The upscale is r_scale's: in 05 (r_scale 0.5) every 2x2 block of the
#      3D view (the top 560 rows, above the native-resolution HUD) is one
#      colour, on each side -- an upscale that filtered, or a viewport left
#      at the native size, fails it (08 is 0.5 too, but its FXAA runs after
#      the upscale).  In 06 (0.75) the 3D view has at
#      most 3/4 of its 560 rows distinct (+1%, 426), on each side: every
#      fourth row repeats the one above.
#   5. The gates that keep P6's and P4e's work off under -gpu, as frames that
#      must be identical on -gpu: 13-pools-oit equals 09-pools (r_oit 1: OIT
#      is P6, its gates off -- a removed gate loses every translucent), and
#      20-gy-bloom equals 14-gy-gamma09 (r_bloom 1: bloom is P4e, PP_BloomOn
#      false).  Bloom's GL pair must differ (else the check proves nothing);
#      OIT's is printed, not checked: GL's OIT resolves one water layer to
#      the pixels sorted blending gives (0 differ at gamma 0.9), so what
#      shows the gate matters is the broken build without it, not GL.
#
# THE THRESHOLDS are tabled in tools/sweep-pp-gpu.steps' header: the honest
# numbers they were set from and the broken builds that fail them.
#
# FRAMES=<names> narrows every check to those frames (a check whose frames
# are not all in it is skipped).
#
# Exit status is 1 if any check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 2 ] || { sed -n '2,55p' "$0"; exit 2; }
GL=$1; GPU=$2
PSNR_MIN=${PSNR_MIN:-42}
SOFTEMU_MIN=${SOFTEMU_MIN:-40}
MIN_CHANGED=${MIN_CHANGED:-2000}

FRAMES=${FRAMES:-"01-gy-ppoff 02-gy-identity 03-gy-gamma07 04-gy-g085-c15 05-gy-scale050 06-gy-scale075
07-gy-fxaa 08-gy-combo 14-gy-gamma09 20-gy-bloom 26-gy-softemu1 27-gy-softemu2 28-gy-softemu3
09-pools 13-pools-oit 12-console 10-c4-lava 11-egypt-north"}

command -v magick >/dev/null || { echo "pp-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0

HUD="rectangle 100,576 309,599"
CURSOR="rectangle 20,273 30,283"	# the console's input cursor, blinking on realtime
mask() {
	case $(basename "$1") in
	12-console*) magick "$1" -alpha off -fill black -draw "$HUD $CURSOR" "$2" ;;
	*)           magick "$1" -alpha off -fill black -draw "$HUD" "$2" ;;
	esac
}
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
# distinct rows in the 3D view: rows equal to the one above collapse
rows() {
	magick "$1" -alpha off -crop 800x560+0+0 +repage -depth 8 rgb:- |
		python3 -c 'import sys; d=sys.stdin.buffer.read(); w=800*3
r=[d[i*w:(i+1)*w] for i in range(len(d)//w)]
print(1+sum(1 for i in range(1,len(r)) if r[i]!=r[i-1]))'
}
has() { for f in "$@"; do case " $(echo $FRAMES) " in *" $f "*) ;; *) return 1 ;; esac; done; }
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-32s OK    (%s)\n' "$1" "$3"
	else printf '%-32s DIFF  (%s)\n' "$1" "$3"; rc=1; fi
}

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
[ $rc = 0 ] || { echo "pp-diff: FRAMES MISSING OR THE WRONG SIZE"; exit 1; }

# 1
for n in $FRAMES; do
	[ "$n" = 20-gy-bloom ] && continue
	min=$PSNR_MIN
	case $n in 2[678]-gy-softemu*) min=$SOFTEMU_MIN ;; esac
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$min"; report "$n" $? "PSNR $p dB, min $min"
done

# 2
if has 01-gy-ppoff 02-gy-identity; then
	c=$(changed "$GPU/02-gy-identity.png" "$GPU/01-gy-ppoff.png")
	[ "${c:-1}" = 0 ]; report "identity chain -gpu" $? "-gpu: 02 against 01, $c pixels differ, want 0"
	c=$(changed "$GL/02-gy-identity.png" "$GL/01-gy-ppoff.png")
	printf '%-32s INFO  (GL: 02 against 01, %s pixels differ, not checked)\n' "identity chain GL" "$c"
fi

# 3
for n in 03-gy-gamma07 04-gy-g085-c15 05-gy-scale050 07-gy-fxaa 14-gy-gamma09; do
	has 01-gy-ppoff "$n" || continue
	for side in GL GPU; do
		eval d=\$$side
		c=$(changed "$d/$n.png" "$d/01-gy-ppoff.png"); c=${c:-0}
		[ "$c" -ge "$MIN_CHANGED" ]; report "effect ${n%%-*} $side" $? "$side: $n changes $c pixels against 01, min $MIN_CHANGED"
	done
done

# 4
for n in 05-gy-scale050; do
	has "$n" || continue
	for side in GL GPU; do
		eval d=\$$side
		c=$(blocky "$d/$n.png")
		[ "${c:-1}" = 0 ]; report "2x2 blocks ${n%%-*} $side" $? "$side: $c pixels of the 3D view off their 2x2 block, want 0"
	done
done
if has 06-gy-scale075; then
	lim=$(awk 'BEGIN { printf "%d", 560 * 0.76 + 0.5 }')
	for side in GL GPU; do
		eval d=\$$side
		r=$(rows "$d/06-gy-scale075.png")
		[ "${r:-999}" -le "$lim" ]; report "3/4 rows 06 $side" $? "$side: $r distinct rows of 560, max $lim"
	done
fi

# 5
for pr in "13-pools-oit 09-pools oit" "20-gy-bloom 14-gy-gamma09 bloom"; do
	set -- $pr
	has "$1" "$2" || continue
	c=$(changed "$GPU/$1.png" "$GPU/$2.png")
	[ "${c:-1}" = 0 ]; report "$3 gate -gpu" $? "-gpu: $1 against $2, $c pixels differ, want 0"
	c=$(changed "$GL/$1.png" "$GL/$2.png"); c=${c:-0}
	if [ "$3" = oit ]; then
		printf '%-32s INFO  (GL: %s against %s, %s pixels differ, not checked)\n' "$3 effect GL" "$1" "$2" "$c"
		continue
	fi
	[ "$c" -ge "$MIN_CHANGED" ]; report "$3 effect GL" $? "GL: $1 against $2, $c pixels differ, min $MIN_CHANGED (else the gate check proves nothing)"
done

[ $rc -eq 0 ] && echo "pp-diff: all checks pass" || echo "pp-diff: DIFFERENCES FOUND"
exit $rc
