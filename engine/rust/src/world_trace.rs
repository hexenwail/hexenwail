// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the hull-trace core of engine/hexen2/world.c and
// engine/hexenworld/server/world.c: SV_HullPointContents and
// SV_RecursiveHullCheck.  Phase 7, step 4 of #294; the first slice of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
// Adapted from quake-srp: https://github.com/terrapapagalli1516/quake-srp/blob/f3f372bc0779199f2bffc7b77b0dd081edd6624a/quake-rs/src/world.rs
//
// Every collision query in the engine ends here: SV_Move, droptofloor, the
// monster step checks, the chase camera (hexen2/chase.c) and the client's
// view-blend probe (hexen2/cl_main.c) all walk a hull_t's clipnode tree
// through these two functions.  The C originals were moved verbatim out of
// both world.c files into engine/rust/world_trace/tests/, where they are the
// reference half of the differential harness; nothing else in world.c moved.
//
// WHAT CAME FROM QUAKE-SRP, AND WHAT DID NOT
//
// quake-srp ports WinQuake's version of these functions.  Taken from it: the
// shape of the port -- an iterative point walk beside a recursive trace, the
// distance helper both share -- and the observation that the slanted-plane
// distance must be summed in double, which this engine's C already says
// outright with DotProductDBL.  The trace's two tail calls are a loop here,
// after the C's `goto loc0`; quake-srp writes them as recursion.  Deliberately
// NOT taken, because quake-srp chose to be total where this engine's C is not,
// and parity with the C is the contract here (#61, rule 1):
//
//   * a bad node number is Sys_Error (SV_Error in HexenWorld), not "solid";
//   * there is no recursion depth guard: the C has none, and a cyclic hull
//     overflows the stack in both;
//   * `frac -= 0.1` subtracts the DOUBLE 0.1 and rounds back to float, as C's
//     float-minus-double-literal does; quake-srp subtracts 0.1f32, which is a
//     different number and can land the backed-up point a bit apart;
//   * the crosspoint fraction is computed in double for the same reason:
//     DIST_EPSILON is the double literal (0.03125), so
//     `(t1 + DIST_EPSILON)/(t1-t2)` is a double division rounded once;
//   * the clamp is the C's `if (frac < 0) ... else if (frac > 1)`, not
//     f32::clamp, and there is no zero-denominator escape: t1 == t2 cannot
//     reach the split (both signs would match), and a NaN t1 or t2 takes the
//     C's path through it.  (A NaN frac in the backup loop is the exception;
//     see NaN below.)
//
// THE TWO C ORIGINALS
//
// Hexen II's and HexenWorld's copies differ in three places, and only one of
// them is behaviour:
//
//   * the fatal call: Sys_Error in hexen2, SV_Error in hexenworld/server.
//     SV_Error exists only in hwsv, so the name is chosen by the archive's
//     feature set (#233): `world_trace_hw` is in hwsv's set alone;
//   * hexen2 computes the backed-up midpoint through WackyBugFixer, a helper
//     taking frac by pointer, where hexenworld writes the same expression
//     inline.  The name says why it existed: forcing frac through memory
//     rounded it to float on x87.  On every target this engine builds for,
//     float arithmetic is IEEE single already, and the expression is the same;
//   * the backup loop is `while (1) { if (!solid) break; ... }` in one and
//     `while (solid) { ... }` in the other -- the same loop.
//
// The differential harness compiles BOTH originals and holds this one module
// to each, with the error name each target links.
//
// FLOATING POINT AND THE SHIPPED C
//
// Every engine target compiles its C with -ffast-math (engine/CMakeLists.txt).
// That licenses the compiler to reassociate floating-point arithmetic and to
// assume no NaN or infinity reaches a comparison, so the C as it shipped has
// no single defined result: GCC on x86_64 regroups DotProductDBL as
// (n0*p0 + n1*p1) + (n2*p2 - dist), clang and emscripten are free to choose
// otherwise, and a plane whose normal has a component below the double
// rounding window of the others then sees a point land on a different side.
// This module computes what the C SOURCE says, in IEEE single and double, and
// that is the contract the harness holds it to, against the C original built
// without -ffast-math:
//
//   * for any input -- finite, near overflow, NaN, infinite -- wherever the
//     source's IEEE behaviour returns, the same result byte for byte (a
//     NaN's payload and sign aside: IEEE leaves them unspecified);
//   * the port always returns (see NaN below).
//
// The engine-flags build is compared as well, and its differences counted,
// not enforced.  They are real but rare: GCC's regrouping moves the last bits
// of the double sum, and where a slanted plane's distance nearly cancels --
// a qbsp face slightly off axial, millions of units out, crossed a few units
// above it -- t1/t2 round differently and the fraction moves.  A review's
// targeted probe, not re-measured by the gate, put that at 0.02% of such
// crossings, relative difference up to 1.4e-4.  On a sub-ulp normal
// component a point can land on the other side.  The harness prints how
// often it sees each, phase by phase: in the gate's run, nothing in the
// main phase, one trace in the cancellation phase, and in the timed phase,
// the only one with sub-ulp normals and NaN, tens of traces and a couple of
// point contents, plus in the Hexen II run a few hangs of its own.  Those are differences
// between the C source and one compiler's reading of it; this port takes the
// source.  What the shipped engine actually does with real
// maps is checked end to end instead: h2ded's spawn-time state on demo1-3
// against origin/master's C was byte-identical.
//
// NaN.  With a NaN frac, the C source's backup loop never ends: `frac < 0` is
// false for NaN, mid stays NaN, and the point walk keeps finding SOLID.  A
// NaN frac needs no NaN input: finite coordinates near overflow give
// t1 = inf, t2 = -inf and inf/inf.  The IEEE build hangs there, and the
// engine's -ffast-math build hangs on some trees and not others, depending on
// how the compiler lowered that `<`.  A
// hung server is never the behaviour anyone wants from a mod's NaN origin, so
// the port's loop exit is `!(frac >= 0)`: identical for every finite frac, and
// for NaN it takes the "backup past 0" exit.  That is the one place this
// module departs from the C source, and the only inputs it changes are ones
// where the source never returns.
//
// 32-bit x86.  No shipped target is i686 (the Windows builds are
// x86_64-w64-mingw32), but CMake would map one.  A C build there could use
// x87 -- the hazard hexen2's WackyBugFixer was written against -- and round
// differently from the SSE2 arithmetic Rust's i686 targets use.
//
// The arithmetic is written operation for operation as the C evaluates it,
// with C's usual arithmetic conversions spelled out (`as f64` where C
// promotes a float to double, `as f32` where it assigns back).  Rust never
// contracts a*b+c into a fused multiply-add; C may, where the target has FMA
// and the compiler is allowed to (GCC defaults to -ffp-contract=fast on
// aarch64).  This engine passes no -ffp-contract flag, so an aarch64 C build
// could round a trace differently in the last place where the Rust cannot.
// The mathlib port carries the same exposure; the harness runs on x86_64,
// where neither side fuses.

