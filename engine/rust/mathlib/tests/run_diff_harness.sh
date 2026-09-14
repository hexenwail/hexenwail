#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build mathlib.c with renamed exports and compare it with mathlib-rs.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/.." && pwd)"
engine="$(cd "$crate/../.." && pwd)"
root="$(cd "$engine/.." && pwd)"
RUST_LIB="${RUST_LIB:-$crate/target/release/libmathlib_rs.a}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

if [ ! -f "$RUST_LIB" ]; then
	echo "error: Rust staticlib not found: $RUST_LIB" >&2
	exit 2
fi

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common")
CFLAGS=(-O1 -g -Wall -Wno-unused-function)
RENAME=(
	-Dvec3_origin=c_vec3_origin -DQ_isnan=c_Q_isnan -Danglemod=c_anglemod
	-DBOPS_Error=c_BOPS_Error -DBoxOnPlaneSide=c_BoxOnPlaneSide
	-DAngleVectors=c_AngleVectors -DR_ConcatRotations=c_R_ConcatRotations
	-DR_ConcatTransforms=c_R_ConcatTransforms -DFloorDivMod=c_FloorDivMod
	-DGreatestCommonDivisor=c_GreatestCommonDivisor -DNearestMultiple=c_NearestMultiple
	-DQ_log2=c_Q_log2 -DInvert24To16=c_Invert24To16
)

echo "== compiling renamed C original =="
cc "${INCS[@]}" "${CFLAGS[@]}" -DGLQUAKE "${RENAME[@]}" \
	-c "$engine/h2shared/mathlib.c" -o "$OUT/mathlib_c.o"
echo "== compiling harness =="
cc "${CFLAGS[@]}" -c "$here/diff_harness.c" -o "$OUT/diff_harness.o"
echo "== linking both implementations =="
cc -o "$OUT/diff_harness" "$OUT/diff_harness.o" "$OUT/mathlib_c.o" "$RUST_LIB" -lm
echo "== running =="
"$OUT/diff_harness"
echo "RESULT: PASS"
