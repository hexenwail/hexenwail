// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the connect and signon of engine/hexen2/sv_main.c:
// SV_SendServerinfo, SV_ConnectClient, SV_CheckForNewClients, SV_ModelIndex,
// SV_SignonBufferCount, SV_SignonBufferSize, SV_ReserveSignonSpace,
// SV_SendSignonBuffer, SV_CreateBaseline, SV_SendReconnect and the
// logging-only SV_ProtocolName.  #469 slice D, of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// A client connects in three stages.  SV_CheckForNewClients accepts the
// socket and SV_ConnectClient zeroes the client_t around the saved
// netconnection (and spawn_parms, when loading), then SV_SendServerinfo
// writes the first reliable message: version banner, serverinfo, the
// precache lists, model effects (GL builds), ex-inventory icons, music,
// setview and signonnum 1, arming the signon state machine.  Later,
// SV_CreateBaseline appends one 24-byte svc_spawnbaseline record per
// visible edict to sv.signon (via SV_ReserveSignonSpace, which seals full
// buffers and Host_Errors past MAX_SIGNON_BUFFERS), and SV_SendSignonBuffer
// hands those buffers to each client one per reliable message.  The C
// originals moved verbatim into engine/rust/sv_main/tests/sv_main_connect_h2.c.
//
// Hexen II only, like the other sv_main_* slices.  The constants, the
// client/server layouts and the globals come through
// engine/rust/sv_main_target.c; the protocol numbers and per-protocol
// limits through sv_main_sound.rs's SV_MaxDatagram/SV_MaxReliableMessage.
//
// C SEMANTICS KEPT
//
//   * `(int)` of a float -- the baseline scale/abslight bytes -- goes
//     through world_hull.rs's cvt_c, the target's own conversion.
//   * `!svent->v.modelindex` is a float truthiness test, as in C.
//   * The `va("gfx/arti%02d.lmp", id)` comparison and the
//     `sprintf(message, "%c\nVERSION %4.2f SERVER (%i CRC)", ...)` banner
//     are byte-exact via the tiny formatters below (no_std has no
//     format!); the harness fuzzes them against libc.
//   * `Con_DPrintf`/`Con_Printf`/`Sys_Error`/`Host_Error` are the engine's
//     variadic C functions, called as such -- their formatting is C's.
//   * `__thisfunc__`/`__func__` become explicit string literals; the
//     harness normalizes the fixture's `c_`-prefixed rename, as slice C's
//     does.
//   * SV_ConnectClient saves `netconnection` (always) and `spawn_parms`
//     (only when `sv.loadgame`) across the whole-struct memset, then
//     reattaches `msgbuf`/`datagram_buf` with SZ_Init -- in that order.
//   * SV_CheckForNewClients' `while (1)` breaks only on a NULL return from
//     NET_CheckNewConnections; the full-server Sys_Error aborts, as C's
//     does (FUNC_NORETURN).
//   * The model-effects loop honors the target's build: the
//     `!SERVERONLY && GLQUAKE` block runs only when the facts say the
//     target compiled it (sv_main_target.c sets model_effects).
//
// DEPARTURES FROM THE C
//
//   * SV_SendReconnect's `#if !defined(SERVERONLY)` listen-server branch
//     is not ported: `isDedicated`/`src_command` live in the client
//     overlay h2ded never compiles.  This is the SERVERONLY variant, as
//     slice C's dev_stats.packetsize precedent.
//   * SV_ProtocolName returns a nullable `*const c_char`; a NULL passed
//     to `%s` prints `(null)` under glibc, exactly as the C's does.

use core::ffi::{c_char, c_double, c_int, c_uchar, c_uint, c_ushort, c_void};

use crate::msg_io::{
    MSG_WriteAngle, MSG_WriteByte, MSG_WriteChar, MSG_WriteCoord, MSG_WriteFloat,
    MSG_WriteLong, MSG_WriteShort, MSG_WriteString,
};
use crate::sizebuf::{SZ_Init, SZ_Write, SizeBufC};
use crate::sv_main_sound::{SV_MaxDatagram, SV_MaxReliableMessage};
use crate::world_hull::cvt_c;

