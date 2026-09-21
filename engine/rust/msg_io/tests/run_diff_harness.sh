#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build msg_io.c twice with renamed exports -- once as the Hexen II targets
# compile it (no H2W) and once as hwsv does (-DH2W) -- and compare both with
# the consolidated Rust engine staticlib's msg_io feature.  All three live in
# one binary and must agree byte for byte.
#
# The Rust staticlib must have been built with both the msg_io and the sizebuf
# feature: the harness deliberately does not compile the C sizebuf.c, because
# the consolidated crate lands in a single codegen unit and would collide with
# it.  Both C objects and the Rust module call the same SZ_GetSpace/SZ_Write,
# which is what makes this a comparison of msg_io alone.
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
	echo "       build it first: cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features hashindex,mathlib,sizebuf,crc,link_ops,msg_io" >&2
	exit 2
fi

# The Hexen II include order, as the client and h2ded use it.
INCS_H2=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
# The hwsv include order: hexenworld/shared must shadow h2shared so that
# "protocol.h" is the HexenWorld one, with the usercmd_t these functions take.
INCS_H2W=(-I"$engine/hexenworld/server" -I"$engine/hexenworld/shared" \
	-I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every symbol msg_io.c defines, renamed, so the same file can be compiled
# twice and linked against the Rust union without colliding.  The five
# H2W-only names are in the list too: they are simply absent from the object
# built without -DH2W.
rename_for() {
	local p="$1"
	RENAME=(
		-DMSG_WriteChar="${p}MSG_WriteChar"
		-DMSG_WriteByte="${p}MSG_WriteByte"
		-DMSG_WriteShort="${p}MSG_WriteShort"
		-DMSG_WriteLong="${p}MSG_WriteLong"
		-DMSG_WriteFloat="${p}MSG_WriteFloat"
		-DMSG_WriteString="${p}MSG_WriteString"
		-DMSG_WriteCoord="${p}MSG_WriteCoord"
		-DMSG_WriteAngle="${p}MSG_WriteAngle"
		-DMSG_WriteAngle16="${p}MSG_WriteAngle16"
		-DMSG_WriteUsercmd="${p}MSG_WriteUsercmd"
		-DMSG_BeginReading="${p}MSG_BeginReading"
		-DMSG_BeginReadingFrom="${p}MSG_BeginReadingFrom"
		-DMSG_ReadChar="${p}MSG_ReadChar"
		-DMSG_ReadByte="${p}MSG_ReadByte"
		-DMSG_ReadShort="${p}MSG_ReadShort"
		-DMSG_ReadLong="${p}MSG_ReadLong"
		-DMSG_ReadFloat="${p}MSG_ReadFloat"
		-DMSG_ReadString="${p}MSG_ReadString"
		-DMSG_ReadStringLine="${p}MSG_ReadStringLine"
		-DMSG_ReadCoord="${p}MSG_ReadCoord"
		-DMSG_ReadAngle="${p}MSG_ReadAngle"
		-DMSG_ReadAngle16="${p}MSG_ReadAngle16"
		-DMSG_ReadUsercmd="${p}MSG_ReadUsercmd"
		-Dmsg_readcount="${p}msg_readcount"
		-Dmsg_badread="${p}msg_badread"
	)
}

echo "== compiling the C original as the Hexen II targets do (no H2W) =="
rename_for c_
cc "${INCS_H2[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$engine/h2shared/msg_io.c" -o "$OUT/msg_io_h2.o"

echo "== compiling the C original as hwsv does (-DH2W) =="
rename_for w_
cc "${INCS_H2W[@]}" "${CFLAGS[@]}" -DGLQUAKE -DH2W -DSERVERONLY "${RENAME[@]}" \
	-c "$engine/h2shared/msg_io.c" -o "$OUT/msg_io_h2w.o"

echo "== compiling the differential harness =="
cc "${INCS_H2W[@]}" "${CFLAGS[@]}" -DGLQUAKE \
	-c "$here/diff_harness.c" -o "$OUT/diff_harness.o"

echo "== linking all three implementations into one binary =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" \
	"$OUT/msg_io_h2.o" "$OUT/msg_io_h2w.o" "$RUST_LIB" -lm

echo "== running =="
"$OUT/diff_harness"
echo "RESULT: PASS"
