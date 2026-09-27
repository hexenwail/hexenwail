#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-zone.sh -- gate for the Rust port of engine/h2shared/zone.c.
#
# The differential half builds the C original with every exported name renamed,
# links it beside the Rust module, and compares the two byte for byte after
# every call -- and it does that twice, because zone.c has two variants.  In the
# client arm (-DGLQUAKE) the main zone defaults to 2 MB, a 256 KB secondary zone
# exists and the cache API is compiled in; in the SERVERONLY arm the default is
# 1 MB and both of those are gone.  The consolidated Rust archive is one object
# linked into all three engine targets, so the difference is carried by
# engine/rust/zone_target.c -- the per-target shim decided on #233 -- compiled
# here with each arm's defines, exactly as the engine compiles it.  Two arms,
# one implementation, each matching the C it was built against: that is the
# claim this gate exists to keep true.
#
# What is compared is *where things landed*, not what was returned.  zone.c
# keeps its whole structure inside the caller's buffer, so the harness gives
# both implementations the same static buffer (same address in both child
# processes, so the absolute pointers compare equal), and records the CRC of the
# buffer, the two ends verbatim, the hunk marks, the cache_user_t values, the
# commands registered and every diagnostic.  Zone name pointers -- the C's
# static "MAINZONE" and the Rust module's own -- fall outside the buffer and are
# normalised to zero; the name *contents* are still compared, because every hunk
# header carries a copy of its allocation name.
#
# The benchmark is required by #61's quality gates (the first hot-path port) and
# by #288: the same operation mix is timed for both implementations and the
# budget below is checked rather than asserted away.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"

# The exported surface, and the struct-layout accessors the harness and
# engine/rust/tests/abi_layout.c call.
symbols=(
	Memory_Init Z_Free Z_Malloc Z_Realloc Z_Strdup
	Hunk_Alloc Hunk_AllocName Hunk_HighAllocName Hunk_Strdup Hunk_TempAlloc
	Hunk_LowMark Hunk_FreeToLowMark Hunk_HighMark Hunk_FreeToHighMark Hunk_Check
	Cache_Flush Cache_Check Cache_Free Cache_Alloc Cache_Report
)
target_fns=(Zone_TargetDefSize Zone_TargetSecSize Zone_TargetHasCache Zone_TargetDedicated)
accessors=(
	ZonelistC_sizeof ZonelistC_offsetof_id ZonelistC_offsetof_magic
	ZonelistC_offsetof_name ZonelistC_offsetof_zone ZonelistC_offsetof_next
	MemBlockC_sizeof MemBlockC_alignof MemBlockC_offsetof_size
	MemBlockC_offsetof_tag MemBlockC_offsetof_magic MemBlockC_offsetof_pad
	MemBlockC_offsetof_next MemBlockC_offsetof_prev
	MemZoneC_sizeof MemZoneC_offsetof_size MemZoneC_offsetof_blocklist
	MemZoneC_offsetof_rover
	HunkC_sizeof HunkC_offsetof_sentinal HunkC_offsetof_size HunkC_offsetof_name
	CacheUserC_sizeof CacheUserC_offsetof_data
	CacheSystemC_sizeof CacheSystemC_alignof CacheSystemC_offsetof_size
	CacheSystemC_offsetof_user CacheSystemC_offsetof_name
	CacheSystemC_offsetof_prev CacheSystemC_offsetof_next
	CacheSystemC_offsetof_lru_prev CacheSystemC_offsetof_lru_next
	QuakeParmsC_sizeof QuakeParmsC_offsetof_basedir QuakeParmsC_offsetof_userdir
	QuakeParmsC_offsetof_argc QuakeParmsC_offsetof_argv QuakeParmsC_offsetof_membase
	QuakeParmsC_offsetof_memsize QuakeParmsC_offsetof_errstate
)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

