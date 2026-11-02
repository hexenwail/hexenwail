/*
 * sv_main.c -- server main program
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"

server_t	sv;
server_static_t	svs;

/* not static: engine/rust/sv_main_target.c reads it for SV_SpawnServer (E2 / slice E) */
char	localmodels[MAX_MODELS][8];	// inline model names for precache

static	cvar_t	sv_sound_distance	= {"sv_sound_distance", "800", CVAR_NONE};
						/* doesn't seem functional, but the hcode calls it */

/* not static: engine/rust/sv_main_target.c reads them, and sv_netsort below,
 * for sv_main_ents.rs */
cvar_t	sv_update_player	= {"sv_update_player", "1", CVAR_ARCHIVE};
cvar_t	sv_update_monsters	= {"sv_update_monsters", "1", CVAR_ARCHIVE};
cvar_t	sv_update_missiles	= {"sv_update_missiles", "1", CVAR_ARCHIVE};
cvar_t	sv_update_misc		= {"sv_update_misc", "1", CVAR_ARCHIVE};

/* Ironwail's sv_netsort.  Decides WHICH entity updates survive when a frame's
 * deltas do not all fit in the datagram: nearest-and-largest first rather than
 * whatever happened to come last in edict order.  0 restores the edict-order
 * truncation, for comparison.  Not archived, as upstream. */
cvar_t	sv_netsort		= {"sv_netsort", "1", CVAR_NONE};

/* Engine identity for HexenC mods -- issue #279, asked for by Mathuzzz.
 *
 * A mod that wants to light up engine-specific content has no portable way to
 * ask which engine it is on.  checkextension() cannot do it: that builtin is
 * #130 here and #99 in DP/FTE, so the number a mod writes against means two
 * different things.  cvar() is #45 everywhere, including FTE's Hexen II
 * support, so a numerically-valued cvar is the one probe that compiles and
 * behaves identically on every engine -- and reads 0 on all of them but this
 * one, because an unknown cvar reads as 0.
 *
 * CVAR_ROM so a player cannot set it and fake the answer, and deliberately NOT
 * CVAR_ARCHIVE: a value written into config.cfg by one build would be read
 * back by another and would then be lying about the engine it is in.  The
 * value is stamped from HW_VERSION_NUM at registration rather than typed here,
 * so it cannot drift from HW_BASE_VERSION; engine/tests/version_cvar_test.c
 * fails the build if the two ever disagree.
 *
 * Registered in SV_Init, which both the client and h2ded run, because the mod
 * asking is server-side QuakeC. */
cvar_t	hexenwail_version	= {"hexenwail", "0", CVAR_ROM};

/* Deliberately not CVAR_ARCHIVE, as upstream: a ceiling silently persisted
 * into a config is how you get a mod that only fails on one machine.  Takes
 * effect at the next map load, since sv.edicts is sized once there.
 *
 * Ironwail's default and ceiling: 16384, clamped into MAX_EDICTS 32000.  The
 * ceiling was 8192 until GitHub #411, so this could only LOWER the limit;
 * the costs of raising it are in the note on MIN_EDICTS in quakedef.h, and
 * entities past 8192 make sounds only on protocol 100 (SV_SoundEntityFits). */
cvar_t	max_edicts		= {"max_edicts", "16384", CVAR_NONE};

/* Cap on the per-frame unreliable datagram, in bytes; 0 (the default) is no
 * cap beyond what the connection and protocol allow (SV_SendClientDatagram).
 * MAX_DATAGRAM is 4096, well over a 1500-byte path MTU, so on a real link
 * every full datagram is IP-fragmented in three and losing any one fragment
 * loses the frame; a frame is re-sent next tick anyway, so a smaller one that
 * arrives beats a larger one that does not.  ~1400 suits such links; sv_netsort
 * decides what to shed when the budget tightens.  Applies to local
 * connections too, which makes it the test lever for the shedding path that
 * stock maps never reach.  Floored at SV_MAXDATAGRAM_FLOOR so the
 * svc_clear_edicts reserve still leaves room for entities.  GitHub #82. */
