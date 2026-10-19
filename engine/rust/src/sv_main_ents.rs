// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the entity updates of engine/hexen2/sv_main.c: the
// fat PVS (SV_AddToFatPVS, SV_FatPVS), SV_EntityUpdateSize,
// SV_EntityPriority, SV_PrepareClientEntities and SV_CleanupEnts.  #469
// slice B, of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_PrepareClientEntities writes one client's entity updates for this
// frame, as deltas against the reference frame the client last
// acknowledged: an svc_reference header, an update for each entity in the
// client's fat PVS that changed, and an svc_clear_edicts trailer naming the
// entities the client should drop.  It records what it sent in
// sv.states[client].frames[current_frame], the client's next reference.
// sv_netsort (Ironwail's) decides which updates survive when they do not
// all fit: a measuring pass totals the bytes each priority bin wants, and
// the write pass sheds the bins past the budget.  The C original's comments
// explain each rule; this keeps them where the logic is the C's own, and
// the C original moved verbatim into engine/rust/sv_main/tests/
// sv_main_ents_h2.c.
//
// Hexen II only, like sv_main_sound.rs.  HexenWorld's entity updates are
// its sv_ents.c, #469's slice G.
//
// LAYOUTS
//
// entity_state_t (an edict's baseline), entity_state2_t, client_frames_t and
// client_state2_t are mirrored below as #[repr(C)]: they hold no pointers
// and no `long`, so their layout is one on every target.  The harness
// checks each against the sizes and offsets sv_main_target.c takes from the
// Hexen II headers.  client_t and qmodel_t members, the cvars, sv and svs
// come through engine/rust/sv_main_target.c; edict and node layouts through
// world_target.c (world_hull.rs's facts).  The fat PVS buffer is
// sv_main_target.c's C storage, sized by the target's MAX_MAP_LEAFS.
//
// C SEMANTICS KEPT
//
//   * A float stored to a byte (or char) member -- frame, colormap, skin,
//     effects, drawflags, the class-dependent model name's digit -- is what
//     the compiler emits for it on x86: a 32-bit cvttss2si (cvt_c) and its
//     low byte.  `(int)` of a double -- scale*100.0, abslight*255.0, the
//     alpha encoding's Q_rint -- is cvttsd2si (cvt_c_f64).  `(long)` of the
//     flags is the target's own `long`, 64-bit on LP64 and 32-bit on
//     Windows (cvt_c_long).
//   * The origin delta is a float compared against the double 0.1; the
//     priority is the C library's sqrt over doubles, rounded to float.
//   * The struct copies (`*set_ent = *ref_ent` and the rest) copy every
//     field; padding is not part of the contract, and the harness compares
//     with padding canonicalised.
//   * EDICT_NUM, SV_ModelIndex, Mod_LeafPVS, GetEdictFieldValue and
//     PR_GetString are the engine's C, called as the C calls them.
//
// DEPARTURES FROM THE C
//
//   * FL_CLASS_DEPENDENT, in undefined behaviour only.  It rewrites the
//     model name's fifth character from the end.  The C strcpy's the name
//     into a MAX_QPATH buffer and indexes strlen-5, so a name of MAX_QPATH
//     or more overflows the stack and one under five characters writes
//     before the buffer.  The port copies at most MAX_QPATH-1 characters
//     and skips the rewrite for a name under five; it does what the C does
//     for every name the C handles defined.
//   * SV_EntityPriority's clamp, on a key that is not a number -- see it
//     below.  The C's `dist >= NETSORT_BINS - 1' is false for a NaN, so it
//     returns (int) of one, INT_MIN, and the measuring pass indexes
//     binbytes[] with it, some 8 GB below the array, and the server dies.
//     The port takes a non-finite key as the least important bin, which is
//     what Ironwail's own `(int) q_min (dist, 255.f)' does, and what a box
//     its priority cannot be computed for deserves.  A crash becomes a shed
//     entity, and the harness holds both sides of it.

use core::ffi::{c_char, c_int, c_long, c_uint};

use crate::msg_io::{MSG_WriteAngle, MSG_WriteByte, MSG_WriteCoord, MSG_WriteShort};
use crate::sizebuf::SizeBufC;
use crate::world_hull::{cvt_c, facts};

/// `MAX_CLIENT_STATES` and `MAX_FRAMES`, protocol.h; the harness checks the
/// mirrored sizes against the header's.
const MAX_CLIENT_STATES: usize = 8192;
const MAX_FRAMES: usize = 5;
/// sv_main.c's frame markers.
const CLIENT_FRAME_INIT: u8 = 255;
const CLIENT_FRAME_RESET: u8 = 254;
/// sv_main.c's `NETSORT_MAX_UPDATE` and `NETSORT_BINS`.
const NETSORT_MAX_UPDATE: c_int = 24;
const NETSORT_BINS: c_int = 256;
/// `0xff000000`: flag bits QuakeC may not set.
const FLAGS_INVALID: u32 = 0xff00_0000;

/// protocol.h's `entity_state_t`: an edict's baseline.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EntityState {
    pub origin: [f32; 3],
    pub angles: [f32; 3],
    pub modelindex: i16,
    pub frame: u8,
    pub colormap: u8,
    pub skin: u8,
    pub effects: u8,
    pub scale: u8,
    pub drawflags: u8,
    pub abslight: u8,
    pub alpha: u8,
    pub clear_count: [u8; 32],
}

/// protocol.h's `entity_state2_t`: one entry of a client frame.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EntityState2 {
    pub flags: u8,
    pub index: i16,
    pub origin: [f32; 3],
    pub angles: [f32; 3],
    pub modelindex: i16,
    pub frame: u8,
    pub colormap: u8,
    pub skin: u8,
    pub effects: u8,
    pub scale: u8,
    pub drawflags: u8,
    pub abslight: u8,
    pub alpha: u8,
}

