#!/usr/bin/env bash
#
# check-rust-hashindex.sh -- gate for the optional Rust port of hashindex.c.
#
# WHY THIS EXISTS
#
#   USE_RUST_HASHINDEX defaults to OFF and every other target in this tree
#   builds with the default.  Without this script the Rust crate is compiled by
#   *nothing*: a rustc bump, a change to hashindex.h, or a change to any engine
#   header it shares could break the port outright while every other CI step
#   stayed green.  It would rot silently and be discovered only by whoever next
#   tried to switch the flag on.
#
#   The differential harness is the half that matters.  A build gate alone
#   proves the crate compiles and links; it says nothing about whether the Rust
#   implementation still computes what the C original computes.
#
# WHAT IT CHECKS
#
#   1. the crate builds, offline, with no dependencies
#   2. the differential harness passes -- Rust and C agree on the raw array
#      contents AND on per-bucket chain ORDER, over 1.2M operations, plus
#      abort parity on the four fatal paths
#   3. all three targets build with -DUSE_RUST_HASHINDEX=ON
#   4. exactly one definition of each of the five symbols in each binary; this
#      is the duplicate-symbol hazard the CMake source-list removal exists to
#      prevent, and the failure mode if that removal is ever lost
#   5. with the flag OFF, the C original is what gets linked -- i.e. the
#      rollback is real and not merely documented
#
# Requires cc, cargo/rustc and cmake -- run it inside `nix develop`.
# No game data needed: nothing here starts the engine.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust/hashindex"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
rust_lib="$crate/target/release/libhashindex_rs.a"
symbols=(Hash_Allocate Hash_Free Hash_Add Hash_Remove Hash_Clear)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the crate (offline, no dependencies) =="
cargo build --release --offline --manifest-path "$crate/Cargo.toml"

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$rust_lib" OUT="$work/diff" "$crate/tests/run_diff_harness.sh" >"$diff_log" 2>&1
harness_status=$?
set -e
grep -E 'RESULT:|checks run:' "$diff_log" || true
if [ "$harness_status" -ne 0 ]; then
	echo "FAIL: differential harness exited $harness_status (a divergence)" >&2
	sed -n '/FAIL /p' "$diff_log" | head -20 >&2
	exit 1
fi
# Re-assert the summary line too: a harness that somehow exited 0 without
# having run anything must not pass as green.
grep -q '^RESULT: PASS$' "$diff_log" || {
	echo "FAIL: harness did not print 'RESULT: PASS'" >&2
	exit 1
}

echo
echo "== 3. build all three targets with -DUSE_RUST_HASHINDEX=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_RUST_HASHINDEX=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one definition of each symbol, in each binary =="
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1." >&2
			echo "      If 0, the staticlib was not linked; if 2, the C original" >&2
			echo "      is still in the source list (see the USE_RUST_HASHINDEX" >&2
			echo "      block in engine/CMakeLists.txt)." >&2
			exit 1
		fi
	done
	echo "  $bin: 5/5 symbols exactly once"
done

# The C object must not have been compiled into an ON build either, or the
# single definitions above are luck rather than construction.
stray=$(find "$work/build-on" -name 'hashindex.c.o' | wc -l)
if [ "$stray" -ne 0 ]; then
	echo "FAIL: $stray hashindex.c.o object(s) in an ON build" >&2
	exit 1
fi

echo
echo "== 5. flag OFF still links the C original (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_RUST_HASHINDEX=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'hashindex.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile hashindex.c ($found objects)" >&2
		exit 1
	fi
done
echo "  all three OFF builds compile the C original"

echo
echo "PASS: Rust hashindex -- differential harness green, all three targets"
echo "      build with the flag ON, none with a duplicate symbol, and the OFF"
echo "      build still uses the C original."
