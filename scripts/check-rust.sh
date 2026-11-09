#!/usr/bin/env bash
#
# check-rust.sh -- the whole Rust gate, as CI runs it.
#
# The engine links engine/rust into every target, so the Rust ports are not a
# configuration to test on the side; they are the build.  This runs, in order:
#
#   1. toolchain parity: the nixpkgs rustc this shell provides must be the
#      version engine/rust/rust-toolchain.toml pins, which is what the
#      WebAssembly builds use.  A flake.lock bump that moves one and not the
#      other would have the web client compiling different Rust.
#   2. the removed switches stay removed: configuring with any old
#      -DUSE_*_RS=OFF must stop with the explanation, not silently link Rust.
#   3. one engine build of hexenwail, h2ded and hwsv, shared by every gate
#      below (it used to be two per gate: twenty-two builds of one tree).
#   4. #233 archive mapping/ownership gate, then each port's gate -- crate,
#      differential harness against the C original (crc, strlcpy, huffman,
#      strlcat, mathlib, hashindex and info_str instead replay frozen fixtures and re-check them against the C
#      with tests/gen_fixtures.sh --check, #475), symbol and stray-object
#      checks on the shared build.
#   5. the Rust/C struct layouts on the host (scripts/check-rust-abi.sh; the
#      PWA job runs the same check for wasm32).
#   6. every port carries its C original's copyright notices
#      (scripts/check-rust-notices.sh).  That gate's own --self-test runs in
#      .github/workflows/rust-notices-selftest.yml, only when the checker or
#      a file it reads changes: it tests the checker, not the tree, and took
#      27s of every run here.
#
# Every check runs even after one fails, so a red run lists all of them.  The
# one exception is a failed shared build: the per-port gates all inspect it,
# so they are reported as skipped rather than run against nothing.
#
# SHARDING.  Run whole, this is ~24 minutes on a GitHub runner, and ~21 of
# them are two gates' differential harnesses: sv-main's six and world-trace's
# eighteen (nine ports x the h2 and hw variants).  Each of those is already
# independent -- its own crate build, its own target dir, its own log, its own
# verdict -- so CI splits them across a job matrix:
#
#   --plan N        print the item each of N shards runs (no tools needed)
#   --shard I/N     run shard I of N (1-based)
#
# The items are "core" (steps 1, 2, the archive gate, every other port, 5
# and 6) plus one item per sv-main and world-trace part, as each of those
# scripts lists them with --list-parts.  Shards are filled longest-first,
# each item to the least-loaded shard, using the CI-measured seconds in
# item_weight below; ties go to the lower shard.  Every shard computes the
# same plan, so N shards cover every item exactly once by construction, and
# a part added to either script, or a port added to the list below, is
# picked up with no edit here (unknown items weigh 60s, new ports ride in
# core).  The weights only balance the shards; a stale one costs minutes,
# never coverage.  tools/test_rust_gate_shards.py checks the partition.
#
# With neither flag this runs everything, as before.
#
# Requires cc, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

plan_n=
shard=
while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		--plan) plan_n=${2:-}; shift 2 || { echo "--plan needs N" >&2; exit 2; } ;;
		--shard) shard=${2:-}; shift 2 || { echo "--shard needs I/N" >&2; exit 2; } ;;
		*) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
	esac
done

ports=(hashindex mathlib sizebuf crc link-ops msg-io info-str strlcpy strlcat huffman wad cvar zone cmd net-udp-hw quakefs net-chan net-h2 world-trace sv-main)
# The gates whose parts are items of their own.  Every other port is in core.
split_gates=(sv-main world-trace)

