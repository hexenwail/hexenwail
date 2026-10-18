// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the pushers of engine/hexen2/sv_phys.c and
// engine/hexenworld/server/sv_phys.c: SV_PushMove (and HexenWorld's SV_Push,
// which its SV_PushMove calls), SV_PushRotate and SV_Physics_Pusher.  The
// second sv_phys.c slice of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// A pusher is a MOVETYPE_PUSH brush -- a door, a plat, a train, a rotating
// bridge.  It moves (or turns) by its velocity over the frame, carries what
// stands on it, shoves what it now overlaps, and if something will not
// move it puts everything back and runs its `blocked' function.  The C
// originals moved verbatim into engine/rust/sv_phys/tests/.
//
// THE TWO C ORIGINALS
//
// They differ more here than anywhere so far, each selected by world_trace_hw:
//
//   * SV_PushMove: Hexen II's pushes each entity through SV_PushEntity and
//     carries two Hexenwail changes -- a liquid pusher (watertype set) skips
//     the collision pass (Inky), and sv_gameplayfix_elevators lifts a rider
//     blocked by the platform it stands on by DIST_EPSILON (uhexen2-a5nn.7),
//     logged at map_checks or developer level.  HexenWorld's moves the
//     pusher through SV_Push, Quake's older test-and-nudge, which only
//     HexenWorld has.
//   * SV_PushRotate: while pushing, Hexen II restores the pusher's own solid
//     (uhexen2-3aus); HexenWorld writes SOLID_BSP back.
//   * SV_Physics_Pusher: HexenWorld names the think "think in
//     SV_Physics_Pusher", not "... SV_RunThink", and when the think has moved
//     the pusher more than 1/64, puts it back and SV_Push-es it there.
//
// Both SV_PushRotate's carry a dead "slave" mechanism: the loop that fills
// it is commented out, so slaves_moved is always 0 and its loops never run.
// They are kept, as the C keeps them, with slaves_moved a constant 0.
//
// THE MOVED LISTS
//
// Hexen II's moved_edict/moved_from are static (uhexen2-gyul: 32000 entries
// would not fit a stack frame); HexenWorld's SV_Push and SV_PushRotate keep
// theirs on the stack, at its MAX_EDICTS of 768.  Here each function has its
// own static list, which behaves the same: none of these is re-entered
// (QuakeC runs inside them, but no builtin reaches the physics), and each
// list is written before it is read.  Its size is the target's MAX_EDICTS,
// checked against phys_target.c's on every call.
//
// FLOAT ARITHMETIC
//
// As in sv_phys.rs: IEEE semantics of the C source, double where the C mixes
// in a double (host_frametime, sv.time, DIST_EPSILON, the 1.0/64 snap).

use core::ffi::{c_char, c_int};
#[cfg(not(feature = "world_trace_hw"))]
use core::ffi::c_uint;

use crate::mathlib::AngleVectors;
use crate::sv_phys::{dot, edict_to_prog, free, pf, v, vi, SV_PushEntity};
use crate::world_hull::{cvt_c, facts};
use crate::world_link::SV_LinkEdict;
use crate::world_move::SV_TestEntityPosition;

/// The target's MAX_EDICTS (quakedef.h), which sizes the moved lists.
#[cfg(not(feature = "world_trace_hw"))]
const MAX_EDICTS: usize = 32000;
#[cfg(feature = "world_trace_hw")]
const MAX_EDICTS: usize = 768;

/// `YAW`, mathlib.h.
const YAW: usize = 1;
/// quakedef.h's `#define DIST_EPSILON (0.03125)`, a double.
#[cfg(not(feature = "world_trace_hw"))]
const DIST_EPSILON: f64 = 0.03125;

extern "C" {
    fn Phys_HostFrametime() -> f64;
    fn Phys_EdictSize() -> c_int;
    fn Phys_NumEdicts() -> c_int;

    fn World_SvEdicts() -> *mut u8;
    fn World_SvTime() -> f64;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_GlobalOther() -> *mut c_int;
    fn World_GlobalTime() -> *mut f32;

    fn PR_ExecuteProgram(fnum: c_int, funcname: *const c_char);
    fn abort() -> !;
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    fn Phys_MaxClients() -> c_int;
    fn Phys_MapChecks() -> c_int;
    fn Phys_Elevators() -> f32;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn PR_GetString(num: c_int) -> *const c_char;
    fn NUM_FOR_EDICT(e: *mut u8) -> c_int;
}