/// sv_main_target.h's `sv_main_connect_facts_t`.
#[repr(C)]
pub struct SvMainConnectFacts {
    pub protocol_raven_111: c_int,
    pub protocol_raven_112: c_int,
    pub protocol_uqe_113: c_int,
    pub protocol_uh2_114: c_int,
    pub protocol_hexenwail_1: c_int,
    pub msg_print: c_int,
    pub msg_serverinfo: c_int,
    pub msg_cdtrack: c_int,
    pub msg_midi_name: c_int,
    pub msg_mod_name: c_int,
    pub msg_skybox: c_int,
    pub msg_setview: c_int,
    pub msg_signonnum: c_int,
    pub msg_spawnbaseline: c_int,
    pub msg_stufftext: c_int,
    pub game_coop: c_int,
    pub game_deathmatch: c_int,
    pub max_models: c_int,
    pub max_sounds: c_int,
    pub max_qpath: c_int,
    pub max_signon_size: c_int,
    pub max_signon_buffers: c_int,
    pub num_spawn_parms: c_int,
    pub entalpha_default: c_int,
    pub engine_version: c_double,
    pub model_effects: c_int,
    pub qmodel_ex_flags: usize,
    pub qmodel_glow_settings: usize,
    pub client_size: usize,
    pub client_message: usize,
    pub client_msgbuf: usize,
    pub client_msgbuf_size: usize,
    pub client_datagram: usize,
    pub client_datagram_buf: usize,
    pub client_datagram_buf_size: usize,
    pub client_netconnection: usize,
    pub client_spawn_parms: usize,
    pub client_name: usize,
    pub client_name_size: usize,
    pub client_active: usize,
    pub client_spawned: usize,
    pub client_send_all_v: usize,
    pub client_signon_buffer: usize,
    pub client_edict: usize,
    pub client_ex_inventory: usize,
    pub client_sendsignon: usize,
    pub client_state2_size: usize,
    pub edict_v: usize,
    pub edict_baseline: usize,
    pub edict_free: usize,
    pub v_origin: usize,
    pub v_angles: usize,
    pub v_frame: usize,
    pub v_skin: usize,
    pub v_scale: usize,
    pub v_drawflags: usize,
    pub v_abslight: usize,
    pub v_model: usize,
    pub v_modelindex: usize,
    pub es_origin: usize,
    pub es_angles: usize,
    pub es_modelindex: usize,
    pub es_frame: usize,
    pub es_colormap: usize,
    pub es_skin: usize,
    pub es_scale: usize,
    pub es_drawflags: usize,
    pub es_abslight: usize,
    pub es_alpha: usize,
    pub es_clearcount: usize,
    pub ex_item_id: usize,
    pub ex_item_icon: usize,
}

// Layout accessors, for engine/rust/tests/abi_layout.c (the facts table)
// and the differential harness.
macro_rules! accessor {
    ($name:ident, $e:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            $e
        }
    };
}
accessor!(
    SvMainConnectFacts_sizeof,
    core::mem::size_of::<SvMainConnectFacts>()
);
accessor!(
    SvMainConnectFacts_offsetof_msg_spawnbaseline,
    core::mem::offset_of!(SvMainConnectFacts, msg_spawnbaseline)
);
accessor!(
    SvMainConnectFacts_offsetof_client_message,
    core::mem::offset_of!(SvMainConnectFacts, client_message)
);
accessor!(
    SvMainConnectFacts_offsetof_es_clearcount,
    core::mem::offset_of!(SvMainConnectFacts, es_clearcount)
);
accessor!(
    SvMainConnectFacts_offsetof_ex_item_icon,
    core::mem::offset_of!(SvMainConnectFacts, ex_item_icon)
);

/// printsys.h: Con_Printf is _PRINT_NORMAL, Con_DPrintf _PRINT_DEVEL.
const PRINT_NORMAL: c_uint = 0;
const PRINT_DEVEL: c_uint = 2;

extern "C" {
    static SvMainConnect_TargetFacts: SvMainConnectFacts;
    fn SvMain_MaxClients() -> c_int;
    fn SvMain_Coop() -> c_int;
    fn SvMain_Deathmatch() -> c_int;
    fn SvMain_KingOfHill() -> c_int;
    fn SvMain_LoadGame() -> c_int;
    fn SvMain_PrecacheCRC() -> c_ushort;
    fn SvMain_Signon() -> *mut SizeBufC;
    fn SvMain_SignonBufs() -> *mut u8;
    fn SvMain_NumSignonBuffers() -> c_int;
    fn SvMain_SetNumSignonBuffers(v: c_int);
    fn SvMain_SignonSize(i: c_int) -> c_int;
    fn SvMain_SetSignonSize(i: c_int, v: c_int);
    fn SvMain_ModelPrecache() -> *const *const c_char;
    fn SvMain_SoundPrecache() -> *const *const c_char;
    fn SvMain_NumExItems() -> c_int;
    fn SvMain_ExItems() -> *mut u8;
    fn SvMain_CdTrack() -> c_uchar;
    fn SvMain_MidiName() -> *const c_char;
    fn SvMain_ModName() -> *const c_char;
    fn SvMain_Skybox() -> *const c_char;
    fn SvMain_NetActiveConnections() -> *mut c_int;
    fn SvMain_Models() -> *mut *mut u8;
    fn SvMain_Protocol() -> c_int;
    fn SvMain_Clients() -> *mut u8;
    fn SvMain_States() -> *mut u8;
    fn SvMain_Edicts() -> *mut u8;
    fn SvMain_NumEdicts() -> c_int;
    fn SvMain_EdictSize() -> c_int;

    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn Sys_Error(error: *const c_char, ...) -> !;
    fn Host_Error(error: *const c_char, ...) -> !;
    fn NET_IsLocalConnection(sock: *mut u8) -> c_int;
    fn NET_MaxUnreliableMessage(sock: *mut u8) -> c_int;
    fn NET_CheckNewConnections() -> *mut u8;
    fn NET_QSocketGetAddressString(sock: *const u8) -> *const c_char;
    fn NET_SendToAll(data: *mut SizeBufC, blocktime: c_double);
    fn SV_ClientInventoryPage(clientnum: c_int) -> *mut u8;
    fn SV_GetLevelname() -> *const c_char;
    fn GetEdictFieldValue(ed: *mut u8, field: *const c_char) -> *mut u8;
    fn PR_GetString(num: c_int) -> *const c_char;
    fn strcmp(s1: *const c_char, s2: *const c_char) -> c_int;
}

