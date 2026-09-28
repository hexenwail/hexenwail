#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-net-udp-hw.sh -- gate for the Rust port of HexenWorld's transport,
# engine/hexenworld/shared/net_udp.c.
#
# The differential half links the renamed C original beside the Rust module and
# drives both through one table of function pointers: the address conversions
# and their printed forms, a real round trip over a loopback UDP socket with the
# packet encoded and decoded through the Huffman codec at both ends, and the
# failure paths.  Each case runs in a child process per implementation, because
# the transport owns a process-wide socket and process-wide storage.
#
# The build half proves the two things that make this port unusual:
#
#   * the **names** are per target.  hexenworld/shared/net.h renames every
#     symbol here under H2W_INTEGRATED, so the client's C calls HWNET_Init and
#     reads hw_net_message while hwsv's calls NET_Init and reads net_message.
#     The gate checks each binary for the spelling its own C uses, and that the
#     plain names in hwsv come from engine/rust/net_udp_hw_target.c.
#   * there is exactly one definition of each exported global per target,
#     including the client's, where Hexen II's net_main.c already owns a
#     `net_message`.
#
# It also compares the port's exported name set and its two format strings
# against the C source, entry for entry: an address formatter that quietly
# gained or lost a field is exactly the class of defect a differential harness
# run against one address value would miss.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"

# The names the Rust module exports -- the unambiguous spelling -- and the names
# each target's C actually calls, which is what the per-binary checks assert.
hw_functions=(HWNET_Init HWNET_Shutdown HWNET_GetPacket HWNET_SendPacket
	HWNET_CheckReadTimeout HWNET_CompareAdr HWNET_CompareBaseAdr
	HWNET_AdrToString HWNET_BaseAdrToString HWNET_StringToAdr)
plain_functions=(NET_Init NET_Shutdown NET_GetPacket NET_SendPacket
	NET_CheckReadTimeout NET_CompareAdr NET_CompareBaseAdr
	NET_AdrToString NET_BaseAdrToString NET_StringToAdr)
hw_globals=(hw_net_local_adr hw_net_loopback_adr hw_net_from hw_net_message)
plain_globals=(net_local_adr net_loopback_adr net_from net_message)
accessors=(NetUDP_TargetMessageBuf NetUDP_TargetFrom NetUDP_TargetLocalAdr
	NetUDP_TargetLoopbackAdr)
# The harness links the sizebuf and the codec from the archive: they are the
# Rust ports, and the C original calls the same functions, so they are the
# substrate both arms share rather than a second implementation.
HARNESS_FEATURES="net_udp_hw,sizebuf,huffman"

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate with the harness's substrate =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features "$HARNESS_FEATURES"

echo
echo "== 2. differential harness: the C original and the Rust module =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/net_udp_hw/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checked |differential harness' "$diff_log" || true
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
# Floors rather than pins: RESULT alone would still pass if the case list were
# cut down to nothing.  Observed today: 3 cases, 800 trace bytes.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" || true)
cases=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 3p || true)
bytes=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 4p || true)
if [ -z "$cases" ] || [ "$cases" -lt 3 ]; then
	echo "FAIL: harness case count is below the three-case floor" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	exit 1
fi
if [ -z "$bytes" ] || [ "$bytes" -lt 500 ]; then
	echo "FAIL: harness compared fewer than 500 trace bytes" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	exit 1
fi
echo "  $cases cases, $bytes trace bytes compared"

echo
echo "== 3. the C's exported names and format strings, against the port =="
# Every non-static definition in the C original, which is what the port owes.
c_names=$(sed -n 's/^\(void\|int\|qboolean\|const char \*\)[[:space:]]*\(NET_[A-Za-z]*\)[[:space:]]*(.*/\2/p' \
	"$root/engine/hexenworld/shared/net_udp.c" | sort -u)
r_names=$(printf '%s\n' "${hw_functions[@]}" | sed 's/^HWNET_/NET_/' | sort -u)
if [ "$c_names" != "$r_names" ]; then
	echo "FAIL: the port's function set differs from the C's" >&2
	diff <(printf '%s\n' "$c_names") <(printf '%s\n' "$r_names") >&2 || true
	exit 1
fi
echo "  $(printf '%s\n' "$c_names" | wc -l) functions match the C's, name for name"

# The two printed forms, which are the part of the API a player actually sees.
for fmt in '%i.%i.%i.%i:%i' '%i.%i.%i.%i'; do
	grep -qF "\"$fmt\"" "$root/engine/hexenworld/shared/net_udp.c" || {
		echo "FAIL: the C no longer formats \"$fmt\"" >&2
		exit 1
	}
	grep -qF "c\"$fmt\"" "$crate/src/net_udp_hw.rs" || {
		echo "FAIL: the port does not format \"$fmt\"" >&2
		exit 1
	}
done
echo "  2 address format strings present in both"

echo
echo "== 4. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 5. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
# The accessors are not here: they live in the per-target shim, which is
# compiled into each binary rather than into the archive.  Step 6 checks them
# where they are.
for sym in "${hw_functions[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#hw_functions[@]} HWNET_* functions once"

echo
echo "== 6. each target gets the spelling its own C calls =="
check_once() { # binary symbol
	local n
	n=$(nm "$1" | grep -cE " [TtDdBb] $2\$" || true)
	[ "$n" -eq 1 ] || {
		echo "FAIL: $3: $2 has $n definitions, expected exactly 1" >&2
		exit 1
	}
}
check_at_most_once() {
	local n
	n=$(nm "$1" | grep -cE " [TtDdBb] $2\$" || true)
	[ "$n" -le 1 ] || {
		echo "FAIL: $3: $2 is defined $n times, expected at most 1" >&2
		exit 1
	}
}

# glhexen2: H2W_INTEGRATED, so the renamed spellings, and Hexen II's own
# net_message/NET_Init stay Hexen II's.
for sym in "${hw_functions[@]}" "${hw_globals[@]}" "${accessors[@]}"; do
	check_once "$engine_build/bin/glhexen2" "$sym" glhexen2
done
echo "  glhexen2: ${#hw_functions[@]} HWNET_* functions, ${#hw_globals[@]} hw_net_* globals and ${#accessors[@]} shim accessors exactly once"

# hwsv: the plain names, the wrappers the shim adds, and the shim's storage.
for sym in "${plain_functions[@]}" "${plain_globals[@]}" "${hw_functions[@]}" "${accessors[@]}"; do
	check_once "$engine_build/bin/hwsv" "$sym" hwsv
done
echo "  hwsv: ${#plain_functions[@]} NET_* functions, ${#plain_globals[@]} globals and the HWNET_* entry points exactly once"

# h2ded builds no transport, but links the archive: the symbols may exist and
# must never be duplicated.
for sym in "${hw_functions[@]}" "${accessors[@]}" net_message NET_Init; do
	check_at_most_once "$engine_build/bin/h2ded" "$sym" h2ded
done
echo "  h2ded: every transport symbol at most once (no transport in this target)"

stray=$(find "$engine_build" -name 'net_udp.c.o' -path '*hexenworld*' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray hexenworld net_udp.c object(s) in the engine build" >&2
	exit 1
}

echo
echo "PASS: Rust net_udp_hw -- the C original and the Rust transport agree byte"
echo "      for byte across the address conversions, a real loopback round trip"
echo "      through the Huffman codec and the failure paths; each target gets the"
echo "      spelling its own C calls, exactly once; no net_udp.c object is left in"
echo "      the HexenWorld build."
