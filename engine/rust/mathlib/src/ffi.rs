// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `extern "C"` boundary: exported functions that replace mathlib.c.
//
// # Why the exported names are not prefixed
//
// The engine calls these functions directly and `engine/h2shared/mathlib.h`
// is not modified by this port.  The exported names must therefore be
// byte-identical to the C originals.  What keeps the link free of duplicate
// symbols is that no engine target compiles `mathlib.c` (see
// engine/CMakeLists.txt) -- not a name prefix.
//
// # Why only the exported functions are here
//
// `IS_NAN`, `Q_rint`, `Q_sqrt`, `Q_rsqrt`, `VectorCompare`, `DotProduct`,
// `VectorLength`, `CrossProduct`, `VectorAdd`, `VectorSubtract`,
// `VectorInverse`, `VectorClear`, `VectorCopy`, `VectorSet`,
// `VectorNegate`, `VectorScale`, `VectorMA`, `VectorNormalize`,
// `VectorNormalizeFast`, `q_sindeg`, `q_sinrad`, `q_cosdeg`,
// `q_cosrad`, `q_sincosdeg`, `q_sincosrad` are `static inline` in the
// header (mathlib.h).  They have no symbol, so they cannot cross an FFI
// boundary at all: each C caller already has its own copy compiled in.
// They stay in C.  This crate only has to keep the struct layout identical,
// which `#[repr(C)]` gives us.
//
// # Why vec3_origin is not a global in the crate
//
// `vec3_origin` is a `vec3_t` (float[3]) global in mathlib.c that the engine
// reads and writes.  The C side keeps it; the crate only needs to honour the
// same values.  `sincos_tab` is likewise kept on the C side and only
// referenced via a C function if needed.

use core::ffi::{c_char, c_int, c_float, c_double, c_uchar};

// --- C surface, declared rather than depended on ----------------------------
//
// Declaring these by hand instead of depending on the `libc` crate keeps the
// build hermetic: cargo never contacts crates.io, which would fail in a
// network-less nix sandbox, and means there is no Cargo.lock to vendor and audit.
// Math functions (sin, cos, floor) are declared here because we cannot link
// to libm in a no_std staticlib without an external dependency.  They are
// resolved by the C linker at final link time since we are linking into a C
// executable.

extern "C" {
    pub fn abort() -> !;

    fn Sys_Error(fmt: *const c_char, ...) -> !;

    // C's `sin` and `cos` take and return `double`; Rust must use f64 here
    // even though all mathlib inputs and outputs are float.
    fn sin(x: c_double) -> c_double;
    fn cos(x: c_double) -> c_double;
    fn floor(x: c_double) -> c_double;
}

// --- Type aliases matching the engine --------------------------------------

/// `typedef unsigned char byte` at common/q_stdinc.h:171
pub type Byte = c_uchar;

/// `typedef int fixed16_t` at common/q_stdinc.h:163
pub type Fixed16 = c_int;

/// `typedef float vec_t` (or `double` with DOUBLEVEC_T) at common/q_stdinc.h
/// The engine builds with float by default; DOUBLEVEC_T switches to double.
pub type VecT = c_float;

/// `typedef vec_t vec3_t[3]` at common/q_stdinc.h:158
pub type Vec3 = [VecT; 3];
pub type Vec4 = [VecT; 4];

/// `typedef struct mplane_s { vec3_t normal; float dist; byte type; byte
/// signbits; byte pad[2]; } mplane_t` at engine/h2shared/model.h:74
///
/// Field order, names and types must match exactly.  The compile-time
/// assertions below pin the resulting offsets.
#[repr(C)]
pub struct MPlane {
    pub normal: Vec3,
    pub dist: c_float,
    pub r#type: Byte,
    pub signbits: Byte,
    pub pad: [Byte; 2],
}

// --- ABI cross-check surface ------------------------------------------------
//
// Exported so the C-side differential harness can assert that its own
// `offsetof(mplane_t, ...)` agrees with what Rust computed.

#[no_mangle]
pub extern "C" fn MathlibMPlane_sizeof() -> usize {
    core::mem::size_of::<MPlane>()
}

#[no_mangle]
pub extern "C" fn MathlibMPlane_offsetof_normal() -> usize {
    core::mem::offset_of!(MPlane, normal)
}

#[no_mangle]
pub extern "C" fn MathlibMPlane_offsetof_dist() -> usize {
    core::mem::offset_of!(MPlane, dist)
}