#[inline(always)]
unsafe fn facts() -> &'static SvMainConnectFacts {
    &*(&raw const SvMainConnect_TargetFacts)
}

/// A client_t int (qboolean) member.
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

/// A float entvars_t member, by offset from `v`.
#[inline(always)]
unsafe fn vf(ent: *mut u8, member: usize) -> f32 {
    *ent.add(facts().edict_v + member).cast::<f32>()
}

/// An int entvars_t member (model, a string_t).
#[inline(always)]
unsafe fn vi(ent: *mut u8, member: usize) -> c_int {
    *ent.add(facts().edict_v + member).cast::<c_int>()
}

/// edict_t's `free` qboolean at an absolute offset from the edict start
/// (it is NOT in entvars_t).
#[inline(always)]
unsafe fn vb(ent: *mut u8, off: usize) -> c_int {
    *(ent.add(off) as *mut c_int)
}

/// Copy a byte string into `buf`; returns bytes written.
fn write_str(buf: &mut [u8], s: &[u8]) -> usize {
    buf[..s.len()].copy_from_slice(s);
    s.len()
}

/// `%4.2f`: fixed-point, 2 decimals, minimum field width 4, space-padded
/// left.  Byte-exact with glibc's sprintf: the rounding is done on the
/// exact binary value via integer arithmetic on the mantissa and
/// exponent (a floating-point `v * 100.0` would not be exact, and
/// round-half-to-even needs the true halfway cases).  `buf` must hold at
/// least 32 bytes.  Returns bytes written.  Values whose integer part
/// exceeds u64::MAX saturate; production input is ENGINE_VERSION (1.29).
fn fmt_4_2f(buf: &mut [u8], v: f64) -> usize {
    // glibc's shapes for the non-finite values, width-padded like the rest.
    if v.is_nan() {
        return write_padded(buf, b"nan", 4);
    }
    if v.is_infinite() {
        if v > 0.0 {
            return write_padded(buf, b"inf", 4);
        }
        return write_padded(buf, b"-inf", 4);
    }
    let neg = v.is_sign_negative();
    let a = v.abs();
    // Exact round(a * 100), half to even: decompose a = m * 2^e with a
    // 53-bit integer mantissa, then scale by 100 in integers.
    let bits = a.to_bits();
    let exp = ((bits >> 52) & 0x7ff) as i32;
    let (m, e): (u64, i32) = if exp == 0 {
        // Subnormal: no implicit leading 1.
        ((bits & 0xfffffffffffff) as u64, -1074)
    } else {
        (((bits & 0xfffffffffffff) | 0x10000000000000) as u64, exp - 1075)
    };
    // m * 100 < 2^60: fits in u64.
    let scaled_m = m * 100;
    let n: u64 = if e >= 0 {
        let e = e as u32;
        if e >= 64 || scaled_m > u64::MAX >> e {
            u64::MAX
        } else {
            scaled_m << e
        }
    } else {
        let k = (-e) as u32;
        if k >= 61 {
            // scaled_m < 2^60 <= 2^(k-1): below half, rounds to 0.
            0
        } else {
            let q = scaled_m >> k;
            let r = scaled_m & ((1u64 << k) - 1);
            let half = 1u64 << (k - 1);
            if r > half || (r == half && q & 1 == 1) {
                q + 1
            } else {
                q
            }
        }
    };
    let ip = n / 100;
    let fp = (n % 100) as u8;
    // Digits in forward order: [sign] int "." frac.
    let mut tmp = [0u8; 32];
    let mut len = 0;
    if neg {
        tmp[len] = b'-';
        len += 1;
    }
    let mut digits = [0u8; 20];
    let mut nd = 0;
    let mut x = ip;
    if x == 0 {
        digits[nd] = b'0';
        nd += 1;
    }
    while x > 0 {
        digits[nd] = b'0' + (x % 10) as u8;
        nd += 1;
        x /= 10;
    }
    while nd > 0 {
        nd -= 1;
        tmp[len] = digits[nd];
        len += 1;
    }
    tmp[len] = b'.';
    len += 1;
    tmp[len] = b'0' + fp / 10;
    len += 1;
    tmp[len] = b'0' + fp % 10;
    len += 1;
    let width = len.max(4);
    let mut out = 0;
    for _ in 0..(width - len) {
        buf[out] = b' ';
        out += 1;
    }
    buf[out..out + len].copy_from_slice(&tmp[..len]);
    out + len
}

