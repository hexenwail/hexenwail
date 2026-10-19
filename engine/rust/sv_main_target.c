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
