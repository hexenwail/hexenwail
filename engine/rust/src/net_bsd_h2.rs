// SPDX-License-Identifier: GPL-2.0-or-later
// Rust replacement for engine/hexen2/net_bsd.c.
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
// The order and function-pointer fields are the network driver's C ABI.

use core::ffi::{c_char, c_int};
use crate::sizebuf::SizeBufC;
use crate::net_loop_h2::{QSockAddr, QSocket};

type Socket = c_int;
#[repr(C)]
pub struct NetDriver {
    pub name: *const c_char,
    pub initialized: c_int,
    pub init: Option<unsafe extern "C" fn() -> c_int>,
    pub listen: Option<unsafe extern "C" fn(c_int)>,
    #[cfg(not(feature = "h2ded_target"))]
    pub search_for_hosts: Option<unsafe extern "C" fn(c_int)>,
    #[cfg(not(feature = "h2ded_target"))]
    pub connect: Option<unsafe extern "C" fn(*const c_char) -> *mut QSocket>,
    pub check_new_connections: Option<unsafe extern "C" fn() -> *mut QSocket>,
    pub get_message: Option<unsafe extern "C" fn(*mut QSocket) -> c_int>,
    pub send_message: Option<unsafe extern "C" fn(*mut QSocket, *mut SizeBufC) -> c_int>,
    pub send_unreliable_message: Option<unsafe extern "C" fn(*mut QSocket, *mut SizeBufC) -> c_int>,
    pub can_send_message: Option<unsafe extern "C" fn(*mut QSocket) -> c_int>,
    pub can_send_unreliable_message: Option<unsafe extern "C" fn(*mut QSocket) -> c_int>,
    pub close: Option<unsafe extern "C" fn(*mut QSocket)>,
    pub shutdown: Option<unsafe extern "C" fn()>,
}
#[repr(C)]
pub struct NetLandriver {
    pub name: *const c_char,
    pub initialized: c_int,
    pub control_sock: Socket,
    pub init: Option<unsafe extern "C" fn() -> Socket>,
    pub shutdown: Option<unsafe extern "C" fn()>,
    pub listen: Option<unsafe extern "C" fn(c_int)>,
    pub open_socket: Option<unsafe extern "C" fn(c_int) -> Socket>,
    pub close_socket: Option<unsafe extern "C" fn(Socket) -> c_int>,
    pub connect: Option<unsafe extern "C" fn(Socket, *mut QSockAddr) -> c_int>,
    pub check_new_connections: Option<unsafe extern "C" fn() -> Socket>,
    pub read: Option<unsafe extern "C" fn(Socket, *mut u8, c_int, *mut QSockAddr) -> c_int>,
    pub write: Option<unsafe extern "C" fn(Socket, *mut u8, c_int, *mut QSockAddr) -> c_int>,
    pub broadcast: Option<unsafe extern "C" fn(Socket, *mut u8, c_int) -> c_int>,
    pub addr_to_string: Option<unsafe extern "C" fn(*mut QSockAddr) -> *const c_char>,
    pub string_to_addr: Option<unsafe extern "C" fn(*const c_char, *mut QSockAddr) -> c_int>,
    pub get_socket_addr: Option<unsafe extern "C" fn(Socket, *mut QSockAddr) -> c_int>,
    pub get_name_from_addr: Option<unsafe extern "C" fn(*mut QSockAddr, *mut c_char) -> c_int>,
    pub get_addr_from_name: Option<unsafe extern "C" fn(*const c_char, *mut QSockAddr) -> c_int>,
    pub addr_compare: Option<unsafe extern "C" fn(*mut QSockAddr, *mut QSockAddr) -> c_int>,
    pub get_socket_port: Option<unsafe extern "C" fn(*mut QSockAddr) -> c_int>,
    pub set_socket_port: Option<unsafe extern "C" fn(*mut QSockAddr, c_int) -> c_int>,
}
#[no_mangle]
pub extern "C" fn H2NetDriver_sizeof() -> usize { core::mem::size_of::<NetDriver>() }
#[no_mangle]
pub extern "C" fn H2NetDriver_init_offset() -> usize { core::mem::offset_of!(NetDriver, init) }
#[no_mangle]
pub extern "C" fn H2NetDriver_shutdown_offset() -> usize { core::mem::offset_of!(NetDriver, shutdown) }
#[no_mangle]
pub extern "C" fn H2NetLandriver_sizeof() -> usize { core::mem::size_of::<NetLandriver>() }
#[no_mangle]
pub extern "C" fn H2NetLandriver_read_offset() -> usize { core::mem::offset_of!(NetLandriver, read) }

extern "C" {
    fn Datagram_Init() -> c_int;
    fn Datagram_Listen(state: c_int);
    #[cfg(not(feature = "h2ded_target"))]
    fn Datagram_SearchForHosts(xmit: c_int);
    #[cfg(not(feature = "h2ded_target"))]
    fn Datagram_Connect(host: *const c_char) -> *mut QSocket;
    fn Datagram_CheckNewConnections() -> *mut QSocket;
    fn Datagram_GetMessage(sock: *mut QSocket) -> c_int;
    fn Datagram_SendMessage(sock: *mut QSocket, data: *mut SizeBufC) -> c_int;
    fn Datagram_SendUnreliableMessage(sock: *mut QSocket, data: *mut SizeBufC) -> c_int;
    fn Datagram_CanSendMessage(sock: *mut QSocket) -> c_int;
    fn Datagram_CanSendUnreliableMessage(sock: *mut QSocket) -> c_int;
    fn Datagram_Close(sock: *mut QSocket);
    fn Datagram_Shutdown();
}