/// Left-pad `s` with spaces to `width`; returns bytes written.
fn write_padded(buf: &mut [u8], s: &[u8], width: usize) -> usize {
    let mut out = 0;
    for _ in s.len()..width {
        buf[out] = b' ';
        out += 1;
    }
    buf[out..out + s.len()].copy_from_slice(s);
    out + s.len()
}

/// `%i` / `%d`: decimal int, no padding.  `buf` must hold at least 12
/// bytes.  Returns bytes written.
fn fmt_int(buf: &mut [u8], v: c_int) -> usize {
    let neg = v < 0;
    // i64 so abs(INT_MIN) does not overflow.
    let mut x = if neg { -(v as i64) } else { v as i64 } as u64;
    let mut digits = [0u8; 20];
    let mut nd = 0;
    if x == 0 {
        digits[nd] = b'0';
        nd += 1;
    }
    while x > 0 {
        digits[nd] = b'0' + (x % 10) as u8;
        nd += 1;
        x /= 10;
    }
    let mut out = 0;
    if neg {
        buf[out] = b'-';
        out += 1;
    }
    while nd > 0 {
        nd -= 1;
        buf[out] = digits[nd];
        out += 1;
    }
    out
}

/// `%02d`: decimal int, minimum width 2, zero-padded; the sign counts
/// toward the width (`5` -> `"05"`, `-5` -> `"-5"`, `123` -> `"123"`).
/// `buf` must hold at least 12 bytes.  Returns bytes written.
fn fmt_02d(buf: &mut [u8], v: c_int) -> usize {
    let neg = v < 0;
    let mut x = if neg { -(v as i64) } else { v as i64 } as u64;
    let mut digits = [0u8; 20];
    let mut nd = 0;
    if x == 0 {
        digits[nd] = b'0';
        nd += 1;
    }
    while x > 0 {
        digits[nd] = b'0' + (x % 10) as u8;
        nd += 1;
        x /= 10;
    }
    let width = (nd + if neg { 1 } else { 0 }).max(2);
    let mut out = 0;
    if neg {
        buf[out] = b'-';
        out += 1;
    }
    for _ in (nd + if neg { 1 } else { 0 })..width {
        buf[out] = b'0';
        out += 1;
    }
    while nd > 0 {
        nd -= 1;
        buf[out] = digits[nd];
        out += 1;
    }
    out
}

// Test hooks for the differential harness: the formatters above, callable
// from C.  Each writes into the caller's buffer (32 bytes for %4.2f, 12
// for the ints) and returns the bytes written, NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn SvMainConnect_TestFmt42(buf: *mut u8, v: c_double) -> usize {
    let n = fmt_4_2f(core::slice::from_raw_parts_mut(buf, 32), v);
    *buf.add(n) = 0;
    n
}
#[no_mangle]
pub unsafe extern "C" fn SvMainConnect_TestFmtInt(buf: *mut u8, v: c_int) -> usize {
    let n = fmt_int(core::slice::from_raw_parts_mut(buf, 12), v);
    *buf.add(n) = 0;
    n
}
#[no_mangle]
pub unsafe extern "C" fn SvMainConnect_TestFmt02d(buf: *mut u8, v: c_int) -> usize {
    let n = fmt_02d(core::slice::from_raw_parts_mut(buf, 12), v);
    *buf.add(n) = 0;
    n
}

/// SV_ProtocolName: the four protocols this engine speaks, or NULL for
/// anything else.  #[no_mangle]: the still-C SV_Protocol_f console
/// command and the -protocol startup check call it.  A NULL return
/// passed to `%s` prints `(null)` under glibc, as the C's does.
#[no_mangle]
pub unsafe extern "C" fn SV_ProtocolName(protocol: c_int) -> *const c_char {
    let f = facts();
    if protocol == f.protocol_raven_111 {
        c"Raven/H2/1.11".as_ptr()
    } else if protocol == f.protocol_raven_112 {
        c"Raven/MP/1.12".as_ptr()
    } else if protocol == f.protocol_uqe_113 {
        c"UQE/1.13".as_ptr()
    } else if protocol == f.protocol_uh2_114 {
        c"Raven/MP/1.14".as_ptr()
    } else if protocol == f.protocol_hexenwail_1 {
        c"Hexenwail/1".as_ptr()
    } else {
        core::ptr::null()
    }
}

/// SV_SignonBufferCount: the sealed buffers plus the one being filled.
#[inline(always)]
unsafe fn signon_buffer_count() -> c_int {
    SvMain_NumSignonBuffers() + 1
}

/// SV_SignonBufferSize: sealed buffers read sv.signon_size[i]; the filling
/// buffer's length is sv.signon.cursize.
#[inline(always)]
unsafe fn signon_buffer_size(i: c_int) -> c_int {
    if i < SvMain_NumSignonBuffers() {
        SvMain_SignonSize(i)
    } else {
        (*SvMain_Signon()).cursize
    }
}

