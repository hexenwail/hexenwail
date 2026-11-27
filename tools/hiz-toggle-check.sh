#!/usr/bin/env bash
#
# hiz-toggle-check.sh -- live check that Hi-Z culling never uses depth from a
# frame other than the last one, when the post-process chain comes up or goes
# down at runtime (GitHub #539).
#
#   ./tools/hiz-toggle-check.sh [basedir]
#     basedir  a Hexen II install with village1   (default ~/hexen2)
#
# r_ssao now raises the post chain itself, so typing `r_ssao 1` or `0` in a
# running game is the common way to bring the chain up or take it down.  Two
# older faults sat on that path; gamma had always reached them too:
#   * up, with MSAA (vid_fsaa 4, the default): the scene goes to a
#     multisampled FBO with no sampler-readable depth, the Hi-Z build is
#     skipped, and the cull went on using the pyramid of the last frame before
#     the toggle with the current frame's matrix -- surfaces in view were
#     culled.  Master, village1 after turning 90 degrees: hiz=65 where no
#     pyramid exists and the answer is 0.
#   * down, without MSAA: the scene FBO stays allocated, its getters kept
#     handing it out, and Hi-Z was built from a depth texture nothing drew
#     into any more.  Master: hiz=356 at a view whose answer is 199.
#
# Each run loads village1, stands at view A, raises the chain (r_ssao 1, then
# in a second run gamma 0.9), turns to view B, drops the chain at B, turns back
# to A, and samples gl_hiz_stats at each step.  The numbers depend on the
# resolution and the map; the check is on relations, which hold everywhere:
#   * A before and A after the round trip agree (no stale depth);
#   * with MSAA, while the chain is up, hiz=0 (nothing to cull against);
#   * without MSAA, B with the chain up and B with it down agree.
#
# Engine: ENGINE (default ./result/bin/hexenwail), desktop GL.  The commands
# go through an exec'd cfg because a "+setpos 200 -784 ..." on the command
# line ends at "-784".  ENGINE_ARGS go on the command line ahead of the map,
# and TIMEOUT (default 180) bounds each run.  The SDL_GPU backend (#340 P5d):
#
#   ENGINE_ARGS="-gpu +gpu_allow_software 1" ./tools/hiz-toggle-check.sh
#
# with VK_ICD_FILENAMES naming the device to run on.  -gpu has no MSAA (the
# scene is single-sampled whatever vid_fsaa says), so it takes the fsaa 0
# runs alone: an fsaa 4 run would hold it to an MSAA relation it has no
# MSAA for.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
ENGINE=${ENGINE:-./result/bin/hexenwail}
ENGINE_ARGS=${ENGINE_ARGS:-}
TIMEOUT=${TIMEOUT:-180}
case " $ENGINE_ARGS " in *" -gpu "*) FSAAS=0 ;; *) FSAAS="4 0" ;; esac

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/hiz-toggle.XXXXXX")
trap 'rm -rf "$work"' EXIT

w() { local i; for i in $(seq "$1"); do echo wait; done; }
st() { echo "echo PHASE-$1"; echo "gl_hiz_stats 1"; w 30; echo "gl_hiz_stats 0"; w 5; }

# run <name> <fsaa> <on-cmd> <off-cmd>: prints "phase hiz" lines, one per phase
run() {
	local ud="$work/$1"
	mkdir -p "$ud/data1"
	{
		w 400; echo god; echo notarget
		echo "setpos 200 -784 30 0 90 0"; w 60; st 1
		echo "$3"; w 60; st 2
		echo "setpos 200 -784 30 0 180 0"; w 60; st 3
		echo "$4"; w 60; st 4
		echo "setpos 200 -784 30 0 90 0"; w 60; st 5
		echo quit
	} >"$ud/data1/probe.cfg"
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 "$TIMEOUT" "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-width 800 -height 600 -window -fsaa "$2" $ENGINE_ARGS +gl_hiz_cull 1 \
		+map village1 +exec probe.cfg >"$ud/stdout" 2>&1 || true
	local log
	log=$(find "$ud" -name qconsole.log -print -quit)
	[ -n "$log" ] || return 0
	# Every sample of a phase must agree (static camera); print the set.
	awk '/^PHASE-/ {p = substr($1, 7)}
	     /^hiz_stats:/ {for (i = 2; i <= NF; i++) if ($i ~ /^hiz=/) h[p] = h[p] " " substr($i, 5)}
	     END {for (k in h) {n = split(h[k], a, " "); u = a[1]; for (i = 2; i <= n; i++) if (a[i] != u) u = "varies"; print k, u}}' \
		"$log" | sort
}

fail=0
for toggle in "r_ssao 1|r_ssao 0" "gamma 0.9|gamma 1"; do
	on=${toggle%%|*}; off=${toggle##*|}
	for fsaa in $FSAAS; do
		name="${on%% *}-fsaa$fsaa"
		declare -A h=()
		while read -r p v; do h[$p]=$v; done < <(run "$name" "$fsaa" "$on" "$off")
		got="A=${h[1]:-?} up=${h[2]:-?} B-up=${h[3]:-?} B-down=${h[4]:-?} A-again=${h[5]:-?}"
		bad=
		for p in 1 2 3 4 5; do
			case "${h[$p]:-?}" in ''|'?'|varies) bad="phase $p has no steady hiz";; esac
		done
		if [ -z "$bad" ]; then
			[ "${h[1]}" = "${h[5]}" ] || bad="A after the round trip differs: stale depth"
			if [ "$fsaa" -ge 2 ]; then
				{ [ "${h[2]}" = 0 ] && [ "${h[3]}" = 0 ]; } || bad="${bad:+$bad; }hiz>0 with the MSAA chain up: stale pyramid"
			else
				[ "${h[3]}" = "${h[4]}" ] || bad="${bad:+$bad; }B differs with the chain up and down"
			fi
		fi
		if [ -n "$bad" ]; then
			echo "FAIL $name: $got -- $bad"; fail=1
		else
			echo "ok   $name: $got"
		fi
		unset h
	done
done

[ "$fail" -eq 0 ] && echo "PASS: Hi-Z culls only against the last frame's depth across post-chain toggles"
exit "$fail"
