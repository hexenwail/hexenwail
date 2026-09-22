#!/usr/bin/env bash
#
# check-rust-notices.sh -- every Rust port carries its C original's notices.
#
# The project decision (GitHub #240), made once for the whole of engine/rust:
# a Rust file translated from C code is a derivative of that code, so it
# reproduces the original's copyright notices in its own header, beside the
# SPDX tag.  The C original staying in the tree is not the attribution
# mechanism: the engine no longer compiles most of those files, and a notice
# that ships only in a file the build ignores is one refactor from deletion.
#
# For each derived file listed in the map below, this checks:
#
#   1. every holder line in any comment of the C original -- a line starting
#      `Copyright` or `Portions Copyright` in any case, or `(c) <year>` --
#      appears as a whole line of the Rust file's leading `//` comment
#      (whitespace runs collapsed), so a holder cannot be dropped, padded or
#      buried in prose;
#   2. the Rust header carries `Copyright (C) <years> Hexenwail contributors.`;
#   3. if the C licence is not the GPL -- today, the ISC notice on
#      common/strlcpy.c and common/strlcat.c -- the licence block's whole
#      permission and disclaimer text after its holder lines appears too,
#      because that licence requires the notice itself in every copy, whatever
#      licence the port is distributed under.  For the GPL the SPDX tag stands
#      in for the boilerplate, as it does across engine/rust;
#   4. the Rust file's SPDX identifier is one the C licence allows:
#      GPL-2.0-or-later or GPL-3.0-or-later for GPLv2-or-later, only
#      GPL-3.0-or-later for GPLv3-or-later, and for ISC either GPL tag or ISC
#      itself.  The project tags the ISC ports GPL-2.0-or-later, so the crate
#      reads as GPL throughout; ISC permits that, and check 3 still holds
#      them to the full notice.  The licence is read from the
#      comment block holding the first holder line; LGPL, AGPL and anything
#      else unrecognised fail rather than being guessed at;
#   5. every .rs under a src/ directory of engine/rust, at any depth, is either
#      in the map or listed as original Hexenwail code, so a new port cannot
#      land without being registered here.
#
# A holder statement that wraps onto a second line in the C is compared line by
# line; none does today, and one that did would fail check 1 loudly, not pass.
#
# Usage: check-rust-notices.sh [--self-test]
#
#   --self-test  run the checks on a scratch copy of the tree, then break the
#                copy in each way the checks exist to catch and require every
#                breakage to fail.
#
# Requires only bash and awk; no build.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

self_test=0
for arg in "$@"; do
	case "$arg" in
		--self-test) self_test=1 ;;
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Rust file (relative to the repo root) -> the C file it was translated from.
derived_map() {
	cat <<-'EOF'
	engine/rust/src/crc.rs                common/crc.c
	engine/rust/src/huffman.rs            engine/hexenworld/shared/huffman.c
	engine/rust/src/info_str.rs           engine/hexenworld/shared/info_str.c
	engine/rust/src/link_ops.rs           engine/h2shared/link_ops.c
	engine/rust/src/msg_io.rs             engine/h2shared/msg_io.c
	engine/rust/src/sizebuf.rs            engine/h2shared/sizebuf.c
	engine/rust/src/strlcat.rs            common/strlcat.c
	engine/rust/src/strlcpy.rs            common/strlcpy.c
	engine/rust/src/wad.rs                engine/h2shared/wad.c
	engine/rust/hashindex/src/lib.rs      engine/h2shared/hashindex.c
	engine/rust/hashindex/src/ffi.rs      engine/h2shared/hashindex.c
	engine/rust/mathlib/src/lib.rs        engine/h2shared/mathlib.c
	engine/rust/mathlib/src/ffi.rs        engine/h2shared/mathlib.c
	EOF
}

# Rust sources that translate no C file: the crate root that wires the ports
# together.  (build.rs reads hufffreq.h as data at build time and is outside
# the src/ directories this gate walks.)
original_files() {
	cat <<-'EOF'
	engine/rust/src/lib.rs
	EOF
}