const ZERO_STATE2: EntityState2 = EntityState2 {
    flags: 0,
    index: 0,
    origin: [0.0; 3],
    angles: [0.0; 3],
    modelindex: 0,
    frame: 0,
    colormap: 0,
    skin: 0,
    effects: 0,
    scale: 0,
    drawflags: 0,
    abslight: 0,
    alpha: 0,
};

/// protocol.h's `client_frames_t`.
#[repr(C)]
pub struct ClientFrames {
    pub states: [EntityState2; MAX_CLIENT_STATES],
    pub count: c_int,
}

/// protocol.h's `client_state2_t`: 0 = base, 1..MAX_FRAMES = proposed,
/// MAX_FRAMES+1 = too late.
#[repr(C)]
pub struct ClientState2 {
    pub frames: [ClientFrames; MAX_FRAMES + 2],
}

/// sv_main_target.h's `sv_main_ents_facts_t`.
#[repr(C)]
pub struct SvMainEntsFacts {
    pub v_effects: usize,
    pub v_model: usize,
    pub v_colormap: usize,
    pub v_skin: usize,
    pub v_drawflags: usize,
    pub v_frame: usize,
    pub v_scale: usize,
    pub v_abslight: usize,
    pub v_cameramode: usize,
    pub edict_baseline: usize,
    pub client_size: usize,
    pub client_playerclass: usize,
    pub client_current_frame: usize,
    pub client_last_frame: usize,
    pub client_current_sequence: usize,
    pub client_last_sequence: usize,
    pub model_numleafs: usize,
    pub entity_state_size: usize,
    pub entity_state2_size: usize,
    pub client_frames_size: usize,
    pub client_state2_size: usize,
    pub client_frames_count: usize,
    pub max_client_states: c_int,
    pub max_frames: c_int,
    pub clear_limit: c_int,
    pub max_clear_edicts_per_msg: c_int,
    pub ent_cleared: c_int,
    pub ef_nodraw: c_int,
    pub ef_muzzleflash: c_int,
    pub fl_client: c_int,
    pub fl_class_dependent: c_int,
    pub movetype_flymissile: c_int,
    pub movetype_bouncemissile: c_int,
    pub movetype_bounce: c_int,
    pub movetype_step: c_int,
    pub contents_solid: c_int,
    pub entalpha_default: c_int,
    pub max_qpath: c_int,
    pub msg_reference: c_int,
    pub msg_clear_edicts: c_int,
    pub u_morebits: c_int,
    pub u_origin1: c_int,
    pub u_angle1: c_int,
    pub u_angle2: c_int,
    pub u_angle3: c_int,
    pub u_nolerp: c_int,
    pub u_frame: c_int,
    pub u_signal: c_int,
    pub u_model: c_int,
    pub u_clear_ent: c_int,
    pub u_longentity: c_int,
    pub u_morebits2: c_int,
    pub u_skin: c_int,
    pub u_effects: c_int,
    pub u_scale: c_int,
    pub u_colormap: c_int,
    pub u_alpha: c_int,
}

// Layout accessors, for engine/rust/tests/abi_layout.c (the facts table) and
// the differential harness (the mirrored reference-frame structs, which only
// a Hexen II compile can check).
macro_rules! accessor {
    ($name:ident, $e:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            $e
        }
    };
}
accessor!(SvMainEntsFacts_sizeof, core::mem::size_of::<SvMainEntsFacts>());
accessor!(SvMainEntsFacts_offsetof_v_cameramode, core::mem::offset_of!(SvMainEntsFacts, v_cameramode));
accessor!(SvMainEntsFacts_offsetof_client_frames_count, core::mem::offset_of!(SvMainEntsFacts, client_frames_count));
accessor!(SvMainEntsFacts_offsetof_max_client_states, core::mem::offset_of!(SvMainEntsFacts, max_client_states));
accessor!(SvMainEntsFacts_offsetof_max_qpath, core::mem::offset_of!(SvMainEntsFacts, max_qpath));
accessor!(SvMainEntsFacts_offsetof_u_alpha, core::mem::offset_of!(SvMainEntsFacts, u_alpha));
accessor!(SvMainEnts_EntityState_sizeof, core::mem::size_of::<EntityState>());
accessor!(SvMainEnts_EntityState_offsetof_modelindex, core::mem::offset_of!(EntityState, modelindex));
accessor!(SvMainEnts_EntityState_offsetof_alpha, core::mem::offset_of!(EntityState, alpha));
accessor!(SvMainEnts_EntityState_offsetof_clear_count, core::mem::offset_of!(EntityState, clear_count));
accessor!(SvMainEnts_EntityState2_sizeof, core::mem::size_of::<EntityState2>());
accessor!(SvMainEnts_EntityState2_offsetof_index, core::mem::offset_of!(EntityState2, index));
accessor!(SvMainEnts_EntityState2_offsetof_origin, core::mem::offset_of!(EntityState2, origin));
accessor!(SvMainEnts_EntityState2_offsetof_modelindex, core::mem::offset_of!(EntityState2, modelindex));
accessor!(SvMainEnts_EntityState2_offsetof_alpha, core::mem::offset_of!(EntityState2, alpha));
accessor!(SvMainEnts_ClientFrames_sizeof, core::mem::size_of::<ClientFrames>());
accessor!(SvMainEnts_ClientFrames_offsetof_count, core::mem::offset_of!(ClientFrames, count));
accessor!(SvMainEnts_ClientState2_sizeof, core::mem::size_of::<ClientState2>());

/// printsys.h: Con_Printf is _PRINT_NORMAL, Con_DPrintf _PRINT_DEVEL.
const PRINT_NORMAL: c_uint = 0;
const PRINT_DEVEL: c_uint = 2;