/// A moved list: who was moved, and where from.
struct Moved {
    edict: [*mut u8; MAX_EDICTS],
    from: [[f32; 3]; MAX_EDICTS],
}

impl Moved {
    const fn new() -> Self {
        Moved { edict: [core::ptr::null_mut(); MAX_EDICTS], from: [[0.0; 3]; MAX_EDICTS] }
    }
}

/// One list per function, so no two can share one (see the header).
#[cfg(not(feature = "world_trace_hw"))]
static mut PUSHMOVE_MOVED: Moved = Moved::new();
static mut PUSHROTATE_MOVED: Moved = Moved::new();
#[cfg(feature = "world_trace_hw")]
static mut PUSH_MOVED: Moved = Moved::new();

/// The list's size must be the target's MAX_EDICTS; a mismatch would let a
/// level with more entities than the list write past it.
#[inline(always)]
unsafe fn check_max_edicts() {
    if pf().max_edicts as usize != MAX_EDICTS {
        abort();
    }
}

/// `NEXT_EDICT(e)`.
#[inline(always)]
unsafe fn next_edict(e: *mut u8) -> *mut u8 {
    e.add(Phys_EdictSize() as usize)
}

/// `PROG_TO_EDICT(e)`.
#[inline(always)]
unsafe fn prog_to_edict(e: c_int) -> *mut u8 {
    World_SvEdicts().wrapping_offset(e as isize)
}

#[inline(always)]
unsafe fn flags(ent: *mut u8) -> c_int {
    cvt_c(*v(ent, facts().v_flags))
}

#[inline(always)]
unsafe fn copy3(dst: *mut f32, src: *const f32) {
    for i in 0..3 {
        *dst.add(i) = *src.add(i);
    }
}

/// The skip test both pushers start the entity walk with.  MOVETYPE_FOLLOW
/// is in both SV_PushRotate's, and in no SV_PushMove's (Hexen II's has it
/// under QUAKE2, HexenWorld's SV_Push not at all).
#[inline(always)]
unsafe fn skip_movetype(check: *mut u8, follow: bool) -> bool {
    let mt = *v(check, facts().v_movetype);
    let p = pf();
    mt == facts().movetype_push as f32
        || mt == p.movetype_none as f32
        || (follow && mt == p.movetype_follow as f32)
        || mt == p.movetype_noclip as f32
}

/// `check` misses the box mins..maxs, as the C's six-way test has it.
#[inline(always)]
unsafe fn outside(check: *mut u8, mins: &[f32; 3], maxs: &[f32; 3]) -> bool {
    let f = facts();
    let amin = v(check, f.v_absmin);
    let amax = v(check, f.v_absmax);
    *amin.add(0) >= maxs[0]
        || *amin.add(1) >= maxs[1]
        || *amin.add(2) >= maxs[2]
        || *amax.add(0) <= mins[0]
        || *amax.add(1) <= mins[1]
        || *amax.add(2) <= mins[2]
}

/// `check` stands on `pusher`.
#[inline(always)]
unsafe fn riding_on(check: *mut u8, pusher: *mut u8) -> bool {
    (flags(check) & facts().fl_onground) != 0 && prog_to_edict(*vi(check, facts().v_groundentity)) == pusher
}

/// The corpse rule: `mins[0] = mins[1] = 0; VectorCopy (mins, maxs);`.
#[inline(always)]
unsafe fn flatten(check: *mut u8) {
    let f = facts();
    let mins = v(check, f.v_mins);
    *mins.add(0) = 0.0;
    *mins.add(1) = 0.0;
    copy3(v(check, f.v_maxs), mins);
}

/// Run the pusher's `blocked' function, if it has one.
unsafe fn run_blocked(pusher: *mut u8, check: *mut u8) {
    let blocked = *vi(pusher, pf().v_blocked);
    if blocked != 0 {
        *World_GlobalSelf() = edict_to_prog(pusher);
        *World_GlobalOther() = edict_to_prog(check);
        PR_ExecuteProgram(blocked, c"blocked".as_ptr());
    }
}

