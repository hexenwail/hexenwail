# mathlib fixtures

`mathlib.txt` is the frozen output of the C original, `engine/h2shared/mathlib.c`,
replacing the live differential harness (#475). `../replay.rs` replays it with
`cargo test` (host crate `../../replay/`, which compiles `src/ffi.rs` unchanged).

One record per line; `#` lines are comments. Floats are the 8-digit lowercase hex
of the IEEE-754 bit pattern (exact, no decimal rounding); ints are decimal.

    layout <sizeof> <off normal> <off dist> <off type> <off signbits>   mplane_t
    isnan <x> <0|1>                       Q_isnan
    anglemod <in> <out>                   anglemod
    log2 <val> <out>                      Q_log2
    inv <val> <out>                       Invert24To16
    gcd <a> <b> <out>                     GreatestCommonDivisor
    nearest <num> <mul> <out>             NearestMultiple
    anglevec <pitch yaw roll> <fwd x3> <right x3> <up x3>   AngleVectors
    rot <a x9> <b x9> <out x9>            R_ConcatRotations
    xform <a x12> <b x12> <out x12>       R_ConcatTransforms
    bops <signbits> <normal x3> <dist> <mins x3> <maxs x3> <sides>   BoxOnPlaneSide
    fdm <numer> <denom> <quotient> <rem>  FloorDivMod

Comparison (as the retired harness did, but stricter): every record is a bit-exact
match except `anglevec`, which goes through the host libm `sin`/`cos` and keeps the
harness's absolute 1e-6. Matrix and plane inputs are mostly multiples of 1/8, so
the expected bits do not depend on fused multiply-add; the generator also builds
with `-ffp-contract=off`.

Coverage: all 8 `signbits` arms of BoxOnPlaneSide against seeded boxes and planes,
each result 1/2/3, exact-boundary planes; both FloorDivMod branches including the
exact-multiple negatives (remainder 0); NearestMultiple on negatives; Q_log2 at
every power of two and its neighbours; Invert24To16 on both sides of its 256
threshold; NaN/Inf/denormal/payload bit patterns for Q_isnan; the 360-degree wrap
for anglemod, plus 62 anglemod records chosen by the generator because the same
expression in single precision gives a different answer from the C (whose
`65536/360.0` constants make it run in double): 1.46667469 gives 1.46118164 in
C and 1.4666748 in f32, and the rest cover the 1.4667..1.5326 neighbourhood,
larger positive angles and negative angles such as -2779.5. The replay floors every kind and branch, so a truncated fixture fails.

Deliberately not in the fixture:

- `Invert24To16` for 256..512: `2^40/val` does not fit an `int` there and C's
  double-to-int conversion is undefined (x86 gives INT_MIN, Rust saturates to
  INT_MAX). 513 is the first value whose result fits.
- `BOPS_Error` and the `FloorDivMod` bad-denominator path call `Sys_Error`, which
  never returns. The replay covers them separately: it re-runs itself as a child,
  requires SIGABRT and the message (`bops_bad_signbits_calls_sys_error`,
  `floordivmod_*_denominator_calls_sys_error`).

Regenerate or audit against C (needs cc and the engine headers, so run it inside
`nix develop`):

    bash engine/rust/mathlib/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/mathlib/tests/gen_fixtures.sh --check   # diff against C

Mutant check (needs perl, cargo): `bash engine/rust/mathlib/tests/mutants.sh`.
Thirty-eight mutants across every routine and every BoxOnPlaneSide arm; all die under
this replay. The subset the retired live harness could kill died there too (see
the #475 commit message for the two-sided table).
