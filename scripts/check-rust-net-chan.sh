#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check-rust-net-chan.sh -- gate for the Rust port of HexenWorld's channel
# layer, engine/hexenworld/shared/net_chan.c.
#
# The differential half links the renamed C original beside the Rust module and
# drives both through one table of function pointers.  Every framing case
# records three things: the bytes the implementation under test put on the
# wire, the state it derived from those bytes, and what the *other*
# implementation derived from the same bytes when they were handed to it.  So
# the traces can only agree if both ends of the protocol agree -- the bytes
# must be identical for the arms to be comparable at all, and the other side
# must read the same state out of them.
#
# Each case runs in a child process per implementation: the channel's buffers
# and the engine's clock are process-wide, so a fresh process is how each arm
# starts where the engine starts, and a crash is one reported case rather than
# a dead gate.  A child that dies without producing a trace counts as a
# failure; two arms that both crash must never "agree".
#
# The build half proves the naming overlay, which is this port's version of
# what the transport does: net.h renames every symbol under H2W_INTEGRATED, so
# the client's C calls HWNetchan_Transmit and reads hw_net_drop while hwsv's
# calls Netchan_Transmit and reads net_drop.  The module exports the
# HWNetchan_* spelling; engine/rust/net_chan_target.c owns the storage under
# each target's name, the NOT_DEMOPLAYBACK predicate (there is no `cls` in any
# dedicated binary) and the C-variadic Netchan_OutOfBandPrint, which stable
# Rust cannot define.
#
# The structural half compares the port's exported name set and every printf
# format string against the C source, entry for entry: a channel that quietly
# reworded a diagnostic, or that lost an entry point, is exactly the class of
# defect a differential harness run over one packet would miss.  showpackets
# and showdrop are static cvars in the C and private statics here, so no
# harness can set them -- their format strings are checked here instead.
#
# Benchmark decision: none, and for a different reason than the transport's.
# This file's work is header packing and queue arithmetic -- no syscalls at all
# -- but the quantities are a handful of integer and double operations per
# packet, and the operations a benchmark would compare are the two `MSG_Write`
# calls and two `memcpy`s the port shares with the C.  A number from it would
# measure the harness's loop, not the port.
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

# The module exports the unambiguous spelling; the C's eight exported names are
# what the structural comparison below checks it against.  OutOfBandPrint is
# the ninth symbol the ABI needs and the shim owns it, so it is checked per
# binary rather than in the archive.
hw_functions=(HWNetchan_Init HWNetchan_OutOfBand HWNetchan_Setup
	HWNetchan_CanPacket HWNetchan_CanReliable HWNetchan_Transmit
	HWNetchan_Process)
plain_functions=(Netchan_Init Netchan_OutOfBand Netchan_OutOfBandPrint
	Netchan_Setup Netchan_CanPacket Netchan_CanReliable Netchan_Transmit
	Netchan_Process)
hw_extra=(HWNetchan_OutOfBandPrint)
globals=(net_drop)
accessors=(NetChan_TargetDrop NetChan_TargetNotDemoPlayback)
formats=('--> s=%i(%i) a=%i(%i) %i' '<-- s=%u(%i) a=%u(%i) %i'
	'%s:Outgoing message overflow' '%s:Out of order packet %u at %i'
	'%s:Dropped %u packets at %u')
# The harness links the sizebuf and the message reader from the archive: they
# are Rust ports, and the C original calls the same functions, so they are the
# substrate both arms share rather than a second implementation.
HARNESS_FEATURES="net_chan,sizebuf,msg_io"

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
echo "== 2. differential harness: the C original and the Rust module, both ways =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/net_chan/tests/run_diff_harness.sh" >"$diff_log" 2>&1
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
# cut down to nothing, so the count and the number of bytes compared are both
# bounded below.  Observed today: 6 cases, 16715 trace bytes.
count_line=$(grep -E '^checked [0-9]+ expectations, [0-9]+ failures, [0-9]+ cases, [0-9]+ trace bytes$' "$diff_log" || true)
cases=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 3p || true)
bytes=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | sed -n 4p || true)
if [ -z "$cases" ] || [ "$cases" -lt 6 ]; then
	echo "FAIL: harness case count is below the six-case floor" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi
if [ -z "$bytes" ] || [ "$bytes" -lt 12000 ]; then
	echo "FAIL: harness compared fewer than 12000 trace bytes" >&2
	echo "      observed: ${count_line:-<no summary line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi
echo "  $cases cases, $bytes trace bytes compared"

echo
echo "== 3. the C's exported names and format strings, against the port =="
c_names=$(sed -n 's/^\(void\|int\|qboolean\|const char \*\)[[:space:]]*\(Netchan_[A-Za-z]*\)[[:space:]]*(.*/\2/p' \
	"$root/engine/hexenworld/shared/net_chan.c" | sort -u)
r_names=$(printf '%s\n' "${plain_functions[@]}" | sort -u)
if [ "$c_names" != "$r_names" ]; then
	echo "FAIL: the port's function set differs from the C's" >&2
	diff <(printf '%s\n' "$c_names") <(printf '%s\n' "$r_names") >&2 || true
	exit 1
fi
echo "  $(printf '%s\n' "$c_names" | wc -l) functions match the C's, name for name"

for fmt in "${formats[@]}"; do
	grep -qF "\"$fmt" "$root/engine/hexenworld/shared/net_chan.c" || {
		echo "FAIL: the C no longer formats \"$fmt\"" >&2
		exit 1
	}
	grep -qF "c\"$fmt" "$crate/src/net_chan.rs" || {
		echo "FAIL: the port does not format \"$fmt\"" >&2
		exit 1
	}
done
echo "  ${#formats[@]} format strings present in both"

echo
echo "== 4. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 5. the archive exports the whole ABI exactly once =="
archive="$engine_build/rust/client/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${hw_functions[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
# The accessors are not here: they live in the per-target shim, which is
# compiled into each binary rather than into the archive.  Step 6 checks them
# where they are.
echo "  libengine_rs.a: ${#hw_functions[@]} HWNetchan_* functions once"

echo
echo "== 6. each target gets the spelling its own C calls =="
check_once() { # binary symbol label
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

# glhexen2: H2W_INTEGRATED, so the renamed spellings, and the shim's storage
# for net_drop together with the transport's for the message buffers.
for sym in "${hw_functions[@]}" "${hw_extra[@]}"; do
	check_once "$engine_build/bin/glhexen2" "$sym" glhexen2
done
check_once "$engine_build/bin/glhexen2" hw_net_drop glhexen2
for sym in "${accessors[@]}"; do
	check_once "$engine_build/bin/glhexen2" "$sym" glhexen2
done
echo "  glhexen2: ${#hw_functions[@]} HWNetchan_* functions, the variadic $(printf '%s' "${hw_extra[@]}"), hw_net_drop and ${#accessors[@]} accessors exactly once"

# hwsv: the plain names the C calls, which the shim forwards, plus the
# HWNetchan_* entry points the forwards reach.
for sym in "${plain_functions[@]}" "${hw_functions[@]}"; do
	check_once "$engine_build/bin/hwsv" "$sym" hwsv
done
check_once "$engine_build/bin/hwsv" net_drop hwsv
for sym in "${accessors[@]}"; do
	check_once "$engine_build/bin/hwsv" "$sym" hwsv
done
echo "  hwsv: ${#plain_functions[@]} Netchan_* functions, ${#hw_functions[@]} HWNetchan_* entry points, net_drop and ${#accessors[@]} accessors exactly once"

# h2ded builds no channel, but links the archive: its symbols may exist and
# must never be duplicated.
for sym in "${hw_functions[@]}" "${accessors[@]}"; do
	check_at_most_once "$engine_build/bin/h2ded" "$sym" h2ded
done
echo "  h2ded: every channel symbol at most once (no HexenWorld channel in this target)"

stray=$(find "$engine_build" -name 'net_chan.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray net_chan.c object(s) in the engine build" >&2
	exit 1
}

echo
echo "PASS: Rust net_chan -- the C original and the Rust channel agree byte for"
echo "      byte on the packets they emit and on the state each derives from the"
echo "      other's packets; each target gets the spelling its own C calls,"
echo "      exactly once; no net_chan.c object is left in the engine build."
