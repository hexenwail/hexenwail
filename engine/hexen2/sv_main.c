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

static	cvar_t	sv_update_player	= {"sv_update_player", "1", CVAR_ARCHIVE};
static	cvar_t	sv_update_monsters	= {"sv_update_monsters", "1", CVAR_ARCHIVE};
static	cvar_t	sv_update_missiles	= {"sv_update_missiles", "1", CVAR_ARCHIVE};
static	cvar_t	sv_update_misc		= {"sv_update_misc", "1", CVAR_ARCHIVE};

/* Ironwail's sv_netsort.  Decides WHICH entity updates survive when a frame's
 * deltas do not all fit in the datagram: nearest-and-largest first rather than
 * whatever happened to come last in edict order.  0 restores the edict-order
 * truncation, for comparison.  Not archived, as upstream. */
static	cvar_t	sv_netsort		= {"sv_netsort", "1", CVAR_NONE};

/* Deliberately not CVAR_ARCHIVE, as upstream: a ceiling silently persisted
 * into a config is how you get a mod that only fails on one machine.  Takes
 * effect at the next map load, since sv.edicts is sized once there.
 *
 * MAX_EDICTS is still the hard ceiling this clamps into, so today this can only
 * lower the limit, not raise it past 8192.  Raising the hard cap is a separate
 * decision with real costs -- see the note on MIN_EDICTS in quakedef.h. */
cvar_t	max_edicts		= {"max_edicts", "8192", CVAR_NONE};

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

/*
===============
SV_ProtocolName

The four protocols this engine speaks, or NULL for anything else.  Shared by
SV_Init and SV_Protocol_f so the accepted set is stated once: a list that
disagrees with itself is how a value gets accepted at the console and then
Sys_Errors on the next launch from the same config.
===============
*/
static const char *SV_ProtocolName (int protocol)
{
	switch (protocol)
	{
	case PROTOCOL_RAVEN_111:	return "Raven/H2/1.11";
	case PROTOCOL_RAVEN_112:	return "Raven/MP/1.12";
	case PROTOCOL_UQE_113:		return "UQE/1.13";
	case PROTOCOL_UH2_114:		return "Raven/MP/1.14";
	default:			return NULL;
	}
}

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
			Con_Printf ("sv_protocol must be %i, %i, %i or %i\n",
				    PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112,
				    PROTOCOL_UQE_113, PROTOCOL_UH2_114);
			break;
		}
		sv_protocol = i;
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
	Cvar_SetCallback (&max_edicts, Max_Edicts_f);
	Cvar_RegisterVariable (&sv_cheats);
	Cvar_RegisterVariable (&sv_gameplayfix_random);
	Cvar_RegisterVariable (&pr_checkextension);
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
		sv_protocol = atoi (com_argv[i + 1]);
	p = SV_ProtocolName (sv_protocol);
	if (!p)
	{
		/* Still fatal at startup, unlike the console verb: a bad
		 * -protocol means the operator asked for a wire format we cannot
		 * speak, and coming up on a different one would silently hand
		 * every connecting client the wrong answer. */
		Sys_Error ("Bad protocol version request %i. Accepted values: %i, %i, %i, %i.",
				sv_protocol, PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112, PROTOCOL_UQE_113, PROTOCOL_UH2_114);
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

/*
==================
SV_StartParticle

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle (vec3_t org, vec3_t dir, int color, int count)
{
	int		i, v;

	if (sv.datagram.cursize > MAX_DATAGRAM-16)
		return;
	MSG_WriteByte (&sv.datagram, svc_particle);
	MSG_WriteCoord (&sv.datagram, org[0]);
	MSG_WriteCoord (&sv.datagram, org[1]);
	MSG_WriteCoord (&sv.datagram, org[2]);
	for (i = 0; i < 3; i++)
	{
		v = dir[i] * 16;
		if (v > 127)
			v = 127;
		else if (v < -128)
			v = -128;
		MSG_WriteChar (&sv.datagram, v);
	}
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, color);
}

/*
==================
SV_StartParticle2

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle2 (vec3_t org, vec3_t dmin, vec3_t dmax, int color, int effect, int count)
{
	if (sv.datagram.cursize > MAX_DATAGRAM-36)
		return;
	MSG_WriteByte (&sv.datagram, svc_particle2);
	MSG_WriteCoord (&sv.datagram, org[0]);
	MSG_WriteCoord (&sv.datagram, org[1]);
	MSG_WriteCoord (&sv.datagram, org[2]);
	MSG_WriteFloat (&sv.datagram, dmin[0]);
	MSG_WriteFloat (&sv.datagram, dmin[1]);
	MSG_WriteFloat (&sv.datagram, dmin[2]);
	MSG_WriteFloat (&sv.datagram, dmax[0]);
	MSG_WriteFloat (&sv.datagram, dmax[1]);
	MSG_WriteFloat (&sv.datagram, dmax[2]);

	MSG_WriteShort (&sv.datagram, color);
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, effect);
}

/*
==================
SV_StartParticle3

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle3 (vec3_t org, vec3_t box, int color, int effect, int count)
{
	if (sv.datagram.cursize > MAX_DATAGRAM-15)
		return;
	MSG_WriteByte (&sv.datagram, svc_particle3);
	MSG_WriteCoord (&sv.datagram, org[0]);
	MSG_WriteCoord (&sv.datagram, org[1]);
	MSG_WriteCoord (&sv.datagram, org[2]);
	MSG_WriteByte (&sv.datagram, box[0]);
	MSG_WriteByte (&sv.datagram, box[1]);
	MSG_WriteByte (&sv.datagram, box[2]);

	MSG_WriteShort (&sv.datagram, color);
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, effect);
}

/*
==================
SV_StartParticle4

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle4 (vec3_t org, float radius, int color, int effect, int count)
{
	if (sv.datagram.cursize > MAX_DATAGRAM-13)
		return;
	MSG_WriteByte (&sv.datagram, svc_particle4);
	MSG_WriteCoord (&sv.datagram, org[0]);
	MSG_WriteCoord (&sv.datagram, org[1]);
	MSG_WriteCoord (&sv.datagram, org[2]);
	MSG_WriteByte (&sv.datagram, radius);

	MSG_WriteShort (&sv.datagram, color);
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, effect);
}

/*
==================
SV_StopSound
==================
*/
void SV_StopSound (edict_t *entity, int channel)
{
	int			ent;

	if (sv.datagram.cursize > MAX_DATAGRAM-4)
		return;

	ent = NUM_FOR_EDICT(entity);
	channel = (ent<<3) | channel;

	MSG_WriteByte (&sv.datagram, svc_stopsound);
	MSG_WriteShort (&sv.datagram, channel);
}

/*
==================
SV_UpdateSoundPos
==================
*/
void SV_UpdateSoundPos (edict_t *entity, int channel)
{
	int			ent;
	int			i;

	if (sv.datagram.cursize > MAX_DATAGRAM-4)
		return;

	ent = NUM_FOR_EDICT(entity);
	channel = (ent<<3) | channel;

	MSG_WriteByte (&sv.datagram, svc_sound_update_pos);
	MSG_WriteShort (&sv.datagram, channel);
	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i]));
}

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

/*
==================
SV_MaxSounds

Sound indices this protocol can express in svc_sound.  See server.h.
==================
*/
int SV_MaxSounds (void)
{
	if (sv_protocol <= PROTOCOL_RAVEN_111)
		return MAX_SOUNDS_OLD;		/* bare byte				*/
	if (sv_protocol < PROTOCOL_UH2_114)
		return MAX_SOUNDS_H2MP;		/* byte + SND_OVERFLOW			*/
	return MAX_SOUNDS_UH2;			/* byte + SND_OVERFLOW + SND_OVERFLOW2	*/
}

/*
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
already running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)
==================
*/
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation)
{
	int			sound_num, ent;
	int			i, field_mask;

	if (q_strcasecmp(sample,"misc/null.wav") == 0)
	{
		SV_StopSound(entity,channel);
		return;
	}

	if (volume < 0 || volume > 255)
		Host_Error ("%s: volume = %i", __func__, volume);

	if (attenuation < 0 || attenuation > 4)
		Host_Error ("%s: attenuation = %f", __func__, attenuation);

	if (channel < 0 || channel > 7)
		Host_Error ("%s: channel = %i", __func__, channel);

	if (sv.datagram.cursize > MAX_DATAGRAM-16)
		return;

// find precache number for sound
	for (sound_num = 1; sound_num < MAX_SOUNDS && sv.sound_precache[sound_num]; sound_num++)
	{
		if (!strcmp(sample, sv.sound_precache[sound_num]))
			break;
	}

	if (sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num])
	{
		Con_Printf ("%s: %s not precached\n", __func__, sample);
		return;
	}

	ent = NUM_FOR_EDICT(entity);

	channel = (ent<<3) | channel;

	field_mask = 0;
	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		field_mask |= SND_VOLUME;
	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		field_mask |= SND_ATTENUATION;
	if (sound_num >= SV_MaxSounds())
	{
		/* Precache is clamped to SV_MaxSounds(), so this only fires if
		 * sv_protocol changed under us after the map was precached. */
		Con_DPrintf("%s: protocol %i violation: %s sound_num == %i >= %i\n",
				__func__, sv_protocol, sample, sound_num, SV_MaxSounds());
		return;
	}
	/* Split the index across the byte and the two overflow mask bits.
	 * Both bits may be set at once on protocol 21 (=> +768); older
	 * protocols never reach that far because of the clamp above. */
	if (sound_num >= MAX_SOUNDS_H2MP)
	{
		field_mask |= SND_OVERFLOW2;
		sound_num -= MAX_SOUNDS_H2MP;
	}
	if (sound_num >= MAX_SOUNDS_OLD)
	{
		field_mask |= SND_OVERFLOW;
		sound_num -= MAX_SOUNDS_OLD;
	}

// directed messages go only to the entity the are targeted on
	MSG_WriteByte (&sv.datagram, svc_sound);
	MSG_WriteByte (&sv.datagram, field_mask);
	if (field_mask & SND_VOLUME)
		MSG_WriteByte (&sv.datagram, volume);
	if (field_mask & SND_ATTENUATION)
		MSG_WriteByte (&sv.datagram, attenuation*64);
	MSG_WriteShort (&sv.datagram, channel);
	MSG_WriteByte (&sv.datagram, sound_num);
	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i]));
}

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

