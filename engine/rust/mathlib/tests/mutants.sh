#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen mathlib fixtures can still fail.  Applies each mutant to
# engine/rust/mathlib/src/ffi.rs in turn, runs the kill command, and requires
# it to FAIL (kill) for every mutant.  The file is restored on exit.
#
#   bash engine/rust/mathlib/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/mathlib/tests/mutants.sh -- some cmd...  # any other kill command
#
# Not part of check-rust-mathlib.sh (it rewrites a tracked source); run it when
# the fixtures or ffi.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src/ffi.rs"
crate="$here/../replay"
orig="$(mktemp)"
cp "$src" "$orig"
trap 'cp "$orig" "$src"; rm -f "$orig"' EXIT

# Default kill command: the release-profile replay (what the engine ships) must
# run and fail.  A mutant that merely
# fails to compile proves nothing, so that is an error, not a kill.
replay() {
	local out
	# A mutant that loops forever (GCD with the wrong step) is a kill: 124.
	if out=$(timeout 120 cargo test --release --offline --manifest-path "$crate/Cargo.toml" 2>&1); then return 0; fi
	if grep -q 'could not compile' <<<"$out"; then
		echo "BROKEN MUTANT (does not compile)" >&2; echo "$out" >&2; return 2
	fi
	return 1
}
errf="$(mktemp)"
trap 'cp "$orig" "$src"; rm -f "$orig" "$errf"' EXIT
if [ "${1:-}" = "--" ]; then shift; kill_cmd=("$@"); else kill_cmd=(replay); fi

