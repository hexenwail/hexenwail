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
#      land without being registered here;
#   6. every file in borrowed_map -- code adapted from quake-srp, the Rust port
#      of id's WinQuake (#455) -- is also in derived_map, pins quake-srp at a
#      full 40-hex commit, names a path under quake-rs/, carries the header line
#      `Adapted from quake-srp: <repo>/blob/<commit>/<path>` for each source, and
#      is tagged GPL-2.0-or-later or GPL-3.0-or-later (quake-srp is GPLv2+, so
#      ISC is out even where the C original allows it).  With QUAKE_SRP_DIR set
#      to a clone (bare or not), the cited object must be a commit descending
#      from quake-srp's root commit and contained in an upstream branch, the
#      path must be a file at that commit, and quake-rs/Cargo.toml there must
#      say GPL-2.0-or-later;
#   7. any .rs under engine/rust (not only src/; cargo's target/ aside) that
#      names quake-srp in any spelling, its owner or a quake-rs/ path is in
#      borrowed_map, and every "Adapted from" header line of a borrowed file
#      is one borrowed_map asked for -- so code cannot arrive with a credit
#      comment and no pinned source, or a second source ride on the first's.
#
# A holder statement that wraps onto a second line in the C is compared line by
# line; none does today, and one that did would fail check 1 loudly, not pass.
#
# Usage: [QUAKE_SRP_DIR=<clone>] check-rust-notices.sh [--self-test]
#
#   --self-test  run the checks on a scratch copy of the tree, then break the
#                copy in each way the checks exist to catch and require every
#                breakage to fail.
#
# Requires bash and GNU awk (gawk); no build.  mawk 1.3.4 fails to match the
# `\*\/.*$` that c_comments uses to strip a closing */, so under it the ISC
# notice check fails on a correct tree.  `nix develop` provides gawk.
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

# Captured, not piped into grep -q: under pipefail an early grep exit can
# SIGPIPE awk and fail the pipeline even on a match.
awk_ver=$(awk --version 2>/dev/null | head -n1 || true)
if [[ "$awk_ver" != "GNU Awk"* ]]; then
	awk_ver=$(awk -W version 2>/dev/null | head -n1 || true)
	echo "error: GNU awk required (awk is ${awk_ver:-unknown}); run inside 'nix develop' or install gawk" >&2
	exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Rust file (relative to the repo root) -> the C file it was translated from.
derived_map() {
	cat <<-'EOF'
	engine/rust/src/cmd.rs                engine/h2shared/cmd.c
	engine/rust/src/crc.rs                common/crc.c
	engine/rust/src/cvar.rs               engine/h2shared/cvar.c
	engine/rust/src/huffman.rs            engine/hexenworld/shared/huffman.c
	engine/rust/src/info_str.rs           engine/hexenworld/shared/info_str.c
	engine/rust/src/link_ops.rs           engine/h2shared/link_ops.c
	engine/rust/src/msg_io.rs             engine/h2shared/msg_io.c
	engine/rust/src/net_chan.rs              engine/hexenworld/shared/net_chan.c
	engine/rust/src/net_bsd_h2.rs            engine/hexen2/net_bsd.c
	engine/rust/src/net_loop_h2.rs           engine/hexen2/net_loop.c
	engine/rust/src/net_udp_h2.rs            engine/hexen2/net_udp.c
	engine/rust/src/net_udp_hw.rs        engine/hexenworld/shared/net_udp.c
	engine/rust/src/quakefs.rs             engine/h2shared/quakefs.c
	engine/rust/src/sizebuf.rs            engine/h2shared/sizebuf.c
	engine/rust/src/sv_move.rs            engine/hexen2/sv_move.c
	engine/rust/src/sv_phys.rs            engine/hexen2/sv_phys.c
	engine/rust/src/sv_phys_push.rs       engine/hexen2/sv_phys.c
	engine/rust/src/sv_phys_walk.rs       engine/hexen2/sv_phys.c
	engine/rust/src/sv_phys_move.rs       engine/hexen2/sv_phys.c
	engine/rust/src/sv_main_sound.rs      engine/hexen2/sv_main.c
	engine/rust/src/sv_main_ents.rs       engine/hexen2/sv_main.c
	engine/rust/src/sv_main_datagram.rs   engine/hexen2/sv_main.c
	engine/rust/src/sv_main_connect.rs    engine/hexen2/sv_main.c
	engine/rust/src/strlcat.rs            common/strlcat.c
	engine/rust/src/strlcpy.rs            common/strlcpy.c
	engine/rust/src/wad.rs                engine/h2shared/wad.c
	engine/rust/src/world_hull.rs         engine/hexen2/world.c
	engine/rust/src/world_link.rs         engine/hexen2/world.c
	engine/rust/src/world_move.rs         engine/hexen2/world.c
	engine/rust/src/world_trace.rs        engine/hexen2/world.c
	engine/rust/src/zone.rs               engine/h2shared/zone.c
	engine/rust/hashindex/src/lib.rs      engine/h2shared/hashindex.c
	engine/rust/hashindex/src/ffi.rs      engine/h2shared/hashindex.c
	engine/rust/mathlib/src/lib.rs        engine/h2shared/mathlib.c
	engine/rust/mathlib/src/ffi.rs        engine/h2shared/mathlib.c
	EOF
}

