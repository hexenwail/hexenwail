/* Platform facts read by net_udp_h2.rs.
 *
 * The UDP driver's logic, the address conversions and the interface scan all
 * live in Rust.  What cannot is the small set of values the C library decides
 * per platform and net_udp.c reads straight out of the system headers:
 *
 *   - the socket-domain and socket-option constants.  SOL_SOCKET is 1 on
 *     Linux and 0xffff on the BSDs and macOS, SO_BROADCAST is 6 there and
 *     0x20 here, so a hardcoded pair is wrong on one of them;
 *   - the two ioctl request numbers (FIONBIO, FIONREAD), whose encodings are
 *     not the same either;
 *   - the errno values compared against EWOULDBLOCK and ECONNREFUSED, and how
 *     errno and h_errno are reached (glibc hides the latter behind a macro);
 *   - the byte layout of sockaddr_in.  BSD-family structs carry an sa_len
 *     byte, so the family sits one byte further in and is one byte wide
 *     instead of two -- common/net_sys.h names this HAVE_SA_LEN and
 *     SA_FAM_OFFSET -- and engine/hexen2/net_defs.h's struct qsockaddr
 *     mirrors the same split, so the two cast between each other on both;
 *   - whether SIOCGIFCONF/SIOCGIFADDR exist at all, their request numbers,
 *     and the offsets of struct ifreq's name and address members, which are
 *     a different size and shape on Linux and BSD.
 *
 * Returning those from C rather than writing per-OS constant tables in Rust
 * is deliberate: the values then come from the same headers the C original
 * compiled against, so there is no second copy to drift, and a platform
 * nobody thought to enumerate still gets the right numbers.  Nothing here is
 * transport logic -- every function is a one-line read of a macro or an
 * offsetof, and the driver that uses them is entirely in Rust.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"

#include <stddef.h>

#if defined(PLATFORM_UNIX) && !defined(__EMSCRIPTEN__)
/* SIOCGIFCONF/SIOCGIFADDR live in <net/if.h> on the platforms that have them;
 * net_sys.h has already pulled in <sys/ioctl.h>, <sys/socket.h>,
 * <netinet/in.h> and <netdb.h>.  The browser build has no network interface
 * to enumerate and no net/if.h, which is why it is excluded here rather than
 * left to fail. */
#include <net/if.h>
#endif

/* --- socket constants ----------------------------------------------------- */

int H2UDP_AF_INET(void) { return AF_INET; }
int H2UDP_SOCK_DGRAM(void) { return SOCK_DGRAM; }
int H2UDP_IPPROTO_UDP(void) { return IPPROTO_UDP; }
int H2UDP_SOL_SOCKET(void) { return SOL_SOCKET; }
int H2UDP_SO_BROADCAST(void) { return SO_BROADCAST; }

/* --- ioctl requests ------------------------------------------------------- */

unsigned long H2UDP_FIONBIO(void) { return (unsigned long) FIONBIO; }

#if defined(FIONREAD)
unsigned long H2UDP_FIONREAD(void) { return (unsigned long) FIONREAD; }
#else
/* No FIONREAD means CheckNewConnections cannot poll for a pending packet; the
 * Rust side reads 0 as "unsupported" rather than inventing a number. */
unsigned long H2UDP_FIONREAD(void) { return 0; }
#endif

/* --- errno ---------------------------------------------------------------- */

int H2UDP_EWOULDBLOCK(void) { return NET_EWOULDBLOCK; }
int H2UDP_ECONNREFUSED(void) { return NET_ECONNREFUSED; }

int *H2UDP_errno_ptr(void) { return &errno; }

const char *H2UDP_strerror(int err) { return strerror(err); }

/* h_errno is a macro on glibc (*__h_errno_location()) and on the BSDs
 * (*__h_errno()), and some old EMX SDKs have no hstrerror at all -- net_sys.h
 * already defines that fallback, so the whole thing is one call here.  Winsock
 * has neither name, and does not need one: the Hexen II UDP driver is
 * net_win.c there, not this port. */
#if defined(PLATFORM_WINDOWS)
const char *H2UDP_host_error_string(void) { return strerror(errno); }
#elif defined(__EMSCRIPTEN__)
/* The browser build never resolves a host name (there is no DNS), so there is
 * nothing to report through h_errno. */
const char *H2UDP_host_error_string(void) { return strerror(errno); }
#else
const char *H2UDP_host_error_string(void) { return hstrerror(h_errno); }
#endif

/* --- sockaddr_in layout --------------------------------------------------- */

int H2UDP_sockaddr_in_sizeof(void) { return (int) sizeof(struct sockaddr_in); }

int H2UDP_sockaddr_in_family_offset(void)
{
	return (int) offsetof(struct sockaddr_in, sin_family);
}

/* 1 where the family is an sa_len-style unsigned char (BSD, macOS, OS/2,
 * Hurd, Haiku), 2 where it is a short (Linux and friends).  Reading the
 * member's own size is what makes this correct without naming the platform:
 * sa_family_t is unsigned char there and unsigned short here. */
int H2UDP_sockaddr_family_size(void)
{
	return (int) sizeof(((struct sockaddr_in *)0)->sin_family);
}

int H2UDP_sockaddr_in_port_offset(void)
{
	return (int) offsetof(struct sockaddr_in, sin_port);
}

int H2UDP_sockaddr_in_addr_offset(void)
{
	return (int) offsetof(struct sockaddr_in, sin_addr);
}

/* --- interface scan ------------------------------------------------------- */

#if defined(SIOCGIFCONF) && defined(SIOCGIFADDR)
int H2UDP_has_ifscan(void) { return 1; }
unsigned long H2UDP_SIOCGIFCONF(void) { return (unsigned long) SIOCGIFCONF; }
unsigned long H2UDP_SIOCGIFADDR(void) { return (unsigned long) SIOCGIFADDR; }
int H2UDP_ifreq_sizeof(void) { return (int) sizeof(struct ifreq); }
int H2UDP_ifreq_name_offset(void) { return (int) offsetof(struct ifreq, ifr_name); }
int H2UDP_ifreq_addr_offset(void) { return (int) offsetof(struct ifreq, ifr_addr); }
int H2UDP_ifconf_sizeof(void) { return (int) sizeof(struct ifconf); }
int H2UDP_ifconf_len_offset(void) { return (int) offsetof(struct ifconf, ifc_len); }
int H2UDP_ifconf_buf_offset(void) { return (int) offsetof(struct ifconf, ifc_buf); }
#else
int H2UDP_has_ifscan(void) { return 0; }
unsigned long H2UDP_SIOCGIFCONF(void) { return 0; }
unsigned long H2UDP_SIOCGIFADDR(void) { return 0; }
int H2UDP_ifreq_sizeof(void) { return 0; }
int H2UDP_ifreq_name_offset(void) { return 0; }
int H2UDP_ifreq_addr_offset(void) { return 0; }
int H2UDP_ifconf_sizeof(void) { return 0; }
int H2UDP_ifconf_len_offset(void) { return 0; }
int H2UDP_ifconf_buf_offset(void) { return 0; }
#endif
