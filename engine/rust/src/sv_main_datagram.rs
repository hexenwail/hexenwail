// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the client data and datagrams of
// engine/hexen2/sv_main.c: SV_ClearDatagram, SV_WriteClientdataToMessage,
// SV_SendClientDatagram, SV_UpdateToReliableMessages, SV_SendNop and
// SV_SendClientMessages.  #469 slice C, of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_SendClientMessages runs once per server frame: frags and the reliable
// datagram go out on every client's reliable stream
// (SV_UpdateToReliableMessages), each spawned client gets its datagram --
// the clientdata delta block, the entity updates slice B writes, and the
// two queued datagrams -- built against what its connection can carry
// (SV_SendClientDatagram), and clients still in signon get their signon
// buffers one per reliable message, or a nop keepalive.  The C originals
// moved verbatim into engine/rust/sv_main/tests/sv_main_datagram_h2.c.
//
// Hexen II only, like the other sv_main_* slices.  The constants, the
// client and server layouts and the globals come through
// engine/rust/sv_main_target.c; the edict offsets through world_target.c
// (world_hull.rs's facts); the protocol numbers and per-protocol limits
// through sv_main_sound.rs's facts.
//
// C SEMANTICS KEPT
//
//   * `(int)` of a float -- the dmg bytes, the view/angle/velocity chars,
//     weaponframe, armorvalue, `(int)ent->v.flags & FL_ONGROUND`, the
//     `(int)ent->v.cnt_*` inventory counts, the health short and the
//     experience long -- goes through world_hull.rs's cvt_c, the target's
//     own conversion: on x86 a NaN or out-of-range value is INT_MIN, whose
//     low byte is 0.
//   * `old_frags != ent->v.frags` compares as float: the int side widens,
//     as C's usual arithmetic conversions do it.
//   * The damage origin is `origin + 0.5*(mins+maxs)`: mins+maxs in float,
//     the rest in double, rounded to float for MSG_WriteCoord, as C's usual
//     arithmetic conversions do it (the same dance as slice A's
//     write_sound_origin).
//   * `weaponmodel` and `cameramode` are int fields (string_t, progdefs.h);
//     they are compared and passed as ints, not floats: +0.0 and -0.0 are
//     different ints.
//   * The split brain is the C's: svc_clientdata (damage, setangle, the
//     SU_* delta block) goes to the `msg` parameter, while svc_update_inv
//     (the SC1_*/SC2_* inventory block) and the extended-inventory page
//     loop go to `host_client->message`, the reliable stream.  The function
//     reads `client->send_all_v` and `client->info_mask`/`info_mask2` but
//     clears `host_client->send_all_v` and copies into `client->old_v` --
//     exactly as written, not "fixed" to one side.
//   * `host_client` is the loop variable and the implicit parameter, as in
//     C: SV_SendClientMessages and SV_UpdateToReliableMessages assign it
//     each iteration, and the callees read the global.
//   * `1 << i` for the inventory bit `i` is computed in u32: for i = 31
//     the C's signed shift is undefined, and x86 gives 0x80000000, which
//     the u32 shift reproduces exactly.
//   * SV_DropClient returns in the C (host.c); the port calls the engine's,
//     as the C does.
//
// DEPARTURES FROM THE C
//
//   * `dev_stats.packetsize`, under `#ifndef SERVERONLY`: not written.  The
//     struct lives in client.h, which the dedicated build does not compile,
//     and the field is consumed only by the client overlay's devstats
//     display -- no server logic and no protocol reads it.  This is the
//     SERVERONLY variant, as info_str.rs does for its own split; h2ded's C
//     never writes it either.

use core::ffi::{c_char, c_int, c_uchar, c_uint, c_void};

use crate::msg_io::{
    MSG_WriteAngle, MSG_WriteByte, MSG_WriteChar, MSG_WriteCoord, MSG_WriteFloat,
    MSG_WriteLong, MSG_WriteShort, MSG_WriteString,
};
use crate::sizebuf::{SZ_Clear, SZ_Init, SZ_Write, SizeBufC};
use crate::sv_main_ents::{SV_CleanupEnts, SV_PrepareClientEntities};
use crate::sv_main_sound::{SV_MaxDatagram, SV_MaxReliableMessage};
use crate::world_hull::cvt_c;

/// sv_main.c's `NET_MAXMESSAGE`, net.h: the stack datagram buffer.  The
/// wire limit below is always smaller; this is only the backing store.
const NET_MAXMESSAGE: usize = 32768;

