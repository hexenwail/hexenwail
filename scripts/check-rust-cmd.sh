#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-cmd.sh -- gate for the Rust port of engine/h2shared/cmd.c.
#
# The differential half compiles the C original with every exported name
# renamed and runs it beside the Rust module in one binary, comparing the two
# byte for byte after every call.  cmd.c really does differ per target, so it
# runs three arms -- client (-DGLQUAKE), SERVERONLY (-DSERVERONLY) and hwsv
# (-DH2W -DSERVERONLY) -- with engine/rust/cmd_target.c compiled with the same
# defines as the C it is compared against, and compares the arms where the
# variants must agree before comparing either with Rust.
#
# The harness records what the public API exposes and what the hunk does: the
# tokenizer's argc/argv and each token's offset in the zone, the hunk low mark
# (where the registry's nodes and the command buffer's storage come from), the
# cvar list Cmd_Init registers into, Cmd_Exists / Cmd_CheckCommand /
# Cmd_AliasExists over a fixed probe list, and every diagnostic.  Each case runs
# in a child process per implementation, because the registry and alias table
# are static in both with no exported global to reset.
#
# There is deliberately no --engine smoke arm: driving a console session through
# a headless engine diff would need a C-only reference binary built from a
# revision that still had the per-port switches, and the two arms already cover
# command execution, aliasing and dispatch against the C implementation.  Phase
# 6's cvar and zone gates are in the same position.  The PWA job's boot smoke
# and the three-target build below carry the end-to-end load instead.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
		-h|--help) sed -n '2,32p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"

# The exported surface, in cmd.h order, and the per-target predicates
# engine/rust/cmd_target.c adds, and the layout accessors the harness and
# engine/rust/tests/abi_layout.c call.
symbols=(
	Cbuf_Init Cbuf_Clear Cbuf_AddText Cbuf_InsertText Cbuf_Execute
	Cmd_StuffCmds_f Cmd_StartupScript Cmd_Unalias_f Cmd_Unaliasall_f
	Cmd_Argc Cmd_Argv Cmd_Args Cmd_TokenizeString Cmd_AddCommand
	Cmd_Exists Cmd_AliasExists Cmd_CheckCommand Cmd_MoveToFront
	Cmd_ExecuteString Cmd_CheckParm ListCommands ListCvars ListAlias
	Cmd_Init
)
predicates=(
	Cmd_TargetHasClientLists Cmd_TargetIsH2W
	Cmd_TargetHasBuiltinStartupScript
)
accessors=(
	CmdFunctionC_sizeof CmdFunctionC_alignof CmdFunctionC_offsetof_next
	CmdFunctionC_offsetof_name CmdFunctionC_offsetof_function
	CmdAliasC_sizeof CmdAliasC_alignof CmdAliasC_offsetof_next
	CmdAliasC_offsetof_name CmdAliasC_offsetof_value
	Cmd_MAX_ARGS Cmd_MAX_ALIAS_NAME
)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with cmd, sizebuf and zone, for the harness =="
# sizebuf and zone are the substrate both arms call: the C arm's SZ_* and Z_*
# resolve to the same Rust implementations, so the comparison isolates cmd.c.
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features cmd,sizebuf,zone

echo
echo "== 2. differential harness: three arms, the C original and Rust =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/cmd/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checked |^== arm|^-- running' "$diff_log" || true
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

# The summary line carries the case count and the bytes compared, per arm.
# RESULT alone would still pass if the case list were cut down to nothing, so
# both are floored one decimal place below what the arms produce today rather
# than pinned, which would mean editing the gate every time a case is added.
# Observed today: client 8 cases / 19385 bytes, serveronly 7 / 15404,
# hwsv 8 / 15964.
arm_lines=$(grep -cE '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" || true)
if [ "$arm_lines" -lt 3 ]; then
	echo "FAIL: expected three arm summaries, found $arm_lines" >&2
	cat "$diff_log" >&2
	exit 1
fi
while read -r line; do
	nums=$(printf '%s\n' "$line" | grep -oE '[0-9]+' | tr '\n' ' ')
	# fields: expectations failures cases bytes -- cases and bytes are what the
	# floors are about.
	cases=$(printf '%s\n' "$nums" | awk '{print $3}')
	bytes=$(printf '%s\n' "$nums" | awk '{print $4}')
	if [ "${cases:-0}" -lt 7 ]; then
		echo "FAIL: an arm compared ${cases:-0} cases, below the seven-case floor" >&2
		cat "$diff_log" >&2
		exit 1
	fi
	if [ "${bytes:-0}" -lt 15000 ]; then
		echo "FAIL: an arm compared ${bytes:-0} trace bytes, below the 15000 floor" >&2
		cat "$diff_log" >&2
		exit 1
	fi
done < <(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log")
echo "  $(grep -E '^checked ' "$diff_log" | tr '\n' ';')"

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/client/libengine_rs.a"
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
echo "== 5. exactly one cmd definition, and the predicates, in every target =="
# cmd.c is compiled into all three: COMMON_SOURCES for glhexen2 and h2ded,
# HWSV_SOURCES for hwsv, with the SERVERONLY guard inside it.  So every target
# has callers and every target must define the whole surface exactly once, plus
# the per-target predicates engine_rs_attach compiles in.
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
	for sym in "${predicates[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: ${#symbols[@]}/${#symbols[@]} cmd symbols and ${#predicates[@]}/${#predicates[@]} predicates exactly once"
done
stray=$(find "$engine_build" -name 'cmd.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray cmd.c object(s) in the engine build" >&2
	exit 1
}

echo
echo "== 6. the legacy command list matches the C's, entry for entry =="
# Cmd_ExecuteString silently ignores a list of removed legacy commands.  A name
# the port drops turns a silent return into "Unknown command"; a name it
# invents turns a diagnostic into a silent return.  Neither is visible to a
# harness probe of one name, so the two lists are compared here directly and in
# order -- this is the check that catches an entry that exists in the port and
# nowhere in the C.
c_legacy=$(sed -n '/static const char \*legacy_cmds\[\]/,/NULL/p' \
	"$root/engine/h2shared/cmd.c" | grep -oE '"[^"]+"' | tr -d '"')
r_legacy=$(sed -n '/static LEGACY_CMDS/,/^\];/p' \
	"$root/engine/rust/src/cmd.rs" | grep -oE 'b"[^"]*"' \
	| sed 's/^b"//; s/"$//; s/\\0$//' | grep -v '^$')
if [ -z "$c_legacy" ] || [ "$c_legacy" != "$r_legacy" ]; then
	echo "FAIL: the legacy command lists differ (C on the left, Rust on the right)" >&2
	diff <(printf '%s\n' "$c_legacy") <(printf '%s\n' "$r_legacy") >&2 || true
	exit 1
fi
echo "  $(printf '%s\n' "$c_legacy" | wc -l) legacy names match the C's, in order"

echo
echo "PASS: Rust cmd -- the C original and the Rust module agree byte for byte"
echo "      across the registry, the tokenizer, the command buffer, the aliases,"
echo "      every removed legacy name and the diagnostics, in all three target"
echo "      arms; the per-target predicates are compiled into every binary; no"
echo "      cmd.c object is left in the engine build."
