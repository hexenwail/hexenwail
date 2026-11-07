/* sv_main_target.c -- the facts engine/rust/src/sv_main_*.rs need from
 * Hexen II's headers and globals.  See sv_main_target.h.  Nothing here is
 * game logic: each value is a header constant, an offsetof, a sizeof, one
 * global or its address -- and the fat PVS buffer's storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#include "quakedef.h"
#include "sv_main_target.h"

const sv_main_target_facts_t SvMain_TargetFacts =
{
	svc_particle, svc_particle2, svc_particle3, svc_particle4,
	svc_sound, svc_stopsound, svc_stopsound_large,
	svc_sound_update_pos, svc_sound_update_pos_large,
	SND_VOLUME, SND_ATTENUATION, SND_OVERFLOW, SND_OVERFLOW2,
	SND_LARGEENTITY, SND_MAX_PACKED_ENTITY, DEFAULT_SOUND_PACKET_VOLUME,
	MAX_DATAGRAM, MAX_DATAGRAM_OLD, MAX_DATAGRAM_114,
	MAX_MSGLEN, MAX_MSGLEN_OLD,
	MAX_SOUNDS, MAX_SOUNDS_OLD, MAX_SOUNDS_H2MP, MAX_SOUNDS_UH2,
	PROTOCOL_RAVEN_111, PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1,
	DEFAULT_SOUND_PACKET_ATTENUATION
};

struct sizebuf_s *SvMain_Datagram (void)
{
	return &sv.datagram;
}

const char **SvMain_SoundPrecache (void)
{
	return sv.sound_precache;
}

/* sv_main.c's entity-update cvars; no header declares them */
extern	cvar_t	sv_update_player, sv_update_monsters, sv_update_missiles;
extern	cvar_t	sv_update_misc, sv_netsort;

#define V(member)	offsetof (entvars_t, member)
#define CL(member)	offsetof (client_t, member)

const sv_main_ents_facts_t SvMainEnts_TargetFacts =
{
	V(effects), V(model), V(colormap), V(skin), V(drawflags), V(frame),
	V(scale), V(abslight), V(cameramode),
	offsetof (edict_t, baseline),
	sizeof (client_t), CL(playerclass), CL(current_frame),
	CL(last_frame), CL(current_sequence), CL(last_sequence),
	offsetof (qmodel_t, numleafs),
	sizeof (entity_state_t), sizeof (entity_state2_t), sizeof (client_frames_t),
	sizeof (client_state2_t), offsetof (client_frames_t, count),
	MAX_CLIENT_STATES, MAX_FRAMES, CLEAR_LIMIT, MAX_CLEAR_EDICTS_PER_MSG,
	ENT_CLEARED, EF_NODRAW, EF_MUZZLEFLASH, FL_CLIENT, FL_CLASS_DEPENDENT,
	MOVETYPE_FLYMISSILE, MOVETYPE_BOUNCEMISSILE, MOVETYPE_BOUNCE, MOVETYPE_STEP,
	CONTENTS_SOLID, ENTALPHA_DEFAULT, MAX_QPATH,
	svc_reference, svc_clear_edicts,
	U_MOREBITS, U_ORIGIN1, U_ANGLE1, U_ANGLE2, U_ANGLE3, U_NOLERP, U_FRAME,
	U_SIGNAL, U_MODEL, U_CLEAR_ENT, U_LONGENTITY, U_MOREBITS2, U_SKIN,
	U_EFFECTS, U_SCALE, U_COLORMAP, U_ALPHA
};

struct edict_s *SvMain_Edicts (void)		{ return sv.edicts; }
int SvMain_NumEdicts (void)			{ return sv.num_edicts; }
int SvMain_EdictSize (void)			{ return pr_edict_size; }
struct qmodel_s *SvMain_WorldModel (void)	{ return sv.worldmodel; }
void *SvMain_States (void)			{ return sv.states; }
struct client_s *SvMain_Clients (void)		{ return svs.clients; }
int SvMain_Protocol (void)			{ return sv_protocol; }
int SvMain_UpdatePlayer (void)			{ return sv_update_player.integer; }
int SvMain_UpdateMonsters (void)		{ return sv_update_monsters.integer; }
int SvMain_UpdateMissiles (void)		{ return sv_update_missiles.integer; }
int SvMain_UpdateMisc (void)			{ return sv_update_misc.integer; }
int SvMain_NetSort (void)			{ return sv_netsort.integer; }

