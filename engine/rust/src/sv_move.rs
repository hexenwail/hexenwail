// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the monster movement of engine/hexen2/sv_move.c and
// engine/hexenworld/server/sv_move.c: SV_CheckBottom, SV_movestep (with
// set_move_trace), SV_StepDirection, SV_FixCheckBottom, SV_NewChaseDir,
// SV_CloseEnough and the movetogoal builtin SV_MoveToGoal.  #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// Monsters walk by asking SV_movestep for one step: a straight move for
// fliers and swimmers, a step-up-then-down trace for walkers, then
// SV_CheckBottom to refuse a step that leaves a corner over a ledge.
// SV_MoveToGoal and SV_NewChaseDir are the QuakeC movetogoal builtin's chase
// logic over them.  The C originals moved verbatim into
// engine/rust/sv_move/tests/.
//
// THE DIAGNOSTICS STAY C
//
// sv_debugmovestep (#282) -- the cvar, its change callback, and the three
// helpers that log a refused step, dump nearby movers and log every
// SV_CheckBottom exit -- stays in sv_move.c, unchanged except that the
// helpers are no longer static.  They are variadic or format through
// q_vsnprintf, and Rust can call a C-variadic function but not define one.
// The port calls them at exactly the points the C did, with the same
// arguments (floats promoted to double, as C's varargs promote them), so a
// diagnostic run reads the same.  engine/tests/sv_checkbottom_test.c now pins
// the moved C original's shape; the differential harness holds this module
// to it, log lines included.
//
// THE TWO C ORIGINALS
//
// They differ in behaviour four ways, each selected by world_trace_hw as for
// world.c's ports:
//
//   * SV_movestep's flier test: Hexen II also requires !FL_HUNTFACE and
//     !FL_NOZ, HexenWorld tests FL_SWIM|FL_FLY alone;
//   * SV_movestep's swimmer: Hexen II checks the water before the move and
//     refuses one that would leave it, HexenWorld checks the trace's end;
//   * SV_StepDirection: Hexen II passes FL_SET_TRACE through to SV_movestep,
//     HexenWorld never sets the trace globals;
//   * SV_MoveToGoal: Hexen II clears trace_plane_normal, writes a return
//     value on every path and draws rand() after the step; HexenWorld writes
//     none, and draws rand() first, short-circuiting the step.
//
// SV_CheckBottom differs only in a commented-out block (#278), which neither
// compiles.
//
// FLOAT TO INT AND MATH
//
// `(int)` casts go through world_hull.rs's cvt_c, the target's own
// conversion.  cos, sin and rand are the C library's, called as the C calls
// them: the crate is no_std with no libm of its own, and the libm and rand()
// the engine links are the ones these results must match.
//
// The contract is the C source's IEEE semantics, as for world.c's ports
// (world_trace.rs).  The engine built that C with -ffast-math, under which
// GCC folds SV_StepDirection's `yaw * M_PI * 2 / 360` into one multiply by
// 2*pi/360; this port, like the plain C, multiplies then divides, so a step
// vector can differ from an old binary's by an ulp.  The retail-map run in
// the PR found no observable difference.

use core::ffi::{c_char, c_int};

use crate::mathlib::anglemod;
use crate::world_hull::{cvt_c, facts};
use crate::world_link::SV_LinkEdict;
use crate::world_move::{SV_Move, SV_PointContents};
use crate::world_trace::Trace;

/// common/bspfile.h.
const CONTENTS_SOLID: c_int = -2;
const CONTENTS_EMPTY: c_int = -1;

/// sv_move.c's `#define STEPSIZE 18`.
const STEPSIZE: c_int = 18;
/// sv_move.c's `#define DI_NODIR -1`.
const DI_NODIR: f32 = -1.0;
/// `YAW`, mathlib.h.
const YAW: usize = 1;
/// `M_PI`, as the C's double literal.
const M_PI: f64 = core::f64::consts::PI;
/// common/pr_comp.h.
const OFS_RETURN: usize = 1;
const OFS_PARM0: usize = 4;

