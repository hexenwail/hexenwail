#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the quakefs differential harness twice and compare the two traces.
#
# Unlike the other ports' harnesses, which put both implementations in one
# binary behind a table of function pointers, this one compiles the SAME
# harness source twice -- once with -DIMPL=c_ against the renamed C original,
# once bare against the Rust module -- because quakefs.c exports seventy-odd
# symbols and a vtable that wide is worse than two binaries.  Each writes its
# trace to a file and the script compares them byte for byte, reporting the
# first differing offset.
#
# Both binaries link the SAME substrate: the landed Rust ports for zone,
# hashindex, cvar, cmd, info_str, sizebuf, strlcpy/strlcat and crc, plus the C
# miniz the port still calls.  Only the engine surface is stubbed, once, in the
# harness.  So a difference in the traces is a difference in quakefs.
#
# ARMS.  The C original and the per-target shim are compiled with one of:
#   client      -DGLQUAKE -DH2W_INTEGRATED   (glhexen2)
#   serveronly  -DSERVERONLY                 (h2ded)
#   h2w         -DH2W -DSERVERONLY           (hwsv)
# The Rust module is the same code in all three; what changes is the shim it
# asks, which is the point of the per-target mechanism.
#
# ARMS THAT RUN, AND THE ONE THAT CANNOT.  `client` and `serveronly` run
# green against the demo data.  `h2w` cannot: hwsv's FS_Init refuses that data
# with "You must have the HexenWorld data installed", because HexenWorld needs
# hw/pak4.pak and no HexenWorld data ships in this tree -- the same gap the
# engine smoke tests record ("pak loading on the dedicated HexenWorld server,
# which needs hw/pak4.pak").  The arm is kept because it compiles and links,
# which is what proves the shim's H2W arm and the port's H2W references
# resolve; running its cases needs HexenWorld data that does not exist here.
#
# A FINDING FOR THE WIRING STEP, from building that arm: in H2W the C's
# Host_Error is a macro for SV_Error (hexenworld/server/host.h:62), and the C
# only ever defines SV_Error.  The Rust archive references Host_Error by name,
# so wiring quakefs into hwsv will need that answered -- the shim forwarding
# one to the other, most likely -- or hwsv will not link.  This harness has to
# define both names for the same reason.
#
# Requires cc, cargo and nm -- run inside `nix develop`.
set -euo pipefail

arm="${1:-client}"
case "$arm" in
	client)     CDEFS=(-DGLQUAKE -DH2W_INTEGRATED); INCS=(-Iengine/hexen2 -Iengine/h2shared -Icommon) ;;
	serveronly) CDEFS=(-DSERVERONLY);                INCS=(-Iengine/hexen2 -Iengine/h2shared -Icommon) ;;
	h2w)        CDEFS=(-DH2W -DSERVERONLY);          INCS=(-Iengine/hexenworld/server -Iengine/hexenworld/shared -Iengine/h2shared -Icommon) ;;
	*) echo "unknown arm: $arm (client|serveronly|h2w)" >&2; exit 2 ;;
esac

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"
cd "$root"

RUST_LIB="${RUST_LIB:-$rust_root/target/release/libengine_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

[ -f "$RUST_LIB" ] || {
	echo "error: $RUST_LIB not found; build it with" >&2
	echo "       cargo build --release --offline --manifest-path $rust_root/Cargo.toml --features quakefs,zone,hashindex,cvar,cmd,info_str,sizebuf,crc,strlcpy,strlcat" >&2
	exit 2
}

CFLAGS=(-O1 -g -Wall -Wno-unused-function)

# Every exported name the C original defines, renamed.  The list is the
# declarations in the header, so a new export cannot silently go unrenamed.
RENAME=()
while read -r sym; do
	[ -n "$sym" ] && RENAME+=("-D$sym=c_$sym")
done < <(grep -oE '^(const char|char|void|int|long|qboolean|size_t|byte|unsigned int|fshandle_t|FILE) +\**([A-Za-z_][A-Za-z0-9_]*) *\(' \
	"$engine/h2shared/quakefs.h" | grep -oE '[A-Za-z_][A-Za-z0-9_]* *\($' | tr -d ' (' | sort -u)
[ "${#RENAME[@]}" -ge 50 ] || {
	echo "error: only ${#RENAME[@]} exported names found in quakefs.h; the extraction is broken" >&2
	exit 2
}
for g in fs_gamedir_nopath gameflags fs_filesize file_from_pak oem registered; do
	RENAME+=("-D$g=c_$g")
