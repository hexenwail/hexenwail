/*
 * HexenWorld's QuakeWorld-derived connection transport inside Hexenwail.
 *
 * This intentionally owns no renderer or platform loop.  It is driven by the
 * normal Hexenwail client frame and is the first layer of the protocol mode.
 */
#include "quakedef.h"
#include "cl_hw.h"
#include "../hexenworld/shared/net.h"
#include "../hexenworld/shared/huffman.h"

/* quakedef.h already includes Hexen II's effects.h.  HexenWorld uses the
 * same names for a different numeric table, so keep the values local to this
 * protocol reader instead of letting the two headers collide. */
#undef CE_ACID_MUZZFL
#define CE_ACID_MUZZFL 58
#undef CE_FLAMESTREAM
#define CE_FLAMESTREAM 74
#undef CE_BLDRN_EXPL
#define CE_BLDRN_EXPL 57
#undef CE_ACID_HIT
#define CE_ACID_HIT 59
#undef CE_FIREWALL_SMALL
#define CE_FIREWALL_SMALL 60
#undef CE_FIREWALL_MEDIUM
#define CE_FIREWALL_MEDIUM 61
#undef CE_FIREWALL_LARGE
#define CE_FIREWALL_LARGE 62
#undef CE_LBALL_EXPL
#define CE_LBALL_EXPL 63
#undef CE_ACID_SPLAT
#define CE_ACID_SPLAT 64
#undef CE_ACID_EXPL
#define CE_ACID_EXPL 65
#undef CE_FBOOM
#define CE_FBOOM 66
#undef CE_BOMB
#define CE_BOMB 67
#undef CE_BRN_BOUNCE
#define CE_BRN_BOUNCE 68
#undef CE_LSHOCK
#define CE_LSHOCK 69
#undef CE_FLAMEWALL
#define CE_FLAMEWALL 70
#undef CE_FLAMEWALL2
#define CE_FLAMEWALL2 71
#undef CE_ONFIRE
#define CE_ONFIRE 73
#define CE_RIPPLE 56
#define CE_SM_EXPLOSION2 50
#define CE_HWMISSILESTAR 42
#define CE_HWEIDOLONSTAR 43
#define CE_HWSHEEPINATOR 44
#define CE_TRIPMINE 45
#define CE_HWBONEBALL 46
#define CE_HWRAVENSTAFF 47
#define CE_TRIPMINESTILL 48
#define CE_SCARABCHAIN 49
#define CE_HWSPLITFLASH 51
#define CE_HWXBOWSHOOT 52
#define CE_HWRAVENPOWER 53
#define CE_HWDRILLA 54
#define CE_DEATHBUBBLES 55

#define HW_PORT_CLIENT 26901
#define HW_PORT_SERVER 26950
#define HW_S2C_CONNECTION 'j'
#define HW_A2C_PRINT 'n'
#define HW_CLC_MOVE 3
#define HW_CLC_STRINGCMD 4

/* Keep this protocol namespace separate from Hexen II's protocol.h, which is
 * already included through quakedef.h. */
#define HW_OLD_PROTOCOL_VERSION 24
#define HW_PROTOCOL_VERSION 25
#define HW_PROTOCOL_VERSION_EXT 26
#define HW_PROTOCOL_VERSION_HEXENWAIL_1 100
#define HW_SVC_NOP 1
#define HW_SVC_DISCONNECT 2
#define HW_SVC_UPDATESTAT 3
#define HW_SVC_SETVIEW 5
#define HW_SVC_SOUND 6
#define HW_SVC_TIME 7
#define HW_SVC_PRINT 8
#define HW_SVC_STUFFTEXT 9
#define HW_SVC_SETANGLE 10
#define HW_SVC_SERVERDATA 11
#define HW_SVC_LIGHTSTYLE 12
#define HW_SVC_UPDATEFRAGS 14
#define HW_SVC_STOPSOUND 16
#define HW_SVC_PARTICLE 18
#define HW_SVC_DAMAGE 19
#define HW_SVC_SPAWNBASELINE 22
#define HW_SVC_CENTERPRINT 26
#define HW_SVC_KILLEDMONSTER 27
#define HW_SVC_FOUNDSECRET 28
#define HW_SVC_SPAWNSTATICSOUND 29
#define HW_SVC_INTERMISSION 30
#define HW_SVC_FINALE 31
#define HW_SVC_CDTRACK 32
#define HW_SVC_SELLSCREEN 33
#define HW_SVC_SMALLKICK 34
#define HW_SVC_BIGKICK 35
#define HW_SVC_UPDATEPING 36
#define HW_SVC_UPDATEENTERTIME 37
#define HW_SVC_UPDATESTATLONG 38
#define HW_SVC_MUZZLEFLASH 39
#define HW_SVC_UPDATEUSERINFO 40
#define HW_SVC_DOWNLOAD 41
#define HW_SVC_PLAYERINFO 42
#define HW_SVC_NAILS 43
#define HW_SVC_CHOKECOUNT 44
#define HW_SVC_MODELLIST 45
#define HW_SVC_SOUNDLIST 46
#define HW_SVC_PACKETENTITIES 47
#define HW_SVC_DELTAPACKETENTITIES 48
#define HW_SVC_MAXSPEED 49
#define HW_SVC_ENTGRAVITY 50
#define HW_SVC_SET_VIEW_TINT 53
#define HW_SVC_START_EFFECT 54
#define HW_SVC_END_EFFECT 55
#define HW_SVC_SET_VIEW_FLAGS 56
#define HW_SVC_CLEAR_VIEW_FLAGS 57
#define HW_SVC_UPDATE_INV 58
#define HW_SVC_PARTICLE2 59
#define HW_SVC_PARTICLE3 60
#define HW_SVC_PARTICLE4 61
#define HW_SVC_TURN_EFFECT 62
#define HW_SVC_UPDATE_EFFECT 63
#define HW_SVC_MULTIEFFECT 64
#define HW_SVC_MIDI_NAME 65
#define HW_SVC_RAINEFFECT 66
#define HW_SVC_PACKMISSILE 67
#define HW_SVC_INDEXED_PRINT 68
#define HW_SVC_TARGETUPDATE 69
#define HW_SVC_NAME_PRINT 70
#define HW_SVC_SOUND_UPDATE_POS 71
#define HW_SVC_UPDATE_PIV 72
#define HW_SVC_PLAYER_SOUND 73
#define HW_SVC_UPDATEPCLASS 74
#define HW_SVC_UPDATEDMINFO 75
#define HW_SVC_UPDATESIEGEINFO 76
#define HW_SVC_UPDATESIEGETEAM 77
#define HW_SVC_UPDATESIEGELOSSES 78
#define HW_SVC_HASKEY 79
#define HW_SVC_NONEHASKEY 80
#define HW_SVC_ISDOC 81
#define HW_SVC_NODOC 82
#define HW_SVC_PLAYERSKIPPED 83
#define HW_SND_VOLUME (1 << 15)
#define HW_SND_ATTENUATION (1 << 14)
#define HWCL_MAX_ENTITIES 768
#define HWCL_MAX_CLIENTS 32

typedef enum
{
	hwcl_disconnected,
	hwcl_connecting,
	hwcl_connected
} hwcl_state_t;

static hwcl_state_t hwcl_state;
static qboolean hwcl_net_initialized;
static netadr_t hwcl_server;
static netchan_t hwcl_netchan;
static double hwcl_connect_time;
static qboolean hwcl_received_packet;
static int hwcl_protocol;
static int hwcl_servercount;
static int hwcl_entity_sequence = -1;

/* This is deliberately protocol state rather than cl: Hexen II's cl is tied
 * to its qsocket session and renderer.  Keeping the values here lets the
 * eventual adapter map them deliberately instead of corrupting a concurrent
 * Hexen II connection. */
typedef struct
{
	qboolean active;
	int modelindex;
	int frame;
	int colormap;
	int skinnum;
	int weaponframe;
	int scale;
	int drawflags;
	int abslight;
	int alpha;
	int effects;
	vec3_t origin;
	vec3_t angles;
	vec3_t velocity;
} hwcl_entity_state_t;

