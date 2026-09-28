/*
 * net_chan_target.c -- the C-visible half of HexenWorld's channel port.
 *
 * Three jobs, all of which have to be compiled per target because they name
 * something that exists in only some of them:
 *
 * 1. **`net_drop` under the name this target's C expects.**  net.h renames it
 *    to `hw_net_drop` under H2W_INTEGRATED, so hwsv's `sv_user.c` reads
 *    `net_drop` while the client's C would read `hw_net_drop`.  One Rust
 *    module cannot export both, so the storage lives here and the module
 *    reaches it through NetChan_TargetDrop().
 *
 * 2. **`NOT_DEMOPLAYBACK`.**  The C's macro is `(!cls.demoplayback)` in the
 *    client and `true` under SERVERONLY, and `net_chan.c:79` uses it in three
 *    places.  `cls` exists in no dedicated binary, so this cannot be a Rust
 *    read: the predicate below is the whole of it.
 *
 * 3. **`Netchan_OutOfBandPrint`, which is C-variadic.**  Stable Rust cannot
 *    define a C-variadic function, so the signature stays here, formats with
 *    q_vsnprintf, and calls the non-variadic HWNetchan_OutOfBand the module
 *    owns -- the same split quakefs uses for its two path builders.  The name
 *    it must be defined under is per target, which is why both arms are here.
 *
 * h2ded links the archive but builds no HexenWorld channel: its arm of the
 * predicates is a constant, and nothing else here is compiled for it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "q_stdinc.h"
#include "arch_def.h"

#ifdef H2W_INTEGRATED
#include "../hexen2/quakedef.h"
#else
#include "quakedef.h"
#endif

/* netadr_t and the renaming macros the definitions below depend on.  Included
 * by path because h2ded -- which compiles this file only because the archive
 * is linked everywhere -- has neither on its include path and needs neither. */
#if defined(H2W) || defined(H2W_INTEGRATED)
#include "../hexenworld/shared/net.h"
#endif

#if defined(H2W) || defined(H2W_INTEGRATED)

/* The storage, under this target's spelling: hw_net_drop in the client through
 * net.h's macro, net_drop in hwsv as written. */
int	net_drop;

int *NetChan_TargetDrop (void)
{
	return &net_drop;
}

#else

/* h2ded builds no channel; the accessor still has to exist because the Rust
 * module is in the archive it links. */
static int	unused_net_drop;

int *NetChan_TargetDrop (void)
{
	return &unused_net_drop;
}

#endif	/* H2W || H2W_INTEGRATED */

/* NOT_DEMOPLAYBACK, as the C expands it: net_chan.c:76-81. */
int NetChan_TargetNotDemoPlayback (void)
{
#if defined(SERVERONLY)
	return 1;
#else
	return !cls.demoplayback;
#endif
}

/* The one exported entry point Rust cannot define.  It keeps the variadic
 * signature, formats exactly as the C does into a static 8192-byte buffer, and
 * hands the string to the non-variadic send path in the module. */
#if defined(H2W) || defined(H2W_INTEGRATED)

extern void HWNetchan_OutOfBand (const netadr_t *adr, int length, byte *data);

static void NetChan_OutOfBandPrintV (const netadr_t *adr, const char *format,
		va_list argptr)
{
	static char	string[8192];

	q_vsnprintf (string, sizeof (string), format, argptr);
	HWNetchan_OutOfBand (adr, (int)strlen (string), (byte *)string);
}

#if defined(H2W_INTEGRATED)

/* The client's header macro has already renamed its calls to this spelling. */
void HWNetchan_OutOfBandPrint (const netadr_t *adr, const char *format, ...)
{
	va_list		argptr;

	va_start (argptr, format);
	NetChan_OutOfBandPrintV (adr, format, argptr);
	va_end (argptr);
}

#else

/* hwsv calls the plain name. */
void Netchan_OutOfBandPrint (const netadr_t *adr, const char *format, ...)
{
	va_list		argptr;

	va_start (argptr, format);
	NetChan_OutOfBandPrintV (adr, format, argptr);
	va_end (argptr);
}

/* hwsv also calls the plain names of the seven the module exports under the
 * HWNetchan_* spelling; these forward, as net_udp_hw_target.c does for the
 * transport. */
extern void HWNetchan_Init (void);
extern void HWNetchan_Setup (netchan_t *chan, const netadr_t *adr);
extern qboolean HWNetchan_CanPacket (const netchan_t *chan);
extern qboolean HWNetchan_CanReliable (const netchan_t *chan);
extern void HWNetchan_Transmit (netchan_t *chan, int length, byte *data);
extern qboolean HWNetchan_Process (netchan_t *chan);

void Netchan_Init (void) { HWNetchan_Init (); }
void Netchan_Setup (netchan_t *chan, const netadr_t *adr) { HWNetchan_Setup (chan, adr); }
qboolean Netchan_CanPacket (const netchan_t *chan) { return HWNetchan_CanPacket (chan); }
qboolean Netchan_CanReliable (const netchan_t *chan) { return HWNetchan_CanReliable (chan); }
void Netchan_Transmit (netchan_t *chan, int length, byte *data)
{
	HWNetchan_Transmit (chan, length, data);
}
qboolean Netchan_Process (netchan_t *chan) { return HWNetchan_Process (chan); }

/* Netchan_OutOfBand is called directly by hwsv's C as well as through the
 * variadic wrapper above; the module owns the body under the HWNetchan_
 * spelling, so the plain name forwards to it. */
void Netchan_OutOfBand (const netadr_t *adr, int length, byte *data)
{
	HWNetchan_OutOfBand (adr, length, data);
}

#endif	/* H2W_INTEGRATED */

#endif	/* H2W || H2W_INTEGRATED */
