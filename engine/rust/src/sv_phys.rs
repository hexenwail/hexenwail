// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the base helpers of engine/hexen2/sv_phys.c and
// engine/hexenworld/server/sv_phys.c: SV_CheckVelocity, SV_RunThink,
// SV_Impact, ClipVelocity, SV_FlyMove, SV_FlyExtras, SV_AddGravity and
// SV_PushEntity.  The first sv_phys.c slice of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// These are what every movetype's physics is built from: bound a velocity,
// run a think, run two entities' touch functions, slide a velocity off a
// plane, slide a box along up to four planes (SV_FlyMove), hover, fall, and
// push an entity through the world (SV_PushEntity).  The rest of sv_phys.c
// -- the pushers, walking, and the per-movetype dispatch -- stays C for now
// and calls these.  The C originals moved verbatim into
// engine/rust/sv_phys/tests/.
//
// THE TWO C ORIGINALS
//
// In Hexen II these were static; HexenWorld already exported five of them
// (sv_user.c runs SV_RunThink for clients).  They differ three ways, each
// selected by world_trace_hw as for the world.c ports:
//
//   * SV_CheckVelocity's NaN reports: Con_DPrintf in Hexen II, Con_Printf in
//     HexenWorld;
//   * SV_FlyMove's fatal `!trace.ent': Sys_Error, HexenWorld's SV_Error;
//   * SV_AddGravity: Hexen II takes the entity's own `gravity' field (1 when
//     absent or 0) times sv_gravity; HexenWorld takes a scale argument times
//     movevars.gravity.
//
// SV_RunThink is written differently in the two but decides the same way.
// HexenWorld's SV_FlyExtras is under `#if 0` with its one caller, so only
// Hexen II's is ported.
//
// LAYOUTS AND C STATE
//
// The entity layout comes through world_target.c, as for the world.c ports,
// plus engine/rust/phys_target.c for the fields and globals only physics
// reads: velocity, nextthink, think, hoverz, host_frametime, sv_player,
// sv_maxvelocity and the gravity.
//
// FLOAT ARITHMETIC
//
// The contract is the C source's IEEE semantics (world_trace.rs).  Where the
// C mixes a float with a double -- sv.time and host_frametime are double,
// and the literals `.1', `0.7' and STOP_EPSILON are double -- the
// arithmetic here is done in f64 at the same points and rounded back where
// the C stores to a float.  IS_NAN is mathlib.h's bit test, which is true
// for infinities too.

use core::ffi::{c_char, c_int, c_uint};

use crate::world_hull::{cvt_c, facts};
use crate::world_link::SV_LinkEdict;
use crate::world_move::SV_Move;
use crate::world_trace::Trace;

/// engine/rust/phys_target.h's `phys_target_facts_t`.
#[repr(C)]
pub struct PhysTargetFacts {
    pub v_velocity: usize,
    pub v_nextthink: usize,
    pub v_think: usize,
    pub v_hoverz: usize,
    pub solid_phase: c_int,
    pub movetype_walk: c_int,
    pub movetype_flymissile: c_int,
    pub movetype_bounce: c_int,
    pub movetype_bouncemissile: c_int,
    pub movetype_swim: c_int,
    pub v_ltime: usize,
    pub v_avelocity: usize,
    pub v_watertype: usize,
    pub v_blocked: usize,
    pub movetype_none: c_int,
    pub movetype_noclip: c_int,
    pub movetype_follow: c_int,
    pub max_edicts: c_int,
    pub v_oldorigin: usize,
    pub v_waterlevel: usize,
    pub v_v_angle: usize,
    pub fl_waterjump: c_int,
    pub movetype_step: c_int,
    pub movetype_toss: c_int,
    pub movetype_fly: c_int,
}

