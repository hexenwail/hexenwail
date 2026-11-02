#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the sv_main_inv differential harness can fail.  Applies each mutant
# to engine/rust/src/sv_main_inv.rs in turn, rebuilds the archive with
# sv_main_inv, runs run_inv_harness.sh without the coverage pass, and
# requires a MISMATCH -- a difference between the Rust and the C -- for every
# mutant except the ones marked EQUIVALENT, whose proofs are at the sites
# they mutate, and the CONTROL, which only edits a comment and must survive.
# The file is restored on exit.
#
#   bash engine/rust/sv_main/tests/inv_mutants.sh            # all mutants
#   bash engine/rust/sv_main/tests/inv_mutants.sh 12 30      # mutants 12 and 30
#
# Not part of check-rust-sv-main.sh (it rewrites a tracked source); run it
# when sv_main_inv.rs or the harness changes.  HARNESS_CASES sets the random
# case count per run (default 9000; every directed case runs regardless).
# Needs perl, cargo, cc -- run inside `nix develop`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/../.." && pwd)"
src="$crate/src/sv_main_inv.rs"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
orig="$work/sv_main_inv.rs.orig"
cp "$src" "$orig"
trap 'cp "$orig" "$src"' EXIT
export HARNESS_CASES="${HARNESS_CASES:-9000}"
export HARNESS_FAIL_FAST=1