/// world_target.h's `world_trace_globals_t`.
#[repr(C)]
struct TraceGlobals {
    allsolid: *mut f32,
    startsolid: *mut f32,
    fraction: *mut f32,
    inwater: *mut f32,
    inopen: *mut f32,
    endpos: *mut f32,
    plane_normal: *mut f32,
    plane_dist: *mut f32,
    ent: *mut c_int,
}

extern "C" {
    static pr_globals: *mut f32;

    fn World_SvEdicts() -> *mut u8;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_TraceGlobals(out: *mut TraceGlobals);

    // sv_move.c's diagnostics (see the header)
    fn SV_DebugMoveStep_Log(ent: *const u8, trace: *const Trace, fmt: *const c_char, ...);
    fn SV_DebugMoveStep_DumpMovers(foot: *const f32);
    fn SV_DebugMoveStep_Call(ent: *const u8, mins: *const f32, maxs: *const f32, ok: c_int, why: *const c_char) -> c_int;

    fn PF_changeyaw();
    fn rand() -> c_int;
    fn cos(x: f64) -> f64;
    fn sin(x: f64) -> f64;
}

#[inline(always)]
unsafe fn v(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
unsafe fn flags(ent: *mut u8) -> c_int {
    cvt_c(*v(ent, facts().v_flags))
}

/// `PROG_TO_EDICT(e)`.
#[inline(always)]
unsafe fn prog_to_edict(e: c_int) -> *mut u8 {
    World_SvEdicts().wrapping_offset(e as isize)
}

/// `EDICT_TO_PROG(e)`.
#[inline(always)]
unsafe fn edict_to_prog(e: *mut u8) -> c_int {
    (e as isize).wrapping_sub(World_SvEdicts() as isize) as c_int
}

/// `vec3_origin`: always zero, and only ever read through these calls.
static ZERO: [f32; 3] = [0.0; 3];

/// `SV_Move (start, vec3_origin, vec3_origin, stop, true, ent)`, with
/// CheckBottom's `.hull` save and restore around it.
unsafe fn point_trace_down(ent: *mut u8, start: &mut [f32; 3], stop: &mut [f32; 3]) -> Trace {
    let hull = v(ent, facts().v_hull);
    let save_hull = *hull; // temp hack so HullForEntity doesn't calculate the wrong offset
    *hull = 0.0;
    let trace = SV_Move(
        start.as_mut_ptr(),
        ZERO.as_ptr().cast_mut(),
        ZERO.as_ptr().cast_mut(),
        stop.as_mut_ptr(),
        1,
        ent,
    );
    *hull = save_hull;
    trace
}

/// `qboolean SV_CheckBottom (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_CheckBottom(ent: *mut u8) -> c_int {
    let f = facts();
    let mut mins = [0.0f32; 3];
    let mut maxs = [0.0f32; 3];
    let mut start = [0.0f32; 3];
    let mut stop = [0.0f32; 3];

    // The corner box is the entity's own bbox.  Hexen II's original carries
    // a commented-out block that would replace it with a world clip hull's
    // fixed box; it was live here for six months and is wrong (#278).  The
    // account is the comment on that block in sv_move_h2.c, and
    // engine/tests/sv_checkbottom_test.c fails if it comes back, here or there.
    let origin = v(ent, f.v_origin);
    for i in 0..3 {
        mins[i] = *origin.add(i) + *v(ent, f.v_mins).add(i);
    }
    for i in 0..3 {
        maxs[i] = *origin.add(i) + *v(ent, f.v_maxs).add(i);
    }

    // if all of the points under the corners are solid world, don't bother
    // with the tougher checks; the corners must be within 16 of the midpoint
    start[2] = mins[2] - 1.0;
    let mut easy = true;
    for x in 0..2 {
        for y in 0..2 {
            // the C's `goto realcheck` on the first corner that is not solid
            if easy {
                start[0] = if x != 0 { maxs[0] } else { mins[0] };
                start[1] = if y != 0 { maxs[1] } else { mins[1] };
                if SV_PointContents(start.as_mut_ptr()) != CONTENTS_SOLID {
                    easy = false;
                }
            }
        }
    }
    if easy {
        // we got out easy
        return SV_DebugMoveStep_Call(ent, mins.as_ptr(), maxs.as_ptr(), 1, c"all corners over solid".as_ptr());
    }

    // realcheck: check it for real...
    start[2] = mins[2];

    // the midpoint must be within 16 of the bottom; `(a + b) * 0.5` in
    // double, assigned to stop first and start from it
    stop[0] = ((mins[0] + maxs[0]) as f64 * 0.5) as f32;
    start[0] = stop[0];
    stop[1] = ((mins[1] + maxs[1]) as f64 * 0.5) as f32;
    start[1] = stop[1];
    stop[2] = start[2] - (2 * STEPSIZE) as f32;

    let trace = point_trace_down(ent, &mut start, &mut stop);

    if trace.fraction == 1.0 {
        SV_DebugMoveStep_Log(ent, core::ptr::null(), c"CheckBottom: midpoint trace had no ground within 2*STEPSIZE down".as_ptr());
        SV_DebugMoveStep_DumpMovers(start.as_ptr());
        return SV_DebugMoveStep_Call(ent, mins.as_ptr(), maxs.as_ptr(), 0, c"midpoint has no ground".as_ptr());
    }

    // trace did not reach full 36 below, so set mid and bottom
    let mid = trace.endpos[2];
    let mut bottom = trace.endpos[2];

    // the corners must be within 16 of the midpoint
    for x in 0..2 {
        for y in 0..2 {
            // `start[0] = stop[0] = x ? maxs[0] : mins[0];`
            stop[0] = if x != 0 { maxs[0] } else { mins[0] };
            start[0] = stop[0];
            stop[1] = if y != 0 { maxs[1] } else { mins[1] };
            start[1] = stop[1];

            // same check as above, just from the 4 corners down 36
            let trace = point_trace_down(ent, &mut start, &mut stop);

            if trace.fraction != 1.0 && trace.endpos[2] > bottom {
                bottom = trace.endpos[2];
            }

            if trace.fraction == 1.0 || mid - trace.endpos[2] > STEPSIZE as f32 {
                let off = trace.fraction == 1.0;
                SV_DebugMoveStep_Log(
                    ent,
                    &trace,
                    c"CheckBottom: corner (x=%d,y=%d) %s (mid=%.1f corner_z=%.1f)".as_ptr(),
                    x as c_int,
                    y as c_int,
                    if off { c"off edge".as_ptr() } else { c"drop > STEPSIZE".as_ptr() },
                    mid as f64,
                    trace.endpos[2] as f64,
                );
                SV_DebugMoveStep_DumpMovers(start.as_ptr());
                return SV_DebugMoveStep_Call(
                    ent,
                    mins.as_ptr(),
                    maxs.as_ptr(),
                    0,
                    if off { c"a corner is off an edge".as_ptr() } else { c"a corner drops > STEPSIZE".as_ptr() },
                );
            }
        }
    }
    let _ = bottom; // computed, as the C does, and never read

    SV_DebugMoveStep_Call(ent, mins.as_ptr(), maxs.as_ptr(), 1, c"traced, corners within STEPSIZE".as_ptr())
}

