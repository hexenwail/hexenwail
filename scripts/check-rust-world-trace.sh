#!/usr/bin/env bash
#
# check-rust-world-trace.sh -- gate for the Rust ports out of
# engine/hexen2/world.c and engine/hexenworld/server/world.c (#455): the hull
# traces (SV_HullPointContents, SV_RecursiveHullCheck; world_trace.rs),
# hull selection (the box hull, SV_HullForEntity; world_hull.rs), area
# linking (SV_ClearWorld, SV_LinkEdict, SV_UnlinkEdict; world_link.rs) and
# moves (SV_Move, SV_PointContents, SV_TestEntityPosition; world_move.rs) --
# all of world.c's code; what world.c still holds is sv_areanodes' storage --
# and, built on them, sv_move.c's monster movement (SV_CheckBottom,
# SV_movestep, SV_MoveToGoal; sv_move.rs), which leaves sv_move.c only the
# sv_debugmovestep diagnostics, and sv_phys.c's base helpers (SV_FlyMove,
# SV_PushEntity and the rest; sv_phys.rs) and pushers (SV_PushMove,
# SV_PushRotate, SV_Physics_Pusher, HexenWorld's SV_Push; sv_phys_push.rs)
# and client movement (SV_WalkMove, SV_Physics_Client and their helpers;
# sv_phys_walk.rs).
#
# The differential half runs each port's harness twice: against the Hexen II
# original, and against the HexenWorld original with world_trace_hw, the
# feature hwsv's archive carries.  Each run compares what both sides return
# and write byte for byte (NaN payloads canonicalised), and every console
# line and fatal call; requires its outcome floors; and requires every line
# of its C original to have run under gcov, but for lines run_diff_harness.sh
# proves unreachable (one clamp in the traces, the two-line null-hull branch
# in hull selection, the sv_link_prev store in area linking, the startsolid
# store in SV_ClipToLinks, Hexen II's FL_HUNTFACE dz line in SV_movestep,
# SV_FlyMove's MAX_CLIP_PLANES guard, SV_PushRotate's empty slave loops).  The hull harness also compiles the real
# engine/rust/world_target.c, through which the port reads each target's
# layouts.
#
# The CMake half proves the shared engine build links each exported symbol
# exactly once per binary, and that neither world.c object still defines any
# of them.  check-rust-archives.sh holds the error-name split: only hwsv's
# archive may reach SV_Error.
#
# USAGE
#
#   ./scripts/check-rust-world-trace.sh
#
# Requires cc, gcov, cargo/rustc, cmake and nm -- run inside `nix develop`.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

for arg in "$@"; do
	case "$arg" in
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
symbols=(SV_HullPointContents SV_RecursiveHullCheck SV_InitBoxHull SV_HullForEntity SV_ClearWorld SV_LinkEdict SV_UnlinkEdict SV_Move SV_PointContents SV_TestEntityPosition)
sv_move_symbols=(SV_CheckBottom SV_movestep SV_MoveToGoal)
# SV_FlyExtras is Hexen II's alone (HexenWorld's is #if 0), so it is checked
# per binary below
sv_phys_symbols=(SV_CheckVelocity SV_RunThink SV_Impact ClipVelocity SV_FlyMove SV_AddGravity SV_PushEntity SV_PushMove SV_PushRotate SV_Physics_Pusher SV_Physics_Client)

for tool in cc gcov cargo cmake nm; do
	command -v "$tool" >/dev/null || {
		echo "error: $tool not on PATH -- run this inside 'nix develop'" >&2
		exit 2
	}
done