extern "C" {
    static SvMainEnts_TargetFacts: SvMainEntsFacts;
    fn SvMain_Edicts() -> *mut u8;
    fn SvMain_NumEdicts() -> c_int;
    fn SvMain_EdictSize() -> c_int;
    fn SvMain_WorldModel() -> *mut u8;
    fn SvMain_States() -> *mut ClientState2;
    fn SvMain_Clients() -> *mut u8;
    fn SvMain_Protocol() -> c_int;
    fn SvMain_UpdatePlayer() -> c_int;
    fn SvMain_UpdateMonsters() -> c_int;
    fn SvMain_UpdateMissiles() -> c_int;
    fn SvMain_UpdateMisc() -> c_int;
    fn SvMain_NetSort() -> c_int;
    fn SvMain_FatPvs() -> *mut u8;

    fn EDICT_NUM(n: c_int) -> *mut u8;
    fn SV_ModelIndex(name: *const c_char) -> c_int;
    fn Mod_LeafPVS(leaf: *mut u8, model: *mut u8) -> *mut u8;
    fn GetEdictFieldValue(ed: *mut u8, field: *const c_char) -> *mut f32;
    fn PR_GetString(num: c_int) -> *const c_char;
    fn strlen(s: *const c_char) -> usize;
    fn sqrt(x: f64) -> f64;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn Host_Error(error: *const c_char, ...) -> !;
}

/// `PROTOCOL_HEXENWAIL_1`, the first protocol with U_ALPHA; from slice A's
/// facts, which carry the protocol numbers.
#[inline(always)]
unsafe fn protocol_hexenwail_1() -> c_int {
    crate::sv_main_sound::protocol_hexenwail_1()
}

#[inline(always)]
unsafe fn ef() -> &'static SvMainEntsFacts {
    &*(&raw const SvMainEnts_TargetFacts)
}

/// C's `(int) d` of a double on this target: as cvt_c, for cvttsd2si.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[inline(always)]
fn cvt_c_f64(d: f64) -> c_int {
    if d.is_nan() || d >= 2147483648.0 || d <= -2147483649.0 {
        c_int::MIN
    } else {
        d as c_int
    }
}

#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
#[inline(always)]
fn cvt_c_f64(d: f64) -> c_int {
    d as c_int
}

/// C's `(long) f` of a float on this target: as cvt_c, at the width of
/// the target's `long`.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[inline(always)]
fn cvt_c_long(f: f32) -> c_long {
    let lim = (c_long::MAX as f64) + 1.0;
    if f.is_nan() || f as f64 >= lim || (f as f64) < -lim {
        c_long::MIN
    } else {
        f as c_long
    }
}

#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
#[inline(always)]
fn cvt_c_long(f: f32) -> c_long {
    f as c_long
}

/// A float stored to a byte or char: the low byte of the int conversion.
#[inline(always)]
fn f2b(f: f32) -> u8 {
    cvt_c(f) as u8
}

// --- field access -----------------------------------------------------------

