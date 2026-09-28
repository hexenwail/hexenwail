// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/hexenworld/shared/net_udp.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
// Copyright (C) 2026 Hexenwail contributors.
//
// HexenWorld's transport: the NET_* address and packet API, the single UDP
// socket, and the receive path that decodes through the Huffman codec.  Three
// properties of the C original shape this port.
//
// 1. **It is renamed in the client.**  hexenworld/shared/net.h renames every
//    symbol here when H2W_INTEGRATED is defined -- `net_message` becomes
//    `hw_net_message`, `NET_Init` becomes `HWNET_Init` -- so that the
//    HexenWorld transport can live in one executable beside Hexen II's own
//    qsocket layer.  That is how the two `net_message` definitions in this
//    tree avoid colliding, and it makes the symbol names per target.
//
//    One Rust module cannot export both name sets conditionally, so the split
//    is: **this module exports the `HWNET_*` names, which are unambiguous
//    everywhere** (nothing else in the tree defines them), and the per-target
//    shim engine/rust/net_udp_hw_target.c owns the C-visible storage under the
//    name each target's C expects and, in hwsv, the plain `NET_*` wrappers that
//    forward here.  The module reaches the storage through the shim's
//    accessors rather than naming it, so each global has exactly one
//    definition per target.
//
// 2. **The exported globals are storage, not logic.**  `net_message`,
//    `net_from`, `net_local_adr` and `net_loopback_adr` are read directly by C
//    (`net_chan.c`, `sv_send.c`, `sv_ccmds.c`, the message reader), so the shim
//    owns them.  `net_socket`, `huffbuff` and `net_message_buffer` are `static`
//    in the C and stay private here.
//
// 3. **The browser build does not contain this transport.**  net_udp.c is
//    compiled into glhexen2 and hwsv only -- never h2ded, never Emscripten --
//    so on wasm32 this module compiles to nothing and nothing references it.
//    The `imp` module below is gated accordingly rather than shipping stubs
//    that would pretend to send.
//
// The socket layer is libc on unix and winsock on Windows; `sys` presents both
// through one Rust interface so the logic below is platform-free.  The
// constants are the ones net_sys.h's compatibility macros stand for, each
// commented where it is defined.

const _: () = ();

#[cfg(any(unix, windows))]
mod imp {
    use core::ffi::{c_char, c_int, c_long, c_uint, c_void};

    use crate::sizebuf::SizeBufC;

    /// `quakeparms_t` from engine/hexen2/host.h, only as far as `argc` and
    /// `argv`: `com_argc`/`com_argv` are macros over `host_parms->argc/argv`
    /// (common.h:92-93), so `-ip`/`-bindip` are read through this.  The zone
    /// port has the same struct for `-zone`; the fields are asserted here so
    /// a copy cannot drift silently, which is the same trade the other ports
    /// make when sharing the type would drag a whole module into a harness.
    #[repr(C)]
    pub struct QuakeParmsC {
        pub basedir: *mut c_char,
        pub userdir: *mut c_char,
        pub argc: c_int,
        pub argv: *mut *mut c_char,
    }

    const _: () = {
        assert!(core::mem::offset_of!(QuakeParmsC, basedir) == 0);
        assert!(core::mem::offset_of!(QuakeParmsC, argc) == 2 * core::mem::size_of::<*mut c_char>());
        assert!(
            core::mem::offset_of!(QuakeParmsC, argv)
                == 2 * core::mem::size_of::<*mut c_char>() + 4
                    + (core::mem::align_of::<*mut c_char>()
                        - (2 * core::mem::size_of::<*mut c_char>() + 4)
                            % core::mem::align_of::<*mut c_char>())
                        % core::mem::align_of::<*mut c_char>()
        );
    };

    //========================================================================
    // the C ABI this module speaks
    //========================================================================

    /// `_PRINT_NORMAL` and `_PRINT_SAFE` from engine/h2shared/printsys.h: what
    /// the C's `Con_Printf` and `Con_SafePrintf` macros pass to `CON_Printf`.
    const PRINT_NORMAL: c_uint = 0;
    const PRINT_SAFE: c_uint = 4;

    /// `HWNET_MAX_MSGLEN` from hexenworld/shared/net.h.
    const HWNET_MAX_MSGLEN: usize = 7500;

    /// `MAX_UDP_PACKET` -- one more than the message plus the header, and the
    /// size of both receive buffers.
    const MAX_UDP_PACKET: usize = HWNET_MAX_MSGLEN + 9;

