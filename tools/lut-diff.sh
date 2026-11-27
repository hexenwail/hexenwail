#!/usr/bin/env bash
#
# lut-diff.sh -- the checks for tools/sweep-lut-gpu.steps: the softemu palette
# LUT built by a compute pass under -gpu, against GL's (#340 P5a-2).  See the
# steps file's header for how to take the two runs.
#
# Usage:
#   lut-diff.sh <gl-dir> <gpu-dir> <game-dir>
#
#   0. Frames 01-09 go through tools/softemu-diff.sh as they are its
#      graveyard (PSNR, palette membership, the metric and dither live, the
#      r_scale blocks, the REPORT sections GY and METRIC at 0 calls and 0
#      routed calls, no Validation Error and the layers loaded).
#   1. PSNR against GL of 10-13 >= SOFTEMU_MIN (default 40), HUD masked.
#   2. Exact on each side (AE 0, HUD masked): 10 is 05 (naive), 11 is 06
#      (Riemersma), 12 is 02 (OKLab, the -1 default), 13 is 12 (the round
#      trip) -- one table per metric however it was reached.
#   3. r_softemu_lutcheck: five lines on each side, naive, Riemersma, OKLab,
#      OKLab, OKLab, every one GPU-built.  The -gpu line's CRC-32 is GL's and
#      its count of cells off the CPU table is GL's (LUT_SLACK=0, the
#      default: lavapipe against llvmpipe, one gallivm).  With LUT_SLACK=N
#      (a Tesla P4 run against llvmpipe GL) the CRC is not compared and the
#      count may exceed GL's by N.  On each side 13's CRC is 12's, and so
#      is the fifth's, after the map change.
#   4. The boot built the LUT on the GPU, both sides: "PostProcess: palette
#      LUT built (OKLab metric, GPU)".
#   5. The -gpu run: REPORT-BOOT's routed-but-unhandled count is 0 (the boot
#      dispatch ran outside any frame); REPORT-MAP, across the map change,
#      counts 0 routed calls and no null-table call but glGetError (a load's
#      two, LM_BuildStyleLayers' error drain, on master too); renderer_status
#      there shows no dispatch dropped and at least MIN_DISPATCH (default 9:
#      the boot's, 05-07's, 10-12's and the round trip's two) since boot, every one with
#      its image cycled (the LUT is overwritten whole).
#
# THE NUMBERS, measured 2026-11-26 on the P5a-2 binary, 800x600, llvmpipe GL
# (Mesa 26.2.0) against lavapipe -gpu, three runs a side, and two Tesla P4
# -gpu runs (NVIDIA Vulkan) against the llvmpipe GL runs -- see the PR for
# the tables.  The broken builds each one catches are in
# scratchpad mutate340p5a2.py's report, summarised in the PR.
#
# Exit status is 1 if any check fails.  Requires ImageMagick and python3.
set -uo pipefail

