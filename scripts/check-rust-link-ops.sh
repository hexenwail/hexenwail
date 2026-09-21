#!/usr/bin/env bash
#
# check-rust-link-ops.sh -- gate for the Rust port of h2shared/link_ops.c.
#
# The differential half proves the intrusive list operations leave the C and
# Rust implementations in the same state: normalised topology, forward and
# backward walks, stale pointers after RemoveLink, empty-list no-ops, and the
# aliased/duplicated cases where the C's statement order is the answer.  The
# CMake half proves the consolidated engine archive selects link_ops
# independently and that the C fallback remains buildable.
#
# The --engine arm adds the end-to-end half: it runs the REAL ENGINE with the
# Rust link_ops and with the C original, on a real map load, and diffs the
# output.  SV_LinkEdict and SV_TouchLinks in engine/hexen2/world.c are the call
# sites it reaches, and a map load drives both.  It is opt-in because it needs
# the demo game data and Xvfb, which CI does not provide.
#
# USAGE
#
#   ./check-rust-link-ops.sh            # differential harness + CMake gate
#   ./check-rust-link-ops.sh --engine   # ... plus the engine smoke
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
		-h|--help) sed -n '2,25p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(ClearLink RemoveLink InsertLinkBefore InsertLinkAfter)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf,crc,link_ops

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	bash "$crate/link_ops/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|differential harness:' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status" >&2
	cat "$diff_log" >&2
	exit 1
fi
grep -q '^RESULT: PASS$' "$diff_log" || {
	echo "FAIL: harness did not print 'RESULT: PASS'" >&2
	cat "$diff_log" >&2
	exit 1
}

echo
echo "== 3. build all three targets with USE_LINK_OPS_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_LINK_OPS_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one link_ops definition per binary =="
# The archive must export the whole C ABI.  In a linked binary,
# InsertLinkAfter has no engine caller (world.c links everything through
# InsertLinkBefore), so whether it survives depends on how the linker
# partitions the archive; require it at most once there, and every called
# symbol exactly once.
archive="$work/build-on/rust/libengine_rs.a"
[ -f "$archive" ] || { echo "FAIL: $archive was not built" >&2; exit 1; }
for sym in "${symbols[@]}"; do
	n=$(nm "$archive" 2>/dev/null | grep -cE " T $sym\$" || true)
	if [ "$n" -ne 1 ]; then
		echo "FAIL: libengine_rs.a: $sym has $n definitions, expected exactly 1" >&2
		exit 1
	fi
done
echo "  libengine_rs.a: ${#symbols[@]}/${#symbols[@]} symbols exported exactly once"
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$sym" = InsertLinkAfter ]; then
			[ "$n" -le 1 ] && continue
		elif [ "$n" -eq 1 ]; then
			continue
		fi
		echo "FAIL: $bin: $sym has $n definitions" >&2
		exit 1
	done
	echo "  $bin: called link_ops symbols exactly once, InsertLinkAfter at most once"
done
stray=$(find "$work/build-on" -name 'link_ops.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray link_ops.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 5. flag OFF still compiles link_ops.c (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_LINK_OPS_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'link_ops.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile link_ops.c ($found objects)" >&2
		exit 1
	fi
	for sym in "${symbols[@]}"; do
		n=$(nm "$work/build-off/bin/$bin" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: OFF $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
done

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 6. engine smoke: run the real engine both ways =="
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$work/build-on/bin/glhexen2" \
	OFF_BIN="$work/build-off/bin/glhexen2" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh"
fi

echo "PASS: Rust link_ops -- differential harness green, the consolidated"
echo "      archive links exactly one symbol per target, and the C fallback"
echo "      still compiles when USE_LINK_OPS_RS=OFF."