/*
================
SV_SendServerinfo

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
static void SV_SendServerinfo (client_t *client)
{
	int			i;
	const char		**s;
	char			message[2048];

	MSG_WriteByte (&client->message, svc_print);
	sprintf (message, "%c\nVERSION %4.2f SERVER (%i CRC)", 2, ENGINE_VERSION, pr_crc);
	MSG_WriteString (&client->message,message);

	MSG_WriteByte (&client->message, svc_serverinfo);
	MSG_WriteLong (&client->message, sv_protocol);
	MSG_WriteByte (&client->message, svs.maxclients);

	if (!coop.integer && deathmatch.integer)
	{
		MSG_WriteByte (&client->message, GAME_DEATHMATCH);
		if (sv_protocol > PROTOCOL_RAVEN_111)
			MSG_WriteShort (&client->message, sv_kingofhill);
	}
	else
		MSG_WriteByte (&client->message, GAME_COOP);

// send full levelname
	MSG_WriteString(&client->message, SV_GetLevelname ());

	/* i must advance with s: these loops relied purely on the NULL
	 * terminator, so a completely full precache array ran off the end. */
	for (i = 1, s = sv.model_precache + 1; i < MAX_MODELS && *s; s++, i++)
		MSG_WriteString (&client->message, *s);
	MSG_WriteByte (&client->message, 0);

	for (i = 1, s = sv.sound_precache + 1; i < MAX_SOUNDS && *s; s++, i++)
		MSG_WriteString (&client->message, *s);
	MSG_WriteByte (&client->message, 0);

	if (sv_protocol == PROTOCOL_UH2_114)
	{
		// send model effects
		for (i = 1, s = sv.model_precache + 1; i < MAX_MODELS && *s; s++)
		{
			#if !defined(SERVERONLY) && defined(GLQUAKE)
			if ((sv.models[i] != NULL) && (sv.models[i]->ex_flags != 0))
			{
				MSG_WriteString(&client->message, *s);
				MSG_WriteShort(&client->message, sv.models[i]->ex_flags);
				MSG_WriteFloat(&client->message, sv.models[i]->glow_settings[COLOR_R]);
				MSG_WriteFloat(&client->message, sv.models[i]->glow_settings[COLOR_G]);
				MSG_WriteFloat(&client->message, sv.models[i]->glow_settings[COLOR_B]);
				MSG_WriteFloat(&client->message, sv.models[i]->glow_settings[COLOR_A]);
			}
			#endif
			i++;
		}
		MSG_WriteByte(&client->message, 0);

		// send inventory extension info
		for (i = 0; i < sv.num_ex_items; i++)
		{
			//only send new/changed artifacts
			if ((!strcmp(va("gfx/arti%02d.lmp", sv.ex_items[i].id), sv.ex_items[i].icon)) || (sv.ex_items[i].id > 15))
			{
				MSG_WriteByte(&client->message, sv.ex_items[i].id);
				MSG_WriteString(&client->message, sv.ex_items[i].icon);
			}
		}
		MSG_WriteByte(&client->message, 0);
	}

// send music
	MSG_WriteByte (&client->message, svc_cdtrack);
	MSG_WriteByte (&client->message, sv.cd_track);
	MSG_WriteByte (&client->message, sv.cd_track);

	MSG_WriteByte (&client->message, svc_midi_name);
	MSG_WriteString (&client->message, sv.midi_name);

	if (sv_protocol >= PROTOCOL_UQE_113)
	{
		MSG_WriteByte (&client->message, svc_mod_name);
		MSG_WriteString (&client->message, sv.mod_name);
		MSG_WriteByte (&client->message, svc_skybox);
		MSG_WriteString (&client->message, sv.skybox);
	}

// set view
	MSG_WriteByte (&client->message, svc_setview);
	MSG_WriteShort (&client->message, NUM_FOR_EDICT(client->edict));

	MSG_WriteByte (&client->message, svc_signonnum);
	MSG_WriteByte (&client->message, 1);

	client->sendsignon = true;
	client->spawned = false;	// need prespawn, spawn, etc
	/* Void anything the previous map's signon list still owed this client.
	 * We have just told it to start over (svc_signonnum 1), and its next
	 * prespawn sets the index to 0; leaving a stale index here would make a
	 * changelevel that lands between prespawn and spawn feed the new list
	 * from the middle, silently dropping the baselines and static entities
	 * in the buffers it skipped.  uhexen2-z5wt. */
	client->signon_buffer = -1;
}

/*
================
SV_ConnectClient

Initializes a client_t for a new net connection.  This will only be called
once for a player each game, not once for each level change.
================
*/
static void SV_ConnectClient (int clientnum)
{
	edict_t			*ent;
	client_t		*client;
	int				edictnum;
	struct qsocket_s *netconnection;
	int			i;
	float			spawn_parms[NUM_SPAWN_PARMS];

	client = svs.clients + clientnum;

	Con_DPrintf ("Client %s connected\n", NET_QSocketGetAddressString(client->netconnection));

	edictnum = clientnum+1;

	ent = EDICT_NUM(edictnum);

// set up the client_t
	netconnection = client->netconnection;

	if (sv.loadgame)
		memcpy (spawn_parms, client->spawn_parms, sizeof(spawn_parms));
	memset (client, 0, sizeof(*client));
	client->send_all_v = true;
	client->netconnection = netconnection;

	strcpy (client->name, "unconnected");
	client->active = true;
	client->spawned = false;
	client->signon_buffer = -1;	/* nothing owed until prespawn asks */
	client->edict = ent;

	client->ex_inventory = SV_ClientInventoryPage (clientnum);

	SZ_Init (&client->message, client->msgbuf, sizeof(client->msgbuf));
	client->message.allowoverflow = true;	// we can catch it
	SZ_Init (&client->datagram, client->datagram_buf, sizeof(client->datagram_buf));

	memset(&sv.states[clientnum], 0, sizeof(client_state2_t ));

	if (sv.loadgame)
		memcpy (client->spawn_parms, spawn_parms, sizeof(spawn_parms));
//	else
//	{
	// call the progs to get default spawn parms for the new client
	//	PR_ExecuteProgram (*sv_globals.SetNewParms);
	//	for (i = 0; i < NUM_SPAWN_PARMS; i++)
	//		client->spawn_parms[i] = sv_globals.parm[i];
//	}

	SV_SendServerinfo (client);
}


/*
===================
SV_CheckForNewClients

===================
*/
void SV_CheckForNewClients (void)
{
	struct qsocket_s	*ret;
	int				i;

//
// check for new connections
//
	while (1)
	{
		ret = NET_CheckNewConnections ();
		if (!ret)
			break;

	//
	// init a new client structure
	//
		for (i = 0; i < svs.maxclients; i++)
		{
			if (!svs.clients[i].active)
				break;
		}

		if (i == svs.maxclients)
			Sys_Error ("%s: no free clients", __func__);

		svs.clients[i].netconnection = ret;
		SV_ConnectClient (i);

		net_activeconnections++;
	}
}


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

/*
==================
SV_ClearDatagram

==================
*/
void SV_ClearDatagram (void)
{
	SZ_Clear (&sv.datagram);
	sv_datagram_dropped = false;
}

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static int	fatbytes;
static byte	fatpvs[MAX_MAP_LEAFS/8];

static void SV_AddToFatPVS (vec3_t org, mnode_t *node)
{
	int		i;
	byte	*pvs;
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != CONTENTS_SOLID)
			{
				pvs = Mod_LeafPVS ( (mleaf_t *)node, sv.worldmodel);
				for (i = 0; i < fatbytes; i++)
					fatpvs[i] |= pvs[i];
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_AddToFatPVS (org, node->children[0]);
			node = node->children[1];
		}
	}
}

/*
=============
SV_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
static byte *SV_FatPVS (vec3_t org)
{
	fatbytes = (sv.worldmodel->numleafs+31)>>3;
	memset (fatpvs, 0, fatbytes);
	SV_AddToFatPVS (org, sv.worldmodel->nodes);
	return fatpvs;
}

#define CLIENT_FRAME_INIT	255
#define CLIENT_FRAME_RESET	254

/*
 * Room held back at the end of the per-frame datagram for the svc_clear_edicts
 * trailer that SV_PrepareClientEntities always writes: the opcode, the count
 * byte, and up to MAX_CLEAR_EDICTS_PER_MSG shorts.  Removals are collected all
 * through the entity loop, so the worst case has to be reserved up front --
 * by the time the list is full the message is already written.
 */
#define DATAGRAM_TRAILER_RESERVE	(2 + 2 * MAX_CLEAR_EDICTS_PER_MSG)

/*
 * Exact size of the entity update that the writer below emits for `bits`.
 * Computed rather than estimated: U_SKIN and U_SCALE carry two bytes each,
 * U_LONGENTITY and the U_MOREBITS chain change the header length, and every
 * optional field is independently present or absent, so a single worst-case
 * number would shed entities that would have fit.  Must be kept in step with
 * the write block in SV_PrepareClientEntities.
 */
static int SV_EntityUpdateSize (int bits)
{
	int	len;

	len = 1;					/* bits | U_SIGNAL */
	if (bits & U_MOREBITS)		len += 1;
	if (bits & U_MOREBITS2)		len += 1;
	len += (bits & U_LONGENTITY) ? 2 : 1;	/* entity number */

	if (bits & U_MODEL)		len += 2;
	if (bits & U_FRAME)		len += 1;
	if (bits & U_COLORMAP)		len += 1;
	if (bits & U_SKIN)		len += 2;	/* skin + drawflags */
	if (bits & U_EFFECTS)		len += 1;
	if (bits & U_ORIGIN1)		len += 2;
	if (bits & U_ANGLE1)		len += 1;
	if (bits & U_ORIGIN2)		len += 2;
	if (bits & U_ANGLE2)		len += 1;
	if (bits & U_ORIGIN3)		len += 2;
	if (bits & U_ANGLE3)		len += 1;
	if (bits & U_SCALE)		len += 2;	/* scale + abslight */
	if (bits & U_ALPHA)		len += 1;

	return len;
}

