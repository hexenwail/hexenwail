#!/usr/bin/env bash
#
# mirror-leaks-check.sh -- the checks for tools/sweep-mirror-leaks.steps
# (#517).  See the steps file's header for how to take the run.
#
# Usage:
#   mirror-leaks-check.sh <dir>
#
#   1. caustics   20-pool against 21-pool-nocaus (r_caustics 1 / 0, the view
#                 in the water, the mirror at 0.5) changes at least MIN_CAUS
#                 pixels (default 20000; the status bar's health masked).
#                 Before the fix the mirror took its caustics from the
#                 reflected leaf, which is not water, and only the pool's
#                 walls changed.
#   2. r_speeds   what r_drawviewmodel 1 adds to r_speeds 1's epoly is the
#                 same with the mirror on (REPORT-SPEEDS-MIRROR-VM-* against
#                 REPORT-SPEEDS-MIRROR-*) as off (A1-VM against A1), and more
#                 than 0.  Only R_RenderView draws the view model, so its
#                 polys are the view's own count; before the fix the
#                 reflected pass zeroed them, and with the mirror on they
#                 added 0.  Each section must print one wpoly/epoly pair
#                 (the game is paused, so every frame's is the same).
#
# Exit status is 1 if a check fails.  Requires ImageMagick.
set -uo pipefail

[ $# -eq 1 ] || { sed -n '2,24p' "$0"; exit 2; }
D=$1
MIN_CAUS=${MIN_CAUS:-20000}
command -v magick >/dev/null || { echo "mirror-leaks-check: ImageMagick 'magick' not on PATH" >&2; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0
report() {	# name ok text
	if [ "$2" = 0 ]; then printf '%-12s OK    (%s)\n' "$1" "$3"
	else printf '%-12s FAIL  (%s)\n' "$1" "$3"; rc=1; fi
}

for f in 20-pool 21-pool-nocaus; do
	[ -f "$D/$f.png" ] || { echo "mirror-leaks-check: $D/$f.png missing"; exit 1; }
done
mask() { magick "$1" -alpha off -fill black -draw "rectangle 100,576 319,599" "$2"; }
mask "$D/20-pool.png" "$T/a.png"; mask "$D/21-pool-nocaus.png" "$T/b.png"
c=$(magick "$T/a.png" "$T/b.png" -compose difference -composite -separate -evaluate-sequence max \
	-threshold 0 -format '%[fx:int(mean*w*h+0.5)]' info:)
c=${c:-0}
[ "$c" -ge "$MIN_CAUS" ]; report caustics $? "r_caustics changes $c pixels with the mirror on, min $MIN_CAUS"

LOG=$D/qconsole.log
[ -f "$LOG" ] || { echo "mirror-leaks-check: $LOG missing"; exit 1; }
# The distinct values of one column ($1 wpoly, $3 epoly) r_speeds printed
# between a section's markers.  echo leaves a trailing blank on the marker.
section() {
	awk -v b="REPORT-SPEEDS-$1-BEGIN" -v e="REPORT-SPEEDS-$1-END" -v col="$2" '
		{ sub(/[ \t]+$/, "") }
		$0 == b { on = 1; next }
		$0 == e { on = 0 }
		on && $2 == "wpoly" && $4 == "epoly" { print $col }' "$LOG" | sort -un
}
declare -A EP
ok=1
for s in MIRROR-VM MIRROR A1-VM A1; do
	v=$(section "$s" 3)
	if [ -z "$v" ] || [ "$(printf '%s\n' "$v" | wc -l)" -ne 1 ]; then
		report r_speeds 1 "section $s: want one epoly count, got '$(printf '%s' "$v" | tr '\n' ' ')'"
		ok=0
	fi
	EP[$s]=$v
done
if [ $ok = 1 ]; then
	vm_a1=$(( ${EP[A1-VM]} - ${EP[A1]} ))
	vm_mir=$(( ${EP[MIRROR-VM]} - ${EP[MIRROR]} ))
	[ "$vm_a1" -gt 0 ] && [ "$vm_mir" -eq "$vm_a1" ]
	report r_speeds $? "the view model adds $vm_mir epoly with the mirror on, $vm_a1 with it off"
fi

[ $rc -eq 0 ] && echo "mirror-leaks-check: all checks pass" || echo "mirror-leaks-check: LEAKS FOUND"
exit $rc
