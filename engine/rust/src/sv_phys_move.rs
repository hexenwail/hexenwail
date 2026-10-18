// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the movetype dispatch of engine/hexen2/sv_phys.c and
// engine/hexenworld/server/sv_phys.c: SV_Physics_None, SV_Physics_Noclip,
// SV_CheckWaterTransition, SV_Physics_Toss, SV_Physics_Step and SV_Physics,
// and HexenWorld's SV_ProgStartFrame, SV_RunEntity and SV_RunNewmis.  The
// fourth and last sv_phys.c slice of #455; after it sv_phys.c holds the
// physics cvars and HexenWorld's SV_SetMoveVars, which copies them into
// movevars.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_Physics is the server frame's entity walk: StartFrame, then each live
// entity's movetype physics, then the movechain that drags attached
// entities along.  The C originals moved verbatim into
// engine/rust/sv_phys/tests/.
//
// THE TWO C ORIGINALS
//
// Selected by world_trace_hw as in the earlier slices:
//
//   * SV_Physics: Hexen II's runs the clients through SV_Physics_Client,
//     caps the walk at the clients under sv_freezenonclients, skips an edict
//     spawned during this walk (#138), and advances sv.time by
//     host_frametime.  HexenWorld's is gated by sv_mintic/sv_maxtic on
//     realtime, writes the frametime global, skips the clients (they move
//     from packets), and runs each entity through SV_RunEntity -- once per
//     realtime (lastruntime) -- and then newmis, at host_frametime 0.05.
//   * SV_Physics_None: Hexen II thinks only if it has a think (mod compat);
//     HexenWorld always runs SV_RunThink.
//   * SV_Physics_Toss: HexenWorld clears FL_ONGROUND for an entity moving up
//     before the onground test, and passes SV_AddGravity its 1.0 scale.
//   * SV_Physics_Step: the landing-sound threshold is sv_gravity's in Hexen
//     II, movevars.gravity's in HexenWorld.
//
// SV_CheckWaterTransition's leaving-the-water branch stores the contents in
// waterlevel (`ent->v.waterlevel = cont'), in both; the port keeps it.
//
// FLOAT ARITHMETIC
//
// As in sv_phys.rs: IEEE semantics of the C source, double where the C mixes
// in a double (host_frametime, sv.time, realtime, -0.1 and 0.7, VectorMA and
// VectorScale with host_frametime), rounded back where the C stores a float.

use core::ffi::{c_char, c_int};
#[cfg(not(feature = "world_trace_hw"))]
use core::ffi::c_uint;

use crate::sv_phys::{edict_to_prog, free, pf, v, vi, ClipVelocity, SV_AddGravity, SV_CheckVelocity, SV_FlyMove, SV_PushEntity, SV_RunThink};
use crate::sv_phys_push::SV_Physics_Pusher;
use crate::world_hull::{cvt_c, facts};
use crate::world_link::SV_LinkEdict;
use crate::world_move::SV_PointContents;

#[cfg(not(feature = "world_trace_hw"))]
use crate::sv_phys_walk::SV_Physics_Client;

/// bspfile.h.
const CONTENTS_EMPTY: c_int = -1;
const CONTENTS_WATER: c_int = -3;

extern "C" {
    fn Phys_HostFrametime() -> f64;
    fn Phys_Gravity() -> f32;
    fn Phys_EdictSize() -> c_int;
    fn Phys_NumEdicts() -> c_int;
    fn Phys_ForceRetouch() -> *mut f32;
    fn Phys_StartFrame() -> c_int;

    fn World_SvEdicts() -> *mut u8;
    fn World_SvTime() -> f64;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_GlobalOther() -> *mut c_int;
    fn World_GlobalTime() -> *mut f32;

    fn PR_ExecuteProgram(fnum: c_int, funcname: *const c_char);
    fn SV_StartSound(entity: *mut u8, channel: c_int, sample: *const c_char, volume: c_int, attenuation: f32);
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    fn Phys_MaxClients() -> c_int;
    fn Phys_FreezeNonClients() -> c_int;
    fn Phys_PhysicsWalk() -> *mut c_int;
    fn Phys_SvTimePtr() -> *mut f64;
    fn Sys_Error(fmt: *const c_char, ...) -> !;
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    fn Phys_HostFrametimePtr() -> *mut f64;
    fn Phys_Realtime() -> f64;
    fn Phys_FrameTimeGlobal() -> *mut f32;
    fn Phys_Newmis() -> *mut c_int;
    fn Phys_MinTic() -> f32;
    fn Phys_MaxTic() -> f32;
    fn SV_Error(fmt: *const c_char, ...) -> !;
}