cvar_t	sv_maxdatagram		= {"sv_maxdatagram", "0", CVAR_NONE};
#define	SV_MAXDATAGRAM_FLOOR	MAX_DATAGRAM_OLD	/* 1024: what protocols 18-20 always had */

/* Registered but never read by the engine, exactly as upstream, where it exists
 * so the 2021 rerelease's progs find a cvar to query rather than nothing.  No
 * Hexen II gamecode reads it either; it is here for parity and for a mod that
 * wants somewhere to hang the convention.  Hexen II's own cheat gate is the
 * deathmatch/coop/skill test inside each cheat command, and this does not
 * change it. */
cvar_t	sv_cheats		= {"sv_cheats", "0", CVAR_NONE};

/* max_edicts resolved and clamped, latched at map load.  Read this, not the
 * cvar, so a mid-map change cannot outrun the allocation. */
int	sv_max_edicts = MAX_EDICTS;

/* Set when QC filled sv.datagram past the point where the next write would be
 * fatal.  Cleared every frame by SV_ClearDatagram.  See WriteDest in pr_cmds.c. */
qboolean	sv_datagram_dropped = false;

cvar_t	sv_ce_scale		= {"sv_ce_scale", "0", CVAR_ARCHIVE};
cvar_t	sv_ce_max_size		= {"sv_ce_max_size", "0", CVAR_ARCHIVE};

extern	cvar_t	sv_maxvelocity;
extern	cvar_t	sv_gameplayfix_elevators;	/* sv_phys.c */
extern	cvar_t	sv_freezenonclients;		/* sv_phys.c */
extern	cvar_t	sv_gameplayfix_random;		/* pr_cmds.c */
/* pr_checkextension is declared in progs.h, next to the registry it gates. */
extern	cvar_t	sv_gravity;
extern	cvar_t	sv_nostep;
extern	cvar_t	sv_friction;
extern	cvar_t	sv_edgefriction;
extern	cvar_t	sv_stopspeed;
extern	cvar_t	sv_maxspeed;
extern	cvar_t	sv_accelerate;
extern	cvar_t	sv_altnoclip;
extern	cvar_t	sv_idealpitchscale;
extern	cvar_t	sv_idealrollscale;
extern	cvar_t	sv_aim;
extern	cvar_t	sv_walkpitch;
extern	cvar_t	sv_flypitch;
extern	cvar_t	sv_debugmovestep;
extern void SV_DebugMoveStep_Changed (cvar_t *var);

int		current_skill;
int		sv_protocol = PROTOCOL_VERSION;	/* protocol version to use */

/* Whether sv_protocol got its value from the operator (-protocol or the
 * console verb) rather than from the default.  The auto-detect in
 * SV_SpawnServer used to test `sv_protocol == PROTOCOL_VERSION` for this,
 * which cannot distinguish "nobody chose" from "chose 19 on purpose" -- so
 * asking a server for maximum compatibility silently got you upgraded off it
 * again by the first mod whose progs tripped the sniff.  That is the exact
 * failure this protocol split exists to remove, so it has to be a separate
 * bit rather than an inference from the value.  Not static:
 * engine/rust/sv_main_target.c reads it for SV_SpawnServer (E2 / slice E). */
qboolean	sv_protocol_userset = false;
int		sv_kingofhill;		/* mission pack, king of the hill. */
unsigned int	info_mask, info_mask2;	/* mission pack, objectives */

extern float	scr_centertime_off;

//============================================================================


static void Sv_Edicts_f(void);

static void Max_Edicts_f (cvar_t *var)
{
	(void) var;
	if (sv.active)
		Con_Printf ("max_edicts will not take effect until the next map load.\n");
}

