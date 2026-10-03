// SPDX-License-Identifier: GPL-2.0-or-later
// Rust replacement for engine/hexen2/net_udp.c.
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
//
// Portability: every value this driver used to take from the Linux headers is
// read from the platform through net_udp_h2_target.c -- the socket constants,
// the two ioctl requests, the errno values, and the offset and width of the
// address family inside sockaddr_in, which moves under HAVE_SA_LEN.  There is
// no target_os list here; the only cfg is the macOS hostname workaround, which
// is a deliberate behaviour in the C original rather than an ABI fact.

use core::ffi::{c_char, c_int, c_ulong, c_void, CStr};
use core::ptr;
use crate::net_loop_h2::{QSockAddr, QSOCKADDR_SIZE};

const INVALID_SOCKET: c_int = -1;

#[repr(C)]
struct QuakeParmsC {
    basedir: *mut c_char, userdir: *mut c_char,
    argc: c_int, argv: *mut *mut c_char,
}

#[repr(C)]
struct HostEnt {
    name: *mut c_char, aliases: *mut *mut c_char,
    addr_type: c_int, length: c_int, addresses: *mut *mut u8,
}

/* The platform facts.  See net_udp_h2_target.c for what each one is and why it
 * is a call rather than a constant in this file. */
extern "C" {
    fn H2UDP_AF_INET() -> c_int;
    fn H2UDP_SOCK_DGRAM() -> c_int;
    fn H2UDP_IPPROTO_UDP() -> c_int;
    fn H2UDP_SOL_SOCKET() -> c_int;
    fn H2UDP_SO_BROADCAST() -> c_int;
    fn H2UDP_FIONBIO() -> c_ulong;
    fn H2UDP_FIONREAD() -> c_ulong;
    fn H2UDP_EWOULDBLOCK() -> c_int;
    fn H2UDP_ECONNREFUSED() -> c_int;
    fn H2UDP_errno_ptr() -> *mut c_int;
    fn H2UDP_strerror(err: c_int) -> *const c_char;
    fn H2UDP_host_error_string() -> *const c_char;
    fn H2UDP_sockaddr_in_sizeof() -> c_int;
    fn H2UDP_sockaddr_in_family_offset() -> c_int;
    fn H2UDP_sockaddr_family_size() -> c_int;
    fn H2UDP_sockaddr_in_port_offset() -> c_int;
    fn H2UDP_sockaddr_in_addr_offset() -> c_int;
    fn H2UDP_has_ifscan() -> c_int;
    fn H2UDP_SIOCGIFCONF() -> c_ulong;
    fn H2UDP_SIOCGIFADDR() -> c_ulong;
    fn H2UDP_ifreq_sizeof() -> c_int;
    fn H2UDP_ifreq_name_offset() -> c_int;
    fn H2UDP_ifreq_addr_offset() -> c_int;
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
    fn bind(fd: c_int, addr: *const QSockAddr, len: u32) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn sendto(fd: c_int, buf: *const u8, len: usize, flags: c_int,
        addr: *const QSockAddr, addrlen: u32) -> isize;
    fn recvfrom(fd: c_int, buf: *mut u8, len: usize, flags: c_int,
        addr: *mut QSockAddr, addrlen: *mut u32) -> isize;
    fn getsockname(fd: c_int, addr: *mut QSockAddr, len: *mut u32) -> c_int;
    fn setsockopt(fd: c_int, level: c_int, name: c_int, value: *const c_void, len: u32) -> c_int;
    fn gethostname(buf: *mut c_char, len: usize) -> c_int;
    fn gethostbyname(name: *const c_char) -> *mut HostEnt;
    fn gethostbyaddr(addr: *const u8, len: u32, kind: c_int) -> *mut HostEnt;
    fn inet_addr(name: *const c_char) -> u32;
    fn inet_ntoa(addr: u32) -> *const c_char;
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
static mut BROADCAST_ADDR: QSockAddr = QSockAddr::zeroed();
static mut MY_ADDR: u32 = 0;
static mut LOCAL_ADDR: u32 = !0;
static mut BIND_ADDR: u32 = !0;

/* --- the sockaddr_in / qsockaddr view ------------------------------------ *
 *
 * One 16-byte region, read at the offsets the C headers give.  The family is
 * the only member whose position moves, so it is the only one that needs the
 * width as well as the offset. */
#[inline]
unsafe fn family(addr: *const QSockAddr) -> i16 {
    let p = (*addr).storage.as_ptr().add(H2UDP_sockaddr_in_family_offset() as usize);
    if H2UDP_sockaddr_family_size() == 1 {
        *p as i16
    } else {
        ptr::read_unaligned(p.cast::<i16>())
    }
}
#[inline]
unsafe fn set_family(addr: *mut QSockAddr, value: i16) {
    let p = (*addr).storage.as_mut_ptr().add(H2UDP_sockaddr_in_family_offset() as usize);
    if H2UDP_sockaddr_family_size() == 1 {
        *p = value as u8;
    } else {
        ptr::write_unaligned(p.cast::<i16>(), value);
    }
}
#[inline]
unsafe fn port(addr: *const QSockAddr) -> u16 {
    ptr::read_unaligned((*addr).storage.as_ptr()
        .add(H2UDP_sockaddr_in_port_offset() as usize).cast::<u16>())
}
#[inline]
unsafe fn set_port(addr: *mut QSockAddr, value: u16) {
    ptr::write_unaligned((*addr).storage.as_mut_ptr()
        .add(H2UDP_sockaddr_in_port_offset() as usize).cast::<u16>(), value);
}
#[inline]
unsafe fn ip(addr: *const QSockAddr) -> u32 {
    ptr::read_unaligned((*addr).storage.as_ptr()
        .add(H2UDP_sockaddr_in_addr_offset() as usize).cast::<u32>())
}
#[inline]
unsafe fn set_ip(addr: *mut QSockAddr, value: u32) {
    ptr::write_unaligned((*addr).storage.as_mut_ptr()
        .add(H2UDP_sockaddr_in_addr_offset() as usize).cast::<u32>(), value);
}
#[inline]
unsafe fn sockaddr_addrlen() -> u32 { H2UDP_sockaddr_in_sizeof() as u32 }

/* The C memsets a fresh sockaddr_in and then sets sin_family; sin_len stays
 * zero on the platforms that have one, which is what the original does. */
#[inline]
unsafe fn empty_addr() -> QSockAddr {
    let mut addr = QSockAddr::zeroed();
    set_family(&raw mut addr, H2UDP_AF_INET() as i16);
    addr
}

#[inline]
fn network(v: u16) -> u16 { v.to_be() }
#[inline]
fn host(v: u16) -> u16 { u16::from_be(v) }
#[inline]
fn network32(v: u32) -> u32 { v.to_be() }

#[inline]
unsafe fn error_string() -> *const c_char { H2UDP_strerror(*H2UDP_errno_ptr()) }

#[inline]
unsafe fn parm_value(option: *const c_char) -> *const c_char {
    let i = COM_CheckParm(option);
    if i <= 0 || host_parms.is_null() || i >= (*host_parms).argc - 1 {
        ptr::null()
    } else {
        *(*host_parms).argv.add((i + 1) as usize) as *const c_char
    }
}

/* net_udp.c's PLATFORM_OSX case: a hostname ending in ".local" is a Bonjour
 * name, and gethostbyname() blocks for seconds on one before failing.  The C
 * takes strstr()'s *first* ".local" and requires it to be at the end, which is
 * weaker than a suffix test when the name contains it twice, so this walks to
 * the first occurrence the same way rather than calling ends_with(). */
#[cfg(target_os = "macos")]
unsafe fn hostname_is_local(hostname: *const c_char) -> bool {
    let name = CStr::from_ptr(hostname).to_bytes();
    let mut i = 0usize;
    while i + 6 <= name.len() {
        if &name[i..i + 6] == b".local" { return i + 6 == name.len(); }
        i += 1;
    }
    false
}

// udp_scan_iface: the C's first non-loopback interface selection, guarded by
// SIOCGIFCONF && SIOCGIFADDR in the original.  struct ifreq and the two
// request numbers differ between Linux and BSD; the shim reads both from the
// headers.  Where the platform has no such ioctls -- the browser, Windows --
// H2UDP_has_ifscan() is 0 and this is never entered.
unsafe fn scan_interface(fd: c_int, ifname: *mut c_char) -> c_int {
    if H2UDP_has_ifscan() == 0 { return -1; }
    if COM_CheckParm(c"-noifscan".as_ptr()) != 0 { return -1; }
    let ifreq_size = H2UDP_ifreq_sizeof() as usize;
    let name_offset = H2UDP_ifreq_name_offset() as usize;
    // ifr_addr is a sockaddr; sin_addr sits at the same offset inside it as it
    // does inside sockaddr_in, and is read no further apart than that on the
    // platforms the engine builds for.
    let addr_offset = H2UDP_ifreq_addr_offset() as usize
        + H2UDP_sockaddr_in_addr_offset() as usize;
    #[repr(C)]
    struct IfConf { len: c_int, buf: *mut u8 }
    let mut buf = [0u8; 8192];
    let mut conf = IfConf { len: buf.len() as c_int, buf: buf.as_mut_ptr() };
    if ioctl(fd, H2UDP_SIOCGIFCONF(), &raw mut conf) == -1 {
        CON_Printf(4, c"%s: SIOCGIFCONF failed (%s)\n".as_ptr(),
            c"udp_scan_iface".as_ptr(), error_string());
        return -1;
    }
    for i in 0..(conf.len as usize / ifreq_size) {
        let entry = buf.as_mut_ptr().add(i * ifreq_size);
        if ioctl(fd, H2UDP_SIOCGIFADDR(), entry) == -1 { continue; }
        let entry_ip = ptr::read_unaligned(entry.add(addr_offset).cast::<u32>());
        let name = entry.add(name_offset).cast::<c_char>();
        CON_Printf(6, c"%s: %s\n".as_ptr(), name, inet_ntoa(entry_ip));
        if entry_ip != network32(0x7f000001) {
            MY_ADDR = entry_ip;
            strcpy(ifname, name);
            return 0;
        }
    }
    -1
}

#[no_mangle]
pub unsafe extern "C" fn UDP_Init() -> c_int {
    if COM_CheckParm(c"-noudp".as_ptr()) != 0 { return INVALID_SOCKET; }
    MY_ADDR = network32(0x7f000001);
    let mut hostname = [0 as c_char; 256];
    if gethostname(hostname.as_mut_ptr(), hostname.len()) != 0 {
        CON_Printf(4, c"%s: WARNING: gethostname failed (%s)\n".as_ptr(),
            c"UDP_Init".as_ptr(), error_string());
    } else {
        hostname[255] = 0;
        #[cfg(target_os = "macos")]
        let skip_lookup = hostname_is_local(hostname.as_ptr());
        #[cfg(not(target_os = "macos"))]
        let skip_lookup = false;
        if skip_lookup {
            CON_Printf(4, c"%s: skipping gethostbyname for %s\n".as_ptr(),
                c"UDP_Init".as_ptr(), hostname.as_ptr());
        } else {
            let local = gethostbyname(hostname.as_ptr());
            if local.is_null() {
                CON_Printf(4, c"%s: WARNING: gethostbyname failed (%s)\n".as_ptr(),
                    c"UDP_Init".as_ptr(), H2UDP_host_error_string());
            } else if (*local).addr_type != H2UDP_AF_INET() {
                CON_Printf(4, c"%s: address from gethostbyname not IPv4\n".as_ptr(),
                    c"UDP_Init".as_ptr());
            } else {
                MY_ADDR = ptr::read_unaligned(*(*local).addresses as *const u32);
            }
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
        CON_Printf(4, c"%s: Unable to open control socket, UDP disabled\n".as_ptr(),
            c"UDP_Init".as_ptr());
        return INVALID_SOCKET;
    }
    let mut ifname = [0 as c_char; 16];
    if MY_ADDR == network32(0x7f000001) && scan_interface(CONTROL_SOCKET, ifname.as_mut_ptr()) == 0 {
        CON_Printf(4, c"UDP, Local address: %s (%s)\n".as_ptr(), inet_ntoa(MY_ADDR), ifname.as_ptr());
    }
    if ifname[0] == 0 {
        CON_Printf(4, c"UDP, Local address: %s\n".as_ptr(), inet_ntoa(MY_ADDR));
    }
    set_family(&raw mut BROADCAST_ADDR, H2UDP_AF_INET() as i16);
    set_ip(&raw mut BROADCAST_ADDR, u32::MAX);
    set_port(&raw mut BROADCAST_ADDR, network(net_hostport as u16));
    let mut addr = QSockAddr::zeroed();
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
        if ACCEPT_SOCKET == INVALID_SOCKET {
            Sys_Error(c"%s: Unable to open accept socket".as_ptr(), c"UDP_Listen".as_ptr());
        }
    } else if ACCEPT_SOCKET != INVALID_SOCKET {
        UDP_CloseSocket(ACCEPT_SOCKET);
        ACCEPT_SOCKET = INVALID_SOCKET;
    }
}

#[no_mangle]
pub unsafe extern "C" fn UDP_OpenSocket(port: c_int) -> c_int {
    let fd = socket(H2UDP_AF_INET(), H2UDP_SOCK_DGRAM(), H2UDP_IPPROTO_UDP());
    if fd == INVALID_SOCKET {
        CON_Printf(4, c"%s: %s\n".as_ptr(), c"UDP_OpenSocket".as_ptr(), error_string());
        return INVALID_SOCKET;
    }
    let mut yes: c_int = 1;
    let mut addr = empty_addr();
    set_ip(&raw mut addr, if BIND_ADDR != u32::MAX { BIND_ADDR } else { 0 });
    set_port(&raw mut addr, network(port as u16));
    if ioctl(fd, H2UDP_FIONBIO(), &mut yes) != -1 && bind(fd, &addr, sockaddr_addrlen()) == 0 {
        return fd;
    }
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
    if ioctl(ACCEPT_SOCKET, H2UDP_FIONREAD(), &mut available) == -1 {
        Sys_Error(c"UDP: ioctlsocket (FIONREAD) failed (%s)".as_ptr(), error_string());
    }
    if available != 0 { return ACCEPT_SOCKET; }
    let mut from = empty_addr();
    let mut fromlen: u32 = sockaddr_addrlen();
    let mut buf = [0u8; 1];
    recvfrom(ACCEPT_SOCKET, buf.as_mut_ptr(), 0, 0, &mut from, &mut fromlen);
    INVALID_SOCKET
}

#[no_mangle]
pub unsafe extern "C" fn UDP_Read(fd: c_int, buf: *mut u8, len: c_int, addr: *mut QSockAddr) -> c_int {
    let mut addrlen = sockaddr_addrlen();
    let ret = recvfrom(fd, buf, len as usize, 0, addr, &mut addrlen) as c_int;
    if ret == -1 {
        let err = *H2UDP_errno_ptr();
        if err == H2UDP_EWOULDBLOCK() || err == H2UDP_ECONNREFUSED() { return 0; }
        CON_Printf(6, c"%s, recvfrom: %s\n".as_ptr(), c"UDP_Read".as_ptr(), H2UDP_strerror(err));
    }
    ret
}

#[no_mangle]
pub unsafe extern "C" fn UDP_Write(fd: c_int, buf: *mut u8, len: c_int, addr: *mut QSockAddr) -> c_int {
    let ret = sendto(fd, buf, len as usize, 0, addr, sockaddr_addrlen()) as c_int;
    if ret == -1 {
        let err = *H2UDP_errno_ptr();
        if err == H2UDP_EWOULDBLOCK() { return 0; }
        CON_Printf(6, c"%s, sendto: %s\n".as_ptr(), c"UDP_Write".as_ptr(), H2UDP_strerror(err));
    }
    ret
}

#[no_mangle]
pub unsafe extern "C" fn UDP_Broadcast(fd: c_int, buf: *mut u8, len: c_int) -> c_int {
    if fd != BROADCAST_SOCKET {
        if BROADCAST_SOCKET != 0 {
            Sys_Error(c"Attempted to use multiple broadcasts sockets".as_ptr());
        }
        let yes: c_int = 1;
        if setsockopt(fd, H2UDP_SOL_SOCKET(), H2UDP_SO_BROADCAST(),
                (&yes as *const c_int).cast(), 4) == -1 {
            CON_Printf(4, c"%s, setsockopt: %s\n".as_ptr(),
                c"UDP_MakeSocketBroadcastCapable".as_ptr(), error_string());
            CON_Printf(0, c"Unable to make socket broadcast capable\n".as_ptr());
            return -1;
        }
        BROADCAST_SOCKET = fd;
    }
    UDP_Write(fd, buf, len, &raw mut BROADCAST_ADDR)
}

static mut ADDR_STRING: [c_char; 22] = [0; 22];

#[no_mangle]
pub unsafe extern "C" fn UDP_AddrToString(addr: *mut QSockAddr) -> *const c_char {
    let haddr = u32::from_be(ip(addr));
    q_snprintf(ADDR_STRING.as_mut_ptr(), 22, c"%d.%d.%d.%d:%d".as_ptr(),
        (haddr >> 24) as c_int, ((haddr >> 16) & 255) as c_int,
        ((haddr >> 8) & 255) as c_int, (haddr & 255) as c_int,
        host(port(addr)) as c_int);
    ADDR_STRING.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn UDP_StringToAddr(string: *const c_char, addr: *mut QSockAddr) -> c_int {
    let (mut a, mut b, mut c, mut d, mut p) = (0i32, 0i32, 0i32, 0i32, 0i32);
    sscanf(string, c"%d.%d.%d.%d:%d".as_ptr(), &mut a, &mut b, &mut c, &mut d, &mut p);
    set_family(addr, H2UDP_AF_INET() as i16);
    set_ip(addr, network32(((a as u32) << 24) | ((b as u32) << 16) | ((c as u32) << 8) | d as u32));
    set_port(addr, network(p as u16));
    0
}

#[no_mangle]
pub unsafe extern "C" fn UDP_GetSocketAddr(fd: c_int, addr: *mut QSockAddr) -> c_int {
    ptr::write_bytes((*addr).storage.as_mut_ptr(), 0, QSOCKADDR_SIZE);
    let mut addrlen = sockaddr_addrlen();
    getsockname(fd, addr, &mut addrlen);
    if LOCAL_ADDR != u32::MAX {
        set_ip(addr, LOCAL_ADDR);
    } else if ip(addr) == 0 || ip(addr) == network32(0x7f000001) {
        set_ip(addr, MY_ADDR);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn UDP_GetNameFromAddr(addr: *mut QSockAddr, name: *mut c_char) -> c_int {
    let hostent = gethostbyaddr((*addr).storage.as_ptr()
        .add(H2UDP_sockaddr_in_addr_offset() as usize), 4, H2UDP_AF_INET());
    if !hostent.is_null() {
        strncpy(name, (*hostent).name, 63);
    } else {
        strcpy(name, UDP_AddrToString(addr));
    }
    0
}

unsafe fn partial_address(name: *const c_char, addr: *mut QSockAddr) -> c_int {
    let input = CStr::from_ptr(name).to_bytes();
    let mut pos = 0usize;
    let mut entry_ip = 0u32;
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
        entry_ip = entry_ip.wrapping_shl(8).wrapping_add(num as u32);
        if pos == input.len() || input[pos] == b':' { break; }
        if input[pos] != b'.' { return -1; }
        pos += 1;
        // A trailing dot is accepted by the C as a zero octet.
    }
    let p = if pos < input.len() && input[pos] == b':' {
        atoi(name.add(pos + 1)) as u16
    } else {
        net_hostport as u16
    };
    set_family(addr, H2UDP_AF_INET() as i16);
    set_port(addr, network(p));
    set_ip(addr, (MY_ADDR & network32(mask)) | network32(entry_ip));
    0
}

#[no_mangle]
pub unsafe extern "C" fn UDP_GetAddrFromName(name: *const c_char, addr: *mut QSockAddr) -> c_int {
    if (*name as u8).is_ascii_digit() { return partial_address(name, addr); }
    let hostent = gethostbyname(name);
    if hostent.is_null() { return -1; }
    set_family(addr, H2UDP_AF_INET() as i16);
    set_port(addr, network(net_hostport as u16));
    set_ip(addr, ptr::read_unaligned(*(*hostent).addresses as *const u32));
    0
}

#[no_mangle]
pub unsafe extern "C" fn UDP_AddrCompare(a: *mut QSockAddr, b: *mut QSockAddr) -> c_int {
    if family(a) != family(b) { return -1; }
    if ip(a) != ip(b) { return -1; }
    if port(a) != port(b) { return 1; }
    0
}

#[no_mangle]
pub unsafe extern "C" fn UDP_GetSocketPort(addr: *mut QSockAddr) -> c_int {
    host(port(addr)) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn UDP_SetSocketPort(addr: *mut QSockAddr, port: c_int) -> c_int {
    set_port(addr, network(port as u16));
    0
}