/// sv_main_target.h's `sv_main_datagram_facts_t`.
#[repr(C)]
pub struct SvMainDatagramFacts {
    pub msg_damage: c_int,
    pub msg_setangle: c_int,
    pub msg_clientdata: c_int,
    pub msg_update_inv: c_int,
    pub msg_time: c_int,
    pub msg_nop: c_int,
    pub msg_updatefrags: c_int,
    pub msg_signonnum: c_int,
    pub su_viewheight: c_int,
    pub su_idealpitch: c_int,
    pub su_idealroll: c_int,
    pub su_punch1: c_int,
    pub su_velocity1: c_int,
    pub su_onground: c_int,
    pub su_weaponframe: c_int,
    pub su_armor: c_int,
    pub su_weapon: c_int,
    pub sc1_health: c_int,
    pub sc1_level: c_int,
    pub sc1_intelligence: c_int,
    pub sc1_wisdom: c_int,
    pub sc1_strength: c_int,
    pub sc1_dexterity: c_int,
    pub sc1_weapon: c_int,
    pub sc1_bluemana: c_int,
    pub sc1_greenmana: c_int,
    pub sc1_experience: c_int,
    pub sc1_cnt_torch: c_int,
    pub sc1_cnt_h_boost: c_int,
    pub sc1_cnt_sh_boost: c_int,
    pub sc1_cnt_mana_boost: c_int,
    pub sc1_cnt_teleport: c_int,
    pub sc1_cnt_tome: c_int,
    pub sc1_cnt_summon: c_int,
    pub sc1_cnt_invisibility: c_int,
    pub sc1_cnt_glyph: c_int,
    pub sc1_cnt_haste: c_int,
    pub sc1_cnt_blast: c_int,
    pub sc1_cnt_polymorph: c_int,
    pub sc1_cnt_flight: c_int,
    pub sc1_cnt_cubeofforce: c_int,
    pub sc1_cnt_invincibility: c_int,
    pub sc1_artifact_active: c_int,
    pub sc1_artifact_low: c_int,
    pub sc1_movetype: c_int,
    pub sc1_cameramode: c_int,
    pub sc1_hasted: c_int,
    pub sc1_inventory: c_int,
    pub sc1_rings_active: c_int,
    pub sc2_rings_low: c_int,
    pub sc2_amulet: c_int,
    pub sc2_bracer: c_int,
    pub sc2_breastplate: c_int,
    pub sc2_helmet: c_int,
    pub sc2_flight_t: c_int,
    pub sc2_water_t: c_int,
    pub sc2_turning_t: c_int,
    pub sc2_regen_t: c_int,
    pub sc2_haste_t: c_int,
    pub sc2_tome_t: c_int,
    pub sc2_puzzle1: c_int,
    pub sc2_puzzle2: c_int,
    pub sc2_puzzle3: c_int,
    pub sc2_puzzle4: c_int,
    pub sc2_puzzle5: c_int,
    pub sc2_puzzle6: c_int,
    pub sc2_puzzle7: c_int,
    pub sc2_puzzle8: c_int,
    pub sc2_maxhealth: c_int,
    pub sc2_maxmana: c_int,
    pub sc2_flags: c_int,
    pub sc2_obj: c_int,
    pub sc2_obj2: c_int,
    pub max_inventory_ex: c_int,
    pub fl_onground: c_int,
    pub edict_v: usize,
    pub entvars_size: usize,
    pub v_dmg_take: usize,
    pub v_dmg_save: usize,
    pub v_dmg_inflictor: usize,
    pub v_origin: usize,
    pub v_mins: usize,
    pub v_maxs: usize,
    pub v_fixangle: usize,
    pub v_angles: usize,
    pub v_view_ofs: usize,
    pub v_idealpitch: usize,
    pub v_idealroll: usize,
    pub v_punchangle: usize,
    pub v_velocity: usize,
    pub v_weaponframe: usize,
    pub v_armorvalue: usize,
    pub v_weaponmodel: usize,
    pub v_flags: usize,
    pub v_health: usize,
    pub v_level: usize,
    pub v_intelligence: usize,
    pub v_wisdom: usize,
    pub v_strength: usize,
    pub v_dexterity: usize,
    pub v_weapon: usize,
    pub v_bluemana: usize,
    pub v_greenmana: usize,
    pub v_experience: usize,
    pub v_cnt_torch: usize,
    pub v_cnt_h_boost: usize,
    pub v_cnt_sh_boost: usize,
    pub v_cnt_mana_boost: usize,
    pub v_cnt_teleport: usize,
    pub v_cnt_tome: usize,
    pub v_cnt_summon: usize,
    pub v_cnt_invisibility: usize,
    pub v_cnt_glyph: usize,
    pub v_cnt_haste: usize,
    pub v_cnt_blast: usize,
    pub v_cnt_polymorph: usize,
    pub v_cnt_flight: usize,
    pub v_cnt_cubeofforce: usize,
    pub v_cnt_invincibility: usize,
    pub v_artifact_active: usize,
    pub v_artifact_low: usize,
    pub v_movetype: usize,
    pub v_cameramode: usize,
    pub v_hasted: usize,
    pub v_inventory: usize,
    pub v_rings_active: usize,
    pub v_rings_low: usize,
    pub v_armor_amulet: usize,
    pub v_armor_bracer: usize,
    pub v_armor_breastplate: usize,
    pub v_armor_helmet: usize,
    pub v_ring_flight: usize,
    pub v_ring_water: usize,
    pub v_ring_turning: usize,
    pub v_ring_regeneration: usize,
    pub v_haste_time: usize,
    pub v_tome_time: usize,
    pub v_puzzle_inv1: usize,
    pub v_puzzle_inv2: usize,
    pub v_puzzle_inv3: usize,
    pub v_puzzle_inv4: usize,
    pub v_puzzle_inv5: usize,
    pub v_puzzle_inv6: usize,
    pub v_puzzle_inv7: usize,
    pub v_puzzle_inv8: usize,
    pub v_max_health: usize,
    pub v_max_mana: usize,
    pub v_frags: usize,
    pub client_size: usize,
    pub client_old_v: usize,
    pub client_send_all_v: usize,
    pub client_edict: usize,
    pub client_message: usize,
    pub client_datagram: usize,
    pub client_netconnection: usize,
    pub client_active: usize,
    pub client_spawned: usize,
    pub client_signon_buffer: usize,
    pub client_sendsignon: usize,
    pub client_last_message: usize,
    pub client_dropasap: usize,
    pub client_old_frags: usize,
    pub client_info_mask: usize,
    pub client_info_mask2: usize,
    pub client_name: usize,
    pub client_ex_inventory: usize,
    pub ex_changed_items: usize,
    pub ex_new_items: usize,
    pub ex_item_cnt: usize,
    pub ex_item_id: usize,
    pub ex_next: usize,
}

// Layout accessors, for engine/rust/tests/abi_layout.c (the facts table) and
// the differential harness.
macro_rules! accessor {
    ($name:ident, $e:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            $e
        }
    };
}
accessor!(SvMainDatagramFacts_sizeof, core::mem::size_of::<SvMainDatagramFacts>());
accessor!(
    SvMainDatagramFacts_offsetof_v_cameramode,
    core::mem::offset_of!(SvMainDatagramFacts, v_cameramode)
);
accessor!(
    SvMainDatagramFacts_offsetof_client_message,
    core::mem::offset_of!(SvMainDatagramFacts, client_message)
);
accessor!(
    SvMainDatagramFacts_offsetof_ex_next,
    core::mem::offset_of!(SvMainDatagramFacts, ex_next)
);
accessor!(
    SvMainDatagramFacts_offsetof_sc2_obj2,
    core::mem::offset_of!(SvMainDatagramFacts, sc2_obj2)
);