/* SV_ProtocolName -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Still called from C
 * (SV_Protocol_f, the -protocol startup check), so it stays a C-visible
 * symbol implemented by the Rust module. */
const char *SV_ProtocolName (int protocol);

/*
===============
SV_Protocol_f

Ironwail's sv_protocol verb.  -protocol has always been able to pick the wire
format, but only at launch, which on a dedicated server means restarting the
server to record a demo an older client can play back.

Not a cvar, and deliberately not archived, for the same reason it is not one
upstream: the value is written into the signon at SV_SendServerinfo and read
back out of it by every client for the life of the map, so it is a property of
the running server rather than a preference.  A change lands at the next map
load -- announced, because pretending otherwise is worse than useless here:
SV_MaxSounds() is derived from sv_protocol and is consulted at PRECACHE time,
so a mid-map change would put the sound indices already handed out past what
the protocol can express.  sv_main.c's svc_sound path carries the matching
guard and says so.
===============
*/
static void SV_Protocol_f (void)
{
	int	i;

	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf ("\"sv_protocol\" is \"%i\" (%s)\n",
			    sv_protocol, SV_ProtocolName (sv_protocol));
		break;

	case 2:
		i = atoi (Cmd_Argv(1));
		if (!SV_ProtocolName (i))
		{
			Con_Printf ("sv_protocol must be %i, %i, %i, %i or %i\n",
				    PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112,
				    PROTOCOL_UQE_113, PROTOCOL_UH2_114,
				    PROTOCOL_HEXENWAIL_1);
			break;
		}
		sv_protocol = i;
		sv_protocol_userset = true;
		Con_Printf ("sv_protocol set to %i (%s)\n", i, SV_ProtocolName (i));
		if (sv.active)
			Con_Printf ("changes will not take effect until the next map load.\n");
		break;

	default:
		Con_SafePrintf ("usage: sv_protocol <protocol>\n");
		break;
	}
}

/*
===============
SV_Init
===============
*/
void SV_Init (void)
{
	int		i;
	const char	*p;

	Cvar_RegisterVariable (&sv_maxvelocity);
	Cvar_RegisterVariable (&sv_gravity);
	Cvar_RegisterVariable (&sv_friction);
	Cvar_SetCallback (&sv_gravity, Host_Callback_Notify);
	Cvar_SetCallback (&sv_friction, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_edgefriction);
	Cvar_RegisterVariable (&sv_stopspeed);
	Cvar_RegisterVariable (&sv_maxspeed);
	Cvar_SetCallback (&sv_maxspeed, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_accelerate);
	Cvar_RegisterVariable (&sv_altnoclip);
	Cvar_RegisterVariable (&sv_idealpitchscale);
	Cvar_RegisterVariable (&sv_idealrollscale);
	Cvar_RegisterVariable (&sv_aim);
	Cvar_RegisterVariable (&sv_nostep);
	Cvar_RegisterVariable (&sv_walkpitch);
	Cvar_RegisterVariable (&sv_gameplayfix_elevators);
	Cvar_RegisterVariable (&sv_freezenonclients);
	Cvar_RegisterVariable (&max_edicts);
	Cvar_RegisterVariable (&sv_maxdatagram);
	Cvar_SetCallback (&max_edicts, Max_Edicts_f);
	Cvar_RegisterVariable (&sv_cheats);
	Cvar_RegisterVariable (&sv_gameplayfix_random);
	Cvar_RegisterVariable (&pr_checkextension);
	Cvar_RegisterVariable (&hexenwail_version);
	/* Stamp the real value now that it is registered.  Cvar_SetValueROM is
	 * the only way in -- CVAR_ROM rejects Cvar_Set from console or config. */
	Cvar_SetValueROM ("hexenwail", (float)HW_VERSION_NUM);
	Cvar_RegisterVariable (&sv_flypitch);
	Cvar_RegisterVariable (&sv_debugmovestep);
	Cvar_SetCallback (&sv_debugmovestep, SV_DebugMoveStep_Changed);
	Cvar_RegisterVariable (&sv_sound_distance);
	Cvar_RegisterVariable (&sv_update_player);
	Cvar_RegisterVariable (&sv_update_monsters);
	Cvar_RegisterVariable (&sv_update_missiles);
	Cvar_RegisterVariable (&sv_update_misc);
	Cvar_RegisterVariable (&sv_netsort);
	Cvar_RegisterVariable (&sv_ce_scale);
	Cvar_RegisterVariable (&sv_ce_max_size);

	SV_UserInit ();

	Cmd_AddCommand ("sv_edicts", Sv_Edicts_f);	
	Cmd_AddCommand ("sv_protocol", SV_Protocol_f);

	for (i = 0; i < MAX_MODELS; i++)
		sprintf (localmodels[i], "*%i", i);

	// initialize King of Hill to world
	sv_kingofhill = 0;

	i = COM_CheckParm ("-protocol");
	if (i && i < com_argc - 1)
	{
		sv_protocol = atoi (com_argv[i + 1]);
		sv_protocol_userset = true;
	}
	p = SV_ProtocolName (sv_protocol);
	if (!p)
	{
		/* Still fatal at startup, unlike the console verb: a bad
		 * -protocol means the operator asked for a wire format we cannot
		 * speak, and coming up on a different one would silently hand
		 * every connecting client the wrong answer. */
		Sys_Error ("Bad protocol version request %i. Accepted values: %i, %i, %i, %i, %i.",
				sv_protocol, PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112, PROTOCOL_UQE_113,
				PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1);
		return; /* silence compiler */
	}
	Sys_Printf ("Server using protocol %i (%s)\n", sv_protocol, p);
}

