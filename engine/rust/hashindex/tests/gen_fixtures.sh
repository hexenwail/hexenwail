#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/hashindex.txt from the C original,
# engine/h2shared/hashindex.c.  Needs cc; the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
fixture="$here/fixtures/hashindex.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

# The same include paths the engine's own CMake build uses for hashindex.c.
cc -O1 -g -Wall -Wno-unused-function -DGLQUAKE \
	-I"$root/engine/hexen2" -I"$root/engine/h2shared" -I"$root/common" \
	"$here/gen_fixtures.c" "$root/engine/h2shared/hashindex.c" -o "$out/gen"
"$out/gen" >"$out/hashindex.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/hashindex.txt" || {
			echo "FAIL: $fixture differs from what engine/h2shared/hashindex.c produces" >&2
			exit 1
		}
		echo "hashindex fixture matches engine/h2shared/hashindex.c" ;;
	"") cp "$out/hashindex.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
