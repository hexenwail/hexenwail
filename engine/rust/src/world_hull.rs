// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the hull selection of engine/hexen2/world.c and
// engine/hexenworld/server/world.c: the box hull (SV_InitBoxHull,
// SV_HullForBox) and SV_HullForEntity.  Phase 7 step 4, the second world.c
// slice (#455, #294); the first was world_trace.rs.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_HullForEntity picks the clipping hull a move tests against one entity:
// a brush model's own hull, chosen by the mover's size or its `hull` field,
// or a six-plane box built around a non-brush entity's bounds.  It is Hexen
// II's, not Quake's -- eight clip hulls and size rules for points, pentacles,
// half players, players and golems -- so the Quake port #455 borrows from
// has no counterpart, and this is a translation of the C alone.  The C originals moved
// verbatim into engine/rust/world_hull/tests/, the reference half of the
// differential harness.  SV_ClipMoveToEntity, which calls this, stays in C
// for now: it shares world.c's move_type with SV_Move, and goes with it.
//
// THE TWO C ORIGINALS
//
// Beyond the fatal call (Sys_Error in hexen2, SV_Error in hexenworld, chosen
// by the archive's world_trace_hw feature as in world_trace.rs), HexenWorld's
// copy differs in behaviour twice, and the same feature selects its arm:
//
//   * hexen2 gives a mover of size[0] <= 8 the pentacle hull (4) when the
//     world entity's spawnflags has bit 0; hexenworld has no such rule;
//   * hexen2 zeroes the x and y of the offset for an FL_MONSTER mover;
//     hexenworld does not.
//
// LAYOUTS
//
// The structs this reads -- edict_t, entvars_t, qmodel_t, server_t -- are the
// target's own (see engine/rust/world_target.h): qmodel_t differs between the
// GL client, the software renderer and the dedicated servers, entvars_t
// between the protocols.  So the offsets come from engine/rust/world_target.c,
// which each target compiles with its own headers, and sv.models / sv.edicts
// are read through it at call time.
//
// FLOAT TO INT, WHERE C LEAVES IT UNDEFINED
//
// Four conversions -- the `hull` field minus one, the model index, the
// mover's flags and the world's spawnflags -- are undefined in C for NaN or a
// value outside int range.  That is reachable: spawnflags is a worldspawn
// key read with atof (h2shared/pr_edict.c), so a map can set it to 2^31.
// What the C then does depends on the target's conversion instruction, and
// the shipped targets use two:
//
//   * x86_64 (Linux, Windows): GCC and MinGW emit cvttss2si, which gives
//     INT_MIN for NaN and every out-of-range value -- a map with spawnflags
//     2147483648 has bit 0 clear;
//   * wasm32 (the web client): emscripten's clang emits i32.trunc_sat_f32_s,
//     which saturates (INT_MAX, bit 0 set) and gives 0 for NaN.
//
// So the conversion here is per architecture, cvt_c: the x86 rule on x86 and
// x86_64, Rust's `as` -- which saturates and gives 0 for NaN, exactly
// trunc_sat and aarch64's fcvtzs -- everywhere else.  Each target's port
// then picks the hull that target's C picked.
//
// A `hull` index outside the model's eight hulls makes the C form a pointer
// outside the array and read through it; the port forms the same address
// (wrapping arithmetic) and reads the same memory -- garbage, in both.

use core::ffi::{c_char, c_int, c_uint};

use crate::mathlib::MPlane;
use crate::world_trace::{Hull, MClipNode};

/// `CONTENTS_EMPTY` / `CONTENTS_SOLID`, common/bspfile.h.
const CONTENTS_EMPTY: c_int = -1;
const CONTENTS_SOLID: c_int = -2;

/// `_PRINT_NORMAL`, engine/h2shared/printsys.h: Con_Printf's flags.
const PRINT_NORMAL: c_uint = 0;

/// world_target.h's `world_target_facts_t`, field for field.
#[repr(C)]
pub struct WorldTargetFacts {
    pub edict_v: usize,
    pub v_solid: usize,
    pub v_movetype: usize,
    pub v_modelindex: usize,
    pub v_hull: usize,
    pub v_flags: usize,
    pub v_spawnflags: usize,
    pub v_origin: usize,
    pub v_mins: usize,
    pub v_maxs: usize,
    pub model_type: usize,
    pub model_hulls: usize,
    pub hull_size: usize,
    pub solid_bsp: c_int,
    pub movetype_push: c_int,
    pub mod_brush: c_int,
    pub fl_monster: c_int,
}

extern "C" {
    static World_TargetFacts: WorldTargetFacts;
    fn World_SvModels() -> *mut *mut u8;
    // HexenWorld has no pentacle rule, so its arm never reads sv.edicts.
    #[cfg_attr(feature = "world_trace_hw", allow(dead_code))]
    fn World_SvEdicts() -> *mut u8;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
}