void SV_Edicts (const char *Name)
{
	FILE	*FH;
	int		i;
	edict_t	*e;

	FH = fopen(FS_MakePath(FS_USERDIR,NULL,Name), "w");
	if (!FH)
	{
		Con_Printf("Could not open %s\n", Name);
		return;
	}

	fprintf(FH, "Number of Edicts: %d\n", sv.num_edicts);
	fprintf(FH, "Server Time: %f\n", sv.time);
	fprintf(FH, "\n");
	fprintf(FH, "Num.     Time Class Name                     Model                          Think                                    Touch                                    Use\n");
	fprintf(FH, "---- -------- ------------------------------ ------------------------------ ---------------------------------------- ---------------------------------------- ----------------------------------------\n");

	for (i = 1; i < sv.num_edicts; i++)
	{
		e = EDICT_NUM(i);
		fprintf(FH, "%3d. %8.2f %-30s %-30s %-40s %-40s %-40s\n",
			i, e->v.nextthink, PR_GetString(e->v.classname), PR_GetString(e->v.model),
			PR_GetString(pr_functions[e->v.think].s_name), PR_GetString(pr_functions[e->v.touch].s_name),
			PR_GetString(pr_functions[e->v.use].s_name));
	}
	fclose(FH);
}

static void Sv_Edicts_f (void)
{
	const char	*Name;

	if (!sv.active)
	{
		Con_Printf("This command can only be executed on a server running a map\n");
		return;
	}

	if (Cmd_Argc() < 2)
	{
		Name = "edicts.txt";
	}
	else
	{
		Name = Cmd_Argv(1);
	}

	SV_Edicts(Name);
}

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/* SV_StartParticle..4, SV_StopSound, SV_UpdateSoundPos and SV_StartSound --
 * the sound and particle messages -- and SV_MaxSounds, SV_MaxDatagram and
 * SV_MaxReliableMessage are engine/rust/src/sv_main_sound.rs (#469 slice A
 * of #455); the C originals are engine/rust/sv_main/tests/sv_main_sound_h2.c.
 * Declared in server.h. */