#[inline(always)]
unsafe fn vf(ent: *mut u8, member: usize) -> f32 {
    *ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
unsafe fn vp(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

#[inline(always)]
unsafe fn vi(ent: *mut u8, member: usize) -> c_int {
    *ent.add(facts().edict_v + member).cast::<c_int>()
}

#[inline(always)]
unsafe fn baseline(ent: *mut u8) -> *mut EntityState {
    ent.add(ef().edict_baseline).cast::<EntityState>()
}

#[inline(always)]
unsafe fn next_edict(ent: *mut u8) -> *mut u8 {
    ent.add(SvMain_EdictSize() as usize)
}

#[inline(always)]
unsafe fn client_byte(client: *mut u8, member: usize) -> *mut u8 {
    client.add(member)
}

// --- the fat PVS ------------------------------------------------------------

/// sv_main.c's `static int fatbytes`.
static mut FATBYTES: c_int = 0;

/// SV_AddToFatPVS: OR in the PVS of every leaf within 8 units of `org`.
unsafe fn add_to_fat_pvs(org: &[f32; 3], mut node: *mut u8) {
    let f = facts();
    loop {
        // if this is a leaf, accumulate the pvs bits
        let contents = *node.add(f.node_contents).cast::<c_int>();
        if contents < 0 {
            if contents != ef().contents_solid {
                let pvs = Mod_LeafPVS(node, SvMain_WorldModel());
                let fat = SvMain_FatPvs();
                let n = *(&raw const FATBYTES);
                let mut i: c_int = 0;
                while i < n {
                    *fat.add(i as usize) |= *pvs.add(i as usize);
                    i += 1;
                }
            }
            return;
        }

        let plane = *node.add(f.node_plane).cast::<*const f32>();
        // mplane_t: normal[3] then dist
        let normal = &*plane.cast::<[f32; 3]>();
        let dist = *plane.add(3);
        let d = (org[0] * normal[0] + org[1] * normal[1] + org[2] * normal[2]) - dist;
        let children = node.add(f.node_children).cast::<*mut u8>();
        if d > 8.0 {
            node = *children;
        } else if d < -8.0 {
            node = *children.add(1);
        } else {
            // go down both
            add_to_fat_pvs(org, *children);
            node = *children.add(1);
        }
    }
}

/// SV_FatPVS: the inclusive OR of the PVS of all leafs within 8 units of
/// `org`.
unsafe fn fat_pvs(org: &[f32; 3]) -> *mut u8 {
    let world = SvMain_WorldModel();
    let numleafs = *world.add(ef().model_numleafs).cast::<c_int>();
    let n = (numleafs + 31) >> 3;
    *(&raw mut FATBYTES) = n;
    core::ptr::write_bytes(SvMain_FatPvs(), 0, n as usize);
    add_to_fat_pvs(org, *world.add(facts().model_nodes).cast::<*mut u8>());
    SvMain_FatPvs()
}

// --- sizes and priorities ---------------------------------------------------

/// SV_EntityUpdateSize: the exact size of the update the writer below emits
/// for `bits`.  Kept in step with that write block.
fn entity_update_size(f: &SvMainEntsFacts, bits: c_int) -> c_int {
    let mut len = 1; // bits | U_SIGNAL
    if bits & f.u_morebits != 0 {
        len += 1;
    }
    if bits & f.u_morebits2 != 0 {
        len += 1;
    }
    len += if bits & f.u_longentity != 0 { 2 } else { 1 }; // entity number

    let fields: [(c_int, c_int); 13] = [
        (f.u_model, 2),
        (f.u_frame, 1),
        (f.u_colormap, 1),
        (f.u_skin, 2), // skin + drawflags
        (f.u_effects, 1),
        (f.u_origin1, 2),
        (f.u_angle1, 1),
        (f.u_origin1 << 1, 2), // U_ORIGIN2
        (f.u_angle2, 1),
        (f.u_origin1 << 2, 2), // U_ORIGIN3
        (f.u_angle3, 1),
        (f.u_scale, 2), // scale + abslight
        (f.u_alpha, 1),
    ];
    for (bit, n) in fields {
        if bits & bit != 0 {
            len += n;
        }
    }
    len
}

/// SV_EntityPriority: Ironwail's key, the scaled fourth root of the squared
/// distance from `org` to the box over the box's squared diagonal.  Lower
/// is more important; the client's own entity is 0.
unsafe fn entity_priority(ent: *mut u8, org: &[f32; 3], is_clent: bool) -> c_int {
    if is_clent {
        return 0;
    }
    let f = facts();
    let absmin = &*vp(ent, f.v_absmin).cast::<[f32; 3]>();
    let absmax = &*vp(ent, f.v_absmax).cast::<[f32; 3]>();
    let mut dist: f32 = 0.0;
    let mut size: f32 = 0.0;
    for i in 0..3 {
        // CLAMP(absmin, org, absmax)
        let x = org[i];
        let c = if x < absmin[i] {
            absmin[i]
        } else if x > absmax[i] {
            absmax[i]
        } else {
            x
        };
        let mut delta = c - org[i];
        dist += delta * delta;
        delta = absmax[i] - absmin[i];
        size += delta * delta;
    }

    if size < 1.0 {
        size = 1.0;
    }

    dist = (8.0f64 * sqrt(sqrt((dist / size) as f64))) as f32;
    // `!(dist < NETSORT_BINS - 1`, and not `dist >= NETSORT_BINS - 1`: the
    // two are equal for every number, and differ for a NaN, which is what
    // this test is for.  A NaN box (QuakeC's setorigin on 0/0, or
    // absmax - absmin of two infinities) leaves the key NaN, and (int) of
    // one is INT_MIN, which the measuring pass writes binbytes[INT_MIN]
    // with, far below the array: the C dies there.  The entity becomes the
    // least important in the frame instead.  See the header.
    if !(dist < (NETSORT_BINS - 1) as f32) {
        return NETSORT_BINS - 1;
    }
    cvt_c(dist)
}

/// ENTALPHA_ENCODE: 0 stays the default; anything else maps 0..1 onto
/// 1..255, through Q_rint.
fn entalpha_encode(f: &SvMainEntsFacts, a: f32) -> u8 {
    if a == 0.0 {
        return f.entalpha_default as u8;
    }
    let x = a * 254.0 + 1.0;
    // CLAMP(1, x, 255), a float
    let c: f32 = if x < 1.0 {
        1.0
    } else if x > 255.0 {
        255.0
    } else {
        x
    };
    // Q_rint
    let r = if c > 0.0 {
        cvt_c_f64(c as f64 + 0.5)
    } else {
        cvt_c_f64(c as f64 - 0.5)
    };
    r as u8
}

// --- the entity updates -----------------------------------------------------

// Raw indexing, as the C indexes: frames[] and states[] by values the frame
// state carries, which a Rust bounds check would turn into an abort where
// the C reads or writes past the array.  Both writers stay under
// MAX_CLIENT_STATES -- they break when bcount reaches it -- so a mutated
// `states[i]` (the checked form) is the same address and equivalent.
#[inline(always)]
unsafe fn frame(state: *mut ClientState2, i: usize) -> *mut ClientFrames {
    (&raw mut (*state).frames).cast::<ClientFrames>().add(i)
}

#[inline(always)]
unsafe fn st(frames: *mut ClientFrames, i: usize) -> *mut EntityState2 {
    (&raw mut (*frames).states).cast::<EntityState2>().add(i)
}

/// SV_PrepareClientEntities.
#[no_mangle]
pub unsafe extern "C" fn SV_PrepareClientEntities(client: *mut u8, clent: *mut u8, msg: *mut SizeBufC) {
    let f = ef();
    let w = facts();

    let mut remove_list: [i16; MAX_CLIENT_STATES] = [0; MAX_CLIENT_STATES];
    let mut binbytes: [c_int; NETSORT_BINS as usize];
    let mut build_ent: EntityState2 = ZERO_STATE2;
    let mut scratch_ref_store: EntityState2 = ZERO_STATE2;
    let mut scratch_set: EntityState2 = ZERO_STATE2;
    let scratch_ref: *mut EntityState2 = &raw mut scratch_ref_store;
    let mut new_name = [0u8; 256];
    let mut trailer_reserve: c_int = 2 + 2 * f.max_clear_edicts_per_msg;

    let client_num = ((client as isize - SvMain_Clients() as isize) / f.client_size as isize) as usize;
    let state = SvMain_States().add(client_num);
    let reference: *mut ClientFrames = frame(state, 0);

    let current_frame = client_byte(client, f.client_current_frame);
    let last_frame = client_byte(client, f.client_last_frame);
    let current_sequence = client_byte(client, f.client_current_sequence);
    let last_sequence = client_byte(client, f.client_last_sequence);
    let max_frames_1 = (MAX_FRAMES + 1) as c_int;

    if *last_sequence != *current_sequence {
        // Old sequence
        *current_frame = (*current_frame).wrapping_add(1);
        if *current_frame as c_int > max_frames_1 {
            *current_frame = max_frames_1 as u8;
        }
    } else if *last_frame == CLIENT_FRAME_INIT || *last_frame == 0 || *last_frame as c_int == max_frames_1 {
        // Reference expired in current sequence
        *current_frame = 1;
        *current_sequence = (*current_sequence).wrapping_add(1);
    } else if *last_frame >= 1 && *last_frame <= *current_frame {
        // Got a valid frame
        core::ptr::copy(frame(state, *last_frame as usize), reference, 1);

        let mut i: c_int = 0;
        while i < (*reference).count {
            let rs = st(reference, i as usize);
            if ((*rs).flags as c_int) & f.ent_cleared != 0 {
                let e = (*rs).index as c_int;
                let ent = EDICT_NUM(e);
                let cc = (&raw mut (*baseline(ent)).clear_count).cast::<u8>().add(client_num);
                if (*cc as c_int) < f.clear_limit {
                    *cc = (*cc).wrapping_add(1);
                } else if *cc as c_int == f.clear_limit {
                    *cc = 3;
                    (*rs).flags &= !(f.ent_cleared as u8);
                }
            }
            i += 1;
        }
        *current_frame = 1;
        *current_sequence = (*current_sequence).wrapping_add(1);
    } else {
        // Normal frame advance
        *current_frame = (*current_frame).wrapping_add(1);
        if *current_frame as c_int > max_frames_1 {
            *current_frame = max_frames_1 as u8;
        }
    }

    let seq = *current_sequence as c_int;
    let mut do_player = false;
    let mut do_monsters = false;
    let mut do_missiles = false;
    let mut do_misc = false;
    if SvMain_UpdatePlayer() != 0 {
        do_player = seq % SvMain_UpdatePlayer() == 0;
    }
    if SvMain_UpdateMonsters() != 0 {
        do_monsters = seq % SvMain_UpdateMonsters() == 0;
    }
    if SvMain_UpdateMissiles() != 0 {
        do_missiles = seq % SvMain_UpdateMissiles() == 0;
    }
    if SvMain_UpdateMisc() != 0 {
        do_misc = seq % SvMain_UpdateMisc() == 0;
    }

    let build: *mut ClientFrames = frame(state, *current_frame as usize);
    core::ptr::write_bytes(build, 0, 1);
    *last_frame = CLIENT_FRAME_RESET;

    MSG_WriteByte(msg, f.msg_reference);
    MSG_WriteByte(msg, *current_frame as c_int);
    MSG_WriteByte(msg, *current_sequence as c_int);

    // find the client's PVS
    let mut org = [0.0f32; 3];
    let cameramode = vi(clent, f.v_cameramode);
    if cameramode != 0 {
        let cam = SvMain_Edicts().offset(cameramode as isize);
        org = *vp(cam, w.v_origin).cast::<[f32; 3]>();
    } else {
        let o = &*vp(clent, w.v_origin).cast::<[f32; 3]>();
        let vo = &*vp(clent, w.v_view_ofs).cast::<[f32; 3]>();
        for i in 0..3 {
            org[i] = o[i] + vo[i];
        }
    }

    let pvs = fat_pvs(&org);

    // sv_netsort: see the C original.  Measuring is skipped when the frame
    // cannot overflow even if every edict sent its largest update.
    let mut netsort_cutoff: c_int = NETSORT_BINS;
    let mut measuring = SvMain_NetSort() != 0
        && (*msg).cursize + SvMain_NumEdicts() * NETSORT_MAX_UPDATE + trailer_reserve_worst(f) > (*msg).maxsize;

    let mut num_to_remove: i16;
    let mut num_deferred: c_int;
    let mut num_shed: c_int;
    let mut num_sorted: c_int;
    let mut shed_nearest: c_int;
    let mut bcount: c_int;

    'restart: loop {
        num_to_remove = 0;
        num_deferred = 0;
        num_shed = 0;
        num_sorted = 0;
        shed_nearest = NETSORT_BINS;
        let mut position: c_int = 0;
        bcount = 0;
        let mut cursize = (*msg).cursize;
        binbytes = [0; NETSORT_BINS as usize];

        // send over all entities (except the client) that touch the pvs
        let mut ent = next_edict(SvMain_Edicts());
        let mut e: c_int = 1;
        while e < SvMain_NumEdicts() {
            // the loop's `e++, ent = NEXT_EDICT(ent)`, run on every
            // `continue` below as the C's for-loop runs it
            macro_rules! next {
                () => {{
                    e += 1;
                    ent = next_edict(ent);
                    continue;
                }};
            }

            let mut do_remove = false;
            // don't send if flagged for NODRAW and there are no lighting effects
            if vf(ent, f.v_effects) == f.ef_nodraw as f32 {
                do_remove = true;
            } else if ent != clent {
                // clent is ALWAYS sent; ignore ents without visible models
                if vf(ent, w.v_modelindex) == 0.0 || *PR_GetString(vi(ent, f.v_model)) == 0 {
                    do_remove = true;
                } else {
                    let num_leafs = *ent.add(w.edict_num_leafs).cast::<c_int>();
                    let leafnums = ent.add(w.edict_leafnums).cast::<c_int>();
                    let mut i: c_int = 0;
                    while i < num_leafs {
                        let l = *leafnums.add(i as usize);
                        if (*pvs.offset((l >> 3) as isize) as c_int) & (1 << (l & 7)) != 0 {
                            break;
                        }
                        i += 1;
                    }
                    // A full leaf list may have overflowed: always send.
                    if i == num_leafs && num_leafs < w.max_ent_leafs {
                        do_remove = true;
                    }
                }
            }

            // skipA:
            let mut ignore_ent = false;
            let flagtest = cvt_c_long(vf(ent, w.v_flags));
            if !do_remove {
                let mt = vf(ent, w.v_movetype);
                if flagtest & f.fl_client as c_long != 0 {
                    if !do_player {
                        ignore_ent = true;
                    }
                } else if flagtest & w.fl_monster as c_long != 0 {
                    if !do_monsters {
                        ignore_ent = true;
                    }
                } else if mt == f.movetype_flymissile as f32
                    || mt == f.movetype_bouncemissile as f32
                    || mt == f.movetype_bounce as f32
                {
                    if !do_missiles {
                        ignore_ent = true;
                    }
                } else if !do_misc {
                    ignore_ent = true;
                }
            }

            let mut bits: c_int = 0;

            while position < (*reference).count && e > (*st(reference, position as usize)).index as c_int {
                position += 1;
            }

            let ref_ent: *mut EntityState2;
            let found_in_list: bool;
            if position < (*reference).count && (*st(reference, position as usize)).index as c_int == e {
                found_in_list = true;
                if do_remove {
                    if (num_to_remove as c_int) < f.max_clear_edicts_per_msg {
                        remove_list[num_to_remove as usize] = e as i16;
                        num_to_remove += 1;
                        next!();
                    }
                    // Remove list full: keep it in the build list as if
                    // ignored, so a later message removes it.
                    num_deferred += 1;
                    ignore_ent = true;
                }
                let r = st(reference, position as usize);
                if measuring {
                    // off a copy: measuring must not rewrite the reference
                    *scratch_ref = *r;
                    ref_ent = scratch_ref;
                } else {
                    ref_ent = r;
                }
            } else {
                if do_remove || ignore_ent {
                    next!();
                }

                let b = &*baseline(ent);
                build_ent.index = e as i16;
                build_ent.origin = b.origin;
                build_ent.angles = b.angles;
                build_ent.modelindex = b.modelindex;
                build_ent.frame = b.frame;
                build_ent.colormap = b.colormap;
                build_ent.skin = b.skin;
                build_ent.effects = b.effects;
                build_ent.scale = b.scale;
                build_ent.drawflags = b.drawflags;
                build_ent.abslight = b.abslight;
                build_ent.alpha = b.alpha;
                // Zero, as the C's is: there build_ent is uninitialised stack,
                // and this is the line that makes the entry the C records
                // zero.  Here the value is ZERO_STATE2 at the top and
                // nothing writes its flags, so dropping the line changes
                // nothing -- a mutant that drops it is equivalent, and was
                // left alive on purpose (see the PR's mutant run).
                build_ent.flags = 0;
                ref_ent = &raw mut build_ent;

                found_in_list = false;
            }

            // the reference entry before the ENT_CLEARED reset rewrites it
            let saved_ref = *ref_ent;

            let set_ent: *mut EntityState2 = if measuring {
                &raw mut scratch_set
            } else {
                st(build, bcount as usize)
            };
            bcount += 1;
            let clear_count = clear_count_now(ent, client_num);
            if clear_count < f.clear_limit {
                *ref_ent = ZERO_STATE2;
                (*ref_ent).index = e as i16;
            }
            *set_ent = *ref_ent;

            if ignore_ent {
                next!();
            }

            // send an update
            let origin = &*vp(ent, w.v_origin).cast::<[f32; 3]>();
            for i in 0..3 {
                let miss = origin[i] - (*ref_ent).origin[i];
                if (miss as f64) < -0.1 || miss as f64 > 0.1 {
                    bits |= f.u_origin1 << i;
                    (*set_ent).origin[i] = origin[i];
                }
            }

            let angles = &*vp(ent, w.v_angles).cast::<[f32; 3]>();
            if angles[0] != (*ref_ent).angles[0] {
                bits |= f.u_angle1;
                (*set_ent).angles[0] = angles[0];
            }
            if angles[1] != (*ref_ent).angles[1] {
                bits |= f.u_angle2;
                (*set_ent).angles[1] = angles[1];
            }
            if angles[2] != (*ref_ent).angles[2] {
                bits |= f.u_angle3;
                (*set_ent).angles[2] = angles[2];
            }

            if vf(ent, w.v_movetype) == f.movetype_step as f32 {
                bits |= f.u_nolerp; // don't mess up the step animation
            }

            let colormap = vf(ent, f.v_colormap);
            if (*ref_ent).colormap as f32 != colormap {
                bits |= f.u_colormap;
                (*set_ent).colormap = f2b(colormap);
            }

            let skin = vf(ent, f.v_skin);
            let drawflags = vf(ent, f.v_drawflags);
            if (*ref_ent).skin as f32 != skin || (*ref_ent).drawflags as f32 != drawflags {
                bits |= f.u_skin;
                (*set_ent).skin = f2b(skin);
                (*set_ent).drawflags = f2b(drawflags);
            }

            let frame = vf(ent, f.v_frame);
            if (*ref_ent).frame as f32 != frame {
                bits |= f.u_frame;
                (*set_ent).frame = f2b(frame);
            }

            let effects = vf(ent, f.v_effects);
            if (*ref_ent).effects as f32 != effects {
                bits |= f.u_effects;
                (*set_ent).effects = f2b(effects);
            }

            if (flagtest as u64) & (FLAGS_INVALID as u64) != 0 {
                Host_Error(
                    c"Invalid flags setting for class %s".as_ptr(),
                    PR_GetString(vi(ent, w.v_classname)),
                );
            }

            let mut temp_index = cvt_c(vf(ent, w.v_modelindex));
            let model = vi(ent, f.v_model);
            if (cvt_c(vf(ent, w.v_flags)) & f.fl_class_dependent) != 0 && model != 0 {
                // strcpy into a MAX_QPATH buffer; see the header on the
                // departure for names the C overflows with
                let name = PR_GetString(model);
                let cap = (f.max_qpath as usize).clamp(1, new_name.len()) - 1;
                let len = strlen(name).min(cap);
                core::ptr::copy_nonoverlapping(name.cast::<u8>(), new_name.as_mut_ptr(), len);
                new_name[len] = 0;
                if len >= 5 {
                    let pc = *client.add(f.client_playerclass).cast::<f32>();
                    new_name[len - 5] = f2b(pc + 48.0);
                }
                temp_index = SV_ModelIndex(new_name.as_ptr().cast::<c_char>());
            }

            if (*ref_ent).modelindex as c_int != temp_index {
                bits |= f.u_model;
                (*set_ent).modelindex = temp_index as i16;
            }

            let scale = cvt_c_f64(vf(ent, f.v_scale) as f64 * 100.0) & 255;
            let abslight = cvt_c_f64(vf(ent, f.v_abslight) as f64 * 255.0) & 255;
            if (*ref_ent).scale as c_int != scale || (*ref_ent).abslight as c_int != abslight {
                bits |= f.u_scale;
                (*set_ent).scale = scale as u8;
                (*set_ent).abslight = abslight as u8;
            }

            // Protocols 18-21 have no U_ALPHA; the reference frame carries
            // alpha either way.
            if SvMain_Protocol() >= protocol_hexenwail_1() {
                let val = GetEdictFieldValue(ent, c"alpha".as_ptr());
                let newalpha = if val.is_null() {
                    f.entalpha_default as u8
                } else {
                    entalpha_encode(f, *val)
                };
                if (*ref_ent).alpha != newalpha {
                    bits |= f.u_alpha;
                    (*set_ent).alpha = newalpha;
                }
            }

            if clear_count_now(ent, client_num) < f.clear_limit {
                bits |= f.u_clear_ent;
                (*set_ent).flags |= f.ent_cleared as u8;
            }

            if bits == 0 && found_in_list {
                if bcount >= MAX_CLIENT_STATES as c_int {
                    break;
                }
                next!();
            }

            if e >= 256 {
                bits |= f.u_longentity;
            }
            if bits >= 256 {
                bits |= f.u_morebits;
            }
            // 65535 is unreachable, so this test and `bits >= 65535' agree
            // on every value the code can build: the lowest bit that can
            // push `bits` past 65536 is U_SKIN (1 << 16), and with it clear
            // the most the rest can make is U_MOREBITS | U_ORIGIN1..3 |
            // U_ANGLE1..3 | U_NOLERP | U_FRAME | U_MODEL | U_CLEAR_ENT |
            // U_LONGENTITY = 20351.  U_ENT_OFF (1 << 13) is HexenWorld's and
            // this code never sets it.  Its mutant survives for that reason.
            if bits >= 65536 {
                bits |= f.u_morebits2;
            }

            let bytes = entity_update_size(f, bits);

            // only worth the square roots when it can change the outcome
            let prio = if measuring || netsort_cutoff < NETSORT_BINS {
                entity_priority(ent, &org, ent == clent)
            } else {
                0
            };

            if measuring {
                // Total the demand; do not truncate it.  A NaN box used to
                // make prio INT_MIN (cvt_c), which the C writes through, far
                // outside binbytes[], and dies there.  entity_priority's
                // `!(dist < (NETSORT_BINS - 1) as f32)` guard takes a
                // non-finite key as the least important bin instead, so
                // prio is always in [0, NETSORT_BINS) and this indexes
                // safely.  The C's crash is its own fix (#473); this is the
                // ported side of it.
                binbytes[prio as usize] += bytes;
                if bcount >= MAX_CLIENT_STATES as c_int {
                    break;
                }
                next!();
            }

            // The size guard and the netsort cutoff: shed the entity, and
            // undo its build->states[] bookkeeping (see the C original).
            if prio >= netsort_cutoff || cursize + bytes + trailer_reserve > (*msg).maxsize {
                *ref_ent = saved_ref;
                num_shed += 1;
                if prio >= netsort_cutoff {
                    num_sorted += 1;
                }
                if prio < shed_nearest {
                    shed_nearest = prio;
                }

                if !found_in_list {
                    bcount -= 1;
                    next!();
                }

                *set_ent = saved_ref;
                // no U_CLEAR_ENT went out
                (*set_ent).flags &= !(f.ent_cleared as u8);

                if bcount >= MAX_CLIENT_STATES as c_int {
                    break;
                }
                next!();
            }

            // write the message
            MSG_WriteByte(msg, bits | f.u_signal);

            if bits & f.u_morebits != 0 {
                MSG_WriteByte(msg, bits >> 8);
            }
            if bits & f.u_morebits2 != 0 {
                MSG_WriteByte(msg, bits >> 16);
            }

            if bits & f.u_longentity != 0 {
                MSG_WriteShort(msg, e);
            } else {
                MSG_WriteByte(msg, e);
            }

            if bits & f.u_model != 0 {
                MSG_WriteShort(msg, temp_index);
            }
            if bits & f.u_frame != 0 {
                MSG_WriteByte(msg, cvt_c(vf(ent, f.v_frame)));
            }
            if bits & f.u_colormap != 0 {
                MSG_WriteByte(msg, cvt_c(vf(ent, f.v_colormap)));
            }
            if bits & f.u_skin != 0 {
                // skin and drawflags
                MSG_WriteByte(msg, cvt_c(vf(ent, f.v_skin)));
                MSG_WriteByte(msg, cvt_c(vf(ent, f.v_drawflags)));
            }
            if bits & f.u_effects != 0 {
                MSG_WriteByte(msg, cvt_c(vf(ent, f.v_effects)));
            }
            if bits & f.u_origin1 != 0 {
                MSG_WriteCoord(msg, origin[0]);
            }
            if bits & f.u_angle1 != 0 {
                MSG_WriteAngle(msg, angles[0]);
            }
            if bits & (f.u_origin1 << 1) != 0 {
                MSG_WriteCoord(msg, origin[1]);
            }
            if bits & f.u_angle2 != 0 {
                MSG_WriteAngle(msg, angles[1]);
            }
            if bits & (f.u_origin1 << 2) != 0 {
                MSG_WriteCoord(msg, origin[2]);
            }
            if bits & f.u_angle3 != 0 {
                MSG_WriteAngle(msg, angles[2]);
            }
            if bits & f.u_scale != 0 {
                // scale and abslight
                MSG_WriteByte(msg, cvt_c_f64(vf(ent, f.v_scale) as f64 * 100.0) & 255);
                MSG_WriteByte(msg, cvt_c_f64(vf(ent, f.v_abslight) as f64 * 255.0) & 255);
            }
            if bits & f.u_alpha != 0 {
                MSG_WriteByte(msg, (*set_ent).alpha as c_int);
            }

            // resync rather than add
            cursize = (*msg).cursize;

            if bcount >= MAX_CLIENT_STATES as c_int {
                break;
            }
            next!();
        }

        if measuring {
            // The measuring pass made the write pass's removal decisions,
            // so the trailer's size is known exactly.  GitHub #82.
            trailer_reserve = 2 + 2 * num_to_remove as c_int;
            let budget = (*msg).maxsize - trailer_reserve - (*msg).cursize;

            // the number of whole bins that fit.  `total` is read nowhere
            // past this loop: adding the bin a pass too early moves nothing
            // but `total` itself, which is why the mutant that does it is
            // equivalent and was left alive (the PR's mutant run).
            let mut total: c_int = 0;
            netsort_cutoff = 0;
            while netsort_cutoff < NETSORT_BINS {
                if total + binbytes[netsort_cutoff as usize] > budget {
                    break;
                }
                total += binbytes[netsort_cutoff as usize];
                netsort_cutoff += 1;
            }

            // admit the nearest bin even when it alone does not fit
            if netsort_cutoff == 0 {
                netsort_cutoff = 1;
            }

            measuring = false;
            continue 'restart;
        }
        break;
    }

    (*build).count = bcount;

    // the count goes out as a byte
    if num_to_remove as c_int > f.max_clear_edicts_per_msg {
        num_to_remove = f.max_clear_edicts_per_msg as i16;
    }

    if (*msg).cursize + 2 + 2 * num_to_remove as c_int > (*msg).maxsize {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: removal trailer (%d) exceeds measured reserve (%d)\n".as_ptr(),
            c"SV_PrepareClientEntities".as_ptr(),
            2 + 2 * num_to_remove as c_int,
            trailer_reserve,
        );
        num_to_remove = (((*msg).maxsize - (*msg).cursize - 2) / 2) as i16;
        if num_to_remove < 0 {
            num_to_remove = 0;
        }
    }

    if num_deferred != 0 {
        CON_Printf(
            PRINT_DEVEL,
            c"%s: remove list full, %d edict(s) deferred\n".as_ptr(),
            c"SV_PrepareClientEntities".as_ptr(),
            num_deferred,
        );
    }

    if num_shed != 0 {
        CON_Printf(
            PRINT_DEVEL,
            c"%s: datagram full at %d/%d bytes, %d edict(s) shed (%d by netsort, cutoff bin %d), nearest shed bin %d\n"
                .as_ptr(),
            c"SV_PrepareClientEntities".as_ptr(),
            (*msg).cursize,
            (*msg).maxsize,
            num_shed,
            num_sorted,
            netsort_cutoff,
            shed_nearest,
        );
    }

    MSG_WriteByte(msg, f.msg_clear_edicts);
    MSG_WriteByte(msg, num_to_remove as c_int);
    for i in 0..num_to_remove.max(0) as usize {
        MSG_WriteShort(msg, remove_list[i] as c_int);
    }
}

/// `DATAGRAM_TRAILER_RESERVE`: the opcode, the count byte and up to
/// MAX_CLEAR_EDICTS_PER_MSG shorts.
#[inline(always)]
fn trailer_reserve_worst(f: &SvMainEntsFacts) -> c_int {
    2 + 2 * f.max_clear_edicts_per_msg
}

/// `ent->baseline.ClearCount[client_num]`, read where the C reads it.
#[inline(always)]
unsafe fn clear_count_now(ent: *mut u8, client_num: usize) -> c_int {
    *(&raw const (*baseline(ent)).clear_count).cast::<u8>().add(client_num) as c_int
}

/// SV_CleanupEnts: the muzzle flash lasts one frame.
#[no_mangle]
pub unsafe extern "C" fn SV_CleanupEnts() {
    let f = ef();
    let mut ent = next_edict(SvMain_Edicts());
    let mut e: c_int = 1;
    while e < SvMain_NumEdicts() {
        let effects = vp(ent, f.v_effects);
        *effects = (cvt_c(*effects) & !f.ef_muzzleflash) as f32;
        e += 1;
        ent = next_edict(ent);
    }
}
