#!/usr/bin/env bash
#
# check-rust-link-ops.sh -- gate for the Rust port of h2shared/link_ops.c.
#
# The differential half proves the intrusive list operations leave the C and
# Rust implementations in the same state: normalised topology, forward and
# backward walks, stale pointers after RemoveLink, empty-list no-ops, and the
# aliased/duplicated cases where the C's statement order is the answer.  The
# CMake half proves the engine build every gate shares
# (scripts/lib/rust-gate.sh) links it exactly once per binary with no C object
# left in the build.
#
# The --engine arm adds the end-to-end half: it runs the REAL ENGINE with the
# Rust link_ops and with the C original, on a real map load, and diffs the
# output.  SV_LinkEdict and SV_TouchLinks in engine/hexen2/world.c are the call
# sites it reaches, and a map load drives both.  It is opt-in because it needs
# the demo game data and Xvfb, which CI does not provide.
#
# USAGE
#
#   ./scripts/check-rust-link-ops.sh            # differential harness + CMake gate
#   ./scripts/check-rust-link-ops.sh --engine   # ... plus the engine smoke
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
# The PASS line carries the case count, and RESULT alone would still pass if the
# enumeration were cut down to nothing -- so floor the count at four digits (a
# thousand cases) rather than pinning it, which would mean editing the gate every
# time the enumeration is deliberately widened.
# Observed today: 1,579 cases.  The floor is one decimal place below it.
count_line=$(grep -E '^link_ops differential harness: PASS \(' "$diff_log" || true)
count=$(printf '%s\n' "$count_line" | grep -oE '[0-9]+' | head -n1 || true)
if [ "${#count}" -lt 4 ]; then
	echo "FAIL: harness case count is below the thousand-case floor" >&2
	echo "      observed: ${count_line:-<no 'link_ops differential harness: PASS (N cases)' line>}" >&2
	cat "$diff_log" >&2
	exit 1
fi

echo
echo "== 3. the engine build: glhexen2, h2ded and hwsv =="
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== 4. exactly one link_ops definition per binary =="
# The archive must export the whole C ABI.  In a linked binary,
# InsertLinkAfter has no engine caller (world.c links everything through
# InsertLinkBefore), so whether it survives depends on how the linker
# partitions the archive; require it at most once there, and every called
# symbol exactly once.
archive="$engine_build/rust/libengine_rs.a"
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
	path="$engine_build/bin/$bin"
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
stray=$(find "$engine_build" -name 'link_ops.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray link_ops.c.o object(s) in the engine build" >&2
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
		"$root/engine/rust/hashindex/tests/run_engine_smoke.sh" link_ops
fi

echo "PASS: Rust link_ops -- differential harness green, the consolidated"
echo "      archive links exactly one symbol per target, and no link_ops.c"
echo "      object is left in the engine build."
