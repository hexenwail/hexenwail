#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen huffman fixtures can still fail.  Applies each mutant to
# engine/rust/src/huffman.rs in turn, runs the kill command, and requires it to
# FAIL (kill) for every mutant.  The file is restored on exit.
#
#   bash engine/rust/huffman/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/huffman/tests/mutants.sh -- some cmd...  # any other kill command
#
# Not part of check-rust-huffman.sh (it rewrites a tracked source); run it when
# the fixtures or huffman.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src/huffman.rs"
crate="$here/.."
orig="$(mktemp)"
cp "$src" "$orig"
trap 'cp "$orig" "$src"; rm -f "$orig"' EXIT

# Default kill command: the release-profile replay (what the engine ships) must
# run and fail.  A mutant that merely fails to compile proves nothing, so that
# is an error (return 2, printed by the loop below), not a kill.
replay() {
	local out
	if out=$(cargo test --release --offline --manifest-path "$crate/Cargo.toml" 2>&1); then return 0; fi
	if grep -q 'could not compile' <<<"$out"; then
		echo "BROKEN MUTANT (does not compile)" >&2; echo "$out" >&2; return 2
	fi
	return 1
}
errf="$(mktemp)"
trap 'cp "$orig" "$src"; rm -f "$orig" "$errf"' EXIT
if [ "${1:-}" = "--" ]; then shift; kill_cmd=("$@"); else kill_cmd=(replay); fi

# name|perl -0 substitution (applied to huffman.rs)
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'Masks 0x10 and 0x20 swapped|s/0x10, 0x20/0x20, 0x10/'
	'PutBit xors instead of setting|s/\|= mask;/^= mask;/'
	'GetBit inverted|s/!= 0 \{\n        1\n    \} else \{\n        0/!= 0 {\n        0\n    } else {\n        1/'
	'FindTab one-branch code loses its 1 bit|s/\(bits << 1\) \| 1\)/(bits << 1))/'
	'BuildTree min1 tie-break < becomes <=|s/\(\*w\)\.freq < min1/(*w).freq <= min1/'
	'BuildTree min2 tie-break < becomes <=|s/\(\*w\)\.freq < min2/(*w).freq <= min2/'
	'BuildTree zero and one children swapped|s/\(\*tmp\)\.zero = work\[minat2 as usize\];\n        \(\*tmp\)\.one = work\[minat1 as usize\];/(*tmp).zero = work[minat1 as usize];\n        (*tmp).one = work[minat2 as usize];/'
	'BuildTree subtracts the two frequencies|s/\(\*work\[minat2 as usize\]\)\.freq \+ /(*work[minat2 as usize]).freq - /'
	'HuffInit perturbs one frequency|s/build_tree\(&HUFF_FREQ\);/{ let mut f = HUFF_FREQ; f[32] += 1.0; build_tree(&f); }/'
	'Huffman_freq reads the neighbour|s/\n    HUFF_FREQ\[index\]\n/\n    HUFF_FREQ[index ^ 1]\n/'
	'HuffEncode bit position off by one|s/bitat \+ tab\.len - j - 1/bitat + tab.len - j/'
	'HuffEncode advances one bit too far|s/bitat \+= tab\.len;/bitat += tab.len + 1;/'
	'HuffEncode writes at out, not out+1|s/put_bit\(out\.add\(1\), bitat/put_bit(out.add(2), bitat/'
	'HuffEncode padding count off by one|s/\(8 \* \(\*outlen - 1\) - bitat\) as u8/(8 * (*outlen - 1) - bitat + 1) as u8/'
	'HuffEncode rounds the length down a bit early|s/1 \+ \(bitat \+ 7\) \/ 8/1 + (bitat + 6) \/ 8/'
	'HuffEncode raw fallback needs strictly larger|s/\*outlen >= inlen \+ 1/*outlen > inlen + 1/'
	'HuffEncode raw marker 0xff -> 0xfe|s/\*out = 0xff;\n        \/\/ A negative/*out = 0xfe;\n        \/\/ A negative/'
	'HuffEncode raw outlen drops the header|s/\*outlen = inlen \+ 1;/*outlen = inlen;/'
	'HuffDecode inlen guard off by one|s/if inlen < 0 \{/if inlen < -1 {/'
	'HuffDecode raw copy ignores maxlen|s/if inlen > maxlen \{/if inlen > maxlen + 1 {/'
	'HuffDecode padding count ignored|s/data_bits - \*input as c_int/data_bits/'
	'HuffDecode bound removed (reads past the packet)|s/if bits >= data_bits \{\n                return;\n            \}//'
	'HuffDecode bound one bit late|s/if bits >= data_bits \{/if bits > data_bits {/'
	'HuffDecode maxlen check off by one|s/if \*outlen > maxlen \{/if *outlen >= maxlen {/'
	'HuffDecode reads bits from the header byte|s/get_bit\(input\.add\(1\), bits\)/get_bit(input, bits)/'
	'HuffDecode keeps decoding past maxlen|s/return; \/\/ out\[maxlen - 1\] is written already/\/\/ no return/'
)

survived=0
for m in "${mutants[@]}"; do
	name="${m%%|*}"; sub="${m#*|}"
	perl -0pe "$sub" "$orig" >"$src"
	if cmp -s "$src" "$orig"; then echo "BROKEN MUTANT (no change): $name" >&2; exit 2; fi
	rc=0
	"${kill_cmd[@]}" >/dev/null 2>"$errf" || rc=$?
	if [ "$rc" -eq 2 ]; then cat "$errf" >&2; echo "(mutant: $name)" >&2; exit 2; fi
	if [ "$rc" -eq 0 ]; then
		echo "SURVIVED: $name"; survived=$((survived + 1))
	else
		echo "killed:   $name"
	fi
done
cp "$orig" "$src"
[ "$survived" -eq 0 ] || { echo "FAIL: $survived mutant(s) survived" >&2; exit 1; }
echo "PASS: all ${#mutants[@]} mutants killed"
