/*
 * sv_main_spawn_h2.c -- the C original of the server's map spawn and its
 * map_checks readout, moved verbatim out of engine/hexen2/sv_main.c:
 * SV_MapCheckThresh, SV_MapCheckTitle, SV_PrintMapChecklist and
 * SV_SpawnServer, with the MAP CHECKLIST note above them.  SV_PrintMapCheck,
 * which is C-variadic, stays C in engine/rust/sv_main_variadic.c, and its
 * mapcheck_t moved to engine/rust/sv_main_target.h; both sides of the
 * harness call that one.  The differential harness beside this file
 * includes it; no engine target compiles it.  The engine's implementation
 * is engine/rust/src/sv_main_spawn.rs (E2 / slice E of #469).
 *
 * (from sv_main.c -- server main program)
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

/* sv_main.c's own include, quakedef.h, comes from c_ref_spawn.c, and so do
 * sv_main_target.h (mapcheck_t, SV_PrintMapCheck) and the declarations of
 * sv_main.c's file-scope localmodels, sv_protocol_userset and
 * scr_centertime_off that these functions read. */

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
