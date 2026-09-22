#!/usr/bin/env bash
#
# check-rust-sizebuf.sh -- gate for the Rust port of h2shared/sizebuf.c.
#
# The differential half proves caller-owned layout, interior pointer returns,
# allocation, exact bytes, overflow state and fatal diagnostics.  The CMake
# half proves the engine build every gate shares (scripts/lib/rust-gate.sh)
# links it exactly once per binary with no C object left in the build.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

run_engine=0
for arg in "$@"; do
	case "$arg" in
		--engine) run_engine=1 ;;
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		*) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
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
# The PASS line carries the case count, and RESULT alone would still pass if the
# enumeration were cut down to nothing -- so floor the count at three digits (a
# hundred cases; the count moves by a case or two with pipe chunking in the
# child-process checks) rather than pinning it, which would mean editing the gate
# every time the enumeration is deliberately widened.
# Observed today: 359 to 361 cases, which is the pipe-chunking variation
# described above.  The floor is one decimal place below that.
count_line=$(grep -E '^sizebuf differential harness: PASS \(' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 3 ]; then
	echo "FAIL: harness case count is below the hundred-case floor" >&2
	echo "      observed: ${count_line:-<no 'sizebuf differential harness: PASS (N cases)' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. exactly one sizebuf definition per binary =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
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
stray=$(find "$engine_build" -name 'sizebuf.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray sizebuf.c.o object(s) in the engine build" >&2
	exit 1
}

if [ "$run_engine" -eq 1 ]; then
	echo
	echo "== 5. engine smoke: this engine against a C-only reference =="
	reference=$(rust_gate_reference_bin)
	demo="$(nix build "$root#demodata" --no-link --print-out-paths)/share/hexenwail"
	xvfb="$(nix build nixpkgs#xvfb --no-link --print-out-paths)/bin/Xvfb"
	DEMO_DIR="$demo" \
	ON_BIN="$engine_build/bin/glhexen2" \
	OFF_BIN="$reference" \
	XVFB="$xvfb" \
	WORK="$work/smoke" \
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" sizebuf
fi

echo "PASS: Rust sizebuf -- differential harness green, the consolidated"
echo "      archive links exactly one symbol per target, and no sizebuf.c"
echo "      object is left in the engine build."
