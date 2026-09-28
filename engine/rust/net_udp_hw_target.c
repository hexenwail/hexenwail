/*
 * net_udp_hw_target.c -- the C-visible half of HexenWorld's transport port.
 *
 * Two jobs, both of which have to be compiled per target rather than living in
 * the Rust module:
 *
 * 1. **The exported globals, under the name this target's C expects.**
 *    hexenworld/shared/net.h renames every transport symbol when
 *    H2W_INTEGRATED is defined, so the client's C refers to `hw_net_message`
 *    and `hw_net_from` while hwsv's C refers to `net_message` and `net_from`.
 *    One Rust module cannot export both names conditionally, so the storage
 *    lives here -- including the incantation below, which is what makes the
 *    name come out right per target -- and the Rust reads and writes it
 *    through the accessors at the bottom.
 *
 *    The `#if` around the definitions is load-bearing: h2ded compiles this
 *    file too (engine_rs_attach compiles it wherever the archive is linked)
 *    but builds neither the transport nor the renamed names, and Hexen II's
 *    net_main.c already owns `net_message` there.  Defining them here
 *    unconditionally would be a duplicate symbol in that target.
 *
 * 2. **The plain NET_* names in hwsv.**  The Rust module exports the
 *    `HWNET_*` spelling, which is unambiguous in every target; hwsv's C calls
 *    the plain one, so the thin forwards below are what close the gap.  In the
 *    client the header's macros have already mapped the calls to `HWNET_*`,
 *    so nothing more is needed there.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#ifdef H2W_INTEGRATED
#include "../hexen2/quakedef.h"
#else
#include "quakedef.h"
#endif
/* The H2W header itself, by path rather than by name: the client and hwsv both
 * have it on their include path, but h2ded (which compiles this file only
 * because the archive is linked everywhere) does not, and netadr_t lives there.
 * It also carries the renaming macros job 1 above depends on.
 *
 * h2ded is excluded deliberately: it builds no HexenWorld transport, and Hexen
 * II's own net.h is already in that translation unit declaring the same plain
 * names -- `void NET_Init (void)` against this header's `(int port)` -- so
 * including it there is a type conflict, not a port. */
#if defined(H2W) || defined(H2W_INTEGRATED)
#include "../hexenworld/shared/net.h"
#else
typedef struct
{
	byte		ip[4];
	unsigned short	port;
	unsigned short	pad;
} netadr_t;
#endif

/* The storage, under this target's spelling of the names.  In the client these
 * become hw_net_* through net.h; in hwsv they stay as written. */
#if defined(H2W) || defined(H2W_INTEGRATED)
netadr_t	net_local_adr;
netadr_t	net_loopback_adr;
netadr_t	net_from;
sizebuf_t	net_message;
#endif

/* The accessors the Rust module uses.  Each target gets exactly one definition
 * of each; h2ded builds no transport, so it gets private placeholders rather
 * than aliases of storage the engine owns. */
sizebuf_t *NetUDP_TargetMessageBuf (void)
{
#if defined(H2W) || defined(H2W_INTEGRATED)
	return &net_message;
#else
	static sizebuf_t unused_message;
	return &unused_message;
#endif
}

netadr_t *NetUDP_TargetFrom (void)
{
#if defined(H2W) || defined(H2W_INTEGRATED)
	return &net_from;
#else
	static netadr_t unused_from;
	return &unused_from;
#endif
}

netadr_t *NetUDP_TargetLocalAdr (void)
{
#if defined(H2W) || defined(H2W_INTEGRATED)
	return &net_local_adr;
#else
	static netadr_t unused_local;
	return &unused_local;
#endif
}

netadr_t *NetUDP_TargetLoopbackAdr (void)
{
#if defined(H2W) || defined(H2W_INTEGRATED)
	return &net_loopback_adr;
#else
	static netadr_t unused_loopback;
	return &unused_loopback;
#endif
}

/* hwsv calls the plain names; the client's header macros already spell them
 * HWNET_*.  These forward, and declare the Rust entry points here rather than
 * through a header because the module is the only caller. */
#if defined(H2W)
extern void HWNET_Init (int port);
extern void HWNET_Shutdown (void);
extern int HWNET_GetPacket (void);
extern void HWNET_SendPacket (int length, void *data, const netadr_t *to);
extern int HWNET_CheckReadTimeout (long sec, long usec);
extern qboolean HWNET_CompareAdr (const netadr_t *a, const netadr_t *b);
extern qboolean HWNET_CompareBaseAdr (const netadr_t *a, const netadr_t *b);
extern const char *HWNET_AdrToString (const netadr_t *a);
extern const char *HWNET_BaseAdrToString (const netadr_t *a);
extern qboolean HWNET_StringToAdr (const char *s, netadr_t *a);

void NET_Init (int port) { HWNET_Init (port); }
void NET_Shutdown (void) { HWNET_Shutdown (); }
int NET_GetPacket (void) { return HWNET_GetPacket (); }
void NET_SendPacket (int length, void *data, const netadr_t *to)
{
	HWNET_SendPacket (length, data, to);
}
int NET_CheckReadTimeout (long sec, long usec)
{
	return HWNET_CheckReadTimeout (sec, usec);
}
qboolean NET_CompareAdr (const netadr_t *a, const netadr_t *b)
{
	return HWNET_CompareAdr (a, b);
}
qboolean NET_CompareBaseAdr (const netadr_t *a, const netadr_t *b)
{
	return HWNET_CompareBaseAdr (a, b);
}
const char *NET_AdrToString (const netadr_t *a) { return HWNET_AdrToString (a); }
const char *NET_BaseAdrToString (const netadr_t *a)
{
	return HWNET_BaseAdrToString (a);
}
qboolean NET_StringToAdr (const char *s, netadr_t *a)
{
	return HWNET_StringToAdr (s, a);
}
#endif	/* H2W */