use core::ffi::{c_char, c_int, c_uint};

use crate::mathlib::{MPlane, Vec3};

/// `CONTENTS_EMPTY` / `CONTENTS_SOLID` from common/bspfile.h.  Every other
/// negative number is "some liquid" to the trace: inwater.
const CONTENTS_EMPTY: c_int = -1;
const CONTENTS_SOLID: c_int = -2;

/// `#define DIST_EPSILON (0.03125)` -- a double, in both quakedef.h files.
const DIST_EPSILON: f64 = 0.03125;

/// `_PRINT_DEVEL`, engine/h2shared/printsys.h: what the Con_DPrintf macro
/// passes as CON_Printf's first argument.
const PRINT_DEVEL: c_uint = 2;

/// `mclipnode_t`.  ENABLE_BSP2 is always defined (engine/h2shared/h2config.h),
/// so in every model header -- gl_model.h, model.h, sv_model.h -- this is
/// `dclipnode2_t` from common/bspfile.h: int children, not short.
#[repr(C)]
pub struct MClipNode {
    pub planenum: c_int,
    pub children: [c_int; 2],
}

/// `hull_t`, identical in gl_model.h, model.h and sv_model.h ("if this is
/// changed, it must be changed in asm_i386.h too").
#[repr(C)]
pub struct Hull {
    pub clipnodes: *mut MClipNode,
    pub planes: *mut MPlane,
    pub firstclipnode: c_int,
    pub lastclipnode: c_int,
    pub clip_mins: Vec3,
    pub clip_maxs: Vec3,
}

