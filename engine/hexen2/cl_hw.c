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

#define HW_PORT_CLIENT 26901
#define HW_PORT_SERVER 26950
#define HW_S2C_CONNECTION 'j'
#define HW_A2C_PRINT 'n'
#define HW_CLC_STRINGCMD 4

/* Keep this protocol namespace separate from Hexen II's protocol.h, which is
 * already included through quakedef.h. */
#define HW_OLD_PROTOCOL_VERSION 24
#define HW_PROTOCOL_VERSION 25
#define HW_PROTOCOL_VERSION_EXT 26
#define HW_PROTOCOL_VERSION_HEXENWAIL_1 100
#define HW_SVC_TIME 7
#define HW_SVC_PRINT 8
#define HW_SVC_STUFFTEXT 9
#define HW_SVC_SERVERDATA 11
#define HW_SVC_LIGHTSTYLE 12
#define HW_SVC_SPAWNBASELINE 22
#define HW_SVC_UPDATEPING 36
#define HW_SVC_UPDATEENTERTIME 37
#define HW_SVC_UPDATESTATLONG 38
#define HW_SVC_UPDATEUSERINFO 40
#define HW_SVC_PLAYERINFO 42
#define HW_SVC_MODELLIST 45
#define HW_SVC_SOUNDLIST 46
#define HW_SVC_PACKETENTITIES 47
#define HW_SVC_DELTAPACKETENTITIES 48
#define HW_SVC_UPDATEDMINFO 75
#define HW_SVC_UPDATESIEGEINFO 76
#define HW_SVC_UPDATESIEGETEAM 77
#define HW_SVC_UPDATESIEGELOSSES 78

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

static void HWCL_StringCmd (const char *command)
{
	MSG_WriteByte (&hwcl_netchan.message, HW_CLC_STRINGCMD);
	MSG_WriteString (&hwcl_netchan.message, command);
}

static qboolean HWCL_ValidProtocol (int protocol)
{
	return protocol == HW_OLD_PROTOCOL_VERSION ||
		protocol == HW_PROTOCOL_VERSION ||
		protocol == HW_PROTOCOL_VERSION_EXT ||
		protocol == HW_PROTOCOL_VERSION_HEXENWAIL_1;
}

/* Consume a chunked (protocol 26+) or classic precache list.  Asset loading
 * remains a later integration layer; consuming the lists here is still
 * important because it advances the reliable signon handshake. */
static void HWCL_ParsePrecacheList (qboolean models)
{
	int index = 0;
	const char *entry;

	if (hwcl_protocol >= HW_PROTOCOL_VERSION_EXT)
		index = MSG_ReadLong ();
	for (;;)
	{
		entry = MSG_ReadString ();
		if (!entry[0] || msg_badread)
			break;
		index++;
	}
	if (msg_badread)
		return;

	if (hwcl_protocol >= HW_PROTOCOL_VERSION_EXT)
	{
		index = MSG_ReadLong ();
		if (msg_badread)
			return;
		if (index)
		{
			HWCL_StringCmd (va ("%slist %d %d", models ? "model" : "sound",
					hwcl_servercount, index));
			return;
		}
	}

	if (models)
	{
		Con_Printf ("HexenWorld precache lists received; requesting signon.\n");
		HWCL_StringCmd (va ("prespawn %d 0", hwcl_servercount));
	}
	else
		HWCL_StringCmd (va ("modellist %d 0", hwcl_servercount));
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
	q_strlcpy (gamedir, MSG_ReadString (), sizeof(gamedir));
	playernum = MSG_ReadByte ();
	q_strlcpy (levelname, MSG_ReadString (), sizeof(levelname));
	if (hwcl_protocol >= HW_PROTOCOL_VERSION)
		for (i = 0; i < 10; i++)
			MSG_ReadFloat ();
	if (msg_badread)
		return;

	Con_Printf ("HexenWorld protocol %d, game %s, level %s (player %d%s).\n",
			hwcl_protocol, gamedir, levelname, playernum & 127,
			(playernum & 128) ? ", spectator" : "");
	HWCL_StringCmd (va ("soundlist %d 0", hwcl_servercount));
}

/* Signon buffers contain baseline records before their svc_stufftext request.
 * Consume their wire representation now; retaining baselines for entity delta
 * reconstruction belongs with the client-state adapter. */
static void HWCL_SkipBaseline (void)
{
	int i;

	MSG_ReadShort (); /* entity number */
	MSG_ReadShort (); /* model */
	for (i = 0; i < 6; i++)
		MSG_ReadByte (); /* frame through absolute light */
	for (i = 0; i < 3; i++)
	{
		MSG_ReadCoord ();
		MSG_ReadAngle ();
	}
}

/* Consume an entity delta without retaining it.  This keeps the packet stream
 * synchronised until the state adapter can map these records to Hexenwail's
 * client entities. */
