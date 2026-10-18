// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the rest of engine/hexen2/world.c and
// engine/hexenworld/server/world.c: SV_Move and what serves it
// (SV_ClipMoveToEntity, SV_ClipToLinks, SV_MoveBounds, world.c's move_type),
// SV_PointContents and SV_TestEntityPosition.  The fourth and last world.c
// slice of #455 (#294); after it world.c holds only sv_areanodes' storage.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_Move is the engine's one swept-box query: clip the move against the
// world, then against every entity linked in the area nodes the move's box
// crosses, and return the nearest impact.  The C originals moved verbatim
// into engine/rust/world_move/tests/.
//
// THE TWO C ORIGINALS
//
// They differ two ways, each selected by the world_trace_hw feature as in
// the earlier slices:
//
//   * the fatal call for a trigger found in a clipping list: Hexen II's
//     Sys_Error names its classname, HexenWorld's SV_Error does not;
//   * Hexen II's SOLID_GHOST, clipped only by a MOVE_MISSILE whose mover has
//     a nonzero `ghost_damage` field -- HexenWorld has no SOLID_GHOST.
//
// Two functions are compiled out of every build, so they are in the
// fixtures as text and not ported: SV_TruePointContents, under
// `#ifdef QUAKE2`, which nothing defines; and HexenWorld's
// SV_TestPlayerPosition, under `#if 0 /* no callers */`.
//
// LAYOUTS AND C STATE
//
// As for the earlier slices, edict_t, entvars_t, qmodel_t and server_t come
// through engine/rust/world_target.c.  trace_t is returned by value across
// the C ABI, as the C functions return it.
//
// FLOAT TO INT
//
// `(int)touch->v.flags` goes through world_hull.rs's cvt_c.

use core::ffi::{c_char, c_int, c_void};

use crate::mathlib::AngleVectors;
use crate::world_hull::{cvt_c, facts, SV_HullForEntity};
use crate::world_link::{areanodes, AreaNode};
use crate::world_trace::{Hull, Trace, TracePlane, SV_HullPointContents, SV_RecursiveHullCheck};

/// common/bspfile.h.
const CONTENTS_WATER: c_int = -3;
const CONTENTS_CURRENT_0: c_int = -9;
const CONTENTS_CURRENT_DOWN: c_int = -14;

extern "C" {
    fn World_SvEdicts() -> *mut u8;
    fn World_SvWorldModel() -> *mut u8;
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn PR_GetString(num: c_int) -> *const c_char;
    /// progs.h: `eval_t *GetEdictFieldValue (edict_t *ed, const char *field)`.
    /// eval_t is a union whose `_float` is at offset 0.
    fn GetEdictFieldValue(ed: *mut u8, field: *const c_char) -> *mut f32;
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    fn SV_Error(fmt: *const c_char, ...) -> !;
}

// world.c's `static int move_type;`, set by SV_Move for SV_ClipMoveToEntity.
static mut MOVE_TYPE: c_int = 0;

/// world.c's `moveclip_t`, private to these functions.
#[repr(C)]
struct MoveClip {
    boxmins: [f32; 3],
    boxmaxs: [f32; 3],
    mins: *mut f32,
    maxs: *mut f32,
    mins2: [f32; 3],
    maxs2: [f32; 3],
    start: *mut f32,
    end: *mut f32,
    trace: Trace,
    type_: c_int,
    passedict: *mut u8,
}