/// `static void SV_PushMove (edict_t *pusher, float movetime, qboolean
/// update_time)`, Hexen II's.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_PushMove(pusher: *mut u8, movetime: f32, update_time: c_int) {
    check_max_edicts();
    let f = facts();
    let p = pf();
    let velocity = v(pusher, p.v_velocity);
    let ltime = v(pusher, p.v_ltime);
    let porigin = v(pusher, f.v_origin);
    let moved = &mut *(&raw mut PUSHMOVE_MOVED);

    if *velocity.add(0) == 0.0 && *velocity.add(1) == 0.0 && *velocity.add(2) == 0.0 {
        if update_time != 0 {
            *ltime += movetime;
        }
        return;
    }

    let mut move_ = [0.0f32; 3];
    let mut mins = [0.0f32; 3];
    let mut maxs = [0.0f32; 3];
    for i in 0..3 {
        move_[i] = *velocity.add(i) * movetime;
        mins[i] = *v(pusher, f.v_absmin).add(i) + move_[i];
        maxs[i] = *v(pusher, f.v_absmax).add(i) + move_[i];
    }

    let mut pushorig = [0.0f32; 3];
    copy3(pushorig.as_mut_ptr(), porigin);

    // move the pusher to it's final position
    for i in 0..3 {
        *porigin.add(i) = *porigin.add(i) + move_[i];
    }
    if update_time != 0 {
        *ltime += movetime;
    }
    SV_LinkEdict(pusher, 0);

    // Inky: No collision check if the moving volume is liquid (rising
    // water/lava)
    if *v(pusher, p.v_watertype) != 0.0 {
        return;
    }

    // see if any solid entities are inside the final position
    let mut num_moved = 0usize;
    let mut check = next_edict(World_SvEdicts());
    let mut e: c_int = 1;
    while e < Phys_NumEdicts() {
        'next: {
            if free(check) || skip_movetype(check, false) {
                break 'next;
            }

            // if the entity is standing on the pusher, it will definately be
            // moved
            let riding = if !riding_on(check, pusher) {
                if outside(check, &mins, &maxs) {
                    break 'next;
                }
                // see if the ent's bbox is inside the pusher's final position
                if SV_TestEntityPosition(check).is_null() {
                    break 'next;
                }
                false
            } else {
                true
            };

            // remove the onground flag for non-players
            if *v(check, f.v_movetype) != p.movetype_walk as f32 {
                *v(check, f.v_flags) = (flags(check) & !f.fl_onground) as f32;
            }

            let corigin = v(check, f.v_origin);
            let mut entorig = [0.0f32; 3];
            copy3(entorig.as_mut_ptr(), corigin);
            copy3(moved.from[num_moved].as_mut_ptr(), corigin);
            moved.edict[num_moved] = check;
            num_moved += 1;

            // try moving the contacted entity, with the pusher's own solid
            // put back after (uhexen2-3aus)
            let psolid = v(pusher, f.v_solid);
            let solid_backup = *psolid;
            *psolid = f.solid_not as f32;
            SV_PushEntity(check, move_.as_ptr());
            *psolid = solid_backup;

            // if it is still inside the pusher, block
            let block = SV_TestEntityPosition(check);
            if !block.is_null() {
                // fail the move
                if *v(check, f.v_mins) == *v(check, f.v_maxs) {
                    break 'next;
                }
                let csolid = *v(check, f.v_solid);
                if csolid == f.solid_not as f32 || csolid == f.solid_trigger as f32 {
                    // corpse
                    flatten(check);
                    break 'next;
                }

                // the lift case: a rider blocked by the platform carrying it
                // is lifted one collision epsilon (uhexen2-a5nn.7)
                let elevators = Phys_Elevators();
                if riding && block == pusher && (elevators >= 2.0 || (elevators != 0.0 && e <= Phys_MaxClients())) {
                    *corigin.add(2) = (*corigin.add(2) as f64 + DIST_EPSILON) as f32;
                    if SV_TestEntityPosition(check).is_null() {
                        let flags = if Phys_MapChecks() != 0 { 0 } else { 2 };
                        CON_Printf(
                            flags,
                            c"sv_gameplayfix_elevators nudged %s #%d above %s #%d\n".as_ptr(),
                            PR_GetString(*vi(check, f.v_classname)),
                            NUM_FOR_EDICT(check),
                            PR_GetString(*vi(pusher, f.v_classname)),
                            NUM_FOR_EDICT(pusher),
                        );
                        break 'next;
                    }
                    // nudge did not help; the restore below undoes it
                }

                copy3(corigin, entorig.as_ptr());
                SV_LinkEdict(check, 1);

                copy3(porigin, pushorig.as_ptr());
                SV_LinkEdict(pusher, 0);
                if update_time != 0 {
                    *ltime -= movetime;
                }

                // if the pusher has a "blocked" function, call it otherwise,
                // just stay in place until the obstacle is gone
                run_blocked(pusher, check);

                // move back any entities we already moved
                for i in 0..num_moved {
                    copy3(v(moved.edict[i], f.v_origin), moved.from[i].as_ptr());
                    SV_LinkEdict(moved.edict[i], 0);
                }
                return;
            }
        }
        e += 1;
        check = next_edict(check);
    }
}