/// `static void set_move_trace (trace_t *trace)`.
unsafe fn set_move_trace(trace: &Trace) {
    let mut g = core::mem::MaybeUninit::<TraceGlobals>::uninit();
    World_TraceGlobals(g.as_mut_ptr());
    let g = g.assume_init();
    *g.allsolid = trace.allsolid as f32;
    *g.startsolid = trace.startsolid as f32;
    *g.fraction = trace.fraction;
    *g.inwater = trace.inwater as f32;
    *g.inopen = trace.inopen as f32;
    for i in 0..3 {
        *g.endpos.add(i) = trace.endpos[i];
    }
    for i in 0..3 {
        *g.plane_normal.add(i) = trace.plane.normal[i];
    }
    *g.plane_dist = trace.plane.dist;
    if !trace.ent.is_null() {
        *g.ent = edict_to_prog(trace.ent.cast::<u8>());
    } else {
        *g.ent = edict_to_prog(World_SvEdicts());
    }
}

#[inline(always)]
unsafe fn move_ent(ent: *mut u8, start: *mut f32, end: *mut f32) -> Trace {
    let f = facts();
    SV_Move(start, v(ent, f.v_mins), v(ent, f.v_maxs), end, 0, ent)
}

/// `qboolean SV_movestep (edict_t *ent, vec3_t move, qboolean relink,
/// qboolean noenemy, qboolean set_trace)`.
#[no_mangle]
pub unsafe extern "C" fn SV_movestep(ent: *mut u8, move_: *mut f32, relink: c_int, noenemy: c_int, set_trace: c_int) -> c_int {
    let f = facts();
    let origin = v(ent, f.v_origin);
    let mut oldorg = [0.0f32; 3];
    let mut neworg = [0.0f32; 3];
    let mut trace: Trace;
    let mut enemy: *mut u8 = core::ptr::null_mut();

    // try the move
    for i in 0..3 {
        oldorg[i] = *origin.add(i);
    }
    for i in 0..3 {
        neworg[i] = *origin.add(i) + *move_.add(i);
    }

    // flying monsters don't step up
    #[cfg(not(feature = "world_trace_hw"))]
    let flier = (flags(ent) & (f.fl_swim | f.fl_fly)) != 0
        && (flags(ent) & f.fl_huntface) == 0
        && (flags(ent) & f.fl_noz) == 0;
    #[cfg(feature = "world_trace_hw")]
    let flier = (flags(ent) & (f.fl_swim | f.fl_fly)) != 0;

    if flier {
        // try one move with vertical motion, then one without
        // only read after the loop, which always assigns it at least once
        trace = core::mem::zeroed();
        for i in 0..2 {
            for k in 0..3 {
                neworg[k] = *origin.add(k) + *move_.add(k);
            }
            if noenemy == 0 {
                enemy = prog_to_edict(*v(ent, f.v_enemy).cast::<c_int>());
                if i == 0 && enemy != World_SvEdicts() {
                    let dz = *origin.add(2) - *v(prog_to_edict(*v(ent, f.v_enemy).cast::<c_int>()), f.v_origin).add(2);
                    // Go for face; "utterly broken", as the C says: the
                    // flier test above already excluded FL_HUNTFACE
                    #[cfg(not(feature = "world_trace_hw"))]
                    let dz = if (flags(ent) & f.fl_huntface) != 0 {
                        dz + *v(prog_to_edict(*v(ent, f.v_enemy).cast::<c_int>()), f.v_view_ofs).add(2)
                    } else {
                        dz
                    };
                    if dz > 40.0 {
                        neworg[2] -= 8.0;
                    } else if dz < 30.0 {
                        neworg[2] += 8.0;
                    }
                }
            }

            #[cfg(not(feature = "world_trace_hw"))]
            {
                if (flags(ent) & f.fl_swim) != 0 && SV_PointContents(neworg.as_mut_ptr()) == CONTENTS_EMPTY {
                    // Would end up out of water, don't do z move
                    neworg[2] = *origin.add(2);
                    trace = move_ent(ent, origin, neworg.as_mut_ptr());
                    if set_trace != 0 {
                        set_move_trace(&trace);
                    }
                    if trace.fraction < 1.0 || SV_PointContents(trace.endpos.as_mut_ptr()) == CONTENTS_EMPTY {
                        SV_DebugMoveStep_Log(ent, &trace, c"swim monster left water (frac=%.2f)".as_ptr(), trace.fraction as f64);
                        return 0; // swim monster left water
                    }
                } else {
                    trace = move_ent(ent, origin, neworg.as_mut_ptr());
                    if set_trace != 0 {
                        set_move_trace(&trace);
                    }
                }
            }
            #[cfg(feature = "world_trace_hw")]
            {
                trace = move_ent(ent, origin, neworg.as_mut_ptr());
                if set_trace != 0 {
                    set_move_trace(&trace);
                }
            }

            if trace.fraction == 1.0 {
                #[cfg(feature = "world_trace_hw")]
                if (flags(ent) & f.fl_swim) != 0 && SV_PointContents(trace.endpos.as_mut_ptr()) == CONTENTS_EMPTY {
                    SV_DebugMoveStep_Log(ent, &trace, c"swim monster left water (frac=%.2f)".as_ptr(), trace.fraction as f64);
                    return 0; // swim monster left water
                }
                for k in 0..3 {
                    *origin.add(k) = trace.endpos[k];
                }
                if relink != 0 {
                    SV_LinkEdict(ent, 1);
                }
                return 1;
            }

            if noenemy != 0 || enemy == World_SvEdicts() {
                break;
            }
        }

        SV_DebugMoveStep_Log(ent, &trace, c"flier blocked (frac=%.2f)".as_ptr(), trace.fraction as f64);
        return 0;
    }

    // push down from a step height above the wished position
    neworg[2] += STEPSIZE as f32;
    let mut end = neworg;
    end[2] -= (STEPSIZE * 2) as f32;

    trace = move_ent(ent, neworg.as_mut_ptr(), end.as_mut_ptr());

    if set_trace != 0 {
        set_move_trace(&trace);
    }

    if trace.allsolid != 0 {
        SV_DebugMoveStep_Log(
            ent,
            &trace,
            c"step-down trace allsolid (move=%.0f %.0f %.0f)".as_ptr(),
            *move_.add(0) as f64,
            *move_.add(1) as f64,
            *move_.add(2) as f64,
        );
        return 0;
    }

    if trace.startsolid != 0 {
        neworg[2] -= STEPSIZE as f32;
        trace = move_ent(ent, neworg.as_mut_ptr(), end.as_mut_ptr());
        if set_trace != 0 {
            set_move_trace(&trace);
        }
        if trace.allsolid != 0 || trace.startsolid != 0 {
            SV_DebugMoveStep_Log(
                ent,
                &trace,
                c"step-down still %s after retry (move=%.0f %.0f %.0f)".as_ptr(),
                if trace.allsolid != 0 { c"allsolid".as_ptr() } else { c"startsolid".as_ptr() },
                *move_.add(0) as f64,
                *move_.add(1) as f64,
                *move_.add(2) as f64,
            );
            return 0;
        }
    }
    if trace.fraction == 1.0 {
        // if monster had the ground pulled out, go ahead and fall
        if (flags(ent) & f.fl_partialground) != 0 {
            for k in 0..3 {
                *origin.add(k) = *origin.add(k) + *move_.add(k);
            }
            if relink != 0 {
                SV_LinkEdict(ent, 1);
            }
            *v(ent, f.v_flags) = (flags(ent) & !f.fl_onground) as f32;
            return 1;
        }

        SV_DebugMoveStep_Log(
            ent,
            &trace,
            c"walked off edge: no ground within 2*STEPSIZE down (move=%.0f %.0f %.0f)".as_ptr(),
            *move_.add(0) as f64,
            *move_.add(1) as f64,
            *move_.add(2) as f64,
        );
        return 0; // walked off an edge
    }

    // check point traces down for dangling corners
    for k in 0..3 {
        *origin.add(k) = trace.endpos[k];
    }

    if SV_CheckBottom(ent) == 0 {
        if (flags(ent) & f.fl_partialground) != 0 {
            // entity had floor mostly pulled out from underneath it and is
            // trying to correct
            if relink != 0 {
                SV_LinkEdict(ent, 1);
            }
            return 1;
        }

        SV_DebugMoveStep_Log(ent, &trace, c"CheckBottom failed at landing pos (groundent above)".as_ptr());
        for k in 0..3 {
            *origin.add(k) = oldorg[k];
        }
        return 0;
    }

    if (flags(ent) & f.fl_partialground) != 0 {
        *v(ent, f.v_flags) = (flags(ent) & !f.fl_partialground) as f32;
    }
    *v(ent, f.v_groundentity).cast::<c_int>() = edict_to_prog(trace.ent.cast::<u8>());

    // the move is ok
    if relink != 0 {
        SV_LinkEdict(ent, 1);
    }
    1
}