/// SV_SendServerinfo: the first reliable message to a connected client.
/// #[no_mangle] because the still-C SV_SpawnServer (slice E) calls it.
#[no_mangle]
pub unsafe extern "C" fn SV_SendServerinfo(client: *mut u8) {
    let f = facts();
    let msg = clmsg(client, f.client_message);

    MSG_WriteByte(msg, f.msg_print);
    // sprintf(message, "%c\nVERSION %4.2f SERVER (%i CRC)", 2,
    //         ENGINE_VERSION, pr_crc)
    let mut message = [0u8; 2048];
    let mut n = 0;
    message[n] = 2;
    n += 1;
    n += write_str(&mut message[n..], b"\nVERSION ");
    n += fmt_4_2f(&mut message[n..], f.engine_version);
    n += write_str(&mut message[n..], b" SERVER (");
    n += fmt_int(&mut message[n..], SvMain_PrecacheCRC() as c_int);
    n += write_str(&mut message[n..], b" CRC)");
    message[n] = 0;
    MSG_WriteString(msg, message.as_ptr().cast::<c_char>());

    // Con_DPrintf ("%s: %s, protocol %i (%s), datagram %i, reliable %i\n", ...)
    // The C evaluates NET_IsLocalConnection three times; the harness's
    // call log records every stub call, so this does too.
    let netconnection = clp(client, f.client_netconnection);
    let proto = SvMain_Protocol();
    CON_Printf(
        PRINT_DEVEL,
        c"%s: %s, protocol %i (%s), datagram %i, reliable %i\n".as_ptr(),
        c"SV_SendServerinfo".as_ptr(),
        if NET_IsLocalConnection(netconnection) != 0 {
            c"local".as_ptr()
        } else {
            c"remote".as_ptr()
        },
        proto,
        SV_ProtocolName(proto),
        if NET_IsLocalConnection(netconnection) != 0 {
            NET_MaxUnreliableMessage(netconnection)
        } else {
            SV_MaxDatagram()
        },
        if NET_IsLocalConnection(netconnection) != 0 {
            f.client_msgbuf_size as c_int
        } else {
            SV_MaxReliableMessage()
        },
    );

    MSG_WriteByte(msg, f.msg_serverinfo);
    MSG_WriteLong(msg, proto);
    MSG_WriteByte(msg, SvMain_MaxClients());

    if SvMain_Coop() == 0 && SvMain_Deathmatch() != 0 {
        MSG_WriteByte(msg, f.game_deathmatch);
        if proto > f.protocol_raven_111 {
            MSG_WriteShort(msg, SvMain_KingOfHill());
        }
    } else {
        MSG_WriteByte(msg, f.game_coop);
    }

    // send full levelname
    MSG_WriteString(msg, SV_GetLevelname());

    // The precache walks rely on the NULL terminator; a completely full
    // array stops at MAX_MODELS/MAX_SOUNDS, as the C's `i <` guard does.
    let model_precache = SvMain_ModelPrecache();
    let mut i = 1;
    while i < f.max_models {
        let s = *model_precache.add(i as usize);
        if s.is_null() {
            break;
        }
        MSG_WriteString(msg, s);
        i += 1;
    }
    MSG_WriteByte(msg, 0);

    let sound_precache = SvMain_SoundPrecache();
    i = 1;
    while i < f.max_sounds {
        let s = *sound_precache.add(i as usize);
        if s.is_null() {
            break;
        }
        MSG_WriteString(msg, s);
        i += 1;
    }
    MSG_WriteByte(msg, 0);

    if proto >= f.protocol_uh2_114 {
        // send model effects.  The inner block is the target's
        // !SERVERONLY && GLQUAKE code, via the model_effects fact.
        let models = SvMain_Models();
        i = 1;
        while i < f.max_models {
            let s = *model_precache.add(i as usize);
            if s.is_null() {
                break;
            }
            if f.model_effects != 0 {
                let model = *models.add(i as usize);
                if !model.is_null()
                    && *(model.add(f.qmodel_ex_flags) as *mut c_int) != 0
                {
                    MSG_WriteString(msg, s);
                    MSG_WriteShort(msg, *(model.add(f.qmodel_ex_flags) as *mut c_int));
                    let glow = model.add(f.qmodel_glow_settings).cast::<f32>();
                    MSG_WriteFloat(msg, *glow.add(0));
                    MSG_WriteFloat(msg, *glow.add(1));
                    MSG_WriteFloat(msg, *glow.add(2));
                    MSG_WriteFloat(msg, *glow.add(3));
                }
            }
            i += 1;
        }
        MSG_WriteByte(msg, 0);

        // send inventory extension info: only new/changed artifacts
        let num_ex_items = SvMain_NumExItems();
        let ex_items = SvMain_ExItems();
        let mut tmp = [0u8; 64];
        i = 0;
        while i < num_ex_items {
            let item = ex_items.add(i as usize * ex_item_size());
            let id = *(item.add(f.ex_item_id) as *mut c_int);
            let icon = item.add(f.ex_item_icon).cast::<c_char>();
            // va("gfx/arti%02d.lmp", id)
            let mut n = write_str(&mut tmp, b"gfx/arti");
            n += fmt_02d(&mut tmp[n..], id);
            n += write_str(&mut tmp[n..], b".lmp");
            tmp[n] = 0;
            if strcmp(tmp.as_ptr().cast::<c_char>(), icon) == 0 || id > 15 {
                MSG_WriteByte(msg, id);
                MSG_WriteString(msg, icon);
            }
            i += 1;
        }
        MSG_WriteByte(msg, 0);
    }

    // send music
    MSG_WriteByte(msg, f.msg_cdtrack);
    MSG_WriteByte(msg, SvMain_CdTrack() as c_int);
    MSG_WriteByte(msg, SvMain_CdTrack() as c_int);

    MSG_WriteByte(msg, f.msg_midi_name);
    MSG_WriteString(msg, SvMain_MidiName());

    if proto >= f.protocol_uqe_113 {
        MSG_WriteByte(msg, f.msg_mod_name);
        MSG_WriteString(msg, SvMain_ModName());
        MSG_WriteByte(msg, f.msg_skybox);
        MSG_WriteString(msg, SvMain_Skybox());
    }

    // set view
    MSG_WriteByte(msg, f.msg_setview);
    let edict = clp(client, f.client_edict);
    let edicts = SvMain_Edicts();
    let edict_size = SvMain_EdictSize() as usize;
    MSG_WriteShort(msg, ((edict as usize - edicts as usize) / edict_size) as c_int);

    MSG_WriteByte(msg, f.msg_signonnum);
    MSG_WriteByte(msg, 1);

    *clip(client, f.client_sendsignon) = 1;
    *clip(client, f.client_spawned) = 0;
    // Void anything the previous map's signon list still owed this client
    // (uhexen2-z5wt): prespawn restarts the list from 0.
    *clip(client, f.client_signon_buffer) = -1;
}