#[inline(always)]
unsafe fn flags(ent: *mut u8) -> c_int {
    cvt_c(*v(ent, facts().v_flags))
}

#[inline(always)]
unsafe fn next_edict(e: *mut u8) -> *mut u8 {
    e.add(Phys_EdictSize() as usize)
}

/// `PROG_TO_EDICT(e)`.
#[inline(always)]
unsafe fn prog_to_edict(e: c_int) -> *mut u8 {
    World_SvEdicts().wrapping_offset(e as isize)
}

/// `VectorMA (a, host_frametime, b, a)`: `a + host_frametime*b` in double.
#[inline(always)]
unsafe fn ma_frametime(a: *mut f32, b: *const f32) {
    for i in 0..3 {
        *a.add(i) = (*a.add(i) as f64 + Phys_HostFrametime() * *b.add(i) as f64) as f32;
    }
}

#[inline(always)]
unsafe fn equal3(a: *const f32, b: *const f32) -> bool {
    *a == *b && *a.add(1) == *b.add(1) && *a.add(2) == *b.add(2)
}

/// `static void SV_Physics_None (edict_t *ent)`: non moving objects can
/// only think.
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_None(ent: *mut u8) {
    // regular thinking -- Hexen II guards against a NULL think (mod compat)
    #[cfg(not(feature = "world_trace_hw"))]
    if *vi(ent, pf().v_think) != 0 {
        SV_RunThink(ent);
    }
    #[cfg(feature = "world_trace_hw")]
    SV_RunThink(ent);
}

/// `static void SV_Physics_Noclip (edict_t *ent)`: a moving object that
/// doesn't obey physics.
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Noclip(ent: *mut u8) {
    let f = facts();
    let p = pf();
    // regular thinking
    if SV_RunThink(ent) == 0 {
        return;
    }

    ma_frametime(v(ent, f.v_angles), v(ent, p.v_avelocity));
    ma_frametime(v(ent, f.v_origin), v(ent, p.v_velocity));

    SV_LinkEdict(ent, 0);
}

/// `static void SV_CheckWaterTransition (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_CheckWaterTransition(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let watertype = v(ent, p.v_watertype);
    let waterlevel = v(ent, p.v_waterlevel);

    let cont = SV_PointContents(v(ent, f.v_origin));
    if *watertype == 0.0 {
        // just spawned here
        *watertype = cont as f32;
        *waterlevel = 1.0;
        return;
    }

    if cont <= CONTENTS_WATER {
        if *watertype == CONTENTS_EMPTY as f32 {
            // just crossed into water
            SV_StartSound(ent, 0, c"misc/hith2o.wav".as_ptr(), 255, 1.0);
        }
        *watertype = cont as f32;
        *waterlevel = 1.0;
    } else {
        if *watertype != CONTENTS_EMPTY as f32 {
            // just crossed into water
            SV_StartSound(ent, 0, c"misc/hith2o.wav".as_ptr(), 255, 1.0);
        }
        *watertype = CONTENTS_EMPTY as f32;
        *waterlevel = cont as f32;
    }
}

