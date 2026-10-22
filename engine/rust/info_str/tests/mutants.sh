#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the frozen info_str fixtures and reference model can still fail.
# Applies each mutant to engine/rust/src/info_str.rs in turn, runs the kill
# command, and requires it to FAIL (kill) for every mutant.  The file is
# restored on every exit path.
#
#   bash engine/rust/info_str/tests/mutants.sh                 # kill command: the replay
#   bash engine/rust/info_str/tests/mutants.sh -- some cmd...  # any other kill command
#
# A custom kill command exits 0 when the mutant survives, 1 when it is killed
# and 2 when the mutated source does not build (printed, never a kill).
#
# Not part of check-rust-info-str.sh (it rewrites a tracked source); run it
# when the fixtures or info_str.rs change.  Needs perl and cargo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src/info_str.rs"
crate="$here/.."
orig="$(mktemp)"
errf="$(mktemp)"
cp "$src" "$orig"
# Restores the tracked file on every exit path, including a failed mutant.
trap 'cp "$orig" "$src"; rm -f "$orig" "$errf"' EXIT

# Default kill command: the release-profile replay (what the engine ships) must
# run and fail.  A mutant that merely fails to compile proves nothing, so that
# is an error (return 2, printed by the loop below), not a kill.  A mutant that
# loops forever (an Info_RemoveKey that removes nothing under
# Info_RemovePrefixedKeys' restart) is a kill: timeout exits 124.
replay() {
	local out
	if out=$(timeout 60 cargo test --release --offline --manifest-path "$crate/Cargo.toml" 2>&1); then return 0; fi
	if grep -q 'could not compile' <<<"$out"; then
		echo "BROKEN MUTANT (does not compile)" >&2; echo "$out" >&2; return 2
	fi
	return 1
}
if [ "${1:-}" = "--" ]; then shift; kill_cmd=("$@"); else kill_cmd=(replay); fi

# name|perl -0 substitution (applied to info_str.rs).  Every pattern is
# anchored on code, never on the doc comments that quote it.
#
# Left out as equivalent: `if l < 20` -> `if l <= 20` in Info_Print (at
# l == 20 the pad is zero bytes and key[20] = 0 is the terminator already
# written), and `(*var).integer` -> `((*var).integer != 0) as c_int` (the only
# use is `== 0`).
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'key/value limit 63 -> 64|s/const MAX_KEYVALUE: usize = 63;/const MAX_KEYVALUE: usize = 64;/'
	'key/value limit 63 -> 62|s/const MAX_KEYVALUE: usize = 63;/const MAX_KEYVALUE: usize = 62;/'
	'value length not checked|s/ \|\| strlen\(value\) > MAX_KEYVALUE//'
	'key length not checked|s/if strlen\(key\) > MAX_KEYVALUE \|\| strlen\(value\)/if strlen(value)/'
	'maxsize check >= becomes >|s/strlen\(s\) >= maxsize/strlen(s) > maxsize/'
	'maxsize check forgets the second separator|s/strlen\(value\) \+ 1 \+ strlen\(s\)/strlen(value) + strlen(s)/'
	'maxsize check forgets the first separator|s/strlen\(key\) \+ 1 \+ strlen\(value\)/strlen(key) + strlen(value)/'
	'rotation has three slots|s/INFO_VALUE_INDEX \+ 1\) % 4;/INFO_VALUE_INDEX + 1) % 3;/'
	'rotation never advances|s/INFO_VALUE_INDEX = \(INFO_VALUE_INDEX \+ 1\) % 4;/INFO_VALUE_INDEX = INFO_VALUE_INDEX % 4;/'
	'rotation advances only on a hit|s/(    INFO_VALUE_INDEX = \(INFO_VALUE_INDEX \+ 1\) % 4;\n)//;s/(        if strcmp\(key, pkey.as_ptr\(\)\) == 0 \{\n)(            return value;)/$1            INFO_VALUE_INDEX = (INFO_VALUE_INDEX + 1) % 4;\n$2/'
	'every lookup shares slot 0|s/INFO_VALUE\[INFO_VALUE_INDEX\]\)/INFO_VALUE[0])/'
	'lookup does not skip a leading separator|s/(    INFO_VALUE_INDEX = [^\n]*\n\n)    if \*s == BACKSLASH \{\n        s = s.add\(1\);\n    \}\n/$1/'
	'lookup returns on a key mismatch|s/if strcmp\(key, pkey.as_ptr\(\)\) == 0 \{\n            return value;/if strcmp(key, pkey.as_ptr()) != 0 {\n            return value;/'
	'removal memmove drops the NUL|s/strlen\(s\) \+ 1,\n            \);/strlen(s),\n            );/'
	'removal keeps the separator before the pair|s/start\.cast::<c_void>\(\),/start.add(1).cast::<c_void>(),/'
	'removal moves from one byte early|s/s\.cast::<c_void>\(\),\n                strlen/s.sub(1).cast::<c_void>(),\n                strlen/'
	'removal accepts a backslash in the key|s/if !strstr\(key, c"\\\\"\.as_ptr\(\)\)\.is_null\(\) \{\n        CON_Printf\(\n            PRINT_NORMAL,\n            c"Can.t use a key with/if false {\n        CON_Printf(\n            PRINT_NORMAL,\n            c"Can\x27t use a key with/'
	'prefixed removal matches != prefix|s/if pkey\[0\] == prefix \{/if pkey[0] != prefix {/'
	'prefixed removal stops after one key|s/(Info_RemoveKey\(start, pkey\.as_ptr\(\)\);\n            s = start;\n)/$1            if true { return; }\n/'
	'prefixed removal does not restart|s/Info_RemoveKey\(start, pkey\.as_ptr\(\)\);\n            s = start;/Info_RemoveKey(start, pkey.as_ptr());/'
	'Info_SetValueForKey allows * keys|s/if \*key == b\x27\*\x27 as c_char \{/if false {/'
	'Info_SetValueForKey rejects _ keys instead|s/if \*key == b\x27\*\x27 as c_char \{/if *key == b\x27_\x27 as c_char {/'
	'Info_SetValueForKey caps maxsize by one|s/Info_SetValueForStarKey\(s, key, value, maxsize\);\n\}/Info_SetValueForStarKey(s, key, value, maxsize - 1);\n}/'
	'empty value does not delete the key|s/if \*value == 0 \{\n        return;/if false {\n        return;/'
	'set does not remove the old key first|s/    Info_RemoveKey\(s, key\);\n    if \*value == 0/    let _ = key;\n    if *value == 0/'
	'set overwrites instead of appending|s/let mut s = s\.add\(strlen\(s\)\);/let mut s = s;/'
	'value copy truncated at 62 bytes|s/let vallen = strlen\(value\);/let vallen = strlen(value).min(62);/'
	'quote check removed|s/if !strstr\(key, c"\\""\.as_ptr\(\)\)\.is_null\(\) \|\| !strstr\(value, c"\\""\.as_ptr\(\)\)\.is_null\(\) \{/if false {/'
	'backslash in the value allowed|s/if !strstr\(key, c"\\\\"\.as_ptr\(\)\)\.is_null\(\) \|\| !strstr\(value, c"\\\\"\.as_ptr\(\)\)\.is_null\(\) \{/if !strstr(key, c"\\\\".as_ptr()).is_null() {/'
	'sv_highchars sense inverted|s/if highchars == 0 \{/if highchars != 0 {/'
	'filter keeps byte 13|s/if c > 13 \{/if c >= 13 {/'
	'filter drops byte 14|s/if c > 13 \{/if c > 14 {/'
	'filter lower bound 31 not 32|s/if c < 32 \|\| c > 127/if c < 31 || c > 127/'
	'high bit not stripped|s/c &= 127;/c \&= 255;/'
	'unregistered sv_highchars reads as on|s/if var\.is_null\(\) \{\n        0/if var.is_null() {\n        1/'
	'reads another cvar|s/c"sv_highchars"/c"sv_highchar"/'
	'print pads to 19 columns|s/\(20 - l\) as usize/(19 - l) as usize/'
	'print drops the MISSING VALUE text|s/c"MISSING VALUE\\n"/c"MISSING\\n"/'
	'print does not skip a leading separator|s/(    let mut s = s;\n\n)    if \*s == BACKSLASH \{\n        s = s\.add\(1\);\n    \}\n\n    while \*s != 0 \{/$1    while *s != 0 {/'
	'print does not skip the value separator|s/        if \*s != 0 \{\n            s = s\.add\(1\);\n        \}\n        CON_Printf\(PRINT_NORMAL, c"%s\\n"/        CON_Printf(PRINT_NORMAL, c"%s\\n"/'
	'CON_Printf flags are not _PRINT_NORMAL|s/const PRINT_NORMAL: c_uint = 0;/const PRINT_NORMAL: c_uint = 1;/'
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
