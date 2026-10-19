// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the sound and particle messages of
// engine/hexen2/sv_main.c: SV_StartParticle, SV_StartParticle2..4,
// SV_StopSound, SV_UpdateSoundPos and SV_StartSound, with SV_SoundEntityFits,
// and the per-protocol limits SV_MaxSounds, SV_MaxDatagram and
// SV_MaxReliableMessage.  #469 slice A, of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// Each of the message functions appends one message to sv.datagram, the
// unreliable buffer every client gets a copy of this frame, unless the
// buffer is already within the message's size of MAX_DATAGRAM, in which
// case the event is dropped.  The C originals moved verbatim into
// engine/rust/sv_main/tests/sv_main_sound_h2.c.
//
// Hexen II only.  HexenWorld's server has its own sv_main.c and sends
// sounds through SV_Multicast; that is #469's slice H.  The constants and
// sv's members come through engine/rust/sv_main_target.c, the edict
// offsets through world_target.c (world_hull.rs's facts).
//
// C SEMANTICS KEPT
//
//   * `(int)` of a float -- SV_StartParticle's dir*16, svc_particle3's box
//     bytes, svc_particle4's radius, svc_sound's attenuation*64 -- goes
//     through world_hull.rs's cvt_c, the target's own conversion: on x86 a
//     NaN or out-of-range value is INT_MIN, whose low byte is 0.
//   * SV_StartSound's range checks are false for a NaN attenuation, as the
//     C's are, so a NaN gets through to the field mask and is sent.
//   * The sound position is origin + 0.5*(mins+maxs): mins+maxs in float,
//     the rest in double, rounded to float for MSG_WriteCoord, as C's usual
//     arithmetic conversions do it.
//   * Host_Error does not return; the engine's longjmps to the frame's top.
//     No frame of this module holds anything with a destructor, so that
//     jump skips nothing.

use core::ffi::{c_char, c_int, c_uint};

use crate::msg_io::{
    MSG_WriteByte, MSG_WriteChar, MSG_WriteCoord, MSG_WriteFloat, MSG_WriteShort,
};
use crate::sizebuf::SizeBufC;
use crate::world_hull::{cvt_c, facts};

/// sv_main_target.h's `sv_main_target_facts_t`.
#[repr(C)]
pub struct SvMainTargetFacts {
    pub msg_particle: c_int,
    pub msg_particle2: c_int,
    pub msg_particle3: c_int,
    pub msg_particle4: c_int,
    pub msg_sound: c_int,
    pub msg_stopsound: c_int,
    pub msg_stopsound_large: c_int,
    pub msg_sound_update_pos: c_int,
    pub msg_sound_update_pos_large: c_int,
    pub snd_volume: c_int,
    pub snd_attenuation: c_int,
    pub snd_overflow: c_int,
    pub snd_overflow2: c_int,
    pub snd_largeentity: c_int,
    pub snd_max_packed_entity: c_int,
    pub default_sound_packet_volume: c_int,
    pub max_datagram: c_int,
    pub max_datagram_old: c_int,
    pub max_datagram_114: c_int,
    pub max_msglen: c_int,
    pub max_msglen_old: c_int,
    pub max_sounds: c_int,
    pub max_sounds_old: c_int,
    pub max_sounds_h2mp: c_int,
    pub max_sounds_uh2: c_int,
    pub protocol_raven_111: c_int,
    pub protocol_uh2_114: c_int,
    pub protocol_hexenwail_1: c_int,
    pub default_sound_packet_attenuation: f64,
}

// ABI accessors for engine/rust/tests/abi_layout.c.
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_sizeof() -> usize {
    core::mem::size_of::<SvMainTargetFacts>()
}
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_offsetof_msg_sound_update_pos_large() -> usize {
    core::mem::offset_of!(SvMainTargetFacts, msg_sound_update_pos_large)
}
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_offsetof_default_sound_packet_volume() -> usize {
    core::mem::offset_of!(SvMainTargetFacts, default_sound_packet_volume)
}
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_offsetof_max_sounds_uh2() -> usize {
    core::mem::offset_of!(SvMainTargetFacts, max_sounds_uh2)
}
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_offsetof_protocol_hexenwail_1() -> usize {
    core::mem::offset_of!(SvMainTargetFacts, protocol_hexenwail_1)
}
#[no_mangle]
pub extern "C" fn SvMainTargetFacts_offsetof_default_sound_packet_attenuation() -> usize {
    core::mem::offset_of!(SvMainTargetFacts, default_sound_packet_attenuation)
}