    /// `PORT_ANY`
    const PORT_ANY: c_int = -1;

    /// `MAXHOSTNAMELEN` as the C gets it from the platform's netdb.h; 256 is
    /// what every target this builds for uses.
    const MAXHOSTNAMELEN: usize = 256;

    extern "C" {
        /// The shim's accessors for the C-visible globals.  Naming them here
        /// would put a second definition in whichever target does not expect
        /// it -- see the module header.
        fn NetUDP_TargetMessageBuf() -> *mut SizeBufC;
        fn NetUDP_TargetFrom() -> *mut NetAdrC;
        fn NetUDP_TargetLocalAdr() -> *mut NetAdrC;
        fn NetUDP_TargetLoopbackAdr() -> *mut NetAdrC;

        /// The Rust sizebuf port.
        fn SZ_Init(buf: *mut SizeBufC, data: *mut u8, length: c_int);

        /// The Rust Huffman port: every packet is decoded on the way in and
        /// encoded on the way out.
        fn HuffDecode(
            in_: *const u8,
            out: *mut u8,
            inlen: c_int,
            outlen: *mut c_int,
            maxlen: c_int,
        );
        fn HuffEncode(in_: *const u8, out: *mut u8, inlen: c_int, outlen: *mut c_int);

        /// `Con_Printf`/`Con_SafePrintf` as their macros expand, and the
        /// non-returning fatal path.
        fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
        fn Sys_Error(fmt: *const c_char, ...) -> !;

        /// engine/h2shared/common.c, for `-ip`/`-bindip`.
        fn COM_CheckParm(parm: *const c_char) -> c_int;

        /// libc helpers the C calls directly.
        fn strncpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char;
        fn atoi(s: *const c_char) -> c_int;
        fn sprintf(buf: *mut c_char, fmt: *const c_char, ...) -> c_int;

        /// `com_argc`/`com_argv` are macros over `host_parms`
        /// (common.h:92-93), not symbols.  The struct comes from the zone
        /// port, which already asserts its layout.
        static host_parms: *mut QuakeParmsC;
    }

    /// `netadr_t` from hexenworld/shared/net.h.
    #[repr(C)]
    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    pub struct NetAdrC {
        pub ip: [u8; 4],
        /// Network byte order, as the C stores it.
        pub port: u16,
        pub pad: u16,
    }

    const _: () = {
        assert!(core::mem::offset_of!(NetAdrC, ip) == 0);
        assert!(core::mem::offset_of!(NetAdrC, port) == 4);
        assert!(core::mem::offset_of!(NetAdrC, pad) == 6);
        assert!(core::mem::size_of::<NetAdrC>() == 8);
    };

    /// `static` in the C, so private here.
    static mut NET_SOCKET: sys::Socket = sys::INVALID_SOCKET;
    static mut NET_MESSAGE_BUFFER: [u8; MAX_UDP_PACKET] = [0; MAX_UDP_PACKET];
    static mut HUFFBUFF: [u8; 65536] = [0; 65536];

    /// `int LastCompMessageSize` -- not `static` in the C, but nothing outside
    /// net_udp.c reads it.
    static mut LAST_COMP_MESSAGE_SIZE: c_int = 0;

    //========================================================================
    // the platform socket layer, presented as one interface
    //========================================================================

    #[cfg(unix)]
    mod sys {
        use core::ffi::{c_char, c_int, c_void};

        /// `sys_socket_t` is `int` on unix (net_sys.h:67); `INVALID_SOCKET`
        /// and `SOCKET_ERROR` are -1.
        pub type Socket = c_int;
        pub const INVALID_SOCKET: Socket = -1;
        pub const SOCKET_ERROR: i64 = -1;

        /// `NET_EWOULDBLOCK`/`NET_ECONNREFUSED` are the platform's errno.
        pub const EWOULDBLOCK_ERR: c_int = 11; /* EAGAIN */
        pub const ECONNREFUSED_ERR: c_int = 111;

        const AF_INET: c_int = 2;
        const SOCK_DGRAM: c_int = 2;
        const IPPROTO_UDP: c_int = 17;
        const F_GETFL: c_int = 3;
        const F_SETFL: c_int = 4;
        const O_NONBLOCK: c_int = 0o4000;