# Rust sources that translate no C file: the crate root that wires the ports
# together, and the test-only host crate that compiles crc.rs for its fixture
# replay (it only includes the file, which carries the notices).  (build.rs
# reads hufffreq.h as data at build time and is outside the src/ directories
# this gate walks.)
original_files() {
	cat <<-'EOF'
	engine/rust/src/lib.rs
	engine/rust/crc/src/lib.rs
	engine/rust/strlcpy/src/lib.rs
	EOF
}

# Rust file -> a quake-srp file it adapts code from -> the full commit it was
# taken at (#455).  One line per quake-srp source, so a port seeded from two
# files is listed twice.  Every file here is also in derived_map: borrowed
# code is Quake's, and the port still translates a Hexen II C original whose
# notices it carries -- quake-srp's own headers credit id Software alone, the
# holder that C original already names.
borrowed_map() {
	cat <<-'EOF'
	engine/rust/src/world_trace.rs quake-rs/src/world.rs f3f372bc0779199f2bffc7b77b0dd081edd6624a
	EOF
	# The self-test registers its scratch borrowings here; a real run ignores it.
	if [ "$self_test" -eq 1 ] && [ -f "${NOTICES_EXTRA_BORROWED:-}" ]; then
		cat "$NOTICES_EXTRA_BORROWED"
	fi
}

# The entries above, each line led by where it came from: "real" for the
# heredoc, "scratch" for the self-test's.  The self-test's stand-in quake-srp
# cannot hold a real citation's commit, so its upstream cases verify only the
# scratch entries; the real ones stay registered and are checked offline in
# full, and against upstream in a real run with QUAKE_SRP_DIR.
borrowed_entries() {
	local n_real line
	n_real=$(borrowed_map_real | grep -c . || true)
	borrowed_map | while IFS= read -r line; do
		if [ "$n_real" -gt 0 ]; then
			n_real=$((n_real - 1))
			printf 'real %s\n' "$line"
		else
			printf 'scratch %s\n' "$line"
		fi
	done
}

# The heredoc alone, without the self-test's additions.
borrowed_map_real() {
	local self_test=0
	borrowed_map
}

srp_url='https://github.com/terrapapagalli1516/quake-srp'
# quake-srp's root commit: a citation must descend from it, so a clone of some
# other repository with a quake-rs/ directory cannot stand in for upstream.
# Read at check time, not load time: the self-test's stand-in repository sets
# its own root after the script has loaded, and only a self-test may.
srp_root() {
	if [ "$self_test" -eq 1 ] && [ -n "${NOTICES_SRP_ROOT:-}" ]; then
		echo "$NOTICES_SRP_ROOT"
	else
		echo 29997476d18779a0aa3609d0a6c43a09f1ae9299
	fi
}
# Check 7's net: quake-srp in any spelling (quake_srp, QuakeSRP, a non-ASCII
# hyphen -- up to three bytes between), its owner, or a path into its crate.
srp_re='quake[^a-z0-9]{0,3}srp|terrapapagalli|quake-rs/'

