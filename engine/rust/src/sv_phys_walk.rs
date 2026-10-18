// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the client movement of engine/hexen2/sv_phys.c and
// engine/hexenworld/server/sv_phys.c: Hexen II's SV_CheckStuck,
// SV_CheckWater, SV_WallFriction, SV_TryUnstick, SV_WalkMove and
// SV_Physics_Client, and HexenWorld's SV_Physics_Client.  The third
// sv_phys.c slice of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// Hexen II moves players on the server: SV_Physics_Client runs the
// PlayerPreThink/PostThink pair around one movetype's move, and for
// MOVETYPE_WALK that is SV_WalkMove -- a slide (SV_FlyMove), and if it hit
// a step, the same move again from a step up, kept only if it lands on good
// ground.  HexenWorld moves players on the client (pmove), so all of that is
// `#if 0' in its server; what it keeps is a short SV_Physics_Client that
// sweeps the server's view of a player from oldorigin to origin and runs
// the touch.  The two share no code, and each protocol's arm is its own
// (world_trace_hw).  The C originals moved verbatim into
// engine/rust/sv_phys/tests/.
//
// SV_Physics_Toss stays C until the movetype slice; Hexen II's
// SV_Physics_Client calls it for a tossed or bouncing player.
//
// FLOAT ARITHMETIC
//
// As in sv_phys.rs: IEEE semantics of the C source, double where the C mixes
// in a double -- host_frametime, the literals 0.5, 0.1, 0.03125 and 0.7, and
// fabs (a double function) -- rounded back where the C stores to a float.

use crate::sv_phys::{pf, v};
use crate::world_hull::facts;
#[cfg(feature = "world_trace_hw")]
use crate::sv_phys::SV_Impact;
#[cfg(feature = "world_trace_hw")]
use crate::world_move::SV_Move;
#[cfg(not(feature = "world_trace_hw"))]
use crate::world_hull::cvt_c;
#[cfg(not(feature = "world_trace_hw"))]
use crate::world_link::SV_LinkEdict;
#[cfg(not(feature = "world_trace_hw"))]
use core::ffi::c_int;

#[cfg(not(feature = "world_trace_hw"))]
use crate::mathlib::AngleVectors;
#[cfg(not(feature = "world_trace_hw"))]
use crate::sv_phys::{
    dot, edict_to_prog, vi, SV_AddGravity, SV_CheckVelocity, SV_FlyExtras, SV_FlyMove, SV_PushEntity, SV_RunThink,
};
#[cfg(not(feature = "world_trace_hw"))]
use crate::world_move::SV_PointContents;
#[cfg(not(feature = "world_trace_hw"))]
use crate::world_move::SV_TestEntityPosition;
#[cfg(not(feature = "world_trace_hw"))]
use crate::world_trace::Trace;
#[cfg(not(feature = "world_trace_hw"))]
use core::ffi::{c_char, c_uint};

/// bspfile.h.
#[cfg(not(feature = "world_trace_hw"))]
const CONTENTS_EMPTY: c_int = -1;
#[cfg(not(feature = "world_trace_hw"))]
const CONTENTS_WATER: c_int = -3;
/// sv_phys.c's `#define STEPSIZE 18` (SV_WalkMove's).
#[cfg(not(feature = "world_trace_hw"))]
const STEPSIZE: c_int = 18;
/// printsys.h's _PRINT_DEVEL: Con_DPrintf.
#[cfg(not(feature = "world_trace_hw"))]
const PRINT_DEVEL: c_uint = 2;

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    fn Phys_HostFrametime() -> f64;
    fn Phys_SvPlayer() -> *mut u8;
    fn Phys_NoStep() -> c_int;
    fn Phys_ClientActive(num: c_int) -> c_int;
    fn Phys_PlayerPreThink() -> c_int;
    fn Phys_PlayerPostThink() -> c_int;

    fn World_SvTime() -> f64;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_GlobalTime() -> *mut f32;

    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn PR_ExecuteProgram(fnum: c_int, funcname: *const c_char);
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn fabs(x: f64) -> f64;

    /// Still sv_phys.c's, until the movetype slice.
    fn SV_Physics_Toss(ent: *mut u8);
}

#[inline(always)]
unsafe fn copy3(dst: *mut f32, src: *const f32) {
    for i in 0..3 {
        *dst.add(i) = *src.add(i);
    }
}

#[cfg(not(feature = "world_trace_hw"))]
#[inline(always)]
unsafe fn flags(ent: *mut u8) -> c_int {
    cvt_c(*v(ent, facts().v_flags))
}