# The recorded budget.  The Rust module is held to this multiple of the C
# original's time on the same mix; a port that is an order of magnitude slower
# on the allocator would be a regression the engine cannot absorb, and one that
# is a little slower is acceptable.  See the measured numbers in the output.
BENCH_BUDGET_RATIO="2.0"

echo "== 1. build the crate with zone alone, for the differential harness =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features zone

echo
echo "== 2. differential harness: both variants, the C original and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/zone/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checked |differential harness|benchmark:|^  (C|Rust) +:|ratio Rust/C' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status" >&2
	tail -40 "$diff_log" >&2
	exit 1
fi

# Each arm's summary line carries its case count and the trace bytes compared.
# RESULT alone would still pass if a case list were cut down to nothing, so both
# are floored rather than pinned, one decimal place below what the harness
# produces today.
# Observed today: client 8 cases / 49575 bytes; SERVERONLY 6 cases / 27198 bytes.
# The last summary line belongs to the SERVERONLY arm (it runs second); the one
# before it is the client arm's.
client_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" | head -1 || true)
server_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" | tail -1 || true)
for pair in "client:$client_line:6:30000" "serveronly:$server_line:5:20000"; do
	arm=${pair%%:*}
	rest=${pair#*:}
	line=$(printf '%s' "$rest" | cut -d: -f1)
	mincases=$(printf '%s' "$rest" | cut -d: -f2)
	minbytes=$(printf '%s' "$rest" | cut -d: -f3)
	if [ -z "$line" ]; then
		echo "FAIL: no summary line for the $arm arm" >&2
		cat "$diff_log" >&2
		exit 1
	fi
	cases=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | sed -n 3p)
	bytes=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | sed -n 4p)
	[ "$cases" -ge "$mincases" ] || {
		echo "FAIL: the $arm arm ran $cases cases, below the $mincases floor" >&2
		exit 1
	}
	[ "$bytes" -ge "$minbytes" ] || {
		echo "FAIL: the $arm arm compared $bytes trace bytes, below the $minbytes floor" >&2
		exit 1
	}
	echo "  $arm arm: $cases cases, $bytes trace bytes compared"
done

echo
echo "== 3. benchmark against the C original (budget: Rust <= ${BENCH_BUDGET_RATIO}x C) =="
mean_ratio=$(grep -E '^  ratio Rust/C:' "$diff_log" | awk '{print $3}' | tail -1)
if [ -z "$mean_ratio" ]; then
	echo "FAIL: the harness printed no benchmark ratio" >&2
	tail -20 "$diff_log" >&2
	exit 1
fi
if awk "BEGIN{exit !($mean_ratio > $BENCH_BUDGET_RATIO)}"; then
	echo "FAIL: Rust is ${mean_ratio}x the C original, over the ${BENCH_BUDGET_RATIO}x budget" >&2
	exit 1
fi
echo "  ratio ${mean_ratio}x, within the ${BENCH_BUDGET_RATIO}x budget"

echo
echo "== 4. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 5. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}" "${accessors[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} functions and ${#accessors[@]}/${#accessors[@]} accessors once"

echo
echo "== 6. exactly one zone definition in every target =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}" "${target_fns[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: ${#symbols[@]}/${#symbols[@]} zone symbols and ${#target_fns[@]}/${#target_fns[@]} shim symbols exactly once"
done
stray=$(find "$engine_build" -name 'zone.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray zone.c object(s) in the engine build" >&2
	exit 1
}
shim=$(find "$engine_build" -name 'zone_target.c.o' | wc -l)
[ "$shim" -eq 3 ] || {
	echo "FAIL: $shim zone_target.c object(s), expected 3 (one per target)" >&2
	exit 1
}

echo
echo "PASS: Rust zone -- both zone.c variants agree with the Rust module byte for"
echo "      byte across the hunk, the zone allocator, the cache and the"
echo "      diagnostics; the per-target shim answers correctly in each; all three"
echo "      targets link exactly one definition; the benchmark is within budget."