[ $# -eq 3 ] || { sed -n '2,40p' "$0"; exit 2; }
GL=$1; GPU=$2; GAME=$3
SOFTEMU_MIN=${SOFTEMU_MIN:-40}
LUT_SLACK=${LUT_SLACK:-0}
MIN_DISPATCH=${MIN_DISPATCH:-9}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v magick >/dev/null || { echo "lut-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }
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
	magick compare -metric AE "$T/x.png" "$T/y.png" null: 2>&1 | awk '{print $1}'
}

# 0
FRAMES="01-gy-off 02-gy-se1 03-gy-se2 04-gy-se3 05-gy-se1-naive 06-gy-se1-riem 07-gy-se1-auto
08-gy-se1-dither0 09-gy-se1-scale050" SECTIONS="GY METRIC" \
	"$HERE/softemu-diff.sh" "$GL" "$GPU" "$GAME" | sed 's/^/  softemu-diff: /'
[ "${PIPESTATUS[0]}" = 0 ]; report "frames 01-09 (softemu-diff)" $? "tools/softemu-diff.sh on the graveyard"

LUTS="10-gy-lut-naive 11-gy-lut-riem 12-gy-lut-oklab 13-gy-lut-roundtrip"
for n in $LUTS; do
	for d in "$GL" "$GPU"; do
		[ -f "$d/$n.png" ] || { report "$n" 1 "missing in $d"; continue 2; }
	done
	# 1
	p=$(psnr "$GL/$n.png" "$GPU/$n.png")
	atleast "$p" "$SOFTEMU_MIN"; report "$n" $? "PSNR $p dB, min $SOFTEMU_MIN"
done

# 2
for side in GL GPU; do
	eval d=\$$side
	for pair in "10-gy-lut-naive 05-gy-se1-naive" "11-gy-lut-riem 06-gy-se1-riem" \
		    "12-gy-lut-oklab 02-gy-se1" "13-gy-lut-roundtrip 12-gy-lut-oklab"; do
		set -- $pair
		[ -f "$d/$1.png" ] && [ -f "$d/$2.png" ] || { report "${1%%-*} is ${2%%-*} $side" 1 "missing"; continue; }
		a=$(ae "$d/$1.png" "$d/$2.png")
		[ "$a" = 0 ]; report "${1%%-*} is ${2%%-*} $side" $? "$side: $a pixels differ, want 0"
	done
done

# 3
lut() { grep -a "^r_softemu_lutcheck: " "$1/qconsole.log" 2>/dev/null; }
lut "$GL" > "$T/gl.txt"; lut "$GPU" > "$T/gpu.txt"
python3 - "$T/gl.txt" "$T/gpu.txt" "$LUT_SLACK" <<'EOF' > "$T/lut.out"
import re, sys
rx = re.compile(r"^r_softemu_lutcheck: (.+?) metric, (GPU|CPU)-built LUT crc32 ([0-9a-f]{8}), "
                r"CPU grid crc32 ([0-9a-f]{8}), (\d+) of (\d+) cells differ")
want = ["naive RGB", "Riemersma", "OKLab", "OKLab", "OKLab"]
slack = int(sys.argv[3])
sides = []
for path in sys.argv[1:3]:
    rows = [rx.match(l) for l in open(path, errors="replace")]
    sides.append([m.groups() for m in rows if m])
bad = []
for name, rows in zip(("GL", "GPU"), sides):
    if [r[0] for r in rows] != want:
        bad.append("%s: metrics %s, want %s" % (name, [r[0] for r in rows], want))
    if any(r[1] != "GPU" for r in rows):
        bad.append("%s: a LUT built on the CPU" % name)
    if len(rows) == 5 and not (rows[2][2] == rows[3][2] == rows[4][2]):
        bad.append("%s: OKLab's tables differ across 12/13/14: %s" % (name, [r[2] for r in rows[2:]]))
if not bad:
    for i, (g, v) in enumerate(zip(*sides)):
        tag = "line %d (%s)" % (i + 1, g[0])
        if g[3] != v[3]:
            bad.append("%s: the CPU grids differ, GL %s and -gpu %s" % (tag, g[3], v[3]))
        if slack == 0 and (g[2] != v[2] or g[4] != v[4]):
            bad.append("%s: GL crc32 %s / %s cells off the CPU grid, -gpu %s / %s"
                       % (tag, g[2], g[4], v[2], v[4]))
        if slack > 0 and int(v[4]) > int(g[4]) + slack:
            bad.append("%s: -gpu %s cells off the CPU grid, GL %s + slack %d" % (tag, v[4], g[4], slack))
for name, rows in zip(("GL", "GPU"), sides):
    print("%s: %s" % (name, "; ".join("%s %s %s off" % (r[0], r[2], r[4]) for r in rows)))
for b in bad:
    print("BAD " + b)
sys.exit(1 if bad else 0)
EOF
ok=$?
sed 's/^/  /' "$T/lut.out"
report "lutcheck GL against -gpu" $ok "LUT_SLACK $LUT_SLACK"

# 4
for side in GL GPU; do
	eval d=\$$side
	grep -aqs "^PostProcess: palette LUT built (OKLab metric, GPU)" "$d/qconsole.log"
	report "boot LUT on the GPU $side" $? "$side: 'palette LUT built (OKLab metric, GPU)'"
done

# 5
b=$(sed -n '/^REPORT-BOOT-BEGIN/,/^REPORT-BOOT-END/p' "$GPU/qconsole.log" | grep -a "routed calls the SDL_GPU backend did not handle")
case "$b" in "gpu_unrouted: 0 routed calls"*) ok=0 ;; *) ok=1 ;; esac
report "boot routed calls" $ok "${b:-no REPORT-BOOT}"
m=$(sed -n '/^REPORT-MAP-BEGIN/,/^REPORT-MAP-END/p' "$GPU/qconsole.log")
r=$(printf '%s\n' "$m" | grep -a "routed calls the SDL_GPU backend did not handle")
x=$(printf '%s\n' "$m" | grep -aE '^ +[0-9]+  gl[A-Za-z0-9]+$' | grep -v ' glGetError$' | tr -s ' ' | paste -sd ';' -)
c=$(printf '%s\n' "$m" | grep -a "calls into .* GL entry points")
ok=1; case "$r" in "gpu_unrouted: 0 routed calls"*) [ -z "$x" ] && [ -n "$c" ] && ok=0 ;; esac
report "map change routed calls" $ok "${r:-no REPORT-MAP}; ${c:-no null-table count}; other than glGetError: ${x:-none}"
s=$(sed -n '/^REPORT-MAP-BEGIN/,/^REPORT-MAP-END/p' "$GPU/qconsole.log" | grep -a "SDL_GPU compute since boot:")
n=$(printf '%s' "$s" | sed -n 's/.*since boot: \([0-9]*\) dispatches (\([0-9]*\) images cycled), \([0-9]*\) dropped.*/\1 \2 \3/p')
set -- $n
ok=1; [ $# = 3 ] && [ "$1" -ge "$MIN_DISPATCH" ] && [ "$2" = "$1" ] && [ "$3" = 0 ] && ok=0
report "dispatches" $ok "${s:-no compute line in REPORT-MAP}; min $MIN_DISPATCH, all cycled, 0 dropped"

[ $rc -eq 0 ] && echo "lut-diff: all checks pass" || echo "lut-diff: DIFFERENCES FOUND"
exit $rc