/// `plane_t`, world.h: the trace's impact plane (no type/signbits).
#[repr(C)]
pub struct TracePlane {
    pub normal: Vec3,
    pub dist: f32,
}

/// `trace_t`, identical in engine/hexen2/world.h and
/// engine/hexenworld/server/world.h.  qboolean is a four-byte int.
#[repr(C)]
pub struct Trace {
    pub allsolid: c_int,
    pub startsolid: c_int,
    pub inopen: c_int,
    pub inwater: c_int,
    pub fraction: f32,
    pub endpos: Vec3,
    pub plane: TracePlane,
    pub ent: *mut core::ffi::c_void,
}

extern "C" {
    /// The real function behind Con_DPrintf, in every target.
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    /// engine/h2shared/sys.h, FUNC_NORETURN.  What hexen2/world.c calls.
    fn Sys_Error(fmt: *const c_char, ...) -> !;
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    /// engine/hexenworld/server/host.h, FUNC_NORETURN.  What
    /// hexenworld/server/world.c calls; hwsv alone defines it.
    fn SV_Error(fmt: *const c_char, ...) -> !;
}

/// `X_Error ("%s: bad node number", __thisfunc__)`, X per target.
unsafe fn bad_node_number(func: *const c_char) -> ! {
    #[cfg(not(feature = "world_trace_hw"))]
    Sys_Error(c"%s: bad node number".as_ptr(), func);
    #[cfg(feature = "world_trace_hw")]
    SV_Error(c"%s: bad node number".as_ptr(), func);
}

/// The plane distance both functions take, exactly as the C writes it:
///
///   if (plane->type < 3) d = p[plane->type] - plane->dist;
///   else                 d = DotProductDBL(plane->normal, p) - plane->dist;
///
/// The axial case is a float subtraction.  The other is DotProductDBL's
/// `(double)n0*(double)p0 + (double)n1*(double)p1 + (double)n2*(double)p2`,
/// summed left to right, the float dist promoted and subtracted in double,
/// and the result rounded once on assignment to the float.
#[inline(always)]
unsafe fn plane_dist(plane: *const MPlane, p: *const f32) -> f32 {
    let ty = (*plane).r#type;
    if ty < 3 {
        *p.add(ty as usize) - (*plane).dist
    } else {
        let n = &(*plane).normal;
        let dot = n[0] as f64 * *p as f64
            + n[1] as f64 * *p.add(1) as f64
            + n[2] as f64 * *p.add(2) as f64;
        (dot - (*plane).dist as f64) as f32
    }
}

/// `int SV_HullPointContents (hull_t *hull, int num, vec3_t p)`.
///
/// Static in both C files, and so never exported before.  It was exported for
/// world.c's other callers, then C: SV_PointContents, now world_move.rs.
/// SV_TruePointContents (only under QUAKE2) and HexenWorld's
/// SV_TestPlayerPosition (under `#if 0`) call it too, in no build.
/// hexen2/world.h declared it only behind the dead id386 arm and
/// hexenworld/server/world.h not at all; both now declare it unconditionally.
#[no_mangle]
pub unsafe extern "C" fn SV_HullPointContents(hull: *mut Hull, mut num: c_int, p: *mut f32) -> c_int {
    while num >= 0 {
        if num < (*hull).firstclipnode || num > (*hull).lastclipnode {
            bad_node_number(c"SV_HullPointContents".as_ptr());
        }

        let node = (*hull).clipnodes.offset(num as isize);
        let plane = (*hull).planes.offset((*node).planenum as isize);

        let d = plane_dist(plane, p);
        if d < 0.0 {
            num = (*node).children[1];
        } else {
            num = (*node).children[0];
        }
    }

    num
}

