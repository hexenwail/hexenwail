/* hexen2/server.h
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

#ifndef __HX2_SERVER_H
#define __HX2_SERVER_H

typedef struct
{
	int			maxclients;
	int			maxclientslimit;
	struct client_s	*clients;		// [maxclients]
	int			serverflags;	// episode completion information
	qboolean	changelevel_issued;	// cleared when at SV_SpawnServer
} server_static_t;

//=============================================================================

typedef enum
{
	ss_loading,
	ss_active
} server_state_t;

typedef struct ex_item_s
{
	int		id;
	char		icon[MAX_QPATH];
} ex_item_t;

typedef struct ex_inventory_page_s
{
	int		id;
	int		client_id;
	int		changed_items;		// inventory change bit flags
	int		new_items;		// inventory change bit flags
	int		item_id[32];
	int		item_cnt[32];
	int		inv_order[32];
	float	item_gettime[32];	// cl.time of aquiring item, for blinking
	struct ex_inventory_page_s *next;
	//struct ex_inventory_page_s	*next2;
} ex_inventory_page_t;

/*
 * Signon buffers.
 *
 * The signon is everything a joining client needs to reproduce the map's
 * starting state: an svc_spawnbaseline per model-bearing edict (20 bytes),
 * an svc_spawnstatic per makestatic (18), an svc_spawnstaticsound per
 * ambientsound (11), plus whatever QC writes to MSG_INIT.
 *
 * It used to be a single NET_MAXMESSAGE buffer with allowoverflow false, so
 * the first write past 32 KB was a Sys_Error straight to desktop.  That
 * described about 1600 baselines against a MAX_EDICTS of 8192 -- the engine
 * was advertising an entity budget it could not hand to a client -- and the
 * shipped SoT maps already sat around two thirds of it, so a dense community
 * map went over (uhexen2-z5wt, BloodShot's tristram).
 *
 * So: a list, filled one buffer at a time and sent one reliable message at a
 * time.  A record must never straddle two buffers, because the client parses
 * each message on its own and half a record at the end of one would desync
 * the stream -- that is what SV_ReserveSignonSpace is for.  Call it with the
 * size of the record you are about to write, before writing any of it.
 *
 * MAX_SIGNON_SIZE is half of NET_MAXMESSAGE rather than all of it: each
 * buffer gets copied into client->message, which is MAX_MSGLEN and may
 * already be carrying unrelated reliable data, so exactly filling it would
 * trade this crash for an overflowed-client drop.
 */
#define	MAX_SIGNON_SIZE		(NET_MAXMESSAGE / 2)
#define	MAX_SIGNON_BUFFERS	24