static struct
{
	double server_time;
	vec3_t viewangles;
	int viewentity;
	int stats[MAX_CL_STATS];
	int cdtrack;
	int chokecount;
	int piv;
	int playerclass[HWCL_MAX_CLIENTS];
	int playerlevel[HWCL_MAX_CLIENTS];
	float maxspeed;
	float entgravity;
	char midi_name[MAX_QPATH];
	hwcl_entity_state_t baselines[HWCL_MAX_ENTITIES];
	hwcl_entity_state_t entities[HWCL_MAX_ENTITIES];
	hwcl_entity_state_t players[HWCL_MAX_CLIENTS];
} hwcl_server_state;

typedef struct
{
	vec3_t angles;
	int forwardmove;
	int sidemove;
	int upmove;
	int buttons;
	int impulse;
	int msec;
} hwcl_usercmd_t;

static hwcl_usercmd_t hwcl_cmd_history[3];
static qboolean hwcl_have_cmd_history;
static char hwcl_model_names[MAX_MODELS][MAX_QPATH];
static char hwcl_sound_names[MAX_SOUNDS][MAX_QPATH];
static int hwcl_model_count;
static int hwcl_sound_count;
static int hwcl_playernum;

extern qmodel_t *player_models[MAX_PLAYER_CLASS];

static void HWCL_StringCmd (const char *command)
{
	MSG_WriteByte (&hwcl_netchan.message, HW_CLC_STRINGCMD);
	MSG_WriteString (&hwcl_netchan.message, command);
}

static void HWCL_WriteAngle16 (sizebuf_t *buf, float angle)
{
	int value;

	if (angle >= 0)
		value = (int)(angle * (65536.0f / 360.0f) + 0.5f);
	else
		value = (int)(angle * (65536.0f / 360.0f) - 0.5f);
	MSG_WriteShort (buf, value & 65535);
}

static int HWCL_QuantizeMove (int move)
{
	if (move > 508)
		return 127;
	if (move < -512)
		return -128;
	return (int)(move * 0.25f);
}

static void HWCL_WriteUsercmd (sizebuf_t *buf, const hwcl_usercmd_t *cmd)
{
	int bits = 0;

	if (cmd->angles[0]) bits |= (1 << 0);
	if (cmd->angles[2]) bits |= (1 << 1);
	if (cmd->forwardmove) bits |= (1 << 2);
	if (cmd->sidemove) bits |= (1 << 3);
	if (cmd->upmove) bits |= (1 << 4);
	if (cmd->buttons) bits |= (1 << 5);
	if (cmd->impulse) bits |= (1 << 6);
	if (cmd->msec) bits |= (1 << 7);

	MSG_WriteByte (buf, bits);
	if (bits & (1 << 0)) HWCL_WriteAngle16 (buf, cmd->angles[0]);
	HWCL_WriteAngle16 (buf, cmd->angles[1]);
	if (bits & (1 << 1)) HWCL_WriteAngle16 (buf, cmd->angles[2]);
	if (bits & (1 << 2)) MSG_WriteChar (buf, HWCL_QuantizeMove (cmd->forwardmove));
	if (bits & (1 << 3)) MSG_WriteChar (buf, HWCL_QuantizeMove (cmd->sidemove));
	if (bits & (1 << 4)) MSG_WriteChar (buf, HWCL_QuantizeMove (cmd->upmove));
	if (bits & (1 << 5)) MSG_WriteByte (buf, cmd->buttons);
	if (bits & (1 << 6)) MSG_WriteByte (buf, cmd->impulse);
	if (bits & (1 << 7)) MSG_WriteByte (buf, cmd->msec);
}

static qboolean HWCL_ValidProtocol (int protocol)
{
	return protocol == HW_OLD_PROTOCOL_VERSION ||
		protocol == HW_PROTOCOL_VERSION ||
		protocol == HW_PROTOCOL_VERSION_EXT ||
		protocol == HW_PROTOCOL_VERSION_HEXENWAIL_1;
}

static void HWCL_LoadModels (void)
{
	static const char *player_names[MAX_PLAYER_CLASS] = {
		"models/paladin.mdl", "models/crusader.mdl", "models/necro.mdl",
		"models/assassin.mdl", "models/succubus.mdl"
	};
	qmodel_t *model;
	int i;

	memset (cl.model_precache, 0, sizeof(cl.model_precache));
	for (i = 1; i < hwcl_model_count; i++)
	{
		if (!hwcl_model_names[i][0])
			continue;
		model = Mod_ForName (hwcl_model_names[i], false);
		if (!model)
		{
			Con_DPrintf ("HexenWorld model %d unavailable: %s\n", i,
					hwcl_model_names[i]);
			continue;
		}
		cl.model_precache[i] = model;
	}

	for (i = 0; i < MAX_PLAYER_CLASS; i++)
		if (!player_models[i])
			player_models[i] = Mod_ForName (player_names[i], false);

	if (!cl.model_precache[1])
	{
		Con_Printf ("HexenWorld world model unavailable: %s\n",
				hwcl_model_names[1]);
		return;
	}

	cl.worldmodel = cl_entities[0].model = cl.model_precache[1];
	COM_FileBase (hwcl_model_names[1], cl.mapname, sizeof(cl.mapname));
	R_NewMap ();
}

static void HWCL_LoadSounds (void)
{
	int i;

	S_ClearPrecache ();
	memset (cl.sound_precache, 0, sizeof(cl.sound_precache));
	S_BeginPrecaching ();
	for (i = 1; i < hwcl_sound_count; i++)
	{
		if (hwcl_sound_names[i][0])
			cl.sound_precache[i] = S_PrecacheSound (hwcl_sound_names[i]);
	}
	S_EndPrecaching ();
	CL_PrecacheTEntSounds ();
}

/* Consume a chunked (protocol 26+) or classic precache list and retain its
 * indices.  The chunk command uses zero-based offsets, while wire indices
 * start at one. */
static void HWCL_ParsePrecacheList (qboolean models)
{
	int index = 1;
	int next;
	const char *entry;

	if (hwcl_protocol >= HW_PROTOCOL_VERSION_EXT)
		index = MSG_ReadLong () + 1;
	for (;;)
	{
		entry = MSG_ReadString ();
		if (!entry[0] || msg_badread)
			break;
		if (index > 0 && (models ? index < MAX_MODELS : index < MAX_SOUNDS))
		{
			if (models)
				q_strlcpy (hwcl_model_names[index], entry, MAX_QPATH);
			else
				q_strlcpy (hwcl_sound_names[index], entry, MAX_QPATH);
		}
		index++;
	}
	if (msg_badread)
		return;

	if (hwcl_protocol >= HW_PROTOCOL_VERSION_EXT)
	{
		next = MSG_ReadLong ();
		if (msg_badread)
			return;
		if (next)
		{
			HWCL_StringCmd (va ("%slist %d %d", models ? "model" : "sound",
					hwcl_servercount, next));
			return;
		}
	}

	if (models)
	{
		hwcl_model_count = index > hwcl_model_count ? index : hwcl_model_count;
		HWCL_LoadModels ();
		Con_Printf ("HexenWorld precache lists received; requesting signon.\n");
		HWCL_StringCmd (va ("prespawn %d 0", hwcl_servercount));
	}
	else
	{
		hwcl_sound_count = index > hwcl_sound_count ? index : hwcl_sound_count;
		HWCL_LoadSounds ();
		HWCL_StringCmd (va ("modellist %d 0", hwcl_servercount));
	}
}