extern "C" {
    static SvMainDatagram_TargetFacts: SvMainDatagramFacts;
    fn SvMain_HostClient() -> *mut *mut u8;
    fn SvMain_Clients() -> *mut u8;
    fn SvMain_MaxClients() -> c_int;
    fn SvMain_Datagram() -> *mut SizeBufC;
    fn SvMain_ReliableDatagram() -> *mut SizeBufC;
    fn SvMain_Time() -> f64;
    fn SvMain_Protocol() -> c_int;
    fn SvMain_SetDatagramDropped(v: c_int);
    fn SvMain_InfoMask() -> c_uint;
    fn SvMain_InfoMask2() -> c_uint;
    fn SvMain_MaxDatagramCvar() -> c_int;
    fn SvMain_Realtime() -> f64;
    fn SvMain_Edicts() -> *mut u8;

    fn SV_ModelIndex(name: *const c_char) -> c_int;
    fn SV_SendSignonBuffer(client: *mut u8);
    fn SV_SetIdealPitch();
    fn SV_DropClient(crash: c_int);
    fn PR_GetString(num: c_int) -> *const c_char;
    fn INV_UpdateExItem(page: *mut u8, inv_id: c_int, inv_cnt: c_int, inc: c_int) -> c_int;
    fn NET_MaxUnreliableMessage(sock: *mut u8) -> c_int;
    fn NET_IsLocalConnection(sock: *mut u8) -> c_int;
    fn NET_CanSendMessage(sock: *mut u8) -> c_int;
    fn NET_SendUnreliableMessage(sock: *mut u8, msg: *mut SizeBufC) -> c_int;
    fn NET_SendMessage(sock: *mut u8, msg: *mut SizeBufC) -> c_int;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}

#[inline(always)]
unsafe fn facts() -> &'static SvMainDatagramFacts {
    &*(&raw const SvMainDatagram_TargetFacts)
}

/// PROTOCOL_RAVEN_111 / PROTOCOL_UH2_114, from slice A's facts.
#[inline(always)]
unsafe fn protocol_raven_111() -> c_int {
    crate::sv_main_sound::protocol_raven_111()
}

#[inline(always)]
unsafe fn protocol_uh2_114() -> c_int {
    crate::sv_main_sound::protocol_uh2_114()
}

/// MAX_DATAGRAM_OLD, SV_MAXDATAGRAM_FLOOR's value, from slice A's facts.
#[inline(always)]
unsafe fn sv_maxdatagram_floor() -> c_int {
    crate::sv_main_sound::max_datagram_old()
}

/// A float entvars_t member, by offset from `v`.
#[inline(always)]
unsafe fn vf(ent: *mut u8, member: usize) -> f32 {
    *ent.add(facts().edict_v + member).cast::<f32>()
}

/// A float entvars_t member's address, for writes and vector reads.
#[inline(always)]
unsafe fn vp(ent: *mut u8, member: usize) -> *mut f32 {
    ent.add(facts().edict_v + member).cast::<f32>()
}

/// An int entvars_t member (dmg_inflictor, weaponmodel, cameramode,
/// puzzle_inv*): compared and passed as ints, never floats.
#[inline(always)]
unsafe fn vi(ent: *mut u8, member: usize) -> c_int {
    *ent.add(facts().edict_v + member).cast::<c_int>()
}

/// A client_t int member.
#[inline(always)]
unsafe fn cli(client: *mut u8, member: usize) -> c_int {
    *(client.add(member) as *mut c_int)
}

/// A client_t int member's address, for writes.
#[inline(always)]
unsafe fn clip(client: *mut u8, member: usize) -> *mut c_int {
    client.add(member) as *mut c_int
}

/// A client_t pointer member.
#[inline(always)]
unsafe fn clp(client: *mut u8, member: usize) -> *mut u8 {
    *(client.add(member) as *mut *mut u8)
}

/// A client_t sizebuf_t member.
#[inline(always)]
unsafe fn clmsg(client: *mut u8, member: usize) -> *mut SizeBufC {
    client.add(member) as *mut SizeBufC
}

/// A client_t double member.
#[inline(always)]
unsafe fn cld(client: *mut u8, member: usize) -> f64 {
    *(client.add(member) as *mut f64)
}

/// A client_t double member's address, for writes.
#[inline(always)]
unsafe fn cldp(client: *mut u8, member: usize) -> *mut f64 {
    client.add(member) as *mut f64
}

/// A client_t byte member (active, spawned, sendsignon, dropasap,
/// send_all_v).
#[inline(always)]
unsafe fn clb(client: *mut u8, member: usize) -> c_int {
    *client.add(member) as c_int
}

/// A client_t byte member's address, for writes.
#[inline(always)]
unsafe fn clbp(client: *mut u8, member: usize) -> *mut c_uchar {
    client.add(member)
}

/// sv_main.c's function-local `static int next_update` / `static int
/// next_count`: every third call forces one more SU_* bit, cycling through
/// the twelve.  Unresettable, like the C's; the differential harness runs
/// both sides in lockstep call counts.
static mut NEXT_UPDATE: c_int = 0;
static mut NEXT_COUNT: c_int = 0;

/// SV_ClearDatagram: the broadcast datagram is cleared every frame, and a
/// QuakeC broadcast write abandons it again (pr_cmds.c's WriteDest sets the
/// flag this clears).
#[no_mangle]
pub unsafe extern "C" fn SV_ClearDatagram() {
    SZ_Clear(SvMain_Datagram());
    SvMain_SetDatagramDropped(0);
}