/// printsys.h: Con_Printf is _PRINT_NORMAL, Con_DPrintf _PRINT_DEVEL.
const PRINT_NORMAL: c_uint = 0;
const PRINT_DEVEL: c_uint = 2;

extern "C" {
    static SvMain_TargetFacts: SvMainTargetFacts;
    fn SvMain_Datagram() -> *mut SizeBufC;
    fn SvMain_SoundPrecache() -> *const *const c_char;
    static sv_protocol: c_int;
    fn NUM_FOR_EDICT(e: *mut u8) -> c_int;
    fn q_strcasecmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn strcmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn Host_Error(error: *const c_char, ...) -> !;
}

#[inline(always)]
unsafe fn sf() -> &'static SvMainTargetFacts {
    &*(&raw const SvMain_TargetFacts)
}

/// PROTOCOL_HEXENWAIL_1, for sv_main_ents.rs's U_ALPHA test.
#[cfg(feature = "sv_main_ents")]
#[inline(always)]
pub(crate) unsafe fn protocol_hexenwail_1() -> c_int {
    sf().protocol_hexenwail_1
}

#[inline(always)]
unsafe fn protocol() -> c_int {
    *(&raw const sv_protocol)
}

/// `sv.datagram.cursize > MAX_DATAGRAM - room`: the message would not fit.
#[inline(always)]
unsafe fn datagram_full(room: c_int) -> bool {
    (*SvMain_Datagram()).cursize > sf().max_datagram - room
}

/// An edict's vec3 member, as `float[3]`.
#[inline(always)]
unsafe fn vec3(ent: *mut u8, member: usize) -> *const [f32; 3] {
    ent.add(facts().edict_v + member).cast::<[f32; 3]>()
}

/// `entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i])` for
/// each axis, written as coords.
unsafe fn write_sound_origin(msg: *mut SizeBufC, entity: *mut u8) {
    let f = facts();
    let origin = &*vec3(entity, f.v_origin);
    let mins = &*vec3(entity, f.v_mins);
    let maxs = &*vec3(entity, f.v_maxs);
    for i in 0..3 {
        let mid = 0.5 * ((mins[i] + maxs[i]) as f64);
        MSG_WriteCoord(msg, (origin[i] as f64 + mid) as f32);
    }
}

#[inline(always)]
unsafe fn write_coords(msg: *mut SizeBufC, org: *const f32) {
    for i in 0..3 {
        MSG_WriteCoord(msg, *org.add(i));
    }
}

/// SV_StartParticle: svc_particle with the direction packed into chars.
#[no_mangle]
pub unsafe extern "C" fn SV_StartParticle(org: *const f32, dir: *const f32, color: c_int, count: c_int) {
    if datagram_full(16) {
        return;
    }
    let msg = SvMain_Datagram();
    MSG_WriteByte(msg, sf().msg_particle);
    write_coords(msg, org);
    for i in 0..3 {
        let mut v = cvt_c(*dir.add(i) * 16.0);
        if v > 127 {
            v = 127;
        } else if v < -128 {
            v = -128;
        }
        MSG_WriteChar(msg, v);
    }
    MSG_WriteByte(msg, count);
    MSG_WriteByte(msg, color);
}

/// SV_StartParticle2: svc_particle2, a box of velocities as floats.
#[no_mangle]
pub unsafe extern "C" fn SV_StartParticle2(
    org: *const f32,
    dmin: *const f32,
    dmax: *const f32,
    color: c_int,
    effect: c_int,
    count: c_int,
) {
    if datagram_full(36) {
        return;
    }
    let msg = SvMain_Datagram();
    MSG_WriteByte(msg, sf().msg_particle2);
    write_coords(msg, org);
    for i in 0..3 {
        MSG_WriteFloat(msg, *dmin.add(i));
    }
    for i in 0..3 {
        MSG_WriteFloat(msg, *dmax.add(i));
    }
    MSG_WriteShort(msg, color);
    MSG_WriteByte(msg, count);
    MSG_WriteByte(msg, effect);
}

