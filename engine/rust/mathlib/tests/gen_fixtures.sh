#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/mathlib.txt from the C original,
# engine/h2shared/mathlib.c.  Needs cc; the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
fixture="$here/fixtures/mathlib.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

# -ffp-contract=off: no fma fusion, so the recorded bits are the plain IEEE
# float results on any target.
cc -O1 -Wall -Wno-unused-function -ffp-contract=off -DGLQUAKE \
	-I"$root/engine/hexen2" -I"$root/engine/h2shared" -I"$root/common" \
	"$here/gen_fixtures.c" "$root/engine/h2shared/mathlib.c" -o "$out/gen" -lm
"$out/gen" >"$out/mathlib.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/mathlib.txt" || {
			echo "FAIL: $fixture differs from what engine/h2shared/mathlib.c produces" >&2
			exit 1
		}
		echo "mathlib fixture matches engine/h2shared/mathlib.c" ;;
	"") cp "$out/mathlib.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
