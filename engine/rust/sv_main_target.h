/* sv_main_target.h -- the facts engine/rust/src/sv_main_sound.rs reads from
 * Hexen II's headers and server state: the protocol constants of the sound
 * and particle messages, sv.datagram and sv.sound_precache.
 *
 * Hexen II only: HexenWorld's server has its own sv_main.c, and no
 * sv_protocol.  CMake compiles this into the targets that link the
 * sv_main_sound feature, glhexen2 and h2ded.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */

#ifndef SV_MAIN_TARGET_H
#define SV_MAIN_TARGET_H

typedef struct sv_main_target_facts_s
{
	/* protocol.h's svc_* message numbers, named msg_* here because the
	 * svc_* names are those macros */
	int	msg_particle, msg_particle2, msg_particle3, msg_particle4;
	int	msg_sound, msg_stopsound, msg_stopsound_large;
	int	msg_sound_update_pos, msg_sound_update_pos_large;
	/* svc_sound's field mask and limits */
	int	snd_volume, snd_attenuation, snd_overflow, snd_overflow2;
	int	snd_largeentity, snd_max_packed_entity, default_sound_packet_volume;
	/* quakedef.h's per-protocol limits */
	int	max_datagram, max_datagram_old, max_datagram_114;
	int	max_msglen, max_msglen_old;
	int	max_sounds, max_sounds_old, max_sounds_h2mp, max_sounds_uh2;
	/* protocol.h's versions */
	int	protocol_raven_111, protocol_uh2_114, protocol_hexenwail_1;
	/* DEFAULT_SOUND_PACKET_ATTENUATION, a double literal in the C */
	double	default_sound_packet_attenuation;
} sv_main_target_facts_t;

extern const sv_main_target_facts_t	SvMain_TargetFacts;

struct sizebuf_s	*SvMain_Datagram (void);	/* &sv.datagram */
const char		**SvMain_SoundPrecache (void);	/* sv.sound_precache */

#endif	/* SV_MAIN_TARGET_H */