/// `static void SV_CheckStuck (edict_t *ent)`: a big hack to try and fix
/// the rare case of getting stuck in the world clipping hull.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_CheckStuck(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let oldorigin = v(ent, p.v_oldorigin);

    if SV_TestEntityPosition(ent).is_null() {
        copy3(oldorigin, origin);
        return;
    }

    let mut org = [0.0f32; 3];
    copy3(org.as_mut_ptr(), origin);
    copy3(origin, oldorigin);
    if SV_TestEntityPosition(ent).is_null() {
        CON_Printf(PRINT_DEVEL, c"moved back Unstuck.\n".as_ptr());
        SV_LinkEdict(ent, 1);
        return;
    }

    for z in 0..18 {
        for i in -1..=1 {
            for j in -1..=1 {
                *origin.add(0) = org[0] + i as f32;
                *origin.add(1) = org[1] + j as f32;
                *origin.add(2) = org[2] + z as f32;
                if SV_TestEntityPosition(ent).is_null() {
                    CON_Printf(PRINT_DEVEL, c"scooted Unstuck.\n".as_ptr());
                    SV_LinkEdict(ent, 1);
                    return;
                }
            }
        }
    }

    copy3(origin, org.as_ptr());
    // `if (ent->v.oldorigin != ent->v.origin)' compares the two arrays'
    // addresses, which always differ: the C always prints here.
    CON_Printf(PRINT_DEVEL, c"player is stuck.\n".as_ptr());
}

/// `static qboolean SV_CheckWater (edict_t *ent)`.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_CheckWater(ent: *mut u8) -> c_int {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let waterlevel = v(ent, p.v_waterlevel);
    let watertype = v(ent, p.v_watertype);

    let mut point = [0.0f32; 3];
    point[0] = *origin.add(0);
    point[1] = *origin.add(1);
    point[2] = *origin.add(2) + *v(ent, f.v_mins).add(2) + 1.0;

    *waterlevel = 0.0;
    *watertype = CONTENTS_EMPTY as f32;
    let mut cont = SV_PointContents(point.as_mut_ptr());
    if cont <= CONTENTS_WATER {
        *watertype = cont as f32;
        *waterlevel = 1.0;
        // `origin[2] + (mins[2] + maxs[2])*0.5', in double
        point[2] = (*origin.add(2) as f64 + (*v(ent, f.v_mins).add(2) + *v(ent, f.v_maxs).add(2)) as f64 * 0.5) as f32;
        cont = SV_PointContents(point.as_mut_ptr());
        if cont <= CONTENTS_WATER {
            *waterlevel = 2.0;
            point[2] = *origin.add(2) + *v(ent, f.v_view_ofs).add(2);
            cont = SV_PointContents(point.as_mut_ptr());
            if cont <= CONTENTS_WATER {
                *waterlevel = 3.0;
            }
        }
    }

    (*waterlevel > 1.0) as c_int
}

/// `static void SV_WallFriction (edict_t *ent, trace_t *trace)`.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_WallFriction(ent: *mut u8, trace: *const Trace) {
    let p = pf();
    let velocity = v(ent, p.v_velocity);
    let normal = (*trace).plane.normal.as_ptr();
    let mut forward = [0.0f32; 3];
    let mut right = [0.0f32; 3];
    let mut up = [0.0f32; 3];

    AngleVectors(v(ent, p.v_v_angle), forward.as_mut_ptr(), right.as_mut_ptr(), up.as_mut_ptr());
    let mut d = dot(normal, forward.as_ptr());

    // `d += 0.5', a double
    d = (d as f64 + 0.5) as f32;
    if d >= 0.0 {
        return;
    }

    // cut the tangential velocity
    let i = dot(normal, velocity);
    let mut into = [0.0f32; 3];
    let mut side = [0.0f32; 3];
    for k in 0..3 {
        into[k] = *normal.add(k) * i;
    }
    for k in 0..3 {
        side[k] = *velocity.add(k) - into[k];
    }

    *velocity.add(0) = side[0] * (1.0 + d);
    *velocity.add(1) = side[1] * (1.0 + d);
}

