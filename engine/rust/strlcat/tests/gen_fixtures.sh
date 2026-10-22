#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/strlcat.txt from the C original,
# common/strlcat.c.  Needs cc; the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
fixture="$here/fixtures/strlcat.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cc -O1 -Wall -Wextra -I"$root/common" "$here/gen_fixtures.c" \
	"$root/common/strlcat.c" -o "$out/gen"
"$out/gen" >"$out/strlcat.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/strlcat.txt" || {
			echo "FAIL: $fixture differs from what common/strlcat.c produces" >&2
			exit 1
		}
		echo "strlcat fixture matches common/strlcat.c" ;;
	"") cp "$out/strlcat.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
