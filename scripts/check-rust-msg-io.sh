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
# The CMake half proves the engine build every gate shares
# (scripts/lib/rust-gate.sh) links it exactly once per binary with no C object
# left in the build, the two exported globals included.  (Cargo can still build
# msg_io without the sizebuf feature, which used to be the USE_SIZEBUF_RS=OFF
# engine build; no engine build selects that any more, so nothing here tests
# it.)
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
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
# Observed today: 533 expectations.  The floor is one decimal place below it.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures$' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 3 ]; then
	echo "FAIL: harness expectation count is below the hundred-expectation floor" >&2
	echo "      observed: ${count_line:-<no 'checked N expectations, M failures' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. exactly one msg_io definition per binary =="
# All 23 symbols are called from somewhere in each target -- the H2W-only five
# from hexenworld/server, the rest from both -- so each must be present exactly
# once.  A second definition is what "the staticlib cannot hold two variants"
# would look like if the union were done wrong.
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
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
stray=$(find "$engine_build" -name 'msg_io.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray msg_io.c.o object(s) in the engine build" >&2
	exit 1
}

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 5. engine smoke: this engine against a C-only reference =="
	reference=$(rust_gate_reference_bin)
	# Reuses the hashindex smoke arm verbatim -- it starts the client, the
	# dedicated server and, where data allows, loads a map, and diffs the two
	# logs.  msg_io sits under every packet that reaches a map load, so no
	# separate smoke script is needed.
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$engine_build/bin/glhexen2" \
	OFF_BIN="$reference" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" msg_io
fi

echo "PASS: Rust msg_io -- the C without H2W, the C with H2W and the Rust union"
echo "      agree byte for byte, the consolidated archive links exactly one"
echo "      definition per symbol in all three targets, and no msg_io.c"
echo "      object is left in the engine build."