/// SV_WriteClientdataToMessage: the per-client delta block -- damage,
/// setangle, the SU_* view/weapon deltas -- to `msg`, and the inventory
/// block -- the SC1_*/SC2_* stats and the extended-inventory pages -- to
/// `host_client->message`, the reliable stream.  The split is the C's:
/// `client` is the parameter the damage/clientdata half reads and whose
/// `old_v` the end memcpy refreshes; `host_client` is the global the
/// inventory half reads and writes.  At the only call site they are the
/// same client.
#[no_mangle]
pub unsafe extern "C" fn SV_WriteClientdataToMessage(
    client: *mut u8,
    ent: *mut u8,
    msg: *mut SizeBufC,
) {
    let f = facts();
    let old_v = client.add(f.client_old_v);
    // host_client, the global this half of the function is written against.
    let hc = *SvMain_HostClient();
    let hc_old_v = hc.add(f.client_old_v);
    let hc_msg = clmsg(hc, f.client_message);
    let ex = clp(hc, f.client_ex_inventory);

    //
    // send a damage message
    //
    if vf(ent, f.v_dmg_take) != 0.0 || vf(ent, f.v_dmg_save) != 0.0 {
        // PROG_TO_EDICT: a pure byte offset from sv.edicts (progs.h).
        let other = SvMain_Edicts().offset(vi(ent, f.v_dmg_inflictor) as isize);
        MSG_WriteByte(msg, f.msg_damage);
        MSG_WriteByte(msg, cvt_c(vf(ent, f.v_dmg_save)));
        MSG_WriteByte(msg, cvt_c(vf(ent, f.v_dmg_take)));
        for i in 0..3 {
            let i = i as usize;
            // C's usual arithmetic conversions: mins+maxs in float, the
            // 0.5 product and the sum in double, narrowed to float for
            // MSG_WriteCoord.
            let o = *vp(other, f.v_origin).add(i);
            let mn = *vp(other, f.v_mins).add(i);
            let mx = *vp(other, f.v_maxs).add(i);
            MSG_WriteCoord(msg, (o as f64 + 0.5 * ((mn + mx) as f64)) as f32);
        }
        *vp(ent, f.v_dmg_take) = 0.0;
        *vp(ent, f.v_dmg_save) = 0.0;
    }

    //
    // send the current viewpos offset from the view entity
    //
    SV_SetIdealPitch(); // how much to look up / down ideally

    // a fixangle might get lost in a dropped packet.  Oh well.
    if vf(ent, f.v_fixangle) != 0.0 {
        MSG_WriteByte(msg, f.msg_setangle);
        for i in 0..3 {
            MSG_WriteAngle(msg, *vp(ent, f.v_angles).add(i as usize));
        }
        *vp(ent, f.v_fixangle) = 0.0;
    }

    let mut bits: c_int = 0;

    if clb(client, f.client_send_all_v) != 0 {
        bits = f.su_viewheight
            | f.su_idealpitch
            | f.su_idealroll
            | f.su_velocity1
            | (f.su_velocity1 << 1)
            | (f.su_velocity1 << 2)
            | f.su_punch1
            | (f.su_punch1 << 1)
            | (f.su_punch1 << 2)
            | f.su_weaponframe
            | f.su_armor
            | f.su_weapon;
    } else {
        if *vp(ent, f.v_view_ofs).add(2) != *vp(old_v, f.v_view_ofs).add(2) {
            bits |= f.su_viewheight;
        }
        if vf(ent, f.v_idealpitch) != vf(old_v, f.v_idealpitch) {
            bits |= f.su_idealpitch;
        }
        if vf(ent, f.v_idealroll) != vf(old_v, f.v_idealroll) {
            bits |= f.su_idealroll;
        }
        for i in 0..3 {
            let i = i as usize;
            if *vp(ent, f.v_punchangle).add(i) != *vp(old_v, f.v_punchangle).add(i) {
                bits |= f.su_punch1 << i as c_int;
            }
            if *vp(ent, f.v_velocity).add(i) != *vp(old_v, f.v_velocity).add(i) {
                bits |= f.su_velocity1 << i as c_int;
            }
        }
        if vf(ent, f.v_weaponframe) != vf(old_v, f.v_weaponframe) {
            bits |= f.su_weaponframe;
        }
        if vf(ent, f.v_armorvalue) != vf(old_v, f.v_armorvalue) {
            bits |= f.su_armor;
        }
        // weaponmodel is an int field: compared as ints, not floats.
        if vi(ent, f.v_weaponmodel) != vi(old_v, f.v_weaponmodel) {
            bits |= f.su_weapon;
        }
    }

    // send the data

    //fjm: this wasn't in here b4, and the centerview command requires it.
    if cvt_c(vf(ent, f.v_flags)) & f.fl_onground != 0 {
        bits |= f.su_onground;
    }

    NEXT_COUNT += 1;
    if NEXT_COUNT >= 3 {
        NEXT_COUNT = 0;
        NEXT_UPDATE += 1;
        if NEXT_UPDATE > 11 {
            NEXT_UPDATE = 0;
        }
        match NEXT_UPDATE {
            0 => bits |= f.su_viewheight,
            1 => bits |= f.su_idealpitch,
            2 => bits |= f.su_idealroll,
            3 => bits |= f.su_velocity1,
            4 => bits |= f.su_velocity1 << 1,
            5 => bits |= f.su_velocity1 << 2,
            6 => bits |= f.su_punch1,
            7 => bits |= f.su_punch1 << 1,
            8 => bits |= f.su_punch1 << 2,
            9 => bits |= f.su_weaponframe,
            10 => bits |= f.su_armor,
            _ => bits |= f.su_weapon,
        }
    }

    MSG_WriteByte(msg, f.msg_clientdata);
    MSG_WriteShort(msg, bits);

    if bits & f.su_viewheight != 0 {
        MSG_WriteChar(msg, cvt_c(*vp(ent, f.v_view_ofs).add(2)));
    }
    if bits & f.su_idealpitch != 0 {
        MSG_WriteChar(msg, cvt_c(vf(ent, f.v_idealpitch)));
    }
    if bits & f.su_idealroll != 0 {
        MSG_WriteChar(msg, cvt_c(vf(ent, f.v_idealroll)));
    }
    for i in 0..3 {
        let i = i as usize;
        if bits & (f.su_punch1 << i as c_int) != 0 {
            MSG_WriteChar(msg, cvt_c(*vp(ent, f.v_punchangle).add(i)));
        }
        if bits & (f.su_velocity1 << i as c_int) != 0 {
            MSG_WriteChar(msg, cvt_c(*vp(ent, f.v_velocity).add(i) / 16.0));
        }
    }
    if bits & f.su_weaponframe != 0 {
        MSG_WriteByte(msg, cvt_c(vf(ent, f.v_weaponframe)));
    }
    if bits & f.su_armor != 0 {
        MSG_WriteByte(msg, cvt_c(vf(ent, f.v_armorvalue)));
    }
    if bits & f.su_weapon != 0 {
        MSG_WriteShort(msg, SV_ModelIndex(PR_GetString(vi(ent, f.v_weaponmodel))));
    }

    // The inventory half: host_client's old_v, host_client's message.
    let (mut sc1, mut sc2, mut sc3, mut sc4): (c_int, c_int, c_int, c_int);
    if clb(hc, f.client_send_all_v) != 0 {
        sc1 = -1;
        sc2 = -1;
        *clbp(hc, f.client_send_all_v) = 0;
        sc3 = 0;
        sc4 = 0;
    } else {
        sc1 = 0;
        sc2 = 0;
        if SvMain_Protocol() >= protocol_uh2_114() {
            sc3 = *(ex.add(f.ex_changed_items) as *mut c_int);
            sc4 = *(ex.add(f.ex_new_items) as *mut c_int);
        } else {
            sc3 = 0;
            sc4 = 0;
        }

        // One comparison per stat, against host_client's old_v.  The
        // macro keeps the sixty-odd lines from drifting; cameramode is
        // the int field among them.
        macro_rules! sc {
            ($acc:ident, $field:ident, $bit:ident) => {
                if vf(ent, f.$field) != vf(hc_old_v, f.$field) {
                    $acc |= f.$bit;
                }
            };
        }
        sc!(sc1, v_health, sc1_health);
        sc!(sc1, v_level, sc1_level);
        sc!(sc1, v_intelligence, sc1_intelligence);
        sc!(sc1, v_wisdom, sc1_wisdom);
        sc!(sc1, v_strength, sc1_strength);
        sc!(sc1, v_dexterity, sc1_dexterity);
        sc!(sc1, v_weapon, sc1_weapon);
        sc!(sc1, v_bluemana, sc1_bluemana);
        sc!(sc1, v_greenmana, sc1_greenmana);
        sc!(sc1, v_experience, sc1_experience);
        sc!(sc1, v_cnt_torch, sc1_cnt_torch);
        sc!(sc1, v_cnt_h_boost, sc1_cnt_h_boost);
        sc!(sc1, v_cnt_sh_boost, sc1_cnt_sh_boost);
        sc!(sc1, v_cnt_mana_boost, sc1_cnt_mana_boost);
        sc!(sc1, v_cnt_teleport, sc1_cnt_teleport);
        sc!(sc1, v_cnt_tome, sc1_cnt_tome);
        sc!(sc1, v_cnt_summon, sc1_cnt_summon);
        sc!(sc1, v_cnt_invisibility, sc1_cnt_invisibility);
        sc!(sc1, v_cnt_glyph, sc1_cnt_glyph);
        sc!(sc1, v_cnt_haste, sc1_cnt_haste);
        sc!(sc1, v_cnt_blast, sc1_cnt_blast);
        sc!(sc1, v_cnt_polymorph, sc1_cnt_polymorph);
        sc!(sc1, v_cnt_flight, sc1_cnt_flight);
        sc!(sc1, v_cnt_cubeofforce, sc1_cnt_cubeofforce);
        sc!(sc1, v_cnt_invincibility, sc1_cnt_invincibility);
        sc!(sc1, v_artifact_active, sc1_artifact_active);
        sc!(sc1, v_artifact_low, sc1_artifact_low);
        sc!(sc1, v_movetype, sc1_movetype);
        if vi(ent, f.v_cameramode) != vi(hc_old_v, f.v_cameramode) {
            sc1 |= f.sc1_cameramode;
        }
        sc!(sc1, v_hasted, sc1_hasted);
        sc!(sc1, v_inventory, sc1_inventory);
        sc!(sc1, v_rings_active, sc1_rings_active);
        sc!(sc2, v_rings_low, sc2_rings_low);
        sc!(sc2, v_armor_amulet, sc2_amulet);
        sc!(sc2, v_armor_bracer, sc2_bracer);
        sc!(sc2, v_armor_breastplate, sc2_breastplate);
        sc!(sc2, v_armor_helmet, sc2_helmet);
        sc!(sc2, v_ring_flight, sc2_flight_t);
        sc!(sc2, v_ring_water, sc2_water_t);
        sc!(sc2, v_ring_turning, sc2_turning_t);
        sc!(sc2, v_ring_regeneration, sc2_regen_t);
        sc!(sc2, v_haste_time, sc2_haste_t);
        sc!(sc2, v_tome_time, sc2_tome_t);
        // puzzle_inv* are string_t (int): compared as ints, not floats.
        macro_rules! sc_i {
            ($acc:ident, $field:ident, $bit:ident) => {
                if vi(ent, f.$field) != vi(hc_old_v, f.$field) {
                    $acc |= f.$bit;
                }
            };
        }
        sc_i!(sc2, v_puzzle_inv1, sc2_puzzle1);
        sc_i!(sc2, v_puzzle_inv2, sc2_puzzle2);
        sc_i!(sc2, v_puzzle_inv3, sc2_puzzle3);
        sc_i!(sc2, v_puzzle_inv4, sc2_puzzle4);
        sc_i!(sc2, v_puzzle_inv5, sc2_puzzle5);
        sc_i!(sc2, v_puzzle_inv6, sc2_puzzle6);
        sc_i!(sc2, v_puzzle_inv7, sc2_puzzle7);
        sc_i!(sc2, v_puzzle_inv8, sc2_puzzle8);
        sc!(sc2, v_max_health, sc2_maxhealth);
        sc!(sc2, v_max_mana, sc2_maxmana);
        sc!(sc2, v_flags, sc2_flags);

        // mission pack, objectives.  info_mask is the file-scope global;
        // client->info_mask is the parameter's.
        if SvMain_Protocol() > protocol_raven_111() {
            if SvMain_InfoMask() != *(client.add(f.client_info_mask) as *mut c_uint) {
                sc2 |= f.sc2_obj;
            }
            if SvMain_InfoMask2() != *(client.add(f.client_info_mask2) as *mut c_uint) {
                sc2 |= f.sc2_obj2;
            }
        }
    }

    // The C's `goto end': no inventory traffic, but old_v still refreshes.
    if sc1 != 0 || sc2 != 0 || sc3 != 0 || sc4 != 0 {
        MSG_WriteByte(hc_msg, f.msg_update_inv);
        // The C's `byte test': which sc bytes are non-zero, as a mask.
        // sc1/sc2 as u32 so the 0xff000000 test is well-defined.
        let sc1u = sc1 as u32;
        let sc2u = sc2 as u32;
        let mut test: c_int = 0;
        if sc1u & 0x000000ff != 0 {
            test |= 1;
        }
        if sc1u & 0x0000ff00 != 0 {
            test |= 2;
        }
        if sc1u & 0x00ff0000 != 0 {
            test |= 4;
        }
        if sc1u & 0xff000000 != 0 {
            test |= 8;
        }
        if sc2u & 0x000000ff != 0 {
            test |= 16;
        }
        if sc2u & 0x0000ff00 != 0 {
            test |= 32;
        }
        if sc2u & 0x00ff0000 != 0 {
            test |= 64;
        }
        if sc2u & 0xff000000 != 0 {
            test |= 128;
        }

        MSG_WriteByte(hc_msg, test);

        if test & 1 != 0 {
            MSG_WriteByte(hc_msg, sc1 & 0xff);
        }
        if test & 2 != 0 {
            MSG_WriteByte(hc_msg, (sc1 >> 8) & 0xff);
        }
        if test & 4 != 0 {
            MSG_WriteByte(hc_msg, (sc1 >> 16) & 0xff);
        }
        if test & 8 != 0 {
            MSG_WriteByte(hc_msg, (sc1 >> 24) & 0xff);
        }
        if test & 16 != 0 {
            MSG_WriteByte(hc_msg, sc2 & 0xff);
        }
        if test & 32 != 0 {
            MSG_WriteByte(hc_msg, (sc2 >> 8) & 0xff);
        }
        if test & 64 != 0 {
            MSG_WriteByte(hc_msg, (sc2 >> 16) & 0xff);
        }
        if test & 128 != 0 {
            MSG_WriteByte(hc_msg, (sc2 >> 24) & 0xff);
        }

        macro_rules! wbyte {
            ($acc:ident, $bit:ident, $field:ident) => {
                if $acc & f.$bit != 0 {
                    MSG_WriteByte(hc_msg, cvt_c(vf(ent, f.$field)));
                }
            };
        }
        if sc1 & f.sc1_health != 0 {
            MSG_WriteShort(hc_msg, cvt_c(vf(ent, f.v_health)));
        }
        wbyte!(sc1, sc1_level, v_level);
        wbyte!(sc1, sc1_intelligence, v_intelligence);
        wbyte!(sc1, sc1_wisdom, v_wisdom);
        wbyte!(sc1, sc1_strength, v_strength);
        wbyte!(sc1, sc1_dexterity, v_dexterity);
        wbyte!(sc1, sc1_weapon, v_weapon);
        wbyte!(sc1, sc1_bluemana, v_bluemana);
        wbyte!(sc1, sc1_greenmana, v_greenmana);
        if sc1 & f.sc1_experience != 0 {
            MSG_WriteLong(hc_msg, cvt_c(vf(ent, f.v_experience)));
        }
        // The fifteen artifact counts, each also refreshing the
        // extended-inventory page the client renders from.
        macro_rules! cnt {
            ($bit:ident, $field:ident, $id:expr) => {
                if sc1 & f.$bit != 0 {
                    let n = cvt_c(vf(ent, f.$field));
                    MSG_WriteByte(hc_msg, n);
                    INV_UpdateExItem(ex, $id, n, 0);
                }
            };
        }
        cnt!(sc1_cnt_torch, v_cnt_torch, 1);
        cnt!(sc1_cnt_h_boost, v_cnt_h_boost, 2);
        cnt!(sc1_cnt_sh_boost, v_cnt_sh_boost, 3);
        cnt!(sc1_cnt_mana_boost, v_cnt_mana_boost, 4);
        cnt!(sc1_cnt_teleport, v_cnt_teleport, 5);
        cnt!(sc1_cnt_tome, v_cnt_tome, 6);
        cnt!(sc1_cnt_summon, v_cnt_summon, 7);
        cnt!(sc1_cnt_invisibility, v_cnt_invisibility, 8);
        cnt!(sc1_cnt_glyph, v_cnt_glyph, 9);
        cnt!(sc1_cnt_haste, v_cnt_haste, 10);
        cnt!(sc1_cnt_blast, v_cnt_blast, 11);
        cnt!(sc1_cnt_polymorph, v_cnt_polymorph, 12);
        cnt!(sc1_cnt_flight, v_cnt_flight, 13);
        cnt!(sc1_cnt_cubeofforce, v_cnt_cubeofforce, 14);
        cnt!(sc1_cnt_invincibility, v_cnt_invincibility, 15);
        if sc1 & f.sc1_artifact_active != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_artifact_active));
        }
        if sc1 & f.sc1_artifact_low != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_artifact_low));
        }
        wbyte!(sc1, sc1_movetype, v_movetype);
        if sc1 & f.sc1_cameramode != 0 {
            MSG_WriteByte(hc_msg, vi(ent, f.v_cameramode));
        }
        if sc1 & f.sc1_hasted != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_hasted));
        }
        wbyte!(sc1, sc1_inventory, v_inventory);
        if sc1 & f.sc1_rings_active != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_rings_active));
        }

        if sc2 & f.sc2_rings_low != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_rings_low));
        }
        wbyte!(sc2, sc2_amulet, v_armor_amulet);
        wbyte!(sc2, sc2_bracer, v_armor_bracer);
        wbyte!(sc2, sc2_breastplate, v_armor_breastplate);
        wbyte!(sc2, sc2_helmet, v_armor_helmet);
        wbyte!(sc2, sc2_flight_t, v_ring_flight);
        wbyte!(sc2, sc2_water_t, v_ring_water);
        wbyte!(sc2, sc2_turning_t, v_ring_turning);
        wbyte!(sc2, sc2_regen_t, v_ring_regeneration);
        if sc2 & f.sc2_haste_t != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_haste_time));
        }
        if sc2 & f.sc2_tome_t != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_tome_time));
        }
        macro_rules! puzzle {
            ($bit:ident, $field:ident) => {
                if sc2 & f.$bit != 0 {
                    MSG_WriteString(hc_msg, PR_GetString(vi(ent, f.$field)));
                }
            };
        }
        puzzle!(sc2_puzzle1, v_puzzle_inv1);
        puzzle!(sc2_puzzle2, v_puzzle_inv2);
        puzzle!(sc2_puzzle3, v_puzzle_inv3);
        puzzle!(sc2_puzzle4, v_puzzle_inv4);
        puzzle!(sc2_puzzle5, v_puzzle_inv5);
        puzzle!(sc2_puzzle6, v_puzzle_inv6);
        puzzle!(sc2_puzzle7, v_puzzle_inv7);
        puzzle!(sc2_puzzle8, v_puzzle_inv8);
        if sc2 & f.sc2_maxhealth != 0 {
            MSG_WriteShort(hc_msg, cvt_c(vf(ent, f.v_max_health)));
        }
        wbyte!(sc2, sc2_maxmana, v_max_mana);
        if sc2 & f.sc2_flags != 0 {
            MSG_WriteFloat(hc_msg, vf(ent, f.v_flags));
        }

        // mission pack, objectives.  client->info_mask is the parameter's.
        if SvMain_Protocol() > protocol_raven_111() {
            if sc2 & f.sc2_obj != 0 {
                MSG_WriteLong(hc_msg, SvMain_InfoMask() as c_int);
                *(client.add(f.client_info_mask) as *mut c_uint) = SvMain_InfoMask();
            }
            if sc2 & f.sc2_obj2 != 0 {
                MSG_WriteLong(hc_msg, SvMain_InfoMask2() as c_int);
                *(client.add(f.client_info_mask2) as *mut c_uint) = SvMain_InfoMask2();
            }
        }

        // extended inventory.  The page loop zeroes each page's change
        // flags after writing it; a page with a next page always sends
        // its test byte, even when nothing changed.
        if SvMain_Protocol() >= protocol_uh2_114() {
            let mut page = ex;
            while !page.is_null() {
                sc3 = *(page.add(f.ex_changed_items) as *mut c_int);
                sc4 = *(page.add(f.ex_new_items) as *mut c_int);
                let sc3u = sc3 as u32;

                test = 0;
                if sc3u & 0x000000ff != 0 {
                    test |= 1;
                }
                if sc3u & 0x0000ff00 != 0 {
                    test |= 2;
                }
                if sc3u & 0x00ff0000 != 0 {
                    test |= 4;
                }
                if sc3u & 0xff000000 != 0 {
                    test |= 8;
                }
                if !(*(page.add(f.ex_next) as *mut *mut u8)).is_null() {
                    test |= 16;
                }

                MSG_WriteByte(hc_msg, test);

                if test != 0 {
                    if test & 1 != 0 {
                        MSG_WriteByte(hc_msg, sc3 & 0xff);
                    }
                    if test & 2 != 0 {
                        MSG_WriteByte(hc_msg, (sc3 >> 8) & 0xff);
                    }
                    if test & 4 != 0 {
                        MSG_WriteByte(hc_msg, (sc3 >> 16) & 0xff);
                    }
                    if test & 8 != 0 {
                        MSG_WriteByte(hc_msg, (sc3 >> 24) & 0xff);
                    }

                    for i in 0..f.max_inventory_ex as u32 {
                        if sc3u & (1u32 << i) != 0 {
                            let cntp = (page.add(f.ex_item_cnt) as *mut c_int).add(i as usize);
                            let newbit = if (sc4 as u32) & (1u32 << i) != 0 {
                                128
                            } else {
                                0
                            };
                            MSG_WriteByte(hc_msg, *cntp + newbit);
                            if (sc4 as u32) & (1u32 << i) != 0 {
                                let idp =
                                    (page.add(f.ex_item_id) as *mut c_int).add(i as usize);
                                MSG_WriteByte(hc_msg, *idp);
                            }
                        }
                    }

                    *(page.add(f.ex_changed_items) as *mut c_int) = 0;
                    *(page.add(f.ex_new_items) as *mut c_int) = 0;
                }

                page = *(page.add(f.ex_next) as *mut *mut u8);
            }
        }
    }

    // end: old_v is the parameter client's, refreshed every call.
    memcpy(
        client.add(f.client_old_v) as *mut c_void,
        ent.add(f.edict_v) as *const c_void,
        f.entvars_size,
    );
}

