// SPDX-License-Identifier: GPL-2.0-or-later
// Rust replacement for engine/hexen2/net_loop.c.
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.

/// `struct qsockaddr` from engine/hexen2/net_defs.h, and the `sockaddr_in`
/// every land driver casts it to.
///
/// Its shape is not fixed.  Under `HAVE_SA_LEN` -- the BSDs, macOS, OS/2,
/// Hurd, Haiku -- the first two members are `unsigned char qsa_len` and
/// `unsigned char qsa_family`; everywhere else `qsa_family` is a short at
/// offset 0.  The same split applies to `sockaddr_in` (sa_len, then
/// sa_family, then port and address), which is why net_udp.c can cast one to
/// the other and read both through struct member accesses.
///
/// Both are 16 bytes and the port and address bytes land in the same place
/// either way, so only the family moves -- two bytes at offset 0, or one byte
/// at offset 1.  Storage is therefore a fixed 16 bytes and the family's
/// offset and width come from the C headers through net_udp_h2_target.c.
/// Keeping that out of here is the point: a `#[cfg(target_os = ...)]` list
/// would have to name every BSD to stay right, and the one that got left off
/// would silently read the wrong byte.
pub const QSOCKADDR_SIZE: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct QSockAddr { pub storage: [u8; QSOCKADDR_SIZE] }

impl QSockAddr {
    pub const fn zeroed() -> Self { QSockAddr { storage: [0; QSOCKADDR_SIZE] } }
}
#[no_mangle]
pub extern "C" fn H2QSockAddr_sizeof() -> usize { core::mem::size_of::<QSockAddr>() }
#[no_mangle]
pub extern "C" fn H2QSocket_sizeof() -> usize { core::mem::size_of::<QSocket>() }
#[no_mangle]
pub extern "C" fn H2QSocket_receiveMessage_offset() -> usize {
    core::mem::offset_of!(QSocket, receive_message)
}
#[no_mangle]
pub extern "C" fn H2QSocket_driverdata_offset() -> usize {
    core::mem::offset_of!(QSocket, driverdata)
}

use core::ffi::{c_char, c_int};
const NET_MAXMESSAGE: usize = 32768; // hexen2/net.h
const NET_NAMELEN: usize = 64;
#[cfg(windows)]
type Socket = usize;
#[cfg(not(windows))]
type Socket = c_int;
#[repr(C)]
pub struct QSocket {
    pub next: *mut QSocket,
    pub connecttime: f64,
    pub last_message_time: f64,
    pub last_send_time: f64,
    pub disconnected: c_int,
    pub can_send: c_int,
    pub send_next: c_int,
    pub driver: c_int,
    pub landriver: c_int,
    pub socket: Socket,
    pub driverdata: *mut QSocket,
    pub ack_sequence: u32,
    pub send_sequence: u32,
    pub unreliable_send_sequence: u32,
    pub send_message_length: c_int,
    pub send_message: [u8; NET_MAXMESSAGE],
    pub receive_sequence: u32,
    pub unreliable_receive_sequence: u32,
    pub receive_message_length: c_int,
    pub receive_message: [u8; NET_MAXMESSAGE],
    pub addr: QSockAddr,
    pub address: [c_char; NET_NAMELEN],
}

