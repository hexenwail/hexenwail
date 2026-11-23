#!/usr/bin/env bash
#
# rspeeds-ssao-check.sh -- live check that r_speeds 2 reports (nearly) every
# frame, including when the GPU falls behind the CPU (GitHub #503).
#
#   ./tools/rspeeds-ssao-check.sh [basedir] [map]
#     basedir  a Hexen II install                 (default ~/hexen2)
#     map      a map in it                        (default village1)
#
# The GPU profiler used to own one set of timestamp queries, re-issue it every
# frame and read it the next.  It printed only on a frame where the GPU had
# drained the whole queue, so the moment the GPU was the bottleneck -- r_ssao
# at 1080p is enough on a Tesla P4 -- r_speeds 2 all but stopped.  The fix
# rings RPROF_RING sets (tools/test_rprof_ring.py pins the logic); this is the
# end-to-end count, which needs a GPU, so it is a local tool and not CI.
#
# For r_ssao 0 and then 1, two runs of SECS seconds each at 1920x1080, vsync
# and frame cap off:
#   * r_speeds 1, which prints one "fps" line every frame: the frames drawn;
#   * r_speeds 2: the "GPU ..." lines it printed.
# Coverage is GPU lines / frames.  The two counts come from separate runs, so
# a fixed profiler lands near 1 on either side of it (0.87 to 1.04 measured).
# PASS needs, for each r_ssao:
#   * both runs got in-game and printed;
#   * coverage >= MIN_COVERAGE (default 0.5);
#   * a median GPU total above 0 and below the run's mean frame time;
# and the r_ssao 1 median `world` bucket above r_ssao 0's, else SSAO never ran
# and the GPU-bound case was never exercised.
#
# Tesla P4, village1 (CPU load from other jobs moves these, not the verdict):
#   before the fix  coverage 0.03-0.09 (ssao 0), 0.001-0.007 (ssao 1)
#   after the fix   coverage 0.87-1.04 (ssao 0), 0.96-1.04 (ssao 1)
#   SSAO itself costs ~1.1 ms of GPU (world 0.4 -> 1.5 ms) and ~44% of the frame
#   rate at this resolution: a real cost, not a profiler artefact.
#
# r_ssao needs the post-process scene FBO, which a default config does not
# raise; r_oit 1 raises it for every run alike (PP_NeedsPostProcess).
#
# The P4 is shared and the runs are sequential: check nvidia-smi and the load
# average first.  Coverage is a ratio within one run, so a load change between
# runs moves the frame counts but not the coverage much.
#
# Engine: ENGINE (default ./result/bin/hexenwail), a desktop GL build.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-village1}
ENGINE=${ENGINE:-./result/bin/hexenwail}
SECS=${SECS:-20}
MIN_COVERAGE=${MIN_COVERAGE:-0.5}

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/rspeeds-ssao.XXXXXX")
trap 'rm -rf "$work"' EXIT

median() { sort -n | awk '{a[NR] = $1} END {if (NR) print a[int((NR + 1) / 2)]; else print "nan"}'; }

# run <tag> <r_ssao> <r_speeds>: prints the qconsole.log path
run() {
	local ud="$work/$1"
	mkdir -p "$ud"
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 "$SECS" "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-width 1920 -height 1080 -window -force \
		+vid_vsync 0 +host_maxfps 0 +r_oit 1 +r_ssao "$2" \
		+map "$MAP" +wait +r_speeds "$3" >"$ud/stdout" 2>&1 || true
	find "$ud" -name qconsole.log -print -quit
}

fail=0
declare -A world
for s in 0 1; do
	flog=$(run "frames$s" "$s" 1)
	glog=$(run "gpu$s" "$s" 2)
	if [ -z "$flog" ] || [ -z "$glog" ]; then
		echo "FAIL r_ssao $s: no qconsole.log (see $work)"; exit 1
	fi
	[ "$s" = 1 ] || grep -m1 '^GL_RENDERER:' "$glog" || true
	frames=$(grep -c ' fps ' "$flog" || true)
	lines=$(grep -c '^GPU [0-9]' "$glog" || true)
	if [ "$frames" -eq 0 ] || [ "$lines" -eq 0 ]; then
		echo "FAIL r_ssao $s: $frames frames, $lines GPU lines (never got in-game, or no timer queries)"
		exit 1
	fi
	# GPU <total>  CPU <cpu> | world <world> ...
	gpu=$( { grep '^GPU [0-9]' "$glog" || true; } | awk '{print $2}' | median)
	world[$s]=$( { grep '^GPU [0-9]' "$glog" || true; } | awk '{print $7}' | median)
	frame_ms=$(awk -v n="$frames" -v t="$SECS" 'BEGIN {printf "%.2f", 1000 * t / n}')
	cov=$(awk -v a="$lines" -v b="$frames" 'BEGIN {printf "%.3f", a / b}')
	echo "r_ssao $s: $frames frames, $lines GPU lines, coverage $cov;" \
	     "median GPU $gpu ms (frame ${frame_ms} ms), world ${world[$s]} ms"
	if awk -v c="$cov" -v m="$MIN_COVERAGE" 'BEGIN {exit !(c < m)}'; then
		echo "FAIL r_ssao $s: coverage $cov < $MIN_COVERAGE -- r_speeds 2 skips frames"; fail=1
	fi
	if ! awk -v g="$gpu" -v f="$frame_ms" 'BEGIN {exit !(g > 0 && g < f)}'; then
		echo "FAIL r_ssao $s: median GPU $gpu ms is not inside (0, $frame_ms) -- implausible"; fail=1
	fi
done

if ! awk -v a="${world[1]}" -v b="${world[0]}" 'BEGIN {exit !(a > b)}'; then
	echo "FAIL: r_ssao 1 did not cost the world bucket anything; SSAO never ran, nothing was compared"
	fail=1
fi
[ "$fail" -eq 0 ] && echo "PASS: r_speeds 2 reports at least $MIN_COVERAGE of frames, SSAO on and off"
exit "$fail"