/// `qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f,
/// float p2f, vec3_t p1, vec3_t p2, trace_t *trace)`.
///
/// Returns 1 while the move is still clear, 0 once it has been clipped.  The
/// C's `goto loc0` tail iteration is a loop here; its true recursions stay
/// recursions, and `mid` lives in each frame exactly as the C's does ("this
/// recursion can not be optimized because mid would need to be duplicated
/// on a stack").
#[no_mangle]
pub unsafe extern "C" fn SV_RecursiveHullCheck(
    hull: *mut Hull,
    mut num: c_int,
    p1f: f32,
    p2f: f32,
    p1: *mut f32,
    p2: *mut f32,
    trace: *mut Trace,
) -> c_int {
    // loc0: // optimized recursion
    loop {
        // check for empty
        if num < 0 {
            if num != CONTENTS_SOLID {
                (*trace).allsolid = 0;
                if num == CONTENTS_EMPTY {
                    (*trace).inopen = 1;
                } else {
                    (*trace).inwater = 1;
                }
            } else {
                (*trace).startsolid = 1;
            }
            return 1; // empty
        }

        if num < (*hull).firstclipnode || num > (*hull).lastclipnode {
            bad_node_number(c"SV_RecursiveHullCheck".as_ptr());
        }

        // find the point distances
        let node = (*hull).clipnodes.offset(num as isize);
        let plane = (*hull).planes.offset((*node).planenum as isize);

        let t1 = plane_dist(plane, p1);
        let t2 = plane_dist(plane, p2);

        if t1 >= 0.0 && t2 >= 0.0 {
            num = (*node).children[0];
            continue;
        }
        if t1 < 0.0 && t2 < 0.0 {
            num = (*node).children[1];
            continue;
        }

        // put the crosspoint DIST_EPSILON pixels on the near side.  side is
        // `(t1 < 0)`; the numerator is float +/- the double DIST_EPSILON, the
        // denominator the float t1-t2 promoted, and the quotient rounds once.
        let side: usize = (t1 < 0.0) as usize;
        let mut frac: f32 = if side != 0 {
            ((t1 as f64 + DIST_EPSILON) / (t1 - t2) as f64) as f32
        } else {
            ((t1 as f64 - DIST_EPSILON) / (t1 - t2) as f64) as f32
        };
        if frac < 0.0 {
            frac = 0.0;
        } else if frac > 1.0 {
            frac = 1.0;
        }

        let mut midf: f32 = p1f + (p2f - p1f) * frac;
        let mut mid: Vec3 = [0.0; 3];
        for i in 0..3 {
            mid[i] = *p1.add(i) + frac * (*p2.add(i) - *p1.add(i));
        }

        // move up to the node
        if SV_RecursiveHullCheck(hull, (*node).children[side], p1f, midf, p1, mid.as_mut_ptr(), trace) == 0 {
            return 0;
        }

        // (PARANOID's mid-in-solid check is compiled out in both originals.)

        if SV_HullPointContents(hull, (*node).children[side ^ 1], mid.as_mut_ptr()) != CONTENTS_SOLID {
            // go past the node
            return SV_RecursiveHullCheck(hull, (*node).children[side ^ 1], midf, p2f, mid.as_mut_ptr(), p2, trace);
        }

        if (*trace).allsolid != 0 {
            return 0; // never got out of the solid area
        }

        // the other side of the node is solid, this is the impact point
        if side == 0 {
            (*trace).plane.normal = (*plane).normal;
            (*trace).plane.dist = (*plane).dist;
        } else {
            let n = (*plane).normal;
            (*trace).plane.normal = [-n[0], -n[1], -n[2]];
            (*trace).plane.dist = -(*plane).dist;
        }

        // "shouldn't really happen, but does occasionally"
        while SV_HullPointContents(hull, (*hull).firstclipnode, mid.as_mut_ptr()) == CONTENTS_SOLID {
            // float -= double literal: promote, subtract 0.1 (double), round.
            frac = (frac as f64 - 0.1) as f32;
            // The C's `if (frac < 0)`, written so a NaN frac takes the exit.
            // For every finite frac the two are the same test.  For NaN the
            // C's never fires, and since mid is then NaN in every component
            // the point walk below takes the same path to SOLID each time:
            // the C spins forever.  This is the port's one deliberate
            // departure from the C source -- see "NaN" in the header.
            if !(frac >= 0.0) {
                (*trace).fraction = midf;
                (*trace).endpos = mid;
                CON_Printf(PRINT_DEVEL, c"backup past 0\n".as_ptr());
                return 0;
            }
            midf = p1f + (p2f - p1f) * frac;
            for i in 0..3 {
                mid[i] = *p1.add(i) + frac * (*p2.add(i) - *p1.add(i));
            }
        }

        (*trace).fraction = midf;
        (*trace).endpos = mid;

        return 0;
    }
}