/*
==================
SV_ParseWorldspawnSky

Pull the sky name out of the world's entity lump so the server has an
authoritative answer for svc_skybox instead of the literal "" it used to
send (uhexen2-6dgk).  The client reads the same key itself in Sky_NewMap,
so on a listen server this agrees with what is already on screen and
Sky_LoadSkyBox early-returns; it earns its keep for a remote client whose
copy of the map differs, or which has no BSP at all.

Accepts the same three spellings Sky_NewMap does, so the two cannot
disagree about which key wins.

Not static: SV_SpawnServer, its only caller, is engine/rust/src/sv_main_spawn.rs
(E2 / slice E of #469).  Declared in server.h.
==================
*/
void SV_ParseWorldspawnSky (void)
{
	char		key[128];
	const char	*data;

	sv.skybox[0] = 0;

	if (!sv.worldmodel || !sv.worldmodel->entities)
		return;

	data = COM_Parse (sv.worldmodel->entities);
	if (!data || com_token[0] != '{')
		return;

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return;
		if (com_token[0] == '}')
			return;			/* end of worldspawn */

		q_strlcpy (key, (com_token[0] == '_') ? com_token + 1 : com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ')
			key[strlen(key)-1] = 0;

		data = COM_Parse (data);
		if (!data)
			return;

		if (!strcmp("sky", key) ||		/* quake		*/
		    !strcmp("skyname", key) ||		/* half-life		*/
		    !strcmp("qlsky", key))		/* quake lives		*/
		{
			q_strlcpy (sv.skybox, com_token, sizeof(sv.skybox));
		}
	}
}

/* SV_MaxSounds .. SV_StartSound: see the note above SV_ParseWorldspawnSky. */

/*
==================
SV_UpdateExInventory
==================
*/
/*
==================
SV_ClientInventoryPage

Find, or claim, the extended-inventory page belonging to a client.

sv.ex_inventory_pages lives in the hunk, and Host_ClearMemory both frees
the hunk and zeroes sv on every map load -- so the array is a fresh
allocation at a fresh address each map.  client_t does not follow it:
svs.clients is allocated once in Host_Init and survives.  Any caller that
leaves a previous map's pointer in client->ex_inventory therefore leaves a
dangling pointer into freed hunk, which INV_UpdateExItem then walks as a
linked list -- reading ->next out of whatever the new map has written
there.  That was uhexen2-rutp: a nondeterministic changelevel crash that
retrying made go away, because the reused bytes differ every time.

Returns NULL when no page is available.  Callers must assign the result
unconditionally; INV_UpdateExItem treats NULL as an empty chain.
==================
*/
ex_inventory_page_t *SV_ClientInventoryPage (int clientnum)
{
	int	npages, i;

	if (!sv.ex_inventory_pages)
		return NULL;

	/* The array is maxclients * MAX_INVENTORY_EX_PAGES entries.  Two of
	 * the three original search sites bounded by maxclients alone, so in
	 * single player only page 0 was ever reachable out of the 8. */
	npages = svs.maxclients * MAX_INVENTORY_EX_PAGES;

	for (i = 0; i < npages; i++)
	{
		if (sv.ex_inventory_pages[i].id != 0 &&
		    sv.ex_inventory_pages[i].client_id != clientnum)
			continue;		/* in use by someone else */

		if (sv.ex_inventory_pages[i].id == 0)
			sv.ex_inventory_pages[i].id = ++sv.next_page_id;
		return &sv.ex_inventory_pages[i];
	}

	return NULL;
}