/// SV_SendClientDatagram: one client's unreliable datagram for this frame.
/// Built into a stack buffer but sized to `wire` -- what the connection can
/// actually carry -- never to the buffer: Datagram_SendUnreliableMessage
/// memcpys into a fixed MAX_DATAGRAM packet buffer, so the 32 KB
/// sizeof(buf) would authorise is corruption, not headroom.  The entity
/// section (slice B) sheds worst-priority-first to fit instead.
unsafe fn send_client_datagram(client: *mut u8) -> c_int {
    let f = facts();
    let mut buf = [0u8; NET_MAXMESSAGE];
    // SZ_Init memsets the header itself.
    let mut msg: SizeBufC = core::mem::zeroed();

    let netconn = clp(client, f.client_netconnection);
    let mut wire = NET_MaxUnreliableMessage(netconn);

    // ...and no larger than the negotiated protocol lets this client
    // receive.  Skipped for a local connection: both ends are this process,
    // so there is no other build's buffer to fit inside and single player
    // keeps the full entity budget whatever sv_protocol says.
    if NET_IsLocalConnection(netconn) == 0 {
        let max = SV_MaxDatagram();
        if wire > max {
            wire = max;
        }
    }

    // ...and no larger than sv_maxdatagram, when set (GitHub #82).
    let cap0 = SvMain_MaxDatagramCvar();
    if cap0 > 0 {
        let mut cap = cap0;
        if cap < sv_maxdatagram_floor() {
            cap = sv_maxdatagram_floor();
        }
        if wire > cap {
            wire = cap;
        }
    }

    SZ_Init(&mut msg, buf.as_mut_ptr(), wire);
    msg.name = c"client datagram".as_ptr();

    MSG_WriteByte(&mut msg, f.msg_time);
    MSG_WriteFloat(&mut msg, SvMain_Time() as f32);

    // add the client specific data to the datagram
    let edict = clp(client, f.client_edict);
    SV_WriteClientdataToMessage(client, edict, &mut msg);

    // Hold room for the two datagrams appended below before handing the
    // rest to the entity deltas.  Entity state is re-sent every frame and
    // survives being shed; a temp entity or a sound is sent once and is
    // simply gone if it does not fit.  Never squeeze the entity section
    // below half, though: QC that keeps sv.datagram permanently full would
    // otherwise freeze the visible world rather than lose an effect.
    let sv_datagram = SvMain_Datagram();
    let client_datagram = clmsg(client, f.client_datagram);
    let mut pending = (*sv_datagram).cursize + (*client_datagram).cursize;
    if pending > wire / 2 {
        pending = wire / 2;
    }
    msg.maxsize = wire - pending;

    SV_PrepareClientEntities(client, edict, &mut msg);

    msg.maxsize = wire;

    // copy the server datagram if there is space
    if msg.cursize + (*sv_datagram).cursize < msg.maxsize {
        SZ_Write(
            &mut msg,
            (*sv_datagram).data as *const c_void,
            (*sv_datagram).cursize,
        );
    }
    if msg.cursize + (*client_datagram).cursize < msg.maxsize {
        SZ_Write(
            &mut msg,
            (*client_datagram).data as *const c_void,
            (*client_datagram).cursize,
        );
    }
    SZ_Clear(client_datagram);

    // dev_stats.packetsize: not written; see DEPARTURES above.

    // send the datagram
    if NET_SendUnreliableMessage(netconn, &mut msg) == -1 {
        SV_DropClient(1); // if the message couldn't send, kick off
        return 0;
    }
    1
}