#[inline(always)]
unsafe fn v(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
unsafe fn world_hull(i: usize) -> *mut Hull {
    World_SvWorldModel()
        .add(facts().model_hulls + i * facts().hull_size)
        .cast::<Hull>()
}

/// `PROG_TO_EDICT(e)`: `(edict_t *)((byte *)sv.edicts + e)`.
#[inline(always)]
unsafe fn prog_to_edict(e: c_int) -> *mut u8 {
    World_SvEdicts().wrapping_offset(e as isize)
}

/// `DotProduct(x, y)`, in float, left to right.
#[inline(always)]
fn dot(x: &[f32; 3], y: &[f32; 3]) -> f32 {
    x[0] * y[0] + x[1] * y[1] + x[2] * y[2]
}

/// `fabs(a[0]) > 1 || fabs(a[1]) > 1 || fabs(a[2]) > 1`, in double.
#[inline(always)]
unsafe fn rotated(angles: *const f32) -> bool {
    (*angles.add(0) as f64).abs() > 1.0 || (*angles.add(1) as f64).abs() > 1.0 || (*angles.add(2) as f64).abs() > 1.0
}

/// A zero trace_t, as `memset (&trace, 0, sizeof(trace_t))` leaves it.
fn zero_trace() -> Trace {
    Trace {
        allsolid: 0,
        startsolid: 0,
        inopen: 0,
        inwater: 0,
        fraction: 0.0,
        endpos: [0.0; 3],
        plane: TracePlane { normal: [0.0; 3], dist: 0.0 },
        ent: core::ptr::null_mut(),
    }
}

/// `int SV_PointContents (vec3_t p)`.
#[no_mangle]
pub unsafe extern "C" fn SV_PointContents(p: *mut f32) -> c_int {
    let mut cont = SV_HullPointContents(world_hull(0), 0, p);
    if cont <= CONTENTS_CURRENT_0 && cont >= CONTENTS_CURRENT_DOWN {
        cont = CONTENTS_WATER;
    }
    cont
}

/// `edict_t *SV_TestEntityPosition (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_TestEntityPosition(ent: *mut u8) -> *mut u8 {
    let f = facts();
    let origin = v(ent, f.v_origin);
    let trace = SV_Move(origin, v(ent, f.v_mins), v(ent, f.v_maxs), origin, 0, ent);

    if trace.startsolid != 0 {
        return World_SvEdicts();
    }

    core::ptr::null_mut()
}

/// `static trace_t SV_ClipMoveToEntity (edict_t *ent, vec3_t start,
/// vec3_t mins, vec3_t maxs, vec3_t end, edict_t *move_ent)`.
unsafe fn sv_clip_move_to_entity(
    ent: *mut u8,
    start: *mut f32,
    mins: *mut f32,
    maxs: *mut f32,
    end: *mut f32,
    move_ent: *mut u8,
) -> Trace {
    let f = facts();
    let mut offset = [0.0f32; 3];
    let mut start_l = [0.0f32; 3];
    let mut end_l = [0.0f32; 3];

    // fill in a default trace
    let mut trace = zero_trace();
    trace.fraction = 1.0;
    trace.allsolid = 1;
    for i in 0..3 {
        trace.endpos[i] = *end.add(i);
    }

    // get the clipping hull
    let hull = SV_HullForEntity(ent, mins, maxs, offset.as_mut_ptr(), move_ent);

    for i in 0..3 {
        start_l[i] = *start.add(i) - offset[i];
    }
    for i in 0..3 {
        end_l[i] = *end.add(i) - offset[i];
    }

    let angles = v(ent, f.v_angles);
    let is_bsp = *v(ent, f.v_solid) == f.solid_bsp as f32;

    // rotate start and end into the models frame of reference
    if is_bsp && rotated(angles) {
        let mut forward = [0.0f32; 3];
        let mut right = [0.0f32; 3];
        let mut up = [0.0f32; 3];
        AngleVectors(angles, forward.as_mut_ptr(), right.as_mut_ptr(), up.as_mut_ptr());

        let temp = start_l;
        start_l[0] = dot(&temp, &forward);
        start_l[1] = -dot(&temp, &right);
        start_l[2] = dot(&temp, &up);

        let temp = end_l;
        end_l[0] = dot(&temp, &forward);
        end_l[1] = -dot(&temp, &right);
        end_l[2] = dot(&temp, &up);
    }

    // trace a line through the apropriate clipping hull
    SV_RecursiveHullCheck(hull, (*hull).firstclipnode, 0.0, 1.0, start_l.as_mut_ptr(), end_l.as_mut_ptr(), &mut trace);

    if MOVE_TYPE == f.move_water && SV_PointContents(trace.endpos.as_mut_ptr()) != CONTENTS_WATER {
        trace.endpos = start_l;
        trace.fraction = 0.0;
    }

    // rotate endpos back to world frame of reference
    if *v(ent, f.v_solid) == f.solid_bsp as f32 && rotated(angles) && trace.fraction != 1.0 {
        let a = [-*angles.add(0), -*angles.add(1), -*angles.add(2)];
        let mut forward = [0.0f32; 3];
        let mut right = [0.0f32; 3];
        let mut up = [0.0f32; 3];
        AngleVectors(a.as_ptr(), forward.as_mut_ptr(), right.as_mut_ptr(), up.as_mut_ptr());

        let temp = trace.endpos;
        trace.endpos[0] = dot(&temp, &forward);
        trace.endpos[1] = -dot(&temp, &right);
        trace.endpos[2] = dot(&temp, &up);

        let temp = trace.plane.normal;
        trace.plane.normal[0] = dot(&temp, &forward);
        trace.plane.normal[1] = -dot(&temp, &right);
        trace.plane.normal[2] = dot(&temp, &up);
    }

    // fix trace up by the offset
    if trace.fraction != 1.0 {
        for i in 0..3 {
            trace.endpos[i] = trace.endpos[i] + offset[i];
        }
    }

    // did we clip the move?
    if trace.fraction < 1.0 || trace.startsolid != 0 {
        trace.ent = ent.cast::<c_void>();
    }

    trace
}