static void HWCL_ParseServerData (void)
{
	int i;
	int playernum;
	char gamedir[MAX_QPATH];
	char levelname[1024];

	hwcl_protocol = MSG_ReadLong ();
	if (!HWCL_ValidProtocol (hwcl_protocol))
	{
		Con_Printf ("HexenWorld server returned unsupported protocol %d.\n",
				hwcl_protocol);
		HWCL_Disconnect ();
		return;
	}
	hwcl_servercount = MSG_ReadLong ();
	hwcl_entity_sequence = -1;
	memset (&hwcl_server_state, 0, sizeof(hwcl_server_state));
	q_strlcpy (gamedir, MSG_ReadString (), sizeof(gamedir));
	playernum = MSG_ReadByte ();
	q_strlcpy (levelname, MSG_ReadString (), sizeof(levelname));
	if (hwcl_protocol >= HW_PROTOCOL_VERSION)
		for (i = 0; i < 10; i++)
		{
			float movevar = MSG_ReadFloat ();
			if (i == 2)
				hwcl_server_state.maxspeed = movevar;
			if (i == 9)
				hwcl_server_state.entgravity = movevar;
		}
	if (msg_badread)
		return;

	CL_ClearState ();
	cls.signon = 0;
	memset (hwcl_model_names, 0, sizeof(hwcl_model_names));
	memset (hwcl_sound_names, 0, sizeof(hwcl_sound_names));
	hwcl_model_count = 1;
	hwcl_sound_count = 1;
	hwcl_playernum = playernum & 127;
	q_strlcpy (cl.levelname, levelname, sizeof(cl.levelname));
	q_strlcpy (cl.mod_name, gamedir, sizeof(cl.mod_name));
	cl.maxclients = HWCL_MAX_CLIENTS;
	cl.scores = (scoreboard_t *)Hunk_AllocName (
			cl.maxclients * sizeof(*cl.scores), "hw_scores");
	memset (cl.scores, 0, cl.maxclients * sizeof(*cl.scores));
	cl.gametype = GAME_DEATHMATCH;
	cl.viewentity = hwcl_playernum + 1;
	hwcl_server_state.viewentity = cl.viewentity;
	if (hwcl_playernum >= 0 && hwcl_playernum < HWCL_MAX_CLIENTS)
		hwcl_server_state.playerclass[hwcl_playernum] =
			(int)cl_playerclass.value;

	Con_Printf ("HexenWorld protocol %d, game %s, level %s (player %d%s).\n",
			hwcl_protocol, gamedir, levelname, playernum & 127,
			(playernum & 128) ? ", spectator" : "");
	HWCL_StringCmd (va ("soundlist %d 0", hwcl_servercount));
}

/* Signon buffers contain baseline records.  Keep them because a full packet
 * delta-compresses each entity from its baseline. */
static void HWCL_ParseBaseline (void)
{
	hwcl_entity_state_t *state;
	int entitynum;
	int i;

	entitynum = MSG_ReadShort ();
	if (entitynum < 0 || entitynum >= HWCL_MAX_ENTITIES)
	{
		/* Consume the record without indexing outside our protocol state. */
		MSG_ReadShort ();
		for (i = 0; i < 6; i++) MSG_ReadByte ();
		for (i = 0; i < 3; i++) { MSG_ReadCoord (); MSG_ReadAngle (); }
		return;
	}
	state = &hwcl_server_state.baselines[entitynum];
	memset (state, 0, sizeof(*state));
	state->active = true;
	state->modelindex = MSG_ReadShort ();
	state->frame = MSG_ReadByte ();
	state->colormap = MSG_ReadByte ();
	state->skinnum = MSG_ReadByte ();
	state->scale = MSG_ReadByte ();
	state->drawflags = MSG_ReadByte ();
	state->abslight = MSG_ReadByte ();
	for (i = 0; i < 3; i++)
	{
		state->origin[i] = MSG_ReadCoord ();
		state->angles[i] = MSG_ReadAngle ();
	}
}

static void HWCL_ParseEntityDelta (const hwcl_entity_state_t *from,
		hwcl_entity_state_t *to, int bits)
{
	bits &= ~511; /* low nine bits are the entity number */
	*to = *from;
	if (bits & (1 << 15)) bits |= MSG_ReadByte ();
	if (bits & (1 << 7)) bits |= MSG_ReadByte () << 16;
	if (bits & (1 << 16))
		to->modelindex = (bits & (1 << 6)) ? MSG_ReadShort () : MSG_ReadByte ();
	if (bits & (1 << 13)) to->frame = MSG_ReadByte ();
	if (bits & (1 << 3)) to->colormap = MSG_ReadByte ();
	if (bits & (1 << 4)) to->skinnum = MSG_ReadByte ();
	if (bits & (1 << 18)) to->drawflags = MSG_ReadByte ();
	if (bits & (1 << 5)) to->effects = MSG_ReadLong ();
	if (bits & (1 << 9)) to->origin[0] = MSG_ReadCoord ();
	if (bits & (1 << 0)) to->angles[0] = MSG_ReadAngle ();
	if (bits & (1 << 10)) to->origin[1] = MSG_ReadCoord ();
	if (bits & (1 << 12)) to->angles[1] = MSG_ReadAngle ();
	if (bits & (1 << 11)) to->origin[2] = MSG_ReadCoord ();
	if (bits & (1 << 1)) to->angles[2] = MSG_ReadAngle ();
	if (bits & (1 << 2)) to->scale = MSG_ReadByte ();
	if (bits & (1 << 19)) to->abslight = MSG_ReadByte ();
	if (bits & (1 << 17)) MSG_ReadShort (); /* sound index, routed later */
	if (bits & (1 << 20)) to->alpha = MSG_ReadByte (); /* protocol 100 */
	to->active = true;
}

static void HWCL_ParsePacketEntities (qboolean delta)
{
	hwcl_entity_state_t discard = {0};
	int bits;
	int entitynum;
	int source = -1;
	qboolean apply;

	if (delta)
	{
		source = MSG_ReadByte ();
		apply = hwcl_entity_sequence >= 0 &&
			source == (hwcl_entity_sequence & 255);
	}
	else
	{
		apply = true;
		memset (hwcl_server_state.entities, 0, sizeof(hwcl_server_state.entities));
	}
	for (;;)
	{
		bits = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
			return;
		if (!bits)
			break;
		entitynum = bits & 511;
		if (entitynum >= HWCL_MAX_ENTITIES)
		{
			if (!(bits & (1 << 14)))
				HWCL_ParseEntityDelta (&discard, &discard, bits);
			continue;
		}
		if (bits & (1 << 14))
		{
			if (apply)
				hwcl_server_state.entities[entitynum].active = false;
			continue;
		}
		if (apply)
			HWCL_ParseEntityDelta (delta ? &hwcl_server_state.entities[entitynum] :
					&hwcl_server_state.baselines[entitynum],
					&hwcl_server_state.entities[entitynum], bits);
		else
			HWCL_ParseEntityDelta (&discard, &discard, bits);
		if (msg_badread)
			return;
	}
	if (apply)
		hwcl_entity_sequence = hwcl_netchan.incoming_sequence;
}

static void HWCL_SkipUsercmd (void)
{
	int bits = MSG_ReadByte ();

	if (bits & (1 << 0)) MSG_ReadShort ();
	MSG_ReadShort (); /* angle 2 is always present */
	if (bits & (1 << 1)) MSG_ReadShort ();
	if (bits & (1 << 2)) MSG_ReadChar ();
	if (bits & (1 << 3)) MSG_ReadChar ();
	if (bits & (1 << 4)) MSG_ReadChar ();
	if (bits & (1 << 5)) MSG_ReadByte ();
	if (bits & (1 << 6)) MSG_ReadByte ();
	if (bits & (1 << 7)) MSG_ReadByte ();
}

static void HWCL_ParseSound (void)
{
	int channel;
	int sound_num;
	int volume = 255;
	float attenuation = 1.0f;
	vec3_t origin;
	int i;

	channel = MSG_ReadShort ();
	if (channel & HW_SND_VOLUME)
		volume = MSG_ReadByte ();
	if (channel & HW_SND_ATTENUATION)
		attenuation = MSG_ReadByte () / 32.0f;
	sound_num = MSG_ReadByte ();
	for (i = 0; i < 3; i++)
		origin[i] = MSG_ReadCoord ();
	if (!msg_badread && sound_num > 0 && sound_num < MAX_SOUNDS &&
			cl.sound_precache[sound_num])
		S_StartSound (channel >> 3, channel & 7, cl.sound_precache[sound_num],
				origin, volume / 255.0f, attenuation);
}

static void HWCL_ParseStaticSound (void)
{
	vec3_t origin;
	int sound_num;
	int volume;
	int attenuation;

	origin[0] = MSG_ReadCoord ();
	origin[1] = MSG_ReadCoord ();
	origin[2] = MSG_ReadCoord ();
	sound_num = MSG_ReadByte ();
	volume = MSG_ReadByte ();
	attenuation = MSG_ReadByte ();
	if (!msg_badread && sound_num > 0 && sound_num < MAX_SOUNDS &&
			cl.sound_precache[sound_num])
		S_StaticSound (cl.sound_precache[sound_num], origin, volume / 255.0f,
				attenuation / 32.0f);
}

