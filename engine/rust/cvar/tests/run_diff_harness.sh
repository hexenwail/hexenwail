#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/h2shared/cvar.c the way glhexen2 compiles it -- -DGLQUAKE, with
# engine/hexen2 and engine/h2shared ahead of common/ on the include path -- with
# every exported name renamed so it can share a binary with the Rust cvar
# module, and compare the two against each other.
#
# Only one C variant exists to compare against: cvar.c is in COMMON_SOURCES and
# in HWSV_SOURCES, and it has no H2W / SERVERONLY / H2W_INTEGRATED arm at all,
# so the same translation is what all three targets compile.  The harness
# therefore compares one C against one Rust.
#
# q_snprintf and q_strlcpy come from their real sources; q_strcasecmp is the
# harness's copy of engine/h2shared/common.c's (pulling that file in would drag
# the whole of common.c with it).  CON_Printf, Cmd_* and the zone trio are
# defined by diff_harness.c, which also records every call.
#
# The Rust staticlib must have been built with the cvar feature.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: consolidated Rust staticlib not found: $RUST_LIB" >&2
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features cvar" >&2
	exit 2
fi

# The Hexen II include order, as the client uses it.
INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol cvar.c exports, renamed.  The file's own statics (the list head,
# the alias table, the dirty flag) are internal to each implementation and stay
# unrenamed: the C object gets its own, the Rust module has its own.
RENAME=(
	-DCvar_FindVar=c_Cvar_FindVar
	-DCvar_FindVarAfter=c_Cvar_FindVarAfter
	-DCvar_LockVar=c_Cvar_LockVar
	-DCvar_UnlockVar=c_Cvar_UnlockVar
	-DCvar_UnlockAll=c_Cvar_UnlockAll
	-DCvar_VariableValue=c_Cvar_VariableValue
	-DCvar_VariableString=c_Cvar_VariableString
	-DCvar_MarkConfigDirty=c_Cvar_MarkConfigDirty
	-DCvar_ConfigDirty=c_Cvar_ConfigDirty
	-DCvar_ConfigWritten=c_Cvar_ConfigWritten
	-DCvar_SetQuick=c_Cvar_SetQuick
	-DCvar_SetValueQuick=c_Cvar_SetValueQuick
	-DCvar_Set=c_Cvar_Set
	-DCvar_SetValue=c_Cvar_SetValue
	-DCvar_SetROM=c_Cvar_SetROM
	-DCvar_SetValueROM=c_Cvar_SetValueROM
	-DCvar_RegisterVariable=c_Cvar_RegisterVariable
	-DCvar_Reset=c_Cvar_Reset
	-DCvar_HasValue=c_Cvar_HasValue
	-DCvar_Init=c_Cvar_Init
	-DCvar_SetCallback=c_Cvar_SetCallback
	-DCvar_RegisterAlias=c_Cvar_RegisterAlias
	-DCvar_Command=c_Cvar_Command
	-DCvar_MoveToFront=c_Cvar_MoveToFront
	-DCvar_WriteVariables=c_Cvar_WriteVariables
)

echo "== compiling the C original as glhexen2 does (-DGLQUAKE) =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$engine/h2shared/cvar.c" -o "$OUT/cvar_c.o"

echo "== compiling the engine helpers both implementations call =="
cc "${INCS[@]}" "${CFLAGS[@]}" -c "$root/common/qsnprint.c" -o "$OUT/qsnprint.o"
cc "${INCS[@]}" "${CFLAGS[@]}" -c "$root/common/strlcpy.c" -o "$OUT/strlcpy.o"

echo "== compiling the differential harness =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking both implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/cvar_c.o" \
	"$OUT/qsnprint.o" "$OUT/strlcpy.o" "$RUST_LIB" -lm

echo "== running =="
# The timeout is load-bearing, not defensive habit.  Cvar_RegisterAlias's
# ping-pong and Cvar_SetQuick's callback give a port that gets the no-change
# early return wrong a way to recurse instead of failing, and a gate that hangs
# is worse than one that fails: CI cannot afford a job that never returns, and
# a hang reads as a slow machine rather than as a broken port.
harness_timeout="${HARNESS_TIMEOUT:-60}"
set +e
timeout -k 5 "$harness_timeout" "$OUT/diff_harness"
status=$?
set -e
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "FAIL: the harness did not finish within ${harness_timeout}s." >&2
	echo "      That is what a missing no-change return above the callback" >&2
	echo "      looks like: the alias mirror, or a self-setting callback," >&2
	echo "      never bottoms out." >&2
	exit 1
fi
[ "$status" -eq 0 ] || exit "$status"

echo "RESULT: PASS"