# name|perl -0 substitution applied to ffi.rs
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'MPlane padding grows to 3 bytes (sizeof 20 -> 24)|s/pad: \[Byte; 2\]/pad: [Byte; 3]/'
	'Q_isnan treats infinity as NaN|s/exponent == 0xFF && mantissa != 0/exponent == 0xFF/'
	'Q_isnan mantissa mask loses the top bit|s/bits & 0x7FFFFF;/bits & 0x3FFFFF;/'
	'anglemod masks 15 bits, not 16|s/& 65535\) as c_double\)/& 32767) as c_double)/'
	'anglemod scale 65536/360 -> 65535/360|s/\(65536\.0_f64 \/ 360\.0\)\) as c_int/(65535.0_f64 \/ 360.0)) as c_int/'
	'anglemod runs in f32 (the pre-fix port)|s/\(a as c_double\) \* \(65536\.0_f64 \/ 360\.0\)/(a * (65536.0_f32 \/ 360.0_f32)) as c_double/'
	'GCD uses division, not remainder|s/i2 = i1 % i2;/i2 = i1 \/ i2;/'
	'GCD loop stops at 1|s/while i2 != 0 \{\n        let t/while i2 > 1 {\n        let t/'
	'NearestMultiple negative branch drops the +1|s/\(num - mul \+ 1\)/(num - mul)/'
	'Q_log2 stops at 2|s/while val > 1/while val > 2/'
	'Q_log2 shifts by 2|s/val >>= 1;/val >>= 2;/'
	'Invert24To16 threshold 256 -> 255|s/if val < 256 \{/if val < 255 {/'
	'Invert24To16 loses the rounding 0.5|s/ \+ 0\.5\) as Fixed16/ + 0.0) as Fixed16/'
	'Invert24To16 numerator 2^40 -> 2^39|s/65536\.0 \* 16777216\.0/65536.0 * 8388608.0/'
	'FloorDivMod negative branch skips the q-- fixup|s/q -= 1;\n            r = \(denom/r = (denom/'
	'FloorDivMod negative remainder not mirrored|s/r = \(denom as c_int\) - r;/r = r;/'
	'FloorDivMod negative quotient not negated|s/let mut q = -\(x as c_int\);/let mut q = x as c_int;/'
	'FloorDivMod positive quotient rounds to nearest|s/let x = floor\(numer \/ denom\);/let x = floor(numer \/ denom + 0.5);/'
	'FloorDivMod negative branch fixup runs on r == 0|s/if r != 0 \{\n            q -= 1;/if true {\n            q -= 1;/'
	'R_ConcatRotations reads b transposed|s/\+ a\[i\]\[1\] \* b\[1\]\[j\]\n(\s+)\+ a\[i\]\[2\] \* b\[2\]\[j\];\n        \}\n    \}\n\}\n\n\/\/ --- R_ConcatTransforms/+ a[i][1] * b[j][1]\n$1+ a[i][2] * b[2][j];\n        }\n    }\n}\n\n\/\/ --- R_ConcatTransforms/'
	'R_ConcatTransforms drops the translation column|s/if j == 3 \{ a\[i\]\[3\] \}/if j == 3 { 0.0 }/'
	'R_ConcatTransforms adds the translation to every column|s/if j == 3 \{ a\[i\]\[3\] \}/if j >= 2 { a[i][3] }/'
	'BoxOnPlaneSide dist1 compare is > not >=|s/dist1 >= p\.dist/dist1 > p.dist/'
	'BoxOnPlaneSide dist2 compare is <= not <|s/dist2 < p\.dist/dist2 <= p.dist/'
	'BoxOnPlaneSide never sets the back bit|s/sides \|= 2;/sides |= 0;/'
	'BoxOnPlaneSide signbits 0 swaps min and max|s/0 => \(\n(\s+)p\.normal\[0\] \* emaxs_slice\[0\]/0 => (\n$1p.normal[0] * emins_slice[0]/'
	'BoxOnPlaneSide signbits 3 dist1 z uses min|s/(3 => \(\n\s+p\.normal\[0\] \* emins_slice\[0\] \+ p\.normal\[1\] \* emins_slice\[1\] \+ p\.normal\[2\] \* )emaxs_slice\[2\]/$1emins_slice[2]/'
	'BoxOnPlaneSide signbits 5 dist1 x uses max|s/(5 => \(\n\s+p\.normal\[0\] \* )emins_slice\[0\]/$1emaxs_slice[0]/'
	'BoxOnPlaneSide signbits 6 dist2 y uses max|s/(6 => \(.*?p\.normal\[0\] \* emins_slice\[0\] \+ p\.normal\[1\] \* )emaxs_slice\[1\](\s+\+ p\.normal\[2\] \* emaxs_slice\[2\],\n\s+\),)/$1emins_slice[1]$2/s'
	'BoxOnPlaneSide signbits 7 arms swapped|s/(7 => \(\n\s+)(p\.normal\[0\] \* emins_slice\[0\] \+ p\.normal\[1\] \* emins_slice\[1\] \+ p\.normal\[2\] \* emins_slice\[2\],\n\s+)(p\.normal\[0\] \* emaxs_slice\[0\] \+ p\.normal\[1\] \* emaxs_slice\[1\] \+ p\.normal\[2\] \* emaxs_slice\[2\],)/$1$3\n            $2/'
	'AngleVectors forward z sign dropped|s/forward_slice\[2\] = -sp;/forward_slice[2] = sp;/'
	'AngleVectors yaw reads the pitch angle|s/let yaw = angles_slice\[1\]/let yaw = angles_slice[0]/'
	'AngleVectors roll reads the yaw angle|s/let roll = angles_slice\[2\]/let roll = angles_slice[1]/'
	'AngleVectors right[0] sign dropped|s/right_slice\[0\] = -1\.0 \* \(sr \* sp \* cy \+ cr \* -sy\)/right_slice[0] = 1.0 * (sr * sp * cy + cr * -sy)/'
	'AngleVectors right[2] uses sp|s/right_slice\[2\] = -1\.0 \* \(sr \* cp\)/right_slice[2] = -1.0 * (sr * sp)/'
	'AngleVectors up[1] drops the roll term|s/up_slice\[1\] = cr \* sp \* sy \+ -sr \* cy/up_slice[1] = cr * sp * sy/'
	'AngleVectors up[2] uses sp|s/up_slice\[2\] = cr \* cp;/up_slice[2] = cr * sp;/'
	'AngleVectors degrees-to-radians factor 2pi/360 -> pi/360|s/M_PI \* 2\.0 \/ 360\.0/M_PI \/ 360.0/'
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