/// `static qboolean SV_Push (edict_t *pusher, vec3_t move)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_Push(pusher: *mut u8, move_: *const f32) -> c_int {
    check_max_edicts();
    let f = facts();
    let porigin = v(pusher, f.v_origin);
    let moved = &mut *(&raw mut PUSH_MOVED);

    let mut mins = [0.0f32; 3];
    let mut maxs = [0.0f32; 3];
    for i in 0..3 {
        mins[i] = *v(pusher, f.v_absmin).add(i) + *move_.add(i);
        maxs[i] = *v(pusher, f.v_absmax).add(i) + *move_.add(i);
    }

    let mut pushorig = [0.0f32; 3];
    copy3(pushorig.as_mut_ptr(), porigin);

    // move the pusher to it's final position
    for i in 0..3 {
        *porigin.add(i) = *porigin.add(i) + *move_.add(i);
    }
    SV_LinkEdict(pusher, 0);

    // see if any solid entities are inside the final position
    let mut num_moved = 0usize;
    let mut check = next_edict(World_SvEdicts());
    let mut e: c_int = 1;
    while e < Phys_NumEdicts() {
        'next: {
            if free(check) || skip_movetype(check, false) {
                break 'next;
            }

            *v(pusher, f.v_solid) = f.solid_not as f32;
            let block = SV_TestEntityPosition(check);
            *v(pusher, f.v_solid) = f.solid_bsp as f32;
            if !block.is_null() {
                break 'next;
            }

            // if the entity is standing on the pusher, it will definately be
            // moved
            if !riding_on(check, pusher) {
                if outside(check, &mins, &maxs) {
                    break 'next;
                }
                // see if the ent's bbox is inside the pusher's final position
                if SV_TestEntityPosition(check).is_null() {
                    break 'next;
                }
            }

            let corigin = v(check, f.v_origin);
            copy3(moved.from[num_moved].as_mut_ptr(), corigin);
            moved.edict[num_moved] = check;
            num_moved += 1;

            // try moving the contacted entity
            for i in 0..3 {
                *corigin.add(i) = *corigin.add(i) + *move_.add(i);
            }
            if SV_TestEntityPosition(check).is_null() {
                // pushed ok
                SV_LinkEdict(check, 0);
                break 'next;
            }

            // if it is ok to leave in the old position, do it
            for i in 0..3 {
                *corigin.add(i) = *corigin.add(i) - *move_.add(i);
            }
            if SV_TestEntityPosition(check).is_null() {
                num_moved -= 1;
                break 'next;
            }

            // if it is still inside the pusher, block
            if *v(check, f.v_mins) == *v(check, f.v_maxs) {
                SV_LinkEdict(check, 0);
                break 'next;
            }
            let csolid = *v(check, f.v_solid);
            if csolid == f.solid_not as f32 || csolid == f.solid_trigger as f32 {
                // corpse
                flatten(check);
                SV_LinkEdict(check, 0);
                break 'next;
            }

            copy3(porigin, pushorig.as_ptr());
            SV_LinkEdict(pusher, 0);

            // if the pusher has a "blocked" function, call it otherwise,
            // just stay in place until the obstacle is gone
            run_blocked(pusher, check);

            // move back any entities we already moved
            for i in 0..num_moved {
                copy3(v(moved.edict[i], f.v_origin), moved.from[i].as_ptr());
                SV_LinkEdict(moved.edict[i], 0);
            }
            return 0;
        }
        e += 1;
        check = next_edict(check);
    }

    1
}

