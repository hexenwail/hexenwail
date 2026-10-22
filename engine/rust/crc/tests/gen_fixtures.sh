#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/crc.txt from the C original,
# common/crc.c.  Needs cc; the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
fixture="$here/fixtures/crc.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cc -O1 -Wall -DGLQUAKE -I"$root/engine/hexen2" -I"$root/engine/h2shared" \
	-I"$root/common" "$here/gen_fixtures.c" "$root/common/crc.c" -o "$out/gen"
"$out/gen" >"$out/crc.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/crc.txt" || {
			echo "FAIL: $fixture differs from what common/crc.c produces" >&2
			exit 1
		}
		echo "crc fixture matches common/crc.c" ;;
	"") cp "$out/crc.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