/// sizeof(ex_item_t): the icon (char[MAX_QPATH]) is the last field.
#[inline(always)]
unsafe fn ex_item_size() -> usize {
    let f = facts();
    f.ex_item_icon + f.max_qpath as usize
}

/// SV_ConnectClient: initialize a client_t for a new net connection.
/// Private: only SV_CheckForNewClients calls it.
unsafe fn sv_connect_client(clientnum: c_int) {
    let f = facts();
    let clients = SvMain_Clients();
    let client = clients.add(clientnum as usize * f.client_size);

    CON_Printf(
        PRINT_DEVEL,
        c"Client %s connected\n".as_ptr(),
        NET_QSocketGetAddressString(clp(client, f.client_netconnection) as *const u8),
    );

    let edictnum = clientnum + 1;
    let ent = SvMain_Edicts().add(edictnum as usize * SvMain_EdictSize() as usize);

    // set up the client_t: netconnection always survives the memset,
    // spawn_parms only when loading a game.
    let netconnection = clp(client, f.client_netconnection);
    let mut spawn_parms = [0f32; 16];
    let loadgame = SvMain_LoadGame() != 0;
    if loadgame {
        let src = client.add(f.client_spawn_parms).cast::<f32>();
        let mut k = 0;
        while k < f.num_spawn_parms as usize {
            spawn_parms[k] = *src.add(k);
            k += 1;
        }
    }
    core::ptr::write_bytes(client, 0, f.client_size);
    *clip(client, f.client_send_all_v) = 1;
    *(client.add(f.client_netconnection) as *mut *mut u8) = netconnection;

    // strcpy (client->name, "unconnected")
    let name = client.add(f.client_name).cast::<c_char>();
    let src = c"unconnected".as_ptr();
    let mut k = 0;
    loop {
        let b = *src.add(k);
        *name.add(k) = b;
        if b == 0 {
            break;
        }
        k += 1;
    }
    *clip(client, f.client_active) = 1;
    *clip(client, f.client_spawned) = 0;
    *clip(client, f.client_signon_buffer) = -1;
    *(client.add(f.client_edict) as *mut *mut u8) = ent;

    *(client.add(f.client_ex_inventory) as *mut *mut u8) =
        SV_ClientInventoryPage(clientnum);

    let msg = clmsg(client, f.client_message);
    SZ_Init(
        msg,
        client.add(f.client_msgbuf),
        f.client_msgbuf_size as c_int,
    );
    (*msg).allowoverflow = 1; // we can catch it
    SZ_Init(
        clmsg(client, f.client_datagram),
        client.add(f.client_datagram_buf),
        f.client_datagram_buf_size as c_int,
    );

    // memset(&sv.states[clientnum], 0, sizeof(client_state2_t))
    let states = SvMain_States() as *mut u8;
    core::ptr::write_bytes(
        states.add(clientnum as usize * f.client_state2_size),
        0,
        f.client_state2_size,
    );

    if loadgame {
        let dst = client.add(f.client_spawn_parms).cast::<f32>();
        let mut k = 0;
        while k < f.num_spawn_parms as usize {
            *dst.add(k) = spawn_parms[k];
            k += 1;
        }
    }
    // The commented-out PR_ExecuteProgram block is dead code; not ported.

    SV_SendServerinfo(client);
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

/// ENTALPHA_ENCODE, as sv_main_ents.rs's `entalpha_encode` (slice B):
/// 0 stays the default; anything else maps 0..1 onto 1..255, through
/// Q_rint.
fn entalpha_encode(default: c_int, a: f32) -> u8 {
    if a == 0.0 {
        return default as u8;
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

/// SV_CheckForNewClients: accept pending sockets into free client slots.
/// #[no_mangle]: called from host.c and HexenWorld's host.c.
#[no_mangle]
pub unsafe extern "C" fn SV_CheckForNewClients() {
    let f = facts();
    let clients = SvMain_Clients();
    let maxclients = SvMain_MaxClients();
    loop {
        let ret = NET_CheckNewConnections();
        if ret.is_null() {
            break;
        }
        let mut i = 0;
        while i < maxclients {
            if cli(clients.add(i as usize * f.client_size), f.client_active) == 0 {
                break;
            }
            i += 1;
        }
        if i == maxclients {
            Sys_Error(
                c"%s: no free clients".as_ptr(),
                c"SV_CheckForNewClients".as_ptr(),
            );
        }
        let client = clients.add(i as usize * f.client_size);
        *(client.add(f.client_netconnection) as *mut *mut u8) = ret;
        sv_connect_client(i);
        *SvMain_NetActiveConnections() += 1;
    }
}

/// SV_ModelIndex: the precache index of a model name, 0 for NULL, empty,
/// or not precached (with a console line naming the miss).
/// #[no_mangle]: called from host_cmd.c, pr_cmds.c, HexenWorld's
/// host_cmd.c, and (once merged) slice C's Rust module.
#[no_mangle]
pub unsafe extern "C" fn SV_ModelIndex(name: *const c_char) -> c_int {
    let f = facts();
    if name.is_null() || *name == 0 {
        return 0;
    }
    let precache = SvMain_ModelPrecache();
    let mut i = 0;
    while i < f.max_models {
        let s = *precache.add(i as usize);
        if s.is_null() {
            break;
        }
        if strcmp(s, name) == 0 {
            return i;
        }
        i += 1;
    }
    // The `||` short-circuits: at i == max_models the array is full and
    // precache[i] is never read, as in C.
    if i == f.max_models || (*precache.add(i as usize)).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: model %s not precached\n".as_ptr(),
            c"SV_ModelIndex".as_ptr(),
            name,
        );
        return 0;
    }
    i
}

/// SV_ReserveSignonSpace: make room for a signon record of `length` bytes
/// in one piece, sealing the current buffer and starting a new one if it
/// does not fit.  #[no_mangle]: also called from pr_cmds.c (QC MSG_INIT).
#[no_mangle]
pub unsafe extern "C" fn SV_ReserveSignonSpace(length: c_int) {
    let f = facts();
    if length > f.max_signon_size {
        Sys_Error(
            c"%s: %d byte record does not fit a %d byte signon buffer".as_ptr(),
            c"SV_ReserveSignonSpace".as_ptr(),
            length,
            f.max_signon_size,
        );
    }
    let signon = SvMain_Signon();
    if (*signon).cursize + length <= (*signon).maxsize {
        return;
    }
    if SvMain_NumSignonBuffers() + 1 >= f.max_signon_buffers {
        Host_Error(
            c"Too much signon data for this map: %d buffers of %d bytes are full.  Raise MAX_SIGNON_BUFFERS (server.h) or cut static entities / ambient sounds.".as_ptr(),
            f.max_signon_buffers,
            f.max_signon_size,
        );
    }
    SvMain_SetSignonSize(SvMain_NumSignonBuffers(), (*signon).cursize);
    SvMain_SetNumSignonBuffers(SvMain_NumSignonBuffers() + 1);
    SZ_Init(
        signon,
        SvMain_SignonBufs()
            .add(SvMain_NumSignonBuffers() as usize * f.max_signon_size as usize),
        f.max_signon_size,
    );
    (*signon).name = c"sv.signon".as_ptr();
}

/// SV_SendSignonBuffer: hand a client the next signon buffer it is owed,
/// or the signon-stage marker once the last one has gone.
/// #[no_mangle]: called from SV_SendClientMessages (slice C) and
/// Host_PreSpawn_f.
#[no_mangle]
pub unsafe extern "C" fn SV_SendSignonBuffer(client: *mut u8) {
    let f = facts();
    let msg = clmsg(client, f.client_message);
    let i = cli(client, f.client_signon_buffer);
    if i < 0 || i >= signon_buffer_count() {
        *clip(client, f.client_signon_buffer) = -1;
        return;
    }
    SZ_Write(
        msg,
        SvMain_SignonBufs().add(i as usize * f.max_signon_size as usize).cast::<c_void>(),
        signon_buffer_size(i),
    );
    if i + 1 < signon_buffer_count() {
        *clip(client, f.client_signon_buffer) = i + 1;
    } else {
        *clip(client, f.client_signon_buffer) = -1;
        MSG_WriteByte(msg, f.msg_signonnum);
        MSG_WriteByte(msg, 2);
    }
    *clip(client, f.client_sendsignon) = 1;
}

/// SV_CreateBaseline: per-entity baselines for every non-free edict with
/// a model, appended to sv.signon as svc_spawnbaseline records.
/// #[no_mangle]: the still-C SV_SpawnServer (slice E) calls it.
#[no_mangle]
pub unsafe extern "C" fn SV_CreateBaseline() {
    let f = facts();
    let signon = SvMain_Signon();
    let edicts = SvMain_Edicts();
    let edict_size = SvMain_EdictSize() as usize;
    let num_edicts = SvMain_NumEdicts();
    let maxclients = SvMain_MaxClients();
    let mut entnum = 0;
    while entnum < num_edicts {
        let svent = edicts.add(entnum as usize * edict_size);
        if vb(svent, f.edict_free) != 0 {
            entnum += 1;
            continue;
        }
        if entnum > maxclients && vf(svent, f.v_modelindex) == 0.0 {
            entnum += 1;
            continue;
        }

        // create entity baseline
        let baseline = svent.add(f.edict_baseline);
        // VectorCopy origin/angles
        let mut k = 0;
        while k < 3 {
            *baseline
                .add(f.es_origin)
                .cast::<f32>()
                .add(k) = *svent
                .add(f.edict_v + f.v_origin)
                .cast::<f32>()
                .add(k);
            *baseline
                .add(f.es_angles)
                .cast::<f32>()
                .add(k) = *svent
                .add(f.edict_v + f.v_angles)
                .cast::<f32>()
                .add(k);
            k += 1;
        }
        // float -> byte, as C's implicit conversion (cvt_c then low byte)
        *(baseline.add(f.es_frame) as *mut u8) = cvt_c(vf(svent, f.v_frame)) as u8;
        *(baseline.add(f.es_skin) as *mut u8) = cvt_c(vf(svent, f.v_skin)) as u8;
        // (int)(scale*100.0)&255: the multiply is double, as C's usual
        // arithmetic conversions do it
        *(baseline.add(f.es_scale) as *mut u8) =
            (cvt_c_f64(vf(svent, f.v_scale) as f64 * 100.0) & 255) as u8;
        *(baseline.add(f.es_drawflags) as *mut u8) = cvt_c(vf(svent, f.v_drawflags)) as u8;
        *(baseline.add(f.es_abslight) as *mut u8) =
            (cvt_c_f64(vf(svent, f.v_abslight) as f64 * 255.0) & 255) as u8;
        let val = GetEdictFieldValue(svent, c"alpha".as_ptr());
        *(baseline.add(f.es_alpha) as *mut u8) = if val.is_null() {
            f.entalpha_default as u8
        } else {
            entalpha_encode(f.entalpha_default, *(val as *mut f32))
        };
        if entnum > 0 && entnum <= maxclients {
            *(baseline.add(f.es_colormap) as *mut u8) = entnum as u8;
            *(baseline.add(f.es_modelindex) as *mut u16) = 0;
        } else {
            *(baseline.add(f.es_colormap) as *mut u8) = 0;
            *(baseline.add(f.es_modelindex) as *mut u16) =
                SV_ModelIndex(PR_GetString(vi(svent, f.v_model))) as u16;
        }
        core::ptr::write_bytes(baseline.add(f.es_clearcount), 99, 32);

        // add to the message: 24 bytes reserved (see the C's comment)
        SV_ReserveSignonSpace(24);
        MSG_WriteByte(signon, f.msg_spawnbaseline);
        MSG_WriteShort(signon, entnum);
        MSG_WriteShort(signon, *(baseline.add(f.es_modelindex) as *mut u16) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_frame) as *mut u8) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_colormap) as *mut u8) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_skin) as *mut u8) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_scale) as *mut u8) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_drawflags) as *mut u8) as c_int);
        MSG_WriteByte(signon, *(baseline.add(f.es_abslight) as *mut u8) as c_int);
        let mut k = 0;
        while k < 3 {
            MSG_WriteCoord(
                signon,
                *baseline.add(f.es_origin).cast::<f32>().add(k),
            );
            MSG_WriteAngle(
                signon,
                *baseline.add(f.es_angles).cast::<f32>().add(k),
            );
            k += 1;
        }
        entnum += 1;
    }
}

/// SV_SendReconnect: tell all clients the server is changing levels.
/// #[no_mangle]: the still-C SV_SpawnServer (slice E) calls it.
/// The `#if !defined(SERVERONLY)` listen-server branch is not ported
/// (see the header): this is the SERVERONLY variant.
#[no_mangle]
pub unsafe extern "C" fn SV_SendReconnect() {
    let f = facts();
    let mut data = [0u8; 128];
    let mut msg = SizeBufC {
        allowoverflow: 0,
        overflowed: 0,
        data: data.as_mut_ptr(),
        maxsize: data.len() as c_int,
        cursize: 0,
        name: core::ptr::null(),
    };
    MSG_WriteChar(&mut msg as *mut SizeBufC, f.msg_stufftext);
    MSG_WriteString(&mut msg as *mut SizeBufC, c"reconnect\n".as_ptr());
    NET_SendToAll(&mut msg as *mut SizeBufC, 5.0);
}