/* Largest value SV_EntityUpdateSize() can return, i.e. every optional field
 * present at once.  Used only to decide, before doing any work, whether this
 * frame could possibly overflow the datagram at all. */
#define NETSORT_MAX_UPDATE	24

/* Ironwail compresses the priority key into 256 bins so a counting sort can
 * order the whole entity list in one pass.  We cannot sort the write order
 * (see SV_PrepareClientEntities), but the same 256 bins give us the cutoff. */
#define NETSORT_BINS		256

/*
 * Ironwail's entity priority key, from SV_WriteEntitiesToClient: the scaled
 * fourth root of (squared distance from the viewer to the entity's bounding
 * box) over (the box's squared diagonal), so that a large distant brush entity
 * -- a lift, a rotator -- outranks a small nearby gib.  The fourth root is
 * what squeezes a map-sized range of distances into 0..255.
 *
 * Lower is more important.  The client's own entity is pinned at 0.
 */
static int SV_EntityPriority (edict_t *ent, const vec3_t org, qboolean is_clent)
{
	float	dist, size, delta;
	int	i;

	if (is_clent)
		return 0;

	dist = size = 0.0f;
	for (i = 0; i < 3; i++)
	{
		delta = CLAMP(ent->v.absmin[i], org[i], ent->v.absmax[i]) - org[i];
		dist += delta * delta;
		delta = ent->v.absmax[i] - ent->v.absmin[i];
		size += delta * delta;
	}

	if (size < 1.0f)
		size = 1.0f;

	dist = 8.0f * sqrt(sqrt(dist / size));
	if (dist >= (float)(NETSORT_BINS - 1))
		return NETSORT_BINS - 1;

	return (int) dist;
}

static void SV_PrepareClientEntities (client_t *client, edict_t	*clent, sizebuf_t *msg)
{
	int		e, i;
	int		bits;
	byte	*pvs;
	vec3_t	org;
	float	miss;
	edict_t	*ent;
	int		temp_index;
	char	NewName[MAX_QPATH];
	long	flagtest;
	int			position = 0;
	int			client_num;
	client_frames_t	*reference, *build;
	client_state2_t	*state;
	entity_state2_t	*ref_ent, *set_ent, build_ent, saved_ref;
	qboolean		FoundInList,DoRemove,DoPlayer,DoMonsters,DoMissiles,DoMisc,IgnoreEnt;
	short			RemoveList[MAX_CLIENT_STATES],NumToRemove;
	int			NumDeferred;
	int			NumShed;
	int			NumSorted;
	int			ShedNearest;
	qboolean		measuring;
	int			netsort_cutoff, bcount, cursize, prio, bytes;
	int			binbytes[NETSORT_BINS];
	entity_state2_t		scratch_ref, scratch_set;

	client_num = client-svs.clients;
	state = &sv.states[client_num];
	reference = &state->frames[0];

	if (client->last_sequence != client->current_sequence)
	{	// Old sequence
	//	Con_Printf("SV: Old sequence SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame++;
		if (client->current_frame > MAX_FRAMES+1)
			client->current_frame = MAX_FRAMES+1;
	}
	else if (client->last_frame == CLIENT_FRAME_INIT ||
			 client->last_frame == 0 ||
			 client->last_frame == MAX_FRAMES+1)
	{	// Reference expired in current sequence
	//	Con_Printf("SV: Expired SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame = 1;
		client->current_sequence++;
	}
	else if (client->last_frame >= 1 && client->last_frame <= client->current_frame)
	{	// Got a valid frame
	//	Con_Printf("SV: Valid SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		*reference = state->frames[client->last_frame];

		for (i = 0; i < reference->count; i++)
		{
			if (reference->states[i].flags & ENT_CLEARED)
			{
				e = reference->states[i].index;
				ent = EDICT_NUM(e);
				if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
				{
					ent->baseline.ClearCount[client_num]++;
				}
				else if (ent->baseline.ClearCount[client_num] == CLEAR_LIMIT)
				{
					ent->baseline.ClearCount[client_num] = 3;
					reference->states[i].flags &= ~ENT_CLEARED;
				}
			}
		}
		client->current_frame = 1;
		client->current_sequence++;
	}
	else
	{	// Normal frame advance
	//	Con_Printf("SV: Normal SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame++;
		if (client->current_frame > MAX_FRAMES+1)
			client->current_frame = MAX_FRAMES+1;
	}

	DoPlayer = DoMonsters = DoMissiles = DoMisc = false;

	if (sv_update_player.integer)
		DoPlayer = (client->current_sequence % sv_update_player.integer) == 0;
	if (sv_update_monsters.integer)
		DoMonsters = (client->current_sequence % sv_update_monsters.integer) == 0;
	if (sv_update_missiles.integer)
		DoMissiles = (client->current_sequence % sv_update_missiles.integer) == 0;
	if (sv_update_misc.integer)
		DoMisc = (client->current_sequence % sv_update_misc.integer) == 0;

	build = &state->frames[client->current_frame];
	memset(build, 0, sizeof(*build));
	client->last_frame = CLIENT_FRAME_RESET;

	MSG_WriteByte (msg, svc_reference);
	MSG_WriteByte (msg, client->current_frame);
	MSG_WriteByte (msg, client->current_sequence);

	// find the client's PVS
	if (clent->v.cameramode)
	{
		ent = PROG_TO_EDICT(clent->v.cameramode);
		VectorCopy(ent->v.origin, org);
	}
	else
		VectorAdd (clent->v.origin, clent->v.view_ofs, org);

	pvs = SV_FatPVS (org);

	/*
	 * sv_netsort.  Ironwail counting-sorts the entities by priority and then
	 * writes them best-first, so the packet filling up IS the cutoff.  We
	 * cannot do that: this is a reference-frame delta protocol, the walk over
	 * reference->states[] below is a monotonic cursor, and build->states[]
	 * becomes the next reference -- both have to stay in ascending edict
	 * order.  The write order is not ours to choose.
	 *
	 * The membership is.  So the loop below runs twice when the frame might
	 * overflow: once measuring, which commits nothing -- no writes to msg, no
	 * ENT_CLEARED rewrite of the reference frame, no build->states[] -- and
	 * only totals the exact byte cost of every update per priority bin; then
	 * once for real, shedding the entities whose bin falls past the point
	 * where those totals ran out of datagram.
	 *
	 * Measuring is skipped when the frame cannot overflow even if every edict
	 * sent its largest possible update, which is the ordinary case.  The
	 * cutoff then stays past the last bin, nothing is shed for priority, and
	 * the write pass is byte-for-byte what it was before -- as it also is
	 * under sv_netsort 0.
	 *
	 * Only whole bins are admitted.  Letting the bin the budget runs out in
	 * through as well and leaning on the guard to truncate it measurably
	 * loses entities that netsort is there to keep: its members are the worst
	 * of the admitted set, and any of them early in edict order spends budget
	 * that a better entity later in edict order then has to be shed for.
	 * Stopping a bin short instead leaves a sliver of the datagram unused,
	 * which costs nothing anyone can see.
	 *
	 * The exact per-entity guard further down remains the backstop either
	 * way, and carries the one case the bins cannot help with: a first bin
	 * that overflows the budget on its own, where there is nothing to choose
	 * between the candidates and edict order is as fair a tiebreak as any.
	 */
	netsort_cutoff = NETSORT_BINS;
	measuring = sv_netsort.integer &&
			msg->cursize + sv.num_edicts * NETSORT_MAX_UPDATE
				+ DATAGRAM_TRAILER_RESERVE > msg->maxsize;

restart:
	NumToRemove = 0;
	NumDeferred = 0;
	NumShed = 0;
	NumSorted = 0;
	ShedNearest = NETSORT_BINS;
	position = 0;
	bcount = 0;
	cursize = msg->cursize;
	memset (binbytes, 0, sizeof(binbytes));

	// send over all entities (except the client) that touch the pvs
	ent = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, ent = NEXT_EDICT(ent))
	{
		DoRemove = false;
		// don't send if flagged for NODRAW and there are no lighting effects
		if (ent->v.effects == EF_NODRAW)
		{
			DoRemove = true;
			goto skipA;
		}

		// ignore if not touching a PV leaf
		if (ent != clent)	// clent is ALWAYS sent
		{	// ignore ents without visible models
			if (!ent->v.modelindex || !*PR_GetString(ent->v.model))
			{
				DoRemove = true;
				goto skipA;
			}

			for (i = 0; i < ent->num_leafs; i++)
			{
				if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i] & 7)) )
					break;
			}

			/* If num_leafs == MAX_ENT_LEAFS the leaf list overflowed —
			 * we don't know what the rest of the entity's leaves are,
			 * so we can't safely PVS-cull.  Always send.  Common with
			 * long lifts, rotators, and big brush ents that span many
			 * leaves.  Matches ericw/Ironwail behaviour. */
			if (i == ent->num_leafs && ent->num_leafs < MAX_ENT_LEAFS)
			{
				DoRemove = true;
				goto skipA;
			}
		}

