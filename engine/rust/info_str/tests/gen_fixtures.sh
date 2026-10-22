#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/info_str.txt from the C original,
# engine/hexenworld/shared/info_str.c, compiled as hwsv compiles it.  Needs cc;
# the fixture replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
engine="$root/engine"
fixture="$here/fixtures/info_str.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

# The hwsv include order: hexenworld/server and hexenworld/shared shadow
# h2shared, which is where info_str.c's quakedef.h comes from.
cc -O1 -g -Wall -Wno-unused-function -Wno-comment \
	-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common" \
	-DGLQUAKE -DH2W -DSERVERONLY \
	"$here/gen_fixtures.c" "$engine/hexenworld/shared/info_str.c" \
	-o "$out/gen"
"$out/gen" >"$out/info_str.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/info_str.txt" || {
			echo "FAIL: $fixture differs from what info_str.c produces" >&2
			exit 1
		}
		echo "info_str fixture matches hexenworld/shared/info_str.c" ;;
	"") cp "$out/info_str.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