/* sv_main.c's `static byte fatpvs[MAX_MAP_LEAFS/8]', which SV_FatPVS fills
 * and returns; sized by this target's header */
static byte	fatpvs[MAX_MAP_LEAFS/8];
unsigned char *SvMain_FatPvs (void)		{ return fatpvs; }
size_t SvMain_FatPvsSize (void)			{ return sizeof (fatpvs); }

/* sv_main_datagram.rs (#469 slice C): the clientdata/inventory message
 * numbers and bit masks, the entvars_t/client_t/ex_inventory_page_t member
 * offsets, and the globals the six functions read and write. */

#define EX(member)	offsetof (ex_inventory_page_t, member)

const sv_main_datagram_facts_t SvMainDatagram_TargetFacts =
{
	svc_damage, svc_setangle, svc_clientdata, svc_update_inv,
	svc_time, svc_nop, svc_updatefrags, svc_signonnum,
	SU_VIEWHEIGHT, SU_IDEALPITCH, SU_IDEALROLL,
	SU_PUNCH1, SU_VELOCITY1,
	SU_ONGROUND, SU_WEAPONFRAME, SU_ARMOR, SU_WEAPON,
	SC1_HEALTH, SC1_LEVEL, SC1_INTELLIGENCE, SC1_WISDOM, SC1_STRENGTH,
	SC1_DEXTERITY, SC1_WEAPON, SC1_BLUEMANA, SC1_GREENMANA,
	SC1_EXPERIENCE, SC1_CNT_TORCH, SC1_CNT_H_BOOST, SC1_CNT_SH_BOOST,
	SC1_CNT_MANA_BOOST, SC1_CNT_TELEPORT, SC1_CNT_TOME, SC1_CNT_SUMMON,
	SC1_CNT_INVISIBILITY, SC1_CNT_GLYPH, SC1_CNT_HASTE, SC1_CNT_BLAST,
	SC1_CNT_POLYMORPH, SC1_CNT_FLIGHT, SC1_CNT_CUBEOFFORCE,
	SC1_CNT_INVINCIBILITY, SC1_ARTIFACT_ACTIVE, SC1_ARTIFACT_LOW,
	SC1_MOVETYPE, SC1_CAMERAMODE, SC1_HASTED, SC1_INVENTORY,
	SC1_RINGS_ACTIVE,
	SC2_RINGS_LOW, SC2_AMULET, SC2_BRACER, SC2_BREASTPLATE, SC2_HELMET,
	SC2_FLIGHT_T, SC2_WATER_T, SC2_TURNING_T, SC2_REGEN_T,
	SC2_HASTE_T, SC2_TOME_T,
	SC2_PUZZLE1, SC2_PUZZLE2, SC2_PUZZLE3, SC2_PUZZLE4,
	SC2_PUZZLE5, SC2_PUZZLE6, SC2_PUZZLE7, SC2_PUZZLE8,
	SC2_MAXHEALTH, SC2_MAXMANA, SC2_FLAGS, SC2_OBJ, SC2_OBJ2,
	MAX_INVENTORY_EX, FL_ONGROUND,
	offsetof (edict_t, v), sizeof (entvars_t),
	V(dmg_take), V(dmg_save), V(dmg_inflictor),
	V(origin), V(mins), V(maxs),
	V(fixangle), V(angles), V(view_ofs), V(idealpitch), V(idealroll),
	V(punchangle), V(velocity),
	V(weaponframe), V(armorvalue), V(weaponmodel), V(flags),
	V(health), V(level), V(intelligence), V(wisdom), V(strength),
	V(dexterity), V(weapon), V(bluemana), V(greenmana), V(experience),
	V(cnt_torch), V(cnt_h_boost), V(cnt_sh_boost), V(cnt_mana_boost),
	V(cnt_teleport), V(cnt_tome), V(cnt_summon), V(cnt_invisibility),
	V(cnt_glyph), V(cnt_haste), V(cnt_blast), V(cnt_polymorph),
	V(cnt_flight), V(cnt_cubeofforce), V(cnt_invincibility),
	V(artifact_active), V(artifact_low), V(movetype), V(cameramode),
	V(hasted), V(inventory), V(rings_active), V(rings_low),
	V(armor_amulet), V(armor_bracer), V(armor_breastplate), V(armor_helmet),
	V(ring_flight), V(ring_water), V(ring_turning), V(ring_regeneration),
	V(haste_time), V(tome_time),
	V(puzzle_inv1), V(puzzle_inv2), V(puzzle_inv3), V(puzzle_inv4),
	V(puzzle_inv5), V(puzzle_inv6), V(puzzle_inv7), V(puzzle_inv8),
	V(max_health), V(max_mana), V(frags),
	sizeof (client_t), CL(old_v), CL(send_all_v), CL(edict),
	CL(message), CL(datagram), CL(netconnection),
	CL(active), CL(spawned), CL(signon_buffer),
	CL(sendsignon), CL(last_message), CL(dropasap),
	CL(old_frags), CL(info_mask), CL(info_mask2),
	CL(name), CL(ex_inventory),
	EX(changed_items), EX(new_items), EX(item_cnt), EX(item_id), EX(next)
};