skipA:
		IgnoreEnt = false;
		flagtest = (long)ent->v.flags;
		if (!DoRemove)
		{
			if (flagtest & FL_CLIENT)
			{
				if (!DoPlayer)
					IgnoreEnt = true;
			}
			else if (flagtest & FL_MONSTER)
			{
				if (!DoMonsters)
					IgnoreEnt = true;
			}
			else if (ent->v.movetype == MOVETYPE_FLYMISSILE ||
					 ent->v.movetype == MOVETYPE_BOUNCEMISSILE ||
					 ent->v.movetype == MOVETYPE_BOUNCE)
			{
				if (!DoMissiles)
					IgnoreEnt = true;
			}
			else
			{
				if (!DoMisc)
					IgnoreEnt = true;
			}
		}

		bits = 0;

		while (position < reference->count && 
			   e > reference->states[position].index)
			position++;

		if (position < reference->count && reference->states[position].index == e)
		{
			FoundInList = true;
			if (DoRemove)
			{
				if (NumToRemove < MAX_CLEAR_EDICTS_PER_MSG)
				{
					RemoveList[NumToRemove] = e;
					NumToRemove++;
					continue;
				}

				/* Remove list is full for this message.  svc_clear_edicts
				 * describes its count in one byte, so removals past
				 * MAX_CLEAR_EDICTS_PER_MSG cannot be expressed on the wire.
				 * Fall through as if the entity were merely ignored: it is
				 * kept in the build list, so it survives in the client's
				 * reference frame and is removed by a later message.
				 * Dropping it here instead would leave it out of the
				 * reference with BE_ON still set on the client, stranding it
				 * as a permanent ghost. */
				NumDeferred++;
				IgnoreEnt = true;
			}
			ref_ent = &reference->states[position];
			if (measuring)
			{	/* Off a copy: the ENT_CLEARED rewrite below must not reach
				 * the real reference frame from a pass whose updates are
				 * never sent. */
				scratch_ref = *ref_ent;
				ref_ent = &scratch_ref;
			}
		}
		else
		{
			if (DoRemove || IgnoreEnt)
				continue;

			ref_ent = &build_ent;

			build_ent.index = e;
			build_ent.origin[0] = ent->baseline.origin[0];
			build_ent.origin[1] = ent->baseline.origin[1];
			build_ent.origin[2] = ent->baseline.origin[2];
			build_ent.angles[0] = ent->baseline.angles[0];
			build_ent.angles[1] = ent->baseline.angles[1];
			build_ent.angles[2] = ent->baseline.angles[2];
			build_ent.modelindex = ent->baseline.modelindex;
			build_ent.frame = ent->baseline.frame;
			build_ent.colormap = ent->baseline.colormap;
			build_ent.skin = ent->baseline.skin;
			build_ent.effects = ent->baseline.effects;
			build_ent.scale = ent->baseline.scale;
			build_ent.drawflags = ent->baseline.drawflags;
			build_ent.abslight = ent->baseline.abslight;
			build_ent.alpha = ent->baseline.alpha;
			build_ent.flags = 0;

			FoundInList = false;
		}

		/* Snapshot the reference entry before the ENT_CLEARED memset below
		 * rewrites it: if this entity's update turns out not to fit in the
		 * datagram, nothing was sent, so the reference has to go back to
		 * describing what the client actually holds. */
		saved_ref = *ref_ent;

		set_ent = measuring ? &scratch_set : &build->states[bcount];
		bcount++;
		if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
		{
			memset(ref_ent, 0, sizeof(*ref_ent));
			ref_ent->index = e;
		}
		*set_ent = *ref_ent;

		if (IgnoreEnt)
			continue;

		// send an update
		for (i = 0; i < 3; i++)
		{
			miss = ent->v.origin[i] - ref_ent->origin[i];
			if ( miss < -0.1 || miss > 0.1 )
			{
				bits |= U_ORIGIN1<<i;
				set_ent->origin[i] = ent->v.origin[i];
			}
		}

		if ( ent->v.angles[0] != ref_ent->angles[0] )
		{
			bits |= U_ANGLE1;
			set_ent->angles[0] = ent->v.angles[0];
		}

		if ( ent->v.angles[1] != ref_ent->angles[1] )
		{
			bits |= U_ANGLE2;
			set_ent->angles[1] = ent->v.angles[1];
		}

		if ( ent->v.angles[2] != ref_ent->angles[2] )
		{
			bits |= U_ANGLE3;
			set_ent->angles[2] = ent->v.angles[2];
		}

		if (ent->v.movetype == MOVETYPE_STEP)
			bits |= U_NOLERP;	// don't mess up the step animation

		if (ref_ent->colormap != ent->v.colormap)
		{
			bits |= U_COLORMAP;
			set_ent->colormap = ent->v.colormap;
		}

		if (ref_ent->skin != ent->v.skin
			|| ref_ent->drawflags != ent->v.drawflags)
		{
			bits |= U_SKIN;
			set_ent->skin = ent->v.skin;
			set_ent->drawflags = ent->v.drawflags;
		}

		if (ref_ent->frame != ent->v.frame)
		{
			bits |= U_FRAME;
			set_ent->frame = ent->v.frame;
		}

		if (ref_ent->effects != ent->v.effects)
		{
			bits |= U_EFFECTS;
			set_ent->effects = ent->v.effects;
		}

	//	flagtest = (long)ent->v.flags;
		if (flagtest & 0xff000000)
		{
			Host_Error("Invalid flags setting for class %s", PR_GetString(ent->v.classname));
			return;
		}

		temp_index = ent->v.modelindex;
		if (((int)ent->v.flags & FL_CLASS_DEPENDENT) && ent->v.model)
		{
			strcpy (NewName, PR_GetString(ent->v.model));
			NewName[strlen(NewName)-5] = client->playerclass + 48;
			temp_index = SV_ModelIndex (NewName);
		}

		if (ref_ent->modelindex != temp_index)
		{
			bits |= U_MODEL;
			set_ent->modelindex = temp_index;
		}

		if ( ref_ent->scale != ((int)(ent->v.scale * 100.0) & 255)
			|| ref_ent->abslight != ((int)(ent->v.abslight * 255.0) & 255) )
		{
			bits |= U_SCALE;
			set_ent->scale = ((int)(ent->v.scale * 100.0) & 255);
			set_ent->abslight = (int)(ent->v.abslight * 255.0) & 255;
		}

		{
			byte newalpha;
			eval_t *val = GetEdictFieldValue(ent, "alpha");
			newalpha = val ? ENTALPHA_ENCODE(val->_float) : ENTALPHA_DEFAULT;
			if (ref_ent->alpha != newalpha)
			{
				bits |= U_ALPHA;
				set_ent->alpha = newalpha;
			}
		}

		if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
		{
			bits |= U_CLEAR_ENT;
			set_ent->flags |= ENT_CLEARED;
		}

		if (!bits && FoundInList)
		{
			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

		if (e >= 256)
			bits |= U_LONGENTITY;

		if (bits >= 256)
			bits |= U_MOREBITS;

		if (bits >= 65536)
			bits |= U_MOREBITS2;

		bytes = SV_EntityUpdateSize(bits);

		/* Only worth the two square roots when it can change the outcome:
		 * while measuring, or when measuring came back with a real cutoff. */
		prio = (measuring || netsort_cutoff < NETSORT_BINS)
			? SV_EntityPriority (ent, org, ent == clent) : 0;

		if (measuring)
		{
			/* Total the demand; do not truncate it.  The cutoff can only be
			 * computed from what the frame actually wants to send, so this
			 * pass deliberately runs past the point where the datagram is
			 * full. */
			binbytes[prio] += bytes;

			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

		/* Datagram size guard, and the netsort cutoff that decides who meets
		 * it first.  The buffer is a fixed NET_MAXMESSAGE stack buffer with
		 * allowoverflow false, so without the size test the first delta that
		 * does not fit takes the server down inside SZ_GetSpace.  Shed the
		 * entity instead -- and prefer to shed it for being unimportant,
		 * while there is still room to spare, over shedding whoever happens
		 * to arrive once the datagram is already full.
		 *
		 * This has to undo the build->states[] bookkeeping in the same breath
		 * as it skips the write: build->states[] is the client's next
		 * reference frame and must record exactly the set that went out.
		 * Recording a delta that was never written desynchronises the
		 * reference and reaches the player as "Illegible server message"
		 * (uhexen2-6ugh).  How it is undone depends on whether the client
		 * already knows the entity:
		 *
		 *   FoundInList  - it is in the client's reference and may later need
		 *                  an svc_clear_edicts removal, which only happens for
		 *                  entities still in the reference.  Keep it, holding
		 *                  its unmodified reference state, exactly as the
		 *                  IgnoreEnt path does.  Dropping it would strand it
		 *                  as a permanent ghost.
		 *   !FoundInList - the client has never heard of it.  Drop it, so the
		 *                  next frame offers it again as a new entity.  Keeping
		 *                  it would claim the client holds it at baseline, and
		 *                  a subsequent zero-delta frame would then skip it and
		 *                  leave it invisible until it next changed.
		 *
		 * DATAGRAM_TRAILER_RESERVE keeps room for the svc_clear_edicts trailer
		 * written after the loop. */
		if (prio >= netsort_cutoff ||
			cursize + bytes + DATAGRAM_TRAILER_RESERVE > msg->maxsize)
		{
			*ref_ent = saved_ref;
			NumShed++;
			if (prio >= netsort_cutoff)
				NumSorted++;
			/* The whole point of the sort, as one number: the best-priority
			 * entity this frame threw away.  Under sv_netsort it tracks the
			 * cutoff bin, since whole bins are admitted and the guard is left
			 * with nothing to do; under sv_netsort 0 it is whatever edict
			 * order happened to leave until last, which is routinely
			 * something in the player's face. */
			if (prio < ShedNearest)
				ShedNearest = prio;

			if (!FoundInList)
			{
				bcount--;
				continue;
			}

			*set_ent = saved_ref;
			/* No U_CLEAR_ENT went out, so this frame must not be counted
			 * as a delivered clear by the ClearCount bookkeeping above. */
			set_ent->flags &= ~ENT_CLEARED;

			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

	//
	// write the message
	//
		MSG_WriteByte (msg,bits | U_SIGNAL);

		if (bits & U_MOREBITS)
			MSG_WriteByte (msg, bits >> 8);
		if (bits & U_MOREBITS2)
			MSG_WriteByte (msg, bits >> 16);

		if (bits & U_LONGENTITY)
			MSG_WriteShort (msg, e);
		else
			MSG_WriteByte (msg, e);

		if (bits & U_MODEL)
			MSG_WriteShort (msg, temp_index);
		if (bits & U_FRAME)
			MSG_WriteByte (msg, ent->v.frame);
		if (bits & U_COLORMAP)
			MSG_WriteByte (msg, ent->v.colormap);
		if (bits & U_SKIN)
		{ // Used for skin and drawflags
			MSG_WriteByte(msg, ent->v.skin);
			MSG_WriteByte(msg, ent->v.drawflags);
		}
		if (bits & U_EFFECTS)
			MSG_WriteByte (msg, ent->v.effects);
		if (bits & U_ORIGIN1)
			MSG_WriteCoord (msg, ent->v.origin[0]);
		if (bits & U_ANGLE1)
			MSG_WriteAngle (msg, ent->v.angles[0]);
		if (bits & U_ORIGIN2)
			MSG_WriteCoord (msg, ent->v.origin[1]);
		if (bits & U_ANGLE2)
			MSG_WriteAngle (msg, ent->v.angles[1]);
		if (bits & U_ORIGIN3)
			MSG_WriteCoord (msg, ent->v.origin[2]);
		if (bits & U_ANGLE3)
			MSG_WriteAngle (msg, ent->v.angles[2]);
		if (bits & U_SCALE)
		{ // Used for scale and abslight
			MSG_WriteByte (msg, (int)(ent->v.scale * 100.0) & 255);
			MSG_WriteByte (msg, (int)(ent->v.abslight * 255.0) & 255);
		}
		if (bits & U_ALPHA)
			MSG_WriteByte (msg, set_ent->alpha);

		/* Resync rather than add: this keeps SV_EntityUpdateSize() a
		 * prediction the guard makes, not a second source of truth for how
		 * much of the datagram is gone. */
		cursize = msg->cursize;

		if (bcount >= MAX_CLIENT_STATES)
			break;
	}

	if (measuring)
	{
		int	budget, total;

		/* Nothing was written, so msg->cursize is still just the
		 * svc_reference header: everything after it, less the trailer, is
		 * what the entity updates have to fit into. */
		budget = msg->maxsize - DATAGRAM_TRAILER_RESERVE - msg->cursize;

		/* netsort_cutoff ends as the number of whole bins that fit, so the
		 * write pass sheds everything from that bin outwards.  Reaching
		 * NETSORT_BINS means the frame fits after all and nothing is shed
		 * for priority, which is the same write pass as sv_netsort 0. */
		total = 0;
		for (netsort_cutoff = 0; netsort_cutoff < NETSORT_BINS; netsort_cutoff++)
		{
			if (total + binbytes[netsort_cutoff] > budget)
				break;
			total += binbytes[netsort_cutoff];
		}

		/* Admit the nearest bin even when it alone does not fit: shedding the
		 * whole frame would be worse than letting the guard truncate it. */
		if (netsort_cutoff == 0)
			netsort_cutoff = 1;

		measuring = false;
		goto restart;
	}

	build->count = bcount;

	/* The loop above caps NumToRemove precisely because this count goes out
	 * as a byte while the entries go out as shorts.  Clamp again here so the
	 * two writes can never disagree: a truncated count followed by the full
	 * run of shorts desynchronises every later message in the packet, which
	 * reaches the player as an "Illegible server message" host error rather
	 * than as anything diagnosable (uhexen2-6ugh). */
	if (NumToRemove > MAX_CLEAR_EDICTS_PER_MSG)
		NumToRemove = MAX_CLEAR_EDICTS_PER_MSG;

	if (NumDeferred)
		Con_DPrintf ("%s: remove list full, %d edict(s) deferred\n",
				__thisfunc__, NumDeferred);

	if (NumShed)
		Con_DPrintf ("%s: datagram full at %d/%d bytes, %d edict(s) shed "
				"(%d by netsort, cutoff bin %d), nearest shed bin %d\n",
				__thisfunc__, msg->cursize, msg->maxsize, NumShed,
				NumSorted, netsort_cutoff, ShedNearest);

	MSG_WriteByte (msg, svc_clear_edicts);
	MSG_WriteByte (msg, NumToRemove);
	for (i = 0; i < NumToRemove; i++)
		MSG_WriteShort (msg, RemoveList[i]);
}

/*
=============
SV_CleanupEnts

=============
*/
static void SV_CleanupEnts (void)
{
	int		e;
	edict_t	*ent;

	ent = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, ent = NEXT_EDICT(ent))
	{
		ent->v.effects = (int)ent->v.effects & ~EF_MUZZLEFLASH;
	}
}

