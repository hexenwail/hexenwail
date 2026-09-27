#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-cvar.sh -- gate for the Rust port of engine/h2shared/cvar.c.
#
# The differential half compiles the C original exactly as glhexen2 compiles it
# (-DGLQUAKE, the Hexen II include order) with all twenty-five exported names
# renamed, links it beside the Rust module into one binary, and compares the two
# after every call: the list order as Cvar_FindVarAfter reports it, every field
# of every node the harness registered (including which strings are pointers the
# harness's Z_Malloc handed out), the config-dirty flag, the diagnostics
# CON_Printf produced, the callbacks that fired, and every Z_Malloc / Z_Strdup /
# Z_Free either implementation performed.
#
# Each case runs in a child process, for each implementation separately.
# cvar.c's list head and alias table are `static` in the C original and in the
# Rust module alike, so there is no exported global to reset between cases; a
# fresh process is the only way to start each implementation where the engine
# starts, and it turns a crash into one reported case instead of a dead gate.
#
# There is one C variant to compare against.  cvar.c is in COMMON_SOURCES and in
# HWSV_SOURCES, and it contains no H2W, SERVERONLY or H2W_INTEGRATED arm at all,
# so all three targets compile the same translation -- unlike msg_io (H2W-only
# symbols) or huffman (an allocation strategy per target).  The build half below
# therefore requires all twenty-five symbols in all three binaries, not in one.
#
# --engine is opt-in, as in the wad gate.  cvar registration runs at startup
# (Cvar_Init registers the console commands and every subsystem registers its
# own variables), so an ON/OFF console diff reaches the port; it resolves the
# demo data and Xvfb out of nix itself, so it needs network (or a populated
# store) and a C-only reference glhexen2.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,45p' "$0"; exit 0 ;;
		--engine) run_engine=1 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"

# The exported surface, in cvar.h order, and separately the struct-layout
# accessors the differential harness and engine/rust/tests/abi_layout.c call.
symbols=(
	Cvar_RegisterVariable Cvar_SetCallback Cvar_RegisterAlias Cvar_Set
	Cvar_SetValue Cvar_Reset Cvar_HasValue Cvar_Init Cvar_SetROM
	Cvar_SetValueROM Cvar_SetQuick Cvar_SetValueQuick Cvar_VariableValue
	Cvar_VariableString Cvar_Command Cvar_MoveToFront Cvar_WriteVariables
	Cvar_MarkConfigDirty Cvar_ConfigDirty Cvar_ConfigWritten Cvar_FindVar
	Cvar_FindVarAfter Cvar_LockVar Cvar_UnlockVar Cvar_UnlockAll
)
accessors=(
	CvarC_sizeof CvarC_alignof CvarC_offsetof_name CvarC_offsetof_string
	CvarC_offsetof_flags CvarC_offsetof_value CvarC_offsetof_integer
	CvarC_offsetof_callback CvarC_offsetof_next CvarC_offsetof_default_string
)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with cvar alone, for the differential harness =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features cvar

echo
echo "== 2. differential harness: the C as glhexen2 compiles it, and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/cvar/tests/run_diff_harness.sh" >"$diff_log" 2>&1
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

# The summary line carries the case count and the number of trace bytes
# compared.  RESULT alone would still pass if the case list were cut down to
# nothing, so both are floored rather than pinned: the floor is one decimal
# place below what the harness produces today, which leaves room to widen the
# enumeration without editing the gate.
# Observed today: 11 cases, 20512 trace bytes.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" || true)
cases=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 3p || true)
bytes=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 4p || true)
if [ -z "$cases" ] || [ "${#cases}" -lt 2 ] || [ "$cases" -lt 10 ]; then
	echo "FAIL: harness case count is below the ten-case floor" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi
if [ -z "$bytes" ] || [ "$bytes" -lt 10000 ]; then
	echo "FAIL: harness compared fewer than 10000 trace bytes" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi
echo "  $cases cases, $bytes trace bytes compared"

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
for sym in "${accessors[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} functions and ${#accessors[@]}/${#accessors[@]} accessors once"

echo
echo "== 5. exactly one cvar definition in every target =="
# cvar.c is compiled into all three: COMMON_SOURCES for glhexen2 and h2ded,
# HWSV_SOURCES for hwsv, with no conditional arm.  So every target has callers
# and every target must define the whole surface exactly once.
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
	echo "  $bin: ${#symbols[@]}/${#symbols[@]} cvar symbols exactly once"
done
stray=$(find "$engine_build" -name 'cvar.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray cvar.c object(s) in the engine build" >&2
	exit 1
}

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 6. engine smoke: this engine against a C-only reference =="
	# Reuses the hashindex smoke arm verbatim -- it starts the client, the
	# dedicated server and, where data allows, loads a map, and diffs the two
	# logs.  Cvar_Init and every subsystem's registrations run during startup
	# in all three binaries, and the client writes config.cfg through
	# Cvar_WriteVariables, so the arm reaches the port.
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$engine_build/bin/glhexen2" \
	OFF_BIN="$(rust_gate_reference_bin)" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" cvar
fi

echo
echo "PASS: Rust cvar -- the C original and the Rust module agree byte for byte"
echo "      across the list, the alias table, the callbacks, the allocator and"
echo "      the diagnostics, all three targets link exactly one definition, and"
echo "      no cvar.c object is left in the engine build."
