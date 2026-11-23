#!/usr/bin/env bash
#
# ssao-default-check.sh -- live check that r_ssao works on a default config,
# set at startup or toggled at runtime (GitHub #539).
#
#   ./tools/ssao-default-check.sh [basedir] [map]
#     basedir  a Hexen II install                 (default ~/hexen2)
#     map      a map in it                        (default village1)
#
# SSAO snapshots depth and composites through the post-process scene FBO, and
# nothing raised that FBO for it: with gamma 1, contrast 1, r_scale 1 and every
# other post effect off -- a fresh config -- r_ssao 1 drew exactly what r_ssao 0
# drew.  PP_NeedsPostProcess now asks R_SSAO_WantsSceneFBO.
#
# The witness is r_ssao_debug 1, which replaces the world's pixels with the
# occlusion multiplier (white = none): a frame that is mostly white is a frame
# SSAO ran on, independent of where the camera lands or what moves.  Each case
# loads the map into an empty -userdir (a default config), waits, screenshots:
#   off      r_ssao 0                         -> not white  (the control)
#   on       r_ssao 1 from the command line   -> white
#   toggle   r_ssao 0, shot, r_ssao 1, shot   -> not white, then white
# "White" is the share of pixels at >= 98% grey, against WHITE_MIN (default
# 0.5).  Measured on a Tesla P4, village1: off 0.00, on 0.80, toggle 0.00 then
# 0.80; on master, on and toggle-after stay at 0.00 and fail.
#
# Needs ImageMagick (`magick`): run under
#   nix shell nixpkgs#imagemagick --command ./tools/ssao-default-check.sh
# Engine: ENGINE (default ./result/bin/hexenwail), a desktop GL build -- the
# ES tier has no compute and no SSAO.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

BASEDIR=${1:-$HOME/hexen2}
MAP=${2:-village1}
ENGINE=${ENGINE:-./result/bin/hexenwail}
WAITS=${WAITS:-300}
WHITE_MIN=${WHITE_MIN:-0.5}

[ -x "$ENGINE" ] || { echo "no engine at $ENGINE (nix build .#default)" >&2; exit 2; }
command -v magick >/dev/null || { echo "no magick: run under nix shell nixpkgs#imagemagick" >&2; exit 2; }
BASEDIR=$(readlink -f "$BASEDIR")
[ -d "$BASEDIR/data1" ] || { echo "no $BASEDIR/data1" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/ssao-default.XXXXXX")
trap 'rm -rf "$work"' EXIT

waits=$(printf '+wait %.0s' $(seq "$WAITS"))

# shoot <case> <args...>: runs the engine, prints the shots' white shares in order
shoot() {
	local ud="$work/$1"; shift
	mkdir -p "$ud"
	# shellcheck disable=SC2086  # $waits is meant to split into +wait args
	env -u LD_LIBRARY_PATH SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
		timeout -k 5 180 "$ENGINE" -condebug -basedir "$BASEDIR" -userdir "$ud" \
		-width 800 -height 600 -window +r_ssao_debug 1 "$@" +quit >"$ud/stdout" 2>&1 || true
	# C collation: a second shot in the same second is X_0000.png, which a
	# UTF-8 locale sorts before X.png.
	find "$ud" -path '*/shots/*.png' | LC_ALL=C sort | while read -r png; do
		magick "$png" -colorspace Gray -threshold 98% -format '%[fx:mean]\n' info:
	done
}

white() { awk -v w="$1" -v m="$WHITE_MIN" 'BEGIN {exit !(w >= m)}'; }

fail=0
check() {	# <case> <want: white|dark ...> -- <shares...>
	local name=$1; shift
	local -a want=() got=()
	while [ "$1" != -- ]; do want+=("$1"); shift; done; shift
	got=("$@")
	if [ "${#got[@]}" -ne "${#want[@]}" ]; then
		echo "FAIL $name: ${#got[@]} screenshots, want ${#want[@]} (never got in-game?)"; fail=1; return
	fi
	local i ok=1
	for i in "${!want[@]}"; do
		if [ "${want[$i]}" = white ] && ! white "${got[$i]}"; then ok=0; fi
		if [ "${want[$i]}" = dark ] && white "${got[$i]}"; then ok=0; fi
	done
	if [ "$ok" = 1 ]; then
		echo "ok   $name: white ${got[*]} (want ${want[*]})"
	else
		echo "FAIL $name: white ${got[*]} (want ${want[*]})"; fail=1
	fi
}

# shellcheck disable=SC2086
mapfile -t s < <(shoot off +r_ssao 0 +map "$MAP" $waits +screenshot)
check off dark -- "${s[@]}"
# shellcheck disable=SC2086
mapfile -t s < <(shoot on +r_ssao 1 +map "$MAP" $waits +screenshot)
check on white -- "${s[@]}"
# shellcheck disable=SC2086
mapfile -t s < <(shoot toggle +r_ssao 0 +map "$MAP" $waits +screenshot $waits +r_ssao 1 $waits +screenshot)
check toggle dark white -- "${s[@]}"

[ "$fail" -eq 0 ] && echo "PASS: r_ssao runs on a default config, from startup and toggled at runtime"
exit "$fail"