/// SV_UpdateToReliableMessages: frag changes and the server's reliable
/// datagram, onto every active client's reliable stream.
unsafe fn update_to_reliable_messages() {
    let f = facts();
    let maxclients = SvMain_MaxClients();
    let clients = SvMain_Clients();

    // check for changes to be sent over the reliable streams
    let mut i: c_int = 0;
    while i < maxclients {
        let hc = clients.add(i as usize * f.client_size);
        *SvMain_HostClient() = hc;
        let ent = clp(hc, f.client_edict);
        // int != float: the int widens, as C's usual arithmetic
        // conversions do it.
        if cli(hc, f.client_old_frags) as f32 != vf(ent, f.v_frags) {
            let mut j: c_int = 0;
            while j < maxclients {
                let client = clients.add(j as usize * f.client_size);
                if clb(client, f.client_active) != 0 {
                    let cmsg = clmsg(client, f.client_message);
                    MSG_WriteByte(cmsg, f.msg_updatefrags);
                    MSG_WriteByte(cmsg, i);
                    MSG_WriteShort(cmsg, cvt_c(vf(ent, f.v_frags)));
                }
                j += 1;
            }
            *clip(hc, f.client_old_frags) = cvt_c(vf(ent, f.v_frags));
        }
        i += 1;
    }

    let mut j: c_int = 0;
    while j < maxclients {
        let client = clients.add(j as usize * f.client_size);
        if clb(client, f.client_active) != 0 {
            let rel = SvMain_ReliableDatagram();
            SZ_Write(
                clmsg(client, f.client_message),
                (*rel).data as *const c_void,
                (*rel).cursize,
            );
        }
        j += 1;
    }
    SZ_Clear(SvMain_ReliableDatagram());
}

