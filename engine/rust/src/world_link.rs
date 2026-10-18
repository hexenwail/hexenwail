// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the area linking of engine/hexen2/world.c and
// engine/hexenworld/server/world.c: SV_CreateAreaNode, SV_ClearWorld,
// SV_UnlinkEdict, SV_TouchLinks, SV_FindTouchedLeafs and SV_LinkEdict.
// Phase 7 step 4, the third world.c slice (#455, #294), after world_trace.rs
// and world_hull.rs.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// An entity is linked into the area-node tree whenever it moves: its absolute
// box is recomputed, the BSP leaves it touches are recorded for PVS culling,
// it is hung on the first area node its box crosses, and, when the caller
// asks, every trigger it now overlaps has its QuakeC touch function run.  The
// two C copies are textually identical, so one module serves both protocols;
// what differs between them is layout -- edict_t's progs fields, sv_globals_t,
// server_t, and the renderer's mnode_t/mleaf_t/qmodel_t -- which comes from
// engine/rust/world_target.c, as for world_hull.rs.  The C originals moved
// verbatim into engine/rust/world_link/tests/, the reference half of the
// differential harness.
//
// STORAGE THAT STAYS IN C
//
// sv_areanodes, the tree's 32 nodes, is read by SV_ClipToLinks (still C) and,
// in HexenWorld, by sv_user.c's pmove setup, so its storage stays in world.c
// and the port links to it.  sv_numareanodes and the touch walk's
// sv_link_next / sv_link_prev cursors are private to these functions and move
// here.
//
// RE-ENTRANCY
//
// SV_TouchLinks runs QuakeC, and QuakeC can unlink, relink or remove any
// entity -- including the next one in the list being walked.  The C guards
// that by publishing the loop's `lnext` through sv_link_next, which
// SV_UnlinkEdict advances past an entity it removes.  The port keeps that
// exactly: `lnext` is a local whose address is published the same way and
// read back after every call into QuakeC, so the walk resumes where the C's
// would.
//
// FLOAT TO INT
//
// `(int)ent->v.flags` goes through world_hull.rs's cvt_c, the conversion the
// target's C performs (see world_hull.rs).

use core::ffi::{c_char, c_int, c_uint};

use crate::link_ops::{ClearLink, InsertLinkBefore, LinkC, RemoveLink};
use crate::mathlib::{BoxOnPlaneSide, MPlane};
use crate::world_hull::{cvt_c, facts, SV_InitBoxHull};

/// `CONTENTS_SOLID`, common/bspfile.h.
const CONTENTS_SOLID: c_int = -2;

/// `_PRINT_NORMAL`, engine/h2shared/printsys.h.
const PRINT_NORMAL: c_uint = 0;

/// `AREA_NODES`, both world.h files.  The harness checks it against the
/// header's (WorldAreaNodes_len below).
const AREA_NODES: usize = 32;

/// `areanode_t`, identical in both world.h files.
#[repr(C)]
pub struct AreaNode {
    pub axis: c_int,
    pub dist: f32,
    pub children: [*mut AreaNode; 2],
    pub trigger_edicts: LinkC,
    pub solid_edicts: LinkC,
}

extern "C" {
    /// world.c's `areanode_t sv_areanodes[AREA_NODES]` -- C storage, see the
    /// header.
    static mut sv_areanodes: [AreaNode; AREA_NODES];

    fn World_SvEdicts() -> *mut u8;
    fn World_SvWorldModel() -> *mut u8;
    fn World_SvTime() -> f64;
    fn World_GlobalSelf() -> *mut c_int;
    fn World_GlobalOther() -> *mut c_int;
    fn World_GlobalTime() -> *mut f32;

    fn PR_ExecuteProgram(fnum: c_int, funcname: *const c_char);
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
}