/// `static qboolean SV_StepDirection (edict_t *ent, float yaw, float dist)`.
unsafe fn sv_step_direction(ent: *mut u8, yaw: f32, dist: f32) -> c_int {
    let f = facts();
    let origin = v(ent, f.v_origin);

    *v(ent, f.v_ideal_yaw) = yaw;
    PF_changeyaw();

    // `yaw = yaw * M_PI * 2 / 360;` in double, back to float
    let yaw = ((yaw as f64 * M_PI * 2.0) / 360.0) as f32;
    let mut move_ = [0.0f32; 3];
    move_[0] = (cos(yaw as f64) * dist as f64) as f32;
    move_[1] = (sin(yaw as f64) * dist as f64) as f32;
    move_[2] = 0.0;

    let mut oldorigin = [0.0f32; 3];
    for k in 0..3 {
        oldorigin[k] = *origin.add(k);
    }
    #[cfg(not(feature = "world_trace_hw"))]
    let set_trace_plane: c_int = if (flags(ent) & f.fl_set_trace) != 0 { 1 } else { 0 };
    #[cfg(feature = "world_trace_hw")]
    let set_trace_plane: c_int = 0;

    if SV_movestep(ent, move_.as_mut_ptr(), 0, 0, set_trace_plane) != 0 {
        let delta = *v(ent, f.v_angles).add(YAW) - *v(ent, f.v_ideal_yaw);
        if delta > 45.0 && delta < 315.0 {
            // not turned far enough, so don't take the step
            for k in 0..3 {
                *origin.add(k) = oldorigin[k];
            }
        }
        SV_LinkEdict(ent, 1);
        return 1;
    }
    SV_LinkEdict(ent, 1);

    0
}