static void HWCL_SkipEntityDelta (int bits)
{
	/* The low nine bits are the entity number, not flags. */
	bits &= ~511;
	if (bits & (1 << 15))
		bits |= MSG_ReadByte ();
	if (bits & (1 << 7))
		bits |= MSG_ReadByte () << 16;
	if (bits & (1 << 16))
	{
		if (bits & (1 << 6))
			MSG_ReadShort ();
		else
			MSG_ReadByte ();
	}
	if (bits & (1 << 13)) MSG_ReadByte ();
	if (bits & (1 << 3)) MSG_ReadByte ();
	if (bits & (1 << 4)) MSG_ReadByte ();
	if (bits & (1 << 18)) MSG_ReadByte ();
	if (bits & (1 << 5)) MSG_ReadLong ();
	if (bits & (1 << 9)) MSG_ReadCoord ();
	if (bits & (1 << 0)) MSG_ReadAngle ();
	if (bits & (1 << 10)) MSG_ReadCoord ();
	if (bits & (1 << 12)) MSG_ReadAngle ();
	if (bits & (1 << 11)) MSG_ReadCoord ();
	if (bits & (1 << 1)) MSG_ReadAngle ();
	if (bits & (1 << 2)) MSG_ReadByte ();
	if (bits & (1 << 19)) MSG_ReadByte ();
	if (bits & (1 << 17)) MSG_ReadShort ();
	if (bits & (1 << 20)) MSG_ReadByte ();
}

static void HWCL_SkipPacketEntities (qboolean delta)
{
	int bits;

	if (delta)
		MSG_ReadByte (); /* source sequence */
	for (;;)
	{
		bits = (unsigned short)MSG_ReadShort ();
		if (!bits || msg_badread)
			return;
		if (!(bits & (1 << 14))) /* U_REMOVE has no payload */
			HWCL_SkipEntityDelta (bits);
		if (msg_badread)
			return;
	}
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

static void HWCL_SkipPlayerInfo (void)
{
	int flags;
	int i;

	MSG_ReadByte (); /* player slot */
	flags = (unsigned short)MSG_ReadShort ();
	for (i = 0; i < 3; i++)
		MSG_ReadCoord ();
	MSG_ReadByte (); /* frame */
	if (flags & (1 << 0)) MSG_ReadByte (); /* msec */
	if (flags & (1 << 1)) HWCL_SkipUsercmd ();
	for (i = 0; i < 3; i++)
		if (flags & (1 << (2 + i))) MSG_ReadShort ();
	if (flags & (1 << 5)) MSG_ReadShort ();
	if (flags & (1 << 6)) MSG_ReadByte ();
	if (flags & (1 << 7)) MSG_ReadByte ();
	if (flags & (1 << 11)) MSG_ReadByte ();
	if (flags & (1 << 8)) MSG_ReadByte ();
	if (flags & (1 << 12)) MSG_ReadByte ();
	if (flags & (1 << 13)) MSG_ReadByte ();
	if (flags & (1 << 14)) MSG_ReadByte ();
	if (flags & (1 << 15)) MSG_ReadShort ();
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
		case HW_SVC_PRINT:
			MSG_ReadByte (); /* print level */
			text = MSG_ReadString ();
			if (!msg_badread)
				Con_Printf ("%s", text);
			break;
		case HW_SVC_SERVERDATA:
			HWCL_ParseServerData ();
			break;
		case HW_SVC_TIME:
			MSG_ReadFloat ();
			break;
		case HW_SVC_LIGHTSTYLE:
			MSG_ReadByte ();
			MSG_ReadString ();
			break;
		case HW_SVC_UPDATEPING:
			MSG_ReadByte ();
			MSG_ReadShort ();
			break;
		case HW_SVC_UPDATEENTERTIME:
			MSG_ReadByte ();
			MSG_ReadFloat ();
			break;
		case HW_SVC_UPDATESTATLONG:
			MSG_ReadByte ();
			MSG_ReadLong ();
			break;
		case HW_SVC_UPDATEUSERINFO:
			MSG_ReadByte ();
			MSG_ReadLong ();
			MSG_ReadString ();
			break;
		case HW_SVC_PLAYERINFO:
			HWCL_SkipPlayerInfo ();
			break;
		case HW_SVC_PACKETENTITIES:
			HWCL_SkipPacketEntities (false);
			break;
		case HW_SVC_DELTAPACKETENTITIES:
			HWCL_SkipPacketEntities (true);
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
			HWCL_SkipBaseline ();
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

	hwcl_state = hwcl_connecting;
	HWCL_SendConnectPacket ();
	return true;
}

qboolean HWCL_Active (void)
{
	return hwcl_state != hwcl_disconnected;
}

void HWCL_Disconnect (void)
{
	static const byte drop[] = {HW_CLC_STRINGCMD, 'd', 'r', 'o', 'p', 0};

	hwcl_protocol = 0;
	hwcl_servercount = 0;
	if (hwcl_state == hwcl_connected)
	{
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
		HWNetchan_Transmit (&hwcl_netchan, sizeof(drop), (byte *)drop);
	}
	hwcl_state = hwcl_disconnected;
	hwcl_received_packet = false;
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