int INV_UpdateExItem(ex_inventory_page_t *startPage, int inv_id, int inv_cnt, qboolean inc)
{
	qboolean bFound = false;
	int i, j, result;
	ex_inventory_page_t *page = startPage;

	result = 0;

	while (page != NULL)
	{
		// try to find a matching slot
		for (i = 0; i < MAX_INVENTORY_EX; i++)
		{
			if (page->item_id[i] == inv_id)
			{
				if (inc)
					page->item_cnt[i] += inv_cnt;
				else
					page->item_cnt[i] = inv_cnt;

				page->changed_items |= (1 << i);
				result = page->item_cnt[i];
				bFound = true;

				break;
			}
		}
		page = page->next;
	}

	if (inv_cnt > 0)
	{
		// no matching slot found, create one at first empty
		if (bFound == false)
		{
			page = startPage;

			while (page != NULL)
			{
				for (i = 0; i < MAX_INVENTORY_EX; i++)
				{
					if (page->item_id[i] == 0)
					{
						page->item_id[i] = inv_id;

						if (inc)
							page->item_cnt[i] += inv_cnt;
						else
							page->item_cnt[i] = inv_cnt;

						page->changed_items |= (1 << i);
						page->new_items |= (1 << i);
						result = page->item_cnt[i];
						bFound = true;

						break;
					}
				}

				if (!bFound)
				{
					if (page->next == NULL)
					{
						/* The old search compared client_id against i,
						 * the inventory SLOT index, not a client id.
						 * The owner is whoever owns the chain we are
						 * extending. */
						page->next = SV_ClientInventoryPage (startPage->client_id);
						if (page->next == NULL)
							break;	/* out of pages; without this the
								 * loop spun forever, since page
								 * never advanced and ->next was
								 * never filled in */
					}
					page = page->next;
				}
				else
					break;
			}
		}
	}


	return result;
}

/*
==============================================================================

CLIENT SPAWNING

==============================================================================
*/

/* SV_SendServerinfo -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */
void SV_SendServerinfo (client_t *client);

/* SV_ConnectClient -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Was static; only SV_CheckForNewClients called it. */


/* SV_CheckForNewClients -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h; called from host.c. */


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

/*
 * SV_ClearDatagram -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/* The entity updates -- the fat PVS, SV_EntityUpdateSize, SV_EntityPriority,
 * SV_PrepareClientEntities and SV_CleanupEnts -- are
 * engine/rust/src/sv_main_ents.rs (#469 slice B of #455); the C originals are
 * engine/rust/sv_main/tests/sv_main_ents_h2.c.  The fat PVS buffer is
 * engine/rust/sv_main_target.c's.  SV_SendClientDatagram and
 * SV_SendClientMessages below call these two. */
void SV_PrepareClientEntities (client_t *client, edict_t *clent, sizebuf_t *msg);
void SV_CleanupEnts (void);

/*
 * SV_WriteClientdataToMessage -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_SendClientDatagram -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_UpdateToReliableMessages -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */


/*
 * SV_SendNop -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_SendClientMessages -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */


/*
==============================================================================

SERVER SPAWNING

==============================================================================
*/

/* SV_ModelIndex -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h. */

/* SV_SignonBufferCount and SV_SignonBufferSize -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Were static; only SV_SendSignonBuffer called them. */

/* SV_ReserveSignonSpace -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h; also called from pr_cmds.c. */

/* SV_SendSignonBuffer -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h. */

/* SV_CreateBaseline -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */
void SV_CreateBaseline (void);


/* SV_SendReconnect -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */
void SV_SendReconnect (void);


/*
================
SV_GetLevelname

Return the full levelname
================
*/
const char *SV_GetLevelname (void)
{
	int idx = (int)sv.edicts->v.message;
	if (idx > 0 && idx <= host_string_count)
		return Host_GetString(idx - 1);

/*	return "";*/
/* Use netname on map if there is one, so they don't have to edit strings.txt */
	return PR_GetString(sv.edicts->v.netname);
}


