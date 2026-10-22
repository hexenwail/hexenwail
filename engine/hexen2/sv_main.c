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

static char	localmodels[MAX_MODELS][8];	// inline model names for precache

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
 * bit rather than an inference from the value. */
static qboolean	sv_protocol_userset = false;
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
==================
*/
static void SV_ParseWorldspawnSky (void)
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
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Still called from C (SV_SpawnServer). */
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
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Still called from C (SV_SpawnServer). */
void SV_CreateBaseline (void);


/* SV_SendReconnect -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Still called from C (SV_SpawnServer). */
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

/*
==============================================================================

MAP CHECKLIST

Ironwail's map_checks report, printed once per map load when map_checks (or
developer) is on.  The counting half lives in ED_LoadFromFile; this is the
readout.  uhexen2-a5nn.40

Ironwail's version is Quake's, and four of its lines do not survive contact
with Hexen II unchanged.  Each divergence is argued at its own check below,
because the whole value of a checklist is that a mapper trusts it: one line
that is always wrong makes the other ten unread.

Markers are ASCII rather than upstream's \xDB/\xDD box glyphs -- those are
Quake charset code points, and Hexen II's conchars are a different font.

==============================================================================
*/

typedef enum
{
	MAPCHECK_FAILED,
	MAPCHECK_PARTIAL,
	MAPCHECK_OK,
	MAPCHECK_NA		/* passes because it does not apply here */
} mapcheck_t;


/*
================
SV_MapCheckThresh
================
*/
static mapcheck_t SV_MapCheckThresh (int current, int target)
{
	if (current <= 0)
		return MAPCHECK_FAILED;
	if (current >= target)
		return MAPCHECK_OK;
	return MAPCHECK_PARTIAL;
}

/*
================
SV_PrintMapCheck
================
*/
static void SV_PrintMapCheck (mapcheck_t status, const char *format, ...)
{
	char		str[1024];
	va_list		argptr;
	char		mark;

	va_start (argptr, format);
	q_vsnprintf (str, sizeof(str), format, argptr);
	va_end (argptr);

	switch (status)
	{
	case MAPCHECK_OK:	mark = 'x'; break;
	case MAPCHECK_NA:	mark = '-'; break;
	case MAPCHECK_PARTIAL:	mark = '~'; break;
	default:		mark = ' '; break;
	}

	Con_SafePrintf ("[%c] %s\n", mark, str);

	if (status == MAPCHECK_FAILED || status == MAPCHECK_PARTIAL)
		sv.mapchecks.numwarnings++;
}