static void HWCL_ParseDownload (void)
{
	int size;
	int i;

	size = MSG_ReadShort ();
	MSG_ReadByte (); /* percent */
	if (size <= 0 || msg_badread)
		return;
	for (i = 0; i < size; i++)
		MSG_ReadByte ();
}

static void HWCL_SkipCoords (int count)
{
	int i;

	for (i = 0; i < count; i++)
		MSG_ReadCoord ();
}

static void HWCL_ReadCoords (vec3_t coords)
{
	int i;

	for (i = 0; i < 3; i++)
		coords[i] = MSG_ReadCoord ();
}

static void HWCL_SkipAngles (int count)
{
	int i;

	for (i = 0; i < count; i++)
		MSG_ReadAngle ();
}

static void HWCL_SkipFloats (int count)
{
	int i;

	for (i = 0; i < count; i++)
		MSG_ReadFloat ();
}

static void HWCL_ParseParticle (void)
{
	HWCL_SkipCoords (3);
	MSG_ReadChar ();
	MSG_ReadChar ();
	MSG_ReadChar ();
	MSG_ReadByte (); /* count */
	MSG_ReadByte (); /* color */
}

static void HWCL_ParseParticle2 (void)
{
	HWCL_SkipCoords (3);
	HWCL_SkipFloats (6); /* dmin, dmax */
	MSG_ReadShort (); /* color */
	MSG_ReadByte (); /* count */
	MSG_ReadByte (); /* effect */
}

static void HWCL_ParseParticle3 (void)
{
	HWCL_SkipCoords (3);
	MSG_ReadByte (); /* box x */
	MSG_ReadByte (); /* box y */
	MSG_ReadByte (); /* box z */
	MSG_ReadShort (); /* color */
	MSG_ReadByte (); /* count */
	MSG_ReadByte (); /* effect */
}

static void HWCL_ParseParticle4 (void)
{
	HWCL_SkipCoords (3);
	MSG_ReadByte (); /* radius */
	MSG_ReadShort (); /* color */
	MSG_ReadByte (); /* count */
	MSG_ReadByte (); /* effect */
}

static void HWCL_ParseRainEffect (void)
{
	HWCL_SkipCoords (6); /* origin, size */
	HWCL_SkipAngles (2); /* x/y direction */
	MSG_ReadShort (); /* color */
	MSG_ReadShort (); /* count */
}

static void HWCL_ParsePackedMissiles (void)
{
	int count;
	int i;

	count = MSG_ReadByte ();
	for (i = 0; i < count * 5; i++)
		MSG_ReadByte ();
}

static void HWCL_ParseNails (void)
{
	int count;
	int i;

	count = MSG_ReadByte ();
	for (i = 0; i < count * 6; i++)
		MSG_ReadByte ();
}

static const char *HWCL_InfoValue (const char *info, const char *key)
{
	static char value[512];
	char current[512];
	const char *p = info;
	int i;

	if (*p == '\\')
		p++;
	while (*p)
	{
		i = 0;
		while (*p && *p != '\\' && i < (int)sizeof(current) - 1)
			current[i++] = *p++;
		current[i] = 0;
		if (*p == '\\')
			p++;
		i = 0;
		while (*p && *p != '\\' && i < (int)sizeof(value) - 1)
			value[i++] = *p++;
		value[i] = 0;
		if (!strcmp (current, key))
			return value;
		if (*p == '\\')
			p++;
	}
	return "";
}

static void HWCL_ParseDamage (void)
{
	V_ParseDamage ();
}

static void HWCL_SkipXbowBolts (int turned)
{
	int i;

	for (i = 0; i < 5; i++)
	{
		if (turned & (1 << i))
		{
			HWCL_SkipCoords (3);
			HWCL_SkipAngles (2);
		}
	}
}

static qboolean HWCL_ParseEffectPayload (int type)
{
	switch (type)
	{
	case CE_RAIN:
		HWCL_SkipCoords (12);
		MSG_ReadShort ();
		MSG_ReadShort ();
		MSG_ReadFloat ();
		break;
	case CE_FOUNTAIN:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (3);
		HWCL_SkipCoords (3);
		MSG_ReadShort ();
		MSG_ReadByte ();
		break;
	case CE_QUAKE:
		HWCL_SkipCoords (3);
		MSG_ReadFloat ();
		break;
	case CE_WHITE_SMOKE:
	case CE_GREEN_SMOKE:
	case CE_GREY_SMOKE:
	case CE_RED_SMOKE:
	case CE_SLOW_WHITE_SMOKE:
	case CE_TELESMK1:
	case CE_TELESMK2:
	case CE_GHOST:
	case CE_REDCLOUD:
	case CE_FLAMESTREAM:
	case CE_ACID_MUZZFL:
	case CE_FLAMEWALL:
	case CE_FLAMEWALL2:
	case CE_ONFIRE:
	case CE_RIPPLE:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (4);
		break;
	case CE_SM_WHITE_FLASH:
	case CE_YELLOWRED_FLASH:
	case CE_BLUESPARK:
	case CE_YELLOWSPARK:
	case CE_SM_CIRCLE_EXP:
	case CE_BG_CIRCLE_EXP:
	case CE_SM_EXPLOSION:
	case CE_SM_EXPLOSION2:
	case CE_LG_EXPLOSION:
	case CE_FLOOR_EXPLOSION:
	case CE_BLUE_EXPLOSION:
	case CE_REDSPARK:
	case CE_GREENSPARK:
	case CE_ICEHIT:
	case CE_MEDUSA_HIT:
	case CE_MEZZO_REFLECT:
	case CE_FLOOR_EXPLOSION2:
	case CE_XBOW_EXPLOSION:
	case CE_NEW_EXPLOSION:
	case CE_MAGIC_MISSILE_EXPLOSION:
	case CE_BONE_EXPLOSION:
	case CE_BLDRN_EXPL:
	case CE_ACID_HIT:
	case CE_ACID_SPLAT:
	case CE_ACID_EXPL:
	case CE_LBALL_EXPL:
	case CE_FIREWALL_SMALL:
	case CE_FIREWALL_MEDIUM:
	case CE_FIREWALL_LARGE:
	case CE_FBOOM:
	case CE_BOMB:
	case CE_BRN_BOUNCE:
	case CE_LSHOCK:
		HWCL_SkipCoords (3);
		break;
	case CE_WHITE_FLASH:
	case CE_BLUE_FLASH:
	case CE_SM_BLUE_FLASH:
	case CE_HWSPLITFLASH:
	case CE_RED_FLASH:
	case CE_RIDER_DEATH:
	case CE_TELEPORTERPUFFS:
		HWCL_SkipCoords (3);
		break;
	case CE_TELEPORTERBODY:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (4);
		break;
	case CE_BONESHRAPNEL:
	case CE_HWBONEBALL:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (9);
		break;
	case CE_BONESHARD:
	case CE_HWRAVENSTAFF:
	case CE_HWMISSILESTAR:
	case CE_HWEIDOLONSTAR:
	case CE_HWRAVENPOWER:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (3);
		break;
	case CE_HWDRILLA:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		MSG_ReadShort ();
		break;
	case CE_DEATHBUBBLES:
		MSG_ReadShort ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		break;
	case CE_SCARABCHAIN:
		HWCL_SkipCoords (3);
		MSG_ReadShort ();
		MSG_ReadByte ();
		break;
	case CE_TRIPMINESTILL:
	case CE_TRIPMINE:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (3);
		break;
	case CE_HWSHEEPINATOR:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		{
			int turned = MSG_ReadByte ();
			MSG_ReadByte ();
			HWCL_SkipXbowBolts (turned);
		}
		break;
	case CE_HWXBOWSHOOT:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		MSG_ReadByte ();
		MSG_ReadByte ();
		{
			int turned = MSG_ReadByte ();
			MSG_ReadByte ();
			HWCL_SkipXbowBolts (turned);
		}
		break;
	default:
		return false;
	}
	return !msg_badread;
}

