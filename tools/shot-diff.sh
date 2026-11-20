#!/usr/bin/env bash
#
# shot-diff.sh — compare two directories of headless-drive.sh screenshots.
#
# Usage:
#   shot-diff.sh <refdir> <newdir> [exclude-geometry ...]
#
#   exclude-geometry is ImageMagick WxH+X+Y.  Each named region is painted
#   flat in BOTH images before comparing, so animated widgets do not register
#   as differences.  Pass none to compare whole frames.
#
# Example — the 2D/menu sweep, masking the menu cursor and the console one:
#   ./tools/shot-diff.sh /tmp/ref /tmp/new 34x62+288+56 12x12+20+572
#
# Requires ImageMagick:
#   nix shell nixpkgs#imagemagick --command ./tools/shot-diff.sh ...
#
# PSNR_MIN=<dB> compares by PSNR instead, for two renderers that are meant to
# agree but not to the bit (GL against the SDL_GPU backend, #340): a frame
# passes at PSNR >= PSNR_MIN, and identical frames read "inf".  The same rule
# below holds for the threshold -- take it from three runs, see
# tools/sweep-world-gpu.steps.
#
# WHY THE MASK EXISTS.  Menu frames look bit-deterministic and are not.  The
# main-menu cursor is an animated sprite, so two runs agree only when they
# happen to sample the same animation phase — measured: runs A and B agreed on
# all four frames with 0 differing pixels, and run C of the SAME BINARY
# disagreed with A by 26, 16 and 23 pixels on three of them.  Every differing
# pixel fell inside 25x20+291+60.
#
# The console input cursor (console.c, (int)(realtime*4)&1) is the same problem
# with a nastier sampling profile: it is a two-state blink rather than a
# multi-frame animation, so a handful of runs can all land on one state and
# then a later run flips.  See tools/sweep-2d.steps.
#
# So "0 differing pixels" from a single pair of runs proves nothing on its own.
# Either mask the animated region, or establish the reference from at least
# three runs and confirm they agree.  A gate calibrated on one lucky pair will
# report phantom regressions forever, and — worse — invites whoever sees them
# to start ignoring the gate.
#
# Exit status is 1 if any frame differs outside the masked regions.
set -uo pipefail

[ $# -ge 2 ] || { sed -n '2,20p' "$0"; exit 2; }

REF=$1; NEW=$2; shift 2
EXCLUDES=("$@")

command -v magick >/dev/null || { echo "shot-diff: ImageMagick 'magick' not on PATH" >&2; exit 2; }

mask_args=()
for g in "${EXCLUDES[@]:-}"; do
	[ -n "$g" ] || continue
	mask_args+=(-fill black -draw "rectangle $(
		# WxH+X+Y -> x0,y0 x1,y1
		printf '%s' "$g" | awk -F'[x+]' '{print $3","$4" "$3+$1-1","$4+$2-1}'
	)")
done

rc=0
shopt -s nullglob
for f in "$REF"/*.png; do
	n=$(basename "$f")
	[ -f "$NEW/$n" ] || { printf '%-24s MISSING in %s\n' "$n" "$NEW"; rc=1; continue; }

	# compare reads two frames of different sizes as identical, and a frame
	# it cannot read as a number it cannot compare: both are failures.
	sa=$(magick identify -format '%wx%h' "$f" 2>/dev/null)
	sb=$(magick identify -format '%wx%h' "$NEW/$n" 2>/dev/null)
	if [ -z "$sa" ] || [ "$sa" != "$sb" ]; then
		printf '%-24s DIFF  (size %s vs %s)\n' "$n" "${sa:-unreadable}" "${sb:-unreadable}"
		rc=1; continue
	fi

	a=$(mktemp --suffix=.png); b=$(mktemp --suffix=.png)
	if [ ${#mask_args[@]} -gt 0 ]; then
		magick "$f"       "${mask_args[@]}" "$a"
		magick "$NEW/$n"  "${mask_args[@]}" "$b"
	else
		cp "$f" "$a"; cp "$NEW/$n" "$b"
	fi

	ae=$(magick compare -metric AE "$a" "$b" null: 2>&1 | awk '{print $1}')
	if [ -n "${PSNR_MIN:-}" ]; then
		# "37.01 (0.3701)"; identical frames read 120 (IM 7.1) or "inf".
		# Anything else -- an error message -- fails.
		ps=$(magick compare -metric PSNR "$a" "$b" null: 2>&1 | awk '{print $1}')
		rm -f "$a" "$b"
		if awk -v p="$ps" -v m="$PSNR_MIN" \
			'BEGIN { exit !(p == "inf" || (p ~ /^[0-9]+(\.[0-9]+)?$/ && p + 0 >= m + 0)) }'; then
			printf '%-24s OK    (PSNR %s dB, %s differing pixels)\n' "$n" "$ps" "$ae"
		else
			printf '%-24s DIFF  (PSNR %s dB < %s, %s differing pixels)\n' "$n" "$ps" "$PSNR_MIN" "$ae"
			rc=1
		fi
		continue
	fi
	rm -f "$a" "$b"

	# AE is a float ("26.2078"); anything non-zero is a difference.
	if [ "${ae%%.*}" = "0" ] || [ "$ae" = "0" ]; then
		printf '%-24s OK    (0 differing pixels)\n' "$n"
	else
		printf '%-24s DIFF  (%s differing pixels)\n' "$n" "$ae"
		rc=1
	fi
done

[ $rc -eq 0 ] && echo "shot-diff: all frames match" || echo "shot-diff: DIFFERENCES FOUND"
exit $rc
