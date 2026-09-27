/*
 * quakefs_target.c -- the per-target facts and behaviour hooks the Rust
 * quakefs port needs.
 *
 * The mechanism is the per-target C shim decided on #233 (option (a)) and
 * already carried by zone_target.c and cmd_target.c.  One static library is
 * linked into glhexen2, h2ded and hwsv alike, but quakefs.c's client half
 * calls symbols that exist in no dedicated binary at all -- Host_ClearMemory,
 * CL_Disconnect, Draw_ReInit, TexMgr_NewGame, BGM_Stop, M_BuildBindList,
 * VID_Lock, Cmd_StartupScript, Con_ShowList and the client_state_t `cls` -- so
 * a Rust translation that named them directly would be one archive with
 * undefined references in h2ded and hwsv.  Every one of those sites is a
 * small localised statement, so each gets a hook here: this file is compiled
 * per target with that target's own defines and may name target-only symbols
 * freely, and the Rust module calls the hook instead.
 *
 * Which sites, exactly (history/rust_phase6_quakefs_ownership.md has the full
 * matrix): Host_Game_f's client-only tail (quakefs.c:3146-3170 and :3239-3266),
 * the four console listers that print through Con_ShowList (:2696, :2973,
 * :3018) and the two predicates the Rust needs to know which target it is
 * compiled into.  Cache_Flush and Cache_Alloc are deliberately NOT hooked:
 * they are the zone port's Rust symbols now and are defined in every binary,
 * and on an empty cache a flush is a no-op, so the dedicated targets'
 * "compiled out" arms stay inert rather than needing a shim.
 *
 * A migration tool like the C originals the harnesses keep; it goes away in
 * Phase 11 of #61 or when #233 is settled permanently.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2026 Hexenwail contributors.
 */

/* The same includes quakefs.c takes, minus the platform ones: the hooks below
 * name symbols quakedef.h does not pull in on its own (BGM_Stop lives in
 * bgmusic.h), and the predicates are written against the same world the file
 * they stand in for is compiled in. */
#include "quakedef.h"
#include "pakfile.h"
#include "bgmusic.h"
#include "filenames.h"

/*============================================================================
 * predicates
 *==========================================================================*/

/* Whether this target is the H2W build -- hwsv, and only hwsv in this tree:
 * glhexen2 is GLQUAKE + H2W_INTEGRATED and never H2W. */
#if defined(H2W)

int QuakeFS_TargetIsH2W (void)
{
	return 1;
}

#else

int QuakeFS_TargetIsH2W (void)
{
	return 0;
}

#endif	/* H2W */

/* Whether this target compiles the client-only command surface -- the four
 * listers and their registrations, which are !SERVERONLY in quakefs.c.  A
 * command that exists in one target and not another is observable through
 * Cmd_Exists, so the Rust registers them only where this says so. */
#if defined(SERVERONLY)

int QuakeFS_TargetHasClientCommands (void)
{
	return 0;
}

#else

int QuakeFS_TargetHasClientCommands (void)
{
	return 1;
}

#endif	/* SERVERONLY */

/* Whether this target is the H2W-integrated *client* -- H2W_INTEGRATED and
 * neither H2W nor SERVERONLY, which in this tree means native glhexen2 alone.
 * The wasm client does not define it either: CLIENT_DEFINITIONS only gains
 * H2W_INTEGRATED under `USE_HEXENWORLD_CLIENT AND NOT EMSCRIPTEN`, so the web
 * client takes quakefs.c's plain-Hexen II branch in FS_Gamedir and in the
 * FS_Init rollback.  Distinct from QuakeFS_TargetHasClientCommands, which is
 * every non-SERVERONLY target including the web client. */
#if defined(H2W_INTEGRATED)

int QuakeFS_TargetIsH2WIntegrated (void)
{
	return 1;
}

#else

int QuakeFS_TargetIsH2WIntegrated (void)
{
	return 0;
}