#[cfg(feature = "net_loop_h2")]
pub use imp::*;
#[cfg(feature = "net_loop_h2")]
mod imp {
use super::{QSocket, QSockAddr, NET_MAXMESSAGE};
use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use crate::sizebuf::SizeBufC;
#[repr(C)]
pub struct HostCache {
    pub name: [c_char; 16],
    pub map: [c_char; 16],
    pub cname: [c_char; 32],
    pub users: c_int,
    pub maxusers: c_int,
    pub driver: c_int,
    pub ldriver: c_int,
    pub addr: QSockAddr,
}
#[no_mangle]
pub extern "C" fn H2HostCache_sizeof() -> usize { core::mem::size_of::<HostCache>() }
#[no_mangle]
pub extern "C" fn H2HostCache_addr_offset() -> usize { core::mem::offset_of!(HostCache, addr) }
extern "C" {
    fn Loop_TargetDedicated() -> c_int;
    fn Loop_TargetServerActive() -> c_int;
    fn Loop_TargetMapName() -> *const c_char;
    fn Loop_TargetMaxClients() -> c_int;
    fn Loop_TargetHostname() -> *const c_char;
    static mut hostCacheCount: c_int;
    static mut hostcache: [HostCache; 8];
    static net_activeconnections: c_int;
    static net_driverlevel: c_int;
    static mut net_message: SizeBufC;
    fn NET_NewQSocket() -> *mut QSocket;
    fn SZ_Clear(buf: *mut SizeBufC);
    fn SZ_Write(buf: *mut SizeBufC, data: *const c_void, len: c_int);
    fn CON_Printf(flags: u32, fmt: *const c_char, ...);
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strcpy(dst: *mut c_char, src: *const c_char) -> *mut c_char;
}
static mut LOCAL_CONNECT_PENDING: bool = false;
static mut LOOP_CLIENT: *mut QSocket = ptr::null_mut();
static mut LOOP_SERVER: *mut QSocket = ptr::null_mut();

#[no_mangle]
pub unsafe extern "C" fn Loop_Init() -> c_int { if Loop_TargetDedicated() != 0 { -1 } else { 0 } }
#[no_mangle]
pub unsafe extern "C" fn Loop_Shutdown() {}
#[no_mangle]
pub unsafe extern "C" fn Loop_Listen(_state: c_int) {}
#[no_mangle]
pub unsafe extern "C" fn Loop_SearchForHosts(_xmit: c_int) {
    if Loop_TargetServerActive() == 0 { return; }
    hostCacheCount = 1;
    let host = Loop_TargetHostname();
    let cache = &raw mut hostcache[0];
    let name = if strcmp(host, c"UNNAMED".as_ptr()) == 0 { c"local".as_ptr() } else { host };
    strcpy((*cache).name.as_mut_ptr(), name);
    strcpy((*cache).map.as_mut_ptr(), Loop_TargetMapName());
    (*cache).users = net_activeconnections;
    (*cache).maxusers = Loop_TargetMaxClients();
    (*cache).driver = net_driverlevel;
    strcpy((*cache).cname.as_mut_ptr(), c"local".as_ptr());
}
#[no_mangle]
pub unsafe extern "C" fn Loop_Connect(host: *const c_char) -> *mut QSocket {
    if strcmp(host, c"local".as_ptr()) != 0 { return ptr::null_mut(); }
    LOCAL_CONNECT_PENDING = true;
    if LOOP_CLIENT.is_null() {
        LOOP_CLIENT = NET_NewQSocket();
        if LOOP_CLIENT.is_null() {
            CON_Printf(0, c"%s: no qsocket available\n".as_ptr(), c"Loop_Connect".as_ptr());
            return ptr::null_mut();
        }
        strcpy((*LOOP_CLIENT).address.as_mut_ptr(), c"localhost".as_ptr());
    }
    (*LOOP_CLIENT).receive_message_length = 0;
    (*LOOP_CLIENT).send_message_length = 0;
    (*LOOP_CLIENT).can_send = 1;
    if LOOP_SERVER.is_null() {
        LOOP_SERVER = NET_NewQSocket();
        if LOOP_SERVER.is_null() {
            CON_Printf(0, c"%s: no qsocket available\n".as_ptr(), c"Loop_Connect".as_ptr());
            return ptr::null_mut();
        }
        strcpy((*LOOP_SERVER).address.as_mut_ptr(), c"LOCAL".as_ptr());
    }
    (*LOOP_SERVER).receive_message_length = 0;
    (*LOOP_SERVER).send_message_length = 0;
    (*LOOP_SERVER).can_send = 1;
    (*LOOP_CLIENT).driverdata = LOOP_SERVER;
    (*LOOP_SERVER).driverdata = LOOP_CLIENT;
    LOOP_CLIENT
}
#[no_mangle]
pub unsafe extern "C" fn Loop_CheckNewConnections() -> *mut QSocket {
    if !LOCAL_CONNECT_PENDING { return ptr::null_mut(); }
    LOCAL_CONNECT_PENDING = false;
    (*LOOP_SERVER).send_message_length = 0;
    (*LOOP_SERVER).receive_message_length = 0;
    (*LOOP_SERVER).can_send = 1;
    (*LOOP_CLIENT).send_message_length = 0;
    (*LOOP_CLIENT).receive_message_length = 0;
    (*LOOP_CLIENT).can_send = 1;
    LOOP_SERVER
}
#[inline]
fn int_align(value: c_int) -> c_int { (value + 3) & !3 }
#[no_mangle]
pub unsafe extern "C" fn Loop_GetMessage(sock: *mut QSocket) -> c_int {
    if (*sock).receive_message_length == 0 { return 0; }
    let ret = (*sock).receive_message[0] as c_int;
    let length = (*sock).receive_message[1] as c_int +
        (((*sock).receive_message[2] as c_int) << 8);
    SZ_Clear(&raw mut net_message);
    SZ_Write(&raw mut net_message, (*sock).receive_message.as_ptr().add(4).cast(), length);
    let consumed = int_align(length + 4);
    (*sock).receive_message_length -= consumed;
    if (*sock).receive_message_length != 0 {
        ptr::copy((*sock).receive_message.as_ptr().add(consumed as usize),
            (*sock).receive_message.as_mut_ptr(), (*sock).receive_message_length as usize);
    }
    if !(*sock).driverdata.is_null() && ret == 1 { (*(*sock).driverdata).can_send = 1; }
    ret
}
unsafe fn send_message(sock: *mut QSocket, data: *mut SizeBufC, reliable: bool) -> c_int {
    let peer = (*sock).driverdata;
    if peer.is_null() { return -1; }
    let old_len = (*peer).receive_message_length;
    let len = (*data).cursize;
    if reliable {
        if old_len + len + 4 > NET_MAXMESSAGE as c_int {
            Sys_Error(c"%s: overflow".as_ptr(), c"Loop_SendMessage".as_ptr());
        }
    } else if old_len + len + 3 > NET_MAXMESSAGE as c_int { return 0; }
    let buffer = (*peer).receive_message.as_mut_ptr().add(old_len as usize);
    *buffer = if reliable { 1 } else { 2 };
    *buffer.add(1) = len as u8;
    *buffer.add(2) = (len >> 8) as u8;
    ptr::copy_nonoverlapping((*data).data, buffer.add(4), len as usize);
    (*peer).receive_message_length = int_align(old_len + len + 4);
    if reliable { (*sock).can_send = 0; }
    1
}
#[no_mangle]
pub unsafe extern "C" fn Loop_SendMessage(sock: *mut QSocket, data: *mut SizeBufC) -> c_int {
    send_message(sock, data, true)
}
#[no_mangle]
pub unsafe extern "C" fn Loop_SendUnreliableMessage(sock: *mut QSocket, data: *mut SizeBufC) -> c_int {
    send_message(sock, data, false)
}
#[no_mangle]
pub unsafe extern "C" fn Loop_CanSendMessage(sock: *mut QSocket) -> c_int {
    if (*sock).driverdata.is_null() { 0 } else { (*sock).can_send }
}
#[no_mangle]
pub unsafe extern "C" fn Loop_CanSendUnreliableMessage(_sock: *mut QSocket) -> c_int { 1 }
#[no_mangle]
pub unsafe extern "C" fn Loop_Close(sock: *mut QSocket) {
    if !(*sock).driverdata.is_null() { (*(*sock).driverdata).driverdata = ptr::null_mut(); }
    (*sock).receive_message_length = 0;
    (*sock).send_message_length = 0;
    (*sock).can_send = 1;
    if sock == LOOP_CLIENT { LOOP_CLIENT = ptr::null_mut(); } else { LOOP_SERVER = ptr::null_mut(); }
}
} // client-only implementation
