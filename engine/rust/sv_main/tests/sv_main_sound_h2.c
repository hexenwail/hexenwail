/*
 * sv_main_sound_h2.c -- the C original of the server's sound and particle
 * messages, moved verbatim out of engine/hexen2/sv_main.c: SV_StartParticle
 * through SV_UpdateSoundPos, and SV_MaxSounds through SV_StartSound (the
 * SV_ParseWorldspawnSky between them stays in sv_main.c).  The differential
 * harness beside this file includes it; no engine target compiles it.  The
 * engine's implementation is engine/rust/src/sv_main_sound.rs (#469 slice A).
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
 * sv, sv_protocol and the engine functions these call. */

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
SV_SoundEntityFits

Every Raven-lineage sound message packs the entity into (ent<<3)|channel in
one short, so it can name entities 0..8191 and nothing above.  Above that
the entity goes out whole -- an SND_LARGEENTITY svc_sound, or
svc_stopsound_large / svc_sound_update_pos_large -- which only protocol 100
defines; on 18..21 the message is dropped, as Ironwail drops it on
PROTOCOL_NETQUAKE, rather than sent with a number that wraps onto some
other entity.  Only reachable at all since max_edicts can exceed 8192
(GitHub #411).
==================
*/
static qboolean SV_SoundEntityFits (int ent, qboolean *large)
{
	*large = (ent >= SND_MAX_PACKED_ENTITY);
	return !*large || sv_protocol >= PROTOCOL_HEXENWAIL_1;
}

/*
==================
SV_StopSound
==================
*/
void SV_StopSound (edict_t *entity, int channel)
{
	int			ent;
	qboolean		large;

	if (sv.datagram.cursize > MAX_DATAGRAM-4)
		return;

	ent = NUM_FOR_EDICT(entity);
	if (!SV_SoundEntityFits (ent, &large))
		return;
	if (large)
	{
		MSG_WriteByte (&sv.datagram, svc_stopsound_large);
		MSG_WriteShort (&sv.datagram, ent);
		MSG_WriteByte (&sv.datagram, channel);
		return;
	}
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
	qboolean		large;

	if (sv.datagram.cursize > MAX_DATAGRAM-4)
		return;

	ent = NUM_FOR_EDICT(entity);
	if (!SV_SoundEntityFits (ent, &large))
		return;
	if (large)
	{
		MSG_WriteByte (&sv.datagram, svc_sound_update_pos_large);
		MSG_WriteShort (&sv.datagram, ent);
		MSG_WriteByte (&sv.datagram, channel);
	}
	else
	{
		MSG_WriteByte (&sv.datagram, svc_sound_update_pos);
		MSG_WriteShort (&sv.datagram, (ent<<3) | channel);
	}
	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i]));
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
SV_MaxDatagram

Largest unreliable message the CLIENT can receive, which is not the same
question as how big a packet we can build.  Each engine sized its socket
receive buffer from its own MAX_DATAGRAM at compile time, and an oversized
UDP datagram is truncated by recvfrom rather than rejected, so the client
parses a packet whose tail is simply missing and reports the failure against
whatever message it happened to stop inside.  See protocol.h.
==================
*/
int SV_MaxDatagram (void)
{
	if (sv_protocol < PROTOCOL_UH2_114)
		return MAX_DATAGRAM_OLD;	/* 18-20: upstream uHexen2	*/
	if (sv_protocol < PROTOCOL_HEXENWAIL_1)
		return MAX_DATAGRAM_114;	/* 21: Shanjaq			*/
	return MAX_DATAGRAM;
}

/*
==================
SV_MaxReliableMessage

Largest reliable message the client can reassemble.  Worse than the datagram
case: the fragment reassembly in Datagram_GetMessage memcpys into
qsocket_t::receiveMessage[NET_MAXMESSAGE] without a bounds check -- upstream
has never had one -- so going over does not desynchronise the peer, it writes
past the end of its socket structure.  Upstream sizes that buffer at 16384;
Shanjaq at 65000, which is above our own MAX_MSGLEN, so 21 and 100 are both
bounded by what we can build rather than by what they can hold.
==================
*/
int SV_MaxReliableMessage (void)
{
	if (sv_protocol < PROTOCOL_UH2_114)
		return MAX_MSGLEN_OLD;
	return MAX_MSGLEN;
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
	qboolean		large;

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
	if (!SV_SoundEntityFits (ent, &large))
		return;

	field_mask = large ? SND_LARGEENTITY : 0;
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
	if (field_mask & SND_LARGEENTITY)
	{
		MSG_WriteShort (&sv.datagram, ent);
		MSG_WriteByte (&sv.datagram, channel);
	}
	else
		MSG_WriteShort (&sv.datagram, (ent<<3) | channel);
	MSG_WriteByte (&sv.datagram, sound_num);
	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i] + 0.5*(entity->v.mins[i]+entity->v.maxs[i]));
}