# git against QUAKE_SRP_DIR, deaf to any GIT_DIR or GIT_WORK_TREE in the
# environment, which would otherwise redirect it to another repository.
srp_git() {
	env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
		-u GIT_ALTERNATE_OBJECT_DIRECTORIES -u GIT_COMMON_DIR \
		git -C "$QUAKE_SRP_DIR" "$@"
}

# The refs that mirror upstream: a clone's remote-tracking branches, or a bare
# clone's own branches (it has no others).
srp_upstream_refs() {
	if [ "$(srp_git rev-parse --is-bare-repository 2>/dev/null)" = true ]; then
		echo refs/heads
	else
		echo refs/remotes
	fi
}

# Every .rs under engine/rust, symlinks included, minus cargo's build output.
# Only a crate's target/ -- beside its Cargo.toml -- is pruned: a directory
# merely named target inside a source tree (src/target/) is source.  Extra
# find tests follow.
rust_files() {
	local t=$1/engine/rust d
	shift
	local -a prune=(-path "$t/target")
	for d in "$t"/*/Cargo.toml; do
		[ -f "$d" ] && prune+=(-o -path "${d%/Cargo.toml}/target")
	done
	find "$t" \( "${prune[@]}" \) -prune -o \( -type f -o -type l \) -name '*.rs' "$@" -print | sort
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
	done < <(rust_files "$tree" -path '*/src/*')

	# 6. code adapted from quake-srp: pinned, attributed, GPL-tagged (#455)
	local -A derived=() borrowed=() wanted=()
	local srp sha extra want srp_lic srp_ok=1
	while read -r rs c; do derived[$rs]=1; done < <(derived_map)
	if [ -n "${QUAKE_SRP_DIR:-}" ] && ! srp_git rev-parse -q --git-dir >/dev/null 2>&1; then
		echo "FAIL: QUAKE_SRP_DIR=$QUAKE_SRP_DIR is not a git repository"
		bad=1; srp_ok=0
	fi
	local origin
	while read -r origin rs srp sha extra; do
		[ -n "$rs" ] || continue
		borrowed[$rs]=1
		if [ -z "${sha:-}" ] || [ -n "${extra:-}" ]; then
			echo "FAIL: borrowed_map line for $rs is not \"<rust file> <quake-srp path> <commit>\""
			bad=1; continue
		fi
		want="Adapted from quake-srp: $srp_url/blob/$sha/$srp"
		wanted["$rs|$want"]=1
		if [ -z "${derived[$rs]:-}" ]; then
			echo "FAIL: $rs adapts quake-srp code but is not in derived_map -- a borrowed"
			echo "      file still translates a Hexen II C original, whose notices it carries"
			bad=1; continue
		fi
		if ! [[ $sha =~ ^[0-9a-f]{40}$ ]]; then
			echo "FAIL: $rs pins quake-srp at \"$sha\"; use the full 40-hex commit, a branch moves"
			bad=1; continue
		fi
		if [[ $srp != quake-rs/?* || $srp == *..* || $srp == */ ]]; then
			echo "FAIL: $rs names quake-srp path \"$srp\", which is not a file under quake-rs/"
			bad=1; continue
		fi
		[ -f "$tree/$rs" ] || continue   # the derived_map loop has reported it
		# A whole-line match on the captured header, no pipe into grep -q: an
		# early grep exit can SIGPIPE the writer and fail the test under pipefail.
		hlines=$'\n'$(rs_header "$tree/$rs" | flat_lines)$'\n'
		if [[ $hlines != *$'\n'"$want"$'\n'* ]]; then
			echo "FAIL: $rs header lacks the quake-srp provenance line \"$want\""
			bad=1
		fi
		spdx=$(sed -n '1s|^// SPDX-License-Identifier: ||p' "$tree/$rs")
		if [[ " GPL-2.0-or-later GPL-3.0-or-later " != *" $spdx "* ]]; then
			echo "FAIL: $rs adapts quake-srp code (GPL-2.0-or-later) and may not be tagged \"${spdx:-<none>}\""
			bad=1
		fi
		# Against upstream itself, when a clone is at hand.  Each step closes a
		# way a citation can look right and point at nothing: a tree id for the
		# commit, a commit from some other repository, a local commit no URL
		# reaches, a directory for the file, and a licence change.
		if [ -z "${QUAKE_SRP_DIR:-}" ] || [ "$srp_ok" -ne 1 ]; then
			continue
		fi
		# A real citation cannot be in the self-test's stand-in quake-srp.
		if [ "$self_test" -eq 1 ] && [ "$origin" = real ]; then
			continue
		fi
		if [ "$(srp_git cat-file -t "$sha" 2>/dev/null)" != commit ]; then
			echo "FAIL: $rs cites $sha, which is not a commit in QUAKE_SRP_DIR"
			bad=1
		elif ! srp_git merge-base --is-ancestor "$(srp_root)" "$sha" 2>/dev/null; then
			echo "FAIL: $rs cites $sha, which does not descend from quake-srp's root commit $(srp_root)"
			bad=1
		elif [ -z "$(srp_git for-each-ref --contains "$sha" --format='%(refname)' "$(srp_upstream_refs)")" ]; then
			echo "FAIL: $rs cites $sha, which no upstream branch contains -- a local"
			echo "      commit has no URL; push it to quake-srp or cite one that is there"
			bad=1
		elif [ "$(srp_git cat-file -t "$sha:$srp" 2>/dev/null)" != blob ]; then
			echo "FAIL: $rs cites $srp at $sha, which is not a file in quake-srp there"
			bad=1
		else
			srp_lic=$(srp_git show "$sha:quake-rs/Cargo.toml" 2>/dev/null |
				sed -n 's/^license[[:space:]]*=[[:space:]]*"\([^"]*\)".*$/\1/p')
			if [ "$srp_lic" != GPL-2.0-or-later ]; then
				echo "FAIL: $rs cites quake-srp at $sha, licensed \"${srp_lic:-<none>}\" there, not GPL-2.0-or-later"
				bad=1
			fi
		fi
	done < <(borrowed_entries)

	# 7. no quake-srp code arrives unregistered.  Every .rs in the crate, not
	# only src/ (tests, build.rs, a vendored module), is scanned for any
	# spelling of the name, the owner's handle, or a quake-rs/ path; and every
	# "Adapted from" line in a header is one borrowed_map asked for, so a second
	# source cannot ride in on the first one's registration.
	while IFS= read -r rs; do
		rs=${rs#"$tree/"}
		if grep -Eiq -- "$srp_re" "$tree/$rs" && [ -z "${borrowed[$rs]:-}" ]; then
			echo "FAIL: $rs mentions quake-srp but is not in borrowed_map -- register the"
			echo "      quake-srp file and commit it adapts, or drop the mention"
			bad=1
		fi
		while IFS= read -r line; do
			if [ -n "${borrowed[$rs]:-}" ] && [ -z "${wanted["$rs|$line"]:-}" ]; then
				echo "FAIL: $rs header line \"$line\" matches no borrowed_map entry"
				bad=1
			fi
		done < <(rs_header "$tree/$rs" | flat_lines | grep -Ei '^adapted from' || true)
	done < <(rust_files "$tree")

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
	n_borrowed=$(borrowed_map | grep -c . || true)
	echo "      $n_borrowed quake-srp source(s) pinned and attributed."
	if [ "$n_borrowed" -gt 0 ] && [ -z "${QUAKE_SRP_DIR:-}" ]; then
		echo "note: quake-srp citations not checked against upstream; set QUAKE_SRP_DIR to a clone"
	fi
	exit 0
fi

# --self-test
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# The real environment must not reach the cases: each sets what it tests.
unset QUAKE_SRP_DIR
export NOTICES_EXTRA_BORROWED=$scratch/borrowed

fresh_copy() {
	rm -rf "$scratch/tree" "$NOTICES_EXTRA_BORROWED"
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

# Checks 6 and 7: code adapted from quake-srp (#455).  The real map borrows
# nothing yet, so each case registers a scratch borrowing of its own.

# borrow RS SRP_PATH SHA: register the borrowing and give RS its provenance line.
borrow() {
	printf '%s %s %s\n' "$1" "$2" "$3" >>"$NOTICES_EXTRA_BORROWED"
	sed -i "1a // Adapted from quake-srp: $srp_url/blob/$3/$2" "$scratch/tree/$1"
}

# Stand-in quake-srps, outside the tree so fresh_copy keeps them.  The main
# one has four commits on its upstream branch -- GPL, MIT, GPL again with a
# trailing comment on the licence line -- and one local commit past it, which
# no upstream branch contains.  A second repository, laid out the same but
# with its own root, plays a clone of something else.
srp_commit() { git -C "$1" -c user.name=t -c user.email=t@t commit -qam "$2"; git -C "$1" rev-parse HEAD; }
srp_repo=$scratch/quake-srp
mkdir -p "$srp_repo/quake-rs/src/server"
printf '[package]\nname = "quake-rs"\nlicense = "GPL-2.0-or-later"\n' >"$srp_repo/quake-rs/Cargo.toml"
printf '//! SV_Move.\n' >"$srp_repo/quake-rs/src/server/sv_world.rs"
git -C "$srp_repo" init -q
git -C "$srp_repo" add -A
srp_gpl=$(srp_commit "$srp_repo" gpl)
sed -i 's/GPL-2.0-or-later/MIT/' "$srp_repo/quake-rs/Cargo.toml"
srp_mit=$(srp_commit "$srp_repo" mit)
sed -i 's/"MIT"/"GPL-2.0-or-later"  # relicensed back/' "$srp_repo/quake-rs/Cargo.toml"
srp_regpl=$(srp_commit "$srp_repo" regpl)
git -C "$srp_repo" update-ref refs/remotes/origin/main HEAD
printf '//! Not upstream.\n' >>"$srp_repo/quake-rs/src/server/sv_world.rs"
srp_local=$(srp_commit "$srp_repo" local)
srp_tree=$(git -C "$srp_repo" rev-parse "$srp_gpl^{tree}")
export NOTICES_SRP_ROOT=$srp_gpl
git clone -q --bare "$srp_repo" "$scratch/quake-srp.git"
git -C "$scratch/quake-srp.git" update-ref -d refs/heads/"$(git -C "$srp_repo" branch --show-current)"
git -C "$scratch/quake-srp.git" update-ref refs/heads/main "$srp_regpl"
other_repo=$scratch/other
mkdir -p "$other_repo/quake-rs/src/server"
printf '[package]\nlicense = "GPL-2.0-or-later"\n' >"$other_repo/quake-rs/Cargo.toml"
printf '//! Something else.\n' >"$other_repo/quake-rs/src/server/sv_world.rs"
git -C "$other_repo" init -q
git -C "$other_repo" add -A
srp_other=$(srp_commit "$other_repo" other)
git -C "$other_repo" update-ref refs/remotes/origin/main HEAD
srp_fake=0123456789abcdef0123456789abcdef01234567

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
expect pass "registered, pinned, attributed borrowing (no clone to check against)"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
borrow engine/rust/src/crc.rs quake-rs/src/world.rs "$srp_fake"
expect pass "one port seeded from two quake-srp files"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
sed -i '/^\/\/ Adapted from quake-srp: /d' "$scratch/tree/engine/rust/src/crc.rs"
expect fail "provenance line dropped from a registered borrowing" \
	'crc\.rs header lacks the quake-srp provenance line'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
borrow engine/rust/src/crc.rs quake-rs/src/world.rs "$srp_fake"
sed -i '\|quake-rs/src/world\.rs$|d' "$scratch/tree/engine/rust/src/crc.rs"
expect fail "second of two provenance lines dropped" \
	'crc\.rs header lacks the quake-srp provenance line ".*/quake-rs/src/world\.rs"'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
printf '\nfn later() {}\n// Adapted from quake-srp: %s/blob/%s/quake-rs/src/server/sv_world.rs\n' \
	"$srp_url" "$srp_fake" >>"$scratch/tree/engine/rust/src/crc.rs"
sed -i '2d' "$scratch/tree/engine/rust/src/crc.rs"
expect fail "provenance line below the header, not in it" \
	'crc\.rs header lacks the quake-srp provenance line'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs main
expect fail "borrowing pinned to a branch name" \
	'crc\.rs pins quake-srp at "main"'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs f3f372b
expect fail "borrowing pinned to an abbreviated commit" \
	'crc\.rs pins quake-srp at "f3f372b"'

fresh_copy
borrow engine/rust/src/crc.rs oracle/c/world.c "$srp_fake"
expect fail "borrowing from outside quake-rs/ (id's C, not the port)" \
	'crc\.rs names quake-srp path "oracle/c/world\.c", which is not a file under quake-rs/'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/../oracle/c/world.c "$srp_fake"
expect fail "borrowing path that climbs out of quake-rs/" \
	'crc\.rs names quake-srp path "quake-rs/\.\./oracle/c/world\.c"'

fresh_copy
borrow engine/rust/src/lib.rs quake-rs/src/lib.rs "$srp_fake"
expect fail "borrowing into a file that translates no Hexen II C" \
	'engine/rust/src/lib\.rs adapts quake-srp code but is not in derived_map'

fresh_copy
printf 'engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs\n' >"$NOTICES_EXTRA_BORROWED"
expect fail "borrowed_map line missing its commit" \
	'borrowed_map line for engine/rust/src/crc\.rs is not'

fresh_copy
sed -i '1s/GPL-2.0-or-later/ISC/' "$scratch/tree/engine/rust/src/strlcat.rs"
borrow engine/rust/src/strlcat.rs quake-rs/src/server/sv_world.rs "$srp_fake"
expect fail "ISC tag on a port that adapts quake-srp (its C would allow ISC)" \
	'strlcat\.rs adapts quake-srp code \(GPL-2\.0-or-later\) and may not be tagged "ISC"'

fresh_copy
printf '\n// SV_Move, adapted from Quake-SRP.\n' >>"$scratch/tree/engine/rust/src/zone.rs"
expect fail "quake-srp credited in a comment, no pinned source" \
	'zone\.rs mentions quake-srp but is not in borrowed_map'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/ "$srp_fake"
expect fail "borrowing path that is quake-rs/ itself" \
	'crc\.rs names quake-srp path "quake-rs/", which is not a file'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
sed -i "1a // Adapted from quake-srp: $srp_url/blob/main/quake-rs/src/server/sv_phys.rs" \
	"$scratch/tree/engine/rust/src/crc.rs"
expect fail "second, unregistered provenance line rides on the first's entry" \
	'crc\.rs header line "Adapted from quake-srp: .*/blob/main/quake-rs/src/server/sv_phys\.rs" matches no borrowed_map entry'

for spelling in 'quake_srp' 'QuakeSRP' 'quake srp' $'quake\xe2\x80\x91srp' \
		'github.com/terrapapagalli1516' 'quake-rs/src/world.rs'; do
	fresh_copy
	printf '\n// SV_Move, adapted from %s.\n' "$spelling" >>"$scratch/tree/engine/rust/src/zone.rs"
	expect fail "quake-srp credited as \"$spelling\", no pinned source" \
		'zone\.rs mentions quake-srp but is not in borrowed_map'
done

fresh_copy
mkdir -p "$scratch/tree/engine/rust/cvar/tests"
printf '// adapted from quake-srp\n' >"$scratch/tree/engine/rust/cvar/tests/vendored.rs"
expect fail "quake-srp credited outside src/, in a test" \
	'cvar/tests/vendored\.rs mentions quake-srp'

fresh_copy
printf '// SPDX-License-Identifier: MIT\n' >"$scratch/tree/engine/rust/vendored.rs"
ln -s ../vendored.rs "$scratch/tree/engine/rust/src/vendored.rs"
expect fail "port symlinked into src/ from outside it" \
	'engine/rust/src/vendored\.rs is not in check-rust-notices\.sh'

# The copy carries no Cargo.toml, so these cases give hashindex one: with a
# crate present, its target/ is pruned and a src/target/ still is not.
fresh_copy
touch "$scratch/tree/engine/rust/hashindex/Cargo.toml"
mkdir -p "$scratch/tree/engine/rust/src/target"
printf '// SPDX-License-Identifier: MIT\n' >"$scratch/tree/engine/rust/src/target/world.rs"
expect fail "port in a source directory that happens to be named target" \
	'engine/rust/src/target/world\.rs is not in check-rust-notices\.sh'

fresh_copy
touch "$scratch/tree/engine/rust/hashindex/Cargo.toml"
mkdir -p "$scratch/tree/engine/rust/target/debug" "$scratch/tree/engine/rust/hashindex/target/src"
printf '// quake-srp, unregistered\n' >"$scratch/tree/engine/rust/target/debug/out.rs"
printf '// quake-srp, unregistered\n' >"$scratch/tree/engine/rust/hashindex/target/src/out.rs"
expect pass "cargo's build output is not scanned, top-level or a crate's"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/ "$srp_fake"
expect fail "borrowing path that names a directory" \
	'crc\.rs names quake-srp path "quake-rs/src/", which is not a file'

# The real map's own entries: skipped by the stand-in clone below, which
# cannot hold their commits, but held offline like any other.
if [ -n "$(borrowed_map_real)" ]; then
	fresh_copy
	read -r real_rs _ <<<"$(borrowed_map_real | head -n1)"
	sed -i '/^\/\/ Adapted from quake-srp: /d' "$scratch/tree/$real_rs"
	expect fail "a real borrowing's provenance line dropped (offline check)" \
		"$(printf '%s' "$real_rs" | sed 's/[.]/\\./g') header lacks the quake-srp provenance line"
fi

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_gpl"
QUAKE_SRP_DIR=$srp_repo expect pass "citation checked against a quake-srp clone"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_regpl"
QUAKE_SRP_DIR=$srp_repo expect pass "licence line with a trailing comment"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_gpl"
QUAKE_SRP_DIR=$scratch/quake-srp.git expect pass "citation checked against a bare clone"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_gpl"
# In a subshell for the cd; t_fail does not come back out of one, the status does.
(
	cd "$scratch"
	CDPATH=/ QUAKE_SRP_DIR=quake-srp GIT_DIR=$root/.git \
		expect pass "relative QUAKE_SRP_DIR, with CDPATH and GIT_DIR set"
	exit "$t_fail"
) || t_fail=1

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_phys.rs "$srp_gpl"
QUAKE_SRP_DIR=$srp_repo expect fail "cited path absent at the cited commit" \
	'crc\.rs cites quake-rs/src/server/sv_phys\.rs at [0-9a-f]{40}, which is not a file in quake-srp there'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server "$srp_gpl"
QUAKE_SRP_DIR=$srp_repo expect fail "cited path is a directory" \
	'crc\.rs cites quake-rs/src/server at [0-9a-f]{40}, which is not a file in quake-srp there'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_fake"
QUAKE_SRP_DIR=$srp_repo expect fail "cited commit absent from the clone" \
	'crc\.rs cites 0123456789abcdef0123456789abcdef01234567, which is not a commit in QUAKE_SRP_DIR'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_tree"
QUAKE_SRP_DIR=$srp_repo expect fail "a tree id cited as the commit" \
	'crc\.rs cites [0-9a-f]{40}, which is not a commit in QUAKE_SRP_DIR'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_other"
QUAKE_SRP_DIR=$other_repo expect fail "a clone of some other repository laid out like quake-srp" \
	"crc\\.rs cites [0-9a-f]{40}, which does not descend from quake-srp's root commit"

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_local"
QUAKE_SRP_DIR=$srp_repo expect fail "a local commit no upstream branch contains" \
	'crc\.rs cites [0-9a-f]{40}, which no upstream branch contains'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_mit"
QUAKE_SRP_DIR=$srp_repo expect fail "quake-srp not GPL-2.0-or-later at the cited commit" \
	'crc\.rs cites quake-srp at [0-9a-f]{40}, licensed "MIT" there'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_gpl"
QUAKE_SRP_DIR=$root/engine expect fail "QUAKE_SRP_DIR inside Hexenwail answers with Hexenwail's history" \
	'crc\.rs cites [0-9a-f]{40}, which is not a commit in QUAKE_SRP_DIR'

fresh_copy
borrow engine/rust/src/crc.rs quake-rs/src/server/sv_world.rs "$srp_gpl"
QUAKE_SRP_DIR=$scratch/nowhere expect fail "QUAKE_SRP_DIR that does not exist" \
	'QUAKE_SRP_DIR=.* is not a git repository'

echo
if [ "$t_fail" -ne 0 ]; then
	echo "FAIL: self-test" >&2
	exit 1
fi
echo "PASS: self-test"
