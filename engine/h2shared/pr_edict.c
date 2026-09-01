/* sv_edict.c -- entity dictionary
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

#if defined(H2W) && !defined(SERVERONLY)
#error SERVERONLY not defined for HW server
#endif

#if !defined(H2W)
#define SV_MAXCLIENTS	svs.maxclients
#define SV_ACTIVE	sv.active
#define SV_CURSKILL	current_skill
#else
#define SV_MAXCLIENTS MAX_CLIENTS
#define SV_ACTIVE	(sv.state == ss_active)
#define SV_CURSKILL	sv.current_skill
#endif

dprograms_t		*progs;
dfunction_t		*pr_functions;

static	char		pr_null_string[] = "";
static	char		*pr_strings;
static	int		pr_stringssize;
static	const char	**pr_knownstrings;
static	int		pr_maxknownstrings;
static	int		pr_numknownstrings;
static	ddef_t		*pr_fielddefs;
static	ddef_t		*pr_globaldefs;

dstatement_t	*pr_statements;
float		*pr_globals;
sv_globals_t	sv_globals;
int		pr_edict_size;		/* in bytes */

qboolean	is_progs_v6;

qboolean	ignore_precache = false;

unsigned short	pr_crc;

static int	type_size[8] = {
	1,	/* ev_void */
	1,	/* ev_string */
	1,	/* ev_float */
	3,	/* ev_vector */
	1,	/* ev_entity */
	1,	/* ev_field */
	1,	/* ev_function */
	1	/* ev_pointer */
};

typedef struct sv_def_s
{
	etype_t		type;
	int		offset;
	void		*field;
} sv_def_t;

#if !defined(H2W) /* HEXEN2 PROGS: */

#define OFS_V103(m)	(int)offsetof(globalvars_v103_t,m)/4
#define OFS_V111(m)	(int)offsetof(globalvars_v111_t,m)/4
#define OFS_V112(m)	(int)offsetof(globalvars_v112_t,m)/4

