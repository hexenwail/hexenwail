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
# Example — the 2D/menu sweep, masking the animated menu cursor:
#   ./tools/shot-diff.sh /tmp/ref /tmp/new 25x20+291+60
#
# Requires ImageMagick:
#   nix shell nixpkgs#imagemagick --command ./tools/shot-diff.sh ...
#
# WHY THE MASK EXISTS.  Menu frames look bit-deterministic and are not.  The
# main-menu cursor is an animated sprite, so two runs agree only when they
# happen to sample the same animation phase — measured: runs A and B agreed on
# all four frames with 0 differing pixels, and run C of the SAME BINARY
# disagreed with A by 26, 16 and 23 pixels on three of them.  Every differing
# pixel fell inside 25x20+291+60.
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

	a=$(mktemp --suffix=.png); b=$(mktemp --suffix=.png)
	if [ ${#mask_args[@]} -gt 0 ]; then
		magick "$f"       "${mask_args[@]}" "$a"
		magick "$NEW/$n"  "${mask_args[@]}" "$b"
	else
		cp "$f" "$a"; cp "$NEW/$n" "$b"
	fi

	ae=$(magick compare -metric AE "$a" "$b" null: 2>&1 | awk '{print $1}')
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