step=1
for variant in h2 hw; do
for port in trace hull link move svmove svphys svpush svwalk; do
	# Each harness links its own archive: the hull harness supplies
	# world_target.c, the trace harness does not, so world_hull stays out of
	# the trace archive.  world_trace_hw selects HexenWorld's arms in both.
	case "$variant/$port" in
		h2/trace) feature=world_trace ;;
		hw/trace) feature=world_trace_hw ;;
		h2/hull)  feature=world_hull ;;
		hw/hull)  feature=world_hull,world_trace_hw ;;
		h2/link)  feature=world_link ;;
		hw/link)  feature=world_link,world_trace_hw ;;
		h2/move)  feature=world_move ;;
		hw/move)  feature=world_move,world_trace_hw ;;
		h2/svmove) feature=sv_move ;;
		hw/svmove) feature=sv_move,world_trace_hw ;;
		h2/svphys) feature=sv_phys ;;
		hw/svphys) feature=sv_phys,world_trace_hw ;;
		h2/svpush) feature=sv_phys_push ;;
		hw/svpush) feature=sv_phys_push,world_trace_hw ;;
		h2/svwalk) feature=sv_phys_walk ;;
		hw/svwalk) feature=sv_phys_walk,world_trace_hw ;;
	esac
	case "$port" in
		svmove) dir=sv_move; name=sv_move ;;
		svphys) dir=sv_phys; name=sv_phys ;;
		svpush) dir=sv_phys; name=sv_phys_push ;;
		svwalk) dir=sv_phys; name=sv_phys_walk ;;
		*) dir=world_$port; name=world_$port ;;
	esac
	echo "== $step. $variant: build the crate with $feature =="
	step=$((step + 1))
	cargo build --release --offline \
		--manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target-$variant-$port" \
		--features "$feature"
	lib="$work/target-$variant-$port/release/libengine_rs.a"

	echo
	echo "== $step. $variant: $name differential harness, Rust vs the C original =="
	step=$((step + 1))
	log="$work/diff-$variant-$port.log"
	set +e
	VARIANT=$variant RUST_LIB="$lib" OUT="$work/diff-$variant-$port" \
		bash "$crate/$dir/tests/$(case "$port" in svpush) echo run_push_harness.sh ;; svwalk) echo run_walk_harness.sh ;; *) echo run_diff_harness.sh ;; esac)" >"$log" 2>&1
	status=$?
	set -e
	grep -E '^(undefined in the C|outcomes:|          [0-9A-Za-z-]|generated:|            [-0-9a-z%]|timed phase:|directed:)|^(world_(trace|hull|link|move)|sv_move|sv_phys|sv_phys_push|sv_phys_walk) differential harness|^coverage:|^RESULT:' "$log" || true
	if [ "$status" -ne 0 ] || ! grep -q '^RESULT: PASS$' "$log"; then
		echo "FAIL: $variant $name differential harness (exit $status)" >&2
		grep -E 'MISMATCH|COVERAGE|FAIL|#####|error|harness bug' "$log" >&2 || tail -40 "$log" >&2
		exit 1
	fi
	# RESULT alone would still pass with the enumeration cut to nothing, so
	# floor the case count.  Observed per variant: 416,400 for the traces and
	# 155,580 for hull selection (six digits); 15,281 for area linking, whose
	# cases are whole scenarios -- a cleared world and up to 48 links and
	# unlinks, compared as one end state -- plus the QuakeC calls in them
	# (five digits); 201,204 for SV_Move and the rest (six); 180,000 for the
	# monster movement and 180,000 for the physics helpers, 120,000 each for
	# the pushers and the client movement (six).
	if [ "$port" = link ]; then floor_digits=5; else floor_digits=6; fi
	count_line=$(grep -E "^$name differential harness( \([A-Za-z0-9_]+\))?: PASS \(" "$log" || true)
	count=$(printf '%s\n' "$count_line" | sed -n 's/.*PASS (\([0-9]*\) cases.*/\1/p')
	if [ "${#count}" -lt "$floor_digits" ]; then
		echo "FAIL: $variant $name harness case count is below its $floor_digits-digit floor" >&2
		echo "      observed: ${count_line:-<no PASS line>}" >&2
		exit 1
	fi
	echo
done
done

echo "== $step. the engine build: glhexen2, h2ded and hwsv =="
step=$((step + 1))
engine_build=$(rust_gate_engine_build "$work")
echo "  $engine_build"