/*
================
SV_SaveSpawnparms

Grabs the current state of each client for saving across the
transition to another level
================
*/
void SV_SaveSpawnparms (void)
{
	int		i;
//	int		j;

	svs.serverflags = *sv_globals.serverflags;

	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (!host_client->active)
			continue;

	// call the progs to get default spawn parms for the new client
//		*sv_globals.self = EDICT_TO_PROG(host_client->edict);
//		PR_ExecuteProgram (*sv_globals.SetChangeParms);
//		for (j = 0; j < NUM_SPAWN_PARMS; j++)
//			host_client->spawn_parms[j] = sv_globals.parm[j];
	}
}

/*
=============
INV_Write

For savegames
=============
*/
void INV_WritePage(FILE *f, ex_inventory_page_t *page, int clientId)
{
	int i;

	//Page: id, next_id, client_id, {item_id, item_cnt} ...
	fprintf(f, "Page: %i %i %i", page->id, (page->next != NULL ? page->next->id : -1), clientId); //page_id, next_id, client_id
	for (i = 0; i < MAX_INVENTORY_EX; i++)
	{
		if ((page->item_id[i] != 0) && (page->item_cnt[i] > 0))
		{
			fprintf(f, " %i %i", page->item_id[i], page->item_cnt[i]); //item_id, item_cnt
		}
	}
	fprintf(f, "\n");
}

// All changes need to be in SV_SaveInventory(), SV_LoadInventory(), CL_ParseInventory()
void SV_LoadInventory(FILE *FH)
{
	int Total = 0;
	int id, clientId, nextId, count, c, i;
	int item_id, item_cnt, item_idx;

	fscanf(FH, "Pages: %d\n", &Total);
	if (Total < 0 || Total > MAX_CLIENTS)
		Host_Error("%s: bad numpages", __func__);

	//Page: id, next_id, client_id, {item_id, item_cnt} ...
	for (count = 0; count < Total; count++)
	{
		fscanf(FH, "Page: %d %d %d", &id, &nextId, &clientId);
		for (i = 0; ((i < svs.maxclients) && (sv.ex_inventory_pages[i].id != 0) && (sv.ex_inventory_pages[i].client_id != clientId)); i++);
		memset(&sv.ex_inventory_pages[i], 0, sizeof(ex_inventory_page_t));
		sv.ex_inventory_pages[i].id = id;
		sv.ex_inventory_pages[i].client_id = clientId;
		if (id > sv.next_page_id)
			sv.next_page_id = id;

		item_idx = 0;
		c = fgetc(FH);	/* read one char, see what it is: */
		while ((c != '\n') && (c != '\r'))
		{
			fscanf(FH, "%d %d", &item_id, &item_cnt);
			sv.ex_inventory_pages[i].item_id[item_idx] = item_id;
			sv.ex_inventory_pages[i].item_cnt[item_idx] = item_cnt;
			sv.ex_inventory_pages[i].changed_items |= (1 << item_idx);
			sv.ex_inventory_pages[i].new_items |= (1 << item_idx);

			item_idx++;
			c = fgetc(FH);	/* read one char, see what it is: */
		}
	}

	Total = Total;
}

void INV_SavePages(FILE *FH)
{
	int i, j;
	client_t	*host_client;

	j = 0;
	host_client = svs.clients;
	for (i = 0; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->ex_inventory != NULL)
			j++;
	}

	fprintf(FH, "Pages: %i\n", j);

	host_client = svs.clients;
	for (i = 0; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->ex_inventory != NULL)
			INV_WritePage(FH, host_client->ex_inventory, i);
	}
}

/* E2 / slice E of #469: the map_checks readout -- SV_MapCheckThresh,
 * SV_MapCheckTitle, SV_PrintMapChecklist -- and SV_SpawnServer are
 * engine/rust/src/sv_main_spawn.rs; the C originals, with the MAP CHECKLIST
 * note above them, are engine/rust/sv_main/tests/sv_main_spawn_h2.c.
 * SV_PrintMapCheck is C-variadic and stays C, in
 * engine/rust/sv_main_variadic.c; mapcheck_t moved to
 * engine/rust/sv_main_target.h.  SV_SpawnServer is declared in server.h. */
