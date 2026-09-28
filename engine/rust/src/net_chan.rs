// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/hexenworld/shared/net_chan.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// HexenWorld's channel layer: the packet header, the reliable queue and its
// resend rule, the bandwidth choke, and the receive path that hands the
// payload on through net_message.  Four properties of the C original shape
// this port.
//
// 1. **`netchan_t` is caller-owned.**  It is embedded by its callers --
//    `static netchan_t hwcl_netchan` (hexen2/cl_hw.c) and a member of the
//    server's client struct (hexenworld/server/server.h) -- with 7500-byte
//    buffers inline, so the port never allocates, moves or adopts one.  The
//    layout is asserted field by field below, derived from the pointer width
//    because the same archive is built for wasm32 (where this module compiles
//    to nothing anyway, see point 3).
//
// 2. **The names are per target.**  net.h renames every symbol here under
//    H2W_INTEGRATED, so the client's C calls `HWNetchan_Transmit` and reads
//    `hw_net_drop` while hwsv's calls `Netchan_Transmit` and reads
//    `net_drop`.  As with the transport beside it, this module exports the
//    `HWNetchan_*` spelling -- unambiguous everywhere -- and the per-target
//    shim engine/rust/net_chan_target.c owns what the C sees under each
//    target's name: the `net_drop` storage, the hwsv forwards, and the one
//    entry point Rust cannot define (point 4).
//
// 3. **The browser build does not contain it.**  net_chan.c is compiled into
//    glhexen2 and hwsv only: no HexenWorld transport, no channel.  The `imp`
//    module is gated the same way, so on wasm32 this file compiles to nothing
//    rather than to stubs that would pretend to sequence packets.
//
// 4. **`Netchan_OutOfBandPrint` is C-variadic**, and stable Rust cannot define
//    a C-variadic function.  Like quakefs's two path builders, it therefore
//    stays in the per-target shim: the shim keeps the signature, formats into
//    a buffer, and calls the non-variadic send path this module owns.  CALLING
//    a C variadic from Rust is fine; defining one is what is not.
//
// The wire format is the contract: a message header of {sequence | reliable
// bit, acknowledge | reliable-ack bit}, the reliable part always first, the
// unreliable part only if it fits, and a reliable message resent when the peer
// acknowledges a later sequence without the matching reliable bit.  The #if 0
// block in the C (a rate-estimation experiment) is dead and is not ported.

const _: () = ();

#[cfg(any(unix, windows))]
mod imp {
    use core::ffi::{c_char, c_int, c_uint, c_void};

    use crate::cvar::CvarC;
    use crate::sizebuf::SizeBufC;

    /// `HWNET_MAX_MSGLEN` from net.h.
    const HWNET_MAX_MSGLEN: usize = 7500;
    /// `PACKET_HEADER`, the two 32-bit words the channel writes.
    const PACKET_HEADER: usize = 8;
    /// `MAX_LATENT` from net.h: the window of in-flight sequence numbers.
    const MAX_LATENT: usize = 32;
    /// `MAX_BACKUP` from the C's Netchan_CanPacket.
    const MAX_BACKUP: f64 = 200.0;
    /// `OLD_AVG` from net.h: the rolling-average weight.
    const OLD_AVG: f64 = 0.99;

    extern "C" {
        fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
        fn Cvar_RegisterVariable(var: *mut CvarC);
        fn SZ_Init(buf: *mut SizeBufC, data: *mut u8, length: c_int);
        fn SZ_Write(buf: *mut SizeBufC, data: *const c_void, length: c_int);
        fn MSG_WriteLong(buf: *mut SizeBufC, value: c_int);
        fn MSG_ReadLong(buf: *mut SizeBufC) -> c_int;
        fn MSG_BeginReadingFrom(buf: *mut SizeBufC);

        /// The exported transport entry points.  This module names the
        /// `HWNET_*` spelling rather than the target's: net.h already maps the
        /// client's calls onto it, hwsv's plain `NET_*` calls are forwarded to
        /// it by net_udp_hw_target.c, and h2ded -- which links this archive
        /// without building a HexenWorld transport -- gets the definition from
        /// the archive's own transport module.
        fn HWNET_SendPacket(length: c_int, data: *mut c_void, to: *const NetAdrC);
        fn HWNET_CompareAdr(a: *const NetAdrC, b: *const NetAdrC) -> c_int;
        fn HWNET_AdrToString(a: *const NetAdrC) -> *const c_char;

        /// The per-target facts, from engine/rust/net_chan_target.c.
        fn NetChan_TargetNotDemoPlayback() -> c_int;
        fn NetChan_TargetDrop() -> *mut c_int;

        /// The transport's storage accessors, from net_udp_hw_target.c: the
        /// channel reads `net_message` and `net_from` through these rather than
        /// naming either target's spelling.
        fn NetUDP_TargetMessageBuf() -> *mut SizeBufC;
        fn NetUDP_TargetFrom() -> *mut NetAdrC;

        /// engine/hexen2/host.h, hexen2/server/host.c and
        /// hexenworld/server/sv_main.c: one definition per target.
        static mut realtime: f64;
    }