// world.c's `static int sv_numareanodes;` and the touch walk's cursors.
static mut SV_NUMAREANODES: c_int = 0;
static mut SV_LINK_NEXT: *mut *mut LinkC = core::ptr::null_mut();
static mut SV_LINK_PREV: *mut *mut LinkC = core::ptr::null_mut();

// --- field access through the target's facts ----------------------------------

#[inline(always)]
unsafe fn v(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
unsafe fn area(ent: *mut u8) -> *mut LinkC {
    ent.add(facts().edict_area).cast::<LinkC>()
}

#[inline(always)]
unsafe fn areanodes() -> *mut AreaNode {
    (&raw mut sv_areanodes).cast::<AreaNode>()
}

/// `EDICT_FROM_AREA(l)`: the edict whose `area` member `l` is.
#[inline(always)]
unsafe fn edict_from_area(l: *mut LinkC) -> *mut u8 {
    l.cast::<u8>().wrapping_sub(facts().edict_area)
}

/// `EDICT_TO_PROG(e)`: `(byte *)e - (byte *)sv.edicts`, stored to an int.
#[inline(always)]
unsafe fn edict_to_prog(e: *mut u8) -> c_int {
    (e as isize).wrapping_sub(World_SvEdicts() as isize) as c_int
}

/// `mnode_t` members, through the facts.
#[inline(always)]
unsafe fn node_contents(node: *mut u8) -> c_int {
    *node.add(facts().node_contents).cast::<c_int>()
}
#[inline(always)]
unsafe fn node_plane(node: *mut u8) -> *mut MPlane {
    *node.add(facts().node_plane).cast::<*mut MPlane>()
}
#[inline(always)]
unsafe fn node_child(node: *mut u8, i: usize) -> *mut u8 {
    *node.add(facts().node_children).cast::<*mut u8>().add(i)
}

// --- the area nodes -------------------------------------------------------------

/// `static areanode_t *SV_CreateAreaNode (int depth, vec3_t mins, vec3_t maxs)`.
unsafe fn sv_create_area_node(depth: c_int, mins: *const f32, maxs: *const f32) -> *mut AreaNode {
    let anode = areanodes().add(SV_NUMAREANODES as usize);
    SV_NUMAREANODES += 1;

    ClearLink(&raw mut (*anode).trigger_edicts);
    ClearLink(&raw mut (*anode).solid_edicts);

    if depth == facts().area_depth {
        (*anode).axis = -1;
        (*anode).children = [core::ptr::null_mut(); 2];
        return anode;
    }

    let mut size = [0.0f32; 3];
    for i in 0..3 {
        size[i] = *maxs.add(i) - *mins.add(i);
    }
    if size[0] > size[1] {
        (*anode).axis = 0;
    } else {
        (*anode).axis = 1;
    }

    let axis = (*anode).axis as usize;
    // `0.5 * (maxs[axis] + mins[axis])`: the sum in float, the product in
    // double (0.5 is a double literal), rounded back on assignment.
    (*anode).dist = (0.5f64 * (*maxs.add(axis) + *mins.add(axis)) as f64) as f32;
    let mut mins1 = [0.0f32; 3];
    let mut mins2 = [0.0f32; 3];
    let mut maxs1 = [0.0f32; 3];
    let mut maxs2 = [0.0f32; 3];
    for i in 0..3 {
        mins1[i] = *mins.add(i);
        mins2[i] = *mins.add(i);
        maxs1[i] = *maxs.add(i);
        maxs2[i] = *maxs.add(i);
    }

    // `maxs1[axis] = mins2[axis] = anode->dist;`
    mins2[axis] = (*anode).dist;
    maxs1[axis] = mins2[axis];

    (*anode).children[0] = sv_create_area_node(depth + 1, mins2.as_ptr(), maxs2.as_ptr());
    (*anode).children[1] = sv_create_area_node(depth + 1, mins1.as_ptr(), maxs1.as_ptr());

    anode
}

/// `void SV_ClearWorld (void)`.
#[no_mangle]
pub unsafe extern "C" fn SV_ClearWorld() {
    SV_InitBoxHull();

    // `memset (sv_areanodes, 0, sizeof(sv_areanodes));`
    core::ptr::write_bytes(&raw mut sv_areanodes, 0, 1);
    SV_NUMAREANODES = 0;
    let world = World_SvWorldModel();
    sv_create_area_node(
        0,
        world.add(facts().model_mins).cast::<f32>(),
        world.add(facts().model_maxs).cast::<f32>(),
    );
}

/// `void SV_UnlinkEdict (edict_t *ent)`.
#[no_mangle]
pub unsafe extern "C" fn SV_UnlinkEdict(ent: *mut u8) {
    let a = area(ent);
    if (*a).prev.is_null() {
        return; // not linked in anywhere
    }
    RemoveLink(a);
    if !SV_LINK_NEXT.is_null() && *SV_LINK_NEXT == a {
        *SV_LINK_NEXT = (*a).next;
    }
    if !SV_LINK_PREV.is_null() && *SV_LINK_PREV == a {
        *SV_LINK_PREV = (*a).prev;
    }
    // `ent->area.prev = ent->area.next = NULL;`
    (*a).next = core::ptr::null_mut();
    (*a).prev = core::ptr::null_mut();
}

/// `static void SV_TouchLinks (edict_t *ent, areanode_t *node)`.
unsafe fn sv_touch_links(ent: *mut u8, mut node: *mut AreaNode) {
    let f = facts();
    // `lnext` is published through SV_LINK_NEXT and written by SV_UnlinkEdict
    // from inside QuakeC, so every access here goes through the one raw
    // pointer that was published -- never the local directly, which would
    // invalidate the pointer QuakeC writes through.
    let mut lnext_slot: *mut LinkC = core::ptr::null_mut();
    let lnext = &raw mut lnext_slot;

    // loc0:
    loop {
        // touch linked edicts
        SV_LINK_NEXT = lnext;
        let head = &raw mut (*node).trigger_edicts;
        let mut l = (*head).next;
        while l != head {
            if l.is_null() {
                // my area got removed out from under me!
                CON_Printf(PRINT_NORMAL, c"%s: encountered NULL link!\n".as_ptr(), c"SV_TouchLinks".as_ptr());
                break;
            }

            *lnext = (*l).next;
            let touch = edict_from_area(l);
            if touch == ent
                || *v(touch, f.v_touch).cast::<c_int>() == 0
                || *v(touch, f.v_solid) != f.solid_trigger as f32
            {
                l = *lnext;
                continue;
            }
            let ea = v(ent, f.v_absmin);
            let ex = v(ent, f.v_absmax);
            let ta = v(touch, f.v_absmin);
            let tx = v(touch, f.v_absmax);
            if *ea.add(0) > *tx.add(0)
                || *ea.add(1) > *tx.add(1)
                || *ea.add(2) > *tx.add(2)
                || *ex.add(0) < *ta.add(0)
                || *ex.add(1) < *ta.add(1)
                || *ex.add(2) < *ta.add(2)
            {
                l = *lnext;
                continue;
            }

            let old_self = *World_GlobalSelf();
            let old_other = *World_GlobalOther();

            *World_GlobalSelf() = edict_to_prog(touch);
            *World_GlobalOther() = edict_to_prog(ent);
            *World_GlobalTime() = World_SvTime() as f32;
            PR_ExecuteProgram(*v(touch, f.v_touch).cast::<c_int>(), c"touch".as_ptr());

            *World_GlobalSelf() = old_self;
            *World_GlobalOther() = old_other;

            // QuakeC may have moved lnext on through sv_link_next.
            l = core::ptr::read_volatile(lnext);
        }

        SV_LINK_NEXT = core::ptr::null_mut();

        // recurse down both sides
        if (*node).axis == -1 {
            return;
        }

        let axis = (*node).axis as usize;
        let absmin = *v(ent, f.v_absmin).add(axis);
        let absmax = *v(ent, f.v_absmax).add(axis);
        if absmax > (*node).dist {
            if absmin < (*node).dist {
                sv_touch_links(ent, (*node).children[1]); // order reversed to reduce code
            }
            node = (*node).children[0];
            continue;
        } else if absmin < (*node).dist {
            node = (*node).children[1];
            continue;
        }
        return;
    }
}

/// `BOX_ON_PLANE_SIDE(emins, emaxs, p)`, engine/h2shared/mathlib.h.
#[inline(always)]
unsafe fn box_on_plane_side(emins: *mut f32, emaxs: *mut f32, p: *mut MPlane) -> c_int {
    let ty = (*p).r#type;
    if ty < 3 {
        if (*p).dist <= *emins.add(ty as usize) {
            1
        } else if (*p).dist >= *emaxs.add(ty as usize) {
            2
        } else {
            3
        }
    } else {
        BoxOnPlaneSide(emins, emaxs, p)
    }
}

/// `static void SV_FindTouchedLeafs (edict_t *ent, mnode_t *node)`.
unsafe fn sv_find_touched_leafs(ent: *mut u8, mut node: *mut u8) {
    let f = facts();
    let num_leafs = ent.add(f.edict_num_leafs).cast::<c_int>();

    // loc0:
    loop {
        let contents = node_contents(node);
        if contents == CONTENTS_SOLID {
            return;
        }

        // add an efrag if the node is a leaf
        if contents < 0 {
            if *num_leafs == f.max_ent_leafs {
                return;
            }
            // `leafnum = leaf - sv.worldmodel->leafs - 1;`
            let leafs = *World_SvWorldModel().add(f.model_leafs).cast::<*mut u8>();
            let leafnum = ((node as isize - leafs as isize) / f.leaf_size as isize - 1) as c_int;

            *ent.add(f.edict_leafnums).cast::<c_int>().add(*num_leafs as usize) = leafnum;
            *num_leafs += 1;
            return;
        }

        // NODE_MIXED
        let splitplane = node_plane(node);
        let sides = box_on_plane_side(v(ent, f.v_absmin), v(ent, f.v_absmax), splitplane);

        match sides {
            1 => node = node_child(node, 0),
            2 => node = node_child(node, 1),
            _ => {
                // 3
                if node_contents(node_child(node, 0)) != CONTENTS_SOLID {
                    sv_find_touched_leafs(ent, node_child(node, 0));
                }
                node = node_child(node, 1);
            }
        }
    }
}

/// `void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)`.
#[no_mangle]
pub unsafe extern "C" fn SV_LinkEdict(ent: *mut u8, touch_triggers: c_int) {
    let f = facts();

    if !(*area(ent)).prev.is_null() {
        SV_UnlinkEdict(ent); // unlink from old position
    }

    if ent == World_SvEdicts() {
        return; // don't add the world
    }

    if *ent.add(f.edict_free).cast::<c_int>() != 0 {
        return;
    }

    let origin = v(ent, f.v_origin);
    let mins = v(ent, f.v_mins);
    let maxs = v(ent, f.v_maxs);
    let absmin = v(ent, f.v_absmin);
    let absmax = v(ent, f.v_absmax);
    let angles = v(ent, f.v_angles);

    // set the abs box
    if *v(ent, f.v_solid) == f.solid_bsp as f32
        && (*angles.add(0) != 0.0 || *angles.add(1) != 0.0 || *angles.add(2) != 0.0)
    {
        // expand for rotation; `v = fabs(x)` rounds a double fabs back to
        // float, which is exact
        let mut maxv: f32 = 0.0;
        for i in 0..3 {
            let mut vv = (*mins.add(i) as f64).abs() as f32;
            if vv > maxv {
                maxv = vv;
            }
            vv = (*maxs.add(i) as f64).abs() as f32;
            if vv > maxv {
                maxv = vv;
            }
        }
        for i in 0..3 {
            *absmin.add(i) = *origin.add(i) - maxv;
            *absmax.add(i) = *origin.add(i) + maxv;
        }
    } else {
        for i in 0..3 {
            *absmin.add(i) = *origin.add(i) + *mins.add(i);
        }
        for i in 0..3 {
            *absmax.add(i) = *origin.add(i) + *maxs.add(i);
        }
    }

    // to make items easier to pick up and allow them to be grabbed off of
    // shelves, the abs sizes are expanded
    if (cvt_c(*v(ent, f.v_flags)) & f.fl_item) != 0 {
        *absmin.add(0) -= 15.0;
        *absmin.add(1) -= 15.0;
        *absmax.add(0) += 15.0;
        *absmax.add(1) += 15.0;
    } else {
        // because movement is clipped an epsilon away from an actual edge,
        // we must fully check even when bounding boxes don't quite touch
        *absmin.add(0) -= 1.0;
        *absmin.add(1) -= 1.0;
        *absmin.add(2) -= 1.0;
        *absmax.add(0) += 1.0;
        *absmax.add(1) += 1.0;
        *absmax.add(2) += 1.0;
    }

    // link to PVS leafs
    *ent.add(f.edict_num_leafs).cast::<c_int>() = 0;
    if *v(ent, f.v_modelindex) != 0.0 {
        let nodes = *World_SvWorldModel().add(f.model_nodes).cast::<*mut u8>();
        sv_find_touched_leafs(ent, nodes);
    }

    if *v(ent, f.v_solid) == f.solid_not as f32 {
        return;
    }

    // find the first node that the ent's box crosses
    let mut node = areanodes();
    loop {
        if (*node).axis == -1 {
            break;
        }
        let axis = (*node).axis as usize;
        if *absmin.add(axis) > (*node).dist {
            node = (*node).children[0];
        } else if *absmax.add(axis) < (*node).dist {
            node = (*node).children[1];
        } else {
            break; // crosses the node
        }
    }

    // link it in
    if *v(ent, f.v_solid) == f.solid_trigger as f32 {
        InsertLinkBefore(area(ent), &raw mut (*node).trigger_edicts);
    } else {
        InsertLinkBefore(area(ent), &raw mut (*node).solid_edicts);
    }

    // if touch_triggers, touch all entities at this node and decend for more
    if touch_triggers != 0 {
        sv_touch_links(ent, areanodes());
    }
}

// --- ABI cross-check surface (scripts/check-rust-abi.sh) ----------------------

#[no_mangle]
pub extern "C" fn WorldAreaNode_sizeof() -> usize {
    core::mem::size_of::<AreaNode>()
}
#[no_mangle]
pub extern "C" fn WorldAreaNode_offsetof_dist() -> usize {
    core::mem::offset_of!(AreaNode, dist)
}
#[no_mangle]
pub extern "C" fn WorldAreaNode_offsetof_children() -> usize {
    core::mem::offset_of!(AreaNode, children)
}
#[no_mangle]
pub extern "C" fn WorldAreaNode_offsetof_trigger_edicts() -> usize {
    core::mem::offset_of!(AreaNode, trigger_edicts)
}
#[no_mangle]
pub extern "C" fn WorldAreaNode_offsetof_solid_edicts() -> usize {
    core::mem::offset_of!(AreaNode, solid_edicts)
}
#[no_mangle]
pub extern "C" fn WorldAreaNodes_len() -> usize {
    AREA_NODES
}

/// Harness surface: whether the touch walk's cursor is still published.  It
/// must be null whenever no walk is running -- a stale one would point into a
/// dead stack frame, and the next SV_UnlinkEdict would write through it.  The
/// cursor is private, so the differential harness asks here, as it reads the
/// C's c_sv_link_next directly.
#[no_mangle]
pub unsafe extern "C" fn WorldLink_WalkPending() -> c_int {
    (!SV_LINK_NEXT.is_null()) as c_int
}
