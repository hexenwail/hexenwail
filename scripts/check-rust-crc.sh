#!/usr/bin/env bash
#
# check-rust-crc.sh -- gate for the Rust port of common/crc.c.
#
# The differential half proves the CCITT CRC is bit-identical to the C
# original: every (running value, byte) step exhaustively, CRC_Value over all
# 2^16 inputs, a published check value, and seeded random blocks at unaligned
# offsets.  The CMake half proves the consolidated engine archive selects crc
# independently and that the C fallback remains buildable.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,13p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(CRC_Init CRC_ProcessByte CRC_ProcessBlock CRC_Value CRC_Block)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/crc/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|differential harness:' "$diff_log" || true
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

echo
echo "== 3. build all three targets with USE_CRC_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_CRC_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one crc definition per binary =="
# The archive must export the whole C ABI.  In a linked binary, CRC_Value has
# no engine caller, so whether it is present depends on how rustc partitioned
# codegen units; require it at most once there, and every called symbol
# exactly once.
archive="$work/build-on/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} symbols exported exactly once"
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$sym" = CRC_Value ]; then
			[ "$n" -le 1 ] && continue
		elif [ "$n" -eq 1 ]; then
			continue
		fi
		echo "FAIL: $bin: $sym has $n definitions" >&2
		exit 1
	done
	echo "  $bin: called crc symbols exactly once, CRC_Value at most once"
done
stray=$(find "$work/build-on" -name 'crc.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray crc.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 5. flag OFF still compiles crc.c (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_CRC_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'crc.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile crc.c ($found objects)" >&2
		exit 1
	fi
	for sym in "${symbols[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: OFF $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
done

echo "PASS: Rust crc -- differential harness green, the consolidated archive"
echo "      links exactly one symbol per target, and the C fallback still"
echo "      compiles when USE_CRC_RS=OFF."
