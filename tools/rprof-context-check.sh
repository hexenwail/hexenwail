#!/usr/bin/env bash
#
# rprof-context-check.sh -- live check of r_speeds 2's GPU line across the
# backend it runs on (GitHub #540).
#
#   ./tools/rprof-context-check.sh [basedir] [map]
#     basedir  a Hexen II install                 (default ~/hexen2)
#     map      a map in it                        (default village1)
#
# Two cases, each FAILing on master:
#   gpu      -gpu under Xvfb (SDL_GPU cannot present to the offscreen driver,
#            and would fall back to GL): every r_speeds 2 report must be
#            "GPU timing unavailable (...)" and none "GPU 0.0 ...".  Master
#            printed 99 "GPU 0.0" lines and 0 unavailable ones.
#   restart  desktop GL, offscreen: r_speeds 2, vid_restart, r_speeds 2 again,
#            with KHR_debug on.  After the restart the GPU line must come back
#            and the driver must report no GL error.  Master raised
#            GL_INVALID_OPERATION "Query object not found" (NVIDIA Tesla P4,
#            llvmpipe), then recovered on the driver's leniency.
#
# Needs Xvfb on PATH for the gpu case:
#   nix shell nixpkgs#xorg-server --command ./tools/rprof-context-check.sh
# Engine: ENGINE (default ./result/bin/hexenwail), a desktop GL build with
# the SDL_GPU backend.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-village1}
ENGINE=${ENGINE:-./result/bin/hexenwail}
WAITS=${WAITS:-100}

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
command -v Xvfb >/dev/null || { echo "no Xvfb: run under nix shell nixpkgs#xorg-server" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/rprof-context.XXXXXX")
xpid=
fail=0
# The run's logs stay on a FAIL (the messages point into them), or with KEEP=1.
cleanup() {
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	if [ "$fail" -eq 0 ] && [ -z "${KEEP:-}" ]; then rm -rf "$work"; else echo "logs kept in $work"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

waits=$(printf '+wait %.0s' $(seq "$WAITS"))
count() { grep -c "$1" "$2" || true; }

# ---- gpu
# -displayfd: the server picks a free display itself, so a second session's
# Xvfb is never borrowed (tools/headless-drive.sh does the same).
dfd="$work/displayfd"
: >"$dfd"
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$dfd" >"$work/xvfb.log" 2>&1 &
xpid=$!
for _ in $(seq 100); do
	[ -s "$dfd" ] && break
	kill -0 "$xpid" 2>/dev/null || break
	sleep 0.2
done
disp=":$(tr -d '[:space:]' <"$dfd")"
[ "$disp" != ":" ] || { echo "FAIL gpu: Xvfb never reported a display (see $work/xvfb.log)"; fail=1; exit 1; }
mkdir -p "$work/gpu"
# shellcheck disable=SC2086  # $waits is meant to split into +wait args
env -u LD_LIBRARY_PATH DISPLAY="$disp" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
	timeout -k 5 180 "$ENGINE" -gpu -condebug -basedir "$BASEDIR" -userdir "$work/gpu" \
	-width 640 -height 480 -window +map "$MAP" $waits +r_speeds 2 $waits +quit >"$work/gpu/stdout" 2>&1 || true
log=$(find "$work/gpu" -name qconsole.log -print -quit)
if [ -z "$log" ]; then
	echo "FAIL gpu: no qconsole.log"; fail=1
elif [ "$(count 'falling back to OpenGL' "$log")" -ne 0 ]; then
	echo "FAIL gpu: SDL_GPU fell back to OpenGL, nothing was checked"; fail=1
else
	un=$(count '^GPU timing unavailable' "$log")
	num=$(count '^GPU [0-9]' "$log")
	if [ "$un" -gt 0 ] && [ "$num" -eq 0 ]; then
		echo "ok   gpu: $un reports name the missing timer, 0 print GPU numbers"
	else
		echo "FAIL gpu: $un 'GPU timing unavailable', $num 'GPU <ms>' lines (want >0 and 0)"; fail=1
	fi
fi
kill "$xpid" 2>/dev/null || true
xpid=

# ---- restart
mkdir -p "$work/gl"
# shellcheck disable=SC2086
env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
	timeout -k 5 240 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$work/gl" \
	-width 640 -height 480 -window +map "$MAP" $waits +r_speeds 2 $waits \
	+echo RPROF-RESTART +vid_restart $waits $waits +echo RPROF-END +quit >"$work/gl/stdout" 2>&1 || true
log=$(find "$work/gl" -name qconsole.log -print -quit)
if [ -z "$log" ] || [ "$(count '^RPROF-END' "$log")" -eq 0 ]; then
	echo "FAIL restart: the run never got past vid_restart (see $work/gl/stdout)"; fail=1
else
	grep -m1 '^GL_RENDERER:' "$log" || true
	# Everything after the marker: the echo prints it at column 0, while
	# the echoed command line has it after "+echo ".
	sed -n '/^RPROF-RESTART/,$p' "$log" >"$work/gl/after"
	before=$(sed -n '/^RPROF-RESTART/q;p' "$log" | { grep -c '^GPU [0-9]' || true; })
	after=$(count '^GPU [0-9]' "$work/gl/after")
	# The engine's own line for every GL_DEBUG_TYPE_ERROR / UNDEFINED_BEHAVIOR,
	# whatever the driver's severity or wording.
	errs=$(count '^GL: fatal error type' "$work/gl/after")
	if [ "$(count '^Re-initializing video' "$work/gl/after")" -eq 0 ]; then
		echo "FAIL restart: vid_restart did not re-initialize video (locked, or the mode was refused)"; fail=1
	elif [ "$(count 'GL debug output enabled' "$work/gl/after")" -eq 0 ]; then
		echo "FAIL restart: KHR_debug was not on in the new context, a GL error could not be seen"; fail=1
	elif [ "$before" -gt 0 ] && [ "$after" -gt 0 ] && [ "$errs" -eq 0 ]; then
		echo "ok   restart: GPU lines $before before, $after after vid_restart, 0 GL errors after"
	else
		echo "FAIL restart: GPU lines $before before, $after after, $errs GL errors after (want >0, >0, 0)"
		grep -m3 -E '^GL \[' "$work/gl/after" | sed 's/^/    /' || true
		fail=1
	fi
fi

[ "$fail" -eq 0 ] && echo "PASS: r_speeds 2 names -gpu's missing timer and survives vid_restart cleanly"
exit "$fail"