/// `static int SV_TryUnstick (edict_t *ent, vec3_t oldvel)`: the player has
/// come to a dead stop, possibly due to the limited float precision at some
/// angle joins in the BSP hull; try pushing one pixel in each direction.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_TryUnstick(ent: *mut u8, oldvel: *const f32) -> c_int {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let velocity = v(ent, p.v_velocity);
    let mut oldorg = [0.0f32; 3];
    let mut dir = [0.0f32; 3];
    let mut steptrace: Trace = core::mem::zeroed();
    copy3(oldorg.as_mut_ptr(), origin);

    for i in 0..8 {
        // try pushing a little in an axial direction
        let (x, y) = match i {
            0 => (2.0, 0.0),
            1 => (0.0, 2.0),
            2 => (-2.0, 0.0),
            3 => (0.0, -2.0),
            4 => (2.0, 2.0),
            5 => (-2.0, 2.0),
            6 => (2.0, -2.0),
            _ => (-2.0, -2.0),
        };
        dir[0] = x;
        dir[1] = y;

        SV_PushEntity(ent, dir.as_ptr());

        // retry the original move
        *velocity.add(0) = *oldvel.add(0);
        *velocity.add(1) = *oldvel.add(1);
        *velocity.add(2) = 0.0;
        // `SV_FlyMove (ent, 0.1, &steptrace)': the double 0.1 as the float
        // parameter
        let clip = SV_FlyMove(ent, 0.1f64 as f32, &mut steptrace);

        if fabs((oldorg[1] - *origin.add(1)) as f64) > 4.0 || fabs((oldorg[0] - *origin.add(0)) as f64) > 4.0 {
            return clip;
        }

        // go back to the original pos and try again
        copy3(origin, oldorg.as_ptr());
    }

    for k in 0..3 {
        *velocity.add(k) = 0.0;
    }
    7 // still not moving
}

/// `static void SV_WalkMove (edict_t *ent)`: only used by players.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_WalkMove(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let velocity = v(ent, p.v_velocity);
    let mut oldorg = [0.0f32; 3];
    let mut oldvel = [0.0f32; 3];
    let mut nosteporg = [0.0f32; 3];
    let mut nostepvel = [0.0f32; 3];
    // One steptrace for both SV_FlyMove calls, as in the C: each stores into
    // it only when it hits a vertical wall, and SV_WallFriction reads
    // whichever stored last.  When neither stored, yet the final clip has
    // bit 2 -- both calls returned 3 (allsolid) or 7 (the crease), or
    // SV_TryUnstick's clip carries it -- the C reads its uninitialised stack
    // trace_t: undefined behaviour, stack garbage in the engine, possibly a
    // NaN normal that poisons the velocity.  Here it is zero: a zero normal
    // makes SV_WallFriction's d 0.5, so it returns before writing anything.
    // The harness builds the C reference with -ftrivial-auto-var-init=zero,
    // which gives the C the same zeros.
    let mut steptrace: Trace = core::mem::zeroed();

    // do a regular slide move unless it looks like you ran into a step
    let oldonground = flags(ent) & f.fl_onground;
    *v(ent, f.v_flags) = (flags(ent) & !f.fl_onground) as f32;

    copy3(oldorg.as_mut_ptr(), origin);
    copy3(oldvel.as_mut_ptr(), velocity);

    let mut clip = SV_FlyMove(ent, Phys_HostFrametime() as f32, &mut steptrace);

    if (clip & 2) == 0 {
        return; // move didn't block on a step
    }

    if oldonground == 0 && *v(ent, p.v_waterlevel) == 0.0 {
        return; // don't stair up while jumping
    }

    if *v(ent, f.v_movetype) != p.movetype_walk as f32 {
        return; // gibbed by a trigger
    }

    if Phys_NoStep() != 0 {
        return;
    }

    // sv_player's flags, not ent's, as in the C
    if (flags(Phys_SvPlayer()) & p.fl_waterjump) != 0 {
        return;
    }

    copy3(nosteporg.as_mut_ptr(), origin);
    copy3(nostepvel.as_mut_ptr(), velocity);

    // try moving up and forward to go up a step
    copy3(origin, oldorg.as_ptr()); // back to start pos

    let mut upmove = [0.0f32; 3];
    let mut downmove = [0.0f32; 3];
    upmove[2] = STEPSIZE as f32;
    // `-STEPSIZE + oldvel[2]*host_frametime', in double
    downmove[2] = (-STEPSIZE as f64 + oldvel[2] as f64 * Phys_HostFrametime()) as f32;

    // move up
    SV_PushEntity(ent, upmove.as_ptr()); // FIXME: don't link?

    // move forward
    *velocity.add(0) = oldvel[0];
    *velocity.add(1) = oldvel[1];
    *velocity.add(2) = 0.0;
    clip = SV_FlyMove(ent, Phys_HostFrametime() as f32, &mut steptrace);

    // check for stuckness, possibly due to the limited precision of floats
    // in the clipping hulls
    if clip != 0
        && fabs((oldorg[1] - *origin.add(1)) as f64) < 0.03125
        && fabs((oldorg[0] - *origin.add(0)) as f64) < 0.03125
    {
        // stepping up didn't make any progress
        clip = SV_TryUnstick(ent, oldvel.as_ptr());
    }

    // extra friction based on view angle
    if (clip & 2) != 0 {
        SV_WallFriction(ent, &steptrace);
    }

    // move down
    let downtrace = SV_PushEntity(ent, downmove.as_ptr()); // FIXME: don't link?

    if downtrace.plane.normal[2] as f64 > 0.7 {
        if *v(ent, f.v_solid) == f.solid_bsp as f32 {
            *v(ent, f.v_flags) = (flags(ent) | f.fl_onground) as f32;
            *vi(ent, f.v_groundentity) = edict_to_prog(downtrace.ent.cast::<u8>());
        }
    } else {
        // if the push down didn't end up on good ground, use the move
        // without the step up.  This happens near wall / slope combinations,
        // and can cause the player to hop up higher on a slope too steep to
        // climb
        copy3(origin, nosteporg.as_ptr());
        copy3(velocity, nostepvel.as_ptr());
    }
}

