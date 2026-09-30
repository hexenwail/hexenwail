// SPDX-License-Identifier: GPL-2.0-or-later
// Rust replacement for engine/hexen2/net_udp.c.
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.

use core::ffi::{c_char, c_int, c_ulong, c_void, CStr};
use core::ptr;
use crate::net_loop_h2::QSockAddr;
const INVALID_SOCKET: c_int = -1;
const AF_INET: c_int = 2;
const SOCK_DGRAM: c_int = 2;
const IPPROTO_UDP: c_int = 17;
const SOL_SOCKET: c_int = 1;
const SO_BROADCAST: c_int = 6;
const FIONBIO: c_ulong = 0x5421;
const FIONREAD: c_ulong = 0x541b;
const EWOULDBLOCK: c_int = 11;
const ECONNREFUSED: c_int = 111;
#[repr(C)]
struct QuakeParmsC {
    basedir: *mut c_char, userdir: *mut c_char,
    argc: c_int, argv: *mut *mut c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SockAddrIn { family: u16, port: u16, ip: u32, zero: [u8; 8] }
#[repr(C)]
struct HostEnt {
    name: *mut c_char, aliases: *mut *mut c_char,
    addr_type: c_int, length: c_int, addresses: *mut *mut u8,
}
extern "C" {
    fn COM_CheckParm(s: *const c_char) -> c_int;
    static host_parms: *mut QuakeParmsC;
    static net_hostport: c_int;
    static mut tcpipAvailable: c_int;
    static mut my_tcpip_address: [c_char; 64];
    fn CON_Printf(flags: u32, fmt: *const c_char, ...);
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn socket(domain: c_int, ty: c_int, proto: c_int) -> c_int;
    fn ioctl(fd: c_int, req: c_ulong, ...) -> c_int;
    fn bind(fd: c_int, addr: *const SockAddrIn, len: u32) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn sendto(fd: c_int, buf: *const u8, len: usize, flags: c_int,
        addr: *const SockAddrIn, addrlen: u32) -> isize;
    fn recvfrom(fd: c_int, buf: *mut u8, len: usize, flags: c_int,
        addr: *mut SockAddrIn, addrlen: *mut u32) -> isize;
    fn getsockname(fd: c_int, addr: *mut SockAddrIn, len: *mut u32) -> c_int;
    fn gethostname(buf: *mut c_char, len: usize) -> c_int;
    fn gethostbyname(name: *const c_char) -> *mut HostEnt;
    fn gethostbyaddr(addr: *const u8, len: u32, kind: c_int) -> *mut HostEnt;
    fn inet_addr(name: *const c_char) -> u32;
    fn inet_ntoa(addr: u32) -> *const c_char;
    fn __errno_location() -> *mut c_int;
    fn strerror(err: c_int) -> *const c_char;
    #[cfg(target_os = "linux")]
    fn hstrerror(err: c_int) -> *const c_char;
    #[cfg(target_os = "linux")]
    fn __h_errno_location() -> *mut c_int;
    fn strcpy(dst: *mut c_char, src: *const c_char) -> *mut c_char;
    fn strncpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char;
    fn strrchr(s: *const c_char, c: c_int) -> *mut c_char;
    fn atoi(s: *const c_char) -> c_int;
    fn sscanf(s: *const c_char, fmt: *const c_char, ...) -> c_int;
    fn q_snprintf(buf: *mut c_char, len: usize, fmt: *const c_char, ...) -> c_int;
}
static mut ACCEPT_SOCKET: c_int = INVALID_SOCKET;
static mut CONTROL_SOCKET: c_int = 0;
static mut BROADCAST_SOCKET: c_int = 0;
static mut BROADCAST_ADDR: SockAddrIn = SockAddrIn {family: 0, port: 0, ip: 0, zero: [0;8]};
static mut MY_ADDR: u32 = 0;
static mut LOCAL_ADDR: u32 = !0;
static mut BIND_ADDR: u32 = !0;

fn empty_addr() -> SockAddrIn { SockAddrIn {family: AF_INET as u16, port: 0, ip: 0, zero: [0;8]} }
#[inline]
fn network(v: u16) -> u16 { v.to_be() }
#[inline]
fn host(v: u16) -> u16 { u16::from_be(v) }
#[inline]
fn network32(v: u32) -> u32 { v.to_be() }
#[inline]
unsafe fn sockaddr(addr: *mut QSockAddr) -> *mut SockAddrIn { addr.cast() }
#[inline]
unsafe fn error_string() -> *const c_char { strerror(*__errno_location()) }
#[cfg(target_os = "linux")]
unsafe fn host_error_string() -> *const c_char { hstrerror(*__h_errno_location()) }
#[cfg(not(target_os = "linux"))]
unsafe fn host_error_string() -> *const c_char { error_string() }
#[inline]
unsafe fn parm_value(option: *const c_char) -> *const c_char {
    let i = COM_CheckParm(option);
    if i <= 0 || host_parms.is_null() || i >= (*host_parms).argc - 1 {
        ptr::null()
    } else {
        *(*host_parms).argv.add((i + 1) as usize) as *const c_char
    }
}
// SIOCGIFCONF/SIOCGIFADDR on Linux: the C's first non-loopback interface
// selection. The ioctl-based scan does not exist in the browser's libc.
#[cfg(target_os = "linux")]
unsafe fn scan_interface(fd: c_int, ifname: *mut c_char) -> c_int {
    if COM_CheckParm(c"-noifscan".as_ptr()) != 0 { return -1; }
    #[repr(C)]
    struct IfConf { len: c_int, buf: *mut u8 }
    let mut buf = [0u8; 8192];
    let mut conf = IfConf { len: buf.len() as c_int, buf: buf.as_mut_ptr() };
    if ioctl(fd, 0x8912 as c_ulong, &mut conf) == -1 {
        CON_Printf(4, c"%s: SIOCGIFCONF failed (%s)\n".as_ptr(), c"udp_scan_iface".as_ptr(), error_string());
        return -1;
    }
    // Linux struct ifreq has IFNAMSIZ(16) name followed by a 16-byte
    // sockaddr, padded to the size of its largest (24-byte) union member.
    for entry in buf.chunks_exact_mut(40).take(conf.len as usize / 40) {
        if ioctl(fd, 0x8915 as c_ulong, entry.as_mut_ptr()) == -1 { continue; }
        let ip = ptr::read_unaligned(entry.as_ptr().add(20).cast::<u32>());
        let name = entry.as_ptr().cast::<c_char>();
        CON_Printf(6, c"%s: %s\n".as_ptr(), name, inet_ntoa(ip));
        if ip != network32(0x7f000001) {
            MY_ADDR = ip;
            strcpy(ifname, name);
            return 0;
        }
    }
    -1
}
#[cfg(not(target_os = "linux"))]
unsafe fn scan_interface(_fd: c_int, _name: *mut c_char) -> c_int { -1 }

#[no_mangle]
pub unsafe extern "C" fn UDP_Init() -> c_int {
    if COM_CheckParm(c"-noudp".as_ptr()) != 0 { return INVALID_SOCKET; }
    MY_ADDR = network32(0x7f000001);
    let mut hostname = [0 as c_char; 256];
    if gethostname(hostname.as_mut_ptr(), hostname.len()) != 0 {
        CON_Printf(4, c"%s: WARNING: gethostname failed (%s)\n".as_ptr(), c"UDP_Init".as_ptr(), error_string());
    } else {
        hostname[255] = 0;
        let local = gethostbyname(hostname.as_ptr());
        if local.is_null() {
            CON_Printf(4, c"%s: WARNING: gethostbyname failed (%s)\n".as_ptr(), c"UDP_Init".as_ptr(), host_error_string());
        } else if (*local).addr_type != AF_INET {
            CON_Printf(4, c"%s: address from gethostbyname not IPv4\n".as_ptr(), c"UDP_Init".as_ptr());
        } else {
            MY_ADDR = ptr::read_unaligned(*(*local).addresses as *const u32);
        }
    }
    let mut bindip = parm_value(c"-ip".as_ptr());
    if bindip.is_null() { bindip = parm_value(c"-bindip".as_ptr()); }
    if !bindip.is_null() {
        BIND_ADDR = inet_addr(bindip);
        if BIND_ADDR == u32::MAX {
            Sys_Error(c"%s: %s is not a valid IP address".as_ptr(), c"UDP_Init".as_ptr(), bindip);
        }
        CON_Printf(4, c"Binding to IP Interface Address of %s\n".as_ptr(), bindip);
    } else { BIND_ADDR = u32::MAX; }
    let localip = parm_value(c"-localip".as_ptr());
    if !localip.is_null() {
        LOCAL_ADDR = inet_addr(localip);
        if LOCAL_ADDR == u32::MAX {
            Sys_Error(c"%s: %s is not a valid IP address".as_ptr(), c"UDP_Init".as_ptr(), localip);
        }
        CON_Printf(4, c"Advertising %s as the local IP in response packets\n".as_ptr(), localip);
    } else { LOCAL_ADDR = u32::MAX; }
    CONTROL_SOCKET = UDP_OpenSocket(0);
    if CONTROL_SOCKET == INVALID_SOCKET {
        CON_Printf(4, c"%s: Unable to open control socket, UDP disabled\n".as_ptr(), c"UDP_Init".as_ptr());
        return INVALID_SOCKET;
    }
    let mut ifname = [0 as c_char; 16];
    if MY_ADDR == network32(0x7f000001) && scan_interface(CONTROL_SOCKET, ifname.as_mut_ptr()) == 0 {
        CON_Printf(4, c"UDP, Local address: %s (%s)\n".as_ptr(), inet_ntoa(MY_ADDR), ifname.as_ptr());
    }
    if ifname[0] == 0 {
        CON_Printf(4, c"UDP, Local address: %s\n".as_ptr(), inet_ntoa(MY_ADDR));
    }
    BROADCAST_ADDR.family = AF_INET as u16;
    BROADCAST_ADDR.ip = u32::MAX;
    BROADCAST_ADDR.port = network(net_hostport as u16);
    let mut addr = QSockAddr {family: 0, data: [0;14]};
    UDP_GetSocketAddr(CONTROL_SOCKET, &mut addr);
    strcpy(my_tcpip_address.as_mut_ptr(), UDP_AddrToString(&mut addr));
    let colon = strrchr(my_tcpip_address.as_ptr(), ':' as c_int);
    if !colon.is_null() { *colon = 0; }
    CON_Printf(4, c"UDP Initialized\n".as_ptr());
    tcpipAvailable = 1;
    CONTROL_SOCKET
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Shutdown() {
    UDP_Listen(0);
    UDP_CloseSocket(CONTROL_SOCKET);
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Listen(state: c_int) {
    if state != 0 {
        if ACCEPT_SOCKET != INVALID_SOCKET { return; }
        ACCEPT_SOCKET = UDP_OpenSocket(net_hostport);
        if ACCEPT_SOCKET == INVALID_SOCKET { Sys_Error(c"%s: Unable to open accept socket".as_ptr(), c"UDP_Listen".as_ptr()); }
    } else if ACCEPT_SOCKET != INVALID_SOCKET {
        UDP_CloseSocket(ACCEPT_SOCKET);
        ACCEPT_SOCKET = INVALID_SOCKET;
    }
}
#[no_mangle]
pub unsafe extern "C" fn UDP_OpenSocket(port: c_int) -> c_int {
    let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if fd == INVALID_SOCKET {
        CON_Printf(4, c"%s: %s\n".as_ptr(), c"UDP_OpenSocket".as_ptr(), error_string());
        return INVALID_SOCKET;
    }
    let mut yes: c_int = 1;
    let mut addr = empty_addr();
    addr.ip = if BIND_ADDR != u32::MAX { BIND_ADDR } else { 0 };
    addr.port = network(port as u16);
    if ioctl(fd, FIONBIO, &mut yes) != -1 && bind(fd, &addr, 16) == 0 { return fd; }
    CON_Printf(4, c"%s: %s\n".as_ptr(), c"UDP_OpenSocket".as_ptr(), error_string());
    UDP_CloseSocket(fd);
    INVALID_SOCKET
}
#[no_mangle]
pub unsafe extern "C" fn UDP_CloseSocket(fd: c_int) -> c_int {
    if fd == BROADCAST_SOCKET { BROADCAST_SOCKET = 0; }
    close(fd)
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Connect(_fd: c_int, _addr: *mut QSockAddr) -> c_int { 0 }
#[no_mangle]
pub unsafe extern "C" fn UDP_CheckNewConnections() -> c_int {
    if ACCEPT_SOCKET == INVALID_SOCKET { return INVALID_SOCKET; }
    let mut available: c_int = 0;
    if ioctl(ACCEPT_SOCKET, FIONREAD, &mut available) == -1 {
        Sys_Error(c"UDP: ioctlsocket (FIONREAD) failed (%s)".as_ptr(), error_string());
    }
    if available != 0 { return ACCEPT_SOCKET; }
    let mut from = empty_addr();
    let mut fromlen: u32 = 16;
    let mut buf = [0u8;1];
    recvfrom(ACCEPT_SOCKET, buf.as_mut_ptr(), 0, 0, &mut from, &mut fromlen);
    INVALID_SOCKET
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Read(fd: c_int, buf: *mut u8, len: c_int, addr: *mut QSockAddr) -> c_int {
    let mut addrlen = 16u32;
    let ret = recvfrom(fd, buf, len as usize, 0, sockaddr(addr), &mut addrlen) as c_int;
    if ret == -1 {
        let err = *__errno_location();
        if err == EWOULDBLOCK || err == ECONNREFUSED { return 0; }
        CON_Printf(6, c"%s, recvfrom: %s\n".as_ptr(), c"UDP_Read".as_ptr(), strerror(err));
    }
    ret
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Write(fd: c_int, buf: *mut u8, len: c_int, addr: *mut QSockAddr) -> c_int {
    let ret = sendto(fd, buf, len as usize, 0, sockaddr(addr), 16) as c_int;
    if ret == -1 {
        let err = *__errno_location();
        if err == EWOULDBLOCK { return 0; }
        CON_Printf(6, c"%s, sendto: %s\n".as_ptr(), c"UDP_Write".as_ptr(), strerror(err));
    }
    ret
}
#[no_mangle]
pub unsafe extern "C" fn UDP_Broadcast(fd: c_int, buf: *mut u8, len: c_int) -> c_int {
    if fd != BROADCAST_SOCKET {
        if BROADCAST_SOCKET != 0 { Sys_Error(c"Attempted to use multiple broadcasts sockets".as_ptr()); }
        let yes: c_int = 1;
        if setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (&yes as *const c_int).cast(), 4) == -1 {
            CON_Printf(4, c"%s, setsockopt: %s\n".as_ptr(), c"UDP_MakeSocketBroadcastCapable".as_ptr(), error_string());
            CON_Printf(0, c"Unable to make socket broadcast capable\n".as_ptr());
            return -1;
        }
        BROADCAST_SOCKET = fd;
    }
    UDP_Write(fd, buf, len, (&raw mut BROADCAST_ADDR).cast())
}
extern "C" { fn setsockopt(fd: c_int, level: c_int, name: c_int, value: *const c_void, len: u32) -> c_int; }
static mut ADDR_STRING: [c_char; 22] = [0;22];
#[no_mangle]
pub unsafe extern "C" fn UDP_AddrToString(addr: *mut QSockAddr) -> *const c_char {
    let ip = u32::from_be((*sockaddr(addr)).ip);
    q_snprintf(ADDR_STRING.as_mut_ptr(), 22, c"%d.%d.%d.%d:%d".as_ptr(),
        (ip >> 24) as c_int, ((ip >> 16) & 255) as c_int,
        ((ip >> 8) & 255) as c_int, (ip & 255) as c_int,
        host((*sockaddr(addr)).port) as c_int);
    ADDR_STRING.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn UDP_StringToAddr(string: *const c_char, addr: *mut QSockAddr) -> c_int {
    let (mut a,mut b,mut c,mut d,mut port) = (0i32,0i32,0i32,0i32,0i32);
    sscanf(string, c"%d.%d.%d.%d:%d".as_ptr(), &mut a, &mut b, &mut c, &mut d, &mut port);
    (*addr).family = AF_INET as i16;
    (*sockaddr(addr)).ip = network32(((a as u32) << 24) | ((b as u32) << 16) | ((c as u32) << 8) | d as u32);
    (*sockaddr(addr)).port = network(port as u16);
    0
}
#[no_mangle]
pub unsafe extern "C" fn UDP_GetSocketAddr(fd: c_int, addr: *mut QSockAddr) -> c_int {
    ptr::write_bytes(addr, 0, 1);
    let mut addrlen = 16u32;
    getsockname(fd, sockaddr(addr), &mut addrlen);
    let socketaddr = sockaddr(addr);
    if LOCAL_ADDR != u32::MAX { (*socketaddr).ip = LOCAL_ADDR; }
    else if (*socketaddr).ip == 0 || (*socketaddr).ip == network32(0x7f000001) {
        (*socketaddr).ip = MY_ADDR;
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn UDP_GetNameFromAddr(addr: *mut QSockAddr, name: *mut c_char) -> c_int {
    let hostent = gethostbyaddr((&(*sockaddr(addr)).ip as *const u32).cast(), 4, AF_INET);
    if !hostent.is_null() {
        strncpy(name, (*hostent).name, 63);
    } else { strcpy(name, UDP_AddrToString(addr)); }
    0
}
unsafe fn partial_address(name: *const c_char, addr: *mut QSockAddr) -> c_int {
    let input = CStr::from_ptr(name).to_bytes();
    let mut pos = 0usize;
    let mut ip = 0u32;
    let mut mask = !0u32;
    // The C prefixes a dot and drops a leading dot if already present.
    if input.first() == Some(&b'.') { pos += 1; }
    loop {
        let mut num = 0i32;
        let mut run = 0;
        while pos < input.len() && input[pos].is_ascii_digit() {
            num = num.wrapping_mul(10).wrapping_add((input[pos] - b'0') as i32);
            pos += 1;
            run += 1;
            if run > 3 { return -1; }
        }
        if num > 255 { return -1; }
        mask = mask.wrapping_shl(8);
        ip = ip.wrapping_shl(8).wrapping_add(num as u32);
        if pos == input.len() || input[pos] == b':' { break; }
        if input[pos] != b'.' { return -1; }
        pos += 1;
        // A trailing dot is accepted by the C as a zero octet.
    }
    let port = if pos < input.len() && input[pos] == b':' {
        atoi(name.add(pos+1)) as u16
    } else { net_hostport as u16 };
    (*addr).family = AF_INET as i16;
    (*sockaddr(addr)).port = network(port);
    (*sockaddr(addr)).ip = (MY_ADDR & network32(mask)) | network32(ip);
    0
}
#[no_mangle]
pub unsafe extern "C" fn UDP_GetAddrFromName(name: *const c_char, addr: *mut QSockAddr) -> c_int {
    if (*name as u8).is_ascii_digit() { return partial_address(name, addr); }
    let hostent = gethostbyname(name);
    if hostent.is_null() { return -1; }
    (*addr).family = AF_INET as i16;
    (*sockaddr(addr)).port = network(net_hostport as u16);
    (*sockaddr(addr)).ip = ptr::read_unaligned(*(*hostent).addresses as *const u32);
    0
}
#[no_mangle]
pub unsafe extern "C" fn UDP_AddrCompare(a: *mut QSockAddr, b: *mut QSockAddr) -> c_int {
    if (*a).family != (*b).family || (*sockaddr(a)).ip != (*sockaddr(b)).ip { -1 }
    else if (*sockaddr(a)).port != (*sockaddr(b)).port { 1 } else { 0 }
}
#[no_mangle]
pub unsafe extern "C" fn UDP_GetSocketPort(addr: *mut QSockAddr) -> c_int {
    host((*sockaddr(addr)).port) as c_int
}
#[no_mangle]
pub unsafe extern "C" fn UDP_SetSocketPort(addr: *mut QSockAddr, port: c_int) -> c_int {
    (*sockaddr(addr)).port = network(port as u16);
    0
}