/* sv_main.c's file-scope globals this slice reads and writes */
extern	unsigned int	info_mask, info_mask2;
extern	cvar_t		sv_maxdatagram;
extern	double		realtime;

struct client_s **SvMain_HostClient (void)	{ return &host_client; }
int SvMain_MaxClients (void)			{ return svs.maxclients; }
struct sizebuf_s *SvMain_ReliableDatagram (void) { return &sv.reliable_datagram; }
double SvMain_Time (void)			{ return sv.time; }
int SvMain_DatagramDropped (void)		{ return sv_datagram_dropped; }
void SvMain_SetDatagramDropped (int v)		{ sv_datagram_dropped = v; }
unsigned int SvMain_InfoMask (void)		{ return info_mask; }
unsigned int SvMain_InfoMask2 (void)		{ return info_mask2; }
int SvMain_MaxDatagramCvar (void)		{ return sv_maxdatagram.integer; }
double SvMain_Realtime (void)			{ return realtime; }

/* sv_main_connect.rs (#469 slice D) */
#if !defined(SERVERONLY) && defined(GLQUAKE)
#define SV_MAIN_CONNECT_MODEL_EFFECTS	1
#else
#define SV_MAIN_CONNECT_MODEL_EFFECTS	0
#endif

#define ES(member)	offsetof (entity_state_t, member)
#define EXI(member)	offsetof (ex_item_t, member)

const sv_main_connect_facts_t SvMainConnect_TargetFacts =
{
	PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112, PROTOCOL_UQE_113,
	PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1,
	svc_print, svc_serverinfo, svc_cdtrack, svc_midi_name, svc_mod_name,
	svc_skybox, svc_setview, svc_signonnum, svc_spawnbaseline, svc_stufftext,
	GAME_COOP, GAME_DEATHMATCH,
	MAX_MODELS, MAX_SOUNDS, MAX_QPATH,
	MAX_SIGNON_SIZE, MAX_SIGNON_BUFFERS, NUM_SPAWN_PARMS,
	ENTALPHA_DEFAULT,
	ENGINE_VERSION,
	SV_MAIN_CONNECT_MODEL_EFFECTS,
#if SV_MAIN_CONNECT_MODEL_EFFECTS
	offsetof (qmodel_t, ex_flags), offsetof (qmodel_t, glow_settings),
#else
	0, 0,
#endif
	sizeof (client_t), CL(message), CL(msgbuf), sizeof (((client_t *)0)->msgbuf),
	CL(datagram), CL(datagram_buf), sizeof (((client_t *)0)->datagram_buf),
	CL(netconnection), CL(spawn_parms), CL(name), sizeof (((client_t *)0)->name),
	CL(active), CL(spawned), CL(send_all_v), CL(signon_buffer),
	CL(edict), CL(ex_inventory), CL(sendsignon),
	sizeof (client_state2_t),
	offsetof (edict_t, v), offsetof (edict_t, baseline),
	offsetof (edict_t, free),
	V(origin), V(angles), V(frame), V(skin), V(scale), V(drawflags),
	V(abslight), V(model), V(modelindex),
	ES(origin), ES(angles), ES(modelindex), ES(frame), ES(colormap),
	ES(skin), ES(scale), ES(drawflags), ES(abslight), ES(alpha),
	ES(ClearCount),
	EXI(id), EXI(icon)
};

