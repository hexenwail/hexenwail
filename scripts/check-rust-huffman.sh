#!/usr/bin/env bash
#
# check-rust-huffman.sh -- gate for the Rust port of hexenworld/shared/huffman.c.
#
# The differential half compiles the C original twice -- once as hwsv compiles
# it (-DH2W -DSERVERONLY, Hunk_AllocName for the tree) and once as the
# integrated client compiles it (-DH2W_INTEGRATED, malloc) -- with its three
# exported names renamed, links both plus the Rust module into one binary, and
# requires all three to agree.  The C-vs-C comparison runs first and on its own:
# the consolidated archive is built once and linked into both targets, so the
# Rust has to pick one allocation path for both, and that is only safe if the
# two C variants are indistinguishable at the byte level.
#
# The decode half is guarded: every case runs in a forked child with the packet
# placed against an unmapped page, so a decoder that reads past the `inlen` bytes
# the caller declared is observed as a SIGSEGV rather than assumed.  The C dies
# that way on a crafted header byte -- net_udp.c decodes whatever arrived on the
# socket, and the C's GetBit has no bound -- and the Rust must not.  That the
# guard page is a tighter allocation than production is deliberate: net_udp.c
# receives into huffbuff[65536], so the real overread reads the stale tail of a
# live buffer and has no portable answer to compare against.  Those cases are
# the ones the port deliberately does not reproduce; see the harness header for
# what is checked instead, and why.
#
# The harness links a huffman-only archive: it is self-contained (hufffreq.h
# and the two engine symbols the code under test reaches) and an all-features
# archive would drag in msg_io, sizebuf and info_str, which reach net_message,
# CON_Printf and Cvar_FindVar.  Step 3 then builds the consolidated archive with
# every feature, which is what the CMake arms link and what the symbol checks
# below are about.
#
# The CMake half proves the flag selects huffman independently: huffman.c is
# compiled into glhexen2 through HW_CLIENT_NET_SOURCES and into hwsv through
# HWSV_SOURCES, and h2ded never compiles it at all.  So the ON build must define
# each symbol in the C original exactly once in glhexen2 and hwsv and at most
# once in h2ded, with no huffman.c objects anywhere, and the OFF build must put
# the C back in glhexen2 and hwsv and nowhere else.
#
# hw_utils is deliberately out of scope: hwterm and hwrcon compile
# hexenworld/shared/huffman.c directly (hw_utils/CMakeLists.txt) and link no
# part of the Rust archive, so they keep the C original either way.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,45p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(HuffInit HuffEncode HuffDecode)
accessors=(Huffman_freq Huffman_lookup_len Huffman_lookup_bits)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with huffman alone, for the differential harness =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features huffman

echo
echo "== 2. differential harness: the C as each target compiles it, and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/huffman/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checked |frequency table|lookup table|fallbacks|bounded decodes|guard-page deaths' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status" >&2
	cat "$diff_log" >&2
	exit 1
fi
grep -q '^RESULT: PASS$' "$diff_log" || {
	echo "FAIL: harness did not print 'RESULT: PASS'" >&2
	cat "$diff_log" >&2
	exit 1
}
# The summary line carries the expectation count, and RESULT alone would still
# pass if the enumeration were cut down to nothing -- so floor the count at five
# digits (ten thousand expectations) rather than pinning it, which would mean
# editing the gate every time the enumeration is deliberately widened.  The
# first number on the line is the expectation count; the second is the failure
# count, which the harness exits non-zero on.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures$' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 5 ]; then
	echo "FAIL: harness expectation count is below the ten-thousand-expectation floor" >&2
	echo "      observed: ${count_line:-<no 'checked N expectations, M failures' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. build the consolidated crate with every migrated subsystem =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc,link_ops,msg_io,info_str,huffman

echo
echo "== 4. build all three targets with USE_HUFFMAN_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_HUFFMAN_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 5. the archive exports the whole ABI exactly once =="
archive="$work/build-on/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}" "${accessors[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: $(( ${#symbols[@]} + ${#accessors[@]} ))/$(( ${#symbols[@]} + ${#accessors[@]} )) symbols once"

# The C original must not be in the ON build at all: with huffman.c compiled
# and the archive linked, every target that compiles it would define each of
# the three names twice, and a duplicate definition is only ever reported as a
# link error in whichever target the linker happened to look at first.
stray=$(find "$work/build-on" -name 'huffman.c.o' | wc -l)
if [ "$stray" -ne 0 ]; then
	echo "FAIL: $stray huffman.c.o object(s) in a USE_HUFFMAN_RS=ON build" >&2
	find "$work/build-on" -name 'huffman.c.o' >&2
	exit 1
fi
echo "  no huffman.c object in the ON build (the Rust archive is the only definition)"

echo
echo "== 6. exactly one definition where the C original is reachable =="
# glhexen2 compiles huffman.c through HW_CLIENT_NET_SOURCES and hwsv through
# HWSV_SOURCES, and both link the Rust archive, so each must end up with
# exactly one definition of every name.  h2ded never compiles it and has no
# caller, so a definition there is the archive member being pulled in for
# another port -- allowed, but never more than one.
for sym in "${symbols[@]}"; do
	for bin in glhexen2 hwsv; do
		path="$work/build-on/bin/$bin"
		[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	n=$(nm "$work/build-on/bin/h2ded" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -gt 1 ]; then
		echo "FAIL: h2ded: $sym is defined $n times, expected at most 1" >&2
		exit 1
	fi
done
echo "  glhexen2 and hwsv: ${#symbols[@]}/${#symbols[@]} symbols exactly once"
echo "  h2ded: every symbol at most once (it never compiles huffman.c)"

echo
echo "== 7. flag OFF still compiles huffman.c where it is used (rollback) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_HUFFMAN_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 hwsv; do
	found=$(find "$work/build-off" -name 'huffman.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin compiled huffman.c $found times, expected 1" >&2
		exit 1
	fi
	for sym in "${symbols[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: OFF $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: the C huffman.c is back, and defines every symbol exactly once"
done
for sym in "${symbols[@]}"; do
	n=$(nm "$work/build-off/bin/h2ded" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 0 ]; then
		echo "FAIL: OFF h2ded: $sym is defined $n times, expected 0 -- h2ded" >&2
		echo "      never compiles huffman.c and the Rust huffman feature is off" >&2
		exit 1
	fi
done
found=$(find "$work/build-off" -name 'huffman.c.o' -path "*h2ded.dir*" | wc -l)
if [ "$found" -ne 0 ]; then
	echo "FAIL: OFF build of h2ded compiled huffman.c ($found objects): it is not" >&2
	echo "      in any list h2ded uses" >&2
	exit 1
fi
echo "  h2ded: neither the C original nor a Rust definition, as before"

echo
echo "PASS: Rust huffman -- the C as hwsv and as the integrated client compile it"
echo "      agree with each other byte for byte before either is compared with the"
echo "      Rust module, the decode path stays inside the caller's buffer where the"
echo "      C's does not, the consolidated archive defines the ABI exactly once"
echo "      wherever huffman.c is reachable, and the C fallback still compiles when"
echo "      USE_HUFFMAN_RS=OFF."