/// SV_SendNop: a nop keepalive that neither trashes nor sends the
/// accumulated client message buffer.
unsafe fn send_nop(client: *mut u8) {
    let f = facts();
    let mut buf = [0u8; 4];
    let mut msg: SizeBufC = core::mem::zeroed();

    SZ_Init(&mut msg, buf.as_mut_ptr(), buf.len() as c_int);

    MSG_WriteChar(&mut msg, f.msg_nop);

    if NET_SendUnreliableMessage(clp(client, f.client_netconnection), &mut msg) == -1 {
        SV_DropClient(1); // if the message couldn't send, kick off
    }
    *cldp(client, f.client_last_message) = SvMain_Realtime();
}

/// SV_SendClientMessages: the per-frame server-to-client pass.  Frags and
/// the reliable datagram first, then each active client: a full datagram
/// for the spawned, a signon buffer or a nop keepalive for the rest, then
/// the reliable stream -- dropped outright when it overflows or outgrows
/// the protocol -- and finally the muzzle-flash clear.
#[no_mangle]
pub unsafe extern "C" fn SV_SendClientMessages() {
    let f = facts();

    // update frags, names, etc
    update_to_reliable_messages();

    // build individual updates
    let maxclients = SvMain_MaxClients();
    let clients = SvMain_Clients();
    let mut i: c_int = 0;
    while i < maxclients {
        let hc = clients.add(i as usize * f.client_size);
        *SvMain_HostClient() = hc;
        if clb(hc, f.client_active) != 0 {
            // Whether the frame reaches the reliable-send half below;
            // the C spells it with `continue`.
            let mut send_reliable = true;
            if clb(hc, f.client_spawned) != 0 {
                if send_client_datagram(hc) == 0 {
                    send_reliable = false;
                }
            } else {
                // the player isn't totally in the game yet
                // send small keepalive messages if too much time has passed
                // send a full message when the next signon stage has been requested
                // some other message data (name changes, etc) may accumulate
                // between signon stages

                // Feed out the signon list, one buffer per reliable
                // message.  Only once the previous one has actually
                // gone -- a signon buffer is half of client->message,
                // so stacking two, or one on top of queued name/colour
                // updates, would overflow it.  uhexen2-z5wt.
                let hmsg = clmsg(hc, f.client_message);
                if cli(hc, f.client_signon_buffer) >= 0 && (*hmsg).cursize == 0 {
                    SV_SendSignonBuffer(hc);
                }
                if clb(hc, f.client_sendsignon) == 0 {
                    if SvMain_Realtime() - cld(hc, f.client_last_message) > 5.0 {
                        send_nop(hc);
                    }
                    // don't send out non-signon messages
                    send_reliable = false;
                }
            }

            if send_reliable {
                let hmsg = clmsg(hc, f.client_message);
                // check for an overflowed message.  Should only happen
                // on a very fucked up connection that backs up a lot, then
                // changes level
                if (*hmsg).overflowed != 0 {
                    SV_DropClient(1);
                    (*hmsg).overflowed = 0;
                } else if (*hmsg).cursize != 0 || clb(hc, f.client_dropasap) != 0 {
                    let netconn = clp(hc, f.client_netconnection);
                    // The C's I_Printf("can't write") here is commented
                    // out: when the socket can't take a message this frame,
                    // the client is simply skipped.
                    if NET_CanSendMessage(netconn) != 0 {
                        if clb(hc, f.client_dropasap) != 0 {
                            SV_DropClient(0); // went to another level
                        } else if NET_IsLocalConnection(netconn) == 0
                            && (*hmsg).cursize > SV_MaxReliableMessage()
                        {
                            // Sending this would overrun the far end's
                            // receiveMessage[] rather than merely fail, so drop
                            // the client instead -- the same outcome the
                            // overflowed check above produces, but named, and
                            // before the damage rather than after.  QC writing
                            // this much reliable data in one frame is the usual
                            // cause; sv_protocol 100 raises the ceiling from
                            // 16 KB to 32 KB for clients that can take it.
                            CON_Printf(
                                0, // _PRINT_NORMAL, as Con_Printf does
                                c"%s: %d byte reliable message exceeds the %d byte limit of protocol %i; dropping %s\n".as_ptr(),
                                c"SV_SendClientMessages".as_ptr(),
                                (*hmsg).cursize,
                                SV_MaxReliableMessage(),
                                SvMain_Protocol(),
                                hc.add(f.client_name) as *const c_char,
                            );
                            SV_DropClient(1);
                        } else {
                            if NET_SendMessage(netconn, hmsg) == -1 {
                                SV_DropClient(1); // if the message couldn't send, kick off
                            }
                            SZ_Clear(hmsg);
                            *cldp(hc, f.client_last_message) = SvMain_Realtime();
                            *clbp(hc, f.client_sendsignon) = 0;
                        }
                    }
                }
            }
        }
        i += 1;
    }

    // clear muzzle flashes
    SV_CleanupEnts();
}