#[no_mangle]
pub extern "C" fn PhysTargetFacts_sizeof() -> usize {
    core::mem::size_of::<PhysTargetFacts>()
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_v_hoverz() -> usize {
    core::mem::offset_of!(PhysTargetFacts, v_hoverz)
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_movetype_swim() -> usize {
    core::mem::offset_of!(PhysTargetFacts, movetype_swim)
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_v_blocked() -> usize {
    core::mem::offset_of!(PhysTargetFacts, v_blocked)
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_max_edicts() -> usize {
    core::mem::offset_of!(PhysTargetFacts, max_edicts)
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_v_v_angle() -> usize {
    core::mem::offset_of!(PhysTargetFacts, v_v_angle)
}
#[no_mangle]
pub extern "C" fn PhysTargetFacts_offsetof_movetype_fly() -> usize {
    core::mem::offset_of!(PhysTargetFacts, movetype_fly)
}

/// printsys.h: Hexen II's NaN reports are Con_DPrintf (_PRINT_DEVEL),
/// HexenWorld's Con_Printf (_PRINT_NORMAL).
#[cfg(not(feature = "world_trace_hw"))]
const NAN_PRINT: c_uint = 2;
#[cfg(feature = "world_trace_hw")]
const NAN_PRINT: c_uint = 0;
/// sv_phys.c's `#define STOP_EPSILON 0.1`, a double.
const STOP_EPSILON: f64 = 0.1;
/// sv_phys.c's `#define MAX_CLIP_PLANES 5`.
const MAX_CLIP_PLANES: usize = 5;
/// sv_phys.c's `static const float hoverinc = 0.4;`.
#[cfg(not(feature = "world_trace_hw"))]
const HOVERINC: f32 = 0.4;
/// mathlib.h's `nanmask`.
const NANMASK: u32 = 255 << 23;

extern "C" {
    static Phys_TargetFacts: PhysTargetFacts;
    fn Phys_HostFrametime() -> f64;
    fn Phys_MaxVelocity() -> f32;
    fn Phys_Gravity() -> f32;

    fn World_SvEdicts() -> *mut u8;
    fn World_SvTime() -> f64;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_GlobalOther() -> *mut c_int;
    fn World_GlobalTime() -> *mut f32;

    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn PR_GetString(num: c_int) -> *const c_char;
    fn PR_ExecuteProgram(fnum: c_int, funcname: *const c_char);
    fn sqrt(x: f64) -> f64;
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    /// SV_FlyExtras' only use of it
    fn Phys_SvPlayer() -> *mut u8;
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    /// progs.h: `eval_t *GetEdictFieldValue (edict_t *ed, const char *field)`.
    /// eval_t is a union whose `_float` is at offset 0.
    fn GetEdictFieldValue(ed: *mut u8, field: *const c_char) -> *mut f32;
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    fn SV_Error(fmt: *const c_char, ...) -> !;
}

#[inline(always)]
pub(crate) unsafe fn pf() -> &'static PhysTargetFacts {
    &*(&raw const Phys_TargetFacts)
}

#[inline(always)]
pub(crate) unsafe fn v(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
pub(crate) unsafe fn vi(ent: *mut u8, member: usize) -> *mut c_int {
    v(ent, member).cast::<c_int>()
}

#[inline(always)]
pub(crate) unsafe fn free(ent: *mut u8) -> bool {
    *ent.add(facts().edict_free).cast::<c_int>() != 0
}

/// `EDICT_TO_PROG(e)`.
#[inline(always)]
pub(crate) unsafe fn edict_to_prog(e: *mut u8) -> c_int {
    (e as isize).wrapping_sub(World_SvEdicts() as isize) as c_int
}

/// mathlib.h's IS_NAN: every exponent bit set, so infinities count.
#[inline(always)]
fn is_nan(x: f32) -> bool {
    (x.to_bits() & NANMASK) == NANMASK
}

/// `DotProduct`, in float, left to right as the macro expands.
#[inline(always)]
pub(crate) unsafe fn dot(a: *const f32, b: *const f32) -> f32 {
    *a * *b + *a.add(1) * *b.add(1) + *a.add(2) * *b.add(2)
}

/// `void SV_CheckVelocity (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_CheckVelocity(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let velocity = v(ent, p.v_velocity);
    let origin = v(ent, f.v_origin);

    // bound velocity
    for i in 0..3 {
        if is_nan(*velocity.add(i)) {
            CON_Printf(NAN_PRINT, c"Got a NaN velocity on %s\n".as_ptr(), PR_GetString(*vi(ent, f.v_classname)));
            *velocity.add(i) = 0.0;
        }
        if is_nan(*origin.add(i)) {
            CON_Printf(NAN_PRINT, c"Got a NaN origin on %s\n".as_ptr(), PR_GetString(*vi(ent, f.v_classname)));
            *origin.add(i) = 0.0;
        }
    }

    // `w = VectorLength(ent->v.velocity);`: sqrt of the float dot product,
    // in double, stored to a float
    let w = sqrt(dot(velocity, velocity) as f64) as f32;
    if w > Phys_MaxVelocity() {
        // sv_maxvelocity fix by Maddes
        let scale = Phys_MaxVelocity() / w;
        for i in 0..3 {
            *velocity.add(i) = *velocity.add(i) * scale;
        }
    }
}

/// `qboolean SV_RunThink (edict_t *ent)`.  Returns false if the entity
/// removed itself.
#[no_mangle]
pub unsafe extern "C" fn SV_RunThink(ent: *mut u8) -> c_int {
    let p = pf();
    let nextthink = v(ent, p.v_nextthink);
    let mut thinktime = *nextthink;

    // `thinktime > sv.time + host_frametime`, in double
    if thinktime <= 0.0 || thinktime as f64 > World_SvTime() + Phys_HostFrametime() {
        return 1;
    }

    if (thinktime as f64) < World_SvTime() {
        // don't let things stay in the past.  it is possible to start that
        // way by a trigger with a local time.
        thinktime = World_SvTime() as f32;
    }
    *nextthink = 0.0;
    *World_GlobalTime() = thinktime;
    *World_GlobalSelf() = edict_to_prog(ent);
    *World_GlobalOther() = edict_to_prog(World_SvEdicts());
    PR_ExecuteProgram(*vi(ent, p.v_think), c"think in SV_RunThink".as_ptr());

    (!free(ent)) as c_int
}

/// `void SV_Impact (edict_t *e1, edict_t *e2)`: two entities have touched,
/// so run their touch functions.
#[no_mangle]
pub unsafe extern "C" fn SV_Impact(e1: *mut u8, e2: *mut u8) {
    let f = facts();
    let old_self = *World_GlobalSelf();
    let old_other = *World_GlobalOther();

    *World_GlobalTime() = World_SvTime() as f32;
    if *vi(e1, f.v_touch) != 0 && *v(e1, f.v_solid) != f.solid_not as f32 {
        *World_GlobalSelf() = edict_to_prog(e1);
        *World_GlobalOther() = edict_to_prog(e2);
        PR_ExecuteProgram(*vi(e1, f.v_touch), c"touch".as_ptr());
    }

    if *vi(e2, f.v_touch) != 0 && *v(e2, f.v_solid) != f.solid_not as f32 {
        *World_GlobalSelf() = edict_to_prog(e2);
        *World_GlobalOther() = edict_to_prog(e1);
        PR_ExecuteProgram(*vi(e2, f.v_touch), c"touch".as_ptr());
    }

    *World_GlobalSelf() = old_self;
    *World_GlobalOther() = old_other;
}

/// `int ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float
/// overbounce)`: slide off of the impacting object; returns the blocked
/// flags (1 = floor, 2 = step / wall).
#[no_mangle]
pub unsafe extern "C" fn ClipVelocity(in_: *const f32, normal: *const f32, out: *mut f32, overbounce: f32) -> c_int {
    let mut blocked = 0;
    if *normal.add(2) > 0.0 {
        blocked |= 1; // floor
    }
    if *normal.add(2) == 0.0 {
        blocked |= 2; // step
    }

    let backoff = dot(in_, normal) * overbounce;

    for i in 0..3 {
        let change = *normal.add(i) * backoff;
        *out.add(i) = *in_.add(i) - change;
        let o = *out.add(i) as f64;
        if o > -STOP_EPSILON && o < STOP_EPSILON {
            *out.add(i) = 0.0;
        }
    }

    blocked
}

/// `SV_FlyMove`'s fatal, by the target's error function.
#[cold]
unsafe fn no_trace_ent() -> ! {
    #[cfg(not(feature = "world_trace_hw"))]
    Sys_Error(c"%s: !trace.ent".as_ptr(), c"SV_FlyMove".as_ptr());
    #[cfg(feature = "world_trace_hw")]
    SV_Error(c"%s: !trace.ent".as_ptr(), c"SV_FlyMove".as_ptr());
}

/// `int SV_FlyMove (edict_t *ent, float time, trace_t *steptrace)`: the
/// basic solid body movement clip that slides along multiple planes.
/// Returns the clipflags if the velocity was modified (hit something
/// solid): 1 = floor, 2 = wall / step, 4 = dead stop.  If steptrace is not
/// NULL, the trace of any vertical wall hit is stored there.
#[no_mangle]
pub unsafe extern "C" fn SV_FlyMove(ent: *mut u8, time: f32, steptrace: *mut Trace) -> c_int {
    let f = facts();
    let p = pf();
    let velocity = v(ent, p.v_velocity);
    let origin = v(ent, f.v_origin);

    let numbumps = 4;
    let mut blocked: c_int = 0;
    let mut original_velocity = [0.0f32; 3];
    let mut primal_velocity = [0.0f32; 3];
    let mut new_velocity = [0.0f32; 3];
    let mut planes = [[0.0f32; 3]; MAX_CLIP_PLANES];
    let mut end = [0.0f32; 3];
    for i in 0..3 {
        original_velocity[i] = *velocity.add(i);
    }
    for i in 0..3 {
        primal_velocity[i] = *velocity.add(i);
    }
    let mut numplanes = 0usize;
    let mut time_left = time;

    for _bumpcount in 0..numbumps {
        if *velocity.add(0) == 0.0 && *velocity.add(1) == 0.0 && *velocity.add(2) == 0.0 {
            break;
        }

        for i in 0..3 {
            end[i] = *origin.add(i) + time_left * *velocity.add(i);
        }

        let trace = SV_Move(origin, v(ent, f.v_mins), v(ent, f.v_maxs), end.as_mut_ptr(), 0, ent);

        if trace.allsolid != 0 {
            // entity is trapped in another solid
            for i in 0..3 {
                *velocity.add(i) = 0.0;
            }
            return 3;
        }

        if trace.fraction > 0.0 {
            // actually covered some distance
            for i in 0..3 {
                *origin.add(i) = trace.endpos[i];
            }
            for i in 0..3 {
                original_velocity[i] = *velocity.add(i);
            }
            numplanes = 0;
        }

        if trace.fraction == 1.0 {
            break; // moved the entire distance
        }

        if trace.ent.is_null() {
            no_trace_ent();
        }
        let tent = trace.ent.cast::<u8>();

        if trace.plane.normal[2] as f64 > 0.7 {
            blocked |= 1; // floor
            if *v(tent, f.v_solid) == f.solid_bsp as f32 {
                *v(ent, f.v_flags) = (cvt_c(*v(ent, f.v_flags)) | f.fl_onground) as f32;
                *vi(ent, f.v_groundentity) = edict_to_prog(tent);
            }
        }
        if trace.plane.normal[2] == 0.0 {
            blocked |= 2; // step
            if !steptrace.is_null() {
                core::ptr::copy_nonoverlapping(&trace, steptrace, 1); // save for player extrafriction
            }
        }

        // run the impact function
        SV_Impact(ent, tent);
        if free(ent) {
            break; // removed by the impact function
        }

        time_left -= time_left * trace.fraction;

        // cliped to another plane
        if numplanes >= MAX_CLIP_PLANES {
            // this shouldn't really happen
            for i in 0..3 {
                *velocity.add(i) = 0.0;
            }
            return 3;
        }

        planes[numplanes] = trace.plane.normal;
        numplanes += 1;

        // modify original_velocity so it parallels all of the clip planes
        let mut i = 0;
        while i < numplanes {
            ClipVelocity(original_velocity.as_ptr(), planes[i].as_ptr(), new_velocity.as_mut_ptr(), 1.0);
            let mut j = 0;
            while j < numplanes {
                if j != i && dot(new_velocity.as_ptr(), planes[j].as_ptr()) < 0.0 {
                    break; // not ok
                }
                j += 1;
            }

            if j == numplanes {
                break;
            }
            i += 1;
        }

        if i != numplanes {
            // go along this plane
            for k in 0..3 {
                *velocity.add(k) = new_velocity[k];
            }
        } else {
            // go along the crease
            if numplanes != 2 {
                for k in 0..3 {
                    *velocity.add(k) = 0.0;
                }
                return 7;
            }
            // `CrossProduct (planes[0], planes[1], dir)`
            let (a, b) = (planes[0], planes[1]);
            let dir = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
            let d = dot(dir.as_ptr(), velocity);
            for k in 0..3 {
                *velocity.add(k) = dir[k] * d;
            }
        }

        // if original velocity is against the original velocity, stop dead
        // to avoid tiny occilations in sloping corners
        if dot(velocity, primal_velocity.as_ptr()) <= 0.0 {
            for k in 0..3 {
                *velocity.add(k) = 0.0;
            }
            return blocked;
        }
    }

    blocked
}

/// `static void SV_FlyExtras (edict_t *ent, float time, trace_t
/// *steptrace)`: hover, and friction on vertical progress.  Hexen II's
/// alone: HexenWorld's is under `#if 0`, as is its one caller,
/// SV_Physics_Client.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_FlyExtras(ent: *mut u8, _time: f32, _steptrace: *mut Trace) {
    let f = facts();
    let p = pf();
    let vz = v(ent, p.v_velocity).add(2);
    let hoverz = v(ent, p.v_hoverz);

    // Jumping makes you loose this flag so reset it
    *v(ent, f.v_flags) = (cvt_c(*v(ent, f.v_flags)) | f.fl_onground) as f32;

    if *vz <= 6.0 && *vz >= -6.0 {
        *vz += *hoverz;

        if *vz >= 6.0 {
            *hoverz = -HOVERINC;
            *vz += *hoverz;
        } else if *vz <= -6.0 {
            *hoverz = HOVERINC;
            *vz += *hoverz;
        }
    } else {
        // friction for upward or downward progress once key is released:
        // `-= sv_player->v.velocity[2] * .1', in double
        let pv = *v(Phys_SvPlayer(), p.v_velocity).add(2);
        *vz = (*vz as f64 - pv as f64 * 0.1) as f32;
    }
}

/// `void SV_AddGravity (edict_t *ent)`.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_AddGravity(ent: *mut u8) {
    let p = pf();
    let val = GetEdictFieldValue(ent, c"gravity".as_ptr());
    let ent_gravity: f32 = if !val.is_null() && *val != 0.0 { *val } else { 1.0 };

    // `ent_gravity * sv_gravity.value * host_frametime`: a float product,
    // then double
    let vz = v(ent, p.v_velocity).add(2);
    *vz = (*vz as f64 - (ent_gravity * Phys_Gravity()) as f64 * Phys_HostFrametime()) as f32;
}

/// `void SV_AddGravity (edict_t *ent, float scale)`.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_AddGravity(ent: *mut u8, scale: f32) {
    let p = pf();
    let vz = v(ent, p.v_velocity).add(2);
    *vz = (*vz as f64 - (scale * Phys_Gravity()) as f64 * Phys_HostFrametime()) as f32;
}

/// `trace_t SV_PushEntity (edict_t *ent, vec3_t push)`.  Does not change
/// the entity's velocity at all.
#[no_mangle]
pub unsafe extern "C" fn SV_PushEntity(ent: *mut u8, push: *const f32) -> Trace {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let mins = v(ent, f.v_mins);
    let maxs = v(ent, f.v_maxs);
    let movetype = *v(ent, f.v_movetype);
    let solid = *v(ent, f.v_solid);
    let mut end = [0.0f32; 3];
    for i in 0..3 {
        end[i] = *origin.add(i) + *push.add(i);
    }

    let mut trace = if movetype == p.movetype_flymissile as f32 || movetype == p.movetype_bouncemissile as f32 {
        SV_Move(origin, mins, maxs, end.as_mut_ptr(), f.move_missile, ent)
    } else if solid == f.solid_trigger as f32 || solid == f.solid_not as f32 {
        // only clip against bmodels
        SV_Move(origin, mins, maxs, end.as_mut_ptr(), f.move_nomonsters, ent)
    } else if movetype == p.movetype_swim as f32 {
        SV_Move(origin, mins, maxs, end.as_mut_ptr(), f.move_water, ent)
    } else {
        SV_Move(origin, mins, maxs, end.as_mut_ptr(), 0, ent)
    };

    // the C reads ent->v.solid and movetype again here, after the move
    if *v(ent, f.v_solid) != p.solid_phase as f32 {
        if *v(ent, f.v_movetype) != p.movetype_bounce as f32 || (trace.allsolid == 0 && trace.startsolid == 0) {
            for i in 0..3 {
                *origin.add(i) = trace.endpos[i];
            }
        } else {
            trace.fraction = 0.0;
            return trace;
        }
    } else if !trace.ent.is_null() {
        // Entity is PHASED so bounce off walls and other entities, go
        // through monsters and players.  Can't use FL_CLIENT cause rotating
        // brushes do.
        let tent = trace.ent.cast::<u8>();
        if (cvt_c(*v(tent, f.v_flags)) & f.fl_monster) != 0 || *v(tent, f.v_movetype) == p.movetype_walk as f32 {
            let impact = trace.endpos;
            let impact_e = tent;

            trace = SV_Move(origin, mins, maxs, end.as_mut_ptr(), f.move_phase, ent);

            for i in 0..3 {
                *origin.add(i) = impact[i];
            }
            SV_Impact(ent, impact_e);

            for i in 0..3 {
                *origin.add(i) = trace.endpos[i];
            }
        } else {
            for i in 0..3 {
                *origin.add(i) = trace.endpos[i];
            }
        }
    } else {
        for i in 0..3 {
            *origin.add(i) = trace.endpos[i];
        }
    }

    SV_LinkEdict(ent, 1);

    if !trace.ent.is_null() {
        SV_Impact(ent, trace.ent.cast::<u8>());
    }

    trace
}
