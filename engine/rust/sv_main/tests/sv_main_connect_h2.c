/*
 * sv_main_connect_h2.c -- the C original of the server's connect and signon
 * messages, moved verbatim out of engine/hexen2/sv_main.c: SV_ProtocolName,
 * SV_SendServerinfo, SV_ConnectClient, SV_CheckForNewClients, SV_ModelIndex,
 * SV_SignonBufferCount, SV_SignonBufferSize, SV_ReserveSignonSpace,
 * SV_SendSignonBuffer, SV_CreateBaseline and SV_SendReconnect, plus the
 * supporting SV_GetLevelname (still C in sv_main.c; the harness scripts its
 * Host_GetString).  The differential harness beside this file includes it;
 * no engine target compiles it.  The engine's implementation is
 * engine/rust/src/sv_main_connect.rs (#469 slice D).
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

/* sv_main.c's own include, quakedef.h, comes from the harness, and so do
 * sv, svs, sv_protocol and the engine functions these call. */

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
	case PROTOCOL_HEXENWAIL_1:	return "Hexenwail/1";
	default:			return NULL;
	}
}


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

	/* Say what this connection is actually allowed to carry.  The wire
	 * limits are the peer's, not ours, and when they are wrong the symptom
	 * reaches the player as a truncated packet several messages later with
	 * nothing to point at -- so record the numbers at the one moment they
	 * are decided.  Local connections are exempt (see SV_SendClientDatagram). */
	Con_DPrintf ("%s: %s, protocol %i (%s), datagram %i, reliable %i\n",
		     __thisfunc__,
		     NET_IsLocalConnection (client->netconnection) ? "local" : "remote",
		     sv_protocol, SV_ProtocolName (sv_protocol),
		     NET_IsLocalConnection (client->netconnection) ?
			     NET_MaxUnreliableMessage (client->netconnection) : SV_MaxDatagram (),
		     NET_IsLocalConnection (client->netconnection) ?
			     MAX_MSGLEN : SV_MaxReliableMessage ());

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

	if (sv_protocol >= PROTOCOL_UH2_114)
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