macro_rules! driver {
    ($prefix:ident) => { driver!($prefix, $prefix) };
    (Loop, $unused:ident) => { NetDriver {
        name: b"Loopback\0".as_ptr().cast(), initialized: 0,
        init: Some(crate::net_loop_h2::Loop_Init), listen: Some(crate::net_loop_h2::Loop_Listen),
        search_for_hosts: Some(crate::net_loop_h2::Loop_SearchForHosts),
        connect: Some(crate::net_loop_h2::Loop_Connect),
        check_new_connections: Some(crate::net_loop_h2::Loop_CheckNewConnections),
        get_message: Some(crate::net_loop_h2::Loop_GetMessage),
        send_message: Some(crate::net_loop_h2::Loop_SendMessage),
        send_unreliable_message: Some(crate::net_loop_h2::Loop_SendUnreliableMessage),
        can_send_message: Some(crate::net_loop_h2::Loop_CanSendMessage),
        can_send_unreliable_message: Some(crate::net_loop_h2::Loop_CanSendUnreliableMessage),
        close: Some(crate::net_loop_h2::Loop_Close), shutdown: Some(crate::net_loop_h2::Loop_Shutdown),
    }};
    (Datagram, $unused:ident) => { NetDriver {
        name: b"Datagram\0".as_ptr().cast(), initialized: 0,
        init: Some(Datagram_Init), listen: Some(Datagram_Listen),
        #[cfg(not(feature = "h2ded_target"))]
        search_for_hosts: Some(Datagram_SearchForHosts),
        #[cfg(not(feature = "h2ded_target"))]
        connect: Some(Datagram_Connect),
        check_new_connections: Some(Datagram_CheckNewConnections),
        get_message: Some(Datagram_GetMessage), send_message: Some(Datagram_SendMessage),
        send_unreliable_message: Some(Datagram_SendUnreliableMessage),
        can_send_message: Some(Datagram_CanSendMessage),
        can_send_unreliable_message: Some(Datagram_CanSendUnreliableMessage),
        close: Some(Datagram_Close), shutdown: Some(Datagram_Shutdown),
    }};
}

#[cfg(all(not(feature = "h2ded_target"), not(target_family = "wasm")))]
#[no_mangle]
pub static mut net_drivers: [NetDriver; 2] = [driver!(Loop), driver!(Datagram)];
#[cfg(all(feature = "h2ded_target", not(target_family = "wasm")))]
#[no_mangle]
pub static mut net_drivers: [NetDriver; 1] = [driver!(Datagram)];
#[cfg(all(not(feature = "h2ded_target"), not(target_family = "wasm")))]
#[no_mangle]
pub static net_numdrivers: c_int = 2;
#[cfg(all(feature = "h2ded_target", not(target_family = "wasm")))]
#[no_mangle]
pub static net_numdrivers: c_int = 1;

const fn landriver() -> NetLandriver { NetLandriver {
    name: b"UDP\0".as_ptr().cast(), initialized: 0, control_sock: 0,
    init: Some(crate::net_udp_h2::UDP_Init), shutdown: Some(crate::net_udp_h2::UDP_Shutdown),
    listen: Some(crate::net_udp_h2::UDP_Listen), open_socket: Some(crate::net_udp_h2::UDP_OpenSocket),
    close_socket: Some(crate::net_udp_h2::UDP_CloseSocket), connect: Some(crate::net_udp_h2::UDP_Connect),
    check_new_connections: Some(crate::net_udp_h2::UDP_CheckNewConnections),
    read: Some(crate::net_udp_h2::UDP_Read), write: Some(crate::net_udp_h2::UDP_Write),
    broadcast: Some(crate::net_udp_h2::UDP_Broadcast),
    addr_to_string: Some(crate::net_udp_h2::UDP_AddrToString),
    string_to_addr: Some(crate::net_udp_h2::UDP_StringToAddr),
    get_socket_addr: Some(crate::net_udp_h2::UDP_GetSocketAddr),
    get_name_from_addr: Some(crate::net_udp_h2::UDP_GetNameFromAddr),
    get_addr_from_name: Some(crate::net_udp_h2::UDP_GetAddrFromName),
    addr_compare: Some(crate::net_udp_h2::UDP_AddrCompare),
    get_socket_port: Some(crate::net_udp_h2::UDP_GetSocketPort),
    set_socket_port: Some(crate::net_udp_h2::UDP_SetSocketPort),
} }
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut net_landrivers: [NetLandriver; 1] = [landriver()];
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static net_numlandrivers: c_int = 1;

// rustc's wasm32 staticlib makes exported data local even with #[no_mangle].
// C owns only the four data objects there; all table entries, including each
// function pointer, are constructed here before the first NET_Init traversal.
#[cfg(target_family = "wasm")]
extern "C" {
    static mut net_drivers: [NetDriver; 2];
    static mut net_landrivers: [NetLandriver; 1];
}
#[cfg(target_family = "wasm")]
#[no_mangle]
pub unsafe extern "C" fn NetH2_InitDriverTables() {
    let drivers = [driver!(Loop), driver!(Datagram)];
    core::ptr::copy_nonoverlapping(drivers.as_ptr(), (&raw mut net_drivers).cast::<NetDriver>(), 2);
    let land = [landriver()];
    core::ptr::copy_nonoverlapping(land.as_ptr(), (&raw mut net_landrivers).cast::<NetLandriver>(), 1);
}