/// `static void SV_FixCheckBottom (edict_t *ent)`.
unsafe fn sv_fix_check_bottom(ent: *mut u8) {
    let f = facts();
    *v(ent, f.v_flags) = (flags(ent) | f.fl_partialground) as f32;
}

/// `static void SV_NewChaseDir (edict_t *actor, edict_t *enemy, float dist)`.
unsafe fn sv_new_chase_dir(actor: *mut u8, enemy: *mut u8, dist: f32) {
    let f = facts();
    let mut d = [0.0f32; 3];

    // `(int)(ideal_yaw / 45) * 45`: the product overflows for a yaw past
    // about 2.1e9, where the C is undefined and the targets' imul wraps
    let olddir = anglemod(cvt_c(*v(actor, f.v_ideal_yaw) / 45.0).wrapping_mul(45) as f32);
    let turnaround = anglemod(olddir - 180.0);

    let deltax = *v(enemy, f.v_origin).add(0) - *v(actor, f.v_origin).add(0);
    let deltay = *v(enemy, f.v_origin).add(1) - *v(actor, f.v_origin).add(1);

    d[1] = if deltax > 10.0 {
        0.0
    } else if deltax < -10.0 {
        180.0
    } else {
        DI_NODIR
    };

    d[2] = if deltay < -10.0 {
        270.0
    } else if deltay > 10.0 {
        90.0
    } else {
        DI_NODIR
    };

    // try direct route
    if d[1] != DI_NODIR && d[2] != DI_NODIR {
        let tdir: f32 = if d[1] == 0.0 {
            if d[2] == 90.0 { 45.0 } else { 315.0 }
        } else if d[2] == 90.0 {
            135.0
        } else {
            215.0
        };

        if tdir != turnaround && sv_step_direction(actor, tdir, dist) != 0 {
            return;
        }
    }

    // try other directions
    if ((rand() & 3) & 1) != 0 || cvt_c(deltay).wrapping_abs() > cvt_c(deltax).wrapping_abs() {
        let tdir = d[1];
        d[1] = d[2];
        d[2] = tdir;
    }

    if d[1] != DI_NODIR && d[1] != turnaround && sv_step_direction(actor, d[1], dist) != 0 {
        return;
    }

    if d[2] != DI_NODIR && d[2] != turnaround && sv_step_direction(actor, d[2], dist) != 0 {
        return;
    }

    // there is no direct path to the player, so pick another direction
    if olddir != DI_NODIR && sv_step_direction(actor, olddir, dist) != 0 {
        return;
    }

    if (rand() & 1) != 0 {
        // randomly determine direction of search
        let mut tdir: f32 = 0.0;
        while tdir <= 315.0 {
            if tdir != turnaround && sv_step_direction(actor, tdir, dist) != 0 {
                return;
            }
            tdir += 45.0;
        }
    } else {
        let mut tdir: f32 = 315.0;
        while tdir >= 0.0 {
            if tdir != turnaround && sv_step_direction(actor, tdir, dist) != 0 {
                return;
            }
            tdir -= 45.0;
        }
    }

    if turnaround != DI_NODIR && sv_step_direction(actor, turnaround, dist) != 0 {
        return;
    }

    *v(actor, f.v_ideal_yaw) = olddir; // can't move

    // if a bridge was pulled out from underneath a monster, it may not have
    // a valid standing position at all
    if SV_CheckBottom(actor) == 0 {
        sv_fix_check_bottom(actor);
    }
}