/// SV_StartParticle3: svc_particle3, the box's size in bytes.
#[no_mangle]
pub unsafe extern "C" fn SV_StartParticle3(
    org: *const f32,
    bx: *const f32,
    color: c_int,
    effect: c_int,
    count: c_int,
) {
    if datagram_full(15) {
        return;
    }
    let msg = SvMain_Datagram();
    MSG_WriteByte(msg, sf().msg_particle3);
    write_coords(msg, org);
    for i in 0..3 {
        MSG_WriteByte(msg, cvt_c(*bx.add(i)));
    }
    MSG_WriteShort(msg, color);
    MSG_WriteByte(msg, count);
    MSG_WriteByte(msg, effect);
}

/// SV_StartParticle4: svc_particle4, a radius in a byte.
#[no_mangle]
pub unsafe extern "C" fn SV_StartParticle4(
    org: *const f32,
    radius: f32,
    color: c_int,
    effect: c_int,
    count: c_int,
) {
    if datagram_full(13) {
        return;
    }
    let msg = SvMain_Datagram();
    MSG_WriteByte(msg, sf().msg_particle4);
    write_coords(msg, org);
    MSG_WriteByte(msg, cvt_c(radius));
    MSG_WriteShort(msg, color);
    MSG_WriteByte(msg, count);
    MSG_WriteByte(msg, effect);
}

/// SV_SoundEntityFits: whether this protocol can name `ent` in a sound
/// message, and in `large` whether it needs the whole-entity form, because
/// `(ent<<3)|channel` in one short stops at 8191.  Only protocol 100 has the
/// large forms; on 18..21 the message is dropped (GitHub #411).
#[inline]
unsafe fn sound_entity_fits(ent: c_int, large: &mut bool) -> bool {
    *large = ent >= sf().snd_max_packed_entity;
    !*large || protocol() >= sf().protocol_hexenwail_1
}

/// SV_StopSound.
#[no_mangle]
pub unsafe extern "C" fn SV_StopSound(entity: *mut u8, channel: c_int) {
    if datagram_full(4) {
        return;
    }
    let ent = NUM_FOR_EDICT(entity);
    let mut large = false;
    if !sound_entity_fits(ent, &mut large) {
        return;
    }
    let msg = SvMain_Datagram();
    if large {
        MSG_WriteByte(msg, sf().msg_stopsound_large);
        MSG_WriteShort(msg, ent);
        MSG_WriteByte(msg, channel);
        return;
    }
    MSG_WriteByte(msg, sf().msg_stopsound);
    MSG_WriteShort(msg, (ent << 3) | channel);
}

/// SV_UpdateSoundPos: a playing sound follows its entity's centre.
#[no_mangle]
pub unsafe extern "C" fn SV_UpdateSoundPos(entity: *mut u8, channel: c_int) {
    if datagram_full(4) {
        return;
    }
    let ent = NUM_FOR_EDICT(entity);
    let mut large = false;
    if !sound_entity_fits(ent, &mut large) {
        return;
    }
    let msg = SvMain_Datagram();
    if large {
        MSG_WriteByte(msg, sf().msg_sound_update_pos_large);
        MSG_WriteShort(msg, ent);
        MSG_WriteByte(msg, channel);
    } else {
        MSG_WriteByte(msg, sf().msg_sound_update_pos);
        MSG_WriteShort(msg, (ent << 3) | channel);
    }
    write_sound_origin(msg, entity);
}

/// SV_MaxSounds: sound indices this protocol can express in svc_sound.
#[no_mangle]
pub unsafe extern "C" fn SV_MaxSounds() -> c_int {
    let f = sf();
    if protocol() <= f.protocol_raven_111 {
        return f.max_sounds_old; // bare byte
    }
    if protocol() < f.protocol_uh2_114 {
        return f.max_sounds_h2mp; // byte + SND_OVERFLOW
    }
    f.max_sounds_uh2 // byte + SND_OVERFLOW + SND_OVERFLOW2
}

/// SV_MaxDatagram: the largest unreliable message the client can receive,
/// which is each engine's own compile-time MAX_DATAGRAM.  See protocol.h.
#[no_mangle]
pub unsafe extern "C" fn SV_MaxDatagram() -> c_int {
    let f = sf();
    if protocol() < f.protocol_uh2_114 {
        return f.max_datagram_old; // 18-20: upstream uHexen2
    }
    if protocol() < f.protocol_hexenwail_1 {
        return f.max_datagram_114; // 21: Shanjaq
    }
    f.max_datagram
}