/// `void SV_Physics_Toss (edict_t *ent)`: toss, bounce, and fly movement.
/// When onground, do nothing.
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Toss(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let velocity = v(ent, p.v_velocity);

    // regular thinking
    if SV_RunThink(ent) == 0 {
        return;
    }

    #[cfg(feature = "world_trace_hw")]
    if *velocity.add(2) > 0.0 {
        *v(ent, f.v_flags) = (flags(ent) & !f.fl_onground) as f32;
    }

    // if onground, return without moving
    if (flags(ent) & f.fl_onground) != 0 {
        return;
    }

    SV_CheckVelocity(ent);

    // add gravity
    let mt = *v(ent, f.v_movetype);
    if mt != p.movetype_fly as f32
        && mt != p.movetype_bouncemissile as f32
        && mt != p.movetype_flymissile as f32
        && mt != p.movetype_swim as f32
    {
        #[cfg(not(feature = "world_trace_hw"))]
        SV_AddGravity(ent);
        #[cfg(feature = "world_trace_hw")]
        SV_AddGravity(ent, 1.0);
    }

    // move angles
    ma_frametime(v(ent, f.v_angles), v(ent, p.v_avelocity));

    // move origin: `VectorScale (velocity, host_frametime, move)', in double
    let mut move_ = [0.0f32; 3];
    for i in 0..3 {
        move_[i] = (*velocity.add(i) as f64 * Phys_HostFrametime()) as f32;
    }
    let trace = SV_PushEntity(ent, move_.as_ptr());
    if trace.fraction == 1.0 {
        return;
    }
    if free(ent) {
        return;
    }

    let mt = *v(ent, f.v_movetype);
    let backoff: f32 = if mt == p.movetype_bounce as f32 {
        1.5
    } else if mt == p.movetype_bouncemissile as f32 {
        // Solid phased missiles don't bounce on monsters or players.  The C
        // reads trace.ent here without a NULL test, and so does the port.
        let tent = trace.ent.cast::<u8>();
        if *v(ent, f.v_solid) == p.solid_phase as f32
            && ((flags(tent) & f.fl_monster) != 0 || cvt_c(*v(tent, f.v_movetype)) == p.movetype_walk)
        {
            return;
        }
        2.0
    } else {
        1.0
    };

    ClipVelocity(velocity, trace.plane.normal.as_ptr(), velocity, backoff);

    // stop if on ground
    if trace.plane.normal[2] as f64 > 0.7 && *v(ent, f.v_movetype) != p.movetype_bouncemissile as f32 {
        if *velocity.add(2) < 60.0 || *v(ent, f.v_movetype) != p.movetype_bounce as f32 {
            *v(ent, f.v_flags) = (flags(ent) | f.fl_onground) as f32;
            *vi(ent, f.v_groundentity) = edict_to_prog(trace.ent.cast::<u8>());
            for i in 0..3 {
                *velocity.add(i) = 0.0;
            }
            let avelocity = v(ent, p.v_avelocity);
            for i in 0..3 {
                *avelocity.add(i) = 0.0;
            }
        }
    }

    // check for in water
    SV_CheckWaterTransition(ent);
}

/// `static void SV_Physics_Step (edict_t *ent)`: monsters freefall when they
/// don't have a ground entity, otherwise all movement is done with discrete
/// steps.
#[no_mangle]
pub unsafe extern "C" fn SV_Physics_Step(ent: *mut u8) {
    let f = facts();
    let p = pf();

    // freefall if not onground
    if (flags(ent) & (f.fl_onground | f.fl_fly | f.fl_swim)) == 0 {
        // `velocity[2] < gravity*-0.1', in double
        let hitsound = (*v(ent, p.v_velocity).add(2) as f64) < Phys_Gravity() as f64 * -0.1;

        #[cfg(not(feature = "world_trace_hw"))]
        SV_AddGravity(ent);
        #[cfg(feature = "world_trace_hw")]
        SV_AddGravity(ent, 1.0);
        SV_CheckVelocity(ent);
        SV_FlyMove(ent, Phys_HostFrametime() as f32, core::ptr::null_mut());
        SV_LinkEdict(ent, 1);

        if (flags(ent) & f.fl_onground) != 0 && (flags(ent) & f.fl_monster) == 0 && hitsound {
            // just hit ground
            SV_StartSound(ent, 0, c"fx/thngland.wav".as_ptr(), 255, 1.0);
        }
    }

    // regular thinking
    SV_RunThink(ent);

    SV_CheckWaterTransition(ent);
}

/// The movechain after an entity's physics: up to 10 chained entities move
/// by what it moved (and turn, with FL_MOVECHAIN_ANGLE), and each with a
/// chainmoved function is told, when the origin moved.
unsafe fn run_movechain(ent: *mut u8, mut ent2: *mut u8, old_origin: &mut [f32; 3], old_angle: &mut [f32; 3]) {
    let f = facts();
    let p = pf();
    let origin = v(ent, f.v_origin);
    let angles = v(ent, f.v_angles);

    let origin_moved = !equal3(origin, old_origin.as_ptr());
    if origin_moved || !equal3(angles, old_angle.as_ptr()) {
        for i in 0..3 {
            old_origin[i] = *origin.add(i) - old_origin[i];
        }
        for i in 0..3 {
            old_angle[i] = *angles.add(i) - old_angle[i];
        }

        for _c in 0..10 {
            // chain a max of 10 objects
            if free(ent2) {
                break;
            }

            let o2 = v(ent2, f.v_origin);
            for i in 0..3 {
                *o2.add(i) = old_origin[i] + *o2.add(i);
            }
            if (flags(ent2) & p.fl_movechain_angle) != 0 {
                let a2 = v(ent2, f.v_angles);
                for i in 0..3 {
                    *a2.add(i) = old_angle[i] + *a2.add(i);
                }
            }

            let chainmoved = *vi(ent2, p.v_chainmoved);
            if origin_moved && chainmoved != 0 {
                // callback function
                *World_GlobalSelf() = edict_to_prog(ent2);
                *World_GlobalOther() = edict_to_prog(ent);
                PR_ExecuteProgram(chainmoved, c"chainmoved".as_ptr());
            }

            ent2 = prog_to_edict(*vi(ent2, p.v_movechain));
            if ent2 == World_SvEdicts() {
                break;
            }
        }
    }
}