/// `static void SV_ClipToLinks (areanode_t *node, moveclip_t *clip)`.
unsafe fn sv_clip_to_links(mut node: *mut AreaNode, clip: *mut MoveClip) {
    let f = facts();

    // loc0: // optimized recursion
    loop {
        // touch linked edicts
        let head = &raw mut (*node).solid_edicts;
        let mut l = (*head).next;
        while l != head {
            let next = (*l).next;
            let touch = l.cast::<u8>().wrapping_sub(f.edict_area);
            let solid = *v(touch, f.v_solid);
            if solid == f.solid_not as f32 || touch == (*clip).passedict {
                l = next;
                continue;
            }
            if solid == f.solid_trigger as f32 {
                #[cfg(not(feature = "world_trace_hw"))]
                Sys_Error(
                    c"Trigger in clipping list (%s)".as_ptr(),
                    PR_GetString(*v(touch, f.v_classname).cast::<c_int>()),
                );
                #[cfg(feature = "world_trace_hw")]
                SV_Error(c"Trigger in clipping list".as_ptr());
            }

            if ((*clip).type_ == f.move_nomonsters || (*clip).type_ == f.move_phase)
                && solid != f.solid_bsp as f32
            {
                l = next;
                continue;
            }

            let ta = v(touch, f.v_absmin);
            let tx = v(touch, f.v_absmax);
            if (*clip).boxmins[0] > *tx.add(0)
                || (*clip).boxmins[1] > *tx.add(1)
                || (*clip).boxmins[2] > *tx.add(2)
                || (*clip).boxmaxs[0] < *ta.add(0)
                || (*clip).boxmaxs[1] < *ta.add(1)
                || (*clip).boxmaxs[2] < *ta.add(2)
            {
                l = next;
                continue;
            }

            let pass = (*clip).passedict;
            if !pass.is_null() && *v(pass, f.v_size) != 0.0 && *v(touch, f.v_size) == 0.0 {
                l = next;
                continue; // points never interact
            }

            // might intersect, so do an exact clip
            if (*clip).trace.allsolid != 0 {
                return;
            }
            if !pass.is_null() {
                if prog_to_edict(*v(touch, f.v_owner).cast::<c_int>()) == pass {
                    l = next;
                    continue; // don't clip against own missiles
                }
                if prog_to_edict(*v(pass, f.v_owner).cast::<c_int>()) == touch {
                    l = next;
                    continue; // don't clip against owner
                }
            }

            // SOLID_GHOST: only clipped by projectiles with .ghost_damage set
            #[cfg(not(feature = "world_trace_hw"))]
            if solid == f.solid_ghost as f32 {
                if pass.is_null() || (*clip).type_ != f.move_missile {
                    l = next;
                    continue;
                }
                let val = GetEdictFieldValue(pass, c"ghost_damage".as_ptr());
                if val.is_null() || *val == 0.0 {
                    l = next;
                    continue;
                }
            }

            let trace = if (cvt_c(*v(touch, f.v_flags)) & f.fl_monster) != 0 {
                sv_clip_move_to_entity(
                    touch,
                    (*clip).start,
                    (*clip).mins2.as_mut_ptr(),
                    (*clip).maxs2.as_mut_ptr(),
                    (*clip).end,
                    touch,
                )
            } else {
                sv_clip_move_to_entity(touch, (*clip).start, (*clip).mins, (*clip).maxs, (*clip).end, touch)
            };
            let mut trace = trace;
            if trace.allsolid != 0 || trace.startsolid != 0 || trace.fraction < (*clip).trace.fraction {
                trace.ent = touch.cast::<c_void>();
                if (*clip).trace.startsolid != 0 {
                    (*clip).trace = trace;
                    (*clip).trace.startsolid = 1;
                } else {
                    (*clip).trace = trace;
                }
            } else if trace.startsolid != 0 {
                // Unreachable, as in the C: the test above already took every
                // trace with startsolid set.  Kept as the source has it.
                (*clip).trace.startsolid = 1;
            }
            l = next;
        }

        // recurse down both sides
        if (*node).axis == -1 {
            return;
        }

        let axis = (*node).axis as usize;
        if (*clip).boxmaxs[axis] > (*node).dist {
            if (*clip).boxmins[axis] < (*node).dist {
                sv_clip_to_links((*node).children[1], clip);
            }
            node = (*node).children[0];
            continue;
        } else if (*clip).boxmins[axis] < (*node).dist {
            node = (*node).children[1];
            continue;
        }
        return;
    }
}

