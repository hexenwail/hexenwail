#!/usr/bin/env bash
#
# check-rust-info-str.sh -- gate for the Rust port of hexenworld/shared/info_str.c.
#
# The differential half compiles the C original exactly as hwsv does (-DH2W
# -DSERVERONLY, hexenworld/shared ahead of h2shared) with its six exported names
# renamed, links it against the Rust module in one binary, and compares the two
# across successive calls: empty and malformed info strings, missing, duplicate
# and valueless keys, the 63/64 and >= maxsize rejections, the ascii and
# high-bit filtering including the `c > 13` retention, both sv_highchars states
# and the pre-registration one, the four-buffer rotation by pointer identity, and
# the in-place rewrites of Info_RemoveKey and Info_RemovePrefixedKeys.
#
# The CMake half proves the consolidated engine archive selects info_str
# independently, and covers the shape this port introduced: info_str.c is
# compiled into HWSV_SOURCES alone, so the C symbols exist in hwsv and nowhere
# else, while the Rust archive is linked into all three targets.  hwsv must
# therefore define each of the six exactly once, and glhexen2 and h2ded at most
# once -- whether the unreferenced member is pulled in depends on the linker.
#
# There is deliberately no --engine smoke arm.  The hwsv smoke CI runs dies at
# FS_Init's data gate before any client connects, so no Info_ call is reached;
# an arm that cannot reach the code under test would only look like evidence.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,26p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(
	Info_ValueForKey Info_RemoveKey Info_RemovePrefixedKeys
	Info_SetValueForKey Info_SetValueForStarKey Info_Print
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
	--features hashindex,mathlib,sizebuf,crc,link_ops,msg_io,info_str

echo
echo "== 2. differential harness: the C as hwsv compiles it, and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/info_str/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checked |differential harness:' "$diff_log" || true
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
# Observed today: 231 expectations.  The floor is one decimal place below it.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures$' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 3 ]; then
	echo "FAIL: harness expectation count is below the hundred-expectation floor" >&2
	echo "      observed: ${count_line:-<no 'checked N expectations, M failures' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. build all three targets with USE_INFO_STR_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_INFO_STR_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. the archive exports the whole ABI exactly once =="
archive="$work/build-on/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
# The cvar_t layout accessors the differential harness checks against the real
# struct.
for sym in CvarC_sizeof CvarC_offsetof_integer; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} symbols and 2/2 accessors once"

# This is the invariant that made the port read sv_highchars through
# Cvar_FindVar instead of the global.  The consolidated archive is one object
# member, so any target pulling it for any other port inherits every undefined
# symbol in it -- and only hwsv defines sv_highchars.  A direct reference here
# is a link error in glhexen2 and h2ded, so it must never come back.
stray_hwsv_only=$(nm -u "$archive" 2>/dev/null | grep -cE " U sv_highchars\$" || true)
if [ "$stray_hwsv_only" -ne 0 ]; then
	echo "FAIL: libengine_rs.a references the hwsv-only global sv_highchars;" >&2
	echo "      glhexen2 and h2ded do not define it and will not link." >&2
	exit 1
fi
echo "  libengine_rs.a: no reference to the hwsv-only sv_highchars global"

echo
echo "== 5. exactly one info_str definition where it is reachable =="
# All six are called from hexenworld/server, so hwsv must define each exactly
# once.  The Hexen II targets contain no caller: a definition there is the
# archive member being pulled in for another port, which is allowed but must
# never be more than one.
for sym in "${symbols[@]}"; do
	n=$(nm "$work/build-on/bin/hwsv" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: hwsv: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  hwsv: ${#symbols[@]}/${#symbols[@]} symbols exactly once"
for bin in glhexen2 h2ded; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -gt 1 ]; then
			echo "FAIL: $bin: $sym is defined $n times, expected at most 1" >&2
			exit 1
		fi
	done
	echo "  $bin: every info_str symbol at most once (no caller in this target)"
done
stray=$(find "$work/build-on" -name 'info_str.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray info_str.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 6. flag OFF still compiles info_str.c where it is used (rollback) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_INFO_STR_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded; do
	found=$(find "$work/build-off" -name 'info_str.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 0 ]; then
		echo "FAIL: OFF build of $bin compiled info_str.c ($found objects): the C" >&2
		echo "      original is only in HWSV_SOURCES" >&2
		exit 1
	fi
	for sym in "${symbols[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 0 ]; then
			echo "FAIL: OFF $bin: $sym is defined $n times, expected 0" >&2
			exit 1
		fi
	done
done
found=$(find "$work/build-off" -name 'info_str.c.o' -path "*hwsv.dir*" | wc -l)
if [ "$found" -ne 1 ]; then
	echo "FAIL: OFF build of hwsv did not compile info_str.c ($found objects)" >&2
	exit 1
fi
for sym in "${symbols[@]}"; do
	n=$(nm "$work/build-off/bin/hwsv" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: OFF hwsv: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  the C info_str.c is back in hwsv, and was never in glhexen2 or h2ded"

echo
echo "PASS: Rust info_str -- the C as hwsv compiles it and the Rust module agree"
echo "      across successive calls, the consolidated archive links exactly one"
echo "      definition where the code is reachable, and the C fallback still"
echo "      compiles when USE_INFO_STR_RS=OFF."