# CI-measured seconds per item (crate build + harness), from the Rust ports
# job of run 42697684816.  Balance only -- see SHARDING above.
item_weight() {
	case "$1" in
		core) echo 110 ;;
		sv-main:inv) echo 219 ;;
		sv-main:spawn) echo 162 ;;
		sv-main:sound) echo 125 ;;
		sv-main:datagram) echo 112 ;;
		sv-main:ents) echo 106 ;;
		sv-main:connect) echo 98 ;;
		world-trace:h2/svwalk) echo 82 ;;
		world-trace:hw/svwalk) echo 72 ;;
		world-trace:hw/svphys) echo 56 ;;
		world-trace:h2/svphys) echo 41 ;;
		world-trace:h2/svmovetype) echo 34 ;;
		world-trace:hw/svmovetype) echo 31 ;;
		world-trace:h2/link) echo 18 ;;
		world-trace:hw/link) echo 16 ;;
		world-trace:h2/trace|world-trace:h2/move) echo 12 ;;
		world-trace:hw/move) echo 11 ;;
		world-trace:hw/trace) echo 10 ;;
		world-trace:*/svmove) echo 9 ;;
		world-trace:hw/svpush) echo 7 ;;
		world-trace:h2/svpush) echo 6 ;;
		world-trace:*/hull) echo 3 ;;
		*) echo 60 ;;
	esac
}

all_items() {
	local gate part
	echo core
	for gate in "${split_gates[@]}"; do
		bash "$root/scripts/check-rust-$gate.sh" --list-parts | while read -r part; do
			echo "$gate:$part"
		done
	done
}

# plan N -- "shard item" lines, every item exactly once.
plan() {
	local n=$1 i best item w
	local -a load
	for ((i = 1; i <= n; i++)); do load[i]=0; done
	all_items | while read -r item; do
		printf '%s %s\n' "$(item_weight "$item")" "$item"
	done | LC_ALL=C sort -k1,1nr -k2,2 | {
		while read -r w item; do
			best=1
			for ((i = 2; i <= n; i++)); do
				[ "${load[i]}" -lt "${load[best]}" ] && best=$i
			done
			load[best]=$((load[best] + w))
			echo "$best $item"
		done
	}
}

valid_count() { [[ $1 =~ ^[1-9][0-9]*$ ]]; }

if [ -n "$plan_n" ]; then
	valid_count "$plan_n" || { echo "--plan N: N must be a positive integer" >&2; exit 2; }
	plan "$plan_n"
	exit 0
fi

# Items this run owns.  Unsharded: all of them, and the split gates run whole.
run_core=1
declare -A parts_for=()
sharded=0
if [ -n "$shard" ]; then
	si=${shard%/*}; sn=${shard#*/}
	if [ "$si" = "$shard" ] || ! valid_count "$si" || ! valid_count "$sn" || [ "$si" -gt "$sn" ]; then
		echo "--shard I/N: need 1 <= I <= N, got '$shard'" >&2
		exit 2
	fi
	sharded=1
	run_core=0
	mine=$(plan "$sn" | awk -v s="$si" '$1 == s { print $2 }')
	echo "== shard $si/$sn =="
	if [ -z "$mine" ]; then
		echo "  no items (more shards than items); nothing to do"
		echo "PASS: shard $si/$sn is empty"
		exit 0
	fi
	while read -r item; do
		echo "  $item"
		case "$item" in
			core) run_core=1 ;;
			*:*) parts_for[${item%%:*}]+="${item#*:} " ;;
		esac
	done <<<"$mine"
	echo
fi

# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"

for tool in cc cargo rustc cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

failed=()

is_split() {
	local g
	for g in "${split_gates[@]}"; do [ "$g" = "$1" ] && return 0; done
	return 1
}

# The gates this run executes, each with the RUST_GATE_PARTS it gets ("" = all).
run_ports=()
declare -A run_parts=()
for port in "${ports[@]}"; do
	if [ "$sharded" -eq 0 ]; then
		run_ports+=("$port"); run_parts[$port]=""
	elif is_split "$port"; then
		if [ -n "${parts_for[$port]:-}" ]; then
			run_ports+=("$port"); run_parts[$port]="${parts_for[$port]% }"
		fi
	elif [ "$run_core" -eq 1 ]; then
		run_ports+=("$port"); run_parts[$port]=""
	fi