/// `static void SV_MoveBounds (vec3_t start, vec3_t mins, vec3_t maxs,
/// vec3_t end, vec3_t boxmins, vec3_t boxmaxs)`.
unsafe fn sv_move_bounds(
    start: *const f32,
    mins: *const f32,
    maxs: *const f32,
    end: *const f32,
    boxmins: *mut f32,
    boxmaxs: *mut f32,
) {
    for i in 0..3 {
        if *end.add(i) > *start.add(i) {
            *boxmins.add(i) = *start.add(i) + *mins.add(i) - 1.0;
            *boxmaxs.add(i) = *end.add(i) + *maxs.add(i) + 1.0;
        } else {
            *boxmins.add(i) = *end.add(i) + *mins.add(i) - 1.0;
            *boxmaxs.add(i) = *start.add(i) + *maxs.add(i) + 1.0;
        }
    }
}

/// `trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end,
/// int type, edict_t *passedict)`.
#[no_mangle]
pub unsafe extern "C" fn SV_Move(
    start: *mut f32,
    mins: *mut f32,
    maxs: *mut f32,
    end: *mut f32,
    type_: c_int,
    passedict: *mut u8,
) -> Trace {
    let f = facts();
    // `memset (&clip, 0, sizeof (moveclip_t));`
    let mut clip = MoveClip {
        boxmins: [0.0; 3],
        boxmaxs: [0.0; 3],
        mins: core::ptr::null_mut(),
        maxs: core::ptr::null_mut(),
        mins2: [0.0; 3],
        maxs2: [0.0; 3],
        start: core::ptr::null_mut(),
        end: core::ptr::null_mut(),
        trace: zero_trace(),
        type_: 0,
        passedict: core::ptr::null_mut(),
    };

    MOVE_TYPE = type_;
    // clip to world
    clip.trace = sv_clip_move_to_entity(World_SvEdicts(), start, mins, maxs, end, passedict);

    clip.start = start;
    clip.end = end;
    clip.mins = mins;
    clip.maxs = maxs;
    clip.type_ = type_;
    clip.passedict = passedict;

    if type_ == f.move_missile || type_ == f.move_phase {
        // Larger for projectiles against monsters
        for i in 0..3 {
            clip.mins2[i] = -15.0;
            clip.maxs2[i] = 15.0;
        }
    } else {
        for i in 0..3 {
            clip.mins2[i] = *mins.add(i);
            clip.maxs2[i] = *maxs.add(i);
        }
    }

    // create the bounding box of the entire move
    sv_move_bounds(
        start,
        clip.mins2.as_ptr(),
        clip.maxs2.as_ptr(),
        end,
        clip.boxmins.as_mut_ptr(),
        clip.boxmaxs.as_mut_ptr(),
    );

    // clip to entities
    sv_clip_to_links(areanodes(), &mut clip);

    clip.trace
}
