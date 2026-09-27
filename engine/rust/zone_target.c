/*
 * zone_target.c -- the per-target zone configuration, for the Rust zone port.
 *
 * Why this file exists.  engine/rust is one static library, built once and
 * linked into glhexen2, h2ded and hwsv alike (see #248).  zone.c's behaviour
 * is not the same in all three: ZONE_DEFSIZE is 1 MB under SERVERONLY and 2 MB
 * otherwise, the secondary zone exists only in the client, the cache API is
 * compiled out of the dedicated servers, and isDedicated is a variable in the
 * client and h2ded but a macro in hwsv (hexenworld/server/host.h:50), so it is
 * not even a symbol there.  A single Rust implementation therefore cannot get
 * these four values right by itself.
 *
 * The mechanism is the per-target constant shim decided on #233 (option (a)):
 * this file is compiled into every target with that target's own defines --
 * engine_rs_attach adds it wherever the archive is linked -- so one source
 * yields the three correct answers, and the Rust module asks rather than
 * assumes.  The numbers themselves come from engine/h2shared/zone_target.h,
 * which zone.c also includes, so the shim and the implementation it stands in
 * for cannot drift apart; the differential harness builds both with the same
 * defines for each variant it compares.
 *
 * This is a migration tool, like the C originals the harnesses keep: when the
 * whole engine is Rust (Phase 11 of #61) or when #233 is settled permanently,
 * it goes away with the rest of them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2008-2010  O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "zone_target.h"

/* The main zone's default size: ZONE_DEFSIZE, overridable at runtime by the
 * -zone command line argument, which the Rust module parses for itself. */
int Zone_TargetDefSize (void)
{
	return ZONE_DEFSIZE;
}

/* The secondary zone's size: SECZONE_SIZE, zero in the dedicated servers,
 * where Memory_Init compiles the whole secondary-zone block out. */
int Zone_TargetSecSize (void)
{
	return SECZONE_SIZE;
}

#if defined(SERVERONLY)

/* h2ded and hwsv: no cache API, so Memory_Init must not register the "flush"
 * command either -- Cmd_Exists decides whether a cvar name is refused, so a
 * command that exists in one target and not another is observable. */
int Zone_TargetHasCache (void)
{
	return 0;
}

/* Both SERVERONLY targets are dedicated: h2ded assigns isDedicated = true in
 * its sys_*.c, and hwsv's is the macro 1.  Reading the macro is impossible
 * from Rust and reading a variable is impossible in hwsv, so the answer is
 * taken here, where the target's own defines are in scope. */
int Zone_TargetDedicated (void)
{
	return 1;
}

#else

int Zone_TargetHasCache (void)
{
	return 1;
}

/* The client is not dedicated unless it was started with -dedicated
 * (engine/hexen2/sys_unix.c:790 assigns it before Memory_Init runs), so this
 * has to be a live read, not a constant folded here. */
int Zone_TargetDedicated (void)
{
	extern int isDedicated;	/* qboolean is an int; see common/q_stdinc.h */

	return isDedicated;
}

#endif	/* SERVERONLY */