/*
==================
SV_WriteClientdataToMessage

==================
*/
void SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg)
{
	int	bits,sc1,sc2,sc3,sc4;
	byte	test;
	int	i;
	edict_t	*other;
	static	int next_update = 0;
	static	int next_count = 0;

//
// send a damage message
//
	if (ent->v.dmg_take || ent->v.dmg_save)
	{
		other = PROG_TO_EDICT(ent->v.dmg_inflictor);
		MSG_WriteByte (msg, svc_damage);
		MSG_WriteByte (msg, ent->v.dmg_save);
		MSG_WriteByte (msg, ent->v.dmg_take);
		for (i = 0; i < 3; i++)
			MSG_WriteCoord (msg, other->v.origin[i] + 0.5*(other->v.mins[i] + other->v.maxs[i]));

		ent->v.dmg_take = 0;
		ent->v.dmg_save = 0;
	}

//
// send the current viewpos offset from the view entity
//
	SV_SetIdealPitch ();	// how much to look up / down ideally

// a fixangle might get lost in a dropped packet.  Oh well.
	if (ent->v.fixangle)
	{
		MSG_WriteByte (msg, svc_setangle);
		for (i = 0; i < 3; i++)
			MSG_WriteAngle (msg, ent->v.angles[i] );
		ent->v.fixangle = 0;
	}

	bits = 0;

	if (client->send_all_v)
	{
		bits = SU_VIEWHEIGHT | SU_IDEALPITCH | SU_IDEALROLL | 
			SU_VELOCITY1 | (SU_VELOCITY1<<1) | (SU_VELOCITY1<<2) | 
			SU_PUNCH1 | (SU_PUNCH1<<1) | (SU_PUNCH1<<2) | SU_WEAPONFRAME | 
			SU_ARMOR | SU_WEAPON;
	}
	else
	{
		if (ent->v.view_ofs[2] != client->old_v.view_ofs[2])
			bits |= SU_VIEWHEIGHT;

		if (ent->v.idealpitch != client->old_v.idealpitch)
			bits |= SU_IDEALPITCH;

		if (ent->v.idealroll != client->old_v.idealroll)
			bits |= SU_IDEALROLL;

		for (i = 0; i < 3; i++)
		{
			if (ent->v.punchangle[i] != client->old_v.punchangle[i])
				bits |= (SU_PUNCH1<<i);
			if (ent->v.velocity[i] != client->old_v.velocity[i])
				bits |= (SU_VELOCITY1<<i);
		}

		if (ent->v.weaponframe != client->old_v.weaponframe)
			bits |= SU_WEAPONFRAME;

		if (ent->v.armorvalue != client->old_v.armorvalue)
			bits |= SU_ARMOR;

		if (ent->v.weaponmodel != client->old_v.weaponmodel)
			bits |= SU_WEAPON;
	}

// send the data

	//fjm: this wasn't in here b4, and the centerview command requires it.
	if ( (int)ent->v.flags & FL_ONGROUND)
		bits |= SU_ONGROUND;

	next_count++;
	if (next_count >= 3)
	{
		next_count = 0;
		next_update++;
		if (next_update > 11)
			next_update = 0;

		switch (next_update)
		{
			case 0:
				bits |= SU_VIEWHEIGHT;
				break;
			case 1:
				bits |= SU_IDEALPITCH;
				break;
			case 2:
				bits |= SU_IDEALROLL;
				break;
			case 3:
				bits |= SU_VELOCITY1;
				break;
			case 4:
				bits |= (SU_VELOCITY1<<1);
				break;
			case 5:
				bits |= (SU_VELOCITY1<<2);
				break;
			case 6:
				bits |= SU_PUNCH1;
				break;
			case 7:
				bits |= (SU_PUNCH1<<1);
				break;
			case 8:
				bits |= (SU_PUNCH1<<2);
				break;
			case 9:
				bits |= SU_WEAPONFRAME;
				break;
			case 10:
				bits |= SU_ARMOR;
				break;
			case 11:
				bits |= SU_WEAPON;
				break;
		}
	}

	MSG_WriteByte (msg, svc_clientdata);
	MSG_WriteShort (msg, bits);

	if (bits & SU_VIEWHEIGHT)
		MSG_WriteChar (msg, ent->v.view_ofs[2]);

	if (bits & SU_IDEALPITCH)
		MSG_WriteChar (msg, ent->v.idealpitch);

	if (bits & SU_IDEALROLL)
		MSG_WriteChar (msg, ent->v.idealroll);

	for (i = 0; i < 3; i++)
	{
		if (bits & (SU_PUNCH1<<i))
			MSG_WriteChar (msg, ent->v.punchangle[i]);
		if (bits & (SU_VELOCITY1<<i))
			MSG_WriteChar (msg, ent->v.velocity[i]/16);
	}

	if (bits & SU_WEAPONFRAME)
		MSG_WriteByte (msg, ent->v.weaponframe);
	if (bits & SU_ARMOR)
		MSG_WriteByte (msg, ent->v.armorvalue);
	if (bits & SU_WEAPON)
		MSG_WriteShort (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)));

	if (host_client->send_all_v)
	{
		sc1 = sc2 = 0xffffffff;
		host_client->send_all_v = false;
	}
	else
	{
		sc1 = sc2 = 0;
		if (sv_protocol == PROTOCOL_UH2_114)
		{
			sc3 = host_client->ex_inventory->changed_items;
			sc4 = host_client->ex_inventory->new_items;
		}
		else
			sc3 = sc4 = 0;

		if (ent->v.health != host_client->old_v.health)
			sc1 |= SC1_HEALTH;
		if (ent->v.level != host_client->old_v.level)
			sc1 |= SC1_LEVEL;
		if (ent->v.intelligence != host_client->old_v.intelligence)
			sc1 |= SC1_INTELLIGENCE;
		if (ent->v.wisdom != host_client->old_v.wisdom)
			sc1 |= SC1_WISDOM;
		if (ent->v.strength != host_client->old_v.strength)
			sc1 |= SC1_STRENGTH;
		if (ent->v.dexterity != host_client->old_v.dexterity)
			sc1 |= SC1_DEXTERITY;
		if (ent->v.weapon != host_client->old_v.weapon)
			sc1 |= SC1_WEAPON;
		if (ent->v.bluemana != host_client->old_v.bluemana)
			sc1 |= SC1_BLUEMANA;
		if (ent->v.greenmana != host_client->old_v.greenmana)
			sc1 |= SC1_GREENMANA;
		if (ent->v.experience != host_client->old_v.experience)
			sc1 |= SC1_EXPERIENCE;
		if (ent->v.cnt_torch != host_client->old_v.cnt_torch)
			sc1 |= SC1_CNT_TORCH;
		if (ent->v.cnt_h_boost != host_client->old_v.cnt_h_boost)
			sc1 |= SC1_CNT_H_BOOST;
		if (ent->v.cnt_sh_boost != host_client->old_v.cnt_sh_boost)
			sc1 |= SC1_CNT_SH_BOOST;
		if (ent->v.cnt_mana_boost != host_client->old_v.cnt_mana_boost)
			sc1 |= SC1_CNT_MANA_BOOST;
		if (ent->v.cnt_teleport != host_client->old_v.cnt_teleport)
			sc1 |= SC1_CNT_TELEPORT;
		if (ent->v.cnt_tome != host_client->old_v.cnt_tome)
			sc1 |= SC1_CNT_TOME;
		if (ent->v.cnt_summon != host_client->old_v.cnt_summon)
			sc1 |= SC1_CNT_SUMMON;
		if (ent->v.cnt_invisibility != host_client->old_v.cnt_invisibility)
			sc1 |= SC1_CNT_INVISIBILITY;
		if (ent->v.cnt_glyph != host_client->old_v.cnt_glyph)
			sc1 |= SC1_CNT_GLYPH;
		if (ent->v.cnt_haste != host_client->old_v.cnt_haste)
			sc1 |= SC1_CNT_HASTE;
		if (ent->v.cnt_blast != host_client->old_v.cnt_blast)
			sc1 |= SC1_CNT_BLAST;
		if (ent->v.cnt_polymorph != host_client->old_v.cnt_polymorph)
			sc1 |= SC1_CNT_POLYMORPH;
		if (ent->v.cnt_flight != host_client->old_v.cnt_flight)
			sc1 |= SC1_CNT_FLIGHT;
		if (ent->v.cnt_cubeofforce != host_client->old_v.cnt_cubeofforce)
			sc1 |= SC1_CNT_CUBEOFFORCE;
		if (ent->v.cnt_invincibility != host_client->old_v.cnt_invincibility)
			sc1 |= SC1_CNT_INVINCIBILITY;
		if (ent->v.artifact_active != host_client->old_v.artifact_active)
			sc1 |= SC1_ARTIFACT_ACTIVE;
		if (ent->v.artifact_low != host_client->old_v.artifact_low)
			sc1 |= SC1_ARTIFACT_LOW;
		if (ent->v.movetype != host_client->old_v.movetype)
			sc1 |= SC1_MOVETYPE;
		if (ent->v.cameramode != host_client->old_v.cameramode)
			sc1 |= SC1_CAMERAMODE;
		if (ent->v.hasted != host_client->old_v.hasted)
			sc1 |= SC1_HASTED;
		if (ent->v.inventory != host_client->old_v.inventory)
			sc1 |= SC1_INVENTORY;
		if (ent->v.rings_active != host_client->old_v.rings_active)
			sc1 |= SC1_RINGS_ACTIVE;

		if (ent->v.rings_low != host_client->old_v.rings_low)
			sc2 |= SC2_RINGS_LOW;
		if (ent->v.armor_amulet != host_client->old_v.armor_amulet)
			sc2 |= SC2_AMULET;
		if (ent->v.armor_bracer != host_client->old_v.armor_bracer)
			sc2 |= SC2_BRACER;
		if (ent->v.armor_breastplate != host_client->old_v.armor_breastplate)
			sc2 |= SC2_BREASTPLATE;
		if (ent->v.armor_helmet != host_client->old_v.armor_helmet)
			sc2 |= SC2_HELMET;
		if (ent->v.ring_flight != host_client->old_v.ring_flight)
			sc2 |= SC2_FLIGHT_T;
		if (ent->v.ring_water != host_client->old_v.ring_water)
			sc2 |= SC2_WATER_T;
		if (ent->v.ring_turning != host_client->old_v.ring_turning)
			sc2 |= SC2_TURNING_T;
		if (ent->v.ring_regeneration != host_client->old_v.ring_regeneration)
			sc2 |= SC2_REGEN_T;
		if (ent->v.haste_time != host_client->old_v.haste_time)
			sc2 |= SC2_HASTE_T;
		if (ent->v.tome_time != host_client->old_v.tome_time)
			sc2 |= SC2_TOME_T;
		if (ent->v.puzzle_inv1 != host_client->old_v.puzzle_inv1)
			sc2 |= SC2_PUZZLE1;
		if (ent->v.puzzle_inv2 != host_client->old_v.puzzle_inv2)
			sc2 |= SC2_PUZZLE2;
		if (ent->v.puzzle_inv3 != host_client->old_v.puzzle_inv3)
			sc2 |= SC2_PUZZLE3;
		if (ent->v.puzzle_inv4 != host_client->old_v.puzzle_inv4)
			sc2 |= SC2_PUZZLE4;
		if (ent->v.puzzle_inv5 != host_client->old_v.puzzle_inv5)
			sc2 |= SC2_PUZZLE5;
		if (ent->v.puzzle_inv6 != host_client->old_v.puzzle_inv6)
			sc2 |= SC2_PUZZLE6;
		if (ent->v.puzzle_inv7 != host_client->old_v.puzzle_inv7)
			sc2 |= SC2_PUZZLE7;
		if (ent->v.puzzle_inv8 != host_client->old_v.puzzle_inv8)
			sc2 |= SC2_PUZZLE8;
		if (ent->v.max_health != host_client->old_v.max_health)
			sc2 |= SC2_MAXHEALTH;
		if (ent->v.max_mana != host_client->old_v.max_mana)
			sc2 |= SC2_MAXMANA;
		if (ent->v.flags != host_client->old_v.flags)
			sc2 |= SC2_FLAGS;

		// mission pack, objectives
		if (sv_protocol > PROTOCOL_RAVEN_111)
		{
			if (info_mask != client->info_mask)
				sc2 |= SC2_OBJ;
			if (info_mask2 != client->info_mask2)
				sc2 |= SC2_OBJ2;
		}
	}

	if (!sc1 && !sc2 && !sc3 && !sc4)
		goto end;

	MSG_WriteByte (&host_client->message, svc_update_inv);
	test = 0;
	if (sc1 & 0x000000ff)
		test |= 1;
	if (sc1 & 0x0000ff00)
		test |= 2;
	if (sc1 & 0x00ff0000)
		test |= 4;
	if (sc1 & 0xff000000)
		test |= 8;
	if (sc2 & 0x000000ff)
		test |= 16;
	if (sc2 & 0x0000ff00)
		test |= 32;
	if (sc2 & 0x00ff0000)
		test |= 64;
	if (sc2 & 0xff000000)
		test |= 128;

	MSG_WriteByte (&host_client->message, test);

	if (test & 1)
		MSG_WriteByte (&host_client->message, sc1 & 0xff);
	if (test & 2)
		MSG_WriteByte (&host_client->message, (sc1 >> 8) & 0xff);
	if (test & 4)
		MSG_WriteByte (&host_client->message, (sc1 >> 16) & 0xff);
	if (test & 8)
		MSG_WriteByte (&host_client->message, (sc1 >> 24) & 0xff);
	if (test & 16)
		MSG_WriteByte (&host_client->message, sc2 & 0xff);
	if (test & 32)
		MSG_WriteByte (&host_client->message, (sc2 >> 8) & 0xff);
	if (test & 64)
		MSG_WriteByte (&host_client->message, (sc2 >> 16) & 0xff);
	if (test & 128)
		MSG_WriteByte (&host_client->message, (sc2 >> 24) & 0xff);

	if (sc1 & SC1_HEALTH)
		MSG_WriteShort (&host_client->message, ent->v.health);
	if (sc1 & SC1_LEVEL)
		MSG_WriteByte (&host_client->message, ent->v.level);
	if (sc1 & SC1_INTELLIGENCE)
		MSG_WriteByte (&host_client->message, ent->v.intelligence);
	if (sc1 & SC1_WISDOM)
		MSG_WriteByte (&host_client->message, ent->v.wisdom);
	if (sc1 & SC1_STRENGTH)
		MSG_WriteByte (&host_client->message, ent->v.strength);
	if (sc1 & SC1_DEXTERITY)
		MSG_WriteByte (&host_client->message, ent->v.dexterity);
	if (sc1 & SC1_WEAPON)
		MSG_WriteByte (&host_client->message, ent->v.weapon);
	if (sc1 & SC1_BLUEMANA)
		MSG_WriteByte (&host_client->message, ent->v.bluemana);
	if (sc1 & SC1_GREENMANA)
		MSG_WriteByte (&host_client->message, ent->v.greenmana);
	if (sc1 & SC1_EXPERIENCE)
		MSG_WriteLong (&host_client->message, ent->v.experience);
	if (sc1 & SC1_CNT_TORCH)
		MSG_WriteByte(&host_client->message, ent->v.cnt_torch), INV_UpdateExItem(host_client->ex_inventory, 1, (int)ent->v.cnt_torch, false);
	if (sc1 & SC1_CNT_H_BOOST)
		MSG_WriteByte(&host_client->message, ent->v.cnt_h_boost), INV_UpdateExItem(host_client->ex_inventory, 2, (int)ent->v.cnt_h_boost, false);
	if (sc1 & SC1_CNT_SH_BOOST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_sh_boost), INV_UpdateExItem(host_client->ex_inventory, 3, (int)ent->v.cnt_sh_boost, false);
	if (sc1 & SC1_CNT_MANA_BOOST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_mana_boost), INV_UpdateExItem(host_client->ex_inventory, 4, (int)ent->v.cnt_mana_boost, false);
	if (sc1 & SC1_CNT_TELEPORT)
		MSG_WriteByte (&host_client->message, ent->v.cnt_teleport), INV_UpdateExItem(host_client->ex_inventory, 5, (int)ent->v.cnt_teleport, false);
	if (sc1 & SC1_CNT_TOME)
		MSG_WriteByte (&host_client->message, ent->v.cnt_tome), INV_UpdateExItem(host_client->ex_inventory, 6, (int)ent->v.cnt_tome, false);
	if (sc1 & SC1_CNT_SUMMON)
		MSG_WriteByte (&host_client->message, ent->v.cnt_summon), INV_UpdateExItem(host_client->ex_inventory, 7, (int)ent->v.cnt_summon, false);
	if (sc1 & SC1_CNT_INVISIBILITY)
		MSG_WriteByte (&host_client->message, ent->v.cnt_invisibility), INV_UpdateExItem(host_client->ex_inventory, 8, (int)ent->v.cnt_invisibility, false);
	if (sc1 & SC1_CNT_GLYPH)
		MSG_WriteByte (&host_client->message, ent->v.cnt_glyph), INV_UpdateExItem(host_client->ex_inventory, 9, (int)ent->v.cnt_glyph, false);
	if (sc1 & SC1_CNT_HASTE)
		MSG_WriteByte (&host_client->message, ent->v.cnt_haste), INV_UpdateExItem(host_client->ex_inventory, 10, (int)ent->v.cnt_haste, false);
	if (sc1 & SC1_CNT_BLAST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_blast), INV_UpdateExItem(host_client->ex_inventory, 11, (int)ent->v.cnt_blast, false);
	if (sc1 & SC1_CNT_POLYMORPH)
		MSG_WriteByte (&host_client->message, ent->v.cnt_polymorph), INV_UpdateExItem(host_client->ex_inventory, 12, (int)ent->v.cnt_polymorph, false);
	if (sc1 & SC1_CNT_FLIGHT)
		MSG_WriteByte (&host_client->message, ent->v.cnt_flight), INV_UpdateExItem(host_client->ex_inventory, 13, (int)ent->v.cnt_flight, false);
	if (sc1 & SC1_CNT_CUBEOFFORCE)
		MSG_WriteByte (&host_client->message, ent->v.cnt_cubeofforce), INV_UpdateExItem(host_client->ex_inventory, 14, (int)ent->v.cnt_cubeofforce, false);
	if (sc1 & SC1_CNT_INVINCIBILITY)
		MSG_WriteByte (&host_client->message, ent->v.cnt_invincibility), INV_UpdateExItem(host_client->ex_inventory, 15, (int)ent->v.cnt_invincibility, false);
	if (sc1 & SC1_ARTIFACT_ACTIVE)
		MSG_WriteFloat (&host_client->message, ent->v.artifact_active);
	if (sc1 & SC1_ARTIFACT_LOW)
		MSG_WriteFloat (&host_client->message, ent->v.artifact_low);
	if (sc1 & SC1_MOVETYPE)
		MSG_WriteByte (&host_client->message, ent->v.movetype);
	if (sc1 & SC1_CAMERAMODE)
		MSG_WriteByte (&host_client->message, ent->v.cameramode);
	if (sc1 & SC1_HASTED)
		MSG_WriteFloat (&host_client->message, ent->v.hasted);
	if (sc1 & SC1_INVENTORY)
		MSG_WriteByte (&host_client->message, ent->v.inventory);
	if (sc1 & SC1_RINGS_ACTIVE)
		MSG_WriteFloat (&host_client->message, ent->v.rings_active);

	if (sc2 & SC2_RINGS_LOW)
		MSG_WriteFloat (&host_client->message, ent->v.rings_low);
	if (sc2 & SC2_AMULET)
		MSG_WriteByte (&host_client->message, ent->v.armor_amulet);
	if (sc2 & SC2_BRACER)
		MSG_WriteByte (&host_client->message, ent->v.armor_bracer);
	if (sc2 & SC2_BREASTPLATE)
		MSG_WriteByte (&host_client->message, ent->v.armor_breastplate);
	if (sc2 & SC2_HELMET)
		MSG_WriteByte (&host_client->message, ent->v.armor_helmet);
	if (sc2 & SC2_FLIGHT_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_flight);
	if (sc2 & SC2_WATER_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_water);
	if (sc2 & SC2_TURNING_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_turning);
	if (sc2 & SC2_REGEN_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_regeneration);
	if (sc2 & SC2_HASTE_T)
		MSG_WriteFloat (&host_client->message, ent->v.haste_time);
	if (sc2 & SC2_TOME_T)
		MSG_WriteFloat (&host_client->message, ent->v.tome_time);
	if (sc2 & SC2_PUZZLE1)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv1));
	if (sc2 & SC2_PUZZLE2)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv2));
	if (sc2 & SC2_PUZZLE3)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv3));
	if (sc2 & SC2_PUZZLE4)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv4));
	if (sc2 & SC2_PUZZLE5)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv5));
	if (sc2 & SC2_PUZZLE6)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv6));
	if (sc2 & SC2_PUZZLE7)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv7));
	if (sc2 & SC2_PUZZLE8)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv8));
	if (sc2 & SC2_MAXHEALTH)
		MSG_WriteShort (&host_client->message, ent->v.max_health);
	if (sc2 & SC2_MAXMANA)
		MSG_WriteByte (&host_client->message, ent->v.max_mana);
	if (sc2 & SC2_FLAGS)
		MSG_WriteFloat (&host_client->message, ent->v.flags);