echo
echo "== $step. exactly one definition per binary, none left in world.c =="
for bin in glhexen2 h2ded hwsv; do
	path="$engine_build/bin/$bin"
	[ -x "$path" ] || { echo "FAIL: $path was not built" >&2; exit 1; }
	for sym in "${symbols[@]}" "${sv_move_symbols[@]}" "${sv_phys_symbols[@]}"; do
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne 1 ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected exactly 1" >&2
			exit 1
		fi
	done
	# SV_FlyExtras is Hexen II's alone, SV_Push HexenWorld's
	for pair in SV_FlyExtras:1:0 SV_Push:0:1 SV_CheckStuck:1:0 SV_CheckWater:1:0 SV_WallFriction:1:0 SV_TryUnstick:1:0 SV_WalkMove:1:0; do
		sym=${pair%%:*}; want=${pair#*:}; want=${want%%:*}
		[ "$bin" = hwsv ] && want=${pair##*:}
		n=$(nm "$path" | grep -cE " [Tt] $sym\$" || true)
		if [ "$n" -ne "$want" ]; then
			echo "FAIL: $bin: $sym has $n definitions, expected $want" >&2
			exit 1
		fi
	done
	echo "  $bin: ${symbols[*]} ${sv_move_symbols[*]} ${sv_phys_symbols[*]} defined exactly once"
done
# Every world.c object the build made -- the client's and h2ded's hexen2 one,
# hwsv's hexenworld one -- must only reference the two symbols now.
objects=$(find "$engine_build" -name 'world.c.o' -o -name 'world.c.obj' | sort)
nobj=$(printf '%s\n' "$objects" | grep -c . || true)
if [ "$nobj" -lt 3 ]; then
	echo "FAIL: expected a world.c object per target (3), found $nobj" >&2
	exit 1
fi
while IFS= read -r obj; do
	for sym in "${symbols[@]}" WackyBugFixer SV_HullForBox SV_CreateAreaNode SV_TouchLinks SV_FindTouchedLeafs SV_ClipMoveToEntity SV_ClipToLinks SV_MoveBounds; do
		if nm "$obj" | grep -qE " [Tt] $sym\$"; then
			echo "FAIL: ${obj#"$engine_build"/} still defines $sym" >&2
			exit 1
		fi
	done
done <<<"$objects"
echo "  $nobj world.c object(s): none defines the traces"
# and every sv_move.c object keeps only the diagnostics
objects=$(find "$engine_build" -name 'sv_move.c.o' -o -name 'sv_move.c.obj' | sort)
nobj=$(printf '%s\n' "$objects" | grep -c . || true)
if [ "$nobj" -lt 3 ]; then
	echo "FAIL: expected an sv_move.c object per target (3), found $nobj" >&2
	exit 1
fi
while IFS= read -r obj; do
	for sym in "${sv_move_symbols[@]}" SV_StepDirection SV_NewChaseDir SV_CloseEnough SV_FixCheckBottom set_move_trace; do
		if nm "$obj" | grep -qE " [Tt] $sym\$"; then
			echo "FAIL: ${obj#"$engine_build"/} still defines $sym" >&2
			exit 1
		fi
	done
	for sym in SV_DebugMoveStep_Log SV_DebugMoveStep_DumpMovers SV_DebugMoveStep_Call; do
		nm "$obj" | grep -qE " T $sym\$" || {
			echo "FAIL: ${obj#"$engine_build"/} does not export $sym, which sv_move.rs calls" >&2
			exit 1
		}
	done
done <<<"$objects"
echo "  $nobj sv_move.c object(s): only the sv_debugmovestep diagnostics"
# and no sv_phys.c object still defines a ported helper
objects=$(find "$engine_build" -name 'sv_phys.c.o' -o -name 'sv_phys.c.obj' | sort)
nobj=$(printf '%s\n' "$objects" | grep -c . || true)
if [ "$nobj" -lt 3 ]; then
	echo "FAIL: expected an sv_phys.c object per target (3), found $nobj" >&2
	exit 1
fi
while IFS= read -r obj; do
	for sym in "${sv_phys_symbols[@]}" SV_FlyExtras SV_Push SV_CheckStuck SV_CheckWater SV_WallFriction SV_TryUnstick SV_WalkMove; do
		if nm "$obj" | grep -qE " [Tt] $sym\$"; then
			echo "FAIL: ${obj#"$engine_build"/} still defines $sym" >&2
			exit 1
		fi
	done
done <<<"$objects"
echo "  $nobj sv_phys.c object(s): none defines the ported helpers"
stray=$(find "$engine_build" -name 'world_trace_*.c.o' -o -name 'world_hull_*.c.o' -o -name 'world_link_*.c.o' -o -name 'world_move_*.c.o' -o -name 'sv_move_*.c.o' -o -name 'sv_phys_*.c.o' -o -name 'c_ref.c.o' | wc -l)
[ "$stray" -eq 0 ] || {
	echo "FAIL: $stray harness fixture object(s) in the engine build" >&2
	exit 1
}

echo "PASS: Rust world.c ports (traces, hull selection, area linking, moves), sv_move.c's"
echo "      monster movement and sv_phys.c's helpers, pushers and client movement -- each harness agrees with"
echo "      both C originals, every reachable line of them ran, each binary links"
echo "      each symbol once, and neither world.c defines any of them."