#[cfg(not(feature = "world_trace_hw"))]
extern "C" {
    fn Sys_Error(fmt: *const c_char, ...) -> !;
}

#[cfg(feature = "world_trace_hw")]
extern "C" {
    fn SV_Error(fmt: *const c_char, ...) -> !;
}

/// C's `(int) f` as this target's C performs it (see the header): on x86,
/// cvttss2si -- truncation toward zero, INT_MIN for NaN or any value outside
/// int range; elsewhere the saturating conversion of wasm32's trunc_sat and
/// aarch64's fcvtzs, which is Rust's `as`.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[inline(always)]
fn cvt_c(f: f32) -> c_int {
    if f.is_nan() || f >= 2147483648.0 || f < -2147483648.0 {
        c_int::MIN
    } else {
        f as c_int
    }
}

#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
#[inline(always)]
fn cvt_c(f: f32) -> c_int {
    f as c_int
}

/// `X_Error ("<message>")`, X per target; the message has no conversions.
unsafe fn fatal(msg: *const c_char) -> ! {
    #[cfg(not(feature = "world_trace_hw"))]
    Sys_Error(msg);
    #[cfg(feature = "world_trace_hw")]
    SV_Error(msg);
}

// --- the box hull ------------------------------------------------------------

// world.c's `static hull_t box_hull; static mclipnode_t box_clipnodes[6];
// static mplane_t box_planes[6];` -- zero until SV_InitBoxHull, as in C.
static mut BOX_HULL: Hull = Hull {
    clipnodes: core::ptr::null_mut(),
    planes: core::ptr::null_mut(),
    firstclipnode: 0,
    lastclipnode: 0,
    clip_mins: [0.0; 3],
    clip_maxs: [0.0; 3],
};
static mut BOX_CLIPNODES: [MClipNode; 6] = [const { MClipNode { planenum: 0, children: [0; 2] } }; 6];
static mut BOX_PLANES: [MPlane; 6] = [const {
    MPlane { normal: [0.0; 3], dist: 0.0, r#type: 0, signbits: 0, pad: [0; 2] }
}; 6];

/// `void SV_InitBoxHull (void)`: the six planes and clipnodes that let the six
/// floats of a bounding box be stored out as a hull.  Called by SV_ClearWorld,
/// which stays in C.
#[no_mangle]
pub unsafe extern "C" fn SV_InitBoxHull() {
    let nodes = &raw mut BOX_CLIPNODES;
    let planes = &raw mut BOX_PLANES;
    let hull = &raw mut BOX_HULL;

    (*hull).clipnodes = nodes.cast::<MClipNode>();
    (*hull).planes = planes.cast::<MPlane>();
    (*hull).firstclipnode = 0;
    (*hull).lastclipnode = 5;

    for i in 0..6usize {
        let node = nodes.cast::<MClipNode>().add(i);
        let plane = planes.cast::<MPlane>().add(i);
        (*node).planenum = i as c_int;

        let side = i & 1;

        (*node).children[side] = CONTENTS_EMPTY;
        if i != 5 {
            (*node).children[side ^ 1] = (i + 1) as c_int;
        } else {
            (*node).children[side ^ 1] = CONTENTS_SOLID;
        }

        (*plane).r#type = (i >> 1) as u8;
        (*plane).normal[i >> 1] = 1.0;
    }
}

/// `static hull_t *SV_HullForBox (vec3_t mins, vec3_t maxs)`.
unsafe fn sv_hull_for_box(mins: *const f32, maxs: *const f32) -> *mut Hull {
    let planes = (&raw mut BOX_PLANES).cast::<MPlane>();
    (*planes.add(0)).dist = *maxs.add(0);
    (*planes.add(1)).dist = *mins.add(0);
    (*planes.add(2)).dist = *maxs.add(1);
    (*planes.add(3)).dist = *mins.add(1);
    (*planes.add(4)).dist = *maxs.add(2);
    (*planes.add(5)).dist = *mins.add(2);

    &raw mut BOX_HULL
}

// --- field access through the target's facts ----------------------------------

#[inline(always)]
unsafe fn facts() -> &'static WorldTargetFacts {
    &*(&raw const World_TargetFacts)
}