// mission pack, objectives
	if (sv_protocol > PROTOCOL_RAVEN_111)
	{
		if (sc2 & SC2_OBJ)
		{
			MSG_WriteLong (&host_client->message, info_mask);
			client->info_mask = info_mask;
		}
		if (sc2 & SC2_OBJ2)
		{
			MSG_WriteLong (&host_client->message, info_mask2);
			client->info_mask2 = info_mask2;
		}
	}

// extended inventory
	if (sv_protocol == PROTOCOL_UH2_114)
	{
		//shan page loop here
		ex_inventory_page_t *page = host_client->ex_inventory;

		while (page != NULL)
		{
			sc3 = page->changed_items;
			sc4 = page->new_items;

			test = 0;
			if (sc3 & 0x000000ff)
				test |= 1;
			if (sc3 & 0x0000ff00)
				test |= 2;
			if (sc3 & 0x00ff0000)
				test |= 4;
			if (sc3 & 0xff000000)
				test |= 8;
			if (page->next != NULL)
				test |= 16;

			MSG_WriteByte(&host_client->message, test);

			if (test)
			{
				if (test & 1)
					MSG_WriteByte(&host_client->message, sc3 & 0xff);
				if (test & 2)
					MSG_WriteByte(&host_client->message, (sc3 >> 8) & 0xff);
				if (test & 4)
					MSG_WriteByte(&host_client->message, (sc3 >> 16) & 0xff);
				if (test & 8)
					MSG_WriteByte(&host_client->message, (sc3 >> 24) & 0xff);

				for (i = 0; i < MAX_INVENTORY_EX; i++)
				{
					if (sc3 & (1 << i))
					{
						MSG_WriteByte(&host_client->message, (page->item_cnt[i] + (((sc4 & (1 << i)) != 0)) * 128));
						if (sc4 & (1 << i))
						{
							MSG_WriteByte(&host_client->message, (page->item_id[i]));
						}
					}
				}

				page->changed_items = 0;
				page->new_items = 0;
			}

			page = page->next;
		}

	}

	
