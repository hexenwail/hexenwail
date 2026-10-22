#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen strlcpy fixtures can still fail.  Applies each mutant to
# engine/rust/src/strlcpy.rs in turn, runs the kill command, and requires it to
# FAIL (kill) for every mutant.  The file is restored on exit.
#
#   bash engine/rust/strlcpy/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/strlcpy/tests/mutants.sh -- some cmd...  # any other kill command
#
# Not part of check-rust-strlcpy.sh (it rewrites a tracked source); run it when
# the fixtures or strlcpy.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src/strlcpy.rs"
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

# name|perl -0 substitution (applied to strlcpy.rs)
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'copy loop stops one byte early|s/if n == 0 \{\n                break;/if n <= 1 {\n                break;/'
	'size treated as siz + 1|s/let mut n = siz;/let mut n = siz.wrapping_add(1);/'
	'copy loop ignores the source NUL|s/if byte == 0 \{/if byte == 7 {/'
	'terminator is not NUL|s/\*d = 0;/*d = 1;/'
	'size 0 still writes the terminator|s/if siz != 0 \{\n            \*d = 0;/if true {\n            *d = 0;/'
	'size 1 skips the terminator|s/if n != 0 \{\n        loop/if n > 1 {\n        loop/'
	'terminator also clobbers the last copied byte|s/\*d = 0;/*d = 0;\n            if siz > 2 { *d.sub(1) = 0; }/'
	'terminator written one byte too far|s/\*d = 0;/*d = 0;\n            *d.add(1) = 0;/'
	'destination pointer never advances|s/d = d.add\(1\);/d = d.add(0);/'
	'return value is the truncated length|s/s\.offset_from\(src\) as usize - 1/(s.offset_from(src) as usize - 1).min(siz.saturating_sub(1))/'
	'return value off by one|s/s\.offset_from\(src\) as usize - 1/s.offset_from(src) as usize/'
	'return value excludes the NUL scan|s/s\.offset_from\(src\) as usize - 1\n/s.offset_from(src) as usize - 1 - (n == 0) as usize\n/'
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