typedef struct
{
	qboolean	active;		// false if only a net client

	qboolean	paused;
	qboolean	loadgame;	// handle connections specially

	double		time;

	int		lastcheck;	// used by PF_checkclient
	double		lastchecktime;

	char		name[64];	// map name
	char		midi_name[128];	// midi file name
	/* svc_mod_name / svc_skybox, protocol 20+ (UQE 1.13).  Both used to
	 * be sent as literal "" -- see uhexen2-6dgk.  skybox is the sky the
	 * map's worldspawn declares; the client already read the same key
	 * out of the BSP, so this is normally a confirmation, and matters
	 * when the client's copy of the map differs or is absent. */
	char		mod_name[128];	// gamedir this server is running
	char		skybox[64];	// worldspawn sky/skyname/qlsky key, "" if none
	byte		cd_track;	// cd track number

	char		startspot[64];
	char		modelname[MAX_QPATH];	// maps/<name>.bsp, for model_precache[0]
	struct qmodel_s	*worldmodel;
	const char	*model_precache[MAX_MODELS];	// NULL terminated
	struct qmodel_s	*models[MAX_MODELS];
	const char	*sound_precache[MAX_SOUNDS];	// NULL terminated
	const char	*lightstyles[MAX_LIGHTSTYLES];
	struct EffectT	Effects[MAX_EFFECTS];

	client_state2_t	*states;
	int		num_edicts;
	edict_t		*edicts;	// can NOT be array indexed, because
					// edict_t is variable sized, but can
					// be used to reference the world ent

	server_state_t	state;		// some actions are only valid during load

	sizebuf_t	datagram;
	byte		datagram_buf[NET_MAXMESSAGE];

	sizebuf_t	reliable_datagram;	// copied to all clients at end of frame
	byte		reliable_datagram_buf[NET_MAXMESSAGE];

	/* Signon data: entity baselines, static entities, static sounds, and
	 * anything QC writes to MSG_INIT.  A list of buffers rather than one,
	 * see MAX_SIGNON_SIZE above.  num_signon_buffers counts the COMPLETED
	 * ones; the buffer being filled is signon_bufs[num_signon_buffers] and
	 * its length lives in sv.signon.cursize, not in signon_size[], because
	 * QC can still write to MSG_INIT after the map has spawned. */
	sizebuf_t	signon;
	int		num_signon_buffers;
	int		signon_size[MAX_SIGNON_BUFFERS];
	byte		signon_bufs[MAX_SIGNON_BUFFERS][MAX_SIGNON_SIZE];
	ex_item_t	*ex_items;
	int			next_page_id;
	ex_inventory_page_t	*ex_inventory_pages;
	int			num_ex_items;
} server_t;


#define	NUM_PING_TIMES		16
#define	NUM_SPAWN_PARMS		16

typedef struct client_s
{
	qboolean	active;		// false = client is free
	qboolean	spawned;	// false = don't send datagrams
	qboolean	dropasap;	// has been told to go to another level
	qboolean	sendsignon;	// only valid before spawned
	int		signon_buffer;	// next signon buffer owed to this client,
					// -1 when none is pending.  The list goes
					// out one per reliable message; see
					// SV_SendSignonBuffer.

	double		last_message;	// reliable messages must be sent
					// periodically

	struct qsocket_s *netconnection; // communications handle

	usercmd_t	cmd;		// movement
	vec3_t		wishdir;	// intended motion calced from cmd

	sizebuf_t	message;	// can be added to at any time,
					// copied and clear once per frame
	byte		msgbuf[MAX_MSGLEN];

	sizebuf_t	datagram;
	byte		datagram_buf[NET_MAXMESSAGE];

	edict_t		*edict;		// EDICT_NUM(clientnum+1)
	char		name[32];	// for printing to other people
	int		colors;
	float		playerclass;

	float		ping_times[NUM_PING_TIMES];
	int		num_pings;	// ping_times[num_pings%NUM_PING_TIMES]

	// spawn parms are carried from level to level
	float		spawn_parms[NUM_SPAWN_PARMS];

	// client known data for deltas
	int		old_frags;
	entvars_t	old_v;
	qboolean	send_all_v;

	byte		current_frame, last_frame;
	byte		current_sequence, last_sequence;

// mission pack, objectives strings
	unsigned int	info_mask, info_mask2;
	ex_inventory_page_t *ex_inventory;
} client_t;


//=============================================================================

//
// edict->movetype values
//
#define	MOVETYPE_NONE		0		// never moves
#define	MOVETYPE_ANGLENOCLIP	1
#define	MOVETYPE_ANGLECLIP	2
#define	MOVETYPE_WALK		3		// gravity
#define	MOVETYPE_STEP		4		// gravity, special edge handling
#define	MOVETYPE_FLY		5
#define	MOVETYPE_TOSS		6		// gravity
#define	MOVETYPE_PUSH		7		// no clip to world, push and crush
#define	MOVETYPE_NOCLIP		8
#define	MOVETYPE_FLYMISSILE	9		// extra size to monsters
#define	MOVETYPE_BOUNCE		10
//#ifdef QUAKE2
#define	MOVETYPE_BOUNCEMISSILE	11		// bounce w/o gravity
#define	MOVETYPE_FOLLOW		12		// track movement of aiment
//#endif
#define	MOVETYPE_PUSHPULL	13		// pushable/pullable object
#define	MOVETYPE_SWIM		14		// should keep the object in water