# kind|name|perl -0 substitution.  kind is KILL (must be a MISMATCH),
# EQUIVALENT (must survive; proof at the site) or CONTROL (must survive).
# shellcheck disable=SC2016  # perl code, not shell: nothing should expand
mutants=(
	'CONTROL|a comment changes|s/end of worldspawn/end of the worldspawn/'
	# SV_ParseWorldspawnSky
	'KILL|skybox is not cleared|s/\n    \*skybox = 0;\n/\n/'
	'KILL|no NULL-world return|s/if world.is_null\(\) \{\n        return;/if false {\n        return;/'
	'KILL|no NULL-entities return|s/if entities.is_null\(\) \{\n        return;/if false {\n        return;/'
	'KILL|opening brace test inverted|s/\*token != b.\{. as c_char/*token == b'"'"'{'"'"' as c_char/'
	'KILL|closing brace is not the end|s/if \*token == b.\}. as c_char \{\n            return; \/\/ end/if *token == b'"'"'{'"'"' as c_char {\n            return; \/\/ end/'
	'KILL|underscore kept|s/token.add\(1\)/token.add(0)/'
	'KILL|dash stripped, not underscore|s/\*token == b._. as c_char/*token == b'"'"'-'"'"' as c_char/'
	'KILL|key buffer one byte short|s/q_strlcpy\(key.as_mut_ptr\(\), src, KEY_SIZE\)/q_strlcpy(key.as_mut_ptr(), src, KEY_SIZE - 1)/'
	'KILL|tabs trimmed, not spaces|s/key\[len - 1\] == b. . as c_char/key[len - 1] == b'"'"'\\t'"'"' as c_char/'
	'KILL|one trailing space trimmed|s/while key\[0\] != 0 && key\[len - 1\]/if key[0] != 0 \&\& key[len - 1]/'
	'KILL|a key with no value still matches|s/(data = COM_Parse\(data\);\n        if data.is_null\(\) \{\n            return;\n        \}\n\n        if key_is)/data = COM_Parse(data);\n        if false {\n            return;\n        }\n\n        if key_is/'
	'KILL|sky misspelled|s/b"sky\\0"/b"skz\\0"/'
	'KILL|skyname dropped|s/key_is\(&key, b"skyname\\0"\)/false/'
	'KILL|qlsky dropped|s/ \|\| key_is\(&key, b"qlsky\\0"\)//'
	'KILL|skybox copy one byte short|s/q_strlcpy\(skybox, token, f.skybox_size\)/q_strlcpy(skybox, token, f.skybox_size - 1)/'
	'KILL|key match needs every name|s/key_is\(&key, b"sky\\0"\) \|\|/key_is(\&key, b"sky\\0") \&\&/'
	# SV_ClientInventoryPage
	'KILL|NULL page array not returned|s/if pages.is_null\(\) \{\n        return core::ptr::null_mut\(\);\n    \}\n\n    let npages/if false {\n        return core::ptr::null_mut();\n    }\n\n    let npages/'
	'KILL|search bounded by maxclients alone|s/SvMain_MaxClients\(\).wrapping_mul\(f.max_inventory_ex_pages\)/SvMain_MaxClients()/'
	'KILL|search one page past|s/while i < npages/while i <= npages/'
	'KILL|owner test inverted|s/\*pint\(page, f.ex_client_id\) != clientnum/*pint(page, f.ex_client_id) == clientnum/'
	'KILL|in-use test is or|s/\*pint\(page, f.ex_id\) != 0 && \*pint\(page, f.ex_client_id\)/*pint(page, f.ex_id) != 0 || *pint(page, f.ex_client_id)/'
	'KILL|page ids advance by two|s/\*next_id = \(\*next_id\).wrapping_add\(1\);/*next_id = (*next_id).wrapping_add(2);/'
	'KILL|claims an owned page|s/if \*pint\(page, f.ex_id\) == 0 \{\n            let next_id/if *pint(page, f.ex_id) != 0 {\n            let next_id/'
	'KILL|claimed page keeps id 0|s/\n            \*pint\(page, f.ex_id\) = \*next_id;\n/\n/'
	# INV_UpdateExItem, the first walk
	'KILL|first walk keeps matching|s/(result = \*cnt;\n                found = true;\n)                break;\n/$1/'
	'KILL|first walk stops at the first page|s/(            i \+= 1;\n        \}\n)        page = \*next\(page\);\n/$1        page = core::ptr::null_mut();\n/'
	'KILL|inc only when 1|s/if inc != 0 \{\n                    \*cnt = \(\*cnt\).wrapping_add\(inv_cnt\);\n                \} else \{\n                    \*cnt = inv_cnt;\n                \}\n                \*pint\(page, f.ex_changed_items\) \|= bit\(i\);\n                result/if inc == 1 {\n                    *cnt = (*cnt).wrapping_add(inv_cnt);\n                } else {\n                    *cnt = inv_cnt;\n                }\n                *pint(page, f.ex_changed_items) |= bit(i);\n                result/'
	'KILL|first walk ignores inc|s/(if inc != 0 \{\n                    \*cnt = )\(\*cnt\).wrapping_add\(inv_cnt\)(;\n                \} else \{\n                    \*cnt = inv_cnt;\n                \}\n                \*pint\(page, f.ex_changed_items\) \|= bit\(i\);\n                result)/${1}inv_cnt$2/'
	'KILL|first walk marks the next slot|s/(\*pint\(page, f.ex_changed_items\) \|= )bit\(i\)(;\n                result = \*cnt;\n                found = true;\n                break;\n            \}\n            i \+= 1;\n        \}\n        page = \*next)/$1bit(i + 1)$2/'
	'KILL|result is the argument|s/result = \*cnt;\n                found = true;\n                break;\n            \}\n            i \+= 1;\n        \}\n        page = \*next/result = inv_cnt;\n                found = true;\n                break;\n            }\n            i += 1;\n        }\n        page = *next/'
	'KILL|first walk never finds|s/(result = \*cnt;\n)                found = true;\n(                break;\n            \}\n            i \+= 1;\n        \}\n        page = \*next)/$1$2/'
	'KILL|a zero count also inserts|s/if inv_cnt > 0 && !found/if inv_cnt >= 0 \&\& !found/'
	# INV_UpdateExItem, the insert
	'KILL|insert into a used slot|s/if \*slot\(page, f.ex_item_id, i\) == 0 \{/if *slot(page, f.ex_item_id, i) != 0 {/'
	'KILL|insert leaves the id|s/\n                    \*slot\(page, f.ex_item_id, i\) = inv_id;\n/\n/'
	'KILL|insert ignores inc|s/(\*slot\(page, f.ex_item_id, i\) = inv_id;\n                    let cnt = slot\(page, f.ex_item_cnt, i\);\n                    if inc != 0 \{\n                        \*cnt = )\(\*cnt\).wrapping_add\(inv_cnt\)/${1}inv_cnt/'
	'KILL|insert is not new|s/\n                    \*pint\(page, f.ex_new_items\) \|= bit\(i\);\n/\n/'
	'KILL|insert is not changed|s/\n                    \*pint\(page, f.ex_changed_items\) \|= bit\(i\);\n                    \*pint\(page, f.ex_new_items\)/\n                    *pint(page, f.ex_new_items)/'
	'KILL|insert does not stop the walk|s/\n            if found \{\n                break;\n            \}\n/\n/'
	'KILL|chain extended for the page, not the start|s/SV_ClientInventoryPage\(\*pint\(start_page, f.ex_client_id\)\)/SV_ClientInventoryPage(*pint(page, f.ex_client_id))/'
	'EQUIVALENT|out-of-pages break dropped|s/\n                    break; \/\/ out of pages\n/\n/'
	# SV_GetLevelname
	'KILL|message 0 is a string|s/if idx > 0 && idx <= SvMain_HostStringCount\(\)/if idx >= 0 \&\& idx <= SvMain_HostStringCount()/'
	'KILL|the last string unreachable|s/idx > 0 && idx <= SvMain_HostStringCount\(\)/idx > 0 \&\& idx < SvMain_HostStringCount()/'
	'KILL|strings indexed from 1|s/Host_GetString\(idx - 1\)/Host_GetString(idx)/'
	'KILL|message converted by Rust as|s/let idx = cvt_c\(\*\(v.add\(f.v_message\) as \*const f32\)\);/let idx = *(v.add(f.v_message) as *const f32) as c_int;/'
	'KILL|netname read from message|s/PR_GetString\(\*\(v.add\(f.v_netname\)/PR_GetString(*(v.add(f.v_message)/'
	# SV_SaveSpawnparms
	'KILL|serverflags converted by Rust as|s/= cvt_c\(\*SvMain_GlobalServerFlags\(\)\);/= *SvMain_GlobalServerFlags() as c_int;/'
	'KILL|serverflags not saved|s/\n    \*SvMain_ServerFlags\(\) = cvt_c\(\*SvMain_GlobalServerFlags\(\)\);\n/\n/'
	'KILL|host_client one client further|s/while i < maxclients \{\n        i \+= 1;\n        client = client.wrapping_add/while i <= maxclients {\n        i += 1;\n        client = client.wrapping_add/'
	'KILL|host_client left alone|s/\n    \*SvMain_HostClient\(\) = client;\n/\n/'
	# INV_WritePage
	'KILL|no next page is 0|s/let next_id = if next_page.is_null\(\) \{\n        -1/let next_id = if next_page.is_null() {\n        0/'
	'KILL|zero counts written|s/if id != 0 && cnt > 0 \{/if id != 0 \&\& cnt >= 0 {/'
	'KILL|negative ids dropped|s/if id != 0 && cnt > 0 \{/if id > 0 \&\& cnt > 0 {/'
	'KILL|item pairs spaced after|s/c" %i %i"/c"%i %i "/'
	'KILL|no newline|s/fprintf\(file, c"\\n".as_ptr\(\)\);/fprintf(file, c"".as_ptr());/'
	'KILL|page owner written for the client|s/        \*pint\(page, f.ex_id\),\n        next_id,\n        client_id,/        *pint(page, f.ex_id),\n        next_id,\n        *pint(page, f.ex_client_id),/'
	# SV_LoadInventory
	'KILL|MAX_CLIENTS pages refused|s/total > f.max_clients/total >= f.max_clients/'
	'KILL|zero pages refused|s/if total < 0 \|\|/if total <= 0 ||/'
	'KILL|Host_Error names another function|s/c"SV_LoadInventory".as_ptr\(\)/c"SV_LoadInv".as_ptr()/'
	'KILL|load search spans the whole array|s/while i < maxclients\n            &&/while i < maxclients * f.max_inventory_ex_pages\n            \&\&/'
	'KILL|load search owner test inverted|s/\*pint\(page_at\(pages, i\), f.ex_client_id\) != client_id/*pint(page_at(pages, i), f.ex_client_id) == client_id/'
	'KILL|loaded page not cleared|s/\n        core::ptr::write_bytes\(page, 0, f.ex_size\);\n/\n/'
	'EQUIVALENT|next_page_id raised on equal|s/if id > \*next_page_id \{/if id >= *next_page_id {/'
	'KILL|next_page_id not raised|s/\n            \*next_page_id = id;\n/\n/'
	'KILL|loaded page keeps its owner|s/\n        \*pint\(page, f.ex_client_id\) = client_id;\n/\n/'
	'KILL|carriage return does not end a line|s/while c != NEWLINE && c != RETURN \{/while c != NEWLINE {/'
	'KILL|bounds abort one item late|s/if item_idx >= f.max_inventory_ex \{/if item_idx > f.max_inventory_ex {/'
	'KILL|loaded items are not new|s/\n            \*pint\(page, f.ex_new_items\) \|= bit\(item_idx\);\n/\n/'
	'KILL|loaded counts land in the next slot|s/\*slot\(page, f.ex_item_cnt, item_idx\) = item_cnt;/*slot(page, f.ex_item_cnt, (item_idx + 1) % f.max_inventory_ex) = item_cnt;/'
	'KILL|an unscanned id reads 1|s/let mut id: c_int = 0;/let mut id: c_int = 1;/'
	'KILL|an unscanned count reads 1|s/let mut item_cnt: c_int = 0;/let mut item_cnt: c_int = 1;/'
	'KILL|item index never advances|s/\n            item_idx \+= 1;\n/\n/'
	'KILL|only the header char is read|s/(item_idx \+= 1;\n)            c = fgetc\(file\);/$1            c = c;/'
	# INV_SavePages
	'KILL|count the clients without pages|s/if !ex_inventory\(i\).is_null\(\) \{\n            j \+= 1;/if ex_inventory(i).is_null() {\n            j += 1;/'
	'KILL|Pages line has no newline|s/c"Pages: %i\\n"/c"Pages: %i "/'
	'KILL|pages numbered from 1|s/INV_WritePage\(file, page, i\);/INV_WritePage(file, page, i + 1);/'
	'KILL|the first client skipped|s/    i = 0;\n    while i < maxclients \{\n        let page/    i = 1;\n    while i < maxclients {\n        let page/'
)