/// SV_MaxReliableMessage: the largest reliable message the client can
/// reassemble without overrunning its socket structure.
#[no_mangle]
pub unsafe extern "C" fn SV_MaxReliableMessage() -> c_int {
    let f = sf();
    if protocol() < f.protocol_uh2_114 {
        return f.max_msglen_old;
    }
    f.max_msglen
}

/// SV_StartSound: eight sound channels per entity; channel 0 auto-allocates
/// and the others override.  An attenuation of 0 plays everywhere.
#[no_mangle]
pub unsafe extern "C" fn SV_StartSound(
    entity: *mut u8,
    channel: c_int,
    sample: *const c_char,
    volume: c_int,
    attenuation: f32,
) {
    let f = sf();

    if q_strcasecmp(sample, c"misc/null.wav".as_ptr()) == 0 {
        SV_StopSound(entity, channel);
        return;
    }

    if !(0..=255).contains(&volume) {
        Host_Error(c"%s: volume = %i".as_ptr(), c"SV_StartSound".as_ptr(), volume);
    }
    if attenuation < 0.0 || attenuation > 4.0 {
        Host_Error(
            c"%s: attenuation = %f".as_ptr(),
            c"SV_StartSound".as_ptr(),
            attenuation as f64,
        );
    }
    if !(0..=7).contains(&channel) {
        Host_Error(c"%s: channel = %i".as_ptr(), c"SV_StartSound".as_ptr(), channel);
    }

    if datagram_full(16) {
        return;
    }

    // find precache number for sound
    let precache = SvMain_SoundPrecache();
    let max_sounds = f.max_sounds;
    let mut sound_num: c_int = 1;
    while sound_num < max_sounds && !(*precache.add(sound_num as usize)).is_null() {
        if strcmp(sample, *precache.add(sound_num as usize)) == 0 {
            break;
        }
        sound_num += 1;
    }
    if sound_num == max_sounds || (*precache.add(sound_num as usize)).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: %s not precached\n".as_ptr(),
            c"SV_StartSound".as_ptr(),
            sample,
        );
        return;
    }

    let ent = NUM_FOR_EDICT(entity);
    let mut large = false;
    if !sound_entity_fits(ent, &mut large) {
        return;
    }

    let mut field_mask = if large { f.snd_largeentity } else { 0 };
    if volume != f.default_sound_packet_volume {
        field_mask |= f.snd_volume;
    }
    if attenuation as f64 != f.default_sound_packet_attenuation {
        field_mask |= f.snd_attenuation;
    }
    if sound_num >= SV_MaxSounds() {
        // Precache is clamped to SV_MaxSounds(), so this only fires if
        // sv_protocol changed under us after the map was precached.
        CON_Printf(
            PRINT_DEVEL,
            c"%s: protocol %i violation: %s sound_num == %i >= %i\n".as_ptr(),
            c"SV_StartSound".as_ptr(),
            protocol(),
            sample,
            sound_num,
            SV_MaxSounds(),
        );
        return;
    }
    // Split the index across the byte and the two overflow mask bits.  Both
    // bits may be set at once on protocol 21 (=> +768).
    if sound_num >= f.max_sounds_h2mp {
        field_mask |= f.snd_overflow2;
        sound_num -= f.max_sounds_h2mp;
    }
    if sound_num >= f.max_sounds_old {
        field_mask |= f.snd_overflow;
        sound_num -= f.max_sounds_old;
    }

    let msg = SvMain_Datagram();
    MSG_WriteByte(msg, f.msg_sound);
    MSG_WriteByte(msg, field_mask);
    if field_mask & f.snd_volume != 0 {
        MSG_WriteByte(msg, volume);
    }
    if field_mask & f.snd_attenuation != 0 {
        MSG_WriteByte(msg, cvt_c(attenuation * 64.0));
    }
    if field_mask & f.snd_largeentity != 0 {
        MSG_WriteShort(msg, ent);
        MSG_WriteByte(msg, channel);
    } else {
        MSG_WriteShort(msg, (ent << 3) | channel);
    }
    MSG_WriteByte(msg, sound_num);
    write_sound_origin(msg, entity);
}