//
// edict->solid values
//
#define	SOLID_NOT		0		// no interaction with other objects
#define	SOLID_TRIGGER		1		// touch on edge, but not blocking
#define	SOLID_BBOX		2		// touch on edge, block
#define	SOLID_SLIDEBOX		3		// touch on edge, but not an onground
#define	SOLID_BSP		4		// bsp clip, touch on edge, block
#define	SOLID_PHASE		5		// won't slow down when hitting entities flagged as FL_MONSTER
#define	SOLID_GHOST		6		// non-solid except to projectiles with .ghost_damage set

//
// edict->deadflag values
//
#define	DEAD_NO			0
#define	DEAD_DYING		1
#define	DEAD_DEAD		2

#define	DAMAGE_NO		0		// Cannot be damaged
#define	DAMAGE_YES		1		// Can be damaged
#define	DAMAGE_NO_GRENADE	2		// Will not set off grenades

//
// edict->flags
//
#define	FL_FLY			1
#define	FL_SWIM			2
#define	FL_CONVEYOR		4
#define	FL_CLIENT		8
#define	FL_INWATER		16
#define	FL_MONSTER		32
#define	FL_GODMODE		64
#define	FL_NOTARGET		128
#define	FL_ITEM			256
#define	FL_ONGROUND		512
#define	FL_PARTIALGROUND	1024	// not all corners are valid
#define	FL_WATERJUMP		2048	// player jumping out of water
#define	FL_JUMPRELEASED		4096	// for jump debouncing
#define	FL_FLASHLIGHT		8192
#define	FL_ARCHIVE_OVERRIDE	1048576
#define	FL_ARTIFACTUSED		16384
#define	FL_MOVECHAIN_ANGLE	32768	// when in a move chain, will update the angle
/* the following three are for monster_pentacles of the mission pack */
#define	FL_HUNTFACE		65536	// Makes monster go for enemy view_ofs thwn moving
#define	FL_NOZ			131072	// Monster will not automove on Z if flying or swimming
#define	FL_SET_TRACE		262144	// Trace will always be set for this monster (pentacles)

#define	FL_CLASS_DEPENDENT	2097152	// model will appear different to each player
#define	FL_SPECIAL_ABILITY1	4194304	// has 1st special ability
#define	FL_SPECIAL_ABILITY2	8388608	// has 2nd special ability

#define	FL2_CROUCHED		4096

//
// Built-in Spawn Flags
//
#define	SPAWNFLAG_NOT_PALADIN		0x00000100
#define	SPAWNFLAG_NOT_CLERIC		0x00000200
#define	SPAWNFLAG_NOT_NECROMANCER	0x00000400
#define	SPAWNFLAG_NOT_THEIF		0x00000800
#define	SPAWNFLAG_NOT_EASY		0x00001000
#define	SPAWNFLAG_NOT_MEDIUM		0x00002000
#define	SPAWNFLAG_NOT_HARD		0x00004000
#define	SPAWNFLAG_NOT_DEATHMATCH	0x00008000
#define	SPAWNFLAG_NOT_COOP		0x00010000
#define	SPAWNFLAG_NOT_SINGLE		0x00020000
/* SPAWNFLAG_NOT_DEMON is NOT used: ED_LoadFromFile
 * checks SPAWNFLAG_NOT_NECROMANCER for CLASS_DEMON !! */
#define	SPAWNFLAG_NOT_DEMON		0x00040000

