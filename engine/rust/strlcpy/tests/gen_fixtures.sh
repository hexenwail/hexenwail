#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/strlcpy.txt from the C original,
# common/strlcpy.c.  Needs cc; the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
fixture="$here/fixtures/strlcpy.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cc -O1 -Wall -Wextra -I"$root/common" "$here/gen_fixtures.c" \
	"$root/common/strlcpy.c" -o "$out/gen"
"$out/gen" >"$out/strlcpy.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/strlcpy.txt" || {
			echo "FAIL: $fixture differs from what common/strlcpy.c produces" >&2
			exit 1
		}
		echo "strlcpy fixture matches common/strlcpy.c" ;;
	"") cp "$out/strlcpy.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
