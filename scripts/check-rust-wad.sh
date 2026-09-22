#!/usr/bin/env bash
#
# check-rust-wad.sh -- gate for the Rust port of engine/h2shared/wad.c.
#
# The differential half compiles the C original exactly as glhexen2 does
# (-DGLQUAKE, the Hexen II include order) with its four exported functions and
# all three exported globals renamed, links it against the Rust module in one
# binary, and compares the two across a wad built inside the harness: lump
# counts of zero and one, mixed case and 16-byte padding, the WAD2
# identification check and the couldn't-load path, the endian fields, the
# in-place name cleaning, lookups of present and absent lumps, and SwapPic.
# The whole mapping is compared after every call, because the C edits it in
# place -- names and picture headers -- rather than a copy.
#
# The CMake half proves the consolidated engine archive selects wad
# independently, and covers the shape this port introduced: wad.c is compiled
# into COMMON_SOURCES and removed again for h2ded ("gfx.wad lumps are
# renderer-only data"), and HWSV_SOURCES never lists it.  glhexen2 is therefore
# the only target that contains a definition, and the other two may only ever
# have the unreferenced archive member pulled in for another port.
#
# --engine is opt-in.  host.c and gl_vidsdl.c call W_LoadWadFile("gfx.wad") at
# startup and the client and menu draw every character, icon and backtile from
# those lumps, so an ON/OFF console diff reaches the code under test -- unlike
# info_str, whose only smoke arm dies before any Info_ call.  It resolves the
# demo data and Xvfb out of nix itself, so it needs network (or a populated
# store).
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,31p' "$0"; exit 0 ;;
		--engine) run_engine=1 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# The four exported functions, and separately the three exported globals: nm
# reports those as .bss/data symbols rather than text.
symbols=(W_LoadWadFile W_GetLumpinfo W_GetLumpName SwapPic)
globals=(wad_numlumps wad_lumps wad_base)
accessors=(LumpInfoC_sizeof LumpInfoC_offsetof_name WadInfoC_sizeof \
	QPicC_sizeof Wad_LittleLong)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with wad alone, for the differential harness =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features wad

echo
echo "== 2. differential harness: the C as glhexen2 compiles it, and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/wad/tests/run_diff_harness.sh" >"$diff_log" 2>&1
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
# Observed today: 462 expectations.  The floor is one decimal place below it.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures$' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 3 ]; then
	echo "FAIL: harness expectation count is below the hundred-expectation floor" >&2
	echo "      observed: ${count_line:-<no 'checked N expectations, M failures' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. build all three targets with USE_WAD_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_WAD_RS=ON \
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
# wad_numlumps is an `int`, wad_lumps a `lumpinfo_t *` and wad_base a `byte *`;
# a wrong type does not change the symbol name, which is why the differential
# harness reaches all three through the C header's own types.
for sym in "${globals[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " [BbDd] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
# The struct layout accessors and the endian helper the harness checks against
# the real wad.h structs and the C's LittleLong macro.
for sym in "${accessors[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} functions, ${#globals[@]}/${#globals[@]} globals and ${#accessors[@]}/${#accessors[@]} accessors once"

echo
echo "== 5. exactly one wad definition where it is reachable =="
# glhexen2 is the only target with a caller -- the renderer, the menu and the
# status bar -- so it must define every function, and all three globals.
for sym in "${symbols[@]}"; do
	n=$(nm "$work/build-on/bin/glhexen2" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: glhexen2: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
for sym in "${globals[@]}"; do
	n=$(nm "$work/build-on/bin/glhexen2" | grep -cE " [BbDd] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: glhexen2: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  glhexen2: ${#symbols[@]}/${#symbols[@]} functions and ${#globals[@]}/${#globals[@]} globals exactly once"
# h2ded removes wad.c from its source list and hwsv never had it, so neither has
# a caller: a definition there is the unreferenced archive member being pulled
# in for another port, which is allowed but must never be more than one.
for bin in h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -gt 1 ]; then
			echo "FAIL: $bin: $sym is defined $n times, expected at most 1" >&2
			exit 1
		fi
	done
	for sym in "${globals[@]}"; do
		n=$(nm "$path" | grep -cE " [BbDd] $sym\$" || true)
		if [ "$n" -gt 1 ]; then
			echo "FAIL: $bin: $sym is defined $n times, expected at most 1" >&2
			exit 1
		fi
	done
	echo "  $bin: every wad symbol at most once (no caller in this target)"
done
stray=$(find "$work/build-on" -name 'wad.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray wad.c.o object(s) in a USE_WAD_RS=ON build" >&2
	exit 1
}

echo
echo "== 6. flag OFF still compiles the C original in its one target (rollback) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_WAD_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
found=$(find "$work/build-off" -name 'wad.c.o' -path "*glhexen2.dir*" | wc -l)
if [ "$found" -ne 1 ]; then
	echo "FAIL: OFF build of glhexen2 did not compile wad.c ($found objects)" >&2
	exit 1
fi
for sym in "${symbols[@]}"; do
	n=$(nm "$work/build-off/bin/glhexen2" | grep -cE " [Tt] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: OFF glhexen2: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
for sym in "${globals[@]}"; do
	n=$(nm "$work/build-off/bin/glhexen2" | grep -cE " [BbDd] $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: OFF glhexen2: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
# The two targets that remove wad.c from their source list must not grow one.
for bin in h2ded hwsv; do
	for sym in "${symbols[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 0 ]; then
			echo "FAIL: OFF $bin: $sym is defined $n times, expected 0" >&2
			exit 1
		fi
	done
	for sym in "${globals[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [BbDd] $sym\$" || true)
		if [ "$n" -ne 0 ]; then
			echo "FAIL: OFF $bin: $sym is defined $n times, expected 0" >&2
			exit 1
		fi
	done
done
echo "  the C wad.c is back in glhexen2, and never in h2ded or hwsv"

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 7. engine smoke: run the real engine both ways =="
	# Reuses the hashindex smoke arm verbatim -- it starts the client, the
	# dedicated server and, where data allows, loads a map, and diffs the two
	# logs.  W_LoadWadFile runs during client startup (host.c:1555,
	# gl_vidsdl.c:2460) and every character, icon and backtile is drawn out of
	# those lumps, so the client half of this diff goes through the port.
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$work/build-on/bin/glhexen2" \
	OFF_BIN="$work/build-off/bin/glhexen2" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" wad
fi

echo
echo "PASS: Rust wad -- the C as glhexen2 compiles it and the Rust module agree"
echo "      byte for byte across the mapping they both edit in place, the"
echo "      consolidated archive links exactly one definition in the one target"
echo "      that has a caller, and the C fallback still compiles when"
echo "      USE_WAD_RS=OFF."