COMPILE_TIME_ASSERT(v103_gofs, offsetof(globalvars_v103_t,ClassChangeWeapon) == 412);
static sv_def_t globals_v103[] = {
	{ev_entity,	OFS_V103(self),			&sv_globals.self},
	{ev_entity,	OFS_V103(other),		&sv_globals.other},
	{ev_entity,	OFS_V103(world),		&sv_globals.world},
	{ev_float,	OFS_V103(time),			&sv_globals.time},
	{ev_float,	OFS_V103(frametime),		&sv_globals.frametime},
	{ev_float,	OFS_V103(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V103(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V103(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V103(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V103(coop),			&sv_globals.coop},
	{ev_float,	OFS_V103(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V103(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V103(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V103(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V103(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V103(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V103(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V103(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V103(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V103(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V103(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V103(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V103(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V103(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V103(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V103(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V103(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V103(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V103(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V103(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V103(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V103(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V103(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V103(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V103(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V103(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V103(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V103(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V103(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V103(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V103(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V103(main),			&sv_globals.main},
	{ev_function,	OFS_V103(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V103(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V103(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V103(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V103(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V103(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V103(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V103(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V103(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_void,	0,				NULL }
};

COMPILE_TIME_ASSERT(v111_gofs, offsetof(globalvars_v111_t,ClassChangeWeapon) == 416);
static sv_def_t globals_v111[] = {
	{ev_entity,	OFS_V111(self),			&sv_globals.self},
	{ev_entity,	OFS_V111(other),		&sv_globals.other},
	{ev_entity,	OFS_V111(world),		&sv_globals.world},
	{ev_float,	OFS_V111(time),			&sv_globals.time},
	{ev_float,	OFS_V111(frametime),		&sv_globals.frametime},
	{ev_float,	OFS_V111(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V111(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V111(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V111(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V111(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V111(coop),			&sv_globals.coop},
	{ev_float,	OFS_V111(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V111(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V111(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V111(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V111(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V111(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V111(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V111(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V111(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V111(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V111(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V111(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V111(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V111(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V111(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V111(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V111(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V111(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V111(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V111(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V111(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V111(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V111(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V111(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V111(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V111(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V111(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V111(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V111(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V111(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V111(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V111(main),			&sv_globals.main},
	{ev_function,	OFS_V111(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V111(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V111(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V111(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V111(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V111(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V111(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V111(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V111(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_void,	0,				NULL }
};

COMPILE_TIME_ASSERT(v112_gofs, offsetof(globalvars_v112_t,ClassChangeWeapon) == 404);
static sv_def_t globals_v112[] = {
	{ev_entity,	OFS_V112(self),			&sv_globals.self},
	{ev_entity,	OFS_V112(other),		&sv_globals.other},
	{ev_entity,	OFS_V112(world),		&sv_globals.world},
	{ev_float,	OFS_V112(time),			&sv_globals.time},
	{ev_float,	OFS_V112(frametime),		&sv_globals.frametime},
	{ev_float,	OFS_V112(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V112(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V112(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V112(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V112(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V112(coop),			&sv_globals.coop},
	{ev_float,	OFS_V112(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V112(cl_playerclass),	&sv_globals.cl_playerclass},
	{ev_float,	OFS_V112(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V112(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V112(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V112(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V112(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V112(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V112(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V112(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V112(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V112(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V112(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V112(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V112(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V112(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V112(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V112(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V112(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V112(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V112(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V112(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V112(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V112(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V112(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V112(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V112(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V112(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V112(main),			&sv_globals.main},
	{ev_function,	OFS_V112(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V112(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V112(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V112(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V112(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V112(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V112(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V112(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V112(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_void,	0,				NULL }
};

#else /* HEXENWORLD PROGS: */

#define OFS_V009(m)	(int)offsetof(globalvars_v009_t,m)/4
#define OFS_V011(m)	(int)offsetof(globalvars_v011_t,m)/4
#define OFS_V012(m)	(int)offsetof(globalvars_v012_t,m)/4
#define OFS_V014(m)	(int)offsetof(globalvars_v014_t,m)/4
#define OFS_V015(m)	(int)offsetof(globalvars_v015_t,m)/4

#if 0
COMPILE_TIME_ASSERT(v009_gofs, offsetof(globalvars_v009_t,SetChangeParms) == 464);
static sv_def_t globals_v009[] = {
	{ev_entity,	OFS_V009(self),			&sv_globals.self},
	{ev_entity,	OFS_V009(other),		&sv_globals.other},
	{ev_entity,	OFS_V009(world),		&sv_globals.world},
	{ev_float,	OFS_V009(time),			&sv_globals.time},
	{ev_float,	OFS_V009(frametime),		&sv_globals.frametime},
	{ev_entity,	OFS_V009(newmis),		&sv_globals.newmis},
	{ev_float,	OFS_V009(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V009(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V009(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V009(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V009(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V009(damageScale),		&sv_globals.damageScale},
	{ev_float,	OFS_V009(manaScale),		&sv_globals.manaScale},
	{ev_float,	OFS_V009(tomeMode),		&sv_globals.tomeMode},
	{ev_float,	OFS_V009(tomeRespawn),		&sv_globals.tomeRespawn},
	{ev_float,	OFS_V009(w2Respawn),		&sv_globals.w2Respawn},
	{ev_float,	OFS_V009(altRespawn),		&sv_globals.altRespawn},
	{ev_float,	OFS_V009(fixedLevel),		&sv_globals.fixedLevel},
	{ev_float,	OFS_V009(autoItems),		&sv_globals.autoItems},
	{ev_float,	OFS_V009(dmMode),		&sv_globals.dmMode},
	{ev_float,	OFS_V009(coop),			&sv_globals.coop},
	{ev_float,	OFS_V009(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V009(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V009(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V009(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V009(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V009(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V009(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V009(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V009(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V009(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V009(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V009(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V009(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V009(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V009(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V009(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V009(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V009(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V009(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V009(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V009(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V009(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V009(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V009(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V009(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V009(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V009(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V009(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V009(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V009(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V009(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V009(main),			&sv_globals.main},
	{ev_function,	OFS_V009(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V009(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V009(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V009(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V009(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V009(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V009(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V009(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V009(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_function,	OFS_V009(SetNewParms),		&sv_globals.SetNewParms},
	{ev_function,	OFS_V009(SetChangeParms),	&sv_globals.SetChangeParms},
	{ev_void,	0,				NULL }
};
#endif

COMPILE_TIME_ASSERT(v011_gofs, offsetof(globalvars_v011_t,SetChangeParms) == 480);
static sv_def_t globals_v011[] = {
	{ev_entity,	OFS_V011(self),			&sv_globals.self},
	{ev_entity,	OFS_V011(other),		&sv_globals.other},
	{ev_entity,	OFS_V011(world),		&sv_globals.world},
	{ev_float,	OFS_V011(time),			&sv_globals.time},
	{ev_float,	OFS_V011(frametime),		&sv_globals.frametime},
	{ev_entity,	OFS_V011(newmis),		&sv_globals.newmis},
	{ev_float,	OFS_V011(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V011(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V011(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V011(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V011(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V011(damageScale),		&sv_globals.damageScale},
	{ev_float,	OFS_V011(meleeDamScale),	&sv_globals.meleeDamScale},
	{ev_float,	OFS_V011(shyRespawn),		&sv_globals.shyRespawn},
	{ev_float,	OFS_V011(manaScale),		&sv_globals.manaScale},
	{ev_float,	OFS_V011(tomeMode),		&sv_globals.tomeMode},
	{ev_float,	OFS_V011(tomeRespawn),		&sv_globals.tomeRespawn},
	{ev_float,	OFS_V011(w2Respawn),		&sv_globals.w2Respawn},
	{ev_float,	OFS_V011(altRespawn),		&sv_globals.altRespawn},
	{ev_float,	OFS_V011(fixedLevel),		&sv_globals.fixedLevel},
	{ev_float,	OFS_V011(autoItems),		&sv_globals.autoItems},
	{ev_float,	OFS_V011(dmMode),		&sv_globals.dmMode},
	{ev_float,	OFS_V011(easyFourth),		&sv_globals.easyFourth},
	{ev_float,	OFS_V011(patternRunner),	&sv_globals.patternRunner},
	{ev_float,	OFS_V011(coop),			&sv_globals.coop},
	{ev_float,	OFS_V011(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V011(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V011(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V011(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V011(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V011(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V011(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V011(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V011(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V011(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V011(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V011(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V011(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V011(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V011(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V011(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V011(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V011(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V011(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V011(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V011(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V011(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V011(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V011(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V011(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V011(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V011(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V011(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V011(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V011(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V011(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V011(main),			&sv_globals.main},
	{ev_function,	OFS_V011(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V011(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V011(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V011(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V011(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V011(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V011(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V011(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V011(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_function,	OFS_V011(SetNewParms),		&sv_globals.SetNewParms},
	{ev_function,	OFS_V011(SetChangeParms),	&sv_globals.SetChangeParms},
	{ev_void,	0,				NULL }
};

COMPILE_TIME_ASSERT(v012_gofs, offsetof(globalvars_v012_t,SetChangeParms) == 488);
static sv_def_t globals_v012[] = {
	{ev_entity,	OFS_V012(self),			&sv_globals.self},
	{ev_entity,	OFS_V012(other),		&sv_globals.other},
	{ev_entity,	OFS_V012(world),		&sv_globals.world},
	{ev_float,	OFS_V012(time),			&sv_globals.time},
	{ev_float,	OFS_V012(frametime),		&sv_globals.frametime},
	{ev_entity,	OFS_V012(newmis),		&sv_globals.newmis},
	{ev_float,	OFS_V012(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V012(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V012(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V012(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V012(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V012(damageScale),		&sv_globals.damageScale},
	{ev_float,	OFS_V012(meleeDamScale),	&sv_globals.meleeDamScale},
	{ev_float,	OFS_V012(shyRespawn),		&sv_globals.shyRespawn},
	{ev_float,	OFS_V012(spartanPrint),		&sv_globals.spartanPrint},
	{ev_float,	OFS_V012(manaScale),		&sv_globals.manaScale},
	{ev_float,	OFS_V012(tomeMode),		&sv_globals.tomeMode},
	{ev_float,	OFS_V012(tomeRespawn),		&sv_globals.tomeRespawn},
	{ev_float,	OFS_V012(w2Respawn),		&sv_globals.w2Respawn},
	{ev_float,	OFS_V012(altRespawn),		&sv_globals.altRespawn},
	{ev_float,	OFS_V012(fixedLevel),		&sv_globals.fixedLevel},
	{ev_float,	OFS_V012(autoItems),		&sv_globals.autoItems},
	{ev_float,	OFS_V012(dmMode),		&sv_globals.dmMode},
	{ev_float,	OFS_V012(easyFourth),		&sv_globals.easyFourth},
	{ev_float,	OFS_V012(patternRunner),	&sv_globals.patternRunner},
	{ev_float,	OFS_V012(coop),			&sv_globals.coop},
	{ev_float,	OFS_V012(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V012(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V012(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V012(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V012(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V012(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V012(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V012(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V012(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V012(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V012(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V012(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V012(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V012(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V012(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V012(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V012(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V012(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V012(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V012(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V012(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V012(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V012(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V012(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V012(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V012(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V012(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V012(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V012(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V012(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V012(max_players),		&sv_globals.max_players},
	{ev_float,	OFS_V012(exp_mult),		&sv_globals.exp_mult},

	{ev_function,	OFS_V012(main),			&sv_globals.main},
	{ev_function,	OFS_V012(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V012(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V012(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V012(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V012(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V012(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V012(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V012(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V012(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_function,	OFS_V012(SetNewParms),		&sv_globals.SetNewParms},
	{ev_function,	OFS_V012(SetChangeParms),	&sv_globals.SetChangeParms},
	{ev_void,	0,				NULL }
};

COMPILE_TIME_ASSERT(v014_gofs, offsetof(globalvars_v014_t,SetChangeParms) == 496);
static sv_def_t globals_v014[] = {
	{ev_entity,	OFS_V014(self),			&sv_globals.self},
	{ev_entity,	OFS_V014(other),		&sv_globals.other},
	{ev_entity,	OFS_V014(world),		&sv_globals.world},
	{ev_float,	OFS_V014(time),			&sv_globals.time},
	{ev_float,	OFS_V014(frametime),		&sv_globals.frametime},
	{ev_entity,	OFS_V014(newmis),		&sv_globals.newmis},
	{ev_float,	OFS_V014(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V014(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V014(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V014(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V014(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V014(damageScale),		&sv_globals.damageScale},
	{ev_float,	OFS_V014(meleeDamScale),	&sv_globals.meleeDamScale},
	{ev_float,	OFS_V014(shyRespawn),		&sv_globals.shyRespawn},
	{ev_float,	OFS_V014(spartanPrint),		&sv_globals.spartanPrint},
	{ev_float,	OFS_V014(manaScale),		&sv_globals.manaScale},
	{ev_float,	OFS_V014(tomeMode),		&sv_globals.tomeMode},
	{ev_float,	OFS_V014(tomeRespawn),		&sv_globals.tomeRespawn},
	{ev_float,	OFS_V014(w2Respawn),		&sv_globals.w2Respawn},
	{ev_float,	OFS_V014(altRespawn),		&sv_globals.altRespawn},
	{ev_float,	OFS_V014(fixedLevel),		&sv_globals.fixedLevel},
	{ev_float,	OFS_V014(autoItems),		&sv_globals.autoItems},
	{ev_float,	OFS_V014(dmMode),		&sv_globals.dmMode},
	{ev_float,	OFS_V014(easyFourth),		&sv_globals.easyFourth},
	{ev_float,	OFS_V014(patternRunner),	&sv_globals.patternRunner},
	{ev_float,	OFS_V014(coop),			&sv_globals.coop},
	{ev_float,	OFS_V014(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V014(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V014(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V014(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V014(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V014(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V014(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V014(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V014(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V014(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V014(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V014(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V014(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V014(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V014(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V014(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V014(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V014(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V014(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V014(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V014(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V014(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V014(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V014(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V014(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V014(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V014(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V014(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V014(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V014(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V014(exp_mult),		&sv_globals.exp_mult},
	{ev_float,	OFS_V014(max_players),		&sv_globals.max_players},
	{ev_float,	OFS_V014(defLosses),		&sv_globals.defLosses},
	{ev_float,	OFS_V014(attLosses),		&sv_globals.attLosses},

	{ev_function,	OFS_V014(main),			&sv_globals.main},
	{ev_function,	OFS_V014(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V014(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V014(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V014(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V014(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V014(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V014(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V014(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V014(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_function,	OFS_V014(SetNewParms),		&sv_globals.SetNewParms},
	{ev_function,	OFS_V014(SetChangeParms),	&sv_globals.SetChangeParms},
	{ev_void,	0,				NULL }
};

COMPILE_TIME_ASSERT(v015_gofs, offsetof(globalvars_v015_t,SmitePlayer) == 500);
static sv_def_t globals_v015[] = {
	{ev_entity,	OFS_V015(self),			&sv_globals.self},
	{ev_entity,	OFS_V015(other),		&sv_globals.other},
	{ev_entity,	OFS_V015(world),		&sv_globals.world},
	{ev_float,	OFS_V015(time),			&sv_globals.time},
	{ev_float,	OFS_V015(frametime),		&sv_globals.frametime},
	{ev_entity,	OFS_V015(newmis),		&sv_globals.newmis},
	{ev_float,	OFS_V015(force_retouch),	&sv_globals.force_retouch},
	{ev_string,	OFS_V015(mapname),		&sv_globals.mapname},
	{ev_string,	OFS_V015(startspot),		&sv_globals.startspot},
	{ev_float,	OFS_V015(deathmatch),		&sv_globals.deathmatch},
	{ev_float,	OFS_V015(randomclass),		&sv_globals.randomclass},
	{ev_float,	OFS_V015(damageScale),		&sv_globals.damageScale},
	{ev_float,	OFS_V015(meleeDamScale),	&sv_globals.meleeDamScale},
	{ev_float,	OFS_V015(shyRespawn),		&sv_globals.shyRespawn},
	{ev_float,	OFS_V015(spartanPrint),		&sv_globals.spartanPrint},
	{ev_float,	OFS_V015(manaScale),		&sv_globals.manaScale},
	{ev_float,	OFS_V015(tomeMode),		&sv_globals.tomeMode},
	{ev_float,	OFS_V015(tomeRespawn),		&sv_globals.tomeRespawn},
	{ev_float,	OFS_V015(w2Respawn),		&sv_globals.w2Respawn},
	{ev_float,	OFS_V015(altRespawn),		&sv_globals.altRespawn},
	{ev_float,	OFS_V015(fixedLevel),		&sv_globals.fixedLevel},
	{ev_float,	OFS_V015(autoItems),		&sv_globals.autoItems},
	{ev_float,	OFS_V015(dmMode),		&sv_globals.dmMode},
	{ev_float,	OFS_V015(easyFourth),		&sv_globals.easyFourth},
	{ev_float,	OFS_V015(patternRunner),	&sv_globals.patternRunner},
	{ev_float,	OFS_V015(coop),			&sv_globals.coop},
	{ev_float,	OFS_V015(teamplay),		&sv_globals.teamplay},
	{ev_float,	OFS_V015(serverflags),		&sv_globals.serverflags},
	{ev_float,	OFS_V015(total_secrets),	&sv_globals.total_secrets},
	{ev_float,	OFS_V015(total_monsters),	&sv_globals.total_monsters},
	{ev_float,	OFS_V015(found_secrets),	&sv_globals.found_secrets},
	{ev_float,	OFS_V015(killed_monsters),	&sv_globals.killed_monsters},
	{ev_float,	OFS_V015(chunk_cnt),		&sv_globals.chunk_cnt},
	{ev_float,	OFS_V015(done_precache),	&sv_globals.done_precache},
	{ev_float,	OFS_V015(parm1),		&sv_globals.parm},
	{ev_vector,	OFS_V015(v_forward),		&sv_globals.v_forward},
	{ev_vector,	OFS_V015(v_up),			&sv_globals.v_up},
	{ev_vector,	OFS_V015(v_right),		&sv_globals.v_right},
	{ev_float,	OFS_V015(trace_allsolid),	&sv_globals.trace_allsolid},
	{ev_float,	OFS_V015(trace_startsolid),	&sv_globals.trace_startsolid},
	{ev_float,	OFS_V015(trace_fraction),	&sv_globals.trace_fraction},
	{ev_vector,	OFS_V015(trace_endpos),		&sv_globals.trace_endpos},
	{ev_vector,	OFS_V015(trace_plane_normal),	&sv_globals.trace_plane_normal},
	{ev_float,	OFS_V015(trace_plane_dist),	&sv_globals.trace_plane_dist},
	{ev_entity,	OFS_V015(trace_ent),		&sv_globals.trace_ent},
	{ev_float,	OFS_V015(trace_inopen),		&sv_globals.trace_inopen},
	{ev_float,	OFS_V015(trace_inwater),	&sv_globals.trace_inwater},
	{ev_entity,	OFS_V015(msg_entity),		&sv_globals.msg_entity},
	{ev_float,	OFS_V015(cycle_wrapped),	&sv_globals.cycle_wrapped},
	{ev_float,	OFS_V015(crouch_cnt),		&sv_globals.crouch_cnt},
	{ev_float,	OFS_V015(modelindex_assassin),	&sv_globals.modelindex_assassin},
	{ev_float,	OFS_V015(modelindex_crusader),	&sv_globals.modelindex_crusader},
	{ev_float,	OFS_V015(modelindex_paladin),	&sv_globals.modelindex_paladin},
	{ev_float,	OFS_V015(modelindex_necromancer),&sv_globals.modelindex_necromancer},
	{ev_float,	OFS_V015(modelindex_sheep),	&sv_globals.modelindex_sheep},
	{ev_float,	OFS_V015(num_players),		&sv_globals.num_players},
	{ev_float,	OFS_V015(exp_mult),		&sv_globals.exp_mult},
	{ev_float,	OFS_V015(max_players),		&sv_globals.max_players},
	{ev_float,	OFS_V015(defLosses),		&sv_globals.defLosses},
	{ev_float,	OFS_V015(attLosses),		&sv_globals.attLosses},

	{ev_function,	OFS_V015(main),			&sv_globals.main},
	{ev_function,	OFS_V015(StartFrame),		&sv_globals.StartFrame},
	{ev_function,	OFS_V015(PlayerPreThink),	&sv_globals.PlayerPreThink},
	{ev_function,	OFS_V015(PlayerPostThink),	&sv_globals.PlayerPostThink},
	{ev_function,	OFS_V015(ClientKill),		&sv_globals.ClientKill},
	{ev_function,	OFS_V015(ClientConnect),	&sv_globals.ClientConnect},
	{ev_function,	OFS_V015(PutClientInServer),	&sv_globals.PutClientInServer},
	{ev_function,	OFS_V015(ClientReEnter),	&sv_globals.ClientReEnter},
	{ev_function,	OFS_V015(ClientDisconnect),	&sv_globals.ClientDisconnect},
	{ev_function,	OFS_V015(ClassChangeWeapon),	&sv_globals.ClassChangeWeapon},
	{ev_function,	OFS_V015(SetNewParms),		&sv_globals.SetNewParms},
	{ev_function,	OFS_V015(SetChangeParms),	&sv_globals.SetChangeParms},
	{ev_function,	OFS_V015(SmitePlayer),		&sv_globals.SmitePlayer},
	{ev_void,	0,				NULL }
};

#endif /* H2 / H2W PROGS */

static ddef_t	*ED_FieldAtOfs (int ofs);
static qboolean	ED_ParseEpair (void *base, ddef_t *key, const char *s);

static char field_name[256], class_name[256];
static qboolean RemoveBadReferences;

#define	MAX_FIELD_LEN	64
#define	GEFV_CACHESIZE	2

typedef struct {
	ddef_t	*pcache;
	char	field[MAX_FIELD_LEN];
} gefv_cache;

static gefv_cache	gefvCache[GEFV_CACHESIZE] =
{
		{ NULL,	"" },
		{ NULL,	"" }
};

cvar_t	max_temp_edicts = {"max_temp_edicts", "30", CVAR_ARCHIVE};

/* The gamecode filename, absent a maplist.txt override.  Up here rather than
 * beside PR_GetProgFilename() because the gamecode-source predicates below
 * need it too, and they must not call PR_GetProgFilename() itself -- see
 * PR_GamecodePakOnlyAvailable. */
#if !defined(H2W)
static const char def_progname[] = "progs.dat";
#else
static const char def_progname[] = "hwprogs.dat";
#endif

#if !defined(H2W)
/* WHERE the gamecode about to load should come from.  The three states name a
 * SOURCE, not an author, and that is the whole point of the naming: the first
 * cut called 0 "Classic" and 1 "Updated", which is a claim about whose code
 * runs, and it is false on any install that has a loose copy of OURS sitting in
 * data1/ -- the pre-bundle install method under uhexen2-8qp3 told players to
 * put it there, and one such install is what filed uhexen2-nt96.  "Classic"
 * there loads Hexenwail gamecode and the menu said so with a straight face.
 * A source is something the engine can state truthfully in every case.
 *
 *   GAMECODE_INSTALL (0) -- whatever the install's own gamedir resolves to.
 *                           Loose file if one is there, the pak copy if not;
 *                           the menu reads back which of the two it is.
 *   GAMECODE_BUNDLE  (1) -- the copy shipped beside the engine.  Default,
 *                           which is what the engine did before any of this.
 *   GAMECODE_PAKONLY (2) -- the pak copy specifically, stepping over a loose
 *                           file that would otherwise hide it.  Offered only
 *                           where something IS hiding it, because everywhere
 *                           else it resolves the identical file state 0 does.
 *
 * 0 and 1 keep the meanings they were written with, so a config from an older
 * build still says what it always said.  2 is new; it was a synonym for 1
 * before (any nonzero meant "bundle"), which nothing shipped ever wrote.
 *
 * PR_BundledProgsPath's -vanillaprogs comment argued against ever making this
 * a cvar, on two grounds.  The first still stands and is handled below by
 * latching: PR_LoadProgs runs on every map spawn, so a live cvar would let the
 * gamecode change between levels of one campaign.  The second -- that
 * CVAR_ARCHIVE writes an engine-packaging decision into the player's config --
 * no longer applies now that it is a deliberate player choice with a menu
 * entry rather than something the packager decided for them.  -vanillaprogs
 * still wins outright over all three. */
#define	GAMECODE_INSTALL	0
#define	GAMECODE_BUNDLE		1
#define	GAMECODE_PAKONLY	2

cvar_t	sv_gamecode = {"sv_gamecode", "1", CVAR_ARCHIVE};

/* The latched copy.  -1 means "never latched", in which case the cvar is read
 * directly -- that covers the first map of the session and any path that
 * reaches PR_LoadProgs without going through Host_Map_f. */
static int	pr_gamecode_latched = -1;

/* Defined down with the rest of the bundle lookup; needed here by
 * PR_GamecodeAvailable. */
static const char *PR_FindBundleDir (void);

/*
===============
PR_GamecodeWanted

The cvar's raw integer folded onto one of the three states.  Anything else
nonzero is GAMECODE_BUNDLE: that is what every nonzero value meant while this
was a boolean, and a config carrying one predates the third state.
===============
*/
static int PR_GamecodeWanted (int raw)
{
	if (raw == GAMECODE_PAKONLY)
		return GAMECODE_PAKONLY;
	return (raw != 0) ? GAMECODE_BUNDLE : GAMECODE_INSTALL;
}

/*
===============
PR_LatchGamecode

Called when a NEW GAME starts, and only then.  changelevel and restart must not
call it: a campaign has to finish on the gamecode it began with, for two
reasons.

1. Semantic mismatch.  Drop tables, item behaviour and progression differ
   between the images; changing them between levels of one campaign is
   incoherent on its own terms.
2. Silent savegame field loss.  Nothing refuses a cross-variant load -- see
   below -- so the failure is quiet rather than loud, which makes it worse,
   not better.  Entity state is stored as field-name/value text and restored
   through ED_ParseEdict() -> ED_FindField(); a field the loaded progs does
   not have is reported and skipped, and the load still succeeds with that
   value gone.  uhexen2-acew tracks that hazard on its own.

NOT because of a savegame CRC check -- there is no such check, and an earlier
version of this comment (and ebd68cc11's commit message) claimed there was.
Host_Loadgame_f() (hexen2/host_cmd.c) validates SAVEGAME_VERSION and nothing
else.  progs->crc is consulted only by the CLASS_DEMON availability gate and by
PR_LoadProgs()'s globals-layout switch; pr_crc is saved and restored only by
PR_SaveVMState/PR_RestoreVMState, which exist for CSQC VM switching.  The
24008 / 17499 figures cited in that commit message are the whole-file pr_crc
from the startup provenance line: they prove the two images differ, but no load
path compares them.  uhexen2-lrjk.
===============
*/
void PR_LatchGamecode (void)
{
	/* The player's raw intent, not the folded-for-this-install answer: the
	 * fold below depends on the searchpath, which Host_Game_f can change
	 * under a running session, so it is recomputed at every read rather
	 * than frozen here alongside the choice. */
	pr_gamecode_latched = PR_GamecodeWanted (sv_gamecode.integer);
}

/*
===============
PR_GamecodeState

The latched state, or the cvar if nothing has latched yet, folded onto what
this install can actually offer.

The fold is not cosmetic.  sv_gamecode is CVAR_ARCHIVE, so a config written on
an install where a loose progs.dat shadows a pak copy gets read on one where
nothing shadows anything -- and there GAMECODE_PAKONLY names the same file
GAMECODE_INSTALL does.  Collapsing the two keeps a setting that arrived from
somewhere else from being a state the menu will not show and the loader would
have to fail its way out of.
===============
*/
static int PR_GamecodeState (void)
{
	int	state = (pr_gamecode_latched < 0)
			? PR_GamecodeWanted (sv_gamecode.integer)
			: pr_gamecode_latched;

	if (state == GAMECODE_PAKONLY && !PR_GamecodePakOnlyAvailable())
		return GAMECODE_INSTALL;
	return state;
}

/*
===============
PR_GamecodeIsUpdated

Should the bundle substitute?  GAMECODE_BUNDLE and nothing else: the other two
states both load the install's own file, they only disagree about which of the
install's copies.  Deliberately not widened to "not GAMECODE_INSTALL" -- every
caller of this is asking about the bundle specifically.
===============
*/
qboolean PR_GamecodeIsUpdated (void)
{
	return (PR_GamecodeState() == GAMECODE_BUNDLE);
}

/*
===============
PR_GamecodeIsPakOnly

Should the loader step over a loose progs.dat and take the pak copy?
===============
*/
qboolean PR_GamecodeIsPakOnly (void)
{
	return (PR_GamecodeState() == GAMECODE_PAKONLY);
}

/*
===============
PR_GamecodeAvailable

Is there anything to choose BETWEEN?  Offering the switch when no bundled
gamecode shipped would be a control that does nothing, so the menu hides it.
Answers only the cheap, stable half of PR_BundledProgsPath's conditions -- the
build carries a bundle, the install is one we will substitute for, and the
player has not forced vanilla.  The per-gamedir conditions are deliberately not
repeated here: they depend on the searchpath at map spawn, and a menu item that
appeared and vanished as the player moved between data1 and portals would be
worse than one that is occasionally present but inert.
===============
*/
qboolean PR_GamecodeAvailable (void)
{
	if (COM_CheckParm("-vanillaprogs"))
		return false;
	if (!(gameflags & GAME_REGISTERED))
		return false;
	return (PR_FindBundleDir() != NULL);
}

/*
===============
PR_GamecodeLooseShadowsPak

Is the gamecode the install would hand us a LOOSE file that is hiding a pak
copy of the same name?  This is what lets GAMECODE_INSTALL read back as
"Loose" or "Pak" instead of naming an author it cannot vouch for, which is the
mistake uhexen2-nt96 was filed against.

Asks about def_progname rather than PR_GetProgFilename().  Two reasons, both
about the menu, which is the only caller that draws this every frame:

  - PR_GetProgFilename() opens and parses maplist.txt, and its answer depends
    on sv.name, so the row's reading would change as the player crossed into
    one of the ten data1 maps that pull progs2.dat.  A control that describes
    itself differently depending on which map you last loaded is the flicker
    PR_GamecodeAvailable's comment refuses.
  - It leaves file_from_pak and fs_lastfile_source describing maplist.txt,
    which is exactly the state this function reads back.

progs2.dat is therefore answered for by proxy.  Safe in both directions: the
loader's own paks-only attempt is generic over progname and falls back with a
message if the proxy was optimistic.
===============
*/
qboolean PR_GamecodeLooseShadowsPak (void)
{
	/* Pak members first: pure hash lookups, no syscalls, and a no means the
	 * question is settled without touching a directory. */
	if (!FS_FileExistsInPak (def_progname, NULL))
		return false;
	if (!FS_FileExists (def_progname, NULL))
		return false;			/* no gamecode at all */
	return (file_from_pak == 0);
}

/*
===============
PR_GamecodePakOnlyAvailable

Is GAMECODE_PAKONLY worth offering, i.e. would it reach a different file from
GAMECODE_INSTALL?  Where nothing shadows the pak copy the two are byte for
byte the same load, and a rotation step that changes nothing is the inert
control PR_GamecodeAvailable exists to avoid.

Two conditions on top of the shadowing test:

  - -vanillaprogs outranks every state, so with it set the row can only ever
    describe the one thing that happens.
  - The shadowed file must belong to the base game.  A mod's own progs.dat is
    a loose file shadowing data1's pak copy by this test's letter, and taking
    the pak copy there would run Raven's gamecode under a mod that never
    asked for it.  This is PR_BundledProgsPath's condition 1 and it is needed
    for the same reason.

Its condition 2 (fs_gamedir_nopath must name that same gamedir) is NOT
repeated, because the hole it plugs does not exist here.  There the bundle is
found by gamedir NAME, so a nopath that disagrees with path_id picks the wrong
bundle; here the paks-only search resolves the same progname through the same
searchpath and cannot address the wrong file.  Leaving it out also keeps this
answer from moving when the player enters a map pack that ships no gamecode --
data1's file still loads, and stepping over a loose one is still meaningful.
===============
*/
qboolean PR_GamecodePakOnlyAvailable (void)
{
	unsigned int	path_id, portals_id;

	if (COM_CheckParm("-vanillaprogs"))
		return false;
	if (!PR_GamecodeLooseShadowsPak())
		return false;

	/* PR_GamecodeLooseShadowsPak just resolved this; ask again only for the
	 * path_id it did not return. */
	if (!FS_FileExists (def_progname, &path_id))
		return false;
	portals_id = FS_GetPortalsPathID();
	if (path_id != 1U && !(portals_id && path_id == portals_id))
		return false;			/* a mod owns this file */

	return true;
}

/* WHOSE gamecode is loaded, as opposed to WHICH FILE it came from.  The two
 * are independent and both are needed: sv_gamecode and the bundle decide the
 * file, but a player who hand-copied our progs.dat into the install's data1/
 * -- the pre-bundle install method releases documented under uhexen2-8qp3 --
 * gets our code out of the install's own gamedir, and nothing said so.  It is
 * why the menu row above stopped claiming to name an author at all
 * (uhexen2-nt96) and why PR_LoadProgs warns about the disagreement outright
 * (uhexen2-bflw); this string is the third leg of the same answer.  NULL until
 * PR_LoadProgs() has run, which is also the honest answer on a client
 * connected to someone else's server: that progs was never loaded here.
 * uhexen2-8r3e. */
static const char	*pr_gamecode_ident;

/* The marker gamecode/hc/<tree>/ident.hc defines, as a PREFIX: the real symbol
 * is GAMECODE_SENTINEL "_YYYYMMDD".  A function, not a global, because hcc
 * strips global names under -on -- which is also why the date has to live in
 * the name rather than in a string it could point at.  See that file.
 *
 * Matched as a prefix so a stamp change is not an engine change, and so our
 * builds that carry the bare unstamped name (uhexen2-8r3e as first landed)
 * still identify, just without a date.  Keep the spelling in sync by hand. */
#define	GAMECODE_SENTINEL	"HexenwailGamecode"

/*
===============
PR_ClassifyGamecode

Names the origin of the image just loaded.  Three tiers, most certain first.

The retail CRCs are whole-file pr_crc values for Raven's own shipped files, and
they are the only numbers here that can be hardcoded safely: they were fixed in
1997 and cannot move.  Ours emphatically can -- every gamecode/hc edit changes
it, which is why docs/BUNDLED_GAMECODE.md stamps its own table "dated, and
expected to move" -- so our side is identified by a symbol instead.

Raven is asked first only because pr_crc is already computed and the test is
six compares; retail can never carry the sentinel, so the order is not
load-bearing.

The Raven answer carries a release number, taken from progs->crc -- the same
field the progvstr switch reads, and the file's own statement of its interface
version.  "1.12a" rather than progvstr's "H2MP/v1.12" because 1.12a is what
Raven actually shipped the mission pack as; the bare "1.12" is the progdefs
generation, not a release.

Our answer carries the date the gamecode last changed, read out of the marker's
own name, giving "hexenwail-2026-08-15".  The whole-file CRC on the same line is
the exact revision and the date is not a substitute for it -- but a CRC is not
something a player can hold in their head or recognise as older than another,
and this is the string testers are asked to read back.  A stamp that fails to
parse degrades to "hexenwail (undated)" rather than printing a malformed date.

The word "undated" is spelled out because the first cut said just "hexenwail",
and the menu draws this at 8px: next to "hexenwail-2026-08-15" the short form
reads as the same string at a glance.  A reporter on a build with no stamp
flipped the source row back and forth, saw the row below apparently not move,
and concluded the control was broken.  uhexen2-nt96.

Anything unrecognised reads as third-party rather than as Raven: the retail
list covers the three files a retail install has, and the mods measured in the
verification matrix (sot 28154, soc 58269, karma2 22850, GameOfTomes 41534) all
land here correctly.  An older Raven release we have no CRC for would be
misfiled, which is the known cost of not being able to enumerate them.
===============
*/
/* Defined further down this file and not in any header; host_cmd.c reaches it
 * through a local prototype the same way. */
dfunction_t *ED_FindFunctioni (const char *fn_name);

/*
===============
PR_GamecodeStamp

The tail of the marker function's name -- "_20260815", or "" for a build that
carries the bare unstamped name -- or NULL if the marker is absent entirely.
Walks the function table itself rather than going through ED_FindFunctioni(),
which is an exact match and cannot find a name whose suffix it does not know.
===============
*/
static const char *PR_GamecodeStamp (void)
{
	const size_t	preflen = strlen(GAMECODE_SENTINEL);
	int		i;

	for (i = 0; i < progs->numfunctions; i++)
	{
		const char *name = PR_GetString (pr_functions[i].s_name);

		if (!strncmp (name, GAMECODE_SENTINEL, preflen))
			return name + preflen;
	}
	return NULL;
}

static const char *PR_ClassifyGamecode (void)
{
	/* data1/PROGS.DAT, portals/progs.dat, data1/PROGS2.DAT -- read off a real
	 * retail install in docs/BUNDLED_GAMECODE.md's P8/B6 rows. */
	static const unsigned int	retail_crcs[] = { 17499U, 20799U, 33075U };
	size_t	i;

	for (i = 0; i < sizeof(retail_crcs) / sizeof(retail_crcs[0]); i++)
	{
		if ((unsigned int)pr_crc != retail_crcs[i])
			continue;
		switch (progs->crc) {
		case PROGS_V103_CRC:	return "Raven 1.03";
		case PROGS_V111_CRC:	return "Raven 1.11";
		case PROGS_V112_CRC:	return "Raven 1.12a";
		default:		return "Raven";
		}
	}

	{
		static char	ident[32];
		const char	*stamp = PR_GamecodeStamp ();

		if (stamp)
		{
			/* "_YYYYMMDD" and nothing else.  Anything shorter, longer or
			 * non-numeric is a name this engine does not understand, and
			 * printing part of it as a date would be a confident lie --
			 * so an unparseable tail degrades to the undated form. */
			if (stamp[0] == '_' && strlen(stamp) == 9 &&
			    strspn(stamp + 1, "0123456789") == 8)
			{
				q_snprintf (ident, sizeof(ident), "hexenwail-%.4s-%.2s-%.2s",
					    stamp + 1, stamp + 5, stamp + 7);
				return ident;
			}
			return "hexenwail (undated)";
		}
	}

	/* Builds shipped before uhexen2-8r3e carry no ident.hc, so fall back to
	 * the incidental marker docs/GAMECODE.md already relies on.
	 * BadBackpackDump is ours (uhexen2-hwky) and is absent from both retail
	 * files -- the B8 verification row is exactly this test, run offline with
	 * qcdis.py.  Drop this once no such build is in the field. */
	if (ED_FindFunctioni ("BadBackpackDump") != NULL)
		return "hexenwail (undated)";

	return "Third-party";
}

/*
===============
PR_GamecodeIdent

Origin of the gamecode currently loaded, or NULL if none has been.
===============
*/
const char *PR_GamecodeIdent (void)
{
	return pr_gamecode_ident;
}
#endif	/* !H2W */

#if !defined(H2W)
// these actually are not used in hexen2, but mods may use them.
cvar_t	nomonsters = {"nomonsters", "0", CVAR_NONE};
cvar_t	gamecfg = {"gamecfg", "0", CVAR_NONE};
cvar_t	savedgamecfg = {"savedgamecfg", "0", CVAR_ARCHIVE};
cvar_t	saved1 = {"saved1", "0", CVAR_ARCHIVE};
cvar_t	saved2 = {"saved2", "0", CVAR_ARCHIVE};
cvar_t	saved3 = {"saved3", "0", CVAR_ARCHIVE};
cvar_t	saved4 = {"saved4", "0", CVAR_ARCHIVE};
cvar_t	scratch1 = {"scratch1", "0", CVAR_NONE};
cvar_t	scratch2 = {"scratch2", "0", CVAR_NONE};
cvar_t	scratch3 = {"scratch3", "0", CVAR_NONE};
cvar_t	scratch4 = {"scratch4", "0", CVAR_NONE};
#else /* HexenWorld: */
func_t SpectatorConnect;
func_t SpectatorThink;
func_t SpectatorDisconnect;
#endif

//===========================================================================


/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
void ED_ClearEdict (edict_t *e)
{
	memset (&e->v, 0, progs->entityfields * 4);
	#ifndef H2W
	memset (&e->baseline, 0, sizeof(e->baseline));
	#endif
	e->free = false;
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *ED_Alloc (void)
{
	int			i;
	edict_t		*e;

	for (i = SV_MAXCLIENTS + 1 + max_temp_edicts.integer; i < sv.num_edicts; i++)
	{
		e = EDICT_NUM(i);
		// the first couple seconds of server time can involve a lot of
		// freeing and allocating, so relax the replacement policy
		if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5))
		{
			ED_ClearEdict (e);
			return e;
		}
	}

#if !defined(H2W)
	if (i == sv_max_edicts)
	{
		SV_Edicts("edicts.txt");
		Host_Error ("%s: no free edicts", __thisfunc__);
	}
#else
	if (i == sv_max_edicts)
	{
		Con_Printf ("WARNING: %s: no free edicts\n", __thisfunc__);
		i--;	// step on whatever is the last edict
		e = EDICT_NUM(i);
		SV_UnlinkEdict(e);
	}
	else
#endif
	sv.num_edicts++;
	e = EDICT_NUM(i);
	ED_ClearEdict (e);

	return e;
}

edict_t *ED_Alloc_Temp (void)
{
	int			i, j;
	edict_t		*e, *Least;

	Least = NULL;
	for (i = SV_MAXCLIENTS + 1, j = 0; j < max_temp_edicts.integer; i++, j++)
	{
		e = EDICT_NUM(i);
		// the first couple seconds of server time can involve a lot of
		// freeing and allocating, so relax the replacement policy
		if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5))
		{
			ED_ClearEdict (e);
			e->alloctime = sv.time;

			return e;
		}
		if (Least == NULL || e->alloctime < Least->alloctime)
		{
			Least = e;
		}
	}

	ED_Free(Least);
	ED_ClearEdict (Least);
	Least->alloctime = sv.time;

	return Least;
}

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void ED_Free (edict_t *ed)
{
	SV_UnlinkEdict (ed);		// unlink from world bsp

	// Clear per-entity PimpModel overrides
#ifndef SERVERONLY
	{
		int entnum = NUM_FOR_EDICT(ed);
		pimp_override_t *pimp = R_GetPimpOverride(entnum);
		if (pimp)
			pimp->active = false;
	}
#endif

	ed->free = true;
	ed->v.model = 0;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorClear (ed->v.origin);
	VectorClear (ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;

	ed->freetime = sv.time;
	ed->alloctime = -1;
}

//===========================================================================

/*
============
ED_GlobalAtOfs
============
*/
static ddef_t *ED_GlobalAtOfs (int ofs)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < progs->numglobaldefs; i++)
	{
		def = &pr_globaldefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/*
============
ED_FieldAtOfs
============
*/
static ddef_t *ED_FieldAtOfs (int ofs)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < progs->numfielddefs; i++)
	{
		def = &pr_fielddefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/* uhexen2-4ej9: public field-table reflection so renderer-side debug code
 * (r_showbboxes_links) can iterate entity-typed fields without re-opening
 * the static pr_fielddefs / progs->numfielddefs pair. */
int ED_NumFieldDefs (void)
{
	return progs ? progs->numfielddefs : 0;
}

ddef_t *ED_FieldDefAt (int i)
{
	if (!progs || i < 0 || i >= progs->numfielddefs)
		return NULL;
	return &pr_fielddefs[i];
}

/*
============
ED_FindField
============
*/
static ddef_t *ED_FindField (const char *name)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < progs->numfielddefs; i++)
	{
		def = &pr_fielddefs[i];
		if ( !strcmp(PR_GetString(def->s_name), name) )
			return def;
	}
	return NULL;
}


/*
============
ED_FindGlobal
============
*/
static ddef_t *ED_FindGlobal (const char *name)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < progs->numglobaldefs; i++)
	{
		def = &pr_globaldefs[i];
		if ( !strcmp(PR_GetString(def->s_name), name) )
			return def;
	}
	return NULL;
}


/*
============
ED_FindFunction
============
*/
static dfunction_t *ED_FindFunction (const char *fn_name)
{
	dfunction_t		*func;
	int				i;

	for (i = 0; i < progs->numfunctions; i++)
	{
		func = &pr_functions[i];
		if ( !strcmp(PR_GetString(func->s_name), fn_name) )
			return func;
	}
	return NULL;
}

dfunction_t *ED_FindFunctioni (const char *fn_name)
{
	dfunction_t		*func;
	int				i;
	
	for (i = 0; i < progs->numfunctions; i++)
	{
		func = &pr_functions[i];
		if ( !q_strcasecmp(PR_GetString(func->s_name), fn_name) )
			return func;
	}
	return NULL;
}


eval_t *GetEdictFieldValue(edict_t *ed, const char *field)
{
	ddef_t			*def = NULL;
	int				i;
	static int		rep = 0;

	for (i = 0; i < GEFV_CACHESIZE; i++)
	{
		if (!strcmp(field, gefvCache[i].field))
		{
			def = gefvCache[i].pcache;
			goto Done;
		}
	}

	def = ED_FindField (field);

	if (strlen(field) < MAX_FIELD_LEN)
	{
		gefvCache[rep].pcache = def;
		strcpy (gefvCache[rep].field, field);
		rep ^= 1;
	}

Done:
	if (!def)
		return NULL;

	return (eval_t *)((char *)&ed->v + def->ofs*4);
}


/*
============
PR_ValueString
(etype_t type, eval_t *val)

Returns a string describing *data in a type specific manner
=============
*/
static const char *PR_ValueString (int type, eval_t *val)
{
	static char	line[512];
	ddef_t		*def;
	dfunction_t	*f;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		q_snprintf (line, sizeof(line), "%s", PR_GetString(val->string));
		break;
	case ev_entity:
		q_snprintf (line, sizeof(line), "entity %i", NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)) );
		break;
	case ev_function:
		f = pr_functions + val->function;
		q_snprintf (line, sizeof(line), "%s()", PR_GetString(f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		q_snprintf (line, sizeof(line), ".%s", PR_GetString(def->s_name));
		break;
	case ev_void:
		q_snprintf (line, sizeof(line), "void");
		break;
	case ev_float:
		q_snprintf (line, sizeof(line), "%5.1f", val->_float);
		break;
	case ev_vector:
		q_snprintf (line, sizeof(line), "'%5.1f %5.1f %5.1f'", val->vector[0], val->vector[1], val->vector[2]);
		break;
	case ev_pointer:
		q_snprintf (line, sizeof(line), "pointer");
		break;
	default:
		q_snprintf (line, sizeof(line), "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_UglyValueString
(etype_t type, eval_t *val)

Returns a string describing *data in a type specific manner
Easier to parse than PR_ValueString
=============
*/
static const char *PR_UglyValueString (int type, eval_t *val)
{
	static char	line[512];
	ddef_t		*def;
	dfunction_t	*f;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		q_snprintf (line, sizeof(line), "%s", PR_GetString(val->string));
		break;
	case ev_entity:
		q_snprintf (line, sizeof(line), "%i", NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)));
		break;
	case ev_function:
		f = pr_functions + val->function;
		q_snprintf (line, sizeof(line), "%s", PR_GetString(f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		q_snprintf (line, sizeof(line), "%s", PR_GetString(def->s_name));
		break;
	case ev_void:
		q_snprintf (line, sizeof(line), "void");
		break;
	case ev_float:
		q_snprintf (line, sizeof(line), "%f", val->_float);
		break;
	case ev_vector:
		q_snprintf (line, sizeof(line), "%f %f %f", val->vector[0], val->vector[1], val->vector[2]);
		break;
	default:
		q_snprintf (line, sizeof(line), "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_GlobalString

Returns a string with a description and the contents of a global,
padded to 20 field width
============
*/
const char *PR_GlobalString (int ofs)
{
	static char	line[512];
	const char	*s;
	int		i;
	ddef_t		*def;
	void		*val;

	val = (void *)&pr_globals[ofs];
	def = ED_GlobalAtOfs(ofs);
	if (!def)
		q_snprintf (line, sizeof(line), "%i(?)", ofs);
	else
	{
		s = PR_ValueString (def->type, (eval_t *)val);
		q_snprintf (line, sizeof(line), "%i(%s)%s", ofs, PR_GetString(def->s_name), s);
	}

	i = strlen(line);
	for ( ; i < 20; i++)
		strcat (line, " ");
	strcat (line, " ");

	return line;
}

const char *PR_GlobalStringNoContents (int ofs)
{
	static char	line[512];
	int		i;
	ddef_t		*def;

	def = ED_GlobalAtOfs(ofs);
	if (!def)
		q_snprintf (line, sizeof(line), "%i(?)", ofs);
	else
		q_snprintf (line, sizeof(line), "%i(%s)", ofs, PR_GetString(def->s_name));

	i = strlen(line);
	for ( ; i < 20; i++)
		strcat (line, " ");
	strcat (line, " ");

	return line;
}


/*
=============
ED_Print

For debugging
=============
*/
void ED_Print (edict_t *ed)
{
	ddef_t	*d;
	int		*v;
	int		i, j, l;
	const char	*name;
	int		type;

	if (ed->free)
	{
		Con_Printf ("FREE\n");
		return;
	}

	Con_Printf("\nEDICT %i:\n", NUM_FOR_EDICT(ed));
	for (i = 1; i < progs->numfielddefs; i++)
	{
		d = &pr_fielddefs[i];
		name = PR_GetString(d->s_name);
		l = strlen (name);
		j = l - 1;
		if (j > 0 && name[j-1] == '_' && name[j] >= 'x' && name[j] <= 'z')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + d->ofs*4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;

		for (j = 0; j < type_size[type]; j++)
		{
			if (v[j])
				break;
		}
		if (j == type_size[type])
			continue;

		Con_Printf ("%s", name);
		while (l++ < 15)
			Con_Printf (" ");

		Con_Printf ("%s\n", PR_ValueString(d->type, (eval_t *)v));
	}
}

/*
=============
ED_FieldValueString

One formatted field value for the r_showfields overlay (uhexen2-a5nn.10).

Returns NULL for exactly the fields ED_Print hides -- the _x/_y/_z aliases
that shadow a vector, and anything still all-zero -- so a caller can skip a
NULL rather than restate that rule.  The result is PR_ValueString's static
buffer: copy it before calling again.
=============
*/
const char *ED_FieldValueString (edict_t *ed, ddef_t *d)
{
	const char	*name;
	int		*v;
	int		j, l, type;

	if (!ed || ed->free || !d)
		return NULL;

	name = PR_GetString (d->s_name);
	if (!name)
		return NULL;
	l = strlen (name);
	j = l - 1;
	if (j > 0 && name[j-1] == '_' && name[j] >= 'x' && name[j] <= 'z')
		return NULL;	/* skip _x, _y, _z vars */

	v = (int *)((char *)&ed->v + d->ofs*4);
	type = d->type & ~DEF_SAVEGLOBAL;
	if (type < 0 || type >= (int)(sizeof(type_size)/sizeof(type_size[0])))
		return NULL;
	for (j = 0; j < type_size[type]; j++)
	{
		if (v[j])
			break;
	}
	if (j == type_size[type])
		return NULL;	/* still all zero */

	return PR_ValueString (d->type, (eval_t *)v);
}

/*
=============
ED_GetProperty

Get the value of an edict property by name
=============
*/
const char* ED_GetProperty (edict_t *ed, char* propname)
{
	static char	ret[1];
	ddef_t	*d;
	int		*v;
	int		i;
	const char	*name;

	ret[0] = '\0';

	if (ed->free)
		return ret;

	for (i = 1; i < progs->numfielddefs; i++)
	{
		d = &pr_fielddefs[i];
		name = PR_GetString(d->s_name);

		if (!q_strncasecmp(name, propname, strlen(propname)))
		{
			v = (int*)((char*)&ed->v + d->ofs * 4);
			return PR_ValueString(d->type, (eval_t*)v);
		}
		else
		{
			continue;
		}
	}

	return ret;
}

/*
=============
ED_Write

For savegames
=============
*/
void ED_Write (FILE *f, edict_t *ed)
{
	ddef_t	*d;
	int		*v;
	int		i, j;
	const char	*name;
	int		type;

	fprintf (f, "{\n");

	if (ed->free)
	{
		fprintf (f, "}\n");
		return;
	}

	RemoveBadReferences = true;

	if (ed->v.classname)
		q_strlcpy (class_name, PR_GetString(ed->v.classname), sizeof(class_name));
	else
		class_name[0] = 0;

	for (i = 1; i < progs->numfielddefs; i++)
	{
		d = &pr_fielddefs[i];
		name = PR_GetString(d->s_name);
		j = strlen(name) - 1;
		if (j > 0 && name[j-1] == '_' && name[j] >= 'x' && name[j] <= 'z')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + d->ofs*4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;
		for (j = 0; j < type_size[type]; j++)
		{
			if (v[j])
				break;
		}
		if (j == type_size[type])
			continue;

		q_strlcpy(field_name, name, sizeof(field_name));
		fprintf (f, "\"%s\" ", name);
		fprintf (f, "\"%s\"\n", PR_UglyValueString(d->type, (eval_t *)v));
	}

	field_name[0] = 0;
	class_name[0] = 0;

	fprintf (f, "}\n");

	RemoveBadReferences = false;
}

void ED_PrintNum (int ent)
{
	ED_Print (EDICT_NUM(ent));
}

/*
=============
ED_PrintEdicts

For debugging, prints all the entities in the current server
=============
*/
void ED_PrintEdicts (void)
{
	int		i;

	if (!SV_ACTIVE)
		return;

	Con_Printf ("%i entities\n", sv.num_edicts);
	for (i = 0; i < sv.num_edicts; i++)
		ED_PrintNum (i);
}

/*
=============
ED_PrintEdict_f

For debugging, prints a single edicy
=============
*/
static void ED_PrintEdict_f (void)
{
	int		i;

	if (!SV_ACTIVE)
		return;

	i = atoi (Cmd_Argv(1));
	if (i < 0 || i >= sv.num_edicts)
	{
		Con_Printf("Bad edict number\n");
		return;
	}
	ED_PrintNum (i);
}

/*
=============
ED_Count

For debugging
=============
*/
static void ED_Count (void)
{
	edict_t	*ent;
	int	i, active, models, solid, step;

	if (!SV_ACTIVE)
		return;

	active = models = solid = step = 0;
	for (i = 0; i < sv.num_edicts; i++)
	{
		ent = EDICT_NUM(i);
		if (ent->free)
			continue;
		active++;
		if (ent->v.solid)
			solid++;
		if (ent->v.model)
			models++;
		if (ent->v.movetype == MOVETYPE_STEP)
			step++;
	}

	Con_Printf ("num_edicts:%3i\n", sv.num_edicts);
	Con_Printf ("active    :%3i\n", active);
	Con_Printf ("view      :%3i\n", models);
	Con_Printf ("touch     :%3i\n", solid);
	Con_Printf ("step      :%3i\n", step);
}


/*
==============================================================================

ARCHIVING GLOBALS

FIXME: need to tag constants, doesn't really work
==============================================================================
*/

/*
=============
ED_WriteGlobals
=============
*/
void ED_WriteGlobals (FILE *f)
{
	ddef_t		*def;
	int			i;
	const char		*name;
	int			type;

	fprintf (f, "{\n");
	for (i = 0; i < progs->numglobaldefs; i++)
	{
		def = &pr_globaldefs[i];
		type = def->type;
		if ( !(def->type & DEF_SAVEGLOBAL) )
			continue;
		type &= ~DEF_SAVEGLOBAL;

		if (type != ev_string && type != ev_float && type != ev_entity)
			continue;

		name = PR_GetString(def->s_name);
		fprintf (f, "\"%s\" ", name);
		fprintf (f, "\"%s\"\n", PR_UglyValueString(type, (eval_t *)&pr_globals[def->ofs]));
	}
	fprintf (f, "}\n");
}

/*
=============
ED_ParseGlobals
=============
*/
void ED_ParseGlobals (const char *data)
{
	char	keyname[64];
	ddef_t	*key;

	while (1)
	{
	// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Host_Error ("%s: EOF without closing brace", __thisfunc__);

		q_strlcpy (keyname, com_token, sizeof(keyname));

	// parse value
		data = COM_Parse (data);
		if (!data)
			Host_Error ("%s: EOF without closing brace", __thisfunc__);

		if (com_token[0] == '}')
			Host_Error ("%s: closing brace without data", __thisfunc__);

		key = ED_FindGlobal (keyname);
		if (!key)
		{
			Con_Printf ("'%s' is not a global\n", keyname);
			continue;
		}

		if (!ED_ParseEpair ((void *)pr_globals, key, com_token))
			Host_Error ("%s: parse error", __thisfunc__);
	}
}

//============================================================================


/*
=============
ED_NewString
=============
*/
static string_t ED_NewString (const char *string)
{
	char	*new_p;
	int		i, l;
	string_t	num;

	l = strlen(string) + 1;
	num = PR_AllocString (l, &new_p);

	for (i = 0; i < l; i++)
	{
		if (string[i] == '\\' && i < l-1)
		{
			i++;
			if (string[i] == 'n')
				*new_p++ = '\n';
			else
				*new_p++ = '\\';
		}
		else
			*new_p++ = string[i];
	}

	return num;
}


/*
=============
ED_ParseEval

Can parse either fields or globals
returns false if error
=============
*/
static qboolean	ED_ParseEpair (void *base, ddef_t *key, const char *s)
{
	int		i;
	char	string[128];
	ddef_t	*def;
	char	*v, *w;
	char	*end;
	void	*d;
	dfunction_t	*func;

	d = (void *)((int *)base + key->ofs);

	switch (key->type & ~DEF_SAVEGLOBAL)
	{
	case ev_string:
		*(string_t *)d = ED_NewString(s);
		break;

	case ev_float:
		*(float *)d = atof (s);
		break;

	case ev_vector:
		q_strlcpy (string, s, sizeof(string));
		end = (char *)string + strlen(string);
		v = string;
		w = string;

		for (i = 0; i < 3 && (w <= end); i++) // ericw -- added (w <= end) check
		{
		// set v to the next space (or 0 byte), and change that char to a 0 byte
			while (*v && *v != ' ')
				v++;
			*v = 0;
			((float *)d)[i] = atof (w);
			w = v = v+1;
		}
		// ericw -- fill remaining elements to 0 in case we hit the end of string
		// before reading 3 floats.
		if (i < 3)
		{
			Con_DPrintf ("Avoided reading garbage for \"%s\" \"%s\"\n",
				     PR_GetString(key->s_name), s);
			for (; i < 3; i++)
				((float *)d)[i] = 0.0f;
		}
		break;

	case ev_entity:
		*(int *)d = EDICT_TO_PROG(EDICT_NUM(atoi (s)));
		break;

	case ev_field:
		def = ED_FindField (s);
		if (!def)
		{
			Con_Printf ("Can't find field %s\n", s);
			return false;
		}
		*(int *)d = G_INT(def->ofs);
		break;

	case ev_function:
		func = ED_FindFunction (s);
		if (!func)
		{
			Con_Printf ("Can't find function %s\n", s);
			return false;
		}
		*(func_t *)d = func - pr_functions;
		break;

	default:
		break;
	}
	return true;
}

/*
====================
ED_IsEngineWorldspawnKey

True for worldspawn keys the engine reads straight out of the entity
lump (Sky_NewMap, Fog_ParseWorldspawn) and that the progs therefore has
no field for.  Callers must already have established that ED_FindField
failed, so a mod that does declare one of these keeps normal behaviour.

Engine-only keys should be spelled with a leading underscore (_sky,
_fog) per the FTE convention -- ED_ParseEdict drops those before it ever
gets here, and both worldspawn parsers strip the underscore.  This list
exists for the unprefixed spellings already baked into shipped maps.
====================
*/
static qboolean ED_IsEngineWorldspawnKey (const char *keyname)
{
	static const char *const engine_keys[] =
	{
		"sky",		/* QuakeSpasm / Quakespasm-spiked skybox */
		"skyname",	/* half-life */
		"qlsky",	/* quake lives */
		"skyfog",
		"fog",
		"wad",		/* editor bookkeeping, written by every compiler */
		"mapversion",
		NULL
	};
	int	i;

	for (i = 0; engine_keys[i]; i++)
	{
		if (!strcmp(engine_keys[i], keyname))
			return true;
	}

	return false;
}

/* Keys ED_ParseEdict had to throw away because the loaded progs has no such
 * field.  Only the savegame loader consumes this -- see ED_DroppedFields().
 * uhexen2-acew. */
static int	ed_dropped_fields;

void ED_ResetDroppedFields (void)
{
	ed_dropped_fields = 0;
}

/*
====================
ED_DroppedFields

How many keys the last run of ED_ParseEdict calls silently discarded.

A savegame stores entity state as field-name/value text, and nothing in
Hexen II fingerprints the gamecode a save was written under -- Host_Loadgame_f
checks SAVEGAME_VERSION and nothing else, and progs->crc is never consulted on
load.  So a save written under one progs.dat and loaded under another with a
different field set does not fail; it succeeds with that state quietly gone.
Counting the drops is what lets the loader say so out loud.  uhexen2-acew.
====================
*/
int ED_DroppedFields (void)
{
	return ed_dropped_fields;
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
const char *ED_ParseEdict (const char *data, edict_t *ent)
{
	ddef_t		*key;
	char		keyname[256];
	qboolean	anglehack, init;
	int		n;

	init = false;

	// clear it
	if (ent != sv.edicts)	// hack // we rely on this..
		memset (&ent->v, 0, progs->entityfields * 4);

	// go through all the dictionary pairs
	while (1)
	{
		// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Host_Error ("%s: EOF without closing brace", __thisfunc__);

		// anglehack is to allow QuakeEd to write single scalar angles
		// and allow them to be turned into vectors. (FIXME...)
		if (!strcmp(com_token, "angle"))
		{
			strcpy (com_token, "angles");
			anglehack = true;
		}
		else
			anglehack = false;

		// FIXME: change light to _light to get rid of this hack
		if (!strcmp(com_token, "light"))
			strcpy (com_token, "light_lev");	// hack for single light def

		q_strlcpy (keyname, com_token, sizeof(keyname));

		// another hack to fix keynames with trailing spaces
		n = strlen(keyname);
		while (n && keyname[n-1] == ' ')
		{
			keyname[n-1] = 0;
			n--;
		}

		// parse value
		data = COM_Parse (data);
		if (!data)
			Host_Error ("%s: EOF without closing brace", __thisfunc__);

		if (com_token[0] == '}')
			Host_Error ("%s: closing brace without data", __thisfunc__);

		init = true;

		// keynames with a leading underscore are used for utility comments,
		// and are immediately discarded by quake
		if (keyname[0] == '_')
			continue;

		if (q_strcasecmp(keyname,"MIDI") == 0)
		{
			q_strlcpy(sv.midi_name, com_token, sizeof(sv.midi_name));
			continue;
		}
		else if (q_strcasecmp(keyname,"CD") == 0)
		{
			sv.cd_track = (byte)atoi(com_token);
			continue;
		}

		key = ED_FindField (keyname);
		if (!key)
		{
			/* Worldspawn keys the engine consumes itself are not progs
			 * fields and never will be, so don't nag the mapper about
			 * them.  The prefixed spellings (_sky, _fog) are already
			 * dropped by the leading-underscore rule above and are the
			 * form to prefer; these are the legacy unprefixed ones that
			 * shipping maps use.  uhexen2-9afp. */
			if (!(ent == sv.edicts && ED_IsEngineWorldspawnKey (keyname)))
			{
				Con_Printf ("'%s' is not a field\n", keyname);
				/* Counted so the savegame loader can say something a
				 * player will actually read.  Coming from a .bsp this
				 * is a mapper's typo and one line per key is the right
				 * volume; coming from a savegame it means the save was
				 * written under gamecode with a different field set and
				 * that entity is being restored with state missing, one
				 * quiet line at a time.  uhexen2-acew. */
				ed_dropped_fields++;
			}
			continue;
		}

		if (anglehack)
		{
			char	temp[32];
			strcpy (temp, com_token);
			sprintf (com_token, "0 %s 0", temp);
		}

		if (!ED_ParseEpair ((void *)&ent->v, key, com_token))
			Host_Error ("%s: parse error", __thisfunc__);
	}

	if (!init)
		ent->free = true;

	return data;
}


extern int entity_file_size;

/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void ED_LoadFromFile (const char *data)
{
	dfunction_t	*func;
	edict_t		*ent = NULL;
	int		inhibit = 0;
	#ifndef SERVERONLY
	int		start_amount = current_loading_size;
	const char	*orig = data;
	#endif

	*sv_globals.time = sv.time;

	// parse ents
	while (1)
	{
		// parse the opening brace
		data = COM_Parse (data);
		if (!data)
			break;

		#ifndef SERVERONLY
		if (entity_file_size)
		{
			current_loading_size = start_amount + ((data - orig) * 80 / entity_file_size);
			D_ShowLoadingSize();
		}
		#endif

		if (com_token[0] != '{')
			Host_Error ("%s: found %s when expecting {", __thisfunc__, com_token);

		if (!ent)
			ent = EDICT_NUM(0);
		else
			ent = ED_Alloc ();
		data = ED_ParseEdict (data, ent);

#if 0
		//jfm fuckup test
		//remove for final release
		if (ent->v.spawnflags > 1 && !strcmp("worldspawn", PR_GetString(ent->v.classname)))
		{
			Host_Error ("invalid SpawnFlags on World!!!\n");
		}
#endif

		// remove things from different skill levels or deathmatch
		if (deathmatch.integer)
		{
			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}
		else if (coop.integer)
		{
			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_COOP))
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}
		else
		{ // Gotta be single player
			int		skip;

			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_SINGLE))
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}

			skip = 0;

			#ifndef SERVERONLY
			switch (cl_playerclass.integer)
			{
			case CLASS_PALADIN:
				if ((int)ent->v.spawnflags & SPAWNFLAG_NOT_PALADIN) {
					skip = 1;
				}
				break;

			case CLASS_CLERIC:
				if ((int)ent->v.spawnflags & SPAWNFLAG_NOT_CLERIC) {
					skip = 1;
				}
				break;

			case CLASS_DEMON:
			case CLASS_NECROMANCER:
				if ((int)ent->v.spawnflags & SPAWNFLAG_NOT_NECROMANCER) {
					skip = 1;
				}
				break;

			case CLASS_THEIF:
				if ((int)ent->v.spawnflags & SPAWNFLAG_NOT_THEIF) {
					skip = 1;
				}
				break;
			}
			#endif	/* !SERVERONLY */

			if (skip)
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}

		if ((SV_CURSKILL == 0 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY)) ||
		    (SV_CURSKILL == 1 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM)) ||
		    (SV_CURSKILL >= 2 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD)) ) {
			ED_Free (ent);
			inhibit++;
			continue;
		}

//
// immediately call spawn function
//
		if (!ent->v.classname)
		{
			Con_Printf ("No classname for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

	// look for the spawn function
		func = ED_FindFunction ( PR_GetString(ent->v.classname) );

		if (!func)
		{
			Con_Printf ("No spawn function for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

		*sv_globals.self = EDICT_TO_PROG(ent);
		PR_ExecuteProgram (func - pr_functions, PR_GetString(ent->v.classname));
		#ifdef H2W
		SV_FlushSignon();
		#endif
	}

	Con_DPrintf ("%i entities inhibited\n", inhibit);
}


/*
===============
PR_GetProgFilename

return the correct progs filename based on map name
by parsing maplist.txt
===============
*/
static const char *PR_GetProgFilename (void)
{
#if !USE_MULTIPLE_PROGS
	return def_progname;
#else
static const char maplist_name[] = "maplist.txt";
/* original format:
 * line #1 : <number of lines excluding this one>
 * line #2+: <map name><one space><prog filename>
 */
	static char	finalprogname[MAX_QPATH];
	unsigned int	id0, id1;
	fshandle_t	FH;

	FH.length = FS_OpenFileHandle (maplist_name, &FH, &id1);
	if (FH.length < 0)
		return def_progname;
	else if (FS_FileExists(def_progname, &id0) && id1 < id0)
	{
		Con_DPrintf("ignored %s from a gamedir with lower priority\n", maplist_name);
		goto _fail;
	}
	else
	{
		char	build[256], *test;
		int	entries;


		if (!FS_fgets(build, sizeof(build), &FH))
			goto _fail;
		entries = atoi(build);
		if (entries <= 0)
			goto _fail;

		while (--entries >= 0)
		{
			if (!(test = FS_fgets (build, sizeof(build), &FH)))
				goto _fail; /* unexpected EOF */
			while (*test)
			{
				if (*test == '\r' || *test == '\n')
				{
					*test = '\0';
					break;
				}
				if (*test == '\t')
					*test = ' ';
				++test;
			}
			while (--test > &build[0])
			{
				if (*test == ' ')
					*test = '\0';
				else
					break;
			}
			if (!(test = strchr(build, ' ')))
				continue;
			*test = 0;
			if (q_strcasecmp(build, sv.name) == 0)
			{
				FS_fclose (&FH);
				while (*(++test) == ' ')
					;
				q_strlcpy(finalprogname, test, sizeof(finalprogname));
				return finalprogname;
			}
		}
	}
_fail:
	FS_fclose (&FH);
	return def_progname;
#endif	/* end of USE_MULTIPLE_PROGS */
}

static void set_address (sv_def_t *def, void *address)
{
	switch (def->type) {
		case ev_void:
		case ev_bad:
			break;
		case ev_float:
		case ev_vector:
			*(float **)def->field = (float *) address;
			break;
		case ev_string:
		case ev_entity:
		case ev_field:
		case ev_function:
		case ev_pointer:
			*(int **)def->field = (int *) address;
			break;
	}
}

/*
===============
PR_ConvertV6Defs, PR_ConvertV6Stmts -- Pa3PyX

Convert ddef_v6_t and dstatement_v6_t arrays into _v7 format
with byte swapping.  See PR_ExecuteProgram() for more info.
===============
*/
static ddef_v7_t *PR_ConvertV6Defs (ddef_v6_t *v6defs, int numdefs)
{
	int		i;
	ddef_v7_t	*v7defs, *v7ptr;
	ddef_v6_t	*v6ptr;

	v7defs = (ddef_v7_t *) Hunk_AllocName(sizeof(ddef_v7_t) * numdefs, "prog7defs");
	for (i = 0, v6ptr = v6defs, v7ptr = v7defs; i < numdefs; i++, v6ptr++, v7ptr++)
	{
		v7ptr->type = LittleShort(v6ptr->type);
		v7ptr->ofs = (unsigned short)LittleShort(v6ptr->ofs);
		v7ptr->s_name = LittleLong(v6ptr->s_name);
	}

	return v7defs;
}

static dstatement_v7_t *PR_ConvertV6Stmts (dstatement_v6_t *v6stmts, int numstmts)
{
	int		i;
	dstatement_v7_t	*v7stmts, *v7ptr;
	dstatement_v6_t	*v6ptr;

	v7stmts = (dstatement_v7_t *) Hunk_AllocName(sizeof(dstatement_v7_t) * numstmts, "prog7stmt");
	for (i = 0, v6ptr = v6stmts, v7ptr = v7stmts; i < numstmts; i++, v6ptr++, v7ptr++)
	{
		v7ptr->op = LittleShort(v6ptr->op);
		v7ptr->a = (unsigned short)LittleShort(v6ptr->a);
		v7ptr->b = (unsigned short)LittleShort(v6ptr->b);
		v7ptr->c = (unsigned short)LittleShort(v6ptr->c);
	}

	return v7stmts;
}

#if !defined(H2W)
/* Bundled gamecode: a drop-in engine brings its own progs.dat, so a player who
 * unzips a release gets our fixes without hand-copying anything into data1/.
 *
 * The whole feature is a narrow substitution of one filename inside
 * PR_LoadProgs(), never a searchpath entry: a searchpath entry is a standing
 * offer to shadow *any* file that ever lands in the bundle directory, and one
 * given a fresh path_id would additionally make PR_GetProgFilename() discard
 * data1's maplist.txt, killing progs2.dat on ten maps with no report outside
 * `developer 1'.  Design, findings and the verification matrix are in
 * docs/BUNDLED_GAMECODE.md.
 *
 * H2W is excluded because def_progname there is "hwprogs.dat": this fork
 * builds no HexenWorld engine and ships no HexenWorld gamecode to compare
 * against, so the substitution has nothing correct to do.
 */

/*
===============
PR_FindBundleDir

Directory holding the gamecode shipped beside the engine, or NULL if none is
present.  Three layers, mirroring SF_FindSoundFont()'s: the portable/zip layout
first, then the FHS/Nix one, then a compile-time override for packagers.

Resolved through Sys_GetExeDir() and never through basedir: basedir is the
working directory the game was launched *from*, which for a desktop launcher or
a Steam shortcut is routinely not the install directory.  Sys_GetExeDir()
returns NULL where the platform cannot answer, which simply means the next
layer is tried.

Not cached: the answer is cheap, and recomputing it removes a whole class of
staleness bug for free.
===============
*/
static const char *PR_FindBundleDir (void)
{
	static char	dir[MAX_OSPATH];
	const char	*exedir;

	exedir = Sys_GetExeDir();
	if (exedir)
	{
		/* 1. <exedir>/gamecode/ -- portable/zip layout.  Already the
		 *    shipped path on Windows, whose release zip is flat. */
		q_snprintf (dir, sizeof(dir), "%s/gamecode", exedir);
		if (Sys_FileType(dir) == FS_ENT_DIRECTORY)
			return dir;

		/* 2. <exedir>/../share/hexenwail/ -- FHS/Nix layout, where the
		 *    binary sits in <platform>/bin/. */
		q_snprintf (dir, sizeof(dir), "%s/../share/hexenwail", exedir);
		if (Sys_FileType(dir) == FS_ENT_DIRECTORY)
			return dir;
	}

#ifdef BUNDLED_GAMECODE_DIR
	/* 3. compile-time path, for distribution packagers who put the data
	 *    somewhere neither of the above finds. */
	q_strlcpy (dir, BUNDLED_GAMECODE_DIR, sizeof(dir));
	if (Sys_FileType(dir) == FS_ENT_DIRECTORY)
		return dir;
#endif

	return NULL;
}

/*
===============
PR_BundledProgsPath

Decides whether the progs about to be loaded should come from the bundle
instead of the player's game directory, and if so writes its absolute path to
`out'.

Recomputed on every call.  PR_LoadProgs() runs per map spawn and the gamedir
can change at runtime through Host_Game_f (the Mods menu), so anything derived
from com_argv stops describing reality the moment the player switches -- in
both directions: launched into a mod and switched back to Hexen II, a
command-line gate would refuse to substitute forever; launched bare and
switched into a mod, it would substitute inside the mod.

Two independent conditions establish that the bundle MAY substitute, ANDed
because each alone has a reachable hole:

  - path_id answers "is this the base game's file?".  Alone it is not enough:
    a pure map pack that ships no gamecode of its own resolves progs.dat from
    portals (which -game pulls onto the path by itself) or from data1, i.e. at
    an id we ship a bundle for -- and substituting there would change the
    gamecode underneath a mod.

  - fs_gamedir_nopath answers "is the base game the game the player is in?".
    Alone it is not enough either: launch -portals against an install whose
    portals/ has no progs.dat and nopath reads "portals" while the file that
    actually resolves is data1's, so the portals bundle would be swapped in for
    a data1 file.

Both holes are reachable from the shipped Mods menu.  Do not reduce the AND.

A third condition then decides that it MUST NOT, giving the ordering

	user directory  >  bundle  >  install directory

The two losers are not symmetric, which is what makes this decidable at all.
The install's data1/PROGS.DAT is Raven's file and superseding it IS the
feature -- every retail tree has one, so ranking it above the bundle would
make the bundle dead code.  But nothing creates ~/.hexen2/data1/progs.dat:
not retail, not any installer.  A file there is a deliberate act, and it is
the only statement of intent the engine gets, so it wins.  Windows has no
user directory, so there the ordering is just bundle > install directory.
===============
*/
static qboolean PR_BundledProgsPath (const char *progname, char *out, size_t outsz)
{
	unsigned int	path_id, portals_id;
	const char	*bundle;
	const char	*gamedir;

	/* Hard off switch, still command line only: it exists so a packager or a
	 * bug report can take the engine's gamecode out of the picture entirely,
	 * and it outranks the player's menu choice below. */
	if (COM_CheckParm("-vanillaprogs"))
		return false;

	/* The player's choice, latched at new-game time so a campaign cannot
	 * change gamecode between its own levels.  See sv_gamecode.  False for
	 * BOTH of the other two states: neither GAMECODE_INSTALL nor
	 * GAMECODE_PAKONLY wants the bundle, they differ only in which of the
	 * install's own copies they take, and that choice is made in
	 * PR_LoadProgs where the load actually happens. */
	if (!PR_GamecodeIsUpdated())
		return false;

	/* Our gamecode is built from the 1.11 tree.  The demo, the OEM release
	 * and mix'n'match installs are different versions of the game, so decline
	 * them wholesale.  Fingerprinted once in FS_Init; cannot change. */
	if (!(gameflags & GAME_REGISTERED))
		return false;

	if (!(bundle = PR_FindBundleDir()))
		return false;			/* nothing shipped, or not extracted */

	/* Condition 1 -- is this the BASE GAME's file?  Ask the filesystem where
	 * progname actually resolves rather than assuming; that is what makes
	 * this robust against searchpath arrangements nobody has thought of. */
	if (!FS_FileExists(progname, &path_id))
		return false;			/* no gamecode at all: unchanged error */

	portals_id = FS_GetPortalsPathID();
	if (path_id == 1U)			/* data1 is assigned 1U first */
		gamedir = "data1";
	else if (portals_id && path_id == portals_id)
		gamedir = "portals";
	else
		return false;			/* a mod owns this file */

	/* Condition 2 -- is that gamedir the one the PLAYER IS IN? */
	if (q_strcasecmp(fs_gamedir_nopath, gamedir) != 0)
		return false;

	/* Condition 3, a stand-down -- has the player installed their own copy?
	 * Asked of the user directory only.  path_id cannot answer this: a
	 * gamedir's basedir entry and its userdir entry get the SAME id from
	 * FS_AddGameDirectory, so both read 1U for data1. */
	if (FS_UserdirHasFile (gamedir, progname))
		return false;

	q_snprintf (out, outsz, "%s/%s/%s", bundle, gamedir, progname);

	/* Per-file, not per-install: a bundle carrying data1/progs.dat but not
	 * portals/progs.dat must degrade for portals alone. */
	return (Sys_FileType(out) == FS_ENT_FILE);
}
#endif	/* !H2W */

/*
===============
PR_LumpFits

Does `count' elements of `elemsize' starting at `ofs' lie inside `filelen'?
Written as a division so that neither the multiply nor the addition can
overflow on a header we have not vetted yet.
===============
*/
static qboolean PR_LumpFits (int ofs, int count, size_t elemsize, long filelen)
{
	if (ofs < 0 || count < 0 || (long)ofs > filelen)
		return false;
	return (long)count <= (filelen - (long)ofs) / (long)elemsize;
}

/*
===============
PR_CheckProgsExtents

Returns NULL if every lump the header describes lies inside `filelen', or a
reason string naming the first one that does not.

The byteswap loops at the bottom of PR_LoadProgs() write through each of these
lumps (a v6 image is read through instead and converted into fresh allocations),
so a header whose offsets or counts overrun the file turns those loops into
out-of-bounds writes -- for a plausibly truncated progs.dat, hundreds of KB of
them, into whatever the hunk happens to hold next.  Only the string pool was
checked before, and it is the lump least able to detect truncation because it
sits at the very front of the file.

The header is byteswapped into a local copy so this can run before PR_LoadProgs()
has committed to the image, which is what lets the bundled-progs path reject a
file and fall back rather than die.  CL_LoadCSProgs() relies on the same property
for csprogs.dat.
===============
*/
const char *PR_CheckProgsExtents (const dprograms_t *raw, long filelen)
{
	static char	reason[128];
	dprograms_t	hdr;
	size_t		defsize, stmtsize;
	int		i;

	if (filelen < (long) sizeof(hdr))
	{
		q_snprintf (reason, sizeof(reason),
			    "file is %ld bytes, too short for a %d byte header",
			    filelen, (int) sizeof(hdr));
		return reason;
	}

	memcpy (&hdr, raw, sizeof(hdr));
	for (i = 0; i < (int) sizeof(hdr) / 4; i++)
		((int *)&hdr)[i] = LittleLong ( ((int *)&hdr)[i] );

	switch (hdr.version) {
	case PROG_VERSION_V6:
		defsize = sizeof(ddef_v6_t);
		stmtsize = sizeof(dstatement_v6_t);
		break;
	case PROG_VERSION_V7:
		defsize = sizeof(ddef_v7_t);
		stmtsize = sizeof(dstatement_v7_t);
		break;
	default:
		q_snprintf (reason, sizeof(reason),
			    "unsupported version (%d, should be %d or %d)",
			    hdr.version, PROG_VERSION_V6, PROG_VERSION_V7);
		return reason;
	}

	if (!PR_LumpFits (hdr.ofs_statements, hdr.numstatements, stmtsize, filelen))
		return "statements go past end of file";
	if (!PR_LumpFits (hdr.ofs_globaldefs, hdr.numglobaldefs, defsize, filelen))
		return "globaldefs go past end of file";
	if (!PR_LumpFits (hdr.ofs_fielddefs, hdr.numfielddefs, defsize, filelen))
		return "fielddefs go past end of file";
	if (!PR_LumpFits (hdr.ofs_functions, hdr.numfunctions, sizeof(dfunction_t), filelen))
		return "functions go past end of file";
	if (!PR_LumpFits (hdr.ofs_globals, hdr.numglobals, sizeof(int), filelen))
		return "globals go past end of file";
	/* Strings keep the strict end-of-pool test PR_LoadProgs() has always
	 * used: the pool has to stop short of EOF, not merely reach it.  The
	 * PR_LumpFits() call first is what makes the addition safe. */
	if (!PR_LumpFits (hdr.ofs_strings, hdr.numstrings, 1, filelen) ||
	    (long)hdr.ofs_strings + hdr.numstrings >= filelen)
		return "strings go past end of file";
	/* Every string in a progs image is reached as `pool + offset' and then
	 * handed straight to strcmp(), so keeping the offset in range is only
	 * half of what makes that read safe -- the walk still has to meet a
	 * terminator before the pool ends.  Requiring the pool's final byte to
	 * be NUL supplies that for every offset at once: a scan starting
	 * anywhere inside the pool stops at or before its last byte, whatever
	 * the offset was.  qcc emits each string NUL-terminated, so the last
	 * one ends the pool and every real image already satisfies this; an
	 * image with no NUL in its pool does not, and is the half of
	 * uhexen2-uglw that a range check alone cannot cover.  Callers still
	 * range-check the offset itself. */
	if (hdr.numstrings > 0 &&
	    ((const byte *)raw)[(long)hdr.ofs_strings + hdr.numstrings - 1] != 0)
		return "string pool is not null-terminated";

	return NULL;
}

/* What PR_ExecuteProgram() does with each operand of a statement.  These are
 * roles, not QC types: a/b/c are raw int32s out of the image, and OPA/OPB/OPC
 * turn them into &pr_globals[x] with no check of any kind, so this table plus
 * the walk in PR_ValidateBytecode() is the only thing keeping the interpreter's
 * reads and writes inside the globals lump.  uhexen2-1p3o. */
enum {
	PRO_UNUSED = 0,	/* never dereferenced for this opcode */
	PRO_GLOBAL,	/* offset of one global */
	PRO_VECTOR,	/* offset of three consecutive globals */
	PRO_JUMP,	/* statement offset, relative to the statement itself */
	PRO_ARRAY	/* array base; its element count sits one global below */
};

/* Indexed by opcode, so the row order has to stay exactly the enum order in
 * common/pr_comp.h.  Read off PR_ExecuteProgram()'s case bodies one by one:
 * PRO_VECTOR is written wherever the body touches ->vector[], which is wider
 * than the QC type suggests in a few places -- OP_DONE/OP_RETURN copy three
 * globals out of `a' whatever the function returns, and OP_CALL1..8 copy three
 * out of `b' (and `c' from OP_CALL2 up) into the parameter block whether the
 * argument is a vector or a float. */
static const byte pr_operandrole[][3] =
{
	{ PRO_VECTOR, PRO_UNUSED, PRO_UNUSED },	/* OP_DONE */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_MUL_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_GLOBAL },	/* OP_MUL_V */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_MUL_FV */
	{ PRO_VECTOR, PRO_GLOBAL, PRO_VECTOR },	/* OP_MUL_VF */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_DIV_F */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_ADD_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_VECTOR },	/* OP_ADD_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_SUB_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_VECTOR },	/* OP_SUB_V */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_EQ_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_GLOBAL },	/* OP_EQ_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_EQ_S */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_EQ_E */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_EQ_FNC */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_NE_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_GLOBAL },	/* OP_NE_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_NE_S */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_NE_E */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_NE_FNC */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LE */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_GE */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LT */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_GT */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LOAD_F */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_VECTOR },	/* OP_LOAD_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LOAD_S */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LOAD_ENT */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LOAD_FLD */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_LOAD_FNC */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_ADDRESS */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STORE_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_UNUSED },	/* OP_STORE_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STORE_S */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STORE_ENT */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STORE_FLD */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STORE_FNC */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_F */
	{ PRO_VECTOR, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_S */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_ENT */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_FLD */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STOREP_FNC */

	{ PRO_VECTOR, PRO_UNUSED, PRO_UNUSED },	/* OP_RETURN */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_GLOBAL },	/* OP_NOT_F */
	{ PRO_VECTOR, PRO_UNUSED, PRO_GLOBAL },	/* OP_NOT_V */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_GLOBAL },	/* OP_NOT_S */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_GLOBAL },	/* OP_NOT_ENT */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_GLOBAL },	/* OP_NOT_FNC */
	{ PRO_GLOBAL, PRO_JUMP,   PRO_UNUSED },	/* OP_IF */
	{ PRO_GLOBAL, PRO_JUMP,   PRO_UNUSED },	/* OP_IFNOT */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_UNUSED },	/* OP_CALL0 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_UNUSED },	/* OP_CALL1 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL2 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL3 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL4 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL5 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL6 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL7 */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_VECTOR },	/* OP_CALL8 */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_STATE */
	{ PRO_JUMP,   PRO_UNUSED, PRO_UNUSED },	/* OP_GOTO */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_AND */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_OR */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_BITAND */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_BITOR */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_MULSTORE_F */
	{ PRO_GLOBAL, PRO_VECTOR, PRO_UNUSED },	/* OP_MULSTORE_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_MULSTOREP_F */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_VECTOR },	/* OP_MULSTOREP_V */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_DIVSTORE_F */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_DIVSTOREP_F */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_ADDSTORE_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_UNUSED },	/* OP_ADDSTORE_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_ADDSTOREP_F */
	{ PRO_VECTOR, PRO_GLOBAL, PRO_VECTOR },	/* OP_ADDSTOREP_V */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_SUBSTORE_F */
	{ PRO_VECTOR, PRO_VECTOR, PRO_UNUSED },	/* OP_SUBSTORE_V */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_GLOBAL },	/* OP_SUBSTOREP_F */
	{ PRO_VECTOR, PRO_GLOBAL, PRO_VECTOR },	/* OP_SUBSTOREP_V */

	{ PRO_ARRAY,  PRO_GLOBAL, PRO_GLOBAL },	/* OP_FETCH_GBL_F */
	{ PRO_ARRAY,  PRO_GLOBAL, PRO_VECTOR },	/* OP_FETCH_GBL_V */
	{ PRO_ARRAY,  PRO_GLOBAL, PRO_GLOBAL },	/* OP_FETCH_GBL_S */
	{ PRO_ARRAY,  PRO_GLOBAL, PRO_GLOBAL },	/* OP_FETCH_GBL_E */
	{ PRO_ARRAY,  PRO_GLOBAL, PRO_GLOBAL },	/* OP_FETCH_GBL_FNC */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_CSTATE */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_CWSTATE */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_THINKTIME */

	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_BITSET */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_BITSETP */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_BITCLR */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_BITCLRP */

	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_RAND0 */
	{ PRO_GLOBAL, PRO_UNUSED, PRO_UNUSED },	/* OP_RAND1 */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_UNUSED },	/* OP_RAND2 */
	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_RANDV0 */
	{ PRO_VECTOR, PRO_UNUSED, PRO_UNUSED },	/* OP_RANDV1 */
	{ PRO_VECTOR, PRO_VECTOR, PRO_UNUSED },	/* OP_RANDV2 */

	{ PRO_GLOBAL, PRO_JUMP,   PRO_UNUSED },	/* OP_SWITCH_F */
	/* the other four switch types are PR_RunError()s in the interpreter,
	 * so they never reach an operand */
	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_SWITCH_V */
	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_SWITCH_S */
	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_SWITCH_E */
	{ PRO_UNUSED, PRO_UNUSED, PRO_UNUSED },	/* OP_SWITCH_FNC */

	{ PRO_GLOBAL, PRO_JUMP,   PRO_UNUSED },	/* OP_CASE */
	{ PRO_GLOBAL, PRO_GLOBAL, PRO_JUMP    }	/* OP_CASERANGE */
};

COMPILE_TIME_ASSERT(pr_operandrole,
		    sizeof(pr_operandrole) / sizeof(pr_operandrole[0]) == OP_CASERANGE + 1);

/*
===============
PR_ValidateBytecode

Returns NULL if the statement and function tables reach only memory the image
actually has, or a reason string naming the first place they do not.

PR_CheckProgsExtents() vets the container -- where each lump sits and how far it
runs.  It deliberately says nothing about what the bytecode inside those lumps
then asks the interpreter to do, and PR_ExecuteProgram() asks no questions of its
own: OPA/OPB/OPC are `&pr_globals[st->a]' with the operand a raw int32 out of the
file, and OPC is a write target (`c->_float = a->_float + b->_float').  A forged
operand is therefore an arbitrary read *and* write several GB either side of the
globals lump, which is a different class of bug from the header-field overreads
that hardening pass fixed -- and it is reachable from a player-supplied file,
because csprogs.dat arrives with downloaded mod or server content and runs every
frame once it exports CSQC_DrawHud.  uhexen2-1p3o.

The instruction pointer needs the same treatment for the same reason: the
dispatch loop is a bare `while (1) { st++; ... }' with nothing comparing st
against numstatements, so a function body that never meets a DONE/RETURN walks
straight off the end of the lump and executes whatever the hunk holds next.
uhexen2-im9e.  Three checks close that off between them, without costing the
interpreter's hot loop a single instruction:

  - every function's first_statement lands inside the lump, so execution starts
    in range;
  - every jump lands inside the lump, so it stays in range across a branch;
  - the last statement in the lump is DONE, RETURN or GOTO, so nothing can fall
    off the end -- falling through is the only remaining way past numstatements,
    and it can only happen from that one statement.

Call it once at load, after the byteswap loops and (for a v6 image) after the
statements have been converted, with the same is_v6 flag PR_ExecuteProgram()
would run under: v6 jump offsets are sign-extended from short at dispatch, and
the check has to extend them the same way to compute the same target.
===============
*/
const char *PR_ValidateBytecode (const dstatement_t *statements, int numstatements,
				 const dfunction_t *functions, int numfunctions,
				 int numglobals, qboolean v6)
{
	static char	reason[128];
	int		i, k;

	if (numstatements <= 0 || numfunctions <= 0)
		return "image has no code";

	for (i = 0; i < numfunctions; i++)
	{
		const dfunction_t	*f = &functions[i];
		int			j, parmwords = 0;

		/* A negative first_statement is a builtin number, and the call
		 * site bounds -first_statement against pr_numbuiltins -- but it
		 * negates first, and INT_MIN has no positive counterpart to
		 * compare. */
		if (f->first_statement >= numstatements || f->first_statement == INT_MIN)
		{
			q_snprintf (reason, sizeof(reason),
				    "function %d starts at statement %d of %d",
				    i, f->first_statement, numstatements);
			return reason;
		}
		/* Only the upper end: HCC writes numparms -1 for a variadic builtin
		 * and retail data1/portals gamecode both do -- `starteffect' is
		 * builtin 88 with numparms -1 in every shipped Hexen II progs.dat.
		 * A negative count simply skips EnterFunction()'s copy loop, so it
		 * is the too-large one that walks the parameter block. */
		if (f->numparms > MAX_PARMS)
		{
			q_snprintf (reason, sizeof(reason), "function %d takes %d parameters",
				    i, f->numparms);
			return reason;
		}
		/* parm_size is a byte and the copy loop in EnterFunction() reads
		 * pr_globals[OFS_PARM0 + i*3 + j] for j < parm_size[i], so a size
		 * past a vector's three words walks out of the parameter block
		 * and into whatever global follows it. */
		for (j = 0; j < f->numparms; j++)
		{
			if (f->parm_size[j] > 3)
			{
				q_snprintf (reason, sizeof(reason),
					    "function %d parameter %d is %d words wide",
					    i, j, f->parm_size[j]);
				return reason;
			}
			parmwords += f->parm_size[j];
		}
		/* EnterFunction() saves locals[] off the globals lump starting at
		 * parm_start and LeaveFunction() writes them back, so both ends of
		 * that range have to exist.  Subtraction rather than addition:
		 * parm_start + locals is two attacker-chosen ints. */
		if (f->parm_start < 0 || f->locals < 0 ||
		    f->locals > numglobals - f->parm_start ||
		    parmwords > numglobals - f->parm_start)
		{
			q_snprintf (reason, sizeof(reason),
				    "function %d has %d locals at global %d of %d",
				    i, f->locals, f->parm_start, numglobals);
			return reason;
		}
	}

	for (i = 0; i < numstatements; i++)
	{
		const dstatement_t	*st = &statements[i];
		const int		operand[3] = { st->a, st->b, st->c };

		if (st->op > OP_CASERANGE)	/* the table's last row */
		{
			q_snprintf (reason, sizeof(reason), "statement %d has opcode %d",
				    i, st->op);
			return reason;
		}

		for (k = 0; k < 3; k++)
		{
			int	ofs = operand[k];
			int	width;

			switch (pr_operandrole[st->op][k])
			{
			case PRO_UNUSED:
				continue;

			case PRO_JUMP:
				/* the dispatch loop does `st += ofs - 1' and then
				 * the st++ at the top of the next pass, so the
				 * statement that runs next is at i + ofs */
				if (v6)
					ofs = (signed short) ofs;
				if (ofs < -i || ofs >= numstatements - i)
				{
					q_snprintf (reason, sizeof(reason),
						    "statement %d jumps to %d of %d",
						    i, i + ofs, numstatements);
					return reason;
				}
				continue;

			case PRO_ARRAY:
				/* OP_FETCH_GBL_* reads the element count from the
				 * global below the base before it indexes, so the
				 * base cannot be global 0.  How far the indexing
				 * itself may reach depends on that count, which is
				 * a global the program can write at runtime -- the
				 * interpreter bounds that one for itself. */
				if (ofs < 1 || ofs >= numglobals)
				{
					q_snprintf (reason, sizeof(reason),
						    "statement %d indexes an array at global %d of %d",
						    i, ofs, numglobals);
					return reason;
				}
				continue;

			case PRO_VECTOR:
				width = 3;
				break;

			default:
				width = 1;
				break;
			}

			if (ofs < 0 || ofs > numglobals - width)
			{
				q_snprintf (reason, sizeof(reason),
					    "statement %d reaches global %d of %d",
					    i, ofs, numglobals);
				return reason;
			}
		}
	}

	switch (statements[numstatements - 1].op)
	{
	case OP_DONE:
	case OP_RETURN:
	case OP_GOTO:
		break;
	default:
		q_snprintf (reason, sizeof(reason),
			    "last statement is opcode %d, which falls off the end of the code",
			    statements[numstatements - 1].op);
		return reason;
	}

	return NULL;
}

/*
===============
PR_LoadProgs
===============
*/
void PR_LoadProgs (void)
{
	int			i;
	const char	*progname;
	const char	*progvstr;
	char		progsource[MAX_OSPATH];
	sv_def_t	*def;
	#if defined(H2W)
	char		num[32];
	dfunction_t	*f;
	#endif

	// flush the non-C variable lookup cache
	for (i = 0; i < GEFV_CACHESIZE; i++)
		gefvCache[i].field[0] = 0;

	progname = PR_GetProgFilename();
	progs = NULL;
#if !defined(H2W)
	{
		char	bundled[MAX_OSPATH];
		int	hunkmark = Hunk_LowMark ();

		/* Deliberately after PR_GetProgFilename() has returned: that
		 * function picks progs.dat vs progs2.dat by comparing
		 * maplist.txt's path_id against progs.dat's, and by now that
		 * comparison has already run against the real searchpath.  The
		 * bundle therefore cannot influence which file is wanted -- only
		 * where the bytes for it come from.  Keep it in this order. */
		if (PR_BundledProgsPath (progname, bundled, sizeof(bundled)))
		{
			const char	*bad;

			progs = (dprograms_t *) FS_LoadHunkFileFromOSPath (bundled);
			if (!progs)
				bad = "cannot be opened for reading";
			else
				bad = PR_CheckProgsExtents (progs, fs_filesize);

			if (bad)
			{
				/* The bundle is only ever an optimization over a
				 * file that already works, so anything doubtful
				 * about it surrenders to the player's copy instead
				 * of escalating to Host_Error.  Said out loud
				 * because a silent fallback would leave a player
				 * whose bundle got truncated or quarantined with no
				 * account of why their gamecode is not ours.
				 * FS_LoadHunkFile() below overwrites fs_filesize,
				 * file_from_pak and FS_LastFileSource() wholesale,
				 * so the provenance line still names what loaded. */
				Con_Printf ("Ignoring bundled %s: %s\n", bundled, bad);
				Hunk_FreeToLowMark (hunkmark);
				progs = NULL;
			}
		}
		else if (PR_GamecodeIsPakOnly ())
		{
			/* GAMECODE_PAKONLY.  Mutually exclusive with the bundle by
			 * construction -- PR_GamecodeState returns one state -- so the
			 * else is exhaustive rather than a priority.
			 *
			 * Generic over progname on purpose: whatever PR_GetProgFilename()
			 * settled on above is what gets looked up, so a data1 map that
			 * pulled progs2.dat via maplist.txt takes progs2.dat out of the
			 * pak, not progs.dat.  Nothing here knows or needs to know which
			 * file it is holding.
			 *
			 * Surrenders the same way the bundle does above: this state
			 * exists to reach a copy the player already has, so a pak that
			 * turns out not to hold one, or to hold a damaged one, falls back
			 * to normal resolution rather than refusing to start the map.
			 * Said out loud for the same reason -- the menu offered this and
			 * a silent no-op would look like the menu lying again.
			 * uhexen2-nt96. */
			const char	*bad;

			progs = (dprograms_t *) FS_LoadHunkFileFromPak (progname, NULL);
			if (!progs)
				bad = "no pak on the search path holds it";
			else
				bad = PR_CheckProgsExtents (progs, fs_filesize);

			if (bad)
			{
				Con_Printf ("Ignoring pak-only %s: %s\n", progname, bad);
				Hunk_FreeToLowMark (hunkmark);
				progs = NULL;
			}
		}
	}
#endif
	/* Normal resolution: no bundle, a gate declined, or one of the two
	 * special sources above was asked for and could not deliver. */
	if (!progs)
		progs = (dprograms_t *)FS_LoadHunkFile (progname, NULL);
	if (!progs)
		Host_Error ("%s: couldn't load %s", __thisfunc__, progname);
	/* FS_LastFileSource() describes the last lookup only, so grab it before
	 * anything below touches the filesystem again. */
	q_strlcpy (progsource, FS_LastFileSource(), sizeof(progsource));
	Con_DPrintf ("Programs occupy %ldK.\n", fs_filesize / 1024);

	/* Before the header byteswap below, which is itself a write through
	 * `progs' and needs the header to be there at all. */
	{
		const char	*bad = PR_CheckProgsExtents (progs, fs_filesize);

		if (bad)
			Host_Error ("%s: %s", progname, bad);
	}

	pr_crc = CRC_Block ((byte *)progs, fs_filesize);
	#if defined(H2W) /* add prog crc to the serverinfo */
	sprintf (num, "%u", pr_crc);
	Info_SetValueForStarKey (svs.info, "*progs", num, MAX_SERVERINFO_STRING);
	#endif

	// byte swap the header
	for (i = 0; i < (int) sizeof(*progs) / 4; i++)
		((int *)progs)[i] = LittleLong ( ((int *)progs)[i] );

	switch (progs->version) {
	case PROG_VERSION_V6:
		is_progs_v6 = true;
		break;
	case PROG_VERSION_V7:
		is_progs_v6 = false;
		break;
	default:
		Host_Error ("%s is of unsupported version (%d, should be %d or %d)",
			    progname, progs->version, PROG_VERSION_V6, PROG_VERSION_V7);
		return; /* silence compiler */
	}

	/* entityfields lies inside no lump, so PR_CheckProgsExtents() cannot
	 * speak for it, yet pr_edict_size below is computed straight from it:
	 * a negative value makes that size negative and a large one overflows
	 * the multiply, and every edict memset() in the engine -- ED_Alloc(),
	 * ED_ClearEdict(), the savegame loader -- is sized from the result.
	 * This is the guard CL_LoadCSProgs() already applies to csprogs.dat,
	 * on the path that loads progs.dat.  uhexen2-6z5o. */
	if (progs->entityfields < 0 ||
	    progs->entityfields > (INT_MAX - (int)sizeof(edict_t)) / 4)
		Host_Error ("%s has bad entityfields (%d)", progname, progs->entityfields);

	switch (progs->crc) {
	#if !defined(H2W) /* HEXEN2 PROGS: */
	case PROGS_V103_CRC:
		def = globals_v103;
		progvstr = "H2/v1.03";
		break;
	case PROGS_V111_CRC:
		def = globals_v111;
		progvstr = "H2/v1.11";
		break;
	case PROGS_V112_CRC:
		def = globals_v112;
		progvstr = "H2MP/v1.12";
		break;
	#else /* HEXENWORLD PROGS: */
	#if 0
	case PROGS_V009_CRC:
		def = globals_v009;
		progvstr = "HW/v0.09";
		break;
	#endif
	case PROGS_V011_CRC:
		def = globals_v011;
		progvstr = "HW/v0.11";
		break;
	case PROGS_V012_CRC:
		def = globals_v012;
		progvstr = "HW/v0.12";
		break;
	case PROGS_V014_CRC:
		def = globals_v014;
		progvstr = "HW/v0.14";
		break;
	case PROGS_V015_CRC:
		def = globals_v015;
		progvstr = "HW/v0.15";
		break;
	#endif
	default:
		Host_Error ("Unexpected crc ( %d ) for %s", progs->crc, progname);
		return; /* silence compiler */
	}

	pr_functions = (dfunction_t *)((byte *)progs + progs->ofs_functions);
	pr_strings = (char *)progs + progs->ofs_strings;
	/* extents, strings included, were checked above before anything wrote
	 * through the header */

	// initialize the strings
	pr_numknownstrings = 0;
	pr_maxknownstrings = 0;
	pr_stringssize = progs->numstrings;
	if (pr_knownstrings)
		Z_Free ((void *)pr_knownstrings);
	pr_knownstrings = NULL;
	PR_SetEngineString(pr_null_string);

	if (progs->version == PROG_VERSION_V6)
	{
		pr_globaldefs = PR_ConvertV6Defs ((ddef_v6_t *)((byte *)progs + progs->ofs_globaldefs), progs->numglobaldefs);
		pr_fielddefs  = PR_ConvertV6Defs ((ddef_v6_t *)((byte *)progs + progs->ofs_fielddefs),  progs->numfielddefs);
		pr_statements = PR_ConvertV6Stmts((dstatement_v6_t *)((byte *)progs + progs->ofs_statements), progs->numstatements);
	}
	else
	{
		pr_globaldefs = (ddef_t *)((byte *)progs + progs->ofs_globaldefs);
		pr_fielddefs = (ddef_t *)((byte *)progs + progs->ofs_fielddefs);
		pr_statements = (dstatement_t *)((byte *)progs + progs->ofs_statements);
	}

	Con_DPrintf ("Loaded %s, v%d, %d progdefs crc, %s structures\n",
			progname, progs->version, progs->crc, progvstr);

	/* uhexen2-8qp3: releases now ship compiled gamecode for players to install
	 * by hand, so "which progs is this bug report running?" needs an answer
	 * without `developer 1'.  The source path is the load-bearing half: a
	 * bare name cannot tell retail gamecode (inside pakN.pak) apart from a
	 * hand-installed loose progs.dat apart from a mod's.  pr_crc is the
	 * whole-file CRC, unlike the progdefs crc above, which only names the
	 * interface version and has three possible values.  Repeats are suppressed
	 * so a session logs one line, plus one more whenever the gamecode really
	 * changes -- e.g. a data1 map that pulls progs2.dat via maplist.txt.
	 *
	 * uhexen2-8r3e appends WHOSE code it is.  The path answers "which file",
	 * which is not the same question: our progs.dat hand-copied into the
	 * install's data1/ loads from a path indistinguishable from Raven's while
	 * running our fixes.  Extended in place rather than printed as a second
	 * line because this one is already what the release notes and
	 * gamecode/README ask bug reporters to paste. */
	{
		static char	lastreport[MAX_OSPATH + MAX_QPATH + 64];
		char		report[MAX_OSPATH + MAX_QPATH + 64];

#if !defined(H2W)
		/* Latched for the menu before the report is built, so a caller that
		 * never reaches the print still gets a current answer. */
		pr_gamecode_ident = PR_ClassifyGamecode ();
		q_snprintf (report, sizeof(report), "Gamecode: %s from %s (%s, file crc %u) -- %s\n",
			    progname, (progsource[0]) ? progsource : "unknown", progvstr, pr_crc,
			    pr_gamecode_ident);
#else
		q_snprintf (report, sizeof(report), "Gamecode: %s from %s (%s, file crc %u)\n",
			    progname, (progsource[0]) ? progsource : "unknown", progvstr, pr_crc);
#endif
		if (strcmp(report, lastreport) != 0)
		{
			q_strlcpy (lastreport, report, sizeof(lastreport));
			Con_Printf ("%s", report);
#if !defined(H2W)
			/* The line above states the fact; a reporter has to know the
			 * ident strings to see the contradiction in it.  Say it plainly
			 * when the player asked for their install's own gamecode and got
			 * ours: that means a loose copy of ours is sitting in the game
			 * directory, put there by the pre-bundle install instructions
			 * (uhexen2-8qp3), and no setting in this engine can get past it
			 * -- FS_AddGameDirectory ranks a gamedir's loose files above its
			 * own paks, so the retail file underneath is unreachable while
			 * that one is there.  Naming the resolved path is the whole
			 * value: the remedy is a file operation and the player needs to
			 * know which file.
			 *
			 * Inside the repeat guard so it keeps the provenance line's
			 * once-per-session cadence rather than shouting at every map
			 * spawn.  Prefix-matched because the ident carries a date stamp
			 * that moves every time the gamecode is rebuilt.  uhexen2-bflw. */
			if (!PR_GamecodeIsUpdated() &&
			    !strncmp (pr_gamecode_ident, "hexenwail", 9))
			{
				Con_Printf ("WARNING: gamecode source is set to the game's own file, but\n");
				Con_Printf ("  %s\n", (progsource[0]) ? progsource : "the file that loaded");
				Con_Printf ("is Hexenwail gamecode, not your install's original.\n");
				Con_Printf ("Remove or rename it to get Raven's.\n");
			}
#endif
		}
	}

	/* The walk below hands out the address of G_FLOAT(def->offset) for every
	 * entry in the table the progdefs CRC selected, and so trusts that CRC to
	 * guarantee numglobals covers the fixed globals block the table describes.
	 * A forged-large numglobals is refused by PR_CheckProgsExtents(), but a
	 * forged *small* one keeps the CRC of a layout the image no longer has and
	 * would place those pointers past the end of the globals lump -- where
	 * every later sv_globals read and write lands outside the image, for the
	 * whole life of the server.  Measure the table's own reach instead of
	 * trusting the CRC for it; `def' itself is the walk's cursor, so the
	 * measurement takes its own.  uhexen2-w70p. */
	{
		const sv_def_t	*d;
		int		need = 0;

		for (d = def; d->field; d++)
		{
			/* offsets come from offsetof() in the tables above, so the
			 * only question is how far past the last one the value it
			 * names extends: a vector is three globals wide. */
			int	end = d->offset + ((d->type == ev_vector) ? 3 : 1);

			if (end > need)
				need = end;
		}
		if (progs->numglobals < need)
			Host_Error ("%s declares %d globals, %s structures need %d",
				    progname, progs->numglobals, progvstr, need);
	}

	memset (&sv_globals, 0, sizeof(sv_globals));
	pr_globals = (float *)((byte *)progs + progs->ofs_globals);
	for (; def->field; def++)
		set_address (def, &G_FLOAT(def->offset));

	// byte swap the lumps
	for (i = 0; i < progs->numfunctions; i++)
	{
		pr_functions[i].first_statement = LittleLong (pr_functions[i].first_statement);
		pr_functions[i].parm_start = LittleLong (pr_functions[i].parm_start);
		pr_functions[i].s_name = LittleLong (pr_functions[i].s_name);
		pr_functions[i].s_file = LittleLong (pr_functions[i].s_file);
		pr_functions[i].numparms = LittleLong (pr_functions[i].numparms);
		pr_functions[i].locals = LittleLong (pr_functions[i].locals);
	}

	if (progs->version == PROG_VERSION_V7)
	{
		for (i = 0; i < progs->numstatements; i++)
		{
			pr_statements[i].op = LittleShort(pr_statements[i].op);
			pr_statements[i].a = LittleLong(pr_statements[i].a);
			pr_statements[i].b = LittleLong(pr_statements[i].b);
			pr_statements[i].c = LittleLong(pr_statements[i].c);
		}

		for (i = 0; i < progs->numglobaldefs; i++)
		{
			pr_globaldefs[i].type = LittleShort (pr_globaldefs[i].type);
			pr_globaldefs[i].ofs = LittleLong (pr_globaldefs[i].ofs);
			pr_globaldefs[i].s_name = LittleLong (pr_globaldefs[i].s_name);
		}

		for (i = 0; i < progs->numfielddefs; i++)
		{
			pr_fielddefs[i].type = LittleShort (pr_fielddefs[i].type);
			pr_fielddefs[i].ofs = LittleLong (pr_fielddefs[i].ofs);
			pr_fielddefs[i].s_name = LittleLong (pr_fielddefs[i].s_name);
		}
	}

	for (i = 0; i < progs->numfielddefs; i++)
	{
		if (pr_fielddefs[i].type & DEF_SAVEGLOBAL)
			Host_Error ("%s: pr_fielddefs[i].type & DEF_SAVEGLOBAL", __thisfunc__);
	}

	for (i = 0; i < progs->numglobals; i++)
		((int *)pr_globals)[i] = LittleLong (((int *)pr_globals)[i]);

	/* Last, because every table it reads has to be in host byte order first
	 * (and, for a v6 image, converted).  Fatal here rather than a fallback:
	 * unlike the bundled-progs check above there is no second copy left to
	 * try by this point, and the alternative to stopping is running bytecode
	 * that has been shown to index outside the image.  uhexen2-1p3o,
	 * uhexen2-im9e. */
	{
		const char	*bad = PR_ValidateBytecode (pr_statements, progs->numstatements,
							    pr_functions, progs->numfunctions,
							    progs->numglobals, is_progs_v6);
		if (bad)
			Host_Error ("%s: %s", progname, bad);
	}

	pr_edict_size = progs->entityfields * 4 + sizeof(edict_t) - sizeof(entvars_t);
	// round off to next highest whole word address (esp for Alpha)
	// this ensures that pointers in the engine data area are always
	// properly aligned
	pr_edict_size += sizeof(void *) - 1;
	pr_edict_size &= ~(sizeof(void *) - 1);

#if !defined(SERVERONLY)
	// set the cl_playerclass value after sv_globals has been created
	if (sv_globals.cl_playerclass)
		*sv_globals.cl_playerclass = cl_playerclass.value;
#endif
#if defined(H2W)
	// Zoid, find the spectator functions
	SpectatorConnect = SpectatorThink = SpectatorDisconnect = 0;

	if ((f = ED_FindFunction ("SpectatorConnect")) != NULL)
		SpectatorConnect = (func_t)(f - pr_functions);
	if ((f = ED_FindFunction ("SpectatorThink")) != NULL)
		SpectatorThink = (func_t)(f - pr_functions);
	if ((f = ED_FindFunction ("SpectatorDisconnect")) != NULL)
		SpectatorDisconnect = (func_t)(f - pr_functions);
#endif
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	Cmd_AddCommand ("edict", ED_PrintEdict_f);
	Cmd_AddCommand ("edicts", ED_PrintEdicts);
	Cmd_AddCommand ("edictcount", ED_Count);
	Cmd_AddCommand ("profile", PR_Profile_f);

	Cvar_RegisterVariable (&max_temp_edicts);
#if !defined(H2W)
	Cvar_RegisterVariable (&sv_gamecode);
#endif

#if !defined(H2W)
	Cvar_RegisterVariable (&nomonsters);
	Cvar_RegisterVariable (&gamecfg);
	Cvar_RegisterVariable (&savedgamecfg);
	Cvar_RegisterVariable (&scratch1);
	Cvar_RegisterVariable (&scratch2);
	Cvar_RegisterVariable (&scratch3);
	Cvar_RegisterVariable (&scratch4);
	Cvar_RegisterVariable (&saved1);
	Cvar_RegisterVariable (&saved2);
	Cvar_RegisterVariable (&saved3);
	Cvar_RegisterVariable (&saved4);
#endif
}

/*
==================
PR_SaveVMState / PR_RestoreVMState

Save and restore all progs VM state for CSQC VM switching.
This allows a second QC VM to run without disturbing the server VM.
==================
*/
void PR_SaveVMState (pr_vmstate_t *state)
{
	state->progs = progs;
	state->functions = pr_functions;
	state->statements = pr_statements;
	state->globals = pr_globals;
	state->sv_globals = sv_globals;
	state->edict_size = pr_edict_size;
	state->is_v6 = is_progs_v6;
	state->crc = pr_crc;
	state->strings = pr_strings;
	state->stringssize = pr_stringssize;
	state->knownstrings = pr_knownstrings;
	state->maxknownstrings = pr_maxknownstrings;
	state->numknownstrings = pr_numknownstrings;
	state->fielddefs = pr_fielddefs;
	state->globaldefs = pr_globaldefs;
	state->builtins = pr_builtins;
	state->numbuiltins = pr_numbuiltins;
	state->xfunction = pr_xfunction;
	state->xstatement = pr_xstatement;
	state->argc = pr_argc;
	state->trace = pr_trace;
}

void PR_RestoreVMState (const pr_vmstate_t *state)
{
	progs = state->progs;
	pr_functions = state->functions;
	pr_statements = state->statements;
	pr_globals = state->globals;
	sv_globals = state->sv_globals;
	pr_edict_size = state->edict_size;
	is_progs_v6 = state->is_v6;
	pr_crc = state->crc;
	pr_strings = state->strings;
	pr_stringssize = state->stringssize;
	pr_knownstrings = state->knownstrings;
	pr_maxknownstrings = state->maxknownstrings;
	pr_numknownstrings = state->numknownstrings;
	pr_fielddefs = state->fielddefs;
	pr_globaldefs = state->globaldefs;
	pr_builtins = state->builtins;
	pr_numbuiltins = state->numbuiltins;
	pr_xfunction = state->xfunction;
	pr_xstatement = state->xstatement;
	pr_argc = state->argc;
	pr_trace = state->trace;
}

//===========================================================================


edict_t *EDICT_NUM(int n)
{
	if (n < 0 || n >= MAX_EDICTS)
		Host_Error ("%s: bad number %i", __thisfunc__, n);
	return (edict_t *)((byte *)sv.edicts + (n)*pr_edict_size);
}

int NUM_FOR_EDICT(edict_t *e)
{
	int		b;

	b = (byte *)e - (byte *)sv.edicts;
	b = b / pr_edict_size;

	if (b < 0 || b >= sv.num_edicts)
	{
		if (!RemoveBadReferences)
		{
			Con_DPrintf ("%s: bad pointer, Class: %s Field: %s, Index %d, Total %d\n",
					__thisfunc__, class_name, field_name, b, sv.num_edicts);
		}
		return 0;
	}
	if (e->free && RemoveBadReferences)
	{
	//	Con_DPrintf ("%s: freed edict, Class: %s Field: %s, Index %d, Total %d\n",
	//			__thisfunc__, class_name, field_name, b, sv.num_edicts);
		return 0;
	}
	return b;
}

//===========================================================================


#define	PR_STRING_ALLOCSLOTS	256

static void PR_AllocStringSlots (void)
{
	pr_maxknownstrings += PR_STRING_ALLOCSLOTS;
	Sys_DPrintf("%s: realloc'ing for %d slots\n", __thisfunc__, pr_maxknownstrings);
	pr_knownstrings = (const char **) Z_Realloc ((void *)pr_knownstrings, pr_maxknownstrings * sizeof(char *), Z_MAINZONE);
}

const char *PR_GetString (int num)
{
	if (num >= 0 && num < pr_stringssize)
		return pr_strings + num;
	else if (num < 0 && num >= -pr_numknownstrings)
	{
		if (!pr_knownstrings[-1 - num])
		{
			Host_Error ("%s: attempt to get a non-existant string %d\n",
								__thisfunc__, num);
			return "";
		}
		return pr_knownstrings[-1 - num];
	}
	else
	{
		Host_Error ("%s: invalid string offset %d\n", __thisfunc__, num);
		return "";
	}
}

int PR_SetEngineString (const char *s)
{
	int		i;

	if (!s)
		return 0;
#if 0	/* can't: sv.model_precache & sv.sound_precache points to pr_strings */
	if (s >= pr_strings && s <= pr_strings + pr_stringssize)
		Host_Error ("%s: \"%s\" is in pr_strings area\n", __thisfunc__, s);
#else
	if (s >= pr_strings && s <= pr_strings + pr_stringssize - 2)
		return (int)(s - pr_strings);
#endif
	for (i = 0; i < pr_numknownstrings; i++)
	{
		if (pr_knownstrings[i] == s)
			return -1 - i;
	}
	// new unknown engine string
	DEBUG_Printf ("%s: new engine string %p\n", __thisfunc__, s);
#if 0
	for (i = 0; i < pr_numknownstrings; i++)
	{
		if (!pr_knownstrings[i])
			break;
	}
#endif
//	if (i >= pr_numknownstrings)
//	{
		if (i >= pr_maxknownstrings)
			PR_AllocStringSlots();
		pr_numknownstrings++;
//	}
	pr_knownstrings[i] = s;
	return -1 - i;
}

int PR_AllocString (int size, char **ptr)
{
	int		i;

	if (!size)
		return 0;
	for (i = 0; i < pr_numknownstrings; i++)
	{
		if (!pr_knownstrings[i])
			break;
	}
//	if (i >= pr_numknownstrings)
//	{
		if (i >= pr_maxknownstrings)
			PR_AllocStringSlots();
		pr_numknownstrings++;
//	}
	pr_knownstrings[i] = (char *)Hunk_AllocName(size, "string");
	if (ptr)
		*ptr = (char *) pr_knownstrings[i];
	return -1 - i;
}