/// `&ent->v.<member>`, as a float pointer.
#[inline(always)]
unsafe fn v(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

/// `hull_t *SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs,
/// vec3_t offset, edict_t *move_ent)`.
///
/// Static in both C files and called from SV_ClipMoveToEntity and, in
/// hexenworld, SV_TestPlayerPosition -- both still C -- so it is exported now,
/// declared in both world.h files.  Statement order, and the order of the
/// Vector macros' element writes, are the C's: `offset` is written before
/// `mins`'s later elements are read, which matters only to a caller that
/// aliases them, and that caller then sees what the C gave it.
#[no_mangle]
pub unsafe extern "C" fn SV_HullForEntity(
    ent: *mut u8,
    mins: *mut f32,
    maxs: *mut f32,
    offset: *mut f32,
    move_ent: *mut u8,
) -> *mut Hull {
    let f = facts();
    let hull: *mut Hull;

    // decide which clipping hull to use, based on the size
    if *v(ent, f.v_solid) == f.solid_bsp as f32 {
        // explicit hulls in the BSP model
        if *v(ent, f.v_movetype) != f.movetype_push as f32 {
            fatal(c"SOLID_BSP without MOVETYPE_PUSH".as_ptr());
        }

        let index = cvt_c(*v(ent, f.v_modelindex));
        let model = *World_SvModels().wrapping_offset(index as isize);

        if model.is_null() || *model.add(f.model_type).cast::<c_int>() != f.mod_brush {
            fatal(c"SOLID_BSP with a non bsp model".as_ptr());
        }

        let mut size = [0.0f32; 3];
        for i in 0..3 {
            size[i] = *maxs.add(i) - *mins.add(i);
        }

        // &model->hulls[i]
        let hulls = |i: c_int| -> *mut Hull {
            model
                .add(f.model_hulls)
                .wrapping_offset(i as isize * f.hull_size as isize)
                .cast::<Hull>()
        };

        let hull_field = *v(move_ent, f.v_hull);
        if hull_field != 0.0 {
            // Entity is specifying which hull to use
            let idx = cvt_c(hull_field - 1.0);
            let mut h = hulls(idx);
            if h.is_null() {
                // Invalid hull.  In C the address of an array element is
                // never null, so the compiler drops this branch; the port keeps
                // the source's text.  It fires only if a far out-of-range
                // `hull` index lands the address exactly on zero: on 64-bit
                // only for a model below about 96 GiB at a multiple of
                // sizeof (hull_t), on 32-bit (wasm32) more easily.  There the
                // port prints and takes hulls[0], where the C goes on to
                // dereference the out-of-bounds pointer.
                CON_Printf(PRINT_NORMAL, c"ERROR: hull %d is null.\n".as_ptr(), idx);
                h = hulls(0);
            }
            hull = h;
        } else {
            // Using the old way uses size to determine hull to use
            #[cfg(not(feature = "world_trace_hw"))]
            let pentacles = || {
                size[0] <= 8.0 && (cvt_c(*v(World_SvEdicts(), f.v_spawnflags)) & 1) != 0
            };
            #[cfg(feature = "world_trace_hw")]
            let pentacles = || false;

            hull = if size[0] < 3.0 {
                hulls(0) // Point
            } else if pentacles() {
                hulls(4) // Pentacles (hexen2 only)
            } else if size[0] <= 32.0 && size[2] <= 28.0 {
                hulls(3) // Half Player
            } else if size[0] <= 32.0 {
                hulls(1) // Full Player
            } else {
                hulls(5) // Golem
            };
        }

        // calculate an offset value to center the origin
        for i in 0..3 {
            *offset.add(i) = (*hull).clip_mins[i] - *mins.add(i);
        }
        #[cfg(not(feature = "world_trace_hw"))]
        if (cvt_c(*v(move_ent, f.v_flags)) & f.fl_monster) != 0
            && (*offset.add(0) != 0.0 || *offset.add(1) != 0.0)
        {
            *offset.add(0) = 0.0;
            *offset.add(1) = 0.0;
        }
        let origin = v(ent, f.v_origin);
        for i in 0..3 {
            *offset.add(i) = *offset.add(i) + *origin.add(i);
        }
    } else {
        // create a temp hull from bounding box sizes
        let mut hullmins = [0.0f32; 3];
        let mut hullmaxs = [0.0f32; 3];
        let ent_mins = v(ent, f.v_mins);
        let ent_maxs = v(ent, f.v_maxs);
        for i in 0..3 {
            hullmins[i] = *ent_mins.add(i) - *maxs.add(i);
        }
        for i in 0..3 {
            hullmaxs[i] = *ent_maxs.add(i) - *mins.add(i);
        }
        hull = sv_hull_for_box(hullmins.as_ptr(), hullmaxs.as_ptr());

        let origin = v(ent, f.v_origin);
        for i in 0..3 {
            *offset.add(i) = *origin.add(i);
        }
    }

    hull
}

// --- ABI cross-check surface (scripts/check-rust-abi.sh) ----------------------

#[no_mangle]
pub extern "C" fn WorldTargetFacts_sizeof() -> usize {
    core::mem::size_of::<WorldTargetFacts>()
}
#[no_mangle]
pub extern "C" fn WorldTargetFacts_offsetof_hull_size() -> usize {
    core::mem::offset_of!(WorldTargetFacts, hull_size)
}
#[no_mangle]
pub extern "C" fn WorldTargetFacts_offsetof_solid_bsp() -> usize {
    core::mem::offset_of!(WorldTargetFacts, solid_bsp)
}
#[no_mangle]
pub extern "C" fn WorldTargetFacts_offsetof_fl_monster() -> usize {
    core::mem::offset_of!(WorldTargetFacts, fl_monster)
}
