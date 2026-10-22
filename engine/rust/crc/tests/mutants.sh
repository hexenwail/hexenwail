#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen crc fixtures can still fail.  Applies each mutant to
# engine/rust/src/crc.rs in turn, runs the kill command, and requires it to
# FAIL (kill) for every mutant.  The file is restored on exit.
#
#   bash engine/rust/crc/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/crc/tests/mutants.sh -- some cmd...  # any other kill command
#
# The const asserts that spot-check the generated table are stripped from every
# mutant, so a table mutant must be caught by the replay, not by the compiler.
# Not part of check-rust-crc.sh (it rewrites a tracked source); run it when the
# fixtures or crc.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src/crc.rs"
crate="$here/.."
orig="$(mktemp)"
cp "$src" "$orig"
trap 'cp "$orig" "$src"; rm -f "$orig"' EXIT

# Default kill command: the replay must run and fail.  A mutant that merely
# fails to compile proves nothing, so that is an error, not a kill.
replay() {
	local out
	if out=$(cargo test --offline --manifest-path "$crate/Cargo.toml" 2>&1); then return 0; fi
	if grep -q 'could not compile' <<<"$out"; then
		echo "BROKEN MUTANT (does not compile)" >&2; echo "$out" >&2; exit 2
	fi
	return 1
}
if [ "${1:-}" = "--" ]; then shift; kill_cmd=("$@"); else kill_cmd=(replay); fi

# name|perl -0 substitution (applied to crc.rs with the const asserts removed)
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'poly 0x1021 -> 0x1023|s/\^ 0x1021/^ 0x1023/'
	'table builder runs 7 bits, not 8|s/bit < 8/bit < 7/'
	'table entry 0x5a has one bit flipped|s/(\n    table\n\})/\n    table[0x5a] ^= 1;$1/'
	'init value 0xffff -> 0x0000|s/CRC_INIT_VALUE: u16 = 0xffff/CRC_INIT_VALUE: u16 = 0x0000/'
	'final xor 0x0000 -> 0x0001|s/CRC_XOR_VALUE: u16 = 0x0000/CRC_XOR_VALUE: u16 = 0x0001/'
	'step shifts 7 not 8|s/\(crc << 8\) \^ CRCTABLE/(crc << 7) ^ CRCTABLE/'
	'step index ORs the data instead of xor|s/as u8 \^ data\) as usize/as u8 | data) as usize/'
	'block loop stops one byte early|s/while count != 0/while count > 1/'
	'block loop re-reads the same byte|s/start = start.add\(1\)/start = start.add(0)/'
	'ProcessBlock ignores the running value|s/let mut crc = \*crcvalue;/let mut crc = 0xffff;/'
	'CRC_Block drops the last byte|s/CRC_ProcessBlock\(start, &mut crc, count\)/CRC_ProcessBlock(start, &mut crc, (count - 1).max(0))/'
	'CRC_Block seeds with 0 not init|s/CRC_Init\(&mut crc\);/crc = 0;/'
)

survived=0
for m in "${mutants[@]}"; do
	name="${m%%|*}"; sub="${m#*|}"
	perl -0pe 's/\n\/\/ Spot-check.*?\n\};\n//s' "$orig" | perl -0pe "$sub" >"$src"
	if cmp -s "$src" "$orig"; then echo "BROKEN MUTANT (no change): $name" >&2; exit 2; fi
	if "${kill_cmd[@]}" >/dev/null 2>&1; then
		echo "SURVIVED: $name"; survived=$((survived + 1))
	else
		echo "killed:   $name"
	fi
done
cp "$orig" "$src"
[ "$survived" -eq 0 ] || { echo "FAIL: $survived mutant(s) survived" >&2; exit 1; }
echo "PASS: all ${#mutants[@]} mutants killed"