/*
================
SV_MapCheckTitle

Squeezes a levelname onto one console line.  Hexen II's titles come out of
strings.txt, which is authored as display text and can carry newlines.
================
*/
static void SV_MapCheckTitle (char *out, size_t outsize, const char *in)
{
	size_t	n = 0;

	if (!in)
		in = "";
	while (*in && n + 1 < outsize)
	{
		byte c = (byte)*in++;
		if (c < 32 || c == 127)
		{
			if (!n || out[n - 1] == ' ')
				continue;	/* no leading or doubled space */
			c = ' ';
		}
		out[n++] = (char)c;
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

/*
================
SV_PrintMapChecklist
================
*/
static void SV_PrintMapChecklist (void)
{
	const int	MIN_DM_SPAWN_POINTS = 5;
	const int	MIN_COOP_SPAWN_POINTS = 3;

	qboolean	skill_levels;
	char		buf[1024];
	int		warnings;

	//
	// header
	//
	Con_SafePrintf ("\n");
	Con_SafePrintf ("=====================================\n");
	Con_SafePrintf ("\n");
	Con_SafePrintf ("Map checklist (%s):\n\n", COM_SkipPath (sv.modelname));

	//
	// light data
	//
	SV_PrintMapCheck (sv.worldmodel->lightdata != NULL ? MAPCHECK_OK : MAPCHECK_FAILED, "lightmap data");

	//
	// vis data
	//
	if (!sv.worldmodel->visdata)
	{
		char pointfile[MAX_OSPATH];
		q_snprintf (pointfile, sizeof(pointfile), "maps/%s.pts", sv.name);
		if (FS_FileExists (pointfile, NULL))
			SV_PrintMapCheck (MAPCHECK_FAILED, "vis data (unsealed map? %s exists)", pointfile);
		else
			SV_PrintMapCheck (MAPCHECK_FAILED, "vis data");
	}
	else
		SV_PrintMapCheck (MAPCHECK_OK, "vis data");

	//
	// changelevel trigger
	//
	if (!sv.mapchecks.trigger_changelevel)
		SV_PrintMapCheck (MAPCHECK_FAILED, "trigger_changelevel");
	else if (sv.mapchecks.trigger_changelevel == 1)
	{
		if (sv.mapchecks.valid_changelevel == sv.mapchecks.trigger_changelevel)
		{
			/* The startspot is what makes this a hub link rather than a
			 * plain exit, so it is worth showing when there is one. */
			if (sv.mapchecks.changespot[0])
				SV_PrintMapCheck (MAPCHECK_OK, "trigger_changelevel (%s, startspot \"%s\")",
					sv.mapchecks.changelevel, sv.mapchecks.changespot);
			else
				SV_PrintMapCheck (MAPCHECK_OK, "trigger_changelevel (%s)", sv.mapchecks.changelevel);
		}
		else
			SV_PrintMapCheck (MAPCHECK_PARTIAL, "trigger_changelevel (missing \"map\" key)");
	}
	else
	{
		if (sv.mapchecks.valid_changelevel == sv.mapchecks.trigger_changelevel)
			SV_PrintMapCheck (MAPCHECK_OK, "trigger_changelevel (%d)", sv.mapchecks.trigger_changelevel);
		else
			SV_PrintMapCheck (MAPCHECK_PARTIAL, "trigger_changelevel (%d/%d missing \"map\" key)",
				sv.mapchecks.trigger_changelevel - sv.mapchecks.valid_changelevel,
				sv.mapchecks.trigger_changelevel
			);
	}

	//
	// intermission camera
	//
	// DIVERGES FROM UPSTREAM, twice.  Quake runs the intermission on every
	// level exit, so Ironwail fails a map that has no camera.  Hexen II does
	// not: changelevel_touch calls GotoNextMap() outright when deathmatch is
	// 0 (client.hc, and the comment above it records that the
	// NO_INTERMISSION spawnflag was removed rather than made conditional).
	// The camera is reachable only in deathmatch.  Failing every
	// single-player map for lacking one is the noise that gets a checklist
	// ignored, so single player reports it as not applicable.
	//
	// Nor is it fatal for a deathmatch map: FindIntermission falls back to
	// info_player_start.  This line therefore never counts as a warning,
	// which was arrived at empirically -- with it warning on any map that
	// had deathmatch spawns, all seven official maps tested lit it up,
	// because Raven put deathmatch spawns in the single-player campaign and
	// intermission cameras in none of it.  A line that is always yellow is
	// the thing that gets a checklist stopped being read.
	//
	if (sv.mapchecks.intermission)
		SV_PrintMapCheck (MAPCHECK_OK, "info_intermission (%d)", sv.mapchecks.intermission);
	else if (sv.mapchecks.dm_spawns > 0)
		SV_PrintMapCheck (MAPCHECK_NA, "info_intermission (none: deathmatch views the round end from info_player_start)");
	else
		SV_PrintMapCheck (MAPCHECK_NA, "info_intermission (n/a: Hexen II shows the intermission in deathmatch only)");

	//
	// skill levels
	//
	skill_levels = sv.mapchecks.skill_triggers > 0 ||
		(sv.mapchecks.skill_ents[0] != sv.mapchecks.skill_ents[1] || sv.mapchecks.skill_ents[1] != sv.mapchecks.skill_ents[2]);
	SV_PrintMapCheck (skill_levels ? MAPCHECK_OK : MAPCHECK_FAILED, "skill spawnflags/triggers");

	//
	// single player spawn point
	//
	// NOT IN UPSTREAM.  Hexen II's SelectSpawnPoint ends in
	// error("PutClientInServer: no info_player_start on level"), which drops
	// the player to the console with a QC stack trace the moment the map
	// loads.  Naming it on the checklist is cheaper than reading that.
	//
	SV_PrintMapCheck (sv.mapchecks.sp_spawns > 0 ? MAPCHECK_OK : MAPCHECK_FAILED,
		"info_player_start (%d)", sv.mapchecks.sp_spawns);

	//
	// coop spawn points
	//
	SV_PrintMapCheck (SV_MapCheckThresh (sv.mapchecks.coop_spawns, MIN_COOP_SPAWN_POINTS),
		"info_player_coop (%d/%d+)", sv.mapchecks.coop_spawns, MIN_COOP_SPAWN_POINTS);

	//
	// deathmatch spawn points
	//
	// Upstream's threshold kept.  The consequence is harsher here than in
	// Quake: Hexen II's SelectSpawnPoint scans info_player_deathmatch in an
	// unbounded `loop`, so a map started in deathmatch with none of them
	// hangs the server rather than picking a fallback.
	//
	SV_PrintMapCheck (SV_MapCheckThresh (sv.mapchecks.dm_spawns, MIN_DM_SPAWN_POINTS),
		"info_player_deathmatch (%d/%d+)%s", sv.mapchecks.dm_spawns, MIN_DM_SPAWN_POINTS,
		sv.mapchecks.dm_spawns ? "" : " -- deathmatch on this map would hang the spawn search");

	//
	// music track
	//
	// DIVERGES FROM UPSTREAM, and not by a rename.  Quake's music track is
	// worldspawn's "sounds", read through entvars.  Hexen II has neither:
	// ED_ParseEdict intercepts the worldspawn keys "CD" and "MIDI" by name
	// before the field lookup and stores them in sv.cd_track / sv.midi_name,
	// so there is no progs field to read.  (Hexen II's entvars DO have a
	// float named soundtype, which is unrelated -- it picks the sound set on
	// doors and platforms.  Reading it here would be reporting a door.)
	//
	if (sv.midi_name[0] && sv.cd_track)
		SV_PrintMapCheck (MAPCHECK_OK, "music (MIDI \"%s\", CD track %d)", sv.midi_name, sv.cd_track);
	else if (sv.midi_name[0])
		SV_PrintMapCheck (MAPCHECK_OK, "music (MIDI \"%s\")", sv.midi_name);
	else if (sv.cd_track)
		SV_PrintMapCheck (MAPCHECK_OK, "music (CD track %d)", sv.cd_track);
	else
		SV_PrintMapCheck (MAPCHECK_FAILED, "music (worldspawn \"MIDI\" or \"CD\" key)");

	//
	// map title
	//
	// DIVERGES FROM UPSTREAM.  Quake's worldspawn "message" is the title
	// string itself; Hexen II's is a 1-based index into strings.txt, with
	// "netname" as the literal-string escape hatch for maps that would
	// rather not edit strings.txt.  SV_GetLevelname already resolves both.
	//
	SV_MapCheckTitle (buf, sizeof(buf), SV_GetLevelname ());
	if (buf[0])
		SV_PrintMapCheck (MAPCHECK_OK, "map title (%s)", buf);
	else if ((int)sv.edicts->v.message > 0)
		SV_PrintMapCheck (MAPCHECK_PARTIAL, "map title (worldspawn \"message\" is %d, past the end of strings.txt)",
			(int)sv.edicts->v.message);
	else
		SV_PrintMapCheck (MAPCHECK_FAILED, "map title (worldspawn \"message\" index or \"netname\" string)");

#ifndef SERVERONLY
	//
	// Sky textures.  Upstream warns about maps with more than one sky, of
	// which other engines render only the last, and about sky textures that
	// are not 256 x 128, which older engines could crash on.  Both apply
	// unchanged to Hexen II BSPs -- what changes is the lookup: there is no
	// TEXTYPE_SKY bucket here, so the world texture list is scanned for the
	// "sky" name prefix, which is what the loader itself keys on.
	//
	// Client-only: h2ded builds against sv_model.h, which loads brush
	// geometry without textures and has no numtextures to walk.
	//
	{
		int	i, numskies = 0, oddsized = 0;

		for (i = 0; i < sv.worldmodel->numtextures; i++)
		{
			texture_t *tex = sv.worldmodel->textures[i];
			if (!tex || strncmp (tex->name, "sky", 3) != 0)
				continue;
			numskies++;
			if (tex->width != 256 || tex->height != 128)
				oddsized++;
		}

		if (numskies > 1 || oddsized > 0)
		{
			SV_PrintMapCheck (MAPCHECK_FAILED, "compat: single %ssky texture (%d found)",
				oddsized > 0 ? "256 x 128 " : "", numskies);
			for (i = 0; i < sv.worldmodel->numtextures; i++)
			{
				texture_t *tex = sv.worldmodel->textures[i];
				if (!tex || strncmp (tex->name, "sky", 3) != 0)
					continue;
				if (tex->width != 256 || tex->height != 128)
					Con_SafePrintf ("      %s (%u x %u)\n", tex->name, tex->width, tex->height);
				else
					Con_SafePrintf ("      %s\n", tex->name);
			}
		}
	}
#endif	/* !SERVERONLY */

	//
	// footer
	//
	// Upstream counts warnings and never prints the total.  We do: the list
	// is long enough that "did anything fail?" is a real question, and a
	// counter nothing reads is its own small bug (uhexen2-a5nn.38).
	//
	warnings = sv.mapchecks.numwarnings;
	Con_SafePrintf ("\n");
	if (warnings)
		Con_SafePrintf ("%d item%s to look at.\n", warnings, warnings == 1 ? "" : "s");
	else
		Con_SafePrintf ("Nothing to look at.\n");
	Con_SafePrintf ("=====================================\n");
	Con_SafePrintf ("\n");
}


/*
================
SV_SpawnServer

This is called at the start of each level
================
*/
void SV_SpawnServer (const char *server, const char *startspot)
{
	static char	dummy[8] = { 0,0,0,0,0,0,0,0 };
	edict_t		*ent;
	int			i;

	// let's not have any servers with no name
	if (hostname.string[0] == 0)
		Cvar_Set ("hostname", "UNNAMED");
#if !defined(SERVERONLY)
	scr_centertime_off = 0;
#endif

	Con_DPrintf ("%s: %s\n", __func__, server);

	/* A Host_Error out of SV_Physics skips its reset; a map loaded after it
	 * would stamp every entity with that walk's value instead of 0 (#138). */
	sv_physics_walk = 0;

	if (svs.changelevel_issued)
	{
		SaveGamestate(true);
	}

//
// tell all connected clients that we are going to a new level
//
	if (sv.active)
	{
		SV_SendReconnect ();
	}

/* if this is GL version, we need to tell D_FlushCaches() whether to flush
   OGL textures depending on mapname change. */
#ifdef GLQUAKE
	flush_textures = q_strncasecmp(server, sv.name, 64) ? true : false;
#endif

//
// make cvars consistant
//
	if (coop.integer)
		Cvar_Set ("deathmatch", "0");

	current_skill = skill.integer;
	if (current_skill < 0)
		current_skill = 0;
	if (current_skill > 4)
		current_skill = 4;

	Cvar_SetValue ("skill", current_skill);

//
// set up the new server
//
	//memset (&sv, 0, sizeof(sv));
	Host_ClearMemory ();

	q_strlcpy (sv.name, server, sizeof(sv.name));
	if (startspot)
		q_strlcpy(sv.startspot, startspot, sizeof(sv.startspot));

	/* Has to be set before ED_LoadFromFile runs, which is where the census
	 * happens.  developer turns it on too, the way it does upstream: the
	 * checklist is a strict superset of what a developer build wants to
	 * know about a map it just loaded.  uhexen2-a5nn.40 */
	if (map_checks.integer || developer.integer)
		sv.mapchecks.active = true;

// load progs to get entity field count
#if !defined(SERVERONLY)
	total_loading_size = 100;
	current_loading_size = 0;
	loading_stage = 1;
	D_ShowLoadingSize();
#endif
	PR_LoadProgs ();

	/* Auto-detect protocol: scan progs for extended builtins (107-112)
	 * that require protocol 21 (UH2_114). If none found, use the
	 * default protocol (19/RAVEN_112) for maximum compatibility. */
	if (!sv_protocol_userset)
	{
		int fi;
		qboolean needs_114 = false;
		for (fi = 0; fi < progs->numfunctions; fi++)
		{
			int bi = -pr_functions[fi].first_statement;
			if (pr_functions[fi].first_statement < 0 && bi >= 107 && bi <= 112)
			{
				needs_114 = true;
				break;
			}
		}
		if (needs_114)
		{
			sv_protocol = PROTOCOL_UH2_114;
			Con_Printf ("Progs uses extended builtins — auto-upgraded to protocol %d\n", sv_protocol);
		}

		/* Same sniff, one protocol further up: U_ALPHA is ours, so a mod
		 * that declares .alpha needs PROTOCOL_HEXENWAIL_1 or the field is
		 * simply never transmitted.  Doing it here rather than defaulting
		 * the engine to 100 keeps a stock game byte-identical to upstream
		 * on the wire -- the extension costs compatibility only for the
		 * content that actually asked for it. */
		if (PR_HasField ("alpha"))
		{
			sv_protocol = PROTOCOL_HEXENWAIL_1;
			Con_Printf ("Progs uses .alpha — auto-upgraded to protocol %d\n", sv_protocol);
		}
	}

#if !defined(SERVERONLY)
	current_loading_size += 10;
	D_ShowLoadingSize();
#endif
	Host_LoadStrings();
#if !defined(SERVERONLY)
	current_loading_size += 5;
	D_ShowLoadingSize();
#endif

// allocate server memory
	/* Host_ClearMemory() called above already cleared the whole sv structure */
	sv.states = (client_state2_t *) Hunk_AllocName (svs.maxclients * sizeof(client_state2_t), "states");
	/* Latch the ceiling for this map before anything can allocate against it. */
	sv_max_edicts = (int) max_edicts.value;
	if (sv_max_edicts < MIN_EDICTS)
		sv_max_edicts = MIN_EDICTS;
	if (sv_max_edicts > MAX_EDICTS)
		sv_max_edicts = MAX_EDICTS;

	sv.edicts = (edict_t *) Hunk_AllocName (sv_max_edicts*pr_edict_size, "edicts");

	SZ_Init (&sv.datagram, sv.datagram_buf, sizeof(sv.datagram_buf));
	sv.datagram.name = "sv.datagram";
	SZ_Init (&sv.reliable_datagram, sv.reliable_datagram_buf, sizeof(sv.reliable_datagram_buf));
	sv.reliable_datagram.name = "sv.reliable_datagram";
	/* Start the signon list over: buffer 0 is the one being filled, and
	 * nothing is completed yet.  SV_ReserveSignonSpace adds the rest. */
	sv.num_signon_buffers = 0;
	SZ_Init (&sv.signon, sv.signon_bufs[0], MAX_SIGNON_SIZE);
	sv.signon.name = "sv.signon";

// leave slots at start for clients only
	sv.num_edicts = svs.maxclients + 1 + max_temp_edicts.integer;
	for (i = 0; i < svs.maxclients; i++)
	{
		ent = EDICT_NUM(i+1);
		svs.clients[i].edict = ent;
		svs.clients[i].send_all_v = true;
	}

	for (i = 0; i < max_temp_edicts.integer; i++)
	{
		ent = EDICT_NUM(i + svs.maxclients + 1);
		ED_ClearEdict(ent);

		ent->free = true;
		ent->freetime = -999;
	}

	sv.state = ss_loading;
	sv.paused = false;

	sv.time = 1.0;

	q_strlcpy (sv.name, server, sizeof(sv.name));
	q_snprintf (sv.modelname, sizeof(sv.modelname), "maps/%s.bsp", server);

	sv.worldmodel = Mod_ForName (sv.modelname, false);
	if (!sv.worldmodel)
	{
		Con_Printf ("Couldn't spawn server %s\n", sv.modelname);
		sv.active = false;
#if !defined(SERVERONLY)
		total_loading_size = 0;
		loading_stage = 0;
		/* Every caller reaches us through a SCR_BeginLoadingPlaque, which
		 * sets scr_disabled_for_loading and makes SCR_UpdateScreen a no-op
		 * until someone ends the plaque.  Bailing out here without ending
		 * it left the window painting nothing at all: the "Couldn't spawn
		 * server" line above went to a console that was never drawn, the
		 * game looked hung, and the only way out was SCR_UpdateScreen's
		 * 25-second watchdog, which finally re-enabled drawing and printed
		 * "load timeout." on top.  Field-reported as a ~1 minute freeze on
		 * `map <name>` for a map the current gamedir doesn't have.
		 *
		 * Note this needs the GL screen path to bite: h2shared/screen.c's
		 * SCR_BeginLoadingPlaque still keeps vanilla's
		 * (cls.state != ca_connected) bail, so the flag never got set
		 * there.  gl_screen.c dropped that guard deliberately -- "callers
		 * know when they want a plaque" -- which is what turned a silent
		 * no-op into a visible hang.  uhexen2-h9zy */
		SCR_EndLoadingPlaque ();
#endif
		return;
	}
	sv.models[1] = sv.worldmodel;

	SV_ParseWorldspawnSky ();
	q_strlcpy (sv.mod_name, fs_gamedir_nopath, sizeof(sv.mod_name));

//
// clear world interaction links
//
	SV_ClearWorld ();

	sv.sound_precache[0] = dummy;
	sv.model_precache[0] = dummy;
	sv.model_precache[1] = sv.modelname;
	for (i = 1; i < sv.worldmodel->numsubmodels; i++)
	{
		sv.model_precache[1+i] = localmodels[i];
		sv.models[i+1] = Mod_ForName (localmodels[i], false);
	}

	sv.num_ex_items = 0;
	if (sv.ex_items == NULL)
	{
		sv.ex_items = (ex_item_t *)Hunk_AllocName(MAX_ITEMS_EX * sizeof(ex_item_t), "ex_items");
		for (i = 0; i < 15; i++)
		{
			sv.num_ex_items += 1;
			sv.ex_items[i].id = (int)(i + 1);
			q_strlcpy(sv.ex_items[i].icon, va("gfx/arti%02d.lmp", i), MAX_QPATH);
		}
	}

	if (sv.ex_inventory_pages == NULL)
	{
		sv.ex_inventory_pages = (ex_inventory_page_t *)Hunk_AllocName((svs.maxclients * MAX_INVENTORY_EX_PAGES) * sizeof(ex_inventory_page_t), "ex_pages");
	}

	/* Re-point every client that is still connected.  Host_ClearMemory
	 * freed the previous array a few lines above this function's caller,
	 * and clients survive a changelevel -- so without this their
	 * ex_inventory dangles into freed hunk for the rest of the session.
	 * uhexen2-rutp. */
	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active)
			svs.clients[i].ex_inventory = SV_ClientInventoryPage (i);
	}

//
// load the rest of the entities
//

	ent = EDICT_NUM(0);
	memset (&ent->v, 0, progs->entityfields * 4);
	ent->free = false;
	ent->v.model = PR_SetEngineString(sv.worldmodel->name);
	ent->v.modelindex = 1;		// world model
	ent->v.solid = SOLID_BSP;
	ent->v.movetype = MOVETYPE_PUSH;

	if (coop.integer)
		*sv_globals.coop = coop.value;
	else
		*sv_globals.deathmatch = deathmatch.value;
	if (sv_globals.randomclass)
		*sv_globals.randomclass = randomclass.value;
	*sv_globals.mapname = PR_SetEngineString(sv.name);
	*sv_globals.startspot = PR_SetEngineString(sv.startspot);
	// serverflags are for cross level information (sigils)
	*sv_globals.serverflags = svs.serverflags;

#if !defined(SERVERONLY)
	current_loading_size += 5;
	D_ShowLoadingSize();
#endif
	ED_LoadFromFile (sv.worldmodel->entities);

	sv.active = true;

// all setup is completed, any further precache statements are errors
	sv.state = ss_active;

// run two frames to allow everything to settle
	host_frametime = 0.1;
	SV_Physics ();
	SV_Physics ();

// create a baseline for more efficient communications
	SV_CreateBaseline ();

	/* How much of the signon budget this map actually used.  Worth having
	 * a number for: the shipped SoT maps already need more than one buffer,
	 * and the only warning anyone got before was a crash.  uhexen2-z5wt. */
	{
		int	total = 0;
		for (i = 0; i < sv.num_signon_buffers; i++)
			total += sv.signon_size[i];
		total += sv.signon.cursize;
		Con_DPrintf ("Signon: %d bytes in %d/%d buffers\n",
				total, sv.num_signon_buffers + 1, MAX_SIGNON_BUFFERS);
	}

// send serverinfo to all connected clients
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->active)
			SV_SendServerinfo (host_client);
	}

	svs.changelevel_issued = false;		// now safe to issue another

	Con_DPrintf ("Server spawned.\n");

	if (sv.mapchecks.active)
		SV_PrintMapChecklist ();

#if !defined(SERVERONLY)
	total_loading_size = 0;
	loading_stage = 0;
#endif
}