static qboolean HWCL_ParseStartEffect (void)
{
	int idx = MSG_ReadByte ();
	int type = MSG_ReadByte ();

	(void)idx;
	return HWCL_ParseEffectPayload (type);
}

static qboolean HWCL_ParseUpdateEffect (void)
{
	int idx = MSG_ReadByte ();
	int type = MSG_ReadByte ();
	int command;

	(void)idx;
	switch (type)
	{
	case CE_SCARABCHAIN:
		MSG_ReadShort ();
		break;
	case CE_HWSHEEPINATOR:
	case CE_HWXBOWSHOOT:
		command = MSG_ReadByte ();
		if (command & 1)
			MSG_ReadCoord ();
		else
		{
			MSG_ReadAngle ();
			MSG_ReadAngle ();
			if (command & 128)
				HWCL_SkipCoords (3);
		}
		break;
	case CE_HWDRILLA:
		command = MSG_ReadByte ();
		if (!command)
		{
			HWCL_SkipCoords (3);
			MSG_ReadByte ();
		}
		else
		{
			MSG_ReadAngle ();
			MSG_ReadAngle ();
			HWCL_SkipCoords (3);
		}
		break;
	default:
		return false;
	}
	return !msg_badread;
}

static qboolean HWCL_ParseMultiEffect (void)
{
	int type = MSG_ReadByte ();
	int i;

	if (type != CE_HWRAVENPOWER)
		return false;
	HWCL_SkipCoords (6);
	for (i = 0; i < 3; i++)
		MSG_ReadByte ();
	return !msg_badread;
}

static qmodel_t *HWCL_ModelForEntity (const hwcl_entity_state_t *state,
		qboolean player)
{
	int playerclass;

	if (state->modelindex > 0 && state->modelindex < MAX_MODELS)
		return cl.model_precache[state->modelindex];
	if (!player)
		return NULL;

	playerclass = hwcl_server_state.playerclass[state - hwcl_server_state.players];
	if (playerclass < 1 || playerclass > MAX_PLAYER_CLASS)
		playerclass = 1;
	return player_models[playerclass - 1];
}

static void HWCL_CopyEntity (int entitynum, const hwcl_entity_state_t *state,
		qboolean player)
{
	entity_t *ent = &cl_entities[entitynum];
	qmodel_t *model;
	qboolean was_on;
	qboolean model_changed;

	model = HWCL_ModelForEntity (state, player);
	was_on = (ent->baseline.flags & BE_ON) != 0;
	model_changed = ent->model != model;
	if (model_changed && ent->efrag)
		R_RemoveEfrags (ent);
	if (!was_on || model_changed)
		ent->forcelink = true;

	ent->baseline.flags = BE_ON;
	ent->baseline.modelindex = state->modelindex;
	ent->baseline.frame = state->frame;
	ent->baseline.colormap = state->colormap;
	ent->baseline.skin = state->skinnum;
	ent->baseline.effects = state->effects;
	ent->baseline.scale = state->scale;
	ent->baseline.drawflags = state->drawflags;
	ent->baseline.abslight = state->abslight;
	ent->baseline.alpha = (byte)state->alpha;

	if (was_on)
	{
		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
	}
	VectorCopy (state->origin, ent->msg_origins[0]);
	VectorCopy (state->angles, ent->msg_angles[0]);
	ent->msgtime = cl.mtime[0];
	ent->model = model;
	ent->frame = state->frame;
	ent->colormap = vid.colormap;
	ent->sourcecolormap = vid.colormap;
	ent->skinnum = state->skinnum;
	ent->effects = state->effects;
	ent->scale = state->scale;
	ent->drawflags = state->drawflags;
	ent->abslight = state->abslight;
	ent->alpha = (byte)state->alpha;

	if (model_changed && model && cl.worldmodel)
		R_AddEfrags (ent);
}

static void HWCL_ClearEntity (int entitynum)
{
	entity_t *ent = &cl_entities[entitynum];

	if (ent->efrag)
		R_RemoveEfrags (ent);
	memset (ent, 0, sizeof(*ent));
}

void HWCL_ApplyState (void)
{
	const hwcl_entity_state_t *state;
	int i;
	int highest = 0;
	int viewentity;

	cl.mtime[1] = cl.mtime[0];
	cl.mtime[0] = hwcl_server_state.server_time;
	cl.time = cl.mtime[0];
	for (i = 0; i < MAX_CL_STATS; i++)
		cl.stats[i] = hwcl_server_state.stats[i];

	for (i = 1; i < HWCL_MAX_ENTITIES; i++)
	{
		if (i <= HWCL_MAX_CLIENTS)
			state = &hwcl_server_state.players[i - 1];
		else
			state = &hwcl_server_state.entities[i];

		if (state->active)
		{
			HWCL_CopyEntity (i, state, i <= HWCL_MAX_CLIENTS);
			highest = i;
		}
		else if (cl_entities[i].baseline.flags & BE_ON)
			HWCL_ClearEntity (i);
	}

	viewentity = hwcl_server_state.viewentity;
	if (viewentity < 1 || viewentity >= HWCL_MAX_ENTITIES)
		viewentity = hwcl_playernum + 1;
	cl.viewentity = viewentity;
	if (viewentity <= HWCL_MAX_CLIENTS)
	{
		state = &hwcl_server_state.players[viewentity - 1];
		VectorCopy (state->velocity, cl.velocity);
	}
	cl.num_entities = highest + 1;
	if (cl.num_entities < 1)
		cl.num_entities = 1;
}