/// `void SV_Physics (void)`, Hexen II's: the frame's entity walk.
#[cfg(not(feature = "world_trace_hw"))]
#[no_mangle]
pub unsafe extern "C" fn SV_Physics() {
    let f = facts();
    let p = pf();

    // let the progs know that a new frame has started
    *World_GlobalSelf() = edict_to_prog(World_SvEdicts());
    *World_GlobalOther() = edict_to_prog(World_SvEdicts());
    *World_GlobalTime() = World_SvTime() as f32;
    PR_ExecuteProgram(Phys_StartFrame(), c"StartFrame".as_ptr());

    // treat each object in turn
    let mut ent = World_SvEdicts();
    let mut old_origin = [0.0f32; 3];
    let mut old_angle = [0.0f32; 3];
    // Edict 0 is the world and 1..maxclients are the players, so capping
    // the walk there runs physics for clients only.
    let mut entity_cap = if Phys_FreezeNonClients() != 0 { Phys_MaxClients() + 1 } else { Phys_NumEdicts() };
    if entity_cap > Phys_NumEdicts() {
        entity_cap = Phys_NumEdicts(); // never walk past what is allocated
    }

    // An edict created during this walk waits for the next frame (#138);
    // see the C's comment.  The walk number is unsigned and wraps to 1.
    {
        static mut WALKS: c_uint = 0;
        WALKS = WALKS.wrapping_add(1);
        if WALKS > 0x7fff_ffff {
            WALKS = 1; // stays a positive int; 0 means "not in a walk"
        }
        *Phys_PhysicsWalk() = WALKS as c_int;
    }

    let mut i: c_int = 0;
    while i < entity_cap {
        'next: {
            if free(ent) {
                break 'next;
            }
            if *ent.add(p.edict_spawnwalk).cast::<c_int>() == *Phys_PhysicsWalk() && i > Phys_MaxClients() {
                break 'next; // created during this walk: next frame
            }

            let ent2 = prog_to_edict(*vi(ent, p.v_movechain));
            if ent2 != World_SvEdicts() {
                for k in 0..3 {
                    old_origin[k] = *v(ent, f.v_origin).add(k);
                }
                for k in 0..3 {
                    old_angle[k] = *v(ent, f.v_angles).add(k);
                }
            }

            if *Phys_ForceRetouch() != 0.0 {
                SV_LinkEdict(ent, 1); // force retouch even for stationary
            }

            let mt = *v(ent, f.v_movetype);
            if i > 0 && i <= Phys_MaxClients() {
                SV_Physics_Client(ent, i);
            } else if mt == f.movetype_push as f32 {
                SV_Physics_Pusher(ent);
            } else if mt == p.movetype_none as f32 {
                SV_Physics_None(ent);
            } else if mt == p.movetype_noclip as f32 {
                SV_Physics_Noclip(ent);
            } else if mt == p.movetype_step as f32 || mt == p.movetype_pushpull as f32 {
                SV_Physics_Step(ent);
            } else if mt == p.movetype_toss as f32
                || mt == p.movetype_bounce as f32
                || mt == p.movetype_bouncemissile as f32
                || mt == p.movetype_fly as f32
                || mt == p.movetype_flymissile as f32
                || mt == p.movetype_swim as f32
            {
                SV_Physics_Toss(ent);
            } else {
                Sys_Error(c"%s: bad movetype %i".as_ptr(), c"SV_Physics".as_ptr(), cvt_c(mt));
            }

            if ent2 != World_SvEdicts() {
                run_movechain(ent, ent2, &mut old_origin, &mut old_angle);
            }
        }
        i += 1;
        ent = next_edict(ent);
    }

    *Phys_PhysicsWalk() = 0;

    let retouch = Phys_ForceRetouch();
    if *retouch != 0.0 {
        *retouch -= 1.0;
    }

    // Hold server time too, or frozen entities still age (see the C)
    if Phys_FreezeNonClients() == 0 {
        *Phys_SvTimePtr() += Phys_HostFrametime();
    }
}

