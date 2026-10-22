#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen strlcat fixtures can still fail.  Applies each mutant to
# engine/rust/src/strlcat.rs in turn, runs the kill command, and requires it to
# FAIL (kill) for every mutant.  The file is restored on exit.
#
#   bash engine/rust/strlcat/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/strlcat/tests/mutants.sh -- some cmd...  # any other kill command
#
# Not part of check-rust-strlcat.sh (it rewrites a tracked source); run it when
# the fixtures or strlcat.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src/strlcat.rs"
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

# name|perl -0 substitution (applied to strlcat.rs)
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
# Equivalent mutant, deliberately absent: `n <= 1` for `n == 0` on the no-room
# return.  n == 1 means `d` already points at the NUL that ends dst, so the final
# `*d = 0` rewrites the byte that is already 0 and the return value is the same.
mutants=(
	'scan stops one byte early|s/let more = n != 0;/let more = n > 1;/'
	'scan ignores the siz bound for siz 0 (reads NULL)|s/let more = n != 0;/let more = n != 0 || siz == 0;/'
	'scan stops on the wrong byte|s/if !more \|\| \*d == 0 \{/if !more || *d == 1 {/'
	'scan never advances the destination|s/d = d.add\(1\);\n        dlen \+= 1;/d = d.add(0);\n        dlen += 1;/'
	'scan does not count dlen|s/d = d.add\(1\);\n        dlen \+= 1;/d = d.add(1);\n        dlen += 0;/'
	'room is one byte too large|s/n = siz - dlen;/n = siz - dlen + 1;/'
	'no-room return skipped for siz > 4 only|s/if n == 0 \{\n        return/if n == 0 \&\& siz > 4 {\n        return/'
	'no-room return drops the source length|s/return dlen \+ c_strlen\(s\);/return dlen;/'
	'no-room return is one short|s/return dlen \+ c_strlen\(s\);/return dlen + c_strlen(s) - (c_strlen(s) > 0) as usize;/'
	'truncates one byte early|s/if n != 1 \{/if n != 2 {/'
	'copied byte is not the source byte|s/\*d = \*s;/*d = *s ^ 1;/'
	'destination pointer never advances on copy|s/d = d.add\(1\);\n            n -= 1;/d = d.add(0);\n            n -= 1;/'
	'terminator is not NUL|s/\*d = 0;\n\n/*d = 1;\n\n/'
	'terminator skipped for a non-empty dst|s/\*d = 0;\n\n/if dlen == 0 { *d = 0; }\n\n/'
	'terminator written one byte too far|s/\*d = 0;\n\n/*d = 0;\n    *d.add(1) = 0;\n\n/'
	'return drops the initial dst length|s/dlen \+ s\.offset_from\(src\) as usize/s.offset_from(src) as usize/'
	'return counts the NUL|s/dlen \+ s\.offset_from\(src\) as usize/dlen + s.offset_from(src) as usize + 1/'
	'return is the truncated length|s/dlen \+ s\.offset_from\(src\) as usize/(dlen + s.offset_from(src) as usize).min(siz - 1)/'
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