static void HWCL_ParseInventoryUpdate (void)
{
	unsigned int sc1 = 0;
	unsigned int sc2 = 0;
	int groups;

	groups = MSG_ReadByte ();
	if (groups & 1) sc1 |= (unsigned int)MSG_ReadByte ();
	if (groups & 2) sc1 |= (unsigned int)MSG_ReadByte () << 8;
	if (groups & 4) sc1 |= (unsigned int)MSG_ReadByte () << 16;
	if (groups & 8) sc1 |= (unsigned int)MSG_ReadByte () << 24;
	if (groups & 16) sc2 |= (unsigned int)MSG_ReadByte ();
	if (groups & 32) sc2 |= (unsigned int)MSG_ReadByte () << 8;
	if (groups & 64) sc2 |= (unsigned int)MSG_ReadByte () << 16;
	if (groups & 128) sc2 |= (unsigned int)MSG_ReadByte () << 24;

	if (sc1 & (1u << 0)) MSG_ReadShort (); /* health */
	if (sc1 & (1u << 1)) MSG_ReadByte (); /* level */
	if (sc1 & (1u << 2)) MSG_ReadByte (); /* intelligence */
	if (sc1 & (1u << 3)) MSG_ReadByte (); /* wisdom */
	if (sc1 & (1u << 4)) MSG_ReadByte (); /* strength */
	if (sc1 & (1u << 5)) MSG_ReadByte (); /* dexterity */
	/* Bit 6 is SC1_TELEPORT_TIME in the protocol header, but this server's
	 * writer does not serialize a payload for it. */
	if (sc1 & (1u << 7)) MSG_ReadByte (); /* bluemana */
	if (sc1 & (1u << 8)) MSG_ReadByte (); /* greenmana */
	if (sc1 & (1u << 9)) MSG_ReadLong (); /* experience */
	if (sc1 & (1u << 10)) MSG_ReadByte (); /* cnt_torch */
	if (sc1 & (1u << 11)) MSG_ReadByte (); /* cnt_h_boost */
	if (sc1 & (1u << 12)) MSG_ReadByte (); /* cnt_sh_boost */
	if (sc1 & (1u << 13)) MSG_ReadByte (); /* cnt_mana_boost */
	if (sc1 & (1u << 14)) MSG_ReadByte (); /* cnt_teleport */
	if (sc1 & (1u << 15)) MSG_ReadByte (); /* cnt_tome */
	if (sc1 & (1u << 16)) MSG_ReadByte (); /* cnt_summon */
	if (sc1 & (1u << 17)) MSG_ReadByte (); /* cnt_invisibility */
	if (sc1 & (1u << 18)) MSG_ReadByte (); /* cnt_glyph */
	if (sc1 & (1u << 19)) MSG_ReadByte (); /* cnt_haste */
	if (sc1 & (1u << 20)) MSG_ReadByte (); /* cnt_blast */
	if (sc1 & (1u << 21)) MSG_ReadByte (); /* cnt_polymorph */
	if (sc1 & (1u << 22)) MSG_ReadByte (); /* cnt_flight */
	if (sc1 & (1u << 23)) MSG_ReadByte (); /* cnt_cubeofforce */
	if (sc1 & (1u << 24)) MSG_ReadByte (); /* cnt_invincibility */
	if (sc1 & (1u << 25)) MSG_ReadByte (); /* artifact_active */
	if (sc1 & (1u << 26)) MSG_ReadByte (); /* artifact_low */
	if (sc1 & (1u << 27)) MSG_ReadByte (); /* movetype */
	if (sc1 & (1u << 28)) MSG_ReadByte (); /* cameramode */
	if (sc1 & (1u << 29)) MSG_ReadFloat (); /* hasted */
	if (sc1 & (1u << 30)) MSG_ReadByte (); /* inventory */
	if (sc1 & (1u << 31)) MSG_ReadByte (); /* rings_active */

	if (sc2 & (1u << 0)) MSG_ReadByte (); /* rings_low */
	if (sc2 & (1u << 1)) MSG_ReadByte (); /* armor_amulet */
	if (sc2 & (1u << 2)) MSG_ReadByte (); /* armor_bracer */
	if (sc2 & (1u << 3)) MSG_ReadByte (); /* armor_breastplate */
	if (sc2 & (1u << 4)) MSG_ReadByte (); /* armor_helmet */
	if (sc2 & (1u << 5)) MSG_ReadByte (); /* ring_flight */
	if (sc2 & (1u << 6)) MSG_ReadByte (); /* ring_water */
	if (sc2 & (1u << 7)) MSG_ReadByte (); /* ring_turning */
	if (sc2 & (1u << 8)) MSG_ReadByte (); /* ring_regeneration */
	/* Bits 9 and 10 are declared but not serialized by this server. */
	if (sc2 & (1u << 11)) MSG_ReadString (); /* puzzle_inv1 */
	if (sc2 & (1u << 12)) MSG_ReadString (); /* puzzle_inv2 */
	if (sc2 & (1u << 13)) MSG_ReadString (); /* puzzle_inv3 */
	if (sc2 & (1u << 14)) MSG_ReadString (); /* puzzle_inv4 */
	if (sc2 & (1u << 15)) MSG_ReadString (); /* puzzle_inv5 */
	if (sc2 & (1u << 16)) MSG_ReadString (); /* puzzle_inv6 */
	if (sc2 & (1u << 17)) MSG_ReadString (); /* puzzle_inv7 */
	if (sc2 & (1u << 18)) MSG_ReadString (); /* puzzle_inv8 */
	if (sc2 & (1u << 19)) MSG_ReadShort (); /* max_health */
	if (sc2 & (1u << 20)) MSG_ReadByte (); /* max_mana */
	if (sc2 & (1u << 21)) MSG_ReadFloat (); /* flags */
}

static void HWCL_ParsePlayerInfo (void)
{
	hwcl_entity_state_t discard = {0};
	hwcl_entity_state_t *state;
	int flags;
	int playernum;
	int i;

	playernum = MSG_ReadByte ();
	state = playernum >= 0 && playernum < HWCL_MAX_CLIENTS ?
		&hwcl_server_state.players[playernum] : &discard;
	flags = (unsigned short)MSG_ReadShort ();
	state->active = true;
	for (i = 0; i < 3; i++)
		state->origin[i] = MSG_ReadCoord ();
	state->frame = MSG_ReadByte ();
	if (flags & (1 << 0)) MSG_ReadByte (); /* msec */
	if (flags & (1 << 1)) HWCL_SkipUsercmd ();
	for (i = 0; i < 3; i++)
	{
		if (flags & (1 << (2 + i)))
			state->velocity[i] = MSG_ReadShort ();
		else
			state->velocity[i] = 0;
	}
	if (flags & (1 << 5)) state->modelindex = MSG_ReadShort ();
	if (flags & (1 << 6)) state->skinnum = MSG_ReadByte ();
	if (flags & (1 << 7)) state->effects = MSG_ReadByte ();
	else state->effects = 0;
	if (flags & (1 << 11)) state->effects |= MSG_ReadByte () << 8;
	if (flags & (1 << 8)) state->weaponframe = MSG_ReadByte ();
	else state->weaponframe = 0;
	if (flags & (1 << 12)) state->drawflags = MSG_ReadByte ();
	else state->drawflags = 0;
	if (flags & (1 << 13)) state->scale = MSG_ReadByte ();
	else state->scale = 0;
	if (flags & (1 << 14)) state->abslight = MSG_ReadByte ();
	else state->abslight = 0;
	if (flags & (1 << 15)) MSG_ReadShort (); /* weapon-channel sound */
}

