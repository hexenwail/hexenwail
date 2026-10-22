#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen hashindex fixtures and the reference model can still fail.
# Applies each mutant to engine/rust/hashindex/src/ffi.rs in turn, runs the kill
# command, and requires it to FAIL (kill) for every mutant.  The file is
# restored on every exit path.
#
#   bash engine/rust/hashindex/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/hashindex/tests/mutants.sh -- some cmd...  # any other kill command
#
# A kill command exits 0 when the mutant survives, 1 when it dies, and 2 when
# the mutant does not compile (a broken mutant proves nothing: it is an error).
#
# The compile-time layout asserts are stripped from the layout mutant, so the
# layout record has to kill it, not the compiler.
#
# Equivalent mutants left out on purpose:
#  - Hash_Remove computing `k` before or after the NULL check: the order is
#    unobservable, both paths reach the same Sys_Error.
#  - Hash_Remove's `hash[k] == index` head test: the chain walk below it would
#    find the same head, so dropping it only changes a branch, not a result.
#
# Not part of check-rust-hashindex.sh (it rewrites a tracked source); run it when
# the fixtures or ffi.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src/ffi.rs"
crate="$here/../replay"
orig="$(mktemp)"
errf="$(mktemp)"
cp "$src" "$orig"
# One trap for every exit path, including a broken mutant and a failed kill command.
trap 'cp "$orig" "$src"; rm -f "$orig" "$errf"' EXIT

# Default kill command: the release-profile replay (what the engine ships) must
# run and fail.  A mutant that merely fails to compile proves nothing, so that is
# an error (return 2, printed by the loop below), not a kill.
replay() {
	local out
	# A mutant that loops forever is a kill: timeout exits 124.
	if out=$(timeout 300 cargo test --release --offline --manifest-path "$crate/Cargo.toml" 2>&1); then
		return 0
	fi
	if grep -q 'could not compile' <<<"$out"; then
		echo "BROKEN MUTANT (does not compile)" >&2
		echo "$out" >&2
		return 2
	fi
	return 1
}
if [ "${1:-}" = "--" ]; then shift; kill_cmd=("$@"); else kill_cmd=(replay); fi

# name|perl -0 substitution applied to ffi.rs
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'Allocate accepts size 0|s/x > 0 && \(x & \(x - 1\)\) == 0/x >= 0 \&\& (x \& (x - 1)) == 0/'
	'Allocate accepts 2^n - 1|s/\(x & \(x - 1\)\) == 0/(x \& (x + 1)) == 0/'
	'Allocate re-init guard dropped|s/if !\(\*hi\)\.hash\.is_null\(\) \{/if false \&\& !(*hi).hash.is_null() {/'
	'Allocate checks size only on a fresh table|s/if !is_power_of_two\(hash_size\) \{/if (*hi).hash.is_null() \&\& !is_power_of_two(hash_size) {/'
	'Allocate fills indexChain with 0, not -1|s/(index_chain\.cast::<c_void>\(\), )-1/${1}0/'
	'Allocate mask is half the table|s/hash_mask = hash_size - 1;/hash_mask = (hash_size - 1) >> 1;/'
	'Free keeps the hash pointer|s/\n        \(\*hi\)\.hash = core::ptr::null_mut\(\);//'
	'Free keeps the indexChain pointer|s/\n        \(\*hi\)\.index_chain = core::ptr::null_mut\(\);//'
	'Free zeroes hashMask|s/(\(\*hi\)\.hash = core::ptr::null_mut\(\);)/$1\n        (*hi).hash_mask = 0;/'
	'Add does not link to the old head|s/\*chain\.offset\(index as isize\) = \*hash\.offset\(h\);/*chain.offset(index as isize) = NULL_INDEX;/'
	'Add never updates the head|s/\*hash\.offset\(h\) = index;/*hash.offset(h) = *hash.offset(h);/'
	'Add range check is index > size|s/index >= \(\*hi\)\.hash_size \{/index > (*hi).hash_size {/'
	'Add accepts index -1|s/if index < 0 \|\|/if index < -1 ||/'
	'Add buckets by key >> 1|s/let h = \(key & \(\*hi\)\.hash_mask\) as isize;/let h = ((key >> 1) \& (*hi).hash_mask) as isize;/'
	'Add NULL guard dropped|s/if \(\*hi\)\.hash\.is_null\(\) \{\n        Sys_Error\(c"Hash_Add/if false {\n        Sys_Error(c"Hash_Add/'
	'Remove buckets with half the mask|s/let k = \(key & \(\*hi\)\.hash_mask\) as isize;/let k = (key \& ((*hi).hash_mask >> 1)) as isize;/'
	'Remove head promotes -1, not the next link|s/\*hash\.offset\(k\) = \*chain\.offset\(index as isize\);/*hash.offset(k) = NULL_INDEX;/'
	'Remove relink drops the tail|s/\*chain\.offset\(i as isize\) = \*chain\.offset\(index as isize\);/*chain.offset(i as isize) = NULL_INDEX;/'
	'Remove walk stops after one link|s/i = \*chain\.offset\(i as isize\);/i = NULL_INDEX;/'
	'Remove leaves the removed chain slot stale|s/\n    \}\n    \*chain\.offset\(index as isize\) = NULL_INDEX;\n\}/\n    }\n}/'
	'Remove NULL guard dropped|s/if \(\*hi\)\.hash\.is_null\(\) \{\n        Sys_Error\(c"Hash_Remove/if false {\n        Sys_Error(c"Hash_Remove/'
	'Clear clears half the table|s/(memset\(\(\*hi\)\.hash\.cast::<c_void>\(\), -1, )bytes\);\n\}/${1}bytes \/ 2);\n}/'
	'Clear also clears indexChain|s/(memset\(\(\*hi\)\.hash\.cast::<c_void>\(\), -1, bytes\);)\n\}/$1\n    memset((*hi).index_chain.cast::<c_void>(), -1, bytes);\n}/'
	'Clear NULL guard dropped|s/if \(\*hi\)\.hash\.is_null\(\) \{\n        return;/if false {\n        return;/'
	'Layout swaps hash and indexChain (asserts stripped)|s/const _: \(\) = \{.*?\n\};\n//s; s/(pub hash: \*mut c_int,\n)(\s+pub index_chain: \*mut c_int,\n)/$2$1/'
)

survived=0
broken=0
for m in "${mutants[@]}"; do
	name="${m%%|*}"
	sub="${m#*|}"
	perl -0pe "$sub" "$orig" >"$src"
	if cmp -s "$src" "$orig"; then
		echo "BROKEN MUTANT (no change): $name" >&2
		exit 2
	fi
	rc=0
	"${kill_cmd[@]}" >/dev/null 2>"$errf" || rc=$?
	if [ "$rc" -eq 2 ]; then
		cat "$errf" >&2
		echo "BROKEN MUTANT (kill command exited 2): $name" >&2
		broken=$((broken + 1))
		break
	fi
	if [ "$rc" -eq 0 ]; then
		echo "SURVIVED: $name"
		survived=$((survived + 1))
	else
		echo "killed:   $name"
	fi
done
cp "$orig" "$src"
[ "$broken" -eq 0 ] || exit 2
[ "$survived" -eq 0 ] || { echo "FAIL: $survived mutant(s) survived" >&2; exit 1; }
echo "PASS: all ${#mutants[@]} mutants killed"
