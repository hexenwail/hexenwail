#!/usr/bin/env bash
#
# check-rust-strlcat.sh -- gate for the Rust port of common/strlcat.c.
#
# The differential half compares q_strlcat's return value and every byte it
# writes with the C original over destination sizes 0..288 in four initial
# contents (empty, terminator at the last usable byte, no terminator within
# `siz`, and a case-derived partial string), source lengths 0..256, and
# unaligned source/destination offsets.  The CMake half proves the flag removes
# strlcat.c from the Hexen II and HexenWorld source lists independently and that
# the C fallback remains buildable.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: ISC

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
symbol=q_strlcat

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc,link_ops,strlcpy,strlcat

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/strlcat/tests/run_diff_harness.sh" >"$diff_log" 2>&1
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
# The PASS line carries the case count, and RESULT alone would still pass if the
# enumeration were cut down to nothing -- so floor the count at seven digits
# (a million cases) rather than pinning it, which would mean editing the gate
# every time the enumeration is deliberately widened.
count_line=$(grep -E '^strlcat differential harness: PASS \(' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 7 ]; then
	echo "FAIL: harness case count is below the one-million floor" >&2
	echo "      observed: ${count_line:-<no 'strlcat differential harness: PASS (N cases)' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. build all three targets with USE_STRLCAT_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_STRLCAT_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one q_strlcat definition per binary =="
archive="$work/build-on/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
n=$(nm "$archive" 2>/dev/null | grep -cE " T $symbol\$" || true)
if [ "$n" -ne 1 ]; then
	echo "FAIL: libengine_rs.a: $symbol has $n definitions, expected exactly 1" >&2
	exit 1
fi
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	n=$(nm "$path" | grep -cE " [Tt] $symbol\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: $bin: $symbol has $n definitions, expected exactly 1" >&2
		exit 1
	fi
	echo "  $bin: $symbol exactly once"
done
stray=$(find "$work/build-on" -name 'strlcat.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray strlcat.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 5. flag OFF still compiles strlcat.c (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_STRLCAT_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'strlcat.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile strlcat.c ($found objects)" >&2
		exit 1
	fi
	n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $symbol\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: OFF $bin: $symbol has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done

echo "PASS: Rust strlcat -- differential harness green, the consolidated"
echo "      archive links exactly one definition per target, and the C fallback"
echo "      still compiles when USE_STRLCAT_RS=OFF."