# select by 1-based index, or all
if [ $# -gt 0 ]; then
	sel=("$@")
else
	sel=()
	for i in "${!mutants[@]}"; do sel+=("$((i + 1))"); done
fi

lib="$work/target/release/libengine_rs.a"
killed=0 survived=0 equivalent=0 bad=0
for n in "${sel[@]}"; do
	m="${mutants[$((n - 1))]}"
	kind="${m%%|*}"; rest="${m#*|}"
	name="${rest%%|*}"; sub="${rest#*|}"
	perl -0pe "$sub" "$orig" >"$src"
	if cmp -s "$src" "$orig"; then
		echo "BROKEN MUTANT #$n (no change): $name" >&2
		exit 2
	fi
	if ! cargo build --release --offline --manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target" --features sv_main_inv >"$work/build.log" 2>&1; then
		cat "$work/build.log" >&2
		echo "BROKEN MUTANT #$n (does not compile): $name" >&2
		exit 2
	fi
	rc=0
	HARNESS_SKIP_COVERAGE=1 RUST_LIB="$lib" OUT="$work/run" \
		bash "$here/run_inv_harness.sh" >"$work/run.log" 2>&1 || rc=$?
	if grep -q '^MISMATCH' "$work/run.log"; then
		verdict=killed
		why=$(grep -m1 '^MISMATCH' "$work/run.log")
	elif [ "$rc" -ne 0 ]; then
		verdict=other
		why=$(grep -m1 -E 'FAIL|error' "$work/run.log" || tail -1 "$work/run.log")
	else
		verdict=survived
		why=""
	fi
	case "$kind:$verdict" in
	KILL:killed) killed=$((killed + 1)); echo "killed     #$n: $name -- $why" ;;
	EQUIVALENT:survived) equivalent=$((equivalent + 1)); echo "equivalent #$n: $name (survives, as proved at the site)" ;;
	CONTROL:survived) echo "control    #$n: $name (survives)" ;;
	*) bad=$((bad + 1)); echo "UNEXPECTED #$n [$kind]: $name -- $verdict ${why}" ;;
	esac
done
cp "$orig" "$src"
echo "mutants: ${#sel[@]} run, $killed killed, $equivalent equivalent, $bad unexpected"
[ "$bad" -eq 0 ] || { echo "FAIL: $bad mutant(s) did not behave as declared" >&2; exit 1; }
echo "PASS"