    /// `netadr_t` from hexenworld/shared/net.h.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct NetAdrC {
        pub ip: [u8; 4],
        pub port: u16,
        pub pad: u16,
    }

    /// `netchan_t` from hexenworld/shared/net.h.  Caller-owned: the port only
    /// ever operates on a pointer it was handed.
    #[repr(C)]
    pub struct NetChanC {
        pub fatal_error: c_int, // qboolean
        pub last_received: f32,
        pub frame_latency: f32,
        pub frame_rate: f32,
        pub drop_count: c_int,
        pub good_count: c_int,
        pub remote_address: NetAdrC,
        pub cleartime: f64,
        pub rate: f64,
        pub incoming_sequence: c_int,
        pub incoming_acknowledged: c_int,
        pub incoming_reliable_acknowledged: c_int,
        pub incoming_reliable_sequence: c_int,
        pub outgoing_sequence: c_int,
        pub reliable_sequence: c_int,
        pub last_reliable_sequence: c_int,
        pub message: SizeBufC,
        pub message_buf: [u8; HWNET_MAX_MSGLEN],
        pub reliable_length: c_int,
        pub reliable_buf: [u8; HWNET_MAX_MSGLEN],
        pub outgoing_size: [c_int; MAX_LATENT],
        pub outgoing_time: [f64; MAX_LATENT],
    }

    const PTR_SIZE: usize = core::mem::size_of::<*mut u8>();
    const PTR_ALIGN: usize = core::mem::align_of::<*mut u8>();
    const F32_ALIGN: usize = core::mem::align_of::<f32>();
    const F64_ALIGN: usize = core::mem::align_of::<f64>();

    const fn align_up(x: usize, align: usize) -> usize {
        (x + align - 1) & !(align - 1)
    }

    // The offsets are derived rather than written down: the same archive is
    // built for targets with different pointer widths, and a layout that was
    // only ever checked at 8 bytes would be silent corruption at 4.
    const _: () = {
        // fatal_error, then the three floats and two ints.
        assert!(core::mem::offset_of!(NetChanC, fatal_error) == 0);
        assert!(core::mem::offset_of!(NetChanC, last_received) == align_up(4, F32_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, frame_latency) == align_up(8, F32_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, frame_rate) == align_up(12, F32_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, drop_count) == 16);
        assert!(core::mem::offset_of!(NetChanC, good_count) == 20);
        // netadr_t has align 2 and size 8.
        assert!(core::mem::offset_of!(NetChanC, remote_address) == 24);
        assert!(core::mem::offset_of!(NetChanC, cleartime) == align_up(32, F64_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, rate) == align_up(40, F64_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, incoming_sequence) == 48);
        assert!(core::mem::offset_of!(NetChanC, incoming_acknowledged) == 52);
        assert!(core::mem::offset_of!(NetChanC, incoming_reliable_acknowledged) == 56);
        assert!(core::mem::offset_of!(NetChanC, incoming_reliable_sequence) == 60);
        assert!(core::mem::offset_of!(NetChanC, outgoing_sequence) == 64);
        assert!(core::mem::offset_of!(NetChanC, reliable_sequence) == 68);
        assert!(core::mem::offset_of!(NetChanC, last_reliable_sequence) == 72);
        assert!(core::mem::offset_of!(NetChanC, message) == align_up(76, PTR_ALIGN));
        assert!(core::mem::offset_of!(NetChanC, message_buf)
            == align_up(76, PTR_ALIGN) + core::mem::size_of::<SizeBufC>());
        assert!(core::mem::offset_of!(NetChanC, reliable_length)
            == align_up(76, PTR_ALIGN) + core::mem::size_of::<SizeBufC>() + HWNET_MAX_MSGLEN);
        assert!(core::mem::offset_of!(NetChanC, reliable_buf)
            == core::mem::offset_of!(NetChanC, reliable_length) + 4);
        assert!(core::mem::offset_of!(NetChanC, outgoing_size)
            == core::mem::offset_of!(NetChanC, reliable_buf) + HWNET_MAX_MSGLEN);
        assert!(core::mem::offset_of!(NetChanC, outgoing_time)
            == align_up(core::mem::offset_of!(NetChanC, outgoing_size) + 4 * MAX_LATENT, F64_ALIGN));
        assert!(PTR_SIZE >= 4); // the assertions above are width-dependent
    };

    // The two cvars are `static` in the C: not part of the ABI, so not
    // exported here either.  Initialised as the C's declarations are --
    // { name, string, CVAR_NONE } -- with the rest zero.
    static mut SHOWPACKETS: CvarC = CvarC {
        name: c"showpackets".as_ptr(),
        string: c"0".as_ptr() as *mut c_char,
        flags: 0,
        value: 0.0,
        integer: 0,
        callback: None,
        next: core::ptr::null_mut(),
        default_string: core::ptr::null_mut(),
    };
    static mut SHOWDROP: CvarC = CvarC {
        name: c"showdrop".as_ptr(),
        string: c"0".as_ptr() as *mut c_char,
        flags: 0,
        value: 0.0,
        integer: 0,
        callback: None,
        next: core::ptr::null_mut(),
        default_string: core::ptr::null_mut(),
    };

    #[inline]
    unsafe fn not_demo_playback() -> bool {
        NetChan_TargetNotDemoPlayback() != 0
    }

    #[inline]
    unsafe fn realtime_now() -> f64 {
        realtime
    }

    // The layout accessors let the C differential harness and
    // engine/rust/tests/abi_layout.c check this struct against the real
    // `netchan_t` without restating Rust's offsets in C, as the other ports do.
    #[no_mangle]
    pub extern "C" fn NetChanC_sizeof() -> usize {
        core::mem::size_of::<NetChanC>()
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_alignof() -> usize {
        core::mem::align_of::<NetChanC>()
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_fatal_error() -> usize {
        core::mem::offset_of!(NetChanC, fatal_error)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_last_received() -> usize {
        core::mem::offset_of!(NetChanC, last_received)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_frame_latency() -> usize {
        core::mem::offset_of!(NetChanC, frame_latency)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_frame_rate() -> usize {
        core::mem::offset_of!(NetChanC, frame_rate)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_drop_count() -> usize {
        core::mem::offset_of!(NetChanC, drop_count)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_good_count() -> usize {
        core::mem::offset_of!(NetChanC, good_count)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_remote_address() -> usize {
        core::mem::offset_of!(NetChanC, remote_address)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_cleartime() -> usize {
        core::mem::offset_of!(NetChanC, cleartime)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_rate() -> usize {
        core::mem::offset_of!(NetChanC, rate)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_incoming_sequence() -> usize {
        core::mem::offset_of!(NetChanC, incoming_sequence)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_incoming_acknowledged() -> usize {
        core::mem::offset_of!(NetChanC, incoming_acknowledged)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_incoming_reliable_acknowledged() -> usize {
        core::mem::offset_of!(NetChanC, incoming_reliable_acknowledged)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_incoming_reliable_sequence() -> usize {
        core::mem::offset_of!(NetChanC, incoming_reliable_sequence)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_outgoing_sequence() -> usize {
        core::mem::offset_of!(NetChanC, outgoing_sequence)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_reliable_sequence() -> usize {
        core::mem::offset_of!(NetChanC, reliable_sequence)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_last_reliable_sequence() -> usize {
        core::mem::offset_of!(NetChanC, last_reliable_sequence)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_message() -> usize {
        core::mem::offset_of!(NetChanC, message)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_message_buf() -> usize {
        core::mem::offset_of!(NetChanC, message_buf)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_reliable_length() -> usize {
        core::mem::offset_of!(NetChanC, reliable_length)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_reliable_buf() -> usize {
        core::mem::offset_of!(NetChanC, reliable_buf)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_outgoing_size() -> usize {
        core::mem::offset_of!(NetChanC, outgoing_size)
    }

    #[no_mangle]
    pub extern "C" fn NetChanC_offsetof_outgoing_time() -> usize {
        core::mem::offset_of!(NetChanC, outgoing_time)
    }

    /// `Netchan_Init`
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_Init() {
        Cvar_RegisterVariable(&raw mut SHOWPACKETS);
        Cvar_RegisterVariable(&raw mut SHOWDROP);
    }

    /// `Netchan_OutOfBand` -- sends an out-of-band datagram.
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_OutOfBand(
        adr: *const NetAdrC,
        length: c_int,
        data: *mut u8,
    ) {
        let mut send_buf = [0u8; HWNET_MAX_MSGLEN + PACKET_HEADER];
        let mut senddata = core::mem::MaybeUninit::<SizeBufC>::zeroed().assume_init();

        SZ_Init(&mut senddata, send_buf.as_mut_ptr(), send_buf.len() as c_int);
        // -1 sequence means out of band
        MSG_WriteLong(&mut senddata, -1);
        SZ_Write(&mut senddata, data.cast::<c_void>(), length);

        if not_demo_playback() {
            HWNET_SendPacket(senddata.cursize, senddata.data.cast::<c_void>(), adr);
        }
    }

    /// `Netchan_Setup` -- opens a channel to a remote system.
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_Setup(chan: *mut NetChanC, adr: *const NetAdrC) {
        core::ptr::write_bytes(chan.cast::<u8>(), 0, core::mem::size_of::<NetChanC>());

        (*chan).remote_address = *adr;
        (*chan).last_received = realtime_now() as f32;

        SZ_Init(
            &mut (*chan).message,
            (*chan).message_buf.as_mut_ptr(),
            (*chan).message_buf.len() as c_int,
        );
        (*chan).message.allowoverflow = 1;

        (*chan).rate = 1.0 / 2500.0;
    }

    /// `Netchan_CanPacket` -- true when the bandwidth choke is not active.
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_CanPacket(chan: *const NetChanC) -> c_int {
        if (*chan).cleartime < realtime_now() + MAX_BACKUP * (*chan).rate {
            return 1;
        }
        0
    }

    /// `Netchan_CanReliable`
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_CanReliable(chan: *const NetChanC) -> c_int {
        if (*chan).reliable_length != 0 {
            return 0; // waiting for ack
        }
        HWNetchan_CanPacket(chan)
    }

    /// `Netchan_Transmit` -- sends an unreliable message and handles the
    /// retransmission of the reliable one.  A zero length still generates a
    /// packet and deals with the reliable queue.
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_Transmit(
        chan: *mut NetChanC,
        length: c_int,
        data: *mut u8,
    ) {
        let mut send_buf = [0u8; HWNET_MAX_MSGLEN + PACKET_HEADER];
        let mut senddata = core::mem::MaybeUninit::<SizeBufC>::zeroed().assume_init();
        let mut send_reliable: bool;
        let w1: c_uint;
        let w2: c_uint;

        // check for message overflow
        if (*chan).message.overflowed != 0 {
            (*chan).fatal_error = 1;
            CON_Printf(
                0,
                c"%s:Outgoing message overflow\n".as_ptr(),
                HWNET_AdrToString(&(*chan).remote_address),
            );
            return;
        }

        // if the remote side dropped the last reliable message, resend it
        send_reliable = false;

        if (*chan).incoming_acknowledged > (*chan).last_reliable_sequence
            && (*chan).incoming_reliable_acknowledged != (*chan).reliable_sequence
        {
            send_reliable = true;
        }

        // if the reliable transmit buffer is empty, copy the current message out
        if (*chan).reliable_length == 0 && (*chan).message.cursize != 0 {
            let n = (*chan).message.cursize as usize;
            core::ptr::copy_nonoverlapping(
                (*chan).message_buf.as_ptr(),
                (*chan).reliable_buf.as_mut_ptr(),
                n,
            );
            (*chan).reliable_length = (*chan).message.cursize;
            (*chan).message.cursize = 0;
            (*chan).reliable_sequence ^= 1;
            send_reliable = true;
        }

        // write the packet header
        SZ_Init(&mut senddata, send_buf.as_mut_ptr(), send_buf.len() as c_int);

        w1 = ((*chan).outgoing_sequence as c_uint) | ((send_reliable as c_uint) << 31);
        w2 = ((*chan).incoming_sequence as c_uint)
            | (((*chan).incoming_reliable_sequence as c_uint) << 31);

        (*chan).outgoing_sequence = (*chan).outgoing_sequence.wrapping_add(1);

        MSG_WriteLong(&mut senddata, w1 as c_int);
        MSG_WriteLong(&mut senddata, w2 as c_int);

        // copy the reliable message to the packet first
        if send_reliable {
            SZ_Write(
                &mut senddata,
                (*chan).reliable_buf.as_ptr().cast::<c_void>(),
                (*chan).reliable_length,
            );
            (*chan).last_reliable_sequence = (*chan).outgoing_sequence;
        }

        // add the unreliable part if space is available
        if senddata.maxsize - senddata.cursize >= length {
            SZ_Write(&mut senddata, data.cast::<c_void>(), length);
        }

        // send the datagram
        let i = ((*chan).outgoing_sequence as usize) & (MAX_LATENT - 1);
        (*chan).outgoing_size[i] = senddata.cursize;
        (*chan).outgoing_time[i] = realtime_now();

        if not_demo_playback() {
            HWNET_SendPacket(
                senddata.cursize,
                senddata.data.cast::<c_void>(),
                &(*chan).remote_address,
            );
        }

        let now = realtime_now();
        if (*chan).cleartime < now {
            (*chan).cleartime = now + senddata.cursize as f64 * (*chan).rate;
        } else {
            (*chan).cleartime += senddata.cursize as f64 * (*chan).rate;
        }

        if SHOWPACKETS.integer != 0 {
            CON_Printf(
                0,
                c"--> s=%i(%i) a=%i(%i) %i\n".as_ptr(),
                (*chan).outgoing_sequence,
                send_reliable as c_int,
                (*chan).incoming_sequence,
                (*chan).incoming_reliable_sequence,
                senddata.cursize,
            );
        }
    }

    /// `Netchan_Process` -- called when the current net_message is from the
    /// channel's remote address; modifies net_message so that it points at the
    /// packet payload.
    #[no_mangle]
    pub unsafe extern "C" fn HWNetchan_Process(chan: *mut NetChanC) -> c_int {
        let sequence;
        let sequence_ack;
        let reliable_ack: c_int;
        let reliable_message: c_int;

        if not_demo_playback() && HWNET_CompareAdr(NetUDP_TargetFrom(), &(*chan).remote_address) == 0
        {
            return 0;
        }

        // get sequence numbers.  The C picks MSG_BeginReading() or
        // MSG_BeginReadingFrom(&net_message) by target; both name the same
        // buffer here, so the port always passes it explicitly.
        let msg = NetUDP_TargetMessageBuf();
        MSG_BeginReadingFrom(msg);
        sequence = MSG_ReadLong(msg) as c_uint;
        sequence_ack = MSG_ReadLong(msg) as c_uint;

        reliable_message = (sequence >> 31) as c_int;
        reliable_ack = (sequence_ack >> 31) as c_int;

        let sequence = sequence & !(1u32 << 31);
        let sequence_ack = sequence_ack & !(1u32 << 31);

        if SHOWPACKETS.integer != 0 {
            CON_Printf(
                0,
                c"<-- s=%u(%i) a=%u(%i) %i\n".as_ptr(),
                sequence,
                reliable_message,
                sequence_ack,
                reliable_ack,
                (*msg).cursize,
            );
        }

        // The #if 0 rate-estimation block in the C is dead code and is not
        // ported; chan->rate keeps the value Netchan_Setup gives it.

        // discard stale or duplicated packets
        if sequence <= (*chan).incoming_sequence as c_uint {
            if SHOWDROP.integer != 0 {
                CON_Printf(
                    0,
                    c"%s:Out of order packet %u at %i\n".as_ptr(),
                    HWNET_AdrToString(&(*chan).remote_address),
                    sequence,
                    (*chan).incoming_sequence,
                );
            }
            return 0;
        }

        // dropped packets don't keep the message from being used
        let drop = sequence as c_int - ((*chan).incoming_sequence + 1);
        *NetChan_TargetDrop() = drop;
        if drop > 0 {
            (*chan).drop_count += 1;

            if SHOWDROP.integer != 0 {
                CON_Printf(
                    0,
                    c"%s:Dropped %u packets at %u\n".as_ptr(),
                    HWNET_AdrToString(&(*chan).remote_address),
                    sequence.wrapping_sub((*chan).incoming_sequence as c_uint + 1),
                    sequence,
                );
            }
        }

        // if the current outgoing reliable message has been acknowledged,
        // clear the buffer to make way for the next
        if reliable_ack == (*chan).reliable_sequence {
            (*chan).reliable_length = 0; // it has been received
        }

        // if this message contains a reliable message, bump
        // incoming_reliable_sequence
        (*chan).incoming_sequence = sequence as c_int;
        (*chan).incoming_acknowledged = sequence_ack as c_int;
        (*chan).incoming_reliable_acknowledged = reliable_ack;
        if reliable_message != 0 {
            (*chan).incoming_reliable_sequence ^= 1;
        }

        // the message can now be read from the current message pointer;
        // update statistics counters
        let now = realtime_now();
        (*chan).frame_latency = ((*chan).frame_latency as f64 * OLD_AVG
            + ((*chan).outgoing_sequence - sequence_ack as c_int) as f64 * (1.0 - OLD_AVG))
            as f32;
        (*chan).frame_rate =
            ((*chan).frame_rate as f64 * OLD_AVG + (now - (*chan).last_received as f64) * (1.0 - OLD_AVG))
                as f32;
        (*chan).good_count += 1;

        (*chan).last_received = now as f32;

        1
    }
}