end:
	memcpy (&client->old_v, &ent->v, sizeof(client->old_v));
}

/*
=======================
SV_SendClientDatagram
=======================
*/
static qboolean SV_SendClientDatagram (client_t *client)
{
	byte		buf[NET_MAXMESSAGE];
	sizebuf_t	msg;
	int		wire, pending;

	/* Build into what this connection can actually carry, not into the size
	 * of the stack buffer.  Datagram_SendUnreliableMessage memcpys the result
	 * straight into a fixed MAX_DATAGRAM packet buffer and its only bounds
	 * check is compiled out of release builds, so the 32 KB that sizeof(buf)
	 * would authorise is not a generous limit -- it is memory corruption
	 * eight times over.  SV_PrepareClientEntities sheds entities to fit,
	 * worst-priority first, instead. */
	wire = NET_MaxUnreliableMessage (client->netconnection);
	SZ_Init (&msg, buf, wire);
	msg.name = "client datagram";

	MSG_WriteByte (&msg, svc_time);
	MSG_WriteFloat (&msg, sv.time);

// add the client specific data to the datagram
	SV_WriteClientdataToMessage (client, client->edict, &msg);

	/* Hold room for the two datagrams appended below before handing the rest
	 * to the entity deltas.  Entity state is re-sent every frame and survives
	 * being shed; a temp entity or a sound is sent once and is simply gone if
	 * it does not fit.  Never squeeze the entity section below half, though:
	 * QC that keeps sv.datagram permanently full would otherwise freeze the
	 * visible world rather than lose an effect. */
	pending = sv.datagram.cursize + client->datagram.cursize;
	if (pending > wire / 2)
		pending = wire / 2;
	msg.maxsize = wire - pending;

	SV_PrepareClientEntities (client, client->edict, &msg);

	msg.maxsize = wire;

/*	if ((rand() & 0xff) < 200)
	{
		return true;
	}
*/

// copy the server datagram if there is space
	if (msg.cursize + sv.datagram.cursize < msg.maxsize)
		SZ_Write (&msg, sv.datagram.data, sv.datagram.cursize);

	if (msg.cursize + client->datagram.cursize < msg.maxsize)
		SZ_Write (&msg, client->datagram.data, client->datagram.cursize);

	SZ_Clear(&client->datagram);

/*	if (msg.cursize > 300)
	{
		Con_DPrintf("WARNING: packet size is %i\n",msg.cursize);
	}
*/

#ifndef SERVERONLY
	/* devstats: the size this client's datagram actually reached, next to
	 * the wire limit it was built against.  There is already a commented-out
	 * "packet size is %i" warning just above from whoever wanted this
	 * number; this is the version you can leave on.
	 *
	 * Client build only, and not an oversight: devstats_t lives in client.h,
	 * which the dedicated build does not compile, and a dedicated server has
	 * no overlay to put the number on.  uhexen2-a5nn.34 */
	dev_stats.packetsize = msg.cursize;
#endif

// send the datagram
	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
	{
		SV_DropClient (true);// if the message couldn't send, kick off
		return false;
	}

	return true;
}