        /// `struct sockaddr_in` as every unix in this tree defines it.
        #[repr(C)]
        #[derive(Clone, Copy)]
        pub struct SockAddrIn {
            pub sin_family: u16,
            pub sin_port: u16,
            pub sin_addr: u32,
            pub sin_zero: [u8; 8],
        }

        #[repr(C)]
        struct TimeVal {
            tv_sec: i64,
            tv_usec: i64,
        }

        /// `struct hostent`, of which only `h_addr_list[0]` is read.
        #[repr(C)]
        pub struct HostEnt {
            pub h_name: *mut c_char,
            pub h_aliases: *mut *mut c_char,
            pub h_addrtype: c_int,
            pub h_length: c_int,
            pub h_addr_list: *mut *mut u8,
        }

        extern "C" {
            fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
            fn bind(fd: c_int, addr: *const SockAddrIn, len: u32) -> c_int;
            fn recvfrom(
                fd: c_int,
                buf: *mut c_void,
                len: usize,
                flags: c_int,
                addr: *mut SockAddrIn,
                addrlen: *mut u32,
            ) -> isize;
            fn sendto(
                fd: c_int,
                buf: *const c_void,
                len: usize,
                flags: c_int,
                addr: *const SockAddrIn,
                addrlen: u32,
            ) -> isize;
            fn getsockname(fd: c_int, addr: *mut SockAddrIn, addrlen: *mut u32) -> c_int;
            fn gethostname(name: *mut c_char, len: usize) -> c_int;
            fn gethostbyname(name: *const c_char) -> *mut HostEnt;
            fn inet_addr(cp: *const c_char) -> u32;
            fn inet_ntoa(in_: u32) -> *mut c_char;
            fn htons(v: u16) -> u16;
            fn ntohs(v: u16) -> u16;
            fn htonl(v: u32) -> u32;
            fn fcntl(fd: c_int, cmd: c_int, arg: c_int) -> c_int;
            fn close(fd: c_int) -> c_int;
            fn __errno_location() -> *mut c_int;
            fn strerror(errnum: c_int) -> *mut c_char;
            fn select(
                nfds: c_int,
                readfds: *mut u8,
                writefds: *mut u8,
                exceptfds: *mut u8,
                timeout: *mut TimeVal,
            ) -> c_int;
        }

        pub const AF_INET_FAMILY: u16 = AF_INET as u16;
        pub const PORT_ANY_VALUE: u16 = 0;

        pub unsafe fn open_socket() -> Socket {
            socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        }

        /// The C's `ioctlsocket(fd, FIONBIO, &true)`: the platform's spelling
        /// of "make this socket non-blocking".
        pub unsafe fn set_nonblocking(fd: Socket) -> bool {
            fcntl(fd, F_GETFL, 0) >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) >= 0
        }

        pub unsafe fn bind_socket(fd: Socket, addr: &SockAddrIn) -> bool {
            bind(fd, addr, core::mem::size_of::<SockAddrIn>() as u32) == 0
        }

        pub unsafe fn recv_from(fd: Socket, buf: *mut u8, len: usize, from: &mut SockAddrIn) -> i64 {
            let mut fromlen: u32 = core::mem::size_of::<SockAddrIn>() as u32;
            recvfrom(fd, buf as *mut c_void, len, 0, from, &mut fromlen) as i64
        }

        pub unsafe fn send_to(fd: Socket, buf: *const u8, len: usize, to: &SockAddrIn) -> i64 {
            sendto(
                fd,
                buf as *const c_void,
                len,
                0,
                to,
                core::mem::size_of::<SockAddrIn>() as u32,
            ) as i64
        }

        pub unsafe fn get_sock_name(fd: Socket, out: &mut SockAddrIn) -> bool {
            let mut len: u32 = core::mem::size_of::<SockAddrIn>() as u32;
            getsockname(fd, out, &mut len) == 0
        }

        pub unsafe fn host_name(buf: &mut [u8]) -> bool {
            gethostname(buf.as_mut_ptr() as *mut c_char, buf.len()) == 0
        }

        pub unsafe fn host_by_name(name: *const c_char) -> Option<u32> {
            let h = gethostbyname(name);
            if h.is_null() || (*h).h_addr_list.is_null() || (*(*h).h_addr_list).is_null() {
                return None;
            }
            Some(core::ptr::read_unaligned(*(*h).h_addr_list as *const u32))
        }

        pub unsafe fn addr_from_string(s: *const c_char) -> u32 {
            inet_addr(s)
        }

