#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/hexenworld/shared/huffman.c TWICE with all three exported names
# renamed, and compare both against the Rust huffman module in one binary.
#
#   w_  as hwsv compiles it -- -DH2W -DSERVERONLY, the HexenWorld include
#       order (hexenworld/server and hexenworld/shared ahead of h2shared).
#       huffman.c's USE_HUNKMEM is 1 there, so the tree comes from
#       Hunk_AllocName, which diff_harness.c stubs.
#
#   c_  as the integrated Hexenwail client compiles it -- -DH2W_INTEGRATED,
#       the Hexen II include order.  USE_HUNKMEM is 0 there and the tree is
#       malloc'd.  That is the allocation the Rust module uses for BOTH
#       targets, because the consolidated archive is built once and linked
#       into both; the C-vs-C comparison is what shows the choice is safe.
#
# The harness is deliberately self-contained: it includes hufffreq.h and no
# engine header, and supplies only the two engine symbols the code under test
# reaches (Sys_Error, Hunk_AllocName).  The Rust staticlib it links against
# must therefore have been built with the huffman FEATURE ALONE -- see
# scripts/check-rust-huffman.sh, which builds that archive for this arm and
# proves the consolidated all-features archive links in the CMake arms
# instead.  Linking an all-features archive here would drag in msg_io,
# sizebuf and info_str, which reach net_message, CON_Printf and Cvar_FindVar
# -- engine types this harness would then have to restate.
#
# The Rust staticlib must have been built with the huffman feature.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: consolidated Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features huffman" >&2
	exit 2
fi

# The hwsv include order: hexenworld/shared must shadow h2shared, which is
# where huffman.c's q_stdinc.h/sys.h/zone.h come from.
INCS_H2W=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common")
# The Hexen II include order, as glhexen2 compiles the file.
INCS_H2=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol huffman.c exports, renamed so the same file can be compiled
# twice and linked against the Rust module without colliding.
RENAME=(
	-DHuffInit=__PREFIX__HuffInit
	-DHuffEncode=__PREFIX__HuffEncode
	-DHuffDecode=__PREFIX__HuffDecode
)
rename_for() {
	local p="$1" i
	RENAME_ARGS=()
	for i in "${RENAME[@]}"; do
		RENAME_ARGS+=("${i/__PREFIX__/$p}")
	done
}

echo "== compiling the C original as hwsv does (-DH2W -DSERVERONLY, Hunk) =="
rename_for w_
cc "${INCS_H2W[@]}" "${CFLAGS[@]}" -DGLQUAKE -DH2W -DSERVERONLY \
	"${RENAME_ARGS[@]}" -c "$engine/hexenworld/shared/huffman.c" \
	-o "$OUT/huffman_hwsv.o"

echo "== compiling the C original as the integrated client does (H2W_INTEGRATED, malloc) =="
rename_for c_
cc "${INCS_H2[@]}" "${CFLAGS[@]}" -DGLQUAKE -DH2W_INTEGRATED \
	"${RENAME_ARGS[@]}" -c "$engine/hexenworld/shared/huffman.c" \
	-o "$OUT/huffman_client.o"

echo "== compiling the differential harness =="
# hufffreq.h is the only engine header it needs: the harness rebuilds the C's
# frequency table from it so the 256 values can be compared bit for bit.
cc -I"$engine/hexenworld/shared" "${CFLAGS[@]}" \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking all three implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/huffman_hwsv.o" \
	"$OUT/huffman_client.o" "$RUST_LIB" -lm

echo "== running =="
# The timeout is load-bearing.  The harness decodes every case in a forked
# child; a decoder that neither returns nor dies -- a Huffman walk that never
# reaches a leaf, which a wrong tree or a wrong bit read produces -- leaves the
# child spinning and the parent waiting forever.  A gate that hangs is worse
# than one that fails, so a non-finishing run is reported as the failure it is.
harness_timeout="${HARNESS_TIMEOUT:-300}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	echo "      That is what a decoder that never reaches a leaf looks like:" >&2
	echo "      the tree walk inside HuffDecode has no bound of its own, so a" >&2
	echo "      wrong tree or bit read spins instead of returning." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"