// --- ABI cross-check surface (scripts/check-rust-abi.sh) --------------------

#[no_mangle]
pub extern "C" fn WorldMClipNode_sizeof() -> usize {
    core::mem::size_of::<MClipNode>()
}
#[no_mangle]
pub extern "C" fn WorldMClipNode_offsetof_planenum() -> usize {
    core::mem::offset_of!(MClipNode, planenum)
}
#[no_mangle]
pub extern "C" fn WorldMClipNode_offsetof_children() -> usize {
    core::mem::offset_of!(MClipNode, children)
}

#[no_mangle]
pub extern "C" fn WorldHull_sizeof() -> usize {
    core::mem::size_of::<Hull>()
}
#[no_mangle]
pub extern "C" fn WorldHull_alignof() -> usize {
    core::mem::align_of::<Hull>()
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_clipnodes() -> usize {
    core::mem::offset_of!(Hull, clipnodes)
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_planes() -> usize {
    core::mem::offset_of!(Hull, planes)
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_firstclipnode() -> usize {
    core::mem::offset_of!(Hull, firstclipnode)
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_lastclipnode() -> usize {
    core::mem::offset_of!(Hull, lastclipnode)
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_clip_mins() -> usize {
    core::mem::offset_of!(Hull, clip_mins)
}
#[no_mangle]
pub extern "C" fn WorldHull_offsetof_clip_maxs() -> usize {
    core::mem::offset_of!(Hull, clip_maxs)
}

#[no_mangle]
pub extern "C" fn WorldTrace_sizeof() -> usize {
    core::mem::size_of::<Trace>()
}
#[no_mangle]
pub extern "C" fn WorldTrace_alignof() -> usize {
    core::mem::align_of::<Trace>()
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_allsolid() -> usize {
    core::mem::offset_of!(Trace, allsolid)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_startsolid() -> usize {
    core::mem::offset_of!(Trace, startsolid)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_inopen() -> usize {
    core::mem::offset_of!(Trace, inopen)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_inwater() -> usize {
    core::mem::offset_of!(Trace, inwater)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_fraction() -> usize {
    core::mem::offset_of!(Trace, fraction)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_endpos() -> usize {
    core::mem::offset_of!(Trace, endpos)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_plane_normal() -> usize {
    core::mem::offset_of!(Trace, plane) + core::mem::offset_of!(TracePlane, normal)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_plane_dist() -> usize {
    core::mem::offset_of!(Trace, plane) + core::mem::offset_of!(TracePlane, dist)
}
#[no_mangle]
pub extern "C" fn WorldTrace_offsetof_ent() -> usize {
    core::mem::offset_of!(Trace, ent)
}