static void HWCL_ParseServerMessage (void)
{
	int command;
	const char *text;

	while (msg_readcount < hw_net_message.cursize)
	{
		command = MSG_ReadByte ();
		switch (command)
		{
		case HW_SVC_NOP:
			break;
		case HW_SVC_PRINT:
			MSG_ReadByte (); /* print level */
			text = MSG_ReadString ();
			if (!msg_badread)
				Con_Printf ("%s", text);
			break;
		case HW_SVC_DISCONNECT:
			Con_Printf ("HexenWorld server disconnected.\n");
			HWCL_Disconnect ();
			return;
		case HW_SVC_UPDATESTAT:
			command = MSG_ReadByte ();
			if (command >= 0 && command < MAX_CL_STATS)
				hwcl_server_state.stats[command] = MSG_ReadByte ();
			else
				MSG_ReadByte ();
			break;
		case HW_SVC_SETVIEW:
			hwcl_server_state.viewentity = MSG_ReadShort ();
			break;
		case HW_SVC_SERVERDATA:
			HWCL_ParseServerData ();
			break;
		case HW_SVC_TIME:
			hwcl_server_state.server_time = MSG_ReadFloat ();
			break;
		case HW_SVC_SETANGLE:
			hwcl_server_state.viewangles[0] = MSG_ReadAngle ();
			hwcl_server_state.viewangles[1] = MSG_ReadAngle ();
			hwcl_server_state.viewangles[2] = MSG_ReadAngle ();
			VectorCopy (hwcl_server_state.viewangles, cl.viewangles);
			CL_LatchFixAngle ();
			break;
		case HW_SVC_LIGHTSTYLE:
			{
				int style = MSG_ReadByte ();
				const char *map = MSG_ReadString ();
				if (style >= 0 && style < MAX_LIGHTSTYLES)
				{
					q_strlcpy (cl_lightstyle[style].map, map,
							sizeof(cl_lightstyle[style].map));
					cl_lightstyle[style].length = strlen (
						cl_lightstyle[style].map);
					CL_SetLightstyleLevels (&cl_lightstyle[style]);
				}
			}
			break;
		case HW_SVC_SOUND:
			HWCL_ParseSound ();
			break;
		case HW_SVC_UPDATEFRAGS:
			{
				int slot = MSG_ReadByte ();
				int frags = MSG_ReadShort ();
				if (slot >= 0 && slot < cl.maxclients && cl.scores)
					cl.scores[slot].frags = frags;
			}
			break;
		case HW_SVC_STOPSOUND:
			{
				int channel = MSG_ReadShort ();
				S_StopSound (channel >> 3, channel & 7);
			}
			break;
		case HW_SVC_PARTICLE:
			HWCL_ParseParticle ();
			break;
		case HW_SVC_DAMAGE:
			HWCL_ParseDamage ();
			break;
		case HW_SVC_SET_VIEW_TINT:
			cl.viewent.colorshade = MSG_ReadByte ();
			break;
		case HW_SVC_SET_VIEW_FLAGS:
			cl.viewent.drawflags |= MSG_ReadByte ();
			break;
		case HW_SVC_CLEAR_VIEW_FLAGS:
			cl.viewent.drawflags &= ~MSG_ReadByte ();
			break;
		case HW_SVC_START_EFFECT:
			if (!HWCL_ParseStartEffect ())
				return;
			break;
		case HW_SVC_END_EFFECT:
			MSG_ReadByte ();
			break;
		case HW_SVC_CENTERPRINT:
			SCR_CenterPrint (MSG_ReadString ());
			break;
		case HW_SVC_KILLEDMONSTER:
		case HW_SVC_FOUNDSECRET:
		case HW_SVC_SELLSCREEN:
		case HW_SVC_SMALLKICK:
		case HW_SVC_BIGKICK:
			break;
		case HW_SVC_SPAWNSTATICSOUND:
			HWCL_ParseStaticSound ();
			break;
		case HW_SVC_INTERMISSION:
			HWCL_SkipCoords (3);
			HWCL_SkipAngles (3);
			break;
		case HW_SVC_FINALE:
			SCR_CenterPrint (MSG_ReadString ());
			break;
		case HW_SVC_CDTRACK:
			hwcl_server_state.cdtrack = MSG_ReadByte ();
			break;
		case HW_SVC_MIDI_NAME:
			q_strlcpy (hwcl_server_state.midi_name, MSG_ReadString (),
					sizeof(hwcl_server_state.midi_name));
			break;
		case HW_SVC_UPDATEPING:
			MSG_ReadByte ();
			MSG_ReadShort ();
			break;
		case HW_SVC_UPDATEENTERTIME:
			{
				int slot = MSG_ReadByte ();
				float entertime = MSG_ReadFloat ();
				if (slot >= 0 && slot < cl.maxclients && cl.scores)
					cl.scores[slot].entertime = entertime;
			}
			break;
		case HW_SVC_UPDATESTATLONG:
			command = MSG_ReadByte ();
			if (command >= 0 && command < MAX_CL_STATS)
				hwcl_server_state.stats[command] = MSG_ReadLong ();
			else
				MSG_ReadLong ();
			break;
		case HW_SVC_MUZZLEFLASH:
			MSG_ReadShort ();
			break;
		case HW_SVC_UPDATEUSERINFO:
			{
				int slot = MSG_ReadByte ();
				int uid = MSG_ReadLong ();
				const char *userinfo = MSG_ReadString ();
				const char *name;
				(void)uid;
				if (slot >= 0 && slot < cl.maxclients && cl.scores)
				{
					name = HWCL_InfoValue (userinfo, "name");
					q_strlcpy (cl.scores[slot].name, name,
							sizeof(cl.scores[slot].name));
					cl.scores[slot].colors = atoi (
						HWCL_InfoValue (userinfo, "topcolor")) << 4;
					cl.scores[slot].colors |= atoi (
						HWCL_InfoValue (userinfo, "bottomcolor"));
				}
			}
			break;
		case HW_SVC_DOWNLOAD:
			HWCL_ParseDownload ();
			break;
		case HW_SVC_PLAYERINFO:
			{
				int slot = hwcl_server_state.viewentity - 1;
				HWCL_ParsePlayerInfo ();
				if (slot >= 0 && slot < HWCL_MAX_CLIENTS &&
					hwcl_server_state.players[slot].active)
				{
					entity_t *ent = &cl_entities[cl.viewentity];
					const hwcl_entity_state_t *state =
						&hwcl_server_state.players[slot];
					VectorCopy (state->origin, ent->baseline.origin);
					ent->baseline.flags |= BE_ON;
				}
			}
			break;
		case HW_SVC_NAILS:
			HWCL_ParseNails ();
			break;
		case HW_SVC_PACKETENTITIES:
			HWCL_ParsePacketEntities (false);
			break;
		case HW_SVC_DELTAPACKETENTITIES:
			HWCL_ParsePacketEntities (true);
			break;
		case HW_SVC_CHOKECOUNT:
			hwcl_server_state.chokecount = MSG_ReadByte ();
			break;
		case HW_SVC_MAXSPEED:
			hwcl_server_state.maxspeed = MSG_ReadFloat ();
			break;
		case HW_SVC_ENTGRAVITY:
			hwcl_server_state.entgravity = MSG_ReadFloat ();
			break;
		case HW_SVC_UPDATE_INV:
			HWCL_ParseInventoryUpdate ();
			break;
		case HW_SVC_PARTICLE2:
			HWCL_ParseParticle2 ();
			break;
		case HW_SVC_PARTICLE3:
			HWCL_ParseParticle3 ();
			break;
		case HW_SVC_PARTICLE4:
			HWCL_ParseParticle4 ();
			break;
		case HW_SVC_TURN_EFFECT:
			MSG_ReadByte ();
			MSG_ReadFloat ();
			HWCL_SkipCoords (6);
			break;
		case HW_SVC_UPDATE_EFFECT:
			if (!HWCL_ParseUpdateEffect ())
				return;
			break;
		case HW_SVC_MULTIEFFECT:
			if (!HWCL_ParseMultiEffect ())
				return;
			break;
		case HW_SVC_RAINEFFECT:
			HWCL_ParseRainEffect ();
			break;
		case HW_SVC_PACKMISSILE:
			HWCL_ParsePackedMissiles ();
			break;
		case HW_SVC_TARGETUPDATE:
			MSG_ReadByte ();
			MSG_ReadByte ();
			MSG_ReadByte ();
			break;
		case HW_SVC_INDEXED_PRINT:
			MSG_ReadByte (); /* print level */
			MSG_ReadShort (); /* strings.txt index */
			break;
		case HW_SVC_NAME_PRINT:
			MSG_ReadByte (); /* print level */
			MSG_ReadByte (); /* player slot */
			break;
		case HW_SVC_SOUND_UPDATE_POS:
			{
				vec3_t origin;
				int channel = MSG_ReadShort ();
				HWCL_ReadCoords (origin);
				if (!msg_badread)
					S_UpdateSoundPos (channel >> 3, channel & 7, origin);
			}
			break;
		case HW_SVC_UPDATE_PIV:
			hwcl_server_state.piv = MSG_ReadLong ();
			break;
		case HW_SVC_PLAYER_SOUND:
			{
				vec3_t origin;
				int slot = MSG_ReadByte ();
				int sound_num;
				HWCL_ReadCoords (origin);
				sound_num = MSG_ReadShort ();
				if (!msg_badread && slot >= 0 && slot < cl.maxclients &&
						sound_num > 0 && sound_num < MAX_SOUNDS &&
						cl.sound_precache[sound_num])
					S_StartSound (slot + 1, 2, cl.sound_precache[sound_num],
							origin, 1.0f, 1.0f);
			}
			break;
		case HW_SVC_UPDATEPCLASS:
			{
				int slot = MSG_ReadByte ();
				int packed = MSG_ReadByte ();
				if (slot >= 0 && slot < HWCL_MAX_CLIENTS && cl.scores)
				{
					hwcl_server_state.playerclass[slot] = (packed >> 5) & 7;
					hwcl_server_state.playerlevel[slot] = packed & 31;
					cl.scores[slot].playerclass =
						(float)hwcl_server_state.playerclass[slot];
				}
			}
			break;
		case HW_SVC_HASKEY:
		case HW_SVC_NONEHASKEY:
		case HW_SVC_ISDOC:
		case HW_SVC_NODOC:
			MSG_ReadByte ();
			MSG_ReadByte ();
			break;
		case HW_SVC_PLAYERSKIPPED:
			MSG_ReadByte ();
			break;
		case HW_SVC_UPDATEDMINFO:
			MSG_ReadByte ();
			MSG_ReadShort ();
			MSG_ReadByte ();
			break;
		case HW_SVC_UPDATESIEGEINFO:
			MSG_ReadByte ();
			MSG_ReadByte ();
			break;
		case HW_SVC_UPDATESIEGETEAM:
		case HW_SVC_UPDATESIEGELOSSES:
			MSG_ReadByte ();
			MSG_ReadByte ();
			break;
		case HW_SVC_SOUNDLIST:
			HWCL_ParsePrecacheList (false);
			break;
		case HW_SVC_MODELLIST:
			HWCL_ParsePrecacheList (true);
			break;
		case HW_SVC_SPAWNBASELINE:
			HWCL_ParseBaseline ();
			break;
		case HW_SVC_STUFFTEXT:
			text = MSG_ReadString ();
			if (!msg_badread)
			{
				Con_DPrintf ("HexenWorld server command: %s", text);
				/* This is a server-directed signon request, not console text: the
				 * Hexen II command buffer uses a different transport.  Restrict it
				 * to the three documented handshake commands. */
				if (!q_strncasecmp (text, "cmd prespawn ", 13) ||
					!q_strncasecmp (text, "cmd spawn ", 10) ||
					!q_strncasecmp (text, "cmd begin", 9))
					HWCL_StringCmd (text + 4);
				else if (!q_strcasecmp (text, "skins\n"))
				{
					/* Skin download/translation is not wired up yet, but it must
					 * not hold the transport handshake hostage. */
					HWCL_StringCmd (va ("begin %d", hwcl_servercount));
					cls.signon = SIGNONS;
					Con_Printf ("HexenWorld signon complete; awaiting state adapter.\n");
				}
			}
			break;
		default:
			/* Do not desynchronise the reader by guessing unknown message sizes.
			 * Baselines, entities, and presentation messages are parsed by the
			 * next client-state adapter layer. */
			Con_DPrintf ("HexenWorld server message %d awaits state adapter.\n",
					command);
			return;
		}
		if (msg_badread)
		{
			Con_Printf ("Malformed HexenWorld server message.\n");
			return;
		}
	}
}

