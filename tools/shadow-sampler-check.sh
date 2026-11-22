#!/usr/bin/env bash
#
# shadow-sampler-check.sh -- live check that no world or alias draw runs with
# a non-depth texture on the shadow-map unit (GitHub #526).
#
#   ./tools/shadow-sampler-check.sh [basedir] [map]
#     basedir  a Hexen II install, or .#demodata's share/hexenwail (default ~/hexen2)
#     map      a map in it                                       (default demo1)
#
# The world and alias fragment stages declare u_dlshadowmap, a
# samplerCubeArrayShadow on DLSHADOW_TMU, whatever the frame holds, and GL
# validates a sampler against its unit at every draw whether the shader reads
# it or not.  R_Shadow_Draw used to bind the cube array only on a frame with a
# map, so every frame without one -- r_dynamic 0, r_dlight_occlusion 0 or 1,
# r_shadow_maxlights 0, or just no dlight yet -- drew with texture 0 there, and
# the driver logged one undefined-behaviour report per draw: 27,844 on a demo1
# run of this length, 138,624 on Storm over Thyrion's courtyard.
#
# Each case loads the map offscreen with KHR_debug on (the engine enables it on
# desktop GL and prints every non-notification message as "GL [SEVERITY]: ..."
# to qconsole.log), waits, prints r_shadowinfo and quits.  PASS needs, per case:
#   * "GL debug output enabled" in the log  (else the check could not see it)
#   * the r_shadowinfo line, with the case's r_dlight_occlusion  (it got in-game)
#   * zero messages about a shadow sampler or a non-depth texture
#
# The message is the driver's: NVIDIA's text is "... with a texture object (0)
# with a non-depth format, by a shader that samples it with a shadow sampler".
# A driver that does not report this class passes vacuously, so the GL vendor
# and renderer are printed and a PASS anywhere but NVIDIA says it is
# unverified.  The reference box is a Tesla P4, where reverting the fix fails
# every case.
#
# Engine: ENGINE (default ./result/bin/hexenwail), a desktop GL build -- the ES
# tier has no KHR_debug and declares no shadow sampler.  Runs offscreen; on a
# box without a GPU set SDL_VIDEODRIVER=x11 under Xvfb instead.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-demo1}
ENGINE=${ENGINE:-./result/bin/hexenwail}
WAITS=${WAITS:-200}
PATTERN='shadow sampler|non-depth|depth comparisons'

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/shadow-sampler.XXXXXX")
trap 'rm -rf "$work"' EXIT

# name | the r_dlight_occlusion r_shadowinfo must report | cvars.  r_dynamic 1
# on demo1's start has no dlight in view, so it is the "no map this frame" case
# at the default r_dlight_occlusion 2.
cases=(
	"dynamic0|2|+r_dynamic 0"
	"dynamic1|2|+r_dynamic 1"
	"occlusion0|0|+r_dlight_occlusion 0"
	"occlusion1|1|+r_dlight_occlusion 1"
	"maxlights0|2|+r_shadow_maxlights 0"
)

waits=$(printf '+wait %.0s' $(seq "$WAITS"))
fail=0
vendor=
for c in "${cases[@]}"; do
	IFS='|' read -r name occ rest <<<"$c"
	# shellcheck disable=SC2206  # the cvars are meant to split into + args
	cvars=($rest)
	ud="$work/$name"
	mkdir -p "$ud"
	# shellcheck disable=SC2086  # $waits is meant to split into +wait args
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 240 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		"${cvars[@]}" +map "$MAP" $waits +r_shadowinfo +quit >"$ud/stdout" 2>&1 || true
	log=$(find "$ud" -name qconsole.log -print -quit)
	if [ -z "$log" ]; then
		echo "FAIL $name: no qconsole.log (see $ud/stdout)"; fail=1; continue
	fi
	if [ -z "$vendor" ]; then
		vendor=$(grep -m1 '^GL_VENDOR:' "$log" || true)
		echo "${vendor:-GL_VENDOR: (not logged)}"
		grep -m1 '^GL_RENDERER:' "$log" || true
	fi
	if ! grep -q 'GL debug output enabled' "$log"; then
		echo "FAIL $name: KHR_debug was not enabled, nothing could be seen"; fail=1; continue
	fi
	if ! grep -qE "^--- r_shadowinfo \(r_dlight_occlusion $occ," "$log"; then
		echo "FAIL $name: no r_shadowinfo at r_dlight_occlusion $occ, the run never got in-game"
		fail=1; continue
	fi
	n=$(grep -E '^GL \[' "$log" | grep -cE "$PATTERN" || true)
	if [ "$n" -ne 0 ]; then
		echo "FAIL $name: $n shadow-sampler reports, first:"
		grep -m1 -E "^GL \\[.*($PATTERN)" "$log" | sed 's/^/    /'
		fail=1
	else
		echo "ok   $name: 0 shadow-sampler reports"
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "FAIL: see above"
elif [[ "$vendor" == *NVIDIA* ]]; then
	echo "PASS: no draw sampled a non-depth texture through the shadow sampler"
else
	echo "PASS (unverified on this driver: it may not report this class)"
fi
exit "$fail"