//
// server flags
//
#define	SFL_EPISODE_1		1
#define	SFL_EPISODE_2		2
#define	SFL_EPISODE_3		4
#define	SFL_EPISODE_4		8
#define	SFL_NEW_UNIT		16
#define	SFL_NEW_EPISODE		32
#define	SFL_CROSS_TRIGGERS	65280

//============================================================================

extern	cvar_t	teamplay;
extern	cvar_t	skill;
extern	cvar_t	startmap;
extern	cvar_t	deathmatch;
extern	cvar_t	coop;
extern	cvar_t	randomclass;
extern	cvar_t	fraglimit;
extern	cvar_t	timelimit;

extern	server_static_t	svs;		// persistant server info
extern	int		sv_max_edicts;	// max_edicts, clamped and latched at map load
extern	server_t	sv;		// local server

extern	int		sv_protocol;	// protocol version to use

extern	client_t	*host_client;

extern	edict_t		*sv_player;

//===========================================================

void SV_Init (void);
void SV_UserInit (void);

/* How many sound indices the negotiated protocol can actually put on the
 * wire.  svc_sound sends the index as a byte plus the SND_OVERFLOW /
 * SND_OVERFLOW2 mask bits, so the reachable range is a property of the
 * protocol, not of the MAX_SOUNDS array bound.  Precache paths must clamp
 * to this, otherwise a mod fills slots the client can never be told about
 * and the sounds simply go missing at runtime. */
int SV_MaxSounds (void);

void SV_StartParticle (vec3_t org, vec3_t dir, int color, int count);
void SV_StartParticle2 (vec3_t org, vec3_t dmin, vec3_t dmax, int color, int effect, int count);
void SV_StartParticle3 (vec3_t org, vec3_t box, int color, int effect, int count);
void SV_StartParticle4 (vec3_t org, float radius, int color, int effect, int count);
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation);
void SV_StopSound (edict_t *entity, int channel);
void SV_UpdateSoundPos (edict_t *entity, int channel);
ex_inventory_page_t *SV_ClientInventoryPage (int clientnum);
int INV_UpdateExItem(ex_inventory_page_t *startPage, int inv_id, int inv_cnt, qboolean inc);

void SV_DropClient (qboolean crash);

void SV_Edicts (const char *Name);

void SV_SendClientMessages (void);
/* Call before writing a signon record, with the record's size in bytes, so it
 * cannot end up split across two signon buffers.  See MAX_SIGNON_SIZE. */
void SV_ReserveSignonSpace (int length);
void SV_SendSignonBuffer (struct client_s *client);
void SV_ClearDatagram (void);

int SV_ModelIndex (const char *name);

void SV_SetIdealPitch (void);

void SV_AddUpdates (void);

void SV_ClientThink (void);
void SV_AddClientToServer (struct qsocket_s	*ret);

void SV_ClientPrintf (unsigned int unused, const char *fmt, ...) FUNC_PRINTF(2,3);
void SV_BroadcastPrintf (const char *fmt, ...) FUNC_PRINTF(1,2);

void SV_Physics (void);

qboolean SV_CheckBottom (edict_t *ent);
qboolean SV_movestep (edict_t *ent, vec3_t move, qboolean relink, qboolean noenemy,
					  qboolean set_trace);

void SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg);

void SV_MoveToGoal (void);

void SV_CheckForNewClients (void);
void SV_RunClients (void);
void SV_SaveSpawnparms (void);
void SV_SpawnServer (const char *server, const char *startspot);

const char *SV_GetLevelname (void);

void SV_ParseEffect (sizebuf_t *sb);
void SV_UpdateEffects (sizebuf_t *sb);
void SV_SaveEffects (FILE *FH);
void SV_LoadEffects (FILE *FH);

/* Inventory save/load functions */
void INV_WritePage(FILE *f, struct ex_inventory_page_s *page, int clientId);
void INV_SavePages(FILE *FH);
void SV_LoadInventory(FILE *FH);

#endif	/* __HX2_SERVER_H */