        pub unsafe fn addr_to_string(a: u32) -> *const c_char {
            inet_ntoa(a) as *const c_char
        }

        pub unsafe fn to_network_short(v: u16) -> u16 {
            htons(v)
        }

        pub unsafe fn from_network_short(v: u16) -> u16 {
            ntohs(v)
        }

        pub unsafe fn to_network_long(v: u32) -> u32 {
            htonl(v)
        }

        pub unsafe fn close_socket(fd: Socket) {
            close(fd);
        }

        pub unsafe fn last_error() -> c_int {
            *__errno_location()
        }

        pub unsafe fn error_string(err: c_int) -> *const c_char {
            strerror(err) as *const c_char
        }

        /// `select` on the one socket.  glibc's `fd_set` is a 1024-bit set and
        /// `FD_SETSIZE` is 1024, which is what the C's FD_ZERO/FD_SET write
        /// into.
        pub unsafe fn wait_readable(fd: Socket, sec: i64, usec: i64) -> c_int {
            let mut set = [0u8; 128];
            if fd >= 0 {
                let bit = fd as usize;
                set[bit / 8] |= 1 << (bit % 8);
            }
            let mut timeout = TimeVal {
                tv_sec: sec,
                tv_usec: usec,
            };
            select(
                fd + 1,
                set.as_mut_ptr(),
                core::ptr::null_mut(),
                core::ptr::null_mut(),
                &mut timeout,
            )
        }

        /// Winsock-only on Windows; nothing to do on unix.
        pub fn startup() -> c_int {
            0
        }