#endif	/* H2W_INTEGRATED */

/* The hwsv-only serverinfo write: `Info_SetValueForStarKey (svs.info,
 * "*gamedir", dir, ...)`, which names the `svs` global that exists in no other
 * binary.  Info_SetValueForStarKey itself is the info_str port and so is in
 * the archive everywhere; only the global needs this. */
#if defined(H2W) && defined(SERVERONLY)

void QuakeFS_TargetSetHwServerinfo (const char *dir)
{
	Info_SetValueForStarKey (svs.info, "*gamedir", dir, MAX_SERVERINFO_STRING);
}

#else

void QuakeFS_TargetSetHwServerinfo (const char *dir) { (void)dir; }

#endif	/* H2W && SERVERONLY */

/*============================================================================
 * behaviour hooks.  Each wraps one contiguous client-only statement group;
 * the dedicated builds, where the C has no such code at all, get a no-op.
 *==========================================================================*/

#if defined(SERVERONLY)

void QuakeFS_TargetClientReset (void) { }
void QuakeFS_TargetClientClearState (void) { }
void QuakeFS_TargetClientReinit (void) { }
void QuakeFS_TargetVidLock (void) { }
const char *QuakeFS_TargetStartupScript (void) { return ""; }
void QuakeFS_TargetShowList (int num, const char **list) { (void)num; (void)list; }

#else	/* !SERVERONLY */

/* quakefs.c:3146-3154 -- save the config into the outgoing mod's directory,
 * then stop the client and the server and drop every allocated buffer. */
void QuakeFS_TargetClientReset (void)
{
	Host_WriteConfiguration ("config.cfg");
	CL_Disconnect ();
	Host_ShutdownServer (true);
	Host_ClearMemory ();
}

/* quakefs.c:3156-3168 -- clear the stale client static state, closing a demo
 * file if one was open.  Kept as one hook because the whole block is client
 * state that no dedicated target has. */
void QuakeFS_TargetClientClearState (void)
{
	if (cls.demofile)
	{
		fclose (cls.demofile);
		cls.demofile = NULL;
	}
	memset (cls.demos, 0, sizeof(cls.demos));
	cls.demonum = 0;
	cls.demorecording = false;
	cls.demoplayback = false;
	cls.timedemo = false;
	cls.signon = 0;
}

/* quakefs.c:3239-3255 -- reload the client's view of the new mod.  Skipped
 * entirely when running dedicated, which initialised none of it; Draw_ReInit
 * is the one that used to be fatal, because it pulls the loading disc and
 * backtile through FS_LoadZoneFile (..., Z_SECZONE) and Memory_Init only
 * creates the secondary zone for a non-dedicated process.  uhexen2-jjmb. */
void QuakeFS_TargetClientReinit (void)
{
	extern qboolean isDedicated;

	if (!isDedicated)
	{
#ifdef GLQUAKE
		{ extern void TexMgr_NewGame (void); TexMgr_NewGame (); }
#endif
		Draw_ReInit ();
		BGM_Stop ();	/* stop music from previous game */

		/* re-read bindlist.lst so the previous mod's Key Setup rows
		 * don't linger */
		M_BuildBindList ();
	}
}

/* quakefs.c:3263 -- hold the video mode across the config exec that follows,
 * the arrangement QuakeSpasm and Ironwail use.  uhexen2-a5nn.26 */
void QuakeFS_TargetVidLock (void)
{
	VID_Lock ();
}

/* quakefs.c:3265 -- the startup script Cbuf_AddText() is fed next. */
const char *QuakeFS_TargetStartupScript (void)
{
	return Cmd_StartupScript ();
}

/* quakefs.c:2696, :2973, :3018 -- the listers print through the console's
 * paged list, which only the client has. */
void QuakeFS_TargetShowList (int num, const char **list)
{
	Con_ShowList (num, list);
}

#endif	/* !SERVERONLY */
