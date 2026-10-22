#!/usr/bin/env bash
#
# check-rust-huffman.sh -- gate for the Rust port of hexenworld/shared/huffman.c.
#
# The fixture half replays expected values frozen from the C original --
# hexenworld/shared/huffman.c as the integrated client compiles it (malloc
# tree) -- against the Rust module with no C compiler: the frequency table bit
# for bit, the 256 codes, encodes (the raw 0xff fallback and compressed
# packets), round-trip decodes at several maxlen values, the padding, truncation
# and header-byte sweeps, and a seeded fuzz of malformed packets.  The audit
# half, tests/gen_fixtures.sh --check (step 1b, needs cc), builds the C both
# ways -- as hwsv (Hunk_AllocName) and as the integrated client (malloc) -- and
# fails if the two differ in a byte or from the committed fixture, which is
# what makes the Rust's one allocation path (malloc for both targets) safe.
#
# ACCEPTED LOSS: the retired harness also compared the bytes HuffEncode leaves
# past outlen in the output buffer (the raw fallback leaves scratch bits of the
# failed compressed attempt there).  The fixture does not freeze them: no
# caller reads past outlen, and they are an artefact of the C's two-pass
# encode, not behaviour.  A change that only alters those scratch bytes passes.
# Round trips over 1500 further seeded payloads are checked against the
# encoder's size invariants (outlen <= inlen + 1, from huffman.c).
#
# The decode half is guarded: every replayed decode runs in a forked child with
# the packet placed against an unmapped page, so a decoder that reads past the
# `inlen` bytes the caller declared is observed as a SIGSEGV rather than
# assumed.  The C dies that way on a crafted header byte -- net_udp.c decodes
# whatever arrived on the socket, and the C's GetBit has no bound -- and the
# Rust must not.  Those cases are frozen as `ovr` records holding the C's decode
# with a defined zero tail, and the Rust's answer must be a strict prefix of it.
# That the guard page is a tighter allocation than production is deliberate:
# net_udp.c receives into huffbuff[65536], so the real overread reads the stale
# tail of a live buffer and has no portable answer to compare against.
#
# The replay crate (engine/rust/huffman, test-only) compiles src/huffman.rs
# unchanged and supplies the one engine symbol it reaches, Sys_Error, as an
# aborting stub.  The tree is malloc'd, so no Hunk_* stub is needed.
#
# The CMake half checks the engine build every gate shares
# (scripts/lib/rust-gate.sh).  huffman.c used to be compiled into glhexen2
# through HW_CLIENT_NET_SOURCES and into hwsv through HWSV_SOURCES, and h2ded
# never compiled it at all.  So the build must define each symbol exactly once
# in glhexen2 and hwsv and at most once in h2ded, with no huffman.c objects
# anywhere.
#
# hw_utils is deliberately out of scope: hwterm and hwrcon compile
# hexenworld/shared/huffman.c directly (hw_utils/CMakeLists.txt) and link no
# part of the Rust archive, so they keep the C original either way.
#
# Requires cargo/rustc, cc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(HuffInit HuffEncode HuffDecode)
accessors=(Huffman_freq Huffman_lookup_len Huffman_lookup_bits)

for tool in cargo cc cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. fixture replay: the frozen huffman.c oracle, cargo test --release =="
# The differential harness against the C original was retired (#475).  Its
# expected values are frozen in engine/rust/huffman/tests/fixtures/huffman.txt
# and replayed against src/huffman.rs with no C compiler.  The fixture is
# re-proved against C in step 1b by tests/gen_fixtures.sh --check, and shown
# able to fail by tests/mutants.sh, which rewrites a tracked source and so is
# not run here (docs/rust-harness-retirement.md).  The replay runs --release:
# the engine ships release with panic=abort, so the replay must too.
replay_log="$work/replay.log"
if ! cargo test --release --offline --manifest-path "$crate/huffman/Cargo.toml" >"$replay_log" 2>&1; then
	echo "FAIL: huffman fixture replay failed" >&2
	cat "$replay_log" >&2
	exit 1
fi
# A green exit is not enough: require that both replay tests ran, so a renamed
# or filtered-out test cannot read as green.
for t in replay_c_fixtures code_table_is_a_complete_prefix_code \
         seeded_weighted_payloads_round_trip_and_obey_the_encoder_invariants; do
	grep -q "^test $t \.\.\. ok\$" "$replay_log" || {
		echo "FAIL: $t did not run and pass" >&2
		cat "$replay_log" >&2
		exit 1
	}
done
fixture="$crate/huffman/tests/fixtures/huffman.txt"
records=$(grep -cvE '^(#|$)' "$fixture")
if [ "$records" -lt 2500 ]; then
	echo "FAIL: huffman fixture has $records records, below the 2500 floor" >&2
	exit 1
fi
# Per-category floors live in replay.rs and fail the test itself; this count
# only catches a fixture truncated below anything those floors would allow.
echo "  replay_c_fixtures ok ($records records)"

echo
echo "== 1b. the fixture still matches hexenworld/shared/huffman.c (tests/gen_fixtures.sh --check) =="
# Nothing else re-proves the frozen values against C: the replay only shows
# the Rust agrees with the fixture.  Drift in the C or an edited fixture fails
# the gate here.  Needs cc.
if ! check_log=$(bash "$crate/huffman/tests/gen_fixtures.sh" --check 2>&1); then
	echo "FAIL: huffman fixture no longer matches the C original" >&2
	echo "$check_log" >&2
	exit 1
fi
echo "  $check_log"

echo "== 2. build the consolidated crate with every migrated subsystem =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc,link_ops,msg_io,info_str,huffman

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/client/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}" "${accessors[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: $(( ${#symbols[@]} + ${#accessors[@]} ))/$(( ${#symbols[@]} + ${#accessors[@]} )) symbols once"

# The C original must not be in the build at all: with huffman.c compiled
# and the archive linked, every target that compiles it would define each of
# the three names twice, and a duplicate definition is only ever reported as a
# link error in whichever target the linker happened to look at first.
stray=$(find "$engine_build" -name 'huffman.c.o' | wc -l)
if [ "$stray" -ne 0 ]; then
	echo "FAIL: $stray huffman.c.o object(s) in the engine build" >&2
	find "$engine_build" -name 'huffman.c.o' >&2
	exit 1
fi
echo "  no huffman.c object in the build (the Rust archive is the only definition)"

echo
echo "== 5. exactly one definition where the C original is reachable =="
# glhexen2 compiles huffman.c through HW_CLIENT_NET_SOURCES and hwsv through
# HWSV_SOURCES, and both link the Rust archive, so each must end up with
# exactly one definition of every name.  h2ded never compiles it and has no
# caller, so a definition there is the archive member being pulled in for
# another port -- allowed, but never more than one.
for sym in "${symbols[@]}"; do
	for bin in glhexen2 hwsv; do
		path="$engine_build/bin/$bin"
		[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	n=$(nm "$engine_build/bin/h2ded" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -gt 1 ]; then
		echo "FAIL: h2ded: $sym is defined $n times, expected at most 1" >&2
		exit 1
	fi
done
echo "  glhexen2 and hwsv: ${#symbols[@]}/${#symbols[@]} symbols exactly once"
echo "  h2ded: every symbol at most once (it never compiles huffman.c)"

echo
echo "PASS: Rust huffman -- fixture replay green (the frozen C encodes, decodes and"
echo "      tables match, and the decode path stays inside the caller's buffer"
echo "      where the C's does not), the consolidated archive defines the ABI"
echo "      exactly once wherever huffman.c was reachable, and no huffman.c object"
echo "      is left in the engine build."
