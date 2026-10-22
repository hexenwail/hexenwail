#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate (or --check) tests/fixtures/huffman.txt from the C original,
# engine/hexenworld/shared/huffman.c, built both ways the engine builds it
# (hwsv: Hunk_AllocName; integrated client: malloc).  Needs cc; the fixture
# replay itself does not.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
engine="$root/engine"
fixture="$here/fixtures/huffman.txt"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cflags=(-O1 -g -Wall -Wno-unused-function)

# hwsv: the HexenWorld include order, tree from Hunk_AllocName (stubbed in the
# generator), the three exported names renamed so both variants link together.
cc "${cflags[@]}" -DGLQUAKE -DH2W -DSERVERONLY \
	-DHuffInit=w_HuffInit -DHuffEncode=w_HuffEncode -DHuffDecode=w_HuffDecode \
	-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common" \
	-c "$engine/hexenworld/shared/huffman.c" -o "$out/huffman_hwsv.o"

# integrated client: the Hexen II include order; huffman.c is #included by the
# generator so its statics (HuffLookup, HuffFreq) can be dumped.
cc "${cflags[@]}" -DGLQUAKE -DH2W_INTEGRATED \
	-I"$engine/hexen2" -I"$engine/h2shared" -I"$engine/hexenworld/shared" \
	-I"$root/common" \
	"$here/gen_fixtures.c" "$out/huffman_hwsv.o" -o "$out/gen"

"$out/gen" >"$out/huffman.txt"

case "${1:-}" in
	--check)
		diff -u "$fixture" "$out/huffman.txt" || {
			echo "FAIL: $fixture differs from what huffman.c produces" >&2
			exit 1
		}
		echo "huffman fixture matches huffman.c" ;;
	"") cp "$out/huffman.txt" "$fixture"; wc -lc "$fixture" ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
