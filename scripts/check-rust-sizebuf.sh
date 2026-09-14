#!/usr/bin/env bash
#
# check-rust-sizebuf.sh -- gate for the Rust port of h2shared/sizebuf.c.
#
# The differential half proves caller-owned layout, interior pointer returns,
# allocation, exact bytes, overflow state and fatal diagnostics.  The CMake
# half proves the consolidated engine archive selects sizebuf independently and
# that the C fallback remains buildable.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
		-h|--help) sed -n '2,18p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
crate="$root/engine/rust"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
symbols=(SZ_Init SZ_Clear SZ_GetSpace SZ_Write SZ_Print)

for tool in cc cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

echo "== 1. build the consolidated crate with all migrated subsystems =="
cargo build --release --offline \
	--manifest-path "$crate/Cargo.toml" \
	--features hashindex,mathlib,sizebuf

echo
echo "== 2. differential harness: Rust vs the C original, one binary =="
diff_log="$work/diff.log"
set +e
RUST_LIB="$crate/target/release/libengine_rs.a" \
	OUT="$work/diff" \
	"$crate/sizebuf/tests/run_diff_harness.sh" >"$diff_log" 2>&1
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
echo "== 3. build all three targets with USE_SIZEBUF_RS=ON =="
cmake -B "$work/build-on" -S "$root/engine" \
	-DUSE_SIZEBUF_RS=ON \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-on" -j"$(nproc)" >/dev/null

echo
echo "== 4. exactly one sizebuf definition per binary =="
for bin in glhexen2 h2ded hwsv; do
	path="$work/build-on/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	echo "  $bin: 5/5 symbols exactly once"
done
stray=$(find "$work/build-on" -name 'sizebuf.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray sizebuf.c.o object(s) in an ON build" >&2
	exit 1
}

echo
echo "== 5. flag OFF still compiles sizebuf.c (rollback is real) =="
cmake -B "$work/build-off" -S "$root/engine" \
	-DUSE_SIZEBUF_RS=OFF \
	-DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON >/dev/null
cmake --build "$work/build-off" -j"$(nproc)" >/dev/null
for bin in glhexen2 h2ded hwsv; do
	found=$(find "$work/build-off" -name 'sizebuf.c.o' -path "*$bin.dir*" | wc -l)
	if [ "$found" -ne 1 ]; then
		echo "FAIL: OFF build of $bin did not compile sizebuf.c ($found objects)" >&2
		exit 1
	fi
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

echo "PASS: Rust sizebuf -- differential harness green, the consolidated"
echo "      archive links exactly one symbol per target, and the C fallback"
echo "      still compiles when USE_SIZEBUF_RS=OFF."