        pub fn cleanup() {}
    }

    #[cfg(windows)]
    mod sys {
        use core::ffi::{c_char, c_int, c_void};

        /// On Windows a socket is a `UINT_PTR`, and `INVALID_SOCKET` is
        /// `(SOCKET)(~0)`; the C's `SOCKETERRNO` is `WSAGetLastError()`.
        pub type Socket = usize;
        pub const INVALID_SOCKET: Socket = !0usize;
        pub const SOCKET_ERROR: i64 = -1;

        pub const EWOULDBLOCK_ERR: c_int = 10035; /* WSAEWOULDBLOCK */
        pub const ECONNREFUSED_ERR: c_int = 10061; /* WSAECONNREFUSED */
        pub const EMSGSIZE_ERR: c_int = 10040; /* WSAEMSGSIZE */
        pub const ECONNRESET_ERR: c_int = 10054; /* WSAECONNRESET */

        const AF_INET: c_int = 2;
        const SOCK_DGRAM: c_int = 2;
        const IPPROTO_UDP: c_int = 17;
        const FIONBIO: i32 = -2147195266; /* 0x8004667e */

        #[repr(C)]
        #[derive(Clone, Copy)]
        pub struct SockAddrIn {
            pub sin_family: u16,
            pub sin_port: u16,
            pub sin_addr: u32,
            pub sin_zero: [u8; 8],
        }

        #[repr(C)]
        pub struct HostEnt {
            pub h_name: *mut c_char,
            pub h_aliases: *mut *mut c_char,
            pub h_addrtype: c_int,
            pub h_length: c_int,
            pub h_addr_list: *mut *mut u8,
        }

        /// `WSADATA`, which the C only ever passes a pointer to.
        #[repr(C)]
        pub struct WsaData {
            pub data: [u8; 512],
        }

        #[repr(C)]
        struct WsaPollFd {
            fd: Socket,
            events: i16,
            revents: i16,
        }

        extern "system" {
            fn socket(domain: c_int, ty: c_int, protocol: c_int) -> usize;
            fn bind(fd: usize, addr: *const SockAddrIn, len: c_int) -> c_int;
            fn recvfrom(
                fd: usize,
                buf: *mut c_char,
                len: c_int,
                flags: c_int,
                addr: *mut SockAddrIn,
                addrlen: *mut c_int,
            ) -> c_int;
            fn sendto(
                fd: usize,
                buf: *const c_char,
                len: c_int,
                flags: c_int,
                addr: *const SockAddrIn,
                addrlen: c_int,
            ) -> c_int;
            fn getsockname(fd: usize, addr: *mut SockAddrIn, addrlen: *mut c_int) -> c_int;
            fn gethostname(name: *mut c_char, len: c_int) -> c_int;
            fn gethostbyname(name: *const c_char) -> *mut HostEnt;
            fn inet_addr(cp: *const c_char) -> u32;
            fn inet_ntoa(in_: u32) -> *mut c_char;
            fn htons(v: u16) -> u16;
            fn ntohs(v: u16) -> u16;
            fn htonl(v: u32) -> u32;
            fn ioctlsocket(fd: usize, cmd: i32, argp: *mut u32) -> c_int;
            fn closesocket(fd: usize) -> c_int;
            fn WSAGetLastError() -> c_int;
            fn WSAPoll(fds: *mut WsaPollFd, count: u32, timeout: c_int) -> c_int;
            fn WSAStartup(version: u16, data: *mut WsaData) -> c_int;
            fn WSACleanup() -> c_int;
            fn strerror(errnum: c_int) -> *mut c_char;
        }

        pub const AF_INET_FAMILY: u16 = AF_INET as u16;
        pub const PORT_ANY_VALUE: u16 = 0;

        pub unsafe fn open_socket() -> Socket {
            socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        }

        pub unsafe fn set_nonblocking(fd: Socket) -> bool {
            let mut one: u32 = 1;
            ioctlsocket(fd, FIONBIO, &mut one) == 0
        }

        pub unsafe fn bind_socket(fd: Socket, addr: &SockAddrIn) -> bool {
            bind(fd, addr, core::mem::size_of::<SockAddrIn>() as c_int) == 0
        }

        pub unsafe fn recv_from(fd: Socket, buf: *mut u8, len: usize, from: &mut SockAddrIn) -> i64 {
            let mut fromlen: c_int = core::mem::size_of::<SockAddrIn>() as c_int;
            recvfrom(
                fd,
                buf as *mut c_char,
                len as c_int,
                0,
                from,
                &mut fromlen,
            ) as i64
        }

        pub unsafe fn send_to(fd: Socket, buf: *const u8, len: usize, to: &SockAddrIn) -> i64 {
            sendto(
                fd,
                buf as *const c_char,
                len as c_int,
                0,
                to,
                core::mem::size_of::<SockAddrIn>() as c_int,
            ) as i64
        }

        pub unsafe fn get_sock_name(fd: Socket, out: &mut SockAddrIn) -> bool {
            let mut len: c_int = core::mem::size_of::<SockAddrIn>() as c_int;
            getsockname(fd, out, &mut len) == 0
        }

        pub unsafe fn host_name(buf: &mut [u8]) -> bool {
            gethostname(buf.as_mut_ptr() as *mut c_char, buf.len() as c_int) == 0
        }

        pub unsafe fn host_by_name(name: *const c_char) -> Option<u32> {
            let h = gethostbyname(name);
            if h.is_null() || (*h).h_addr_list.is_null() || (*(*h).h_addr_list).is_null() {
                return None;
            }
            Some(core::ptr::read_unaligned(*(*h).h_addr_list as *const u32))
        }

        pub unsafe fn addr_from_string(s: *const c_char) -> u32 {
            inet_addr(s)
        }

        pub unsafe fn addr_to_string(a: u32) -> *const c_char {
            inet_ntoa(a) as *const c_char
        }

        pub unsafe fn to_network_short(v: u16) -> u16 {
            htons(v)
        }

        pub unsafe fn from_network_short(v: u16) -> u16 {
            ntohs(v)
        }

        pub unsafe fn to_network_long(v: u32) -> u32 {
            htonl(v)
        }

        pub unsafe fn close_socket(fd: Socket) {
            closesocket(fd);
        }

        pub unsafe fn last_error() -> c_int {
            WSAGetLastError()
        }

        pub unsafe fn error_string(err: c_int) -> *const c_char {
            strerror(err) as *const c_char
        }

        /// Winsock's `fd_set` is a count plus an array, so the portable call is
        /// `WSAPoll`; the return value is what the C's `select` returns.
        pub unsafe fn wait_readable(fd: Socket, sec: i64, usec: i64) -> c_int {
            if fd == INVALID_SOCKET {
                return -1;
            }
            let mut pfd = WsaPollFd {
                fd,
                events: 0x0100, /* POLLRDNORM */
                revents: 0,
            };
            WSAPoll(&mut pfd, 1, (sec * 1000 + usec / 1000) as c_int)
        }

        pub unsafe fn startup() -> c_int {
            let mut data: WsaData = core::mem::zeroed();
            WSAStartup(0x0101, &mut data)
        }

        pub unsafe fn cleanup() {
            WSACleanup();
        }
    }

    //========================================================================
    // address conversion
    //========================================================================

    /// `NetadrToSockadr`
    unsafe fn netadr_to_sockadr(a: *const NetAdrC, s: &mut sys::SockAddrIn) {
        *s = core::mem::zeroed();
        s.sin_family = sys::AF_INET_FAMILY;
        core::ptr::copy_nonoverlapping(a.cast::<u8>(), &mut s.sin_addr as *mut u32 as *mut u8, 4);
        s.sin_port = (*a).port;
    }

    /// `SockadrToNetadr`
    unsafe fn sockadr_to_netadr(s: &sys::SockAddrIn, a: *mut NetAdrC) {
        core::ptr::copy_nonoverlapping(
            &s.sin_addr as *const u32 as *const u8,
            (*a).ip.as_mut_ptr(),
            4,
        );
        (*a).port = s.sin_port;
    }

    /// `NET_CompareBaseAdr` -- the four address bytes, no port.
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_CompareBaseAdr(a: *const NetAdrC, b: *const NetAdrC) -> c_int {
        ((*a).ip == (*b).ip) as c_int
    }

    /// `NET_CompareAdr` -- address and port.
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_CompareAdr(a: *const NetAdrC, b: *const NetAdrC) -> c_int {
        ((*a).ip == (*b).ip && (*a).port == (*b).port) as c_int
    }

    /// `NET_AdrToString` -- into a `static char[64]`, as the C does, because
    /// callers print the returned pointer later.
    static mut ADR_STRING: [c_char; 64] = [0; 64];
    static mut BASE_STRING: [c_char; 64] = [0; 64];

    #[no_mangle]
    pub unsafe extern "C" fn HWNET_AdrToString(a: *const NetAdrC) -> *const c_char {
        let s = (&raw mut ADR_STRING).cast::<c_char>();
        // Ports are stored in network order, so ntohs is what makes this print
        // as a port number.
        sprintf(
            s,
            c"%i.%i.%i.%i:%i".as_ptr(),
            (*a).ip[0] as c_int,
            (*a).ip[1] as c_int,
            (*a).ip[2] as c_int,
            (*a).ip[3] as c_int,
            sys::from_network_short((*a).port) as c_int,
        );
        s
    }

    /// `NET_BaseAdrToString`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_BaseAdrToString(a: *const NetAdrC) -> *const c_char {
        let s = (&raw mut BASE_STRING).cast::<c_char>();
        sprintf(
            s,
            c"%i.%i.%i.%i".as_ptr(),
            (*a).ip[0] as c_int,
            (*a).ip[1] as c_int,
            (*a).ip[2] as c_int,
            (*a).ip[3] as c_int,
        );
        s
    }

    /// `NET_StringToAdr` -- "host", "host:port", "1.2.3.4" or "1.2.3.4:port".
    ///
    /// The C walks the whole copy looking for ':' and takes the last one (the
    /// loop does not break), so "a:b:c" splits on the final colon.  That is
    /// kept.
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_StringToAdr(s: *const c_char, a: *mut NetAdrC) -> c_int {
        let mut sadr: sys::SockAddrIn = core::mem::zeroed();
        let mut copy = [0 as c_char; 128];

        sadr.sin_family = sys::AF_INET_FAMILY;
        sadr.sin_port = 0;

        strncpy(copy.as_mut_ptr(), s, 127);
        copy[127] = 0;

        let mut i = 0usize;
        while copy[i] != 0 {
            if copy[i] == b':' as c_char {
                copy[i] = 0;
                sadr.sin_port = sys::to_network_short(atoi(copy.as_ptr().add(i + 1)) as u16);
            }
            i += 1;
        }

        if (copy[0] as u8).is_ascii_digit() {
            sadr.sin_addr = sys::addr_from_string(copy.as_ptr());
        } else {
            match sys::host_by_name(copy.as_ptr()) {
                Some(addr) => sadr.sin_addr = addr,
                None => return 0,
            }
        }

        sockadr_to_netadr(&sadr, a);
        1
    }

    //========================================================================
    // the transport
    //========================================================================

    /// `NET_GetPacket`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_GetPacket() -> c_int {
        let mut from: sys::SockAddrIn = core::mem::zeroed();

        let ret = sys::recv_from(
            NET_SOCKET,
            (&raw mut HUFFBUFF).cast::<u8>(),
            MAX_UDP_PACKET,
            &mut from,
        );

        if ret == sys::SOCKET_ERROR {
            let err = sys::last_error();
            if err == sys::EWOULDBLOCK_ERR {
                return 0;
            }
            if err == sys::ECONNREFUSED_ERR {
                CON_Printf(
                    PRINT_NORMAL,
                    c"%s: Connection refused\n".as_ptr(),
                    c"NET_GetPacket".as_ptr(),
                );
                return 0;
            }
            #[cfg(windows)]
            {
                if err == sys::EMSGSIZE_ERR {
                    CON_Printf(
                        PRINT_NORMAL,
                        c"Oversize packet from %s\n".as_ptr(),
                        HWNET_AdrToString(NetUDP_TargetFrom()),
                    );
                    return 0;
                }
                if err == sys::ECONNRESET_ERR {
                    CON_Printf(
                        PRINT_NORMAL,
                        c"Connection reset by peer %s\n".as_ptr(),
                        HWNET_AdrToString(NetUDP_TargetFrom()),
                    );
                    return 0;
                }
            }
            Sys_Error(
                c"%s: %s".as_ptr(),
                c"NET_GetPacket".as_ptr(),
                sys::error_string(err),
            );
        }

        sockadr_to_netadr(&from, NetUDP_TargetFrom());

        if ret as usize == MAX_UDP_PACKET {
            CON_Printf(
                PRINT_NORMAL,
                c"Oversize packet from %s\n".as_ptr(),
                HWNET_AdrToString(NetUDP_TargetFrom()),
            );
            return 0;
        }

        LAST_COMP_MESSAGE_SIZE += ret as c_int;

        let mut outlen: c_int = ret as c_int;
        HuffDecode(
            (&raw const HUFFBUFF).cast::<u8>(),
            (&raw mut NET_MESSAGE_BUFFER).cast::<u8>(),
            ret as c_int,
            &mut outlen,
            MAX_UDP_PACKET as c_int,
        );
        if outlen as usize > MAX_UDP_PACKET {
            CON_Printf(
                PRINT_NORMAL,
                c"Oversize compressed data from %s\n".as_ptr(),
                HWNET_AdrToString(NetUDP_TargetFrom()),
            );
            return 0;
        }

        (*NetUDP_TargetMessageBuf()).cursize = outlen;
        outlen
    }

    /// `NET_SendPacket`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_SendPacket(length: c_int, data: *mut c_void, to: *const NetAdrC) {
        let mut addr: sys::SockAddrIn = core::mem::zeroed();
        let mut outlen: c_int = 0;

        netadr_to_sockadr(to, &mut addr);
        HuffEncode(
            data.cast::<u8>(),
            (&raw mut HUFFBUFF).cast::<u8>(),
            length,
            &mut outlen,
        );

        let ret = sys::send_to(
            NET_SOCKET,
            (&raw const HUFFBUFF).cast::<u8>(),
            outlen as usize,
            &addr,
        );
        if ret == sys::SOCKET_ERROR {
            let err = sys::last_error();
            if err == sys::EWOULDBLOCK_ERR {
                return;
            }
            if err == sys::ECONNREFUSED_ERR {
                CON_Printf(
                    PRINT_NORMAL,
                    c"%s: Connection refused\n".as_ptr(),
                    c"NET_SendPacket".as_ptr(),
                );
                return;
            }
            CON_Printf(
                PRINT_NORMAL,
                c"%s ERROR: %s\n".as_ptr(),
                c"NET_SendPacket".as_ptr(),
                sys::error_string(err),
            );
        }
    }

    /// `NET_CheckReadTimeout`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_CheckReadTimeout(sec: c_long, usec: c_long) -> c_int {
        sys::wait_readable(NET_SOCKET, sec as i64, usec as i64)
    }

    /// `UDP_OpenSocket`
    unsafe fn udp_open_socket(port: c_int) -> sys::Socket {
        let newsocket = sys::open_socket();
        if newsocket == sys::INVALID_SOCKET {
            let err = sys::last_error();
            Sys_Error(
                c"%s: socket: %s".as_ptr(),
                c"UDP_OpenSocket".as_ptr(),
                sys::error_string(err),
            );
        }

        if !sys::set_nonblocking(newsocket) {
            let err = sys::last_error();
            Sys_Error(
                c"%s: ioctl FIONBIO: %s".as_ptr(),
                c"UDP_OpenSocket".as_ptr(),
                sys::error_string(err),
            );
        }

        let mut address: sys::SockAddrIn = core::mem::zeroed();
        address.sin_family = sys::AF_INET_FAMILY;

        // `-ip` / `-bindip`, through the same COM_CheckParm the C uses.
        let mut i = COM_CheckParm(c"-ip".as_ptr());
        if i == 0 {
            i = COM_CheckParm(c"-bindip".as_ptr());
        }
        let argc = (*host_parms).argc;
        let argv = (*host_parms).argv;
        if i != 0 && i < argc - 1 {
            let arg = *argv.add((i + 1) as usize);
            address.sin_addr = sys::addr_from_string(arg);
            if address.sin_addr == 0xffff_ffff {
                Sys_Error(c"%s is not a valid IP address".as_ptr(), arg);
            }
            CON_Printf(
                PRINT_NORMAL,
                c"Binding to IP Interface Address of %s\n".as_ptr(),
                sys::addr_to_string(address.sin_addr),
            );
        } else {
            address.sin_addr = 0; /* INADDR_ANY */
        }

        if port == PORT_ANY {
            address.sin_port = sys::PORT_ANY_VALUE;
        } else {
            address.sin_port = sys::to_network_short(port as u16);
        }

        if !sys::bind_socket(newsocket, &address) {
            let err = sys::last_error();
            Sys_Error(
                c"%s: bind: %s".as_ptr(),
                c"UDP_OpenSocket".as_ptr(),
                sys::error_string(err),
            );
        }

        newsocket
    }

    /// `NET_GetLocalAddress` -- `static` in the C, so private here.
    unsafe fn net_get_local_address() {
        let mut buff = [0u8; MAXHOSTNAMELEN];
        let mut address: sys::SockAddrIn = core::mem::zeroed();

        if !sys::host_name(&mut buff) {
            let err = sys::last_error();
            Sys_Error(
                c"%s: gethostname: %s".as_ptr(),
                c"NET_GetLocalAddress".as_ptr(),
                sys::error_string(err),
            );
        }
        buff[MAXHOSTNAMELEN - 1] = 0;

        HWNET_StringToAdr(buff.as_ptr() as *const c_char, NetUDP_TargetLocalAdr());

        if !sys::get_sock_name(NET_SOCKET, &mut address) {
            let err = sys::last_error();
            Sys_Error(
                c"%s: getsockname: %s".as_ptr(),
                c"NET_GetLocalAddress".as_ptr(),
                sys::error_string(err),
            );
        }
        (*NetUDP_TargetLocalAdr()).port = address.sin_port;

        CON_Printf(
            PRINT_SAFE,
            c"IP address %s\n".as_ptr(),
            HWNET_AdrToString(NetUDP_TargetLocalAdr()),
        );
    }

    /// `NET_Init`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_Init(port: c_int) {
        let err = sys::startup();
        if err != 0 {
            Sys_Error(
                c"Winsock initialization failed (%s)".as_ptr(),
                sys::error_string(err),
            );
        }

        // the single socket used for all communication
        NET_SOCKET = udp_open_socket(port);

        // the message buffer, and the sizebuf that reads out of it
        SZ_Init(
            NetUDP_TargetMessageBuf(),
            (&raw mut NET_MESSAGE_BUFFER).cast::<u8>(),
            MAX_UDP_PACKET as c_int,
        );

        net_get_local_address();

        let loopback = sys::to_network_long(0x7f00_0001); /* htonl(INADDR_LOOPBACK) */
        core::ptr::write_bytes(
            NetUDP_TargetLoopbackAdr().cast::<u8>(),
            0,
            core::mem::size_of::<NetAdrC>(),
        );
        core::ptr::copy_nonoverlapping(
            &loopback as *const u32 as *const u8,
            (*NetUDP_TargetLoopbackAdr()).ip.as_mut_ptr(),
            4,
        );

        CON_Printf(PRINT_SAFE, c"UDP Initialized\n".as_ptr());
    }

    /// `NET_Shutdown`
    #[no_mangle]
    pub unsafe extern "C" fn HWNET_Shutdown() {
        if NET_SOCKET != sys::INVALID_SOCKET {
            sys::close_socket(NET_SOCKET);
            NET_SOCKET = sys::INVALID_SOCKET;
        }
        sys::cleanup();
    }
} // mod imp

#[cfg(any(unix, windows))]
pub use imp::*;