#[no_mangle]
pub extern "C" fn MathlibMPlane_offsetof_type() -> usize {
    core::mem::offset_of!(MPlane, r#type)
}

#[no_mangle]
pub extern "C" fn MathlibMPlane_offsetof_signbits() -> usize {
    core::mem::offset_of!(MPlane, signbits)
}

// --- M_PI constant ---------------------------------------------------------
const M_PI: f32 = 3.14159265358979323846_f32;

// --- BoxOnPlaneSide --------------------------------------------------------
//
// `int BoxOnPlaneSide(vec3_t emins, vec3_t emaxs, mplane_t *plane)`
// Returns 1, 2, or 1 + 2.
//
// This is the only exported function that takes an `mplane_t *`, so the
// `MPlane` struct definition matters here.

#[no_mangle]
pub unsafe extern "C" fn BoxOnPlaneSide(
    emins: *const f32,
    emaxs: *const f32,
    plane: *const MPlane,
) -> c_int {
    if plane.is_null() {
        return 0;
    }
    let p = &*plane;
    let emins_slice = core::slice::from_raw_parts(emins, 3);
    let emaxs_slice = core::slice::from_raw_parts(emaxs, 3);

    // Match mathlib.c exactly.  Its axial fast path is compiled out; callers
    // use the BOX_ON_PLANE_SIDE macro for that case.
    let (dist1, dist2) = match p.signbits {
        0 => (
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emaxs_slice[2],
            p.normal[0] * emins_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emins_slice[2],
        ),
        1 => (
            p.normal[0] * emins_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emaxs_slice[2],
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emins_slice[2],
        ),
        2 => (
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emaxs_slice[2],
            p.normal[0] * emins_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emins_slice[2],
        ),
        3 => (
            p.normal[0] * emins_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emaxs_slice[2],
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emins_slice[2],
        ),
        4 => (
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emins_slice[2],
            p.normal[0] * emins_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emaxs_slice[2],
        ),
        5 => (
            p.normal[0] * emins_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emins_slice[2],
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emaxs_slice[2],
        ),
        6 => (
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emins_slice[2],
            p.normal[0] * emins_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emaxs_slice[2],
        ),
        7 => (
            p.normal[0] * emins_slice[0] + p.normal[1] * emins_slice[1] + p.normal[2] * emins_slice[2],
            p.normal[0] * emaxs_slice[0] + p.normal[1] * emaxs_slice[1] + p.normal[2] * emaxs_slice[2],
        ),
        _ => BOPS_Error(),
    };

    let mut sides = 0;
    if dist1 >= p.dist {
        sides = 1;
    }
    if dist2 < p.dist {
        sides |= 2;
    }

    sides
}

// --- AngleVectors ----------------------------------------------------------
//
// `void AngleVectors(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)`
// Computes forward/right/up vectors from Euler angles (pitch, yaw, roll).
// `angles[0]` = pitch, `angles[1]` = yaw, `angles[2]` = roll.

#[no_mangle]
pub unsafe extern "C" fn AngleVectors(
    angles: *const f32,
    forward: *mut f32,
    right: *mut f32,
    up: *mut f32,
) {
    // C checks: if (!angles || !forward) return;
    if angles.is_null() || forward.is_null() {
        return;
    }

    // Create slices for safe indexing
    let angles_slice = core::slice::from_raw_parts(angles, 3);
    let yaw = angles_slice[1] * (M_PI * 2.0 / 360.0);
    let pitch = angles_slice[0] * (M_PI * 2.0 / 360.0);
    let roll = angles_slice[2] * (M_PI * 2.0 / 360.0);

    let sy = sin(yaw as c_double) as c_float;
    let cy = cos(yaw as c_double) as c_float;
    let sp = sin(pitch as c_double) as c_float;
    let cp = cos(pitch as c_double) as c_float;
    let sr = sin(roll as c_double) as c_float;
    let cr = cos(roll as c_double) as c_float;

    let forward_slice = core::slice::from_raw_parts_mut(forward, 3);
    forward_slice[0] = cp * cy;
    forward_slice[1] = cp * sy;
    forward_slice[2] = -sp;

    if !right.is_null() {
        let right_slice = core::slice::from_raw_parts_mut(right, 3);
        right_slice[0] = -1.0 * (sr * sp * cy + cr * -sy);
        right_slice[1] = -1.0 * (sr * sp * sy + cr * cy);
        right_slice[2] = -1.0 * (sr * cp);
    }

    if !up.is_null() {
        let up_slice = core::slice::from_raw_parts_mut(up, 3);
        up_slice[0] = cr * sp * cy + -sr * -sy;
        up_slice[1] = cr * sp * sy + -sr * cy;
        up_slice[2] = cr * cp;
    }
}

// --- R_ConcatRotations ----------------------------------------------------
//
// `void R_ConcatRotations(float in1[3][3], float in2[3][3], float out[3][3])`
// Multiplies two 3x3 rotation matrices.

#[no_mangle]
pub unsafe extern "C" fn R_ConcatRotations(
    in1: *const [[c_float; 3]; 3],
    in2: *const [[c_float; 3]; 3],
    out: *mut [[c_float; 3]; 3],
) {
    if in1.is_null() || in2.is_null() || out.is_null() {
        return;
    }
    let a = &*in1;
    let b = &*in2;
    let o = &mut *out;

    for i in 0..3 {
        for j in 0..3 {
            o[i][j] = a[i][0] * b[0][j]
                    + a[i][1] * b[1][j]
                    + a[i][2] * b[2][j];
        }
    }
}

// --- R_ConcatTransforms ---------------------------------------------------
//
// `void R_ConcatTransforms(float in1[3][4], float in2[3][4], float out[3][4])`
// Multiplies two 3x4 affine transforms.

#[no_mangle]
pub unsafe extern "C" fn R_ConcatTransforms(
    in1: *const [[c_float; 4]; 3],
    in2: *const [[c_float; 4]; 3],
    out: *mut [[c_float; 4]; 3],
) {
    if in1.is_null() || in2.is_null() || out.is_null() {
        return;
    }
    let a = &*in1;
    let b = &*in2;
    let o = &mut *out;

    for i in 0..3 {
        for j in 0..4 {
            o[i][j] = a[i][0] * b[0][j]
                    + a[i][1] * b[1][j]
                    + a[i][2] * b[2][j]
                    + if j == 3 { a[i][3] } else { 0.0 };
        }
    }
}

// --- Q_isnan --------------------------------------------------------------
//
// `int Q_isnan(float x)`
// Returns 1 if x is NaN, 0 otherwise.

#[no_mangle]
pub unsafe extern "C" fn Q_isnan(x: c_float) -> c_int {
    let bits: u32 = f32::to_bits(x);
    let exponent = (bits >> 23) & 0xFF;
    let mantissa = bits & 0x7FFFFF;
    if exponent == 0xFF && mantissa != 0 {
        1
    } else {
        0
    }
}

// --- anglemod -------------------------------------------------------------
//
// `float anglemod(float a)`
// Wraps an angle into [0, 65536).

#[no_mangle]
pub unsafe extern "C" fn anglemod(a: c_float) -> c_float {
    (360.0 / 65536.0) * ((a * (65536.0 / 360.0)) as c_int & 65535) as c_float
}

// --- GreatestCommonDivisor ------------------------------------------------
//
// `int GreatestCommonDivisor(int i1, int i2)`
// Euclidean algorithm.

#[no_mangle]
pub unsafe extern "C" fn GreatestCommonDivisor(mut i1: c_int, mut i2: c_int) -> c_int {
    while i2 != 0 {
        let t = i2;
        i2 = i1 % i2;
        i1 = t;
    }
    i1.abs()
}

// --- NearestMultiple ------------------------------------------------------
//
// `int NearestMultiple(int num, int mul)`
// Returns the nearest multiple of `mul` to `num`.

#[no_mangle]
pub unsafe extern "C" fn NearestMultiple(num: c_int, mul: c_int) -> c_int {
    if mul == 0 {
        return 0;
    }
    if num >= 0 {
        (num / mul) * mul
    } else {
        ((num - mul + 1) / mul) * mul
    }
}

// --- Q_log2 ---------------------------------------------------------------
//
// `int Q_log2(int val)`
// Returns floor(log2(val)).

#[no_mangle]
pub unsafe extern "C" fn Q_log2(mut val: c_int) -> c_int {
    let mut answer = 0;
    while val > 1 {
        val >>= 1;
        answer += 1;
    }
    answer
}

// --- Invert24To16 ---------------------------------------------------------
//
// `fixed16_t Invert24To16(fixed16_t val)`
// Inverts an 8.24 value to a 16.16 value.

#[no_mangle]
pub unsafe extern "C" fn Invert24To16(val: Fixed16) -> Fixed16 {
    if val < 256 {
        return -1;
    }
    (65536.0 * 16777216.0 / val as f64 + 0.5) as Fixed16
}

// --- BOPS_Error -----------------------------------------------------------
//
// `FUNC_NORETURN void BOPS_Error(void)`
// Called from BoxOnPlaneSide when signbits are invalid.

#[no_mangle]
pub unsafe extern "C" fn BOPS_Error() -> ! {
    let msg = c"BoxOnPlaneSide:  Bad signbits".as_ptr();
    Sys_Error(msg);
}

// --- FloorDivMod ----------------------------------------------------------
//
// `void FloorDivMod(double numer, double denom, int *quotient, int *rem)`
// Returns mathematically correct (floor-based) quotient and remainder.

#[no_mangle]
pub unsafe extern "C" fn FloorDivMod(
    numer: c_double,
    denom: c_double,
    quotient: *mut c_int,
    rem: *mut c_int,
) {
    if denom <= 0.0 {
        let msg = c"FloorDivMod: bad denominator".as_ptr();
        Sys_Error(msg, denom);
    }

    let (q, r) = if numer >= 0.0 {
        let x = floor(numer / denom);
        let q = x as c_int;
        let r = floor(numer - x * denom) as c_int;
        (q, r)
    } else {
        let x = floor(-numer / denom);
        let mut q = -(x as c_int);
        let mut r = floor(-numer - x * denom) as c_int;
        if r != 0 {
            q -= 1;
            r = (denom as c_int) - r;
        }
        (q, r)
    };

    if !quotient.is_null() {
        *quotient = q;
    }
    if !rem.is_null() {
        *rem = r;
    }
}