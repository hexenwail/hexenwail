/*
 * cmd_target.c -- the per-target facts the Rust cmd port needs.
 *
 * Same mechanism as zone_target.c beside it: the per-target C shim decided on
 * #233 (option (a)).  engine/rust is one static library linked into glhexen2,
 * h2ded and hwsv alike, but cmd.c's behaviour differs between them in two
 * ways that a single implementation cannot know by itself:
 *
 *   - the client-only list surface -- ListCommands, ListCvars, ListAlias and
 *     the commands/cmdlist/cvarlist/aliaslist registrations -- is compiled
 *     only when SERVERONLY is not defined (cmd.c:906-1066, :1173-1179), and
 *     registering those four in h2ded or hwsv would give them commands the C
 *     does not have, which Cmd_Exists makes observable from the cvar port;
 *   - the #if defined(H2W) dispatch arm (cmd.c:835) exists only in hwsv, where
 *     it prints the NULL-handler diagnostic instead of calling through it.
 *
 * engine_rs_attach compiles this file into every target with that target's own
 * defines, so one source yields the three correct answers.  It is a migration
 * tool like the C originals the harnesses keep, and it goes away in Phase 11
 * of #61 or when #233 is settled permanently.
 *
 * Per-port shim files rather than one engine-wide one, deliberately: each
 * port's gate can then assert its own shim's presence and per-target values
 * without coupling to another port's changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2026 Hexenwail contributors.
 */

/* Whether this target compiles the client-only command listing surface. */
#if defined(SERVERONLY)

int Cmd_TargetHasClientLists (void)
{
	return 0;
}

#else

int Cmd_TargetHasClientLists (void)
{
	return 1;
}

#endif	/* SERVERONLY */

/* Whether this target is the HexenWorld build -- hwsv, and only hwsv, in this
 * tree: glhexen2 is GLQUAKE + H2W_INTEGRATED and never H2W. */
#if defined(H2W)

int Cmd_TargetIsH2W (void)
{
	return 1;
}

#else

int Cmd_TargetIsH2W (void)
{
	return 0;
}

#endif	/* H2W */