/// `void SV_ProgStartFrame (void)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_ProgStartFrame() {
    // let the progs know that a new frame has started
    *World_GlobalSelf() = edict_to_prog(World_SvEdicts());
    *World_GlobalOther() = edict_to_prog(World_SvEdicts());
    *World_GlobalTime() = World_SvTime() as f32;
    PR_ExecuteProgram(Phys_StartFrame(), c"StartFrame".as_ptr());
}

/// `static void SV_RunEntity (edict_t *ent)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_RunEntity(ent: *mut u8) {
    let f = facts();
    let p = pf();
    let lastruntime = v(ent, p.v_lastruntime);

    if *lastruntime == Phys_Realtime() as f32 {
        return;
    }
    *lastruntime = Phys_Realtime() as f32;

    let ent2 = prog_to_edict(*vi(ent, p.v_movechain));
    let mut old_origin = [0.0f32; 3];
    let mut old_angle = [0.0f32; 3];
    if ent2 != World_SvEdicts() {
        for k in 0..3 {
            old_origin[k] = *v(ent, f.v_origin).add(k);
        }
        for k in 0..3 {
            old_angle[k] = *v(ent, f.v_angles).add(k);
        }
    }

    // `switch ((int)ent->v.movetype)'
    let mt = cvt_c(*v(ent, f.v_movetype));
    if mt == f.movetype_push {
        SV_Physics_Pusher(ent);
    } else if mt == p.movetype_none {
        SV_Physics_None(ent);
    } else if mt == p.movetype_noclip {
        SV_Physics_Noclip(ent);
    } else if mt == p.movetype_step || mt == p.movetype_pushpull {
        SV_Physics_Step(ent);
    } else if mt == p.movetype_toss
        || mt == p.movetype_bounce
        || mt == p.movetype_bouncemissile
        || mt == p.movetype_fly
        || mt == p.movetype_flymissile
        || mt == p.movetype_swim
    {
        SV_Physics_Toss(ent);
    } else if mt == p.movetype_follow {
    } else if mt == p.movetype_walk {
        SV_RunThink(ent);
    } else {
        SV_Error(c"%s: bad movetype %i".as_ptr(), c"SV_RunEntity".as_ptr(), mt);
    }

    if ent2 != World_SvEdicts() {
        run_movechain(ent, ent2, &mut old_origin, &mut old_angle);
    }
}

/// `void SV_RunNewmis (void)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_RunNewmis() {
    let newmis = Phys_Newmis();
    if *newmis == 0 {
        return;
    }
    let ent = prog_to_edict(*newmis);
    *Phys_HostFrametimePtr() = 0.05;
    *newmis = 0;

    SV_RunEntity(ent);
}

/// `void SV_Physics (void)`, HexenWorld's.
#[cfg(feature = "world_trace_hw")]
#[no_mangle]
pub unsafe extern "C" fn SV_Physics() {
    static mut OLD_TIME: f64 = 0.0;
    let p = pf();

    // don't bother running a frame if sys_ticrate seconds haven't passed
    let ft = Phys_HostFrametimePtr();
    *ft = Phys_Realtime() - OLD_TIME;
    if *ft < Phys_MinTic() as f64 {
        return;
    }
    if *ft > Phys_MaxTic() as f64 {
        *ft = Phys_MaxTic() as f64;
    }
    OLD_TIME = Phys_Realtime();

    *Phys_FrameTimeGlobal() = *ft as f32;

    SV_ProgStartFrame();

    // treat each object in turn; even the world gets a chance to think
    let mut ent = World_SvEdicts();
    let mut i: c_int = 0;
    while i < Phys_NumEdicts() {
        'next: {
            if free(ent) {
                break 'next;
            }

            if *Phys_ForceRetouch() != 0.0 {
                SV_LinkEdict(ent, 1); // force retouch even for stationary
            }

            if i > 0 && i <= p.max_clients {
                break 'next; // clients are run directly from packets
            }

            SV_RunEntity(ent);
            SV_RunNewmis();
        }
        i += 1;
        ent = next_edict(ent);
    }

    let retouch = Phys_ForceRetouch();
    if *retouch != 0.0 {
        *retouch -= 1.0;
    }
}