/*
=======================
SV_UpdateToReliableMessages
=======================
*/
static void SV_UpdateToReliableMessages (void)
{
	int		i, j;
	client_t	*client;
	edict_t		*ent;

// check for changes to be sent over the reliable streams
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		ent = host_client->edict;
		if (host_client->old_frags != ent->v.frags)
		{
			for (j = 0, client = svs.clients; j < svs.maxclients; j++, client++)
			{
				if (!client->active)
					continue;

				MSG_WriteByte (&client->message, svc_updatefrags);
				MSG_WriteByte (&client->message, i);
				MSG_WriteShort (&client->message, host_client->edict->v.frags);
			}

			host_client->old_frags = ent->v.frags;
		}
	}

	for (j = 0, client = svs.clients; j < svs.maxclients; j++, client++)
	{
		if (!client->active)
			continue;
		SZ_Write (&client->message, sv.reliable_datagram.data, sv.reliable_datagram.cursize);
	}

	SZ_Clear (&sv.reliable_datagram);
}


/*
=======================
SV_SendNop

Send a nop message without trashing or sending the accumulated client
message buffer
=======================
*/
static void SV_SendNop (client_t *client)
{
	sizebuf_t	msg;
	byte		buf[4];

	SZ_Init (&msg, buf, sizeof(buf));

	MSG_WriteChar (&msg, svc_nop);

	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
		SV_DropClient (true);	// if the message couldn't send, kick off
	client->last_message = realtime;
}

/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i;

// update frags, names, etc
	SV_UpdateToReliableMessages ();

// build individual updates
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (!host_client->active)
			continue;

		if (host_client->spawned)
		{
			if (!SV_SendClientDatagram (host_client))
				continue;
		}
		else
		{
		// the player isn't totally in the game yet
		// send small keepalive messages if too much time has passed
		// send a full message when the next signon stage has been requested
		// some other message data (name changes, etc) may accumulate
		// between signon stages

			/* Feed out the signon list, one buffer per reliable
			 * message.  Only once the previous one has actually
			 * gone -- a signon buffer is half of client->message,
			 * so stacking two, or one on top of queued name/colour
			 * updates, would overflow it.  uhexen2-z5wt. */
			if (host_client->signon_buffer >= 0 && !host_client->message.cursize)
				SV_SendSignonBuffer (host_client);

			if (!host_client->sendsignon)
			{
				if (realtime - host_client->last_message > 5)
					SV_SendNop (host_client);
				continue;	// don't send out non-signon messages
			}
		}

		// check for an overflowed message.  Should only happen
		// on a very fucked up connection that backs up a lot, then
		// changes level
		if (host_client->message.overflowed)
		{
			SV_DropClient (true);
			host_client->message.overflowed = false;
			continue;
		}

		if (host_client->message.cursize || host_client->dropasap)
		{
			if (!NET_CanSendMessage (host_client->netconnection))
			{
			//	I_Printf ("can't write\n");
				continue;
			}

			if (host_client->dropasap)
				SV_DropClient (false);	// went to another level
			else
			{
				if (NET_SendMessage (host_client->netconnection,
						&host_client->message) == -1)
					SV_DropClient (true);	// if the message couldn't send, kick off
				SZ_Clear (&host_client->message);
				host_client->last_message = realtime;
				host_client->sendsignon = false;
			}
		}
	}

// clear muzzle flashes
	SV_CleanupEnts ();
}


/*
==============================================================================

SERVER SPAWNING

==============================================================================
*/

/*
================
SV_ModelIndex

================
*/
int SV_ModelIndex (const char *name)
{
	int		i;

	if (!name || !name[0])
		return 0;

	for (i = 0; i < MAX_MODELS && sv.model_precache[i]; i++)
	{
		if (!strcmp(sv.model_precache[i], name))
			return i;
	}

	if (i == MAX_MODELS || !sv.model_precache[i])
	{
		Con_Printf("%s: model %s not precached\n", __func__, name);
		return 0;
	}

	return i;
}

/*
================
SV_SignonBufferCount
SV_SignonBufferSize

How many signon buffers there are and how long each one is.  The buffer being
filled is the last, and its length is sv.signon.cursize rather than an entry in
signon_size[] -- QC can still write to MSG_INIT after the map has spawned, so
sealing the length at spawn time would short a client that connected later.
================
*/
static int SV_SignonBufferCount (void)
{
	return sv.num_signon_buffers + 1;
}

static int SV_SignonBufferSize (int i)
{
	return (i < sv.num_signon_buffers) ? sv.signon_size[i] : sv.signon.cursize;
}

/*
================
SV_ReserveSignonSpace

Make room for a signon record of `length' bytes in one piece, starting a new
signon buffer if the current one cannot hold it.  Call it BEFORE writing any
part of the record: a client parses each reliable message on its own, so a
record split across two buffers would desync the stream.

Being generous with `length' is free -- over-reserving wastes at most the
difference once per buffer boundary, and never splits anything.
================
*/
void SV_ReserveSignonSpace (int length)
{
	if (length > MAX_SIGNON_SIZE)
		Sys_Error ("%s: %d byte record does not fit a %d byte signon buffer",
				__thisfunc__, length, MAX_SIGNON_SIZE);

	if (sv.signon.cursize + length <= sv.signon.maxsize)
		return;

	if (sv.num_signon_buffers + 1 >= MAX_SIGNON_BUFFERS)
		Host_Error ("Too much signon data for this map: %d buffers of %d "
			    "bytes are full.  Raise MAX_SIGNON_BUFFERS (server.h) "
			    "or cut static entities / ambient sounds.",
				MAX_SIGNON_BUFFERS, MAX_SIGNON_SIZE);

	sv.signon_size[sv.num_signon_buffers] = sv.signon.cursize;
	sv.num_signon_buffers++;
	SZ_Init (&sv.signon, sv.signon_bufs[sv.num_signon_buffers], MAX_SIGNON_SIZE);
	sv.signon.name = "sv.signon";
}

/*
================
SV_SendSignonBuffer

Hand a client the next signon buffer it is owed, and the signon-stage marker
once the last one has gone.  One buffer per reliable message: they do not all
fit in client->message at once, and the client is happy to take baselines and
statics spread over several messages -- only svc_signonnum advances its state.

Called from Host_PreSpawn_f for the first buffer and from SV_SendClientMessages
for the rest.
================
*/
void SV_SendSignonBuffer (client_t *client)
{
	int	i = client->signon_buffer;

	if (i < 0 || i >= SV_SignonBufferCount())
	{
		client->signon_buffer = -1;
		return;
	}

	SZ_Write (&client->message, sv.signon_bufs[i], SV_SignonBufferSize(i));

	if (i + 1 < SV_SignonBufferCount())
	{
		client->signon_buffer = i + 1;
	}
	else
	{
		client->signon_buffer = -1;
		MSG_WriteByte (&client->message, svc_signonnum);
		MSG_WriteByte (&client->message, 2);
	}

	client->sendsignon = true;
}

/*
================
SV_CreateBaseline

================
*/
static void SV_CreateBaseline (void)
{
	int			i;
	edict_t			*svent;
	int				entnum;

	for (entnum = 0; entnum < sv.num_edicts ; entnum++)
	{
	// get the current server version
		svent = EDICT_NUM(entnum);
		if (svent->free)
			continue;
		if (entnum > svs.maxclients && !svent->v.modelindex)
			continue;

	//
	// create entity baseline
	//
		VectorCopy (svent->v.origin, svent->baseline.origin);
		VectorCopy (svent->v.angles, svent->baseline.angles);
		svent->baseline.frame = svent->v.frame;
		svent->baseline.skin = svent->v.skin;
		svent->baseline.scale = (int)(svent->v.scale*100.0)&255;
		svent->baseline.drawflags = svent->v.drawflags;
		svent->baseline.abslight = (int)(svent->v.abslight*255.0)&255;
		{
			eval_t *val = GetEdictFieldValue(svent, "alpha");
			svent->baseline.alpha = val ? ENTALPHA_ENCODE(val->_float) : ENTALPHA_DEFAULT;
		}
		if (entnum > 0	&& entnum <= svs.maxclients)
		{
			svent->baseline.colormap = entnum;
			svent->baseline.modelindex = 0;//SV_ModelIndex("models/paladin.mdl");
		}
		else
		{
			svent->baseline.colormap = 0;
			svent->baseline.modelindex =
				SV_ModelIndex(PR_GetString(svent->v.model));
		}
		memset (svent->baseline.ClearCount, 99, sizeof(svent->baseline.ClearCount));

	//
	// add to the message
	//
		/* svc + entnum + modelindex + 6 single-byte fields +
		 * 3 * (WriteCoord 2 + WriteAngle 1) = 20; rounded up. */
		SV_ReserveSignonSpace (24);
		MSG_WriteByte (&sv.signon,svc_spawnbaseline);
		MSG_WriteShort (&sv.signon,entnum);

		MSG_WriteShort (&sv.signon, svent->baseline.modelindex);
		MSG_WriteByte (&sv.signon, svent->baseline.frame);
		MSG_WriteByte (&sv.signon, svent->baseline.colormap);
		MSG_WriteByte (&sv.signon, svent->baseline.skin);
		MSG_WriteByte (&sv.signon, svent->baseline.scale);
		MSG_WriteByte (&sv.signon, svent->baseline.drawflags);
		MSG_WriteByte (&sv.signon, svent->baseline.abslight);
		for (i = 0; i < 3; i++)
		{
			MSG_WriteCoord (&sv.signon, svent->baseline.origin[i]);
			MSG_WriteAngle (&sv.signon, svent->baseline.angles[i]);
		}
	}
}


/*
================
SV_SendReconnect

Tell all the clients that the server is changing levels
================
*/
static void SV_SendReconnect (void)
{
	byte	data[128];
	sizebuf_t	msg;

	SZ_Init (&msg, data, sizeof(data));

	MSG_WriteChar (&msg, svc_stufftext);
	MSG_WriteString (&msg, "reconnect\n");
	NET_SendToAll (&msg, 5.0);

#if !defined(SERVERONLY)
	if (!isDedicated)
#ifdef QUAKE2
		Cbuf_InsertText ("reconnect\n");
#else
		Cmd_ExecuteString ("reconnect\n", src_command);
#endif
#endif	/* ! SERVERONLY */
}


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
	if (sv_protocol == PROTOCOL_VERSION && PROTOCOL_VERSION < PROTOCOL_UH2_114)
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