/// `static void SV_PushMove (edict_t *pusher, float movetime, qboolean
/// update_time)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_PushMove(pusher: *mut u8, movetime: f32, update_time: c_int) {
    let p = pf();
    let velocity = v(pusher, p.v_velocity);
    let ltime = v(pusher, p.v_ltime);

    if *velocity.add(0) == 0.0 && *velocity.add(1) == 0.0 && *velocity.add(2) == 0.0 {
        if update_time != 0 {
            *ltime += movetime;
        }
        return;
    }

    let mut move_ = [0.0f32; 3];
    for i in 0..3 {
        move_[i] = *velocity.add(i) * movetime;
    }

    if SV_Push(pusher, move_.as_ptr()) != 0 && update_time != 0 {
        *ltime += movetime;
    }
}

/// `static void SV_PushRotate (edict_t *pusher, float movetime)`, the live
/// ("NEW") one; the "Pre-Mission Pack fix" copy above it in both originals
/// is under `#if 0`.
#[no_mangle]
pub unsafe extern "C" fn SV_PushRotate(pusher: *mut u8, movetime: f32) {
    check_max_edicts();
    let f = facts();
    let p = pf();
    let moved = &mut *(&raw mut PUSHROTATE_MOVED);
    let porigin = v(pusher, f.v_origin);
    let pangles = v(pusher, f.v_angles);

    let mut amove = [0.0f32; 3];
    let mut move_ = [0.0f32; 3];
    let mut mins = [0.0f32; 3];
    let mut maxs = [0.0f32; 3];
    for i in 0..3 {
        amove[i] = *v(pusher, p.v_avelocity).add(i) * movetime;
        move_[i] = *v(pusher, p.v_velocity).add(i) * movetime;
        mins[i] = *v(pusher, f.v_absmin).add(i) + move_[i];
        maxs[i] = *v(pusher, f.v_absmax).add(i) + move_[i];
    }

    let a = [-amove[0], -amove[1], -amove[2]];
    let mut forward = [0.0f32; 3];
    let mut right = [0.0f32; 3];
    let mut up = [0.0f32; 3];
    AngleVectors(a.as_ptr(), forward.as_mut_ptr(), right.as_mut_ptr(), up.as_mut_ptr());

    let mut pushorig = [0.0f32; 3];
    let mut pushorigangles = [0.0f32; 3];
    copy3(pushorig.as_mut_ptr(), porigin);
    copy3(pushorigangles.as_mut_ptr(), pangles);

    // move the pusher to it's final position
    for i in 0..3 {
        *porigin.add(i) = *porigin.add(i) + move_[i];
    }
    for i in 0..3 {
        *pangles.add(i) = *pangles.add(i) + amove[i];
    }

    *v(pusher, p.v_ltime) += movetime;
    SV_LinkEdict(pusher, 0);

    // The C fills a "slave" list here in a block that is commented out, so
    // slaves_moved stays 0 and every loop over it below runs no times.
    const SLAVES_MOVED: usize = 0;

    // see if any solid entities are inside the final position
    let mut num_moved = 0usize;
    let mut check = next_edict(World_SvEdicts());
    let mut testmove = [0.0f32; 3];
    let mut e: c_int = 1;
    while e < Phys_NumEdicts() {
        'next: {
            if free(check) || skip_movetype(check, true) {
                break 'next;
            }

            // if the entity is standing on the pusher, it will definitely be
            // moved
            let mut moveit = false;
            let ground = prog_to_edict(*vi(check, f.v_groundentity));
            if (flags(check) & f.fl_onground) != 0 {
                if ground == pusher {
                    moveit = true;
                } else {
                    for i in 0..SLAVES_MOVED {
                        if ground == moved.edict[MAX_EDICTS - i - 1] {
                            moveit = true;
                            break;
                        }
                    }
                }
            }

            if !moveit {
                if outside(check, &mins, &maxs) {
                    // the C then tests each slave's box, and with none
                    // reaches `if (i == slaves_moved) continue;'
                    break 'next;
                }
                // see if the ent's bbox is inside the pusher's final position
                if SV_TestEntityPosition(check).is_null() {
                    break 'next;
                }
            }

            // remove the onground flag for non-players
            if *v(check, f.v_movetype) != p.movetype_walk as f32 {
                *v(check, f.v_flags) = (flags(check) & !f.fl_onground) as f32;
            }

            let corigin = v(check, f.v_origin);
            let mut entorig = [0.0f32; 3];
            copy3(entorig.as_mut_ptr(), corigin);
            copy3(moved.from[num_moved].as_mut_ptr(), corigin);
            moved.edict[num_moved] = check;
            num_moved += 1;

            // put check in first move spot
            for i in 0..3 {
                *corigin.add(i) = *corigin.add(i) + move_[i];
            }
            // Use center of model, like in QUAKE!!!!  Our origins are on the
            // bottom!!!  `(absmin + absmax) / 2`, in float
            let mut check_center = [0.0f32; 3];
            for i in 0..3 {
                check_center[i] = (*v(check, f.v_absmin).add(i) + *v(check, f.v_absmax).add(i)) / 2.0;
            }
            // calculate destination position
            let mut org = [0.0f32; 3];
            for i in 0..3 {
                org[i] = check_center[i] - *porigin.add(i);
            }
            // put check back
            for i in 0..3 {
                *corigin.add(i) = *corigin.add(i) - move_[i];
            }
            let org2 = [
                dot(org.as_ptr(), forward.as_ptr()),
                -dot(org.as_ptr(), right.as_ptr()),
                dot(org.as_ptr(), up.as_ptr()),
            ];
            let mut move2 = [0.0f32; 3];
            for i in 0..3 {
                move2[i] = org2[i] - org[i];
            }

            // Add all moves together
            let mut move3 = [0.0f32; 3];
            for i in 0..3 {
                move3[i] = move_[i] + move2[i];
            }

            // try moving the contacted entity
            let mut block: *mut u8 = core::ptr::null_mut();
            for t in 0..13 {
                // `VectorSubtract (check->v.origin, testmove, check->v.origin)`
                let undo = |testmove: &[f32; 3]| {
                    for i in 0..3 {
                        *corigin.add(i) = *corigin.add(i) - testmove[i];
                    }
                };
                match t {
                    0 => {
                        // try x, y and z
                        testmove = move3;
                    }
                    1 => {
                        // Try xy only
                        undo(&testmove);
                        testmove = [move3[0], move3[1], 0.0];
                    }
                    2 => {
                        // Try z only
                        undo(&testmove);
                        testmove = [0.0, 0.0, move3[2]];
                    }
                    3 => {
                        // Try none
                        undo(&testmove);
                        testmove = [0.0, 0.0, 0.0];
                    }
                    4 => {
                        // Try xy in opposite dir: the C does not undo here
                        testmove = [move3[0] * -1.0, move3[1] * -1.0, move3[2]];
                    }
                    5 => {
                        // Try z in opposite dir
                        undo(&testmove);
                        testmove = [move3[0], move3[1], move3[2] * -1.0];
                    }
                    6 => {
                        // Try xyz in opposite dir
                        undo(&testmove);
                        testmove = [move3[0] * -1.0, move3[1] * -1.0, move3[2] * -1.0];
                    }
                    7 => {
                        // Try move3 times 2
                        undo(&testmove);
                        testmove = [move3[0] * 2.0, move3[1] * 2.0, move3[2] * 2.0];
                    }
                    8 => {
                        // Try normalized org: `VectorScale (org, movetime,
                        // org)', which the later cases read
                        undo(&testmove);
                        for i in 0..3 {
                            org[i] = org[i] * movetime;
                        }
                        testmove = org;
                    }
                    9 => {
                        // Try normalized org z * 3 only
                        undo(&testmove);
                        testmove = [0.0, 0.0, org[2] * 3.0];
                    }
                    10 => {
                        // Try normalized org xy * 2 only
                        undo(&testmove);
                        testmove = [org[0] * 2.0, org[1] * 2.0, 0.0];
                    }
                    11 => {
                        // Try xy in opposite org dir
                        undo(&testmove);
                        testmove = [org[0] * -2.0, org[1] * -2.0, org[2]];
                    }
                    _ => {
                        // Try z in opposite dir
                        undo(&testmove);
                        testmove = [org[0], org[1], org[2] * -3.0];
                    }
                }

                if t != 3 {
                    // THIS IS VERY BAD BAD HACK...  The C pushes by move3 on
                    // every try; testmove only decides what the next try
                    // subtracts.
                    let psolid = v(pusher, f.v_solid);
                    #[cfg(not(feature = "world_trace_hw"))]
                    let solid_backup = *psolid;
                    *psolid = f.solid_not as f32;
                    SV_PushEntity(check, move3.as_ptr());
                    *v(check, f.v_angles).add(YAW) += amove[YAW];
                    // see the note in SV_PushMove -- uhexen2-3aus
                    #[cfg(not(feature = "world_trace_hw"))]
                    {
                        *psolid = solid_backup;
                    }
                    #[cfg(feature = "world_trace_hw")]
                    {
                        *psolid = f.solid_bsp as f32;
                    }
                }
                // if it is still inside the pusher, block
                block = SV_TestEntityPosition(check);
                if block.is_null() {
                    break;
                }
            }

            if !block.is_null() {
                // fail the move
                if *v(check, f.v_mins) == *v(check, f.v_maxs) {
                    break 'next;
                }
                let csolid = *v(check, f.v_solid);
                if csolid == f.solid_not as f32 || csolid == f.solid_trigger as f32 {
                    // corpse
                    flatten(check);
                    break 'next;
                }

                copy3(corigin, entorig.as_ptr());
                SV_LinkEdict(check, 1);

                copy3(porigin, pushorig.as_ptr());
                copy3(pangles, pushorigangles.as_ptr());
                SV_LinkEdict(pusher, 0);
                *v(pusher, p.v_ltime) -= movetime;

                for i in 0..SLAVES_MOVED {
                    let slave = moved.edict[MAX_EDICTS - i - 1];
                    copy3(v(slave, f.v_angles), moved.from[MAX_EDICTS - i - 1].as_ptr());
                    SV_LinkEdict(slave, 0);
                    *v(slave, p.v_ltime) -= movetime;
                }

                // if the pusher has a "blocked" function, call it otherwise,
                // just stay in place until the obstacle is gone
                run_blocked(pusher, check);

                // move back any entities we already moved
                for i in 0..num_moved {
                    copy3(v(moved.edict[i], f.v_origin), moved.from[i].as_ptr());
                    *v(moved.edict[i], f.v_angles).add(YAW) -= amove[YAW];
                    SV_LinkEdict(moved.edict[i], 0);
                }
                return;
            }
        }
        e += 1;
        check = next_edict(check);
    }
}