done

echo "== arm: $arm =="
echo "-- compiling the C original"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" "${RENAME[@]}" \
	-c "$engine/h2shared/quakefs.c" -o "$OUT/quakefs_c.o"

echo "-- compiling the per-target shims, the engine's common.c and miniz"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/cmd_target.c" -o "$OUT/cmd_target.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/zone_target.c" -o "$OUT/zone_target.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$here/compat.c" -o "$OUT/compat.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$root/common/qsnprint.c" -o "$OUT/qsnprint.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$root/common/q_endian.c" -o "$OUT/q_endian.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/quakefs_target.c" -o "$OUT/shim_c.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/quakefs_target.c" -o "$OUT/shim_rust.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$engine/h2shared/miniz.c" -o "$OUT/miniz.o"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" \
	-c "$rust_root/quakefs_variadic.c" -o "$OUT/variadic.o"

echo "-- compiling the harness twice"
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" -c "$here/diff_harness.c" \
	-o "$OUT/harness_rust.o"
# The C arm also compiles the harness with the same renames, so quakefs.h's
# own declarations become the c_* prototypes the calls resolve to.  Without
# that the harness would call implicitly-declared functions and assume int
# returns for every long and pointer in the API.
cc "${INCS[@]}" "${CFLAGS[@]}" "${CDEFS[@]}" -DIMPL=c_ "${RENAME[@]}" \
	-c "$here/diff_harness.c" -o "$OUT/harness_c.o"

echo "-- linking both implementations over the same substrate"
# The C arm links the renamed C original; the Rust arm links the archive's
# quakefs module plus the variadic shim, which the C original does not need
# (it defines those two functions itself).
cc -o "$OUT/h_c" "$OUT/harness_c.o" "$OUT/quakefs_c.o" "$OUT/shim_c.o" \
	"$OUT/variadic.o" "$OUT/miniz.o" "$OUT/compat.o" "$OUT/qsnprint.o" "$OUT/q_endian.o" "$OUT/cmd_target.o" "$OUT/zone_target.o" "$RUST_LIB" -lm
cc -o "$OUT/h_rust" "$OUT/harness_rust.o" "$OUT/shim_rust.o" \
	"$OUT/variadic.o" "$OUT/miniz.o" "$OUT/compat.o" "$OUT/qsnprint.o" "$OUT/q_endian.o" "$OUT/cmd_target.o" "$OUT/zone_target.o" \
	"$RUST_LIB" -lm

# FS_Init identifies the installation from known paks by size and CRC, so the
# harness needs a real Hexen II install.  This is the same demo data the engine
# smoke tests use; the harness puts its own archives in the userdir.
QF_DEMO="${QF_DEMO:-$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail}"
echo "-- base: $QF_DEMO"
export QF_BASEDIR="$QF_DEMO"

echo "-- running both"
# The fixture root is fixed and wiped before each arm, so the second arm does
# not inherit the first's userdir and no path in the trace carries a pid.
export QF_ROOT="$OUT/fixture-root"
set +e
rm -rf "$QF_ROOT"
timeout 120 "$OUT/h_rust" "$OUT/rust.trace"
rust_status=$?
rm -rf "$QF_ROOT"
timeout 120 "$OUT/h_c" "$OUT/c.trace"
c_status=$?
set -e

[ "$rust_status" -eq 0 ] || { echo "FAIL: the Rust arm exited $rust_status" >&2; exit 1; }
[ "$c_status" -eq 0 ] || { echo "FAIL: the C arm exited $c_status" >&2; exit 1; }

echo "-- comparing traces"
if cmp -s "$OUT/c.trace" "$OUT/rust.trace"; then
	echo "quakefs differential harness ($arm arm): the C original and the Rust module agree"
	echo "RESULT: PASS"
	exit 0
fi

echo "FAIL: the C original and the Rust module produced different traces ($arm arm)" >&2
off=$(cmp "$OUT/c.trace" "$OUT/rust.trace" 2>&1 | sed -n 's/.*byte \([0-9]*\).*/\1/p' | head -1)
echo "      first difference at trace byte ${off:-?}; C=$(stat -c%s "$OUT/c.trace") bytes, Rust=$(stat -c%s "$OUT/rust.trace") bytes" >&2
cmp -l "$OUT/c.trace" "$OUT/rust.trace" 2>/dev/null | head -5 >&2 || true
exit 1