# Every comment line of a C file -- all /* ... */ blocks and whole-line //
# comments, not just the first licence block -- as "<block>\t<text>" with the
# decoration stripped.  Each // line is its own block.
c_comments() {
	awk '
		BEGIN { OFS = "\t" }
		!inblk && /\/\*/ { inblk = 1; blk++; first = 1 }
		inblk {
			# Test the raw line: stripping the leading " * " first would
			# eat the star of a closing " */" and run the block to EOF.
			ended = ($0 ~ /\*\//)
			line = $0
			sub(/[ \t]*\*\/.*$/, "", line)
			if (first) sub(/^.*\/\*[ \t]?/, "", line)
			else sub(/^[ \t]*\*?[ \t]?/, "", line)
			first = 0
			print blk, line
			if (ended) inblk = 0
			next
		}
		/^[ \t]*\/\// { blk++; line = $0; sub(/^[ \t]*\/\/+[ \t]?/, "", line); print blk, line }
	' "$1"
}

# A holder line: "Copyright" (any case, optionally "Portions ") followed by a
# (C) or a year, or a bare "(c) <year>".  The year-or-(C) part is what keeps
# the ISC prose out: its text wraps so that a line starts "copyright notice
# and this permission notice ...", which an earlier draft took for a holder
# and so cut the permission sentence out of check 3.
holder_re='^(portions[[:space:]]+)?copyright[[:space:]]+(\([cC]\)|[0-9])|^\([cC]\)[[:space:]]*[0-9]'

# The leading run of // lines of a Rust file, with the // stripped.
rs_header() {
	awk '/^\/\// { sub(/^\/\/ ?/, ""); print; next } { exit }' "$1"
}

# Join lines with single spaces and collapse all whitespace runs.
flat() {
	tr '\n\t' '  ' | tr -s ' ' | sed 's/^ //; s/ $//'
}

# Collapse whitespace within each line, keeping the lines.
flat_lines() {
	tr '\t' ' ' | tr -s ' ' | sed 's/^ //; s/ $//'
}

# Run every check against the tree at $1.  Prints one line per failure and
# returns non-zero if there was any.
check_tree() {
	local tree=$1 bad=0 rs c comments holders lic_blk notice lic header hlines hflat
	local line spdx allowed needs_notice perm
	local -A registered=()

	while read -r rs c; do
		registered[$rs]=1
		if [ ! -f "$tree/$rs" ]; then
			echo "FAIL: $rs is in the map but does not exist"
			bad=1; continue
		fi
		if [ ! -f "$tree/$c" ]; then
			echo "FAIL: $rs maps to $c, which does not exist"
			bad=1; continue
		fi
		comments=$(c_comments "$tree/$c")
		holders=$(printf '%s\n' "$comments" | cut -f2- | flat_lines | grep -Ei "$holder_re" || true)
		if [ -z "$holders" ]; then
			echo "FAIL: $c has no copyright line in any comment"
			bad=1; continue
		fi
		# The licence text is the block holding the first holder line.
		# (The regex goes through ENVIRON, not -v: -v processes escapes and
		# would turn \( into a group.)
		lic_blk=$(printf '%s\n' "$comments" | HOLDER_RE=$holder_re awk -F'\t' '
			{ t = $2; gsub(/[ \t]+/, " ", t); sub(/^ /, "", t) }
			tolower(t) ~ ENVIRON["HOLDER_RE"] { print $1; exit }')
		notice=$(printf '%s\n' "$comments" | awk -F'\t' -v b="$lic_blk" '$1 == b' | cut -f2-)
		lic=$(printf '%s\n' "$notice" | flat)
		header=$(rs_header "$tree/$rs")
		hlines=$(printf '%s\n' "$header" | flat_lines)
		hflat=$(printf '%s\n' "$header" | flat)

		# 1. every upstream holder, each as a whole header line
		while IFS= read -r line; do
			if ! printf '%s\n' "$hlines" | grep -Fxq -- "$line"; then
				echo "FAIL: $rs header lacks the line \"$line\" (from $c)"
				bad=1
			fi
		done <<<"$holders"

		# 2. the port's own line
		if ! printf '%s\n' "$hlines" |
				grep -Eq '^Copyright \(C\) [0-9]{4}(-[0-9]{4})? Hexenwail contributors\.$'; then
			echo "FAIL: $rs header lacks \"Copyright (C) <year> Hexenwail contributors.\""
			bad=1
		fi

		# 4. licence of the original, and what the port may be tagged
		spdx=$(sed -n '1s|^// SPDX-License-Identifier: ||p' "$tree/$rs")
		allowed='' needs_notice=0
		case "$lic" in
			*Affero*|*Lesser*|*"Library General"*) ;;
			*"GNU General Public License"*"version 3 of the License"*"any later version"*)
				allowed="GPL-3.0-or-later" ;;
			*"GNU General Public License"*"version 2 of the License, or"*"any later version"*)
				allowed="GPL-2.0-or-later GPL-3.0-or-later" ;;
			*"Permission to use, copy, modify, and distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies."*)
				allowed="GPL-2.0-or-later GPL-3.0-or-later ISC" needs_notice=1 ;;
		esac
		if [ -z "$allowed" ]; then
			echo "FAIL: $c carries a licence this gate does not recognise;"
			echo "      teach check-rust-notices.sh what it requires before porting it"
			bad=1; continue
		fi
		if [[ " $allowed " != *" $spdx "* ]]; then
			echo "FAIL: $rs is tagged SPDX \"${spdx:-<none>}\" on line 1, but $c allows only: $allowed"
			bad=1
		fi

		# 3. the full notice, for a licence that requires it in every copy:
		#    the licence block's text after its last holder line.  Empty text
		#    would match anything, so it is a failure, not a pass.
		if [ "$needs_notice" -eq 1 ]; then
			perm=$(printf '%s\n' "$notice" | flat_lines | HOLDER_RE=$holder_re awk '
				tolower($0) ~ ENVIRON["HOLDER_RE"] { n = NR } { l[NR] = $0 }
				END { for (i = n + 1; i <= NR; i++) print l[i] }' | flat)
			if [ -z "$perm" ]; then
				echo "FAIL: $c has no permission text after its copyright lines to check $rs against"
				bad=1
			elif [[ "$hflat" != *"$perm"* ]]; then
				echo "FAIL: $rs header does not reproduce the permission notice and disclaimer of $c"
				bad=1
			fi
		fi
	done < <(derived_map)

	while read -r rs; do
		registered[$rs]=1
		[ -f "$tree/$rs" ] || { echo "FAIL: $rs is listed as original but does not exist"; bad=1; }
	done < <(original_files)

	# 5. nothing unregistered, at any depth (a src/net/mod.rs counts)
	if [ ! -d "$tree/engine/rust/src" ]; then
		echo "FAIL: engine/rust/src does not exist"
		return 1
	fi
	while IFS= read -r rs; do
		rs=${rs#"$tree/"}
		if [ -z "${registered[$rs]:-}" ]; then
			echo "FAIL: $rs is not in check-rust-notices.sh -- add it to the map with"
			echo "      the C file it translates, or to original_files if it translates none"
			bad=1
		fi
	done < <(find "$tree/engine/rust" -name target -prune -o -type f -name '*.rs' -path '*/src/*' -print | sort)

	return "$bad"
}

if [ "$self_test" -eq 0 ]; then
	echo "== upstream notices in the Rust ports =="
	if ! check_tree "$root"; then
		echo "FAIL: see above" >&2
		exit 1
	fi
	echo "PASS: $(derived_map | wc -l) derived files carry their C originals' notices;"
	echo "      $(original_files | wc -l) original file(s); nothing unregistered."
	exit 0
fi

# --self-test
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

fresh_copy() {
	rm -rf "$scratch/tree"
	mkdir -p "$scratch/tree"
	local f
	while read -r f; do
		mkdir -p "$scratch/tree/$(dirname "$f")"
		cp "$root/$f" "$scratch/tree/$f"
	done < <({ derived_map | awk '{ print $1; print $2 }'; original_files; } | sort -u)
}

t_fail=0
# expect pass NAME
# expect fail NAME PATTERN
#
# A breakage counts as caught only if the gate fails and every FAIL line it
# prints matches PATTERN (grep -E).  Without that, every case would go green
# on any unrelated failure in the copy -- which is what the first draft of
# this self-test did.  (More than one line is allowed: hashindex.c backs two
# Rust files, so breaking it fails both.)
expect() {
	local want=$1 name=$2 pattern=${3:-} out status nfail nmatch
	set +e
	out=$(check_tree "$scratch/tree" 2>&1)
	status=$?
	set -e
	nfail=$(printf '%s\n' "$out" | grep -c '^FAIL' || true)
	nmatch=$(printf '%s\n' "$out" | grep '^FAIL' | grep -Ec -- "$pattern" || true)
	if [ "$want" = pass ] && [ "$status" -eq 0 ]; then
		echo "  ok   $name"
	elif [ "$want" = fail ] && [ "$status" -ne 0 ] && [ "$nfail" -ge 1 ] &&
			[ "$nmatch" -eq "$nfail" ]; then
		echo "  ok   $name"
		echo "         ${out%%$'\n'*}"
	else
		echo "  FAIL $name: expected $want${pattern:+ matching /$pattern/}, got exit $status" >&2
		[ -n "$out" ] && printf '%s\n' "$out" >&2
		t_fail=1
	fi
}

echo "== self-test: the gate passes the real tree and fails each breakage =="

fresh_copy
expect pass "unmodified copy of the tree"

fresh_copy
sed -i '/Todd C. Miller/d' "$scratch/tree/engine/rust/src/strlcpy.rs"
expect fail "ISC copyright holder dropped from strlcpy.rs" \
	'strlcpy\.rs header lacks the line "Copyright \(c\) 1998 Todd C\. Miller'

fresh_copy
sed -i '/MERCHANTABILITY AND FITNESS/d' "$scratch/tree/engine/rust/src/strlcat.rs"
expect fail "one line of the ISC disclaimer dropped from strlcat.rs" \
	'strlcat\.rs header does not reproduce the permission notice'

fresh_copy
sed -i '/Raven Software/d' "$scratch/tree/engine/rust/src/msg_io.rs"
expect fail "second of two GPL holders dropped from msg_io.rs" \
	'msg_io\.rs header lacks the line "Copyright \(C\) 1997-1998 Raven Software Corp\."'

fresh_copy
sed -i 's/Copyright (C) 1996-1997  Id Software, Inc./Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company./' \
	"$scratch/tree/engine/rust/mathlib/src/lib.rs"
expect fail "mathlib lib.rs reattributed to the Doom 3 holder (the #240 defect)" \
	'mathlib/src/lib\.rs header lacks the line "Copyright \(C\) 1996-1997 Id Software, Inc\."'

fresh_copy
sed -i '/Hexenwail contributors/d' "$scratch/tree/engine/rust/src/wad.rs"
expect fail "Hexenwail contributors line dropped from wad.rs" \
	'wad\.rs header lacks "Copyright \(C\) <year> Hexenwail contributors\."'

fresh_copy
sed -i '1s/GPL-2.0-or-later/ISC/' "$scratch/tree/engine/rust/src/crc.rs"
expect fail "GPL port retagged ISC" \
	'crc\.rs is tagged SPDX "ISC"'

fresh_copy
sed -i '1s/GPL-2.0-or-later/MIT/' "$scratch/tree/engine/rust/src/strlcpy.rs"
expect fail "ISC-derived port tagged a licence ISC was not relicensed to" \
	'strlcpy\.rs is tagged SPDX "MIT".*allows only: GPL-2\.0-or-later GPL-3\.0-or-later ISC$'

fresh_copy
sed -i '1s/GPL-2.0-or-later/ISC/' "$scratch/tree/engine/rust/src/strlcat.rs"
expect pass "ISC-derived port may keep the ISC tag"

fresh_copy
sed -i '1s/GPL-3.0-or-later/GPL-2.0-or-later/' "$scratch/tree/engine/rust/hashindex/src/lib.rs"
expect fail "GPLv3-or-later original tagged GPL-2.0-or-later" \
	'hashindex/src/lib\.rs is tagged SPDX "GPL-2\.0-or-later"'

fresh_copy
sed -i '1s/^/\/\/ Rust replacement for common\/crc.c, no SPDX line.\n/' "$scratch/tree/engine/rust/src/crc.rs"
expect fail "SPDX tag missing from line 1 of crc.rs" \
	'crc\.rs is tagged SPDX "<none>"'

fresh_copy
sed -i 's/^\( \* Copyright (C) 1996-1997  Id Software, Inc.\)$/\1\n * Copyright (C) 2027  New Upstream Holder/' \
	"$scratch/tree/common/crc.c"
expect fail "C original gains a holder the port does not carry" \
	'crc\.rs header lacks the line "Copyright \(C\) 2027 New Upstream Holder"'

fresh_copy
printf '// SPDX-License-Identifier: GPL-2.0-or-later\n' >"$scratch/tree/engine/rust/src/newport.rs"
expect fail "unregistered new port file" \
	'engine/rust/src/newport\.rs is not in check-rust-notices\.sh'

fresh_copy
sed -i 's/version 2 of the License/version 9 of the Licence/' "$scratch/tree/engine/h2shared/wad.c"
expect fail "C original under a licence the gate does not know" \
	'engine/h2shared/wad\.c carries a licence this gate does not recognise'

fresh_copy
sed -i 's/GNU General Public License/GNU Affero General Public License/g' \
	"$scratch/tree/engine/h2shared/hashindex.c"
expect fail "AGPL original is not accepted as GPL" \
	'engine/h2shared/hashindex\.c carries a licence this gate does not recognise'

# The cases below each broke an earlier draft of this gate (cold review, #240).

fresh_copy
sed -i '/^ \* Copyright (c) 1998 Todd C. Miller/{h;d}; /^ \*\/$/{x;/Todd/{p;x;b};x}' \
	"$scratch/tree/common/strlcpy.c"
expect fail "C holder line last in its block leaves no permission text (empty needle)" \
	'common/strlcpy\.c has no permission text after its copyright lines'

fresh_copy
sed -i '/^\/\/ copyright notice and this permission notice appear in all copies\.$/d' \
	"$scratch/tree/engine/rust/src/strlcat.rs"
expect fail "ISC grant sentence dropped (a 'copyright notice' line is prose, not a holder)" \
	'strlcat\.rs header does not reproduce the permission notice'

fresh_copy
sed -i 's/^\(\/\/ Copyright (C) 1996-1997  Id Software, Inc.\)$/\1 and XYZ Ltd/' \
	"$scratch/tree/engine/rust/src/link_ops.rs"
expect fail "holder line with junk appended is not a match" \
	'link_ops\.rs header lacks the line "Copyright \(C\) 1996-1997 Id Software, Inc\."'

fresh_copy
sed -i 's/^\/\/ \(Copyright (C) 1997-1998  Raven Software Corp.\)$/\/\/ This port is not \1/' \
	"$scratch/tree/engine/rust/src/msg_io.rs"
expect fail "holder named only inside prose is not a match" \
	'msg_io\.rs header lacks the line "Copyright \(C\) 1997-1998 Raven Software Corp\."'

fresh_copy
printf '\n/* Portions Copyright (C) 2030  Later Contributor */\n' >>"$scratch/tree/engine/h2shared/link_ops.c"
expect fail "holder in a later comment block of the C" \
	'link_ops\.rs header lacks the line "Portions Copyright \(C\) 2030 Later Contributor"'

fresh_copy
printf '\n// (c) 2031 Other Holder\n' >>"$scratch/tree/engine/h2shared/sizebuf.c"
expect fail "bare (c) holder in a // comment of the C" \
	'sizebuf\.rs header lacks the line "\(c\) 2031 Other Holder"'

fresh_copy
sed -i 's/^ \* Copyright (C) 1996-1997  Id Software, Inc.$/&\n * COPYRIGHT 2032 SHOUTING HOLDER/' \
	"$scratch/tree/engine/h2shared/wad.c"
expect fail "upper-case COPYRIGHT holder in the C" \
	'wad\.rs header lacks the line "COPYRIGHT 2032 SHOUTING HOLDER"'

fresh_copy
mkdir -p "$scratch/tree/engine/rust/src/net"
printf '// SPDX-License-Identifier: GPL-2.0-or-later\n' >"$scratch/tree/engine/rust/src/net/mod.rs"
expect fail "unregistered nested module" \
	'engine/rust/src/net/mod\.rs is not in check-rust-notices\.sh'

echo
if [ "$t_fail" -ne 0 ]; then
	echo "FAIL: self-test" >&2
	exit 1
fi
echo "PASS: self-test"
