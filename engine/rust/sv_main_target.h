/* sv_main_target.h -- the facts engine/rust/src/sv_main_*.rs read from
 * Hexen II's headers and server state.  sv_main_sound.rs: the protocol
 * constants of the sound and particle messages, sv.datagram and
 * sv.sound_precache.  sv_main_ents.rs: the entity-update layouts and
 * constants, the client and server state it reads, and the fat PVS buffer.
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

#include <stddef.h>

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

/* sv_main_ents.rs (#469 slice B) */
typedef struct sv_main_ents_facts_s
{
	/* entvars_t members, relative to v */
	size_t	v_effects, v_model, v_colormap, v_skin, v_drawflags, v_frame;
	size_t	v_scale, v_abslight, v_cameramode;
	/* edict_t's baseline (an entity_state_t), and client_t members */
	size_t	edict_baseline;
	size_t	client_size, client_playerclass, client_current_frame;
	size_t	client_last_frame, client_current_sequence, client_last_sequence;
	/* qmodel_t's numleafs */
	size_t	model_numleafs;
	/* the reference-frame layouts sv_main_ents.rs mirrors, checked against
	 * these by its harness: sizeof entity_state_t, entity_state2_t,
	 * client_frames_t and client_state2_t, and client_frames_t's count */
	size_t	entity_state_size, entity_state2_size, client_frames_size;
	size_t	client_state2_size, client_frames_count;
	/* header constants */
	int	max_client_states, max_frames, clear_limit, max_clear_edicts_per_msg;
	int	ent_cleared, ef_nodraw, ef_muzzleflash, fl_client, fl_class_dependent;
	int	movetype_flymissile, movetype_bouncemissile, movetype_bounce, movetype_step;
	int	contents_solid, entalpha_default, max_qpath;
	int	msg_reference, msg_clear_edicts;
	int	u_morebits, u_origin1, u_angle1, u_angle2, u_angle3, u_nolerp, u_frame;
	int	u_signal, u_model, u_clear_ent, u_longentity, u_morebits2, u_skin;
	int	u_effects, u_scale, u_colormap, u_alpha;
} sv_main_ents_facts_t;

extern const sv_main_ents_facts_t	SvMainEnts_TargetFacts;

struct edict_s		*SvMain_Edicts (void);		/* sv.edicts */
int			SvMain_NumEdicts (void);	/* sv.num_edicts */
int			SvMain_EdictSize (void);	/* pr_edict_size */
struct qmodel_s		*SvMain_WorldModel (void);	/* sv.worldmodel */
void			*SvMain_States (void);		/* sv.states */
struct client_s		*SvMain_Clients (void);		/* svs.clients */
int			SvMain_Protocol (void);		/* sv_protocol */
/* sv_update_player, _monsters, _missiles, _misc and sv_netsort .integer */
int			SvMain_UpdatePlayer (void);
int			SvMain_UpdateMonsters (void);
int			SvMain_UpdateMissiles (void);
int			SvMain_UpdateMisc (void);
int			SvMain_NetSort (void);
/* the fat PVS: C storage, MAX_MAP_LEAFS/8 bytes */
unsigned char		*SvMain_FatPvs (void);
size_t			SvMain_FatPvsSize (void);

#endif	/* SV_MAIN_TARGET_H */