/// `static void SV_Physics_Pusher (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Pusher(ent: *mut u8) {
    let p = pf();
    let ltime = v(ent, p.v_ltime);
    let oldltime = *ltime;

    let thinktime = *v(ent, p.v_nextthink);
    // `thinktime < ent->v.ltime + host_frametime', in double
    let movetime: f32 = if (thinktime as f64) < *ltime as f64 + Phys_HostFrametime() {
        let m = thinktime - *ltime;
        if m < 0.0 {
            0.0
        } else {
            m
        }
    } else {
        Phys_HostFrametime() as f32
    };

    if movetime != 0.0 {
        let av = v(ent, p.v_avelocity);
        if *av.add(0) != 0.0 || *av.add(1) != 0.0 || *av.add(2) != 0.0 {
            SV_PushRotate(ent, movetime);
        } else {
            SV_PushMove(ent, movetime, 1); // advances ent->v.ltime if not blocked
        }
    }

    if thinktime > oldltime && thinktime <= *ltime {
        #[cfg(feature = "world_trace_hw")]
        let mut oldorg = [0.0f32; 3];
        #[cfg(feature = "world_trace_hw")]
        copy3(oldorg.as_mut_ptr(), v(ent, facts().v_origin));

        *v(ent, p.v_nextthink) = 0.0;
        *World_GlobalTime() = World_SvTime() as f32;
        *World_GlobalSelf() = edict_to_prog(ent);
        *World_GlobalOther() = edict_to_prog(World_SvEdicts());
        #[cfg(not(feature = "world_trace_hw"))]
        PR_ExecuteProgram(*vi(ent, p.v_think), c"think in SV_RunThink".as_ptr());
        #[cfg(feature = "world_trace_hw")]
        PR_ExecuteProgram(*vi(ent, p.v_think), c"think in SV_Physics_Pusher".as_ptr());
        if free(ent) {
            return;
        }

        #[cfg(feature = "world_trace_hw")]
        {
            let origin = v(ent, facts().v_origin);
            let mut move_ = [0.0f32; 3];
            for i in 0..3 {
                move_[i] = *origin.add(i) - oldorg[i];
            }
            // `l = VectorLength (move)': sqrt of the float dot product, in
            // double, stored to a float, then compared with the double 1.0/64
            let l = sqrt(dot(move_.as_ptr(), move_.as_ptr()) as f64) as f32;
            if l as f64 > 1.0 / 64.0 {
                copy3(origin, oldorg.as_ptr());
                SV_Push(ent, move_.as_ptr());
            }
        }
    }
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    fn sqrt(x: f64) -> f64;
}
