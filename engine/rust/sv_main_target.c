/* sv_main_target.c -- the facts engine/rust/src/sv_main_sound.rs needs from
 * Hexen II's headers and globals.  See sv_main_target.h.  Nothing here is
 * game logic: each value is a header constant or one global's address.
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