int SvMain_Coop (void)				{ return coop.integer; }
int SvMain_Deathmatch (void)			{ return deathmatch.integer; }
int SvMain_KingOfHill (void)			{ return sv_kingofhill; }
int SvMain_LoadGame (void)			{ return sv.loadgame; }
unsigned short SvMain_PrecacheCRC (void)	{ return pr_crc; }
struct sizebuf_s *SvMain_Signon (void)		{ return &sv.signon; }
unsigned char *SvMain_SignonBufs (void)		{ return (unsigned char *)sv.signon_bufs; }
int SvMain_NumSignonBuffers (void)		{ return sv.num_signon_buffers; }
void SvMain_SetNumSignonBuffers (int v)		{ sv.num_signon_buffers = v; }
int SvMain_SignonSize (int i)			{ return sv.signon_size[i]; }
void SvMain_SetSignonSize (int i, int v)	{ sv.signon_size[i] = v; }
const char **SvMain_ModelPrecache (void)	{ return sv.model_precache; }
int SvMain_NumExItems (void)			{ return sv.num_ex_items; }
void *SvMain_ExItems (void)			{ return sv.ex_items; }
unsigned char SvMain_CdTrack (void)		{ return sv.cd_track; }
const char *SvMain_MidiName (void)		{ return sv.midi_name; }
const char *SvMain_ModName (void)		{ return sv.mod_name; }
const char *SvMain_Skybox (void)		{ return sv.skybox; }
int *SvMain_NetActiveConnections (void)		{ return &net_activeconnections; }
struct qmodel_s **SvMain_Models (void)		{ return sv.models; }
/* host.h's isDedicated; the dedicated server's own sys_unix.c defines it
 * true, the client's sets it from -dedicated. */
extern qboolean	isDedicated;
int SvMain_IsDedicated (void)			{ return isDedicated; }

/* E1 / slice E: sv_main_inv.rs (#469 slice E, unit E1).  EX() and CL() are
 * slice C's and D's macros above; V() is slice B's. */
#define EXP(member)	offsetof (ex_inventory_page_t, member)

const sv_main_inv_facts_t SvMainInv_TargetFacts =
{
	MAX_INVENTORY_EX, MAX_INVENTORY_EX_PAGES, MAX_CLIENTS, EOF,
	sizeof (ex_inventory_page_t), EXP(id), EXP(client_id),
	EXP(changed_items), EXP(new_items),
	EXP(item_id), EXP(item_cnt), EXP(next),
	sizeof (client_t), CL(active), CL(ex_inventory),
	offsetof (edict_t, v), V(message), V(netname),
	offsetof (qmodel_t, entities), sizeof (((server_t *)0)->skybox)
};

void *SvMain_InventoryPages (void)		{ return sv.ex_inventory_pages; }
int *SvMain_NextPageId (void)			{ return &sv.next_page_id; }
char *SvMain_SkyboxBuf (void)			{ return sv.skybox; }
int *SvMain_ServerFlags (void)			{ return &svs.serverflags; }
float *SvMain_GlobalServerFlags (void)		{ return sv_globals.serverflags; }
int SvMain_HostStringCount (void)		{ return host_string_count; }
char *SvMain_ComToken (void)			{ return com_token; }
/* sv_main_spawn.rs (E2 / slice E of #469): SV_SpawnServer and the map_checks
 * readout.  The build facts are the C's own preprocessor tests, answered
 * here because this file is compiled per target. */
#if !defined(SERVERONLY)
#define SV_MAIN_SPAWN_CLIENT	1
#else
#define SV_MAIN_SPAWN_CLIENT	0
#endif
#if defined(GLQUAKE)
#define SV_MAIN_SPAWN_GLQUAKE	1
#else
#define SV_MAIN_SPAWN_GLQUAKE	0
#endif