done

if [ "$run_core" -eq 1 ]; then
	echo "== 1. toolchain parity =="
	pinned=$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$root/engine/rust/rust-toolchain.toml")
	have=$(rustc --version | awk '{print $2}')
	if [ -z "$pinned" ]; then
		echo "FAIL: no channel in engine/rust/rust-toolchain.toml" >&2
		failed+=(toolchain)
	elif [ "$have" != "$pinned" ]; then
		echo "FAIL: rustc is $have but engine/rust/rust-toolchain.toml pins $pinned." >&2
		echo "      Set the pin to the nixpkgs version, so the WebAssembly builds" >&2
		echo "      compile the same Rust as every other target." >&2
		failed+=(toolchain)
	else
		echo "  rustc $have matches the pin"
	fi

	echo
	echo "== 2. the removed USE_*_RS switches refuse to configure =="
	if cmake -B "$work/removed-flag" -S "$root/engine" -DUSE_CRC_RS=OFF \
			>"$work/removed-flag.log" 2>&1; then
		echo "FAIL: -DUSE_CRC_RS=OFF configured; it must stop with an error" >&2
		failed+=(removed-flag)
	elif ! grep -q 'USE_CRC_RS=OFF is no longer supported' "$work/removed-flag.log"; then
		echo "FAIL: -DUSE_CRC_RS=OFF failed, but not with the removed-switch message:" >&2
		tail -20 "$work/removed-flag.log" >&2
		failed+=(removed-flag)
	else
		echo "  -DUSE_CRC_RS=OFF stops at configure with the explanation"
	fi
	echo
fi

echo "== 3. the shared engine build =="
if ENGINE_BUILD=$(rust_gate_engine_build "$work"); then
	export ENGINE_BUILD
	echo "  $ENGINE_BUILD"
	if [ "$run_core" -eq 1 ]; then
		echo
		echo "== 4. per-target Rust archives =="
		if WORKDIR="$work/archives" bash "$root/scripts/check-rust-archives.sh"; then
			:
		else
			failed+=(archives)
		fi
	fi
else
	# Every per-port gate checks symbols in this build, so none can run; the
	# layout check below does not need it and still does.
	failed+=(engine-build "${run_ports[@]/#/skipped:}")
	run_ports=()
fi

echo
echo "== 4. per-port gates =="
for port in "${run_ports[@]}"; do
	echo
	if [ -n "${run_parts[$port]}" ]; then
		echo "---- $port (parts: ${run_parts[$port]})"
	else
		echo "---- $port"
	fi
	if WORKDIR="$work/$port" RUST_GATE_PARTS="${run_parts[$port]}" \
			bash "$root/scripts/check-rust-$port.sh"; then
		:
	else
		failed+=("$port")
	fi
done

if [ "$run_core" -eq 1 ]; then
	echo
	echo "== 5. Rust/C struct layouts (host) =="
	if ! WORKDIR="$work/abi" bash "$root/scripts/check-rust-abi.sh"; then
		failed+=(abi)
	fi

	echo
	echo "== 6. upstream copyright notices =="
	if ! bash "$root/scripts/check-rust-notices.sh"; then
		failed+=(notices)
	fi
fi

echo
if [ "${#failed[@]}" -ne 0 ]; then
	echo "FAIL: ${failed[*]}" >&2
	exit 1
fi
if [ "$sharded" -eq 1 ]; then
	echo "PASS: shard $shard -- every item listed above"
	exit 0
fi
echo "PASS: every Rust gate -- toolchain pinned, the old switches rejected, all"
echo "      ${#ports[@]} ports link once and agree with their C originals (crc, strlcpy,"
echo "      strlcat, huffman, mathlib, hashindex and info_str by frozen fixtures re-checked against the C; the"
echo "      rest by their own differential harness or fixture), layouts match,"
echo "      and each port carries its original's copyright notices."
