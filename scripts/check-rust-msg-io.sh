#!/usr/bin/env bash
#
# check-rust-msg-io.sh -- gate for the Rust port of h2shared/msg_io.c.
#
# The differential half is a three-way comparison, because msg_io.c is the one
# engine source compiled with different code per target: the harness builds the
# C original twice, once as glhexen2/h2ded compile it (no H2W) and once as hwsv
# does (-DH2W), and links both against the single Rust union.  That is the
# evidence for the claim that the Rust module can define both variants at once,
# and it covers golden packets per primitive, signed/unsigned boundaries, NaN
# and Inf bit patterns, truncation, bad-read state, unterminated strings, the
# angle and coordinate quantization edges, the usercmd_s layout for the short
# and long message variants, and round trips through the opposite
# implementation.
#
# The CMake half proves the consolidated engine archive selects msg_io
# independently -- including the msg_io-on/sizebuf-off combination, where the
# Rust sizebuf entry points must stay out of the way so the C sizebuf.c remains
# the only definition -- and that the C fallback remains buildable.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
		-h|--help) sed -n '2,22p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# Every symbol msg_io.c defines, including the five that exist only under
# -DH2W.  Each must appear exactly once in every binary.
symbols=(
	MSG_WriteChar MSG_WriteByte MSG_WriteShort MSG_WriteLong MSG_WriteFloat
	MSG_WriteString MSG_WriteCoord MSG_WriteAngle MSG_WriteAngle16
	MSG_WriteUsercmd MSG_BeginReading MSG_BeginReadingFrom
	MSG_ReadChar MSG_ReadByte MSG_ReadShort MSG_ReadLong MSG_ReadFloat
	MSG_ReadString MSG_ReadStringLine MSG_ReadCoord MSG_ReadAngle
	MSG_ReadAngle16 MSG_ReadUsercmd
)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc,link_ops,msg_io

echo
echo "== 2. differential harness: the C without H2W, the C with H2W, and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/msg_io/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|differential harness:|^NOTE:' "$diff_log" || true
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
# pass if the enumeration were cut down to nothing -- so floor the count at three
# digits (a hundred expectations) rather than pinning it, which would mean
# editing the gate every time the enumeration is deliberately widened.  The
# first number on the line is the expectation count; the second is the failure
# count, which the harness exits non-zero on.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures$' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 3 ]; then
	echo "FAIL: harness expectation count is below the hundred-expectation floor" >&2
	echo "      observed: ${count_line:-<no 'checked N expectations, M failures' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. build all three targets with USE_MSG_IO_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_MSG_IO_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one msg_io definition per binary =="
# All 23 symbols are called from somewhere in each target -- the H2W-only five
# from hexenworld/server, the rest from both -- so each must be present exactly
# once.  A second definition is what "the staticlib cannot hold two variants"
# would look like if the union were done wrong.
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	# msg_readcount and msg_badread are data the C side also writes.
	for sym in msg_readcount msg_badread; do
		n=$(nm "$path" | grep -cE " [BbDd] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: ${#symbols[@]}/${#symbols[@]} symbols and 2/2 globals exactly once"
done
stray=$(find "$work/build-on" -name 'msg_io.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray msg_io.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 5. msg_io stays switchable independently of sizebuf =="
# The consolidated crate lands in a single codegen unit, so a build with
# USE_MSG_IO_RS=ON and USE_SIZEBUF_RS=OFF is the combination where a sizebuf
# symbol leaking out of the Rust archive would collide with the C sizebuf.c.
cmake -B "$work/build-mixed" -S "$root/engine" \
	-DUSE_MSG_IO_RS=ON -DUSE_SIZEBUF_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-mixed" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-mixed/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: mixed $path was not built" >&2; exit 1; }
	for sym in SZ_GetSpace SZ_Write SZ_Init; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: mixed $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	n=$(nm "$path" | grep -cE " [Tt] MSG_WriteByte\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: mixed $bin: MSG_WriteByte has $n definitions" >&2
		exit 1
	fi
	found=$(find "$work/build-mixed" -name 'sizebuf.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: mixed $bin did not compile sizebuf.c ($found objects)" >&2
		exit 1
	fi
done
echo "  msg_io from Rust with sizebuf from C: all three targets link"

echo
echo "== 6. flag OFF still compiles msg_io.c (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_MSG_IO_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'msg_io.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile msg_io.c ($found objects)" >&2
		exit 1
	fi
	for sym in MSG_WriteByte MSG_ReadShort msg_readcount msg_badread; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [TtBbDd] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: OFF $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
done
# The H2W-only five are only defined by the C original in the HexenWorld
# target, which is the asymmetry the union exists to paper over.
for sym in MSG_WriteAngle16 MSG_WriteUsercmd MSG_ReadStringLine MSG_ReadAngle16 MSG_ReadUsercmd; do
	for bin in glhexen2 h2ded; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 0 ]; then
			echo "FAIL: OFF $bin: $sym is defined $n times, but the H2 build has no such function" >&2
			exit 1
		fi
	done
	n=$(nm "$work/build-off/bin/hwsv" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: OFF hwsv: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  C msg_io.c is back in all three targets, H2W-only five only in hwsv"

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 7. engine smoke: run the real engine both ways =="
	# Reuses the hashindex smoke arm verbatim -- it starts the client, the
	# dedicated server and, where data allows, loads a map, and diffs the two
	# logs.  msg_io sits under every packet that reaches a map load, so no
	# separate smoke script is needed.
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$work/build-on/bin/glhexen2" \
	OFF_BIN="$work/build-off/bin/glhexen2" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" msg_io
fi

echo "PASS: Rust msg_io -- the C without H2W, the C with H2W and the Rust union"
echo "      agree byte for byte, the consolidated archive links exactly one"
echo "      definition per symbol in all three targets, and the C fallback"
echo "      still compiles when USE_MSG_IO_RS=OFF."