/* qboolean is an enum; the Rust reads and writes these members as int */
COMPILE_TIME_ASSERT(sv_main_spawn_qboolean, sizeof (qboolean) == sizeof (int));

/* sv_main.c's file-scope state SV_SpawnServer reads; not static there for
 * this file's sake */
extern	char		localmodels[MAX_MODELS][8];
extern	qboolean	sv_protocol_userset;
extern	cvar_t		max_edicts;
#if SV_MAIN_SPAWN_CLIENT
/* gl_screen.c's / screen.c's; no header declares it (sv_main.c had its own) */
extern	float		scr_centertime_off;
#endif

#define SV(member)	offsetof (server_t, member)
#define SVS(member)	offsetof (server_static_t, member)
#define MEMBER_SIZE(type, member)	sizeof (((type *)0)->member)

const sv_main_spawn_facts_t SvMainSpawn_TargetFacts =
{
	SV_MAIN_SPAWN_CLIENT, SV_MAIN_SPAWN_GLQUAKE,
	_PRINT_NORMAL, _PRINT_DEVEL, _PRINT_SAFE,
	MAPCHECK_FAILED, MAPCHECK_PARTIAL, MAPCHECK_OK, MAPCHECK_NA,
	PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1,
	MIN_EDICTS, MAX_EDICTS, MAX_MODELS, MAX_QPATH, MAX_OSPATH,
	MAX_SIGNON_SIZE, MAX_SIGNON_BUFFERS, MAX_ITEMS_EX, MAX_INVENTORY_EX_PAGES,
	SOLID_BSP, MOVETYPE_PUSH, ss_loading, ss_active,
	SV(active), SV(paused), SV(time), SV(name), MEMBER_SIZE (server_t, name),
	SV(midi_name), SV(mod_name), MEMBER_SIZE (server_t, mod_name), SV(cd_track),
	SV(startspot), MEMBER_SIZE (server_t, startspot),
	SV(modelname), MEMBER_SIZE (server_t, modelname),
	SV(worldmodel), SV(model_precache), SV(models), SV(sound_precache),
	SV(states), SV(num_edicts), SV(edicts), SV(state),
	SV(datagram), SV(datagram_buf), MEMBER_SIZE (server_t, datagram_buf),
	SV(reliable_datagram), SV(reliable_datagram_buf),
	MEMBER_SIZE (server_t, reliable_datagram_buf),
	SV(signon), SV(num_signon_buffers), SV(signon_size), SV(signon_bufs),
	SV(ex_items), SV(ex_inventory_pages), SV(num_ex_items),
	SV(mapchecks.active), SV(mapchecks.numwarnings),
	SV(mapchecks.changelevel), SV(mapchecks.changespot),
	SV(mapchecks.trigger_changelevel), SV(mapchecks.valid_changelevel),
	SV(mapchecks.intermission), SV(mapchecks.skill_triggers),
	SV(mapchecks.coop_spawns), SV(mapchecks.dm_spawns),
	SV(mapchecks.sp_spawns), SV(mapchecks.skill_ents),
	SVS(maxclients), SVS(clients), SVS(serverflags), SVS(changelevel_issued),
	sizeof (client_t), CL(active), CL(edict), CL(send_all_v), CL(ex_inventory),
	offsetof (edict_t, free), offsetof (edict_t, freetime), offsetof (edict_t, v),
	V(model), V(modelindex), V(solid), V(movetype), V(message),
	offsetof (sizebuf_t, name), offsetof (sizebuf_t, cursize),
	sizeof (client_state2_t), sizeof (ex_item_t), EXI(id), EXI(icon),
	sizeof (ex_inventory_page_t),
	offsetof (dprograms_t, numfunctions), offsetof (dprograms_t, entityfields),
	sizeof (dfunction_t), offsetof (dfunction_t, first_statement),
	offsetof (sv_globals_t, coop), offsetof (sv_globals_t, deathmatch),
	offsetof (sv_globals_t, randomclass), offsetof (sv_globals_t, mapname),
	offsetof (sv_globals_t, startspot), offsetof (sv_globals_t, serverflags),
	offsetof (qmodel_t, name), offsetof (qmodel_t, numsubmodels),
	offsetof (qmodel_t, lightdata), offsetof (qmodel_t, visdata),
	offsetof (qmodel_t, entities),
#if SV_MAIN_SPAWN_CLIENT
	offsetof (qmodel_t, numtextures), offsetof (qmodel_t, textures),
	offsetof (texture_t, name), offsetof (texture_t, width),
	offsetof (texture_t, height),
#else
	0, 0, 0, 0, 0,
#endif
	sizeof (localmodels[0])
};