/// `static void SV_Physics_Client (edict_t *ent, int num)`, Hexen II's:
/// player character actions.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Client(ent: *mut u8, num: c_int) {
    let f = facts();
    let p = pf();

    if Phys_ClientActive(num) == 0 {
        return; // unconnected slot
    }

    // call standard client pre-think
    *World_GlobalTime() = World_SvTime() as f32;
    *World_GlobalSelf() = edict_to_prog(ent);
    PR_ExecuteProgram(Phys_PlayerPreThink(), c"PlayerPreThink".as_ptr());

    // do a move
    SV_CheckVelocity(ent);

    // decide which move function to call
    let movetype = cvt_c(*v(ent, f.v_movetype));
    if movetype == p.movetype_none {
        if SV_RunThink(ent) == 0 {
            return;
        }
    } else if movetype == p.movetype_walk {
        if SV_RunThink(ent) == 0 {
            return;
        }
        if SV_CheckWater(ent) == 0 && (flags(ent) & p.fl_waterjump) == 0 {
            SV_AddGravity(ent);
        }
        SV_CheckStuck(ent);
        SV_WalkMove(ent);
    } else if movetype == p.movetype_toss || movetype == p.movetype_bounce {
        SV_Physics_Toss(ent);
    } else if movetype == p.movetype_fly || movetype == p.movetype_swim {
        if SV_RunThink(ent) == 0 {
            return;
        }
        SV_CheckWater(ent);
        SV_FlyMove(ent, Phys_HostFrametime() as f32, core::ptr::null_mut());
        SV_FlyExtras(ent, Phys_HostFrametime() as f32, core::ptr::null_mut()); // Hover & friction
    } else if movetype == p.movetype_noclip {
        if SV_RunThink(ent) == 0 {
            return;
        }
        // `VectorMA (origin, host_frametime, velocity, origin)', in double
        let origin = v(ent, f.v_origin);
        let velocity = v(ent, p.v_velocity);
        for i in 0..3 {
            *origin.add(i) = (*origin.add(i) as f64 + Phys_HostFrametime() * *velocity.add(i) as f64) as f32;
        }
    } else {
        Sys_Error(c"%s: bad movetype %i".as_ptr(), c"SV_Physics_Client".as_ptr(), movetype);
    }

    // call standard player post-think
    SV_LinkEdict(ent, 1);

    *World_GlobalTime() = World_SvTime() as f32;
    *World_GlobalSelf() = edict_to_prog(ent);
    PR_ExecuteProgram(Phys_PlayerPostThink(), c"PlayerPostThink".as_ptr());
}

/// `void SV_Physics_Client (edict_t *ent)`, HexenWorld's (the `#else' of
/// its `#if 0'): the server's sweep of a client-moved player.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Client(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let oldorigin = v(ent, p.v_oldorigin);
    let origin = v(ent, f.v_origin);
    let mins = v(ent, f.v_mins);
    let maxs = v(ent, f.v_maxs);

    let trace = SV_Move(oldorigin, mins, maxs, origin, f.move_nomonsters, ent);

    if trace.fraction < 1.0 {
        return;
    }

    let trace = SV_Move(oldorigin, mins, maxs, origin, 0, ent);

    if *v(ent, f.v_movetype) != p.movetype_bounce as f32 || (trace.allsolid == 0 && trace.startsolid == 0) {
        copy3(origin, trace.endpos.as_ptr());
    } else {
        // `trace.fraction = 0;' on a local the C then drops
        return;
    }

    if !trace.ent.is_null() {
        SV_Impact(ent, trace.ent.cast::<u8>());
    }
}