static void HWCL_InitNet (void)
{
	if (hwcl_net_initialized)
		return;

	HuffInit ();
	HWNetchan_Init ();
	HWNET_Init (HW_PORT_CLIENT);
	hwcl_net_initialized = true;
}

static void HWCL_SendConnectPacket (void)
{
	char data[256];
	int portals;

	portals = ((gameflags & GAME_PORTALS) == GAME_PORTALS);
	q_snprintf (data, sizeof(data),
			"%c%c%c%cconnect %d \"\\name\\%s\\playerclass\\%d\\*cap\\A\"\n",
			255, 255, 255, 255, portals, cl_name.string,
			(int)cl_playerclass.value);
	HWNET_SendPacket (strlen(data), data, &hwcl_server);
	hwcl_connect_time = realtime;
	Con_Printf ("Connecting to HexenWorld server %s...\n",
			HWNET_AdrToString (&hwcl_server));
}

qboolean HWCL_Connect (const char *host)
{
	HWCL_InitNet ();
	HWCL_Disconnect ();

	if (!HWNET_StringToAdr (host, &hwcl_server))
	{
		Con_Printf ("Bad HexenWorld server address: %s\n", host);
		return false;
	}
	if (!hwcl_server.port)
		hwcl_server.port = BigShort (HW_PORT_SERVER);

	CL_ClearState ();
	cls.state = ca_connected;
	hwcl_state = hwcl_connecting;
	HWCL_SendConnectPacket ();
	return true;
}

qboolean HWCL_Active (void)
{
	return hwcl_state != hwcl_disconnected;
}

void HWCL_SendCmd (const usercmd_t *cmd)
{
	hwcl_usercmd_t current;
	hwcl_usercmd_t oldest;
	hwcl_usercmd_t oldcmd;
	sizebuf_t buf;
	byte data[128];
	int i;
	int msec;

	if (hwcl_state != hwcl_connected || hwcl_protocol == 0 ||
			!HWNetchan_CanPacket (&hwcl_netchan))
		return;

	memset (&current, 0, sizeof(current));
	VectorCopy (cl.viewangles, current.angles);
	current.forwardmove = (int)cmd->forwardmove;
	current.sidemove = (int)cmd->sidemove;
	current.upmove = (int)cmd->upmove;
	current.buttons = CL_GetButtonBits ();
	current.impulse = CL_GetImpulse ();
	msec = (int)(host_frametime * 1000.0 + 0.5);
	if (msec < 1)
		msec = 1;
	if (msec > 255)
		msec = 255;
	current.msec = msec;

	if (!hwcl_have_cmd_history)
	{
		hwcl_cmd_history[0] = current;
		hwcl_cmd_history[1] = current;
		hwcl_cmd_history[2] = current;
		hwcl_have_cmd_history = true;
	}

	oldest = hwcl_cmd_history[0];
	oldcmd = hwcl_cmd_history[1];
	/* Impulses are one-shot commands.  Replaying them from the two backup
	 * commands would fire a weapon or select an item multiple times. */
	oldest.impulse = 0;
	oldcmd.impulse = 0;

	SZ_Init (&buf, data, sizeof(data));
	MSG_WriteByte (&buf, HW_CLC_MOVE);
	HWCL_WriteUsercmd (&buf, &oldest);
	HWCL_WriteUsercmd (&buf, &oldcmd);
	HWCL_WriteUsercmd (&buf, &current);
	HWNetchan_Transmit (&hwcl_netchan, buf.cursize, buf.data);

	for (i = 0; i < 2; i++)
		hwcl_cmd_history[i] = hwcl_cmd_history[i + 1];
	hwcl_cmd_history[2] = current;
}

void HWCL_Disconnect (void)
{
	static const byte drop[] = {HW_CLC_STRINGCMD, 'd', 'r', 'o', 'p', 0};

	hwcl_protocol = 0;
	hwcl_servercount = 0;
	hwcl_entity_sequence = -1;
	hwcl_have_cmd_history = false;
	memset (hwcl_cmd_history, 0, sizeof(hwcl_cmd_history));
	memset (&hwcl_server_state, 0, sizeof(hwcl_server_state));
	cls.signon = 0;
	if (hwcl_state == hwcl_connected)
	{
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
	}
	hwcl_state = hwcl_disconnected;
	hwcl_received_packet = false;
	cls.state = ca_disconnected;
}

static void HWCL_ConnectionlessPacket (void)
{
	int command;

	MSG_BeginReadingFrom (&hw_net_message);
	MSG_ReadLong (); /* connectionless -1 marker */
	command = MSG_ReadByte ();

	if (command == HW_S2C_CONNECTION)
	{
		if (hwcl_state != hwcl_connecting)
			return;

		HWNetchan_Setup (&hwcl_netchan, &hw_net_from);
		MSG_WriteChar (&hwcl_netchan.message, HW_CLC_STRINGCMD);
		MSG_WriteString (&hwcl_netchan.message, "new");
		HWNetchan_Transmit (&hwcl_netchan, 0, NULL);
		hwcl_state = hwcl_connected;
		Con_Printf ("HexenWorld connection accepted.\n");
		return;
	}

	if (command == HW_A2C_PRINT && hw_net_message.cursize > 5)
		Con_Printf ("%s", (char *)hw_net_message.data + 5);
}

void HWCL_Frame (void)
{
	if (!hwcl_net_initialized || hwcl_state == hwcl_disconnected)
		return;

	if (hwcl_state == hwcl_connecting && realtime - hwcl_connect_time > 5.0)
		HWCL_SendConnectPacket ();

	while (HWNET_GetPacket ())
	{
		if (hw_net_message.cursize >= 4 &&
		    hw_net_message.data[0] == 255 && hw_net_message.data[1] == 255 &&
		    hw_net_message.data[2] == 255 && hw_net_message.data[3] == 255)
		{
			HWCL_ConnectionlessPacket ();
			continue;
		}

		if (hwcl_state != hwcl_connected ||
		    !HWNET_CompareAdr (&hw_net_from, &hwcl_netchan.remote_address))
			continue;
		if (!HWNetchan_Process (&hwcl_netchan))
			continue;

		if (!hwcl_received_packet)
		{
			Con_Printf ("HexenWorld netchan established (%d payload bytes).\n",
					hw_net_message.cursize - msg_readcount);
			hwcl_received_packet = true;
		}
		HWCL_ParseServerMessage ();
	}

	/* Drive acknowledgements and reliable retransmission even when the server
	 * has not sent a packet this render frame.  In particular, sequence zero is
	 * rejected by the historical receiver, so the queued "new" command must
	 * advance to sequence one on the following frame. */
	if (hwcl_state == hwcl_connected && HWNetchan_CanPacket (&hwcl_netchan))
		HWNetchan_Transmit (&hwcl_netchan, 0, NULL);
}

void HWCL_Shutdown (void)
{
	HWCL_Disconnect ();
	if (hwcl_net_initialized)
	{
		HWNET_Shutdown ();
		hwcl_net_initialized = false;
	}
}