/* SV_SpawnServer's `static char dummy[8] = { 0,0,0,0,0,0,0,0 };' */
static char	precache_dummy[8];

unsigned char *SvMainSpawn_Server (void)	{ return (unsigned char *)&sv; }
unsigned char *SvMainSpawn_Static (void)	{ return (unsigned char *)&svs; }
unsigned char *SvMainSpawn_Globals (void)	{ return (unsigned char *)&sv_globals; }
unsigned char *SvMainSpawn_Progs (void)		{ return (unsigned char *)progs; }
unsigned char *SvMainSpawn_Functions (void)	{ return (unsigned char *)pr_functions; }
int *SvMainSpawn_PhysicsWalk (void)		{ return &sv_physics_walk; }
int *SvMainSpawn_CurrentSkill (void)		{ return &current_skill; }
double *SvMainSpawn_HostFrametime (void)	{ return &host_frametime; }
int *SvMainSpawn_Protocol (void)		{ return &sv_protocol; }
int SvMainSpawn_ProtocolUserset (void)		{ return sv_protocol_userset; }
int *SvMainSpawn_MaxEdicts (void)		{ return &sv_max_edicts; }
char *SvMainSpawn_LocalModels (void)		{ return localmodels[0]; }
const char *SvMainSpawn_GamedirNoPath (void)	{ return fs_gamedir_nopath; }
unsigned char **SvMainSpawn_HostClient (void)	{ return (unsigned char **)&host_client; }
int SvMainSpawn_EdictSize (void)		{ return pr_edict_size; }
char *SvMainSpawn_PrecacheDummy (void)		{ return precache_dummy; }
const char *SvMainSpawn_HostnameString (void)	{ return hostname.string; }
int SvMainSpawn_CoopInteger (void)		{ return coop.integer; }
float SvMainSpawn_CoopValue (void)		{ return coop.value; }
float SvMainSpawn_DeathmatchValue (void)	{ return deathmatch.value; }
int SvMainSpawn_SkillInteger (void)		{ return skill.integer; }
int SvMainSpawn_MapChecksInteger (void)		{ return map_checks.integer; }
int SvMainSpawn_DeveloperInteger (void)		{ return developer.integer; }
float SvMainSpawn_MaxEdictsValue (void)		{ return max_edicts.value; }
int SvMainSpawn_MaxTempEdictsInteger (void)	{ return max_temp_edicts.integer; }
float SvMainSpawn_RandomclassValue (void)	{ return randomclass.value; }

#if SV_MAIN_SPAWN_CLIENT
float *SvMainSpawn_CenterTimeOff (void)		{ return &scr_centertime_off; }
int *SvMainSpawn_TotalLoadingSize (void)	{ return &total_loading_size; }
int *SvMainSpawn_CurrentLoadingSize (void)	{ return &current_loading_size; }
int *SvMainSpawn_LoadingStage (void)		{ return &loading_stage; }
void SvMainSpawn_ShowLoadingSize (void)		{ D_ShowLoadingSize (); }
void SvMainSpawn_EndLoadingPlaque (void)	{ SCR_EndLoadingPlaque (); }
#else
float *SvMainSpawn_CenterTimeOff (void)		{ return NULL; }
int *SvMainSpawn_TotalLoadingSize (void)	{ return NULL; }
int *SvMainSpawn_CurrentLoadingSize (void)	{ return NULL; }
int *SvMainSpawn_LoadingStage (void)		{ return NULL; }
void SvMainSpawn_ShowLoadingSize (void)		{ }
void SvMainSpawn_EndLoadingPlaque (void)	{ }
#endif
#if SV_MAIN_SPAWN_GLQUAKE
int *SvMainSpawn_FlushTextures (void)		{ return (int *)&flush_textures; }
#else
int *SvMainSpawn_FlushTextures (void)		{ return NULL; }
#endif
