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

/* sv_main_datagram.rs (#469 slice C) */
typedef struct sv_main_datagram_facts_s
{
	/* protocol.h's svc_* message numbers */
	int	msg_damage, msg_setangle, msg_clientdata, msg_update_inv;
	int	msg_time, msg_nop, msg_updatefrags, msg_signonnum;
	/* protocol.h's SU_* clientdata bits */
	int	su_viewheight, su_idealpitch, su_idealroll;
	int	su_punch1, su_velocity1;
	int	su_onground, su_weaponframe, su_armor, su_weapon;
	/* protocol.h's SC1_* inventory bits */
	int	sc1_health, sc1_level, sc1_intelligence, sc1_wisdom, sc1_strength;
	int	sc1_dexterity, sc1_weapon, sc1_bluemana, sc1_greenmana;
	int	sc1_experience, sc1_cnt_torch, sc1_cnt_h_boost, sc1_cnt_sh_boost;
	int	sc1_cnt_mana_boost, sc1_cnt_teleport, sc1_cnt_tome, sc1_cnt_summon;
	int	sc1_cnt_invisibility, sc1_cnt_glyph, sc1_cnt_haste, sc1_cnt_blast;
	int	sc1_cnt_polymorph, sc1_cnt_flight, sc1_cnt_cubeofforce;
	int	sc1_cnt_invincibility, sc1_artifact_active, sc1_artifact_low;
	int	sc1_movetype, sc1_cameramode, sc1_hasted, sc1_inventory;
	int	sc1_rings_active;
	/* protocol.h's SC2_* inventory bits */
	int	sc2_rings_low, sc2_amulet, sc2_bracer, sc2_breastplate, sc2_helmet;
	int	sc2_flight_t, sc2_water_t, sc2_turning_t, sc2_regen_t;
	int	sc2_haste_t, sc2_tome_t;
	int	sc2_puzzle1, sc2_puzzle2, sc2_puzzle3, sc2_puzzle4;
	int	sc2_puzzle5, sc2_puzzle6, sc2_puzzle7, sc2_puzzle8;
	int	sc2_maxhealth, sc2_maxmana, sc2_flags, sc2_obj, sc2_obj2;
	/* quakedef.h's MAX_INVENTORY_EX; server.h's FL_ONGROUND */
	int	max_inventory_ex, fl_onground;
	/* entvars_t members, relative to v */
	size_t	edict_v, entvars_size;
	size_t	v_dmg_take, v_dmg_save, v_dmg_inflictor;
	size_t	v_origin, v_mins, v_maxs;
	size_t	v_fixangle, v_angles, v_view_ofs, v_idealpitch, v_idealroll;
	size_t	v_punchangle, v_velocity;
	size_t	v_weaponframe, v_armorvalue, v_weaponmodel, v_flags;
	size_t	v_health, v_level, v_intelligence, v_wisdom, v_strength;
	size_t	v_dexterity, v_weapon, v_bluemana, v_greenmana, v_experience;
	size_t	v_cnt_torch, v_cnt_h_boost, v_cnt_sh_boost, v_cnt_mana_boost;
	size_t	v_cnt_teleport, v_cnt_tome, v_cnt_summon, v_cnt_invisibility;
	size_t	v_cnt_glyph, v_cnt_haste, v_cnt_blast, v_cnt_polymorph;
	size_t	v_cnt_flight, v_cnt_cubeofforce, v_cnt_invincibility;
	size_t	v_artifact_active, v_artifact_low, v_movetype, v_cameramode;
	size_t	v_hasted, v_inventory, v_rings_active, v_rings_low;
	size_t	v_armor_amulet, v_armor_bracer, v_armor_breastplate, v_armor_helmet;
	size_t	v_ring_flight, v_ring_water, v_ring_turning, v_ring_regeneration;
	size_t	v_haste_time, v_tome_time;
	size_t	v_puzzle_inv1, v_puzzle_inv2, v_puzzle_inv3, v_puzzle_inv4;
	size_t	v_puzzle_inv5, v_puzzle_inv6, v_puzzle_inv7, v_puzzle_inv8;
	size_t	v_max_health, v_max_mana, v_frags;
	/* client_t members */
	size_t	client_size, client_old_v, client_send_all_v, client_edict;
	size_t	client_message, client_datagram, client_netconnection;
	size_t	client_active, client_spawned, client_signon_buffer;
	size_t	client_sendsignon, client_last_message, client_dropasap;
	size_t	client_old_frags, client_info_mask, client_info_mask2;
	size_t	client_name, client_ex_inventory;
	/* ex_inventory_page_t members */
	size_t	ex_changed_items, ex_new_items, ex_item_cnt, ex_item_id, ex_next;
} sv_main_datagram_facts_t;

extern const sv_main_datagram_facts_t	SvMainDatagram_TargetFacts;

struct client_s	**SvMain_HostClient (void);	/* &host_client */
int		SvMain_MaxClients (void);	/* svs.maxclients */
struct sizebuf_s *SvMain_ReliableDatagram (void);	/* &sv.reliable_datagram */
double		SvMain_Time (void);		/* sv.time */
int		SvMain_DatagramDropped (void);	/* sv_datagram_dropped */
void		SvMain_SetDatagramDropped (int v);
unsigned int	SvMain_InfoMask (void);		/* info_mask, sv_main.c */
unsigned int	SvMain_InfoMask2 (void);	/* info_mask2, sv_main.c */
int		SvMain_MaxDatagramCvar (void);	/* sv_maxdatagram.integer */
double		SvMain_Realtime (void);		/* realtime */

#endif	/* SV_MAIN_TARGET_H */
