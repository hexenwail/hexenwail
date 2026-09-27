#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build engine/h2shared/cmd.c the way each target compiles it, with every
# exported name renamed so it can share a binary with the Rust cmd module, and
# compare the two against each other.
#
# One binary per arm, because cmd.c really differs per target:
#
#   client         -DGLQUAKE            the listing surface exists
#   serveronly     -DSERVERONLY         it does not; no H2W arm either
#   h2w            -DH2W -DSERVERONLY   the NULL-handler diagnostic
#
# Each arm compiles the C original, this harness and engine/rust/cmd_target.c
# with the same defines, so the Rust module's per-target predicates answer what
# that arm's C compiled rather than what the build host happens to be.
#
# The zone allocator and the sizebuf are linked from the Rust archive -- the
# same implementation for both arms -- so the comparison isolates cmd.c: the C
# arm's Z_Malloc and SZ_Write calls resolve to the same Rust code the Rust arm
# calls.  The cvar list is a harness substrate instead, because Cvar_Init
# registers its console commands through the *unrenamed* Cmd_AddCommand -- the
# port under test -- and letting the real one run would give the Rust registry
# entries the C registry cannot have.
#
# The Rust staticlib must have been built with the cmd, sizebuf and zone
# features.
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features cmd,sizebuf,zone" >&2
	exit 2
fi

# Every symbol cmd.c exports, renamed -- including the cmd_source global.
RENAME=(
	-DCbuf_Init=c_Cbuf_Init
	-DCbuf_Clear=c_Cbuf_Clear
	-DCbuf_AddText=c_Cbuf_AddText
	-DCbuf_InsertText=c_Cbuf_InsertText
	-DCbuf_Execute=c_Cbuf_Execute
	-DCmd_Init=c_Cmd_Init
	-DCmd_AddCommand=c_Cmd_AddCommand
	-DCmd_Exists=c_Cmd_Exists
	-DCmd_AliasExists=c_Cmd_AliasExists
	-DCmd_CheckCommand=c_Cmd_CheckCommand
	-DCmd_MoveToFront=c_Cmd_MoveToFront
	-DCmd_Argc=c_Cmd_Argc
	-DCmd_Argv=c_Cmd_Argv
	-DCmd_Args=c_Cmd_Args
	-DCmd_CheckParm=c_Cmd_CheckParm
	-DCmd_TokenizeString=c_Cmd_TokenizeString
	-DCmd_ExecuteString=c_Cmd_ExecuteString
	-DCmd_StuffCmds_f=c_Cmd_StuffCmds_f
	-DCmd_StartupScript=c_Cmd_StartupScript
	-DCmd_Unalias_f=c_Cmd_Unalias_f
	-DCmd_Unaliasall_f=c_Cmd_Unaliasall_f
	-DListCommands=c_ListCommands
	-DListCvars=c_ListCvars
	-DListAlias=c_ListAlias
	-Dcmd_source=c_cmd_source
)

run_arm() {
	local arm="$1"; shift
	local defines=("$@")
	local incs

	case "$arm" in
		h2w)
			incs=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared"
			      -I"$engine/h2shared" -I"$root/common")
			;;
		*)
			incs=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
			;;
	esac

	local cflags=(-O1 -g -Wall -Wno-unused-function -Wno-unused-variable)

	echo "== arm: $arm ${defines[*]} =="
	cc "${incs[@]}" "${cflags[@]}" "${defines[@]}" "${RENAME[@]}" \
		-c "$engine/h2shared/cmd.c" -o "$OUT/cmd_c_$arm.o"
	cc "${incs[@]}" "${cflags[@]}" "${defines[@]}" \
		-c "$rust_root/cmd_target.c" -o "$OUT/cmd_target_$arm.o"
	# q_strlcpy/q_strlcat come from their real sources: the ports call them and
	# the archive is built without those features.
	cc "${incs[@]}" "${cflags[@]}" -c "$root/common/strlcpy.c" -o "$OUT/strlcpy_$arm.o"
	cc "${incs[@]}" "${cflags[@]}" -c "$root/common/strlcat.c" -o "$OUT/strlcat_$arm.o"
	cc "${incs[@]}" "${cflags[@]}" "${defines[@]}" \
		-c "$here/diff_harness.c" -o "$OUT/cmd_harness_$arm.o"
	cc -o "$OUT/cmd_harness_$arm" "$OUT/cmd_harness_$arm.o" \
		"$OUT/cmd_c_$arm.o" "$OUT/cmd_target_$arm.o" \
		"$OUT/strlcpy_$arm.o" "$OUT/strlcat_$arm.o" \
		"$RUST_LIB" -lm

	echo "-- running $arm"
	timeout -k 5 "${HARNESS_TIMEOUT:-60}" "$OUT/cmd_harness_$arm"
}

run_arm client -DGLQUAKE
run_arm serveronly -DSERVERONLY
run_arm h2w -DH2W -DSERVERONLY

echo "RESULT: PASS"