/// `static qboolean SV_CloseEnough (edict_t *ent, edict_t *goal, float dist)`.
unsafe fn sv_close_enough(ent: *mut u8, goal: *mut u8, dist: f32) -> bool {
    let f = facts();
    for i in 0..3 {
        if *v(goal, f.v_absmin).add(i) > *v(ent, f.v_absmax).add(i) + dist {
            return false;
        }
        if *v(goal, f.v_absmax).add(i) < *v(ent, f.v_absmin).add(i) - dist {
            return false;
        }
    }
    true
}

/// `void SV_MoveToGoal (void)`: the movetogoal builtin.
#[no_mangle]
pub unsafe extern "C" fn SV_MoveToGoal() {
    let f = facts();
    let ent = prog_to_edict(*World_GlobalSelf()); // Entity moving
    let goal = prog_to_edict(*v(ent, f.v_goalentity).cast::<c_int>()); // its goalentity
    let dist = *pr_globals.add(OFS_PARM0); // how far to move

    #[cfg(not(feature = "world_trace_hw"))]
    {
        // Reset trace_plane_normal
        let mut g = core::mem::MaybeUninit::<TraceGlobals>::uninit();
        World_TraceGlobals(g.as_mut_ptr());
        let g = g.assume_init();
        for i in 0..3 {
            *g.plane_normal.add(i) = 0.0;
        }
    }

    // If not onground, flying, or swimming, return 0
    if (flags(ent) & (f.fl_onground | f.fl_fly | f.fl_swim)) == 0 {
        *pr_globals.add(OFS_RETURN) = 0.0;
        return;
    }

    // if the next step hits the enemy, return immediately
    if prog_to_edict(*v(ent, f.v_enemy).cast::<c_int>()) != World_SvEdicts() && sv_close_enough(ent, goal, dist) {
        #[cfg(not(feature = "world_trace_hw"))]
        {
            *pr_globals.add(OFS_RETURN) = 0.0;
        }
        return;
    }

    // bump around...
    #[cfg(not(feature = "world_trace_hw"))]
    {
        if sv_step_direction(ent, *v(ent, f.v_ideal_yaw), dist) == 0 {
            // Find a new direction to go in instead
            sv_new_chase_dir(ent, goal, dist);
            *pr_globals.add(OFS_RETURN) = 0.0;
        } else {
            if (rand() & 3) == 1 {
                sv_new_chase_dir(ent, goal, dist);
            }
            *pr_globals.add(OFS_RETURN) = 1.0;
        }
    }
    #[cfg(feature = "world_trace_hw")]
    {
        if (rand() & 3) == 1 || sv_step_direction(ent, *v(ent, f.v_ideal_yaw), dist) == 0 {
            sv_new_chase_dir(ent, goal, dist);
        }
    }
}
