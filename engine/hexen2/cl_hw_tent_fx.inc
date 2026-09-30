/*
 * cl_hw_tent_fx.inc -- what HexenWorld's temp entities draw and play, for
 * the integrated client.  GitHub #213, part 2.
 *
 * Textually included by cl_hw_tent.inc, after the TE_* table, so that
 * scripts/hw_tent_test.c drives this code through the same stubs as the
 * reader.  Transcribed from the original HexenWorld client's cl_tent.c
 * (engine/hexenworld/client/cl_tent.c, deleted in 036a0b531): CL_ParseTEnt,
 * CL_AllocExplosion, CL_UpdateExplosions and the think functions after it.
 * Models, sounds, timings, counts and colours are the original's, and so
 * is the order of the rand() draws that feed each effect -- including the
 * ones that pick a model last (HWCL_PlaceExplosion, HWCL_TEntBurstPiece).
 * The exception: a few draws whose result the original computed and never
 * used (an ftemp in the grenade thinks, a teleport flash's avel with no
 * rotate flag) are dropped.  Where this file departs from the original in
 * anything else, the comment at that spot says why.
 *
 * Three things the original leaned on that the integrated client does not
 * have, and what stands in for them:
 *
 *  - FindState(n), which looked entity n up in the latest packet (and
 *    returned cl.simorg for the local player).  HWCL_TEntEntityOrigin below
 *    range-checks n and then asks cl_hw.c's HWCL_EntityStateOrigin, which
 *    reads hwcl_server_state -- the same latest-packet record -- and the
 *    predicted origin for the local player.
 *
 *  - Its own stream list.  Streams are drawn by Hexen II's cl_tent.c, which
 *    already renders every HW stream that arrives as a wire TE_STREAM_*;
 *    the effects that built streams directly (ice storm, hammer and chain
 *    lightning, sunstaff, cube beam) do so through CL_CreateStream.
 *
 *  - CL_NewTempEntity, which handed out entities in a visedict array of
 *    structs.  Hexen II's cl_visedicts is an array of pointers, so each
 *    explosion slot owns one entity_t here, the way cl_tent.c's streams
 *    and cl_effect.c's effects own theirs.
 *
 * Every model is looked up with Mod_ForName (..., false) on first use per
 * map, and a missing one is remembered as missing: the effect that wanted
 * it draws nothing, and nothing ever reaches Sys_Error or a NULL deref.
 * HW sounds are precached in HWCL_LoadSounds, so Hexen II single player
 * never asks for a HW-only asset.
 */

#ifndef THINGTYPE_DIRT
#define THINGTYPE_DIRT		25	/* HexenWorld only */
#endif

/* The original's MAX_EXPLOSIONS.  Fixed, with its overflow rule: when every
 * slot is live, the one that started longest ago is reused. */
#define HWCL_MAX_EXPLOSIONS	128

#define HWEX_ROTATE		1
#define HWEX_COLLIDE		2
#define HWEX_STILL_FRAME	4

/* cl_tent.c's STREAM_ATTACHED, and the stream numbers, which the two games
 * share.  HWTE_STREAM_CHAIN / _FAMINE are in the TE_* enum already. */
#define HWCL_STREAM_ATTACHED	16

/* The model skin range; 100 and up selects gfx/skin*.lmp in the GL alias
 * renderer, and above 255 it is a Sys_Error there. */
#define HWCL_MAX_MODEL_SKIN	100

#define HWCL_FRANDOM()	(rand() * (1.0 / RAND_MAX))

/* Called with each entity as it goes into cl_visedicts.  Nothing in the
 * engine; hw_tent_test.c defines it to snapshot the entity and prove that
 * nothing writes it again that frame. */
#ifndef HWCL_TENT_LINKED
#define HWCL_TENT_LINKED(ent)	((void)0)
#endif

typedef struct hwcl_explosion_s hwcl_explosion_t;

struct hwcl_explosion_s
{
	vec3_t		origin;
	vec3_t		oldorg;		/* position last frame */
	double		startTime;
	double		endTime;
	vec3_t		velocity;
	vec3_t		accel;
	vec3_t		angles;
	vec3_t		avel;		/* angular velocity */
	int		flags;		/* drawflags */
	int		abslight;
	int		exflags;	/* HWEX_* */
	int		skin;
	int		scale;
	qmodel_t	*model;		/* NULL: slot is free */
	void		(*frameFunc) (hwcl_explosion_t *ex);
	float		data;
	qboolean	drawn;		/* for the developer trace only */
};

static hwcl_explosion_t	hwcl_explosions[HWCL_MAX_EXPLOSIONS];
/* One per slot, handed to the renderer by pointer.  The invariant that
 * keeps this safe: an entity is written ONLY by HWCL_UpdateTempEntities,
 * at the moment it is linked, and each slot is visited once per frame.
 * Allocation never touches it -- with the pool full, HWCL_AllocExplosion
 * reuses a live slot, and a think function allocating mid-update would
 * otherwise zero an entity already sitting in cl_visedicts, which the sky
 * pass then dereferences (e->model->type).  A slot reused after it was
 * linked keeps drawing its old contents for the rest of that frame; the
 * new explosion first draws on the next. */
static entity_t		hwcl_explosion_ents[HWCL_MAX_EXPLOSIONS];
/* Bumped by every allocation of the slot, so the update can tell that a
 * think function reused the very slot it is running in. */
static unsigned int	hwcl_explosion_gen[HWCL_MAX_EXPLOSIONS];

/* Running totals, read by the developer trace and by hw_tent_test.c. */
static int	hwcl_tent_allocs;
static int	hwcl_tent_streams;
static int	hwcl_tent_linked;

static float	hwcl_ice_sound_time = .6;
static int	hwcl_multigrenade_channel;

/* ------------------------------------------------------------------ */
/* models                                                              */
/* ------------------------------------------------------------------ */

enum
{
	HWM_GEN_EXPL, HWM_SM_EXPLD, HWM_BG_EXPLD, HWM_FL_EXPLD,
	HWM_ARROWHIT, HWM_WHTSMK1, HWM_REDSMK1,
	HWM_SPLNTER1, HWM_SPLNTER2, HWM_SPLNTER3, HWM_SPLNTER4,
	HWM_SHARD1, HWM_SHARD2, HWM_SHARD3, HWM_SHARD4, HWM_SHARD5,
	HWM_METLCHK1, HWM_METLCHK2, HWM_METLCHK3, HWM_METLCHK4,
	HWM_FLESH1, HWM_FLESH2, HWM_FLESH3,
	HWM_SCHUNK1, HWM_SCHUNK2, HWM_SCHUNK3, HWM_SCHUNK4,
	HWM_CLSHARD1, HWM_CLSHARD2, HWM_CLSHARD3, HWM_CLSHARD4,
	HWM_LEAFCHK1, HWM_LEAFCHK2, HWM_LEAFCHK3,
	HWM_HAY1, HWM_HAY2, HWM_HAY3,
	HWM_CLTHCHK1, HWM_CLTHCHK2, HWM_CLTHCHK3,
	HWM_SHARD, HWM_TEMPMETR, HWM_SUCWP2P,
	HWM_SFLESH1, HWM_SFLESH2, HWM_SFLESH3,
	HWM_GHOST, HWM_BONESHRD, HWM_ICEHIT, HWM_HANDFX,
	HWM_TELEPORT, HWM_TELESMK2, HWM_VORPSHOK, HWM_SPARK, HWM_XPSPBLUE,
	HWM_SPARK0, HWM_BLAST, HWM_STSUNSF1, HWM_STSUNSF2, HWM_STSUNSF3,
	HWM_STSUNSF4, HWM_XPLOD29, HWM_FCIRCLE, HWM_BSPARK, HWM_SM_BLUE,
	HWM_NULL, HWM_AXPLSN_1, HWM_AXPLSN_2, HWM_AXPLSN_5,
	HWM_FIREWAL1, HWM_FIREWAL4, HWM_FIREWAL5, HWM_FLAMESTR, HWM_FBOOM,
	HWM_SUCWP1P, HWM_AXBLADE, HWM_AXTAIL, HWM_DRGNBALL, HWM_RING,
	HWM_VORPSHOT, HWM_ICESHOT1, HWM_ICESHOT2, HWM_LBALL, HWM_MUZZLE1,
	HWM_STICE, HWM_STLGHTNG, HWM_STCLRBM,
	HWM_COUNT
};

static const char *const hwcl_tent_model_names[HWM_COUNT] =
{
	"models/gen_expl.spr", "models/sm_expld.spr", "models/bg_expld.spr",
	"models/fl_expld.spr",
	"models/arrowhit.mdl", "models/whtsmk1.spr", "models/redsmk1.spr",
	"models/splnter1.mdl", "models/splnter2.mdl", "models/splnter3.mdl",
	"models/splnter4.mdl",
	"models/shard1.mdl", "models/shard2.mdl", "models/shard3.mdl",
	"models/shard4.mdl", "models/shard5.mdl",
	"models/metlchk1.mdl", "models/metlchk2.mdl", "models/metlchk3.mdl",
	"models/metlchk4.mdl",
	"models/flesh1.mdl", "models/flesh2.mdl", "models/flesh3.mdl",
	"models/schunk1.mdl", "models/schunk2.mdl", "models/schunk3.mdl",
	"models/schunk4.mdl",
	"models/clshard1.mdl", "models/clshard2.mdl", "models/clshard3.mdl",
	"models/clshard4.mdl",
	"models/leafchk1.mdl", "models/leafchk2.mdl", "models/leafchk3.mdl",
	"models/hay1.mdl", "models/hay2.mdl", "models/hay3.mdl",
	"models/clthchk1.mdl", "models/clthchk2.mdl", "models/clthchk3.mdl",
	"models/shard.mdl", "models/tempmetr.mdl", "models/sucwp2p.mdl",
	"models/sflesh1.mdl", "models/sflesh2.mdl", "models/sflesh3.mdl",
	"models/ghost.spr", "models/boneshrd.mdl", "models/icehit.spr",
	"models/handfx.mdl",
	"models/teleport.mdl", "models/telesmk2.spr", "models/vorpshok.mdl",
	"models/spark.spr", "models/xpspblue.spr",
	"models/spark0.spr", "models/blast.mdl", "models/stsunsf1.mdl",
	"models/stsunsf2.mdl", "models/stsunsf3.mdl",
	"models/stsunsf4.mdl", "models/xplod29.spr", "models/fcircle.spr",
	"models/bspark.spr", "models/sm_blue.spr",
	"models/null.spr", "models/axplsn_1.spr", "models/axplsn_2.spr",
	"models/axplsn_5.spr",
	"models/firewal1.spr", "models/firewal4.spr", "models/firewal5.spr",
	"models/flamestr.spr", "models/fboom.spr",
	"models/sucwp1p.mdl", "models/axblade.mdl", "models/axtail.mdl",
	"models/drgnball.mdl", "models/ring.mdl",
	"models/vorpshot.mdl", "models/iceshot1.mdl", "models/iceshot2.mdl",
	"models/lball.mdl", "models/muzzle1.spr",
	"models/stice.mdl", "models/stlghtng.mdl", "models/stclrbm.mdl"
};

static qmodel_t	*hwcl_tent_models[HWM_COUNT];
static qboolean	hwcl_tent_model_tried[HWM_COUNT];

/* Resolved on first use and then remembered, found or not: a missing
 * model is one filesystem search per map, not one per explosion. */
static qmodel_t *HWCL_TEntModel (int id)
{
	if (id < 0 || id >= HWM_COUNT)
		return NULL;
	if (!hwcl_tent_model_tried[id])
	{
		hwcl_tent_model_tried[id] = true;
		hwcl_tent_models[id] = Mod_ForName (hwcl_tent_model_names[id], false);
		if (!hwcl_tent_models[id])
			Con_DPrintf ("HexenWorld temp entity model %s not found; "
					"its effects will not draw\n",
					hwcl_tent_model_names[id]);
	}
	return hwcl_tent_models[id];
}

/* ------------------------------------------------------------------ */
/* sounds                                                              */
/* ------------------------------------------------------------------ */

enum
{
	HWS_RIC1, HWS_RIC2, HWS_RIC3, HWS_R_EXP3, HWS_EXPLODE,
	HWS_BONEHIT, HWS_BONEWAL, HWS_BONEPHIT, HWS_RAVENDIE, HWS_RAVENGO,
	HWS_ICEFLESH, HWS_ICEWALL, HWS_ICESHATTER, HWS_ICESTORM,
	HWS_SUNSTAFF, HWS_SUNHIT, HWS_LIGHTNING1, HWS_LIGHTNING2,
	HWS_AXEBOUNCE, HWS_FIREBALL, HWS_PURIFY2, HWS_TELEFRAG,
	HWS_BIG_GIB, HWS_GIB1, HWS_GIB2, HWS_PURIFY1_FIRE, HWS_PURIFY1_HIT,
	HWS_ACIDHIT, HWS_DROPFIZZLE, HWS_FLAMEEND,
	HWS_TELEPORT1, HWS_TELEPORT2, HWS_TELEPORT3, HWS_TELEPORT4,
	HWS_TELEPORT5,
	HWS_COUNT
};

/* The original's CL_InitTEnts list, less the ones no temp entity plays
 * (tink1 is Hexen II's, and cl_tent.c already has it; hammersound and
 * buzzbee belong to the entity-effect updaters).  Its swordExplode and
 * axeExplode were both weapons/explode.wav and are HWS_EXPLODE here. */
static const char *const hwcl_tent_sound_names[HWS_COUNT] =
{
	"weapons/ric1.wav", "weapons/ric2.wav", "weapons/ric3.wav",
	"weapons/r_exp3.wav", "weapons/explode.wav",
	"necro/bonenhit.wav", "necro/bonenwal.wav", "necro/bonephit.wav",
	"raven/death.wav", "raven/ravengo.wav",
	"crusader/icehit.wav", "crusader/icewall.wav", "misc/icestatx.wav",
	"crusader/blizzard.wav",
	"crusader/sunhum.wav", "crusader/sunhit.wav", "crusader/lghtn1.wav",
	"crusader/lghtn2.wav",
	"paladin/axric1.wav", "weapons/fbfire.wav", "weapons/exphuge.wav",
	"player/telefrag.wav",
	"player/megagib.wav", "player/gib1.wav", "player/gib2.wav",
	"paladin/purfire.wav", "weapons/expsmall.wav",
	"succubus/acidhit.wav", "succubus/dropfizz.wav", "succubus/flamend.wav",
	"misc/teleprt1.wav", "misc/teleprt2.wav", "misc/teleprt3.wav",
	"misc/teleprt4.wav", "misc/teleprt5.wav"
};

static sfx_t	*hwcl_tent_sfx[HWS_COUNT];

/* From HWCL_LoadSounds, after S_ClearPrecache: these are handles into the
 * sound table that call just emptied, exactly like cl_tent.c's own. */
static void HWCL_PrecacheTEntSounds (void)
{
	int	i;

	for (i = 0; i < HWS_COUNT; i++)
		hwcl_tent_sfx[i] = S_PrecacheSound (hwcl_tent_sound_names[i]);
}

/* The original's TempSoundChannel: cycle -1..-20 so no two temp sounds
 * share an entity number. */
static int HWCL_TempSoundChannel (void)
{
	static int	last = -1;

	last--;
	if (last < -20)
		last = -1;
	return last;
}

/* A NULL handle (not precached yet, or cleared) is a no-op in
 * S_StartSound. */
static void HWCL_TEntSoundChan (int entchannel, int id, vec3_t pos)
{
	S_StartSound (HWCL_TempSoundChannel (), entchannel, hwcl_tent_sfx[id],
			pos, 1, 1);
}

static void HWCL_TEntSound (int id, vec3_t pos)
{
	HWCL_TEntSoundChan (0, id, pos);
}

/* ------------------------------------------------------------------ */
/* pool                                                                */
/* ------------------------------------------------------------------ */

/* Level change, connect and disconnect.  Also forgets every model lookup:
 * CL_ClearState's Host_ClearMemory lets mod_known recycle sprite slots, so
 * a pointer kept across maps could name a different model.  The sound
 * handles go too; HWCL_LoadSounds takes them again. */
static void HWCL_ClearTempEntities (void)
{
	memset (hwcl_explosions, 0, sizeof(hwcl_explosions));
	memset (hwcl_explosion_ents, 0, sizeof(hwcl_explosion_ents));
	memset (hwcl_tent_models, 0, sizeof(hwcl_tent_models));
	memset (hwcl_tent_model_tried, 0, sizeof(hwcl_tent_model_tried));
	memset (hwcl_tent_sfx, 0, sizeof(hwcl_tent_sfx));
	hwcl_ice_sound_time = .6;
}

/* CL_AllocExplosion: the first free slot, else the one that started
 * longest ago -- and, if every slot starts in the future, slot 0, as the
 * original did.  The slot comes back zeroed. */
static hwcl_explosion_t *HWCL_AllocExplosion (void)
{
	int	i, idx;
	double	time;

	for (i = 0; i < HWCL_MAX_EXPLOSIONS; i++)
	{
		if (!hwcl_explosions[i].model)
			break;
	}
	if (i < HWCL_MAX_EXPLOSIONS)
		idx = i;
	else
	{
		idx = 0;
		time = cl.time;
		for (i = 0; i < HWCL_MAX_EXPLOSIONS; i++)
		{
			if (hwcl_explosions[i].startTime < time)
			{
				time = hwcl_explosions[i].startTime;
				idx = i;
			}
		}
	}

	memset (&hwcl_explosions[idx], 0, sizeof(hwcl_explosion_t));
	hwcl_explosion_gen[idx]++;
	hwcl_tent_allocs++;
	return &hwcl_explosions[idx];
}

/* Every explosion is made through this: no model, no explosion.  The
 * original set ex->model after allocating and then read
 * ex->model->numframes, which is the NULL deref this refuses to have. */
static hwcl_explosion_t *HWCL_NewExplosion (int model)
{
	qmodel_t		*mod;
	hwcl_explosion_t	*ex;

	mod = HWCL_TEntModel (model);
	if (!mod)
		return NULL;
	ex = HWCL_AllocExplosion ();
	ex->model = mod;
	return ex;
}

/* For effects the original filled in first and gave a model last, with a
 * rand() draw: build the explosion in `t', draw the model, and only then
 * take a pool slot -- so the draws keep the original's order and a
 * missing model still costs no slot. */
static hwcl_explosion_t *HWCL_PlaceExplosion (const hwcl_explosion_t *t,
		int model)
{
	qmodel_t		*mod;
	hwcl_explosion_t	*ex;

	mod = HWCL_TEntModel (model);
	if (!mod)
		return NULL;
	ex = HWCL_AllocExplosion ();
	*ex = *t;
	ex->model = mod;
	return ex;
}

/* FindState, range-checked.  Entity 0 is the world, which is never in a
 * packet, so the original never found it either. */
static qboolean HWCL_TEntEntityOrigin (int ent, vec3_t origin)
{
	if (ent < 1 || ent >= HWCL_MAX_ENTITIES)
		return false;
	return HWCL_EntityStateOrigin (ent, origin);
}

/* NewStream + fill, as the original did inline.  Model ids, -1 for none. */
/* attach_origin is where ent is now, from HWCL_TEntEntityOrigin; needed
 * only with HWCL_STREAM_ATTACHED, NULL otherwise. */
static qboolean HWCL_TEntStream (int type, int ent, int flags, int tag,
		float duration, int skin, int m0, int m1, int m2, int m3,
		const vec3_t source, const vec3_t dest, const vec3_t attach_origin)
{
	qmodel_t	*models[4];

	if (ent < 0 || ent >= HWCL_MAX_ENTITIES)
		return false;
	models[0] = HWCL_TEntModel (m0);
	models[1] = HWCL_TEntModel (m1);
	models[2] = HWCL_TEntModel (m2);
	models[3] = HWCL_TEntModel (m3);
	if (!CL_CreateStream (type, ent, flags, tag, duration, skin, models,
			source, dest, attach_origin))
		return false;
	hwcl_tent_streams++;
	return true;
}

static void HWCL_TEntVecToAngles (vec3_t vec, vec3_t ang)
{
	float	forward, yaw, pitch;

	if (vec[1] == 0 && vec[0] == 0)
	{
		yaw = 0;
		if (vec[2] > 0)
			pitch = 90;
		else	pitch = 270;
	}
	else
	{
		yaw = (int) (atan2(vec[1], vec[0]) * 180 / M_PI);
		if (yaw < 0)
			yaw += 360;
		forward = sqrt (vec[0]*vec[0] + vec[1]*vec[1]);
		pitch = (int) (atan2(vec[2], forward) * 180 / M_PI);
		if (pitch < 0)
			pitch += 360;
	}
	ang[0] = pitch;
	ang[1] = yaw;
	ang[2] = 0;
}

/* The byte-encoded direction every flying temp entity carries: angle and
 * pitch as 0..255 over a full turn (6.28, not 2*pi, as the gamecode
 * encodes it), scaled to speed. */
static void HWCL_TEntDirection (float angle, float pitch, float speed,
		vec3_t out)
{
	float	sinval, cosval;

	q_sincosrad (angle, &sinval, &cosval);
	out[0] = cosval;
	out[1] = sinval;
	q_sincosrad (pitch, &sinval, &cosval);
	out[0] *= cosval;
	out[1] *= cosval;
	out[2] = sinval;
	VectorScale (out, speed, out);
}

/* Mod_PointInLeaf Sys_Errors on a NULL model, and cl.worldmodel is NULL
 * from svc_serverdata until the model list loads -- a window a temp entity
 * can arrive in.  No world reads as solid: nothing moves into it. */
static qboolean HWCL_TEntEmpty (vec3_t p)
{
	if (!cl.worldmodel)
		return false;
	return Mod_PointInLeaf (p, cl.worldmodel)->contents == CONTENTS_EMPTY;
}

/* ------------------------------------------------------------------ */
/* think functions                                                     */
/* ------------------------------------------------------------------ */

static void HWCL_MultiGrenadePieceThink (hwcl_explosion_t *ex);
static void HWCL_MultiGrenadePiece2Think (hwcl_explosion_t *ex);

static void HWCL_MultiGrenadeExplodeSound (hwcl_explosion_t *ex)
{
	/* plug up all of -1's channels w/ grenade sounds */
	if (!(rand() & 7))
	{
		if (hwcl_multigrenade_channel >= MAX_DYNAMIC_CHANNELS ||
				hwcl_multigrenade_channel < 0)
			hwcl_multigrenade_channel = 0;
		HWCL_TEntSoundChan (hwcl_multigrenade_channel++, HWS_EXPLODE,
				ex->origin);
	}
	ex->frameFunc = NULL;
}

/* The integer divisions (rand() / RAND_MAX * 0.5 is almost always 0) are
 * the original's, kept: they are what its grenades looked like. */
static void HWCL_MultiGrenadeThink (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*missile;
	int	attack_counter, number_explosions;
	float	ftemp;

	VectorSet (ex->velocity, 0, 0, 0);
	VectorSet (ex->accel, 0, 0, 0);
	ex->frameFunc = HWCL_MultiGrenadeExplodeSound;

	attack_counter = 0;
	number_explosions = (rand() & 2) + 6;
	while (attack_counter < number_explosions)
	{
		attack_counter++;
		missile = HWCL_NewExplosion ((rand() & 1) ? HWM_GEN_EXPL : HWM_BG_EXPLD);
		if (!missile)
			continue;
		VectorCopy (ex->origin, missile->origin);

		switch (attack_counter % 3)
		{
		case 2:
			missile->frameFunc = HWCL_MultiGrenadePieceThink;
			missile->velocity[0] = (rand() % 600) - 300;
			missile->velocity[1] = (rand() % 600) - 300;
			missile->velocity[2] = 0;
			missile->startTime = cl.time;
			ftemp = (rand() / RAND_MAX * 0.3) - 0.15;
			missile->endTime = missile->startTime + missile->model->numframes * 0.1 + ftemp;
			break;
		case 1:
			missile->frameFunc = HWCL_MultiGrenadePiece2Think;
			missile->velocity[0] = (rand() % 80 ) - 40;
			missile->velocity[1] = (rand() % 80 ) - 40;
			missile->velocity[2] = (rand() % 150) + 150;
			missile->startTime = cl.time;
			ftemp = (rand() / RAND_MAX  * 0.3) - 0.15;
			missile->endTime = missile->startTime + missile->model->numframes * 0.1 + ftemp;
			break;
		default:	/* some extra explosions for at first */
			missile->frameFunc = NULL;
			missile->origin[0] += (rand() % 50) - 25;
			missile->origin[1] += (rand() % 50) - 25;
			missile->origin[2] += (rand() % 50) - 25;
			ftemp = (rand() / RAND_MAX * 0.2);
			missile->startTime = cl.time + ftemp;
			ftemp = (rand() / RAND_MAX * 0.2) - 0.1;
			missile->endTime = missile->startTime + missile->model->numframes * 0.1 + ftemp;
			break;
		}

		ftemp = (rand() / RAND_MAX * 0.2);
		missile->data = ex->data * (0.7 + ftemp);
	}
}

static void HWCL_MultiGrenadePieceThink (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*missile;
	int	attack_counter, number_explosions;
	float	ftemp;

	VectorSet (ex->velocity, 0, 0, 0);
	VectorSet (ex->accel, 0, 0, 0);
	ex->frameFunc = HWCL_MultiGrenadeExplodeSound;

	if (ex->data < 70)
		ex->data = 70;

	ftemp = (rand() / RAND_MAX * 0.2) - 0.1;
	ex->startTime = cl.time + ((1 - (ex->data - 69) / 180.0) + ftemp - 0.3) * 1.25;
	if (ex->startTime < cl.time)
		ex->startTime = cl.time;
	ftemp = (rand() / RAND_MAX * 0.4) - 0.4;
	ex->endTime = ex->startTime + ex->model->numframes * 0.1 + ftemp;

	if (ex->model->numframes > 14)
	{
		ex->startTime -= 0.4;
		ex->endTime -= 0.4;
	}

	if (ex->data <= 71)
		return;

	attack_counter = 0;
	number_explosions = (rand() & 3) + 2;
	while (attack_counter < number_explosions)
	{
		vec3_t	org;

		attack_counter += 1;
		VectorCopy (ex->origin, org);
		org[0] += (rand() % 100) - 50;
		org[1] += (rand() % 100) - 50;
		org[2] += (rand() % 10 ) - 4;
		ftemp = (rand() / RAND_MAX * 0.2);

		missile = HWCL_NewExplosion ((rand() & 7) ? HWM_BG_EXPLD : HWM_FL_EXPLD);
		if (!missile)
			continue;
		missile->frameFunc = HWCL_MultiGrenadePieceThink;
		VectorCopy (org, missile->origin);
		missile->data = ex->data * (0.7 + ftemp);
		missile->startTime = cl.time + 0.01;
		missile->endTime = missile->startTime + missile->model->numframes * 0.1;
	}
}

static void HWCL_MultiGrenadePiece2Think (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*missile;
	int	attack_counter, number_explosions;
	float	ftemp;

	VectorSet (ex->velocity, 0, 0, 0);
	VectorSet (ex->accel, 0, 0, 0);
	ex->frameFunc = HWCL_MultiGrenadeExplodeSound;

	if (ex->data < 70)
		ex->data = 70;

	ex->startTime = cl.time + (((1 - (ex->data - 69) / 200.0)) * 1.5) - 0.2;
	ftemp = (rand() / RAND_MAX * 0.4) - 0.2;
	ex->endTime = ex->startTime + ex->model->numframes * 0.1 + ftemp;

	if (ex->data <= 71)
		return;

	attack_counter = 0;
	number_explosions = (rand() % 3) + 2;
	while (attack_counter < number_explosions)
	{
		vec3_t	org;

		attack_counter += 1;
		VectorCopy (ex->origin, org);
		org[0] += (rand() % 30) - 15;
		org[1] += (rand() % 30) - 15;
		ftemp = rand() / RAND_MAX;
		org[2] += (ftemp * 7) + 30;

		missile = HWCL_NewExplosion ((rand() & 1) ? HWM_GEN_EXPL : HWM_BG_EXPLD);
		if (!missile)
			continue;
		missile->frameFunc = HWCL_MultiGrenadePiece2Think;
		VectorCopy (org, missile->origin);
		missile->data = ex->data - ftemp * 10 - 20;
		missile->startTime = cl.time + 0.01;
		missile->endTime = missile->startTime + missile->model->numframes * 0.1;
	}
}

static void HWCL_ChunkThink (hwcl_explosion_t *ex)
{
	vec3_t	oldorg;
	int	moving = 1;

	if (!HWCL_TEntEmpty (ex->origin))
	{	/* collided with world */
		VectorCopy (ex->oldorg, ex->origin);

		if ((int)ex->data == THINGTYPE_FLESH)
		{
			if (VectorNormalizeFast (ex->velocity) > 100.0)
			{	/* hit, now make a splash of blood */
				vec3_t	dmin = {-40, -40, 10};
				vec3_t	dmax = {40, 40, 40};
				R_RunParticleEffect2 (ex->origin, dmin, dmax,
						136 + (rand() % 5), pt_darken, 20);
			}
		}
		else if ((int)ex->data == THINGTYPE_ACID)
		{
			if (VectorNormalizeFast (ex->velocity) > 100.0)
			{
				if (!(rand() % 3))
					HWCL_TEntSound (HWS_DROPFIZZLE, ex->origin);
			}
		}

		VectorClear (ex->velocity);
		VectorClear (ex->avel);
		moving = 0;
	}

	if (cl.time + host_frametime * 5 > ex->endTime)
	{	/* chunk leaves in 5 frames about */
		switch ((int)ex->data)
		{
		case THINGTYPE_METEOR:
			if (cl.time + host_frametime * 4 < ex->endTime)
				ex->abslight = 200;	/* just crossed the threshold */
			else	ex->abslight -= 35;
			ex->flags |= DRF_TRANSLUCENT|MLS_ABSLIGHT;
			ex->scale *= 1.4;
			ex->angles[0] += 20;
			break;
		default:
			ex->scale *= .8;
			break;
		}
	}

	ex->velocity[2] -= host_frametime * movevars.gravity;

	/* The original passed HW trail numbers 16 and 1 raw; in HW's numbering
	 * those were grenade smoke and smoke.  Hexen II numbers its trails
	 * differently, so they are named here. */
	switch ((int)ex->data)
	{
	case THINGTYPE_FLESH:
		if (moving)
			R_RocketTrail (ex->oldorg, ex->origin, rt_blood);
		break;
	case THINGTYPE_ICE:
		ex->velocity[2] += host_frametime * movevars.gravity * 0.5;	/* lower gravity for ice */
		if (moving)
			R_RocketTrail (ex->oldorg, ex->origin, rt_ice);
		break;
	case THINGTYPE_ACID:
	case THINGTYPE_GREENFLESH:
		if (moving)
			R_RocketTrail (ex->oldorg, ex->origin, rt_grensmoke);
		break;
	case THINGTYPE_METEOR:
		VectorCopy (ex->oldorg, oldorg);
		if (!moving)
		{	/* resting meteors still give off smoke */
			oldorg[0] += (rand() % 7) - 3;
			oldorg[1] += (rand() % 7) - 3;
			oldorg[2] += 16;
		}
		R_RocketTrail (oldorg, ex->origin, rt_smoke);
		break;
	}
}

static void HWCL_MissileFlashThink (hwcl_explosion_t *ex)
{
	ex->abslight -= (0.05*256);
	ex->scale += 5;
	if (ex->abslight < (0.05*256))
		ex->endTime = ex->startTime;	/* remove */
}

static void HWCL_TeleportFlashThink (hwcl_explosion_t *ex)
{
	ex->scale -= 15;
	if (ex->scale < 10)
		ex->endTime = ex->startTime;
}

/* remove tent if not in open air */
static void HWCL_CheckSpaceThink (hwcl_explosion_t *ex)
{
	if (!HWCL_TEntEmpty (ex->origin))
		ex->endTime = ex->startTime;
}

static void HWCL_SwordFrameFunc (hwcl_explosion_t *ex)
{
	ex->scale = (ex->endTime - cl.time)*150 + 1;
	if (((int)(cl.time * 20.0)) % 2)
		ex->skin = 0;
	else	ex->skin = 1;
}

static void HWCL_ZapFrameFunc (hwcl_explosion_t *ex)
{
	ex->scale = (ex->endTime - cl.time)*(150/.3) + 1;
	if (((int)(cl.time * 20.0)) % 2)
		ex->skin = 0;
	else	ex->skin = 1;
}

static void HWCL_FireBallUpdate (hwcl_explosion_t *ex)
{
	ex->scale = (int)(((cl.time - ex->startTime) / 1.0) * 250) + 1;
}

static void HWCL_SunBallUpdate (hwcl_explosion_t *ex)
{
	ex->scale = 121 - (int)(((cl.time - ex->startTime) / .8) * 120);
}

static void HWCL_MeteorBlastThink (hwcl_explosion_t *ex)
{
	int	i, maxI;
	hwcl_explosion_t	*ex2;
	vec3_t	tempVect, oldPos;
	int	hitWall = 0;

	R_RocketTrail (ex->oldorg, ex->origin, rt_fireball);

	ex->data -= 1600 * host_frametime;	/* decrease distance, roughly... */
	VectorClear (oldPos);

	if (ex->data <= 0)
	{	/* ran out of juice */
		VectorCopy (ex->origin, oldPos);
		hitWall = 1;
	}
	else
	{
		VectorCopy (ex->oldorg, tempVect);
		for (i = 0; (i < 10) && (!hitWall); i++)
		{
			VectorCopy (tempVect, oldPos);
			VectorScale (ex->origin, .1 * (i+1), tempVect);
			VectorMA (tempVect, 1.0 - (.1 * (i+1)), ex->oldorg, tempVect);
			if (!HWCL_TEntEmpty (tempVect))
				hitWall = 1;
		}
	}

	if (!hitWall)
		return;

	/* collided with world */
	VectorCopy (oldPos, ex->origin);

	maxI = (host_frametime <= 0.05) ? 12 : 5;
	for (i = 0; i < maxI; i++)
	{
		static const int	models[4] = {
			HWM_SM_EXPLD, HWM_SM_EXPLD, HWM_BG_EXPLD, HWM_GEN_EXPL
		};
		vec3_t	org;

		VectorCopy (ex->origin, org);
		org[0] += (rand() % 10) - 5;
		org[1] += (rand() % 10) - 5;
		org[2] += (rand() % 10);

		ex2 = HWCL_NewExplosion (models[rand() % 4]);
		if (!ex2)
			continue;
		VectorCopy (org, ex2->origin);
		ex2->velocity[0] = (ex2->origin[0] - ex->origin[0])*12;
		ex2->velocity[1] = (ex2->origin[1] - ex->origin[1])*12;
		ex2->velocity[2] = (ex2->origin[2] - ex->origin[2])*12;
		ex2->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
		ex2->abslight = 160 + (rand() % 64);
		ex2->skin = 0;
		ex2->scale = 80 + (rand() % 40);
		ex2->startTime = cl.time + ((rand() % 30) / 200.0);
		ex2->endTime = ex2->startTime + ex2->model->numframes * 0.03;
	}
	if (rand() & 1)
		HWCL_TEntSound (HWS_EXPLODE, ex->origin);

	ex->model = NULL;
	ex->endTime = cl.time;
}

static void HWCL_MeteorCrushSpawnThink (hwcl_explosion_t *ex)
{
	float	chance;
	hwcl_explosion_t	*ex2;

	if (host_frametime <= 0.05)
		chance = (host_frametime / .4 * 16);
	else	chance = (host_frametime / .4 * 8);

	while (chance > 0)
	{
		if (chance < 1.0)
		{	/* take care of fractional numbers of meteors... */
			if (rand() % 100 > chance * 100)
				return;
		}

		ex2 = HWCL_NewExplosion (HWM_TEMPMETR);
		if (!ex2)
			return;
		VectorCopy (ex->origin, ex2->origin);
		ex2->origin[0] += (rand() % 160) - 80;
		ex2->origin[1] += (rand() % 160) - 80;
		ex2->startTime = cl.time;
		ex2->endTime = ex2->startTime + 2.0;
		ex2->frameFunc = HWCL_MeteorBlastThink;
		ex2->exflags = HWEX_ROTATE;
		ex2->avel[0] = 800;
		ex2->avel[1] = 800;
		ex2->avel[2] = 800;
		ex2->velocity[0] = (rand() % 180) - 90;
		ex2->velocity[1] = (rand() % 180) - 90;
		ex2->velocity[2] = -1600 - (rand() % 200);
		ex2->data = ex->data;	/* how far the thingy can travel max */
		ex2->scale = 200 + (rand() % 120);

		chance -= 1.0;
	}
}

static void HWCL_UpdateBloodRain (hwcl_explosion_t *ex)
{
	R_RocketTrail (ex->oldorg, ex->origin, rt_blood);

	ex->scale -= host_frametime / .3 * 60;
	if (ex->scale <= 0)
		ex->endTime = cl.time - 1;
}

static void HWCL_SmokeRingFrameFunc (hwcl_explosion_t *ex)
{
	if (cl.time - ex->startTime < .3)
		ex->skin = 0;
	else if (cl.time - ex->startTime < .6)
		ex->skin = 1;
	else if (cl.time - ex->startTime < .9)
		ex->skin = 2;
	else
		ex->skin = 3;
}

static void HWCL_UpdatePurify2 (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*ex2;
	int	numSprites;

	R_RocketTrail (ex->oldorg, ex->origin, rt_purify);

	if (host_frametime <= 0.05)
		numSprites = 20;
	else	numSprites = 8;

	if ((rand() % 100) / 100.0 < numSprites * host_frametime)
	{
		ex2 = HWCL_NewExplosion (HWM_RING);
		if (!ex2)
			return;
		VectorCopy (ex->origin, ex2->origin);
		ex2->startTime = cl.time;
		ex2->endTime = ex2->startTime + 1.2;
		ex2->scale = 150;
		VectorCopy (ex->angles, ex2->angles);
		ex2->angles[2] += 90;
		ex2->exflags |= HWEX_STILL_FRAME;
		ex2->data = 0;
		ex2->skin = 0;
		ex2->frameFunc = HWCL_SmokeRingFrameFunc;
		ex2->velocity[2] = 15.0;
		ex2->flags |= DRF_TRANSLUCENT | SCALE_ORIGIN_CENTER;
	}
}

static void HWCL_UpdateSwordShot (hwcl_explosion_t *ex)
{
	R_RocketTrail (ex->oldorg, ex->origin, rt_vorpal);

	ex->data = 16 + ((int)(cl.time * 20.0) % 13);
	ex->flags |= MLS_ABSLIGHT;
	ex->abslight = 128;
	if (((int)(cl.time * 20.0)) % 2)
		ex->skin = 0;
	else	ex->skin = 1;
}

static void HWCL_UpdateIceShot (hwcl_explosion_t *ex)
{
	R_RocketTrail (ex->oldorg, ex->origin, rt_ice);
}

static void HWCL_UpdateMeteor (hwcl_explosion_t *ex)
{
	R_RocketTrail (ex->oldorg, ex->origin, rt_smoke);
}

static void HWCL_UpdateAcidBall (hwcl_explosion_t *ex)
{
	R_RocketTrail (ex->oldorg, ex->origin, rt_acidball);
}

static void HWCL_UpdateAcidBlob (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*ex2;
	int	testVal, testVal2;

	R_RocketTrail (ex->oldorg, ex->origin, rt_acidball);

	testVal = (int)(cl.time * 10.0);
	testVal2 = (int)((cl.time - host_frametime)*10.0);
	if (testVal != testVal2 && !(testVal % 2))
	{
		ex2 = HWCL_NewExplosion (HWM_MUZZLE1);
		if (!ex2)
			return;
		VectorCopy (ex->origin, ex2->origin);
		ex2->startTime = cl.time;
		ex2->endTime = ex2->startTime + .4;
		VectorCopy (ex->angles, ex2->angles);
		ex2->angles[2] += 90;
		ex2->skin = 0;
		ex2->flags |= DRF_TRANSLUCENT | SCALE_ORIGIN_CENTER;
	}
}

static void HWCL_TelPuffMove (hwcl_explosion_t *ex)
{
	vec3_t	tvec, tvec2;

	/* angles holds the centre of the swirl, not an orientation */
	VectorSubtract (ex->angles, ex->origin, tvec);
	VectorCopy (tvec, tvec2);
	VectorNormalizeFast (tvec);
	VectorScale (tvec, 320, tvec);

	ex->velocity[0] = tvec[1];
	ex->velocity[1] = -tvec[0];
	ex->velocity[2] += HWCL_FRANDOM();
	if (ex->velocity[2] > 40)
		ex->velocity[2] = 30;

	/* keep it from getting too spread out */
	ex->velocity[0] += tvec2[0];
	ex->velocity[1] += tvec2[1];

	R_RocketTrail (ex->oldorg, ex->origin, rt_magicmissile);
}

static void HWCL_TelEffectUpdate (hwcl_explosion_t *ex)
{
	hwcl_explosion_t	*ex2;
	float	angle;
	int	testVal, testVal2;
	vec3_t	tvec;

	if (ex->endTime - cl.time <= 1.2)
		ex->frameFunc = NULL;

	testVal = (int)(cl.time * 10.0);
	testVal2 = (int)((cl.time - host_frametime)*10.0);
	if (testVal != testVal2 && !(testVal % 3))
	{
		ex2 = HWCL_NewExplosion (HWM_SM_BLUE);
		if (ex2)
		{
			angle = HWCL_FRANDOM() * 3.14159;
			VectorCopy (ex->origin, ex2->origin);
			VectorCopy (ex->origin, ex2->angles);
			q_sincosrad (angle, &tvec[1], &tvec[0]);
			ex2->origin[0] += tvec[0]*10;
			ex2->origin[1] += tvec[1]*10;

			VectorSubtract (ex->origin, ex2->origin, tvec);
			VectorScale (tvec, 20, tvec);
			ex2->startTime = cl.time;
			ex2->endTime = ex2->startTime + 3.2;
			ex2->scale = 100;
			ex2->data = 0;
			ex2->skin = 0;
			ex2->frameFunc = HWCL_TelPuffMove;
			ex2->velocity[0] = tvec[1];
			ex2->velocity[1] = -tvec[0];
			ex2->velocity[2] = 20.0;
			ex2->flags |= MLS_ABSLIGHT;
			ex2->abslight = 128;
		}
	}

	if (ex->endTime > cl.time + 0.01)
		ex->startTime = cl.time + 0.01;
}

/* ------------------------------------------------------------------ */
/* effects                                                             */
/* ------------------------------------------------------------------ */

/* THINGTYPE -> chunk model, with the skin and drawflags some of them
 * carry.  `final' is the original's (rand() % 100) * .01. */
static int HWCL_ChunkModel (int chType, float final, int *skin, int *flags)
{
	switch (chType)
	{
	case THINGTYPE_GLASS:
	case THINGTYPE_REDGLASS:
	case THINGTYPE_CLEARGLASS:
	case THINGTYPE_WEBS:
		if (chType == THINGTYPE_CLEARGLASS)
		{
			*skin = 1;
			*flags |= DRF_TRANSLUCENT;
		}
		else if (chType == THINGTYPE_REDGLASS)
			*skin = 2;
		else if (chType == THINGTYPE_WEBS)
		{
			*skin = 3;
			*flags |= DRF_TRANSLUCENT;
		}
		if (final < 0.20)	return HWM_SHARD1;
		if (final < 0.40)	return HWM_SHARD2;
		if (final < 0.60)	return HWM_SHARD3;
		if (final < 0.80)	return HWM_SHARD4;
		return HWM_SHARD5;
	case THINGTYPE_WOOD:
		if (final < 0.25)	return HWM_SPLNTER1;
		if (final < 0.50)	return HWM_SPLNTER2;
		if (final < 0.75)	return HWM_SPLNTER3;
		return HWM_SPLNTER4;
	case THINGTYPE_METAL:
		if (final < 0.25)	return HWM_METLCHK1;
		if (final < 0.50)	return HWM_METLCHK2;
		if (final < 0.75)	return HWM_METLCHK3;
		return HWM_METLCHK4;
	case THINGTYPE_FLESH:
		if (final < 0.33)	return HWM_FLESH1;
		if (final < 0.66)	return HWM_FLESH2;
		return HWM_FLESH3;
	case THINGTYPE_BROWNSTONE:
	case THINGTYPE_DIRT:
		*skin = 1;
		if (final < 0.25)	return HWM_SCHUNK1;
		if (final < 0.50)	return HWM_SCHUNK2;
		if (final < 0.75)	return HWM_SCHUNK3;
		return HWM_SCHUNK4;
	case THINGTYPE_CLAY:
		if (final < 0.25)	return HWM_CLSHARD1;
		if (final < 0.50)	return HWM_CLSHARD2;
		if (final < 0.75)	return HWM_CLSHARD3;
		return HWM_CLSHARD4;
	case THINGTYPE_LEAVES:
		if (final < 0.33)	return HWM_LEAFCHK1;
		if (final < 0.66)	return HWM_LEAFCHK2;
		return HWM_LEAFCHK3;
	case THINGTYPE_HAY:
		if (final < 0.33)	return HWM_HAY1;
		if (final < 0.66)	return HWM_HAY2;
		return HWM_HAY3;
	case THINGTYPE_CLOTH:
		if (final < 0.33)	return HWM_CLTHCHK1;
		if (final < 0.66)	return HWM_CLTHCHK2;
		return HWM_CLTHCHK3;
	case THINGTYPE_WOOD_LEAF:
		if (final < 0.14)	return HWM_SPLNTER1;
		if (final < 0.28)	return HWM_LEAFCHK1;
		if (final < 0.42)	return HWM_SPLNTER2;
		if (final < 0.56)	return HWM_LEAFCHK2;
		if (final < 0.70)	return HWM_SPLNTER3;
		if (final < 0.84)	return HWM_LEAFCHK3;
		return HWM_SPLNTER4;
	case THINGTYPE_WOOD_METAL:
		if (final < 0.125)	return HWM_SPLNTER1;
		if (final < 0.25)	return HWM_METLCHK1;
		if (final < 0.375)	return HWM_SPLNTER2;
		if (final < 0.50)	return HWM_METLCHK2;
		if (final < 0.625)	return HWM_SPLNTER3;
		if (final < 0.75)	return HWM_METLCHK3;
		if (final < 0.875)	return HWM_SPLNTER4;
		return HWM_METLCHK4;
	case THINGTYPE_WOOD_STONE:
		if (final < 0.125)	return HWM_SPLNTER1;
		if (final < 0.25)	return HWM_SCHUNK1;
		if (final < 0.375)	return HWM_SPLNTER2;
		if (final < 0.50)	return HWM_SCHUNK2;
		if (final < 0.625)	return HWM_SPLNTER3;
		if (final < 0.75)	return HWM_SCHUNK3;
		if (final < 0.875)	return HWM_SPLNTER4;
		return HWM_SCHUNK4;
	case THINGTYPE_METAL_STONE:
		if (final < 0.125)	return HWM_METLCHK1;
		if (final < 0.25)	return HWM_SCHUNK1;
		if (final < 0.375)	return HWM_METLCHK2;
		if (final < 0.50)	return HWM_SCHUNK2;
		if (final < 0.625)	return HWM_METLCHK3;
		if (final < 0.75)	return HWM_SCHUNK3;
		if (final < 0.875)	return HWM_METLCHK4;
		return HWM_SCHUNK4;
	case THINGTYPE_METAL_CLOTH:
		if (final < 0.14)	return HWM_METLCHK1;
		if (final < 0.28)	return HWM_CLTHCHK1;
		if (final < 0.42)	return HWM_METLCHK2;
		if (final < 0.56)	return HWM_CLTHCHK2;
		if (final < 0.70)	return HWM_METLCHK3;
		if (final < 0.84)	return HWM_CLTHCHK3;
		return HWM_METLCHK4;
	case THINGTYPE_ICE:
		*flags |= DRF_TRANSLUCENT|MLS_ABSLIGHT;
		return HWM_SHARD;
	case THINGTYPE_METEOR:
		return HWM_TEMPMETR;
	case THINGTYPE_ACID:
		return HWM_SUCWP2P;
	case THINGTYPE_GREENFLESH:	/* spider guts */
		if (final < 0.33)	return HWM_SFLESH1;
		if (final < 0.66)	return HWM_SFLESH2;
		return HWM_SFLESH3;
	default:	/* THINGTYPE_GREYSTONE */
		if (final < 0.25)	return HWM_SCHUNK1;
		if (final < 0.50)	return HWM_SCHUNK2;
		if (final < 0.75)	return HWM_SCHUNK3;
		return HWM_SCHUNK4;
	}
}

/* TE_CHUNK (directed) and TE_CHUNK2 (volume based).  For CHUNK2 `vel' is
 * the broken object's size and `pos' its absmin. */
static void HWCL_TEntChunks (int type, vec3_t pos, vec3_t vel, int chType,
		int cnt)
{
	float	scale = 1.0, volume;
	int	i;

	if (type == HWTE_CHUNK2)
	{
		volume = vel[0] * vel[1] * vel[2];
		cnt = volume / 8192;
		if (volume < 5000)
		{
			scale = .20;
			cnt *= 3;	/* because so few pieces come out of a small object */
		}
		else if (volume < 50000)
		{
			scale = .45;
			cnt *= 3;
		}
		else if (volume < 500000)
			scale = .50;
		else if (volume < 1000000)
			scale = .75;
		else
			scale = 1;
		if (cnt > 30)
			cnt = 30;
	}

	for (i = 0; i < cnt; i++)
	{
		hwcl_explosion_t	t, *ex = &t;
		int	skin = 0, flags = 0, model;

		memset (&t, 0, sizeof(t));
		ex->frameFunc = HWCL_ChunkThink;

		if (type == HWTE_CHUNK)
		{
			VectorCopy (pos, ex->origin);
			VectorCopy (vel, ex->velocity);
			VectorScale (ex->velocity, .80 + ((rand() % 4) / 10.0), ex->velocity);
			ex->velocity[0] += (rand() % 140) - 70;
			ex->velocity[1] += (rand() % 140) - 70;
			ex->velocity[2] += (rand() % 140) - 70;
			ex->angles[0] = rand() % 360;
			ex->angles[1] = rand() % 360;
			ex->angles[2] = rand() % 360;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = (rand() % 850) - 425;
			ex->avel[1] = (rand() % 850) - 425;
			ex->avel[2] = (rand() % 850) - 425;
			ex->scale = 30 + 100 * (cnt / 40.0) + (rand() % 40);
		}
		else
		{
			/* origin is really absmin here */
			VectorCopy (pos, ex->origin);
			ex->origin[0] += HWCL_FRANDOM() * vel[0];
			ex->origin[1] += HWCL_FRANDOM() * vel[1];
			ex->origin[2] += HWCL_FRANDOM() * vel[2];
			ex->velocity[0] = -210 + HWCL_FRANDOM() * 420;
			ex->velocity[1] = -210 + HWCL_FRANDOM() * 420;
			ex->velocity[2] = -210 + HWCL_FRANDOM() * 490;
			ex->scale = scale*100;
			ex->angles[0] = rand() % 360;
			ex->angles[1] = rand() % 360;
			ex->angles[2] = rand() % 360;
			/* no HWEX_ROTATE for volume chunks: the original set avel
			 * without it, so they never spun */
			ex->avel[0] = rand() % 1200;
			ex->avel[1] = rand() % 1200;
			ex->avel[2] = rand() % 1200;
		}
		ex->data = chType;

		model = HWCL_ChunkModel (chType, (rand() % 100) * .01, &skin, &flags);
		ex = HWCL_PlaceExplosion (&t, model);
		if (!ex)
			continue;
		ex->skin = skin;
		ex->flags |= flags;
		if (chType == THINGTYPE_METEOR)
			VectorScale (ex->avel, 4.0, ex->avel);

		ex->startTime = cl.time;
		ex->endTime = ex->startTime + 4.0;
	}
}

/* The "spray of sprites around pos" explosions (METEORHIT's blast,
 * AXE_EXPLODE, TIME_BOMB, PURIFY2_EXPLODE, ACIDBALL, ACIDBLOB) share this
 * placement: jitter the origin by +-jitter, fly outward at spread times
 * the jitter.  The model is then drawn from models[] -- after the jitter,
 * as the original drew it -- unless there is only one; *pick says which. */
static hwcl_explosion_t *HWCL_TEntBurstPiece (const int *models, int count,
		vec3_t pos, int jitter, float spread, int *pick)
{
	hwcl_explosion_t	*ex;
	vec3_t	org;
	int	j, n;

	VectorCopy (pos, org);
	for (j = 0; j < 3; j++)
		org[j] += (rand() % (jitter * 2)) - jitter;
	n = (count > 1) ? rand() % count : 0;
	if (pick)
		*pick = n;
	ex = HWCL_NewExplosion (models[n]);
	if (!ex)
		return NULL;
	VectorCopy (org, ex->origin);
	for (j = 0; j < 3; j++)
		ex->velocity[j] = (ex->origin[j] - pos[j]) * spread;
	return ex;
}

/* Everything with a fixed payload.  Reads exactly the fields the original
 * CL_ParseTEnt read, in its order, and draws only when the read was good.
 * The caller holds this to hwte_fixed_size[] byte for byte. */
static void HWCL_TEntEffect (int type)
{
	hwcl_explosion_t	*ex;
	dlight_t	*dl;
	vec3_t	pos, vel, org;
	float	sinval, cosval;
	int	i, cnt, cnt2, rnd, chType, damage, ent;

	switch (type)
	{
	case HWTE_DRILLA_EXPLODE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_GEN_EXPL);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;

	case HWTE_BIGGRENADE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		R_RunParticleEffect (pos, pos, 225, 1024);
		ex = HWCL_NewExplosion (HWM_SM_EXPLD);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->frameFunc = HWCL_MultiGrenadeThink;
			ex->data = 250;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		return;

	case HWTE_CHUNK:
	case HWTE_CHUNK2:
		HWCL_ReadCoords (pos);
		HWCL_ReadCoords (vel);	/* vel for CHUNK, volume for CHUNK2 */
		chType = MSG_ReadByte ();
		cnt = (type == HWTE_CHUNK) ? MSG_ReadByte () : 0;
		if (msg_badread)
			return;
		HWCL_TEntChunks (type, pos, vel, chType, cnt);
		return;

	case HWTE_XBOWHIT:
		HWCL_ReadCoords (pos);
		HWCL_ReadCoords (vel);
		chType = MSG_ReadByte ();	/* impact sounds were commented out */
		damage = MSG_ReadByte ();
		if (msg_badread)
			return;
		(void)chType;
		/* generic spinny impact image */
		ex = HWCL_NewExplosion (HWM_ARROWHIT);
		if (ex)
		{
			VectorSubtract (pos, vel, ex->origin);
			HWCL_TEntVecToAngles (vel, ex->angles);
			ex->avel[2] = (rand() % 500) + 200;
			ex->scale = 10;
			ex->startTime = cl.time;
			ex->endTime = cl.time + 0.3;
			ex->exflags = HWEX_ROTATE;
			ex->flags = DRF_TRANSLUCENT | MLS_ABSLIGHT;
			ex->abslight = 128;
		}
		/* white smoke if invulnerable impact */
		if (!damage && (ex = HWCL_NewExplosion (HWM_WHTSMK1)) != NULL)
		{
			for (i = 0; i < 3; i++)
				ex->origin[i] = pos[i] - vel[i]*2;
			ex->velocity[2] = 80.0;
			HWCL_TEntVecToAngles (vel, ex->angles);
			ex->startTime = cl.time;
			ex->endTime = cl.time + 0.35;
			ex->flags = DRF_TRANSLUCENT;
		}
		return;

	case HWTE_METEORHIT:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		/* always make 8 meteors */
		i = (host_frametime < .07) ? 0 : 4;	/* based on framerate */
		for ( ; i < 8; i++)
		{
			ex = HWCL_NewExplosion (HWM_TEMPMETR);
			if (!ex)
				break;
			VectorCopy (pos, ex->origin);
			ex->frameFunc = HWCL_ChunkThink;
			ex->velocity[0] += (rand() % 400) - 200;
			ex->velocity[1] += (rand() % 400) - 200;
			ex->velocity[2] += (rand() % 200) + 150;
			ex->angles[0] = rand() % 360;
			ex->angles[1] = rand() % 360;
			ex->angles[2] = rand() % 360;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = (rand() % 850) - 425;
			ex->avel[1] = (rand() % 850) - 425;
			ex->avel[2] = (rand() % 850) - 425;
			ex->scale = 45 + (rand() % 10);
			ex->data = THINGTYPE_METEOR;
			ex->skin = 0;
			VectorScale (ex->avel, 4.0, ex->avel);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 4.0;
		}
		/* make the actual explosion */
		i = (host_frametime < .07) ? 0 : 8;
		for ( ; i < 11; i++)
		{
			static const int	models[4] = {
				HWM_SM_EXPLD, HWM_SM_EXPLD, HWM_BG_EXPLD, HWM_GEN_EXPL
			};
			ex = HWCL_TEntBurstPiece (models, 4, pos, 5, 12, NULL);
			if (!ex)
				continue;
			if (host_frametime < .07)
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 160 + (rand() % 64);
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.04;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;

	case HWTE_HWBONEPOWER:
	{
		vec3_t	movedir, offset;

		cnt = MSG_ReadByte ();
		HWCL_ReadCoords (pos);
		HWCL_ReadCoords (movedir);
		if (msg_badread)
			return;
		R_RunParticleEffect4 (pos, 50, 368 + (rand() % 16), pt_grav, 10);

		ex = HWCL_NewExplosion (HWM_SM_EXPLD);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			VectorMA (ex->origin, -6, movedir, ex->origin);
			ex->data = 250;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		for (cnt2 = 0; cnt2 < cnt; cnt2++)
		{
			offset[0] = (rand() % 40) - 20;
			offset[1] = (rand() % 40) - 20;
			offset[2] = (rand() % 40) - 20;
			ex = HWCL_NewExplosion (HWM_GHOST);
			if (!ex)
				break;
			VectorAdd (pos, offset, ex->origin);
			VectorMA (ex->origin, -8, movedir, ex->origin);
			VectorCopy (offset, ex->velocity);
			ex->velocity[2] += 30;
			ex->data = 250;
			ex->abslight = 128;
			ex->flags = DRF_TRANSLUCENT | MLS_ABSLIGHT;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		for (cnt2 = 0; cnt2 < 20; cnt2++)
		{
			/* want faster velocity to hide the fact that these
			 * aren't in the real location */
			offset[0] = (rand() % 400) + 300;
			if (rand() % 2)
				offset[0] = -offset[0];
			offset[1] = (rand() % 400) + 300;
			if (rand() % 2)
				offset[1] = -offset[1];
			offset[2] = (rand() % 400) + 300;
			if (rand() % 2)
				offset[2] = -offset[2];
			ex = HWCL_NewExplosion (HWM_BONESHRD);
			if (!ex)
				break;
			/* the original's VectorMA (pos, 1/700, ...) is integer 0 */
			VectorCopy (pos, ex->origin);
			VectorCopy (offset, ex->velocity);
			ex->data = 250;
			ex->startTime = cl.time;
			/* The original's (rand() * 50) / 100 is rand()/2 seconds --
			 * hours, and signed overflow where RAND_MAX is 2^31-1.  The
			 * shards die on the first wall anyway (CheckSpaceThink); half
			 * a second at most is the evident intent. */
			ex->endTime = ex->startTime + (rand() % 50) / 100.0;
			/* Faithfully into drawflags, not exflags, as the original
			 * did: HW drew these shards unspun with MLS style 3. */
			ex->flags |= HWEX_ROTATE|HWEX_COLLIDE;
			ex->angles[0] = rand() % 700;
			ex->angles[1] = rand() % 700;
			ex->angles[2] = rand() % 700;
			ex->avel[0] = rand() % 700;
			ex->avel[1] = rand() % 700;
			ex->avel[2] = rand() % 700;
			ex->frameFunc = HWCL_CheckSpaceThink;
		}
		HWCL_TEntSoundChan (1, HWS_BONEPHIT, pos);
		return;
	}

	case HWTE_HWBONEPOWER2:
		cnt2 = MSG_ReadByte ();		/* did it hit? changes sound */
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_WHTSMK1);	/* white smoke */
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		HWCL_TEntSoundChan (1, cnt2 ? HWS_BONEHIT : HWS_BONEWAL, pos);
		R_RunParticleEffect4 (pos, 3, 368 + (rand() % 16), pt_grav, 7);
		return;

	case HWTE_HWRAVENDIE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		for (i = 0; i < 3; i++)
		{
			ex = HWCL_NewExplosion (i == 1 ? HWM_REDSMK1 : HWM_WHTSMK1);
			if (!ex)
				continue;
			VectorCopy (pos, ex->origin);
			ex->velocity[1] = (i == 0) ? 8 : (i == 2) ? -8 : 0;
			ex->velocity[2] = -10;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + HX_FRAME_TIME * 10;
		}
		HWCL_TEntSoundChan (1, HWS_RAVENDIE, pos);
		return;

	case HWTE_HWRAVENEXPLODE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		for (i = 0; i < 3; i++)
		{
			ex = HWCL_NewExplosion (HWM_WHTSMK1);
			if (!ex)
				break;
			VectorCopy (pos, ex->origin);
			ex->origin[2] -= 5 * i;
			ex->velocity[2] = 8;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + HX_FRAME_TIME * 10;
		}
		HWCL_TEntSoundChan (1, HWS_RAVENGO, pos);
		return;

	case HWTE_ICEHIT:
		HWCL_ReadCoords (pos);
		cnt2 = MSG_ReadByte ();		/* 0 for person, 1 for wall, 2 shatter */
		if (msg_badread)
			return;
		if (cnt2)
		{
			i = (host_frametime < .07) ? 0 : 5;	/* based on framerate */
			for ( ; i < 9; i++)
			{
				ex = HWCL_NewExplosion (HWM_SHARD);
				if (!ex)
					break;
				VectorCopy (pos, ex->origin);
				ex->frameFunc = HWCL_ChunkThink;
				ex->velocity[0] += (rand() % 1000) - 500;
				ex->velocity[1] += (rand() % 1000) - 500;
				ex->velocity[2] += (rand() % 200 ) - 50;
				ex->angles[0] = rand() % 360;
				ex->angles[1] = rand() % 360;
				ex->angles[2] = rand() % 360;
				ex->exflags = HWEX_ROTATE;
				ex->avel[0] = (rand() % 850) - 425;
				ex->avel[1] = (rand() % 850) - 425;
				ex->avel[2] = (rand() % 850) - 425;
				if (cnt2 == 2)
					ex->scale = 65 + (rand() % 10);
				else	ex->scale = 35 + (rand() % 10);
				ex->data = THINGTYPE_ICE;
				ex->skin = 0;
				ex->flags |= DRF_TRANSLUCENT|MLS_ABSLIGHT;
				ex->abslight = 128;
				ex->startTime = cl.time;
				ex->endTime = ex->startTime + 2.0;
				if (cnt2 == 2)
					ex->endTime += 3.0;
			}
		}
		else
		{
			vec3_t	dmin = {-10, -10, -10};
			vec3_t	dmax = {10, 10, 10};
			R_ColoredParticleExplosion (pos, 14, 10, 10);
			R_RunParticleEffect2 (pos, dmin, dmax, 145, pt_explode, 14);
		}
		/* make the actual explosion */
		ex = HWCL_NewExplosion (HWM_ICEHIT);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + ex->model->numframes * 0.1;
		}
		if (cnt2 == 1)
			HWCL_TEntSound (HWS_ICEWALL, pos);	/* hit a wall */
		else if (cnt2 == 2)
			HWCL_TEntSound (HWS_ICESHATTER, pos);
		else	HWCL_TEntSound (HWS_ICEFLESH, pos);	/* hit a person */
		return;

	case HWTE_ICESTORM:
		ent = MSG_ReadShort ();
		if (msg_badread || !HWCL_TEntEntityOrigin (ent, org))
			return;
		hwcl_ice_sound_time += host_frametime;
		if (hwcl_ice_sound_time >= .6)
		{
			HWCL_TEntSound (HWS_ICESTORM, org);
			hwcl_ice_sound_time -= .6;
		}
		for (i = 0; i < 5; i++)
		{	/* make some ice beams... */
			vec3_t	source, dest;

			VectorCopy (org, source);
			source[0] += (rand() % 100) - 50;
			source[1] += (rand() % 100) - 50;
			VectorCopy (source, dest);
			dest[2] += 128;
			if (!HWCL_TEntStream (HWTE_STREAM_ICECHUNKS, ent,
					i + HWCL_STREAM_ATTACHED, i, 0.3, 0,
					HWM_STICE, -1, -1, -1, source, dest, org))
				break;
		}
		return;

	case HWTE_HWMISSILEFLASH:
		HWCL_ReadCoords (pos);
		vel[0] = MSG_ReadCoord ();	/* angles */
		vel[1] = MSG_ReadCoord ();
		if (msg_badread)
			return;
		vel[2] = rand() % 360;
		ex = HWCL_NewExplosion (HWM_HANDFX);
		if (!ex)
			return;
		VectorCopy (pos, ex->origin);
		VectorCopy (vel, ex->angles);
		ex->frameFunc = HWCL_MissileFlashThink;
		ex->startTime = cl.time;
		ex->endTime = ex->startTime + 2;
		ex->exflags = HWEX_ROTATE;
		/* The original's (rand() * 360) + 360 is a spin in the millions of
		 * degrees a second with a 15-bit RAND_MAX, and signed overflow
		 * with a 31-bit one.  Same huge random spin, bounded to 15 bits. */
		ex->avel[2] = (float)(rand() & 0x7fff) * 360 + 360;
		ex->flags = MLS_ABSLIGHT|DRF_TRANSLUCENT;
		ex->abslight = 128;
		ex->scale = 80 + (rand() % 40);
		return;

	case HWTE_LIGHTNING_HAMMER:
		ent = MSG_ReadShort ();
		if (msg_badread || !HWCL_TEntEntityOrigin (ent, org))
			return;
		HWCL_TEntSound ((rand() & 1) ? HWS_LIGHTNING1 : HWS_LIGHTNING2, org);
		for (i = 0; i < 5; i++)
		{	/* make some lightning */
			vec3_t	source, dest;

			VectorCopy (org, source);
			source[0] += (rand() % 30) - 15;
			source[1] += (rand() % 30) - 15;
			VectorCopy (source, dest);
			dest[0] += (rand() % 80) - 40;
			dest[1] += (rand() % 80) - 40;
			dest[2] += 64 + (rand() % 48);
			if (!HWCL_TEntStream (HWTE_STREAM_LIGHTNING, ent, i, i, 0.5, 0,
					HWM_STLGHTNG, -1, -1, -1, source, dest, NULL))
				break;
		}
		return;

	case HWTE_HWTELEPORT:
	{
		float	dir;

		HWCL_ReadCoords (pos);
		cnt = MSG_ReadShort ();		/* skin# */
		if (msg_badread)
			return;
		/* gamecode sends 0 or 1; anything outside the model skin range
		 * would reach the renderer's gfx/skin*.lmp path or its
		 * Sys_Error, so it draws the default skin instead */
		if (cnt < 0 || cnt >= HWCL_MAX_MODEL_SKIN)
			cnt = 0;
		HWCL_TEntSound (HWS_TELEPORT1 + rand() % 5, pos);

		ex = HWCL_NewExplosion (HWM_TELEPORT);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->frameFunc = HWCL_TeleportFlashThink;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 2;
			/* the original's avel is unused: no HWEX_ROTATE */
			ex->flags = SCALE_TYPE_XYONLY | DRF_TRANSLUCENT;
			ex->skin = cnt;
			ex->scale = 100;
		}
		for (dir = 0; dir < 360; dir += 45)
		{
			q_sincosdeg (dir, &sinval, &cosval);
			sinval *= 10;
			cosval *= 10;
			for (i = 0; i < 2; i++)
			{
				ex = HWCL_NewExplosion (HWM_TELESMK2);
				if (!ex)
					break;
				VectorCopy (pos, ex->origin);
				ex->origin[2] += 64 * i;
				ex->startTime = cl.time;
				ex->endTime = ex->startTime + .5;
				ex->velocity[0] = cosval;
				ex->velocity[1] = sinval;
				ex->flags = DRF_TRANSLUCENT;
			}
		}
		return;
	}

	case HWTE_SWORD_EXPLOSION:
		HWCL_ReadCoords (pos);
		ent = MSG_ReadShort ();
		if (msg_badread)
			return;
		if (HWCL_TEntEntityOrigin (ent, org))
		{
			HWCL_TEntSound ((rand() & 1) ? HWS_LIGHTNING1 : HWS_LIGHTNING2, pos);
			for (i = 0; i < 5; i++)
			{	/* make some lightning */
				vec3_t	source, dest;

				VectorCopy (pos, source);
				source[0] += (rand() % 30) - 15;
				source[1] += (rand() % 30) - 15;
				VectorCopy (source, dest);
				dest[0] += (rand() % 80) - 40;
				dest[1] += (rand() % 80) - 40;
				dest[2] += 64 + (rand() % 48);
				if (!HWCL_TEntStream (HWTE_STREAM_LIGHTNING, ent, i, i,
						0.5, 0, HWM_STLGHTNG, -1, -1, -1,
						source, dest, NULL))
					break;
			}
		}
		ex = HWCL_NewExplosion (HWM_VORPSHOK);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 1.0;
			ex->flags |= MLS_ABSLIGHT;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 100;
			ex->frameFunc = HWCL_SwordFrameFunc;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;

	case HWTE_AXE_BOUNCE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_SPARK);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->flags |= MLS_ABSLIGHT;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 100;
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		HWCL_TEntSound (HWS_AXEBOUNCE, pos);
		return;

	case HWTE_AXE_EXPLODE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		i = (host_frametime < .07) ? 0 : 3;	/* based on framerate */
		for ( ; i < 5; i++)
		{
			/* 0,1 xpspblue; 2,3 xpspblue see-through; 4,5 spark0
			 * see-through */
			static const int	models[6] = {
				HWM_XPSPBLUE, HWM_XPSPBLUE, HWM_XPSPBLUE,
				HWM_XPSPBLUE, HWM_SPARK0, HWM_SPARK0
			};

			ex = HWCL_TEntBurstPiece (models, 6, pos, 3, 15, &rnd);
			if (!ex)
				continue;
			if (rnd >= 2)
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->flags |= MLS_ABSLIGHT;
			ex->abslight = 160 + (rand() % 24);
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;

	case HWTE_TIME_BOMB:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		i = (host_frametime < .07) ? 0 : 14;	/* based on framerate */
		for ( ; i < 20; i++)
		{
			static const int	models[4] = {
				HWM_SM_EXPLD, HWM_GEN_EXPL, HWM_BG_EXPLD, HWM_GEN_EXPL
			};
			ex = HWCL_TEntBurstPiece (models, 4, pos, 3, 40, NULL);
			if (!ex)
				continue;
			ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 160 + (rand() % 24);
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;

	case HWTE_FIREBALL:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_BLAST);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->flags |= MLS_ABSLIGHT|SCALE_TYPE_UNIFORM|SCALE_ORIGIN_CENTER;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 1;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 1.0;
			ex->frameFunc = HWCL_FireBallUpdate;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = 50;
			ex->avel[1] = 50;
			ex->avel[2] = 50;
		}
		HWCL_TEntSound (HWS_FIREBALL, pos);
		return;

	case HWTE_SUNSTAFF_POWER:
		ent = MSG_ReadShort ();
		HWCL_ReadCoords (vel);
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		for (i = 0; i < 2; i++)
		{
			ex = HWCL_NewExplosion (i ? HWM_STSUNSF3 : HWM_BLAST);
			if (!ex)
				continue;
			VectorCopy (pos, ex->origin);
			if (i)
				ex->frameFunc = NULL;	/* the original's sunPowerUpdate was empty */
			else
			{
				ex->flags |= DRF_TRANSLUCENT;
				ex->frameFunc = HWCL_SunBallUpdate;
			}
			ex->flags |= MLS_ABSLIGHT|SCALE_TYPE_UNIFORM|SCALE_ORIGIN_CENTER;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 200;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + .8;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = 50;
			ex->avel[1] = 50;
			ex->avel[2] = 50;
		}
		HWCL_TEntSound (HWS_FIREBALL, pos);
		if (HWCL_TEntEntityOrigin (ent, org))
		{
			HWCL_TEntSound (HWS_SUNSTAFF, org);
			/* Drawn by Hexen II's SUNSTAFF2 stream renderer
			 * (cl_tent.c CL_UpdateTEnts): a straight run of models[0]
			 * with [2] and [3] at the end.  That LOOKS DIFFERENT from
			 * HexenWorld, whose renderer twisted two strands of [0]
			 * and [1] around the beam in a spiral that shrank over its
			 * 0.8 s life.  Streams stay Hexen II's to draw (#213). */
			HWCL_TEntStream (HWTE_STREAM_SUNSTAFF2, ent, 0, 0, 0.8, 0,
					HWM_STSUNSF2, HWM_STSUNSF1, HWM_STSUNSF3,
					HWM_STSUNSF4, vel, pos, NULL);
		}
		return;

	case HWTE_PURIFY2_EXPLODE:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_XPLOD29);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->flags |= MLS_ABSLIGHT;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 100;
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		/* the original computed a framerate-based start and then
		 * overwrote it: always all 12 */
		for (i = 0; i < 12; i++)
		{
			static const int	models[4] = {
				HWM_SM_EXPLD, HWM_SM_EXPLD, HWM_BG_EXPLD, HWM_GEN_EXPL
			};
			ex = HWCL_TEntBurstPiece (models, 4, pos, 10, 12, NULL);
			if (!ex)
				continue;
			if (host_frametime < 0.07)
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 160 + (rand() % 64);
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.04;
		}
		HWCL_TEntSound (HWS_PURIFY2, pos);
		return;

	case HWTE_PLAYER_DEATH:
	{
		int	angle, pitch, force, style;	/* 0 to 255 */
		float	throwPower, curAng, curPitch;

		HWCL_ReadCoords (pos);
		angle = MSG_ReadByte ();
		pitch = MSG_ReadByte ();
		force = MSG_ReadByte ();
		style = MSG_ReadByte ();
		if (msg_badread)
			return;

		i = (host_frametime < 0.07) ? 0 : 8;
		for ( ; i < 12; i++)
		{
			static const int	models[3] = {
				HWM_FLESH1, HWM_FLESH2, HWM_FLESH3
			};
			hwcl_explosion_t	t;

			/* filled first, model drawn last, as the original */
			memset (&t, 0, sizeof(t));
			ex = &t;
			VectorCopy (pos, ex->origin);
			ex->origin[0] += (rand() % 40) - 20;
			ex->origin[1] += (rand() % 40) - 20;
			ex->origin[2] += (rand() % 40);
			ex->frameFunc = HWCL_ChunkThink;

			throwPower = 3.5 + ((rand() % 100) / 100.0);
			throwPower *= force;
			curAng = angle*6.28/256.0 + ((rand() % 100) / 50.0) - 1.0;
			curPitch = pitch*6.28/256.0 + ((rand() % 100) / 100.0) - .5;
			HWCL_TEntDirection (curAng, curPitch, throwPower, ex->velocity);

			ex->angles[0] = rand() % 360;
			ex->angles[1] = rand() % 360;
			ex->angles[2] = rand() % 360;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = (rand() % 850) - 425;
			ex->avel[1] = (rand() % 850) - 425;
			ex->avel[2] = (rand() % 850) - 425;
			ex->scale = 80 + (rand() % 40);
			ex->data = THINGTYPE_FLESH;
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 4.0;
			HWCL_PlaceExplosion (&t, models[rand() % 3]);
		}

		switch (style)
		{
		case 0:
			HWCL_TEntSound (HWS_BIG_GIB, pos);
			break;
		case 1:
			HWCL_TEntSound ((rand() % 2) ? HWS_GIB1 : HWS_GIB2, pos);
			break;
		case 2:
			HWCL_TEntSound (HWS_TELEFRAG, pos);
			break;
		}
		return;
	}

	case HWTE_PURIFY1_EFFECT:
	{
		float	angle, pitch, dist;
		vec3_t	endPos;

		HWCL_ReadCoords (pos);
		angle = MSG_ReadByte ()*6.28/256.0;
		pitch = MSG_ReadByte ()*6.28/256.0;
		dist = MSG_ReadShort ();
		if (msg_badread)
			return;
		HWCL_TEntDirection (angle, pitch, dist, endPos);
		VectorAdd (endPos, pos, endPos);

		R_RocketTrail (pos, endPos, rt_purify);
		HWCL_TEntSound (HWS_PURIFY1_FIRE, pos);
		HWCL_TEntSound (HWS_PURIFY1_HIT, endPos);

		ex = HWCL_NewExplosion (HWM_FCIRCLE);
		if (ex)
		{
			VectorCopy (endPos, ex->origin);
			ex->startTime = cl.time;
			ex->flags |= MLS_ABSLIGHT;
			ex->abslight = 128;
			ex->skin = 0;
			ex->scale = 100;
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		return;
	}

	case HWTE_TELEPORT_LINGER:
	{
		float	duration;

		HWCL_ReadCoords (pos);
		duration = MSG_ReadCoord ();
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_BSPARK);
		if (!ex)
			return;
		VectorCopy (pos, ex->origin);
		ex->startTime = cl.time;
		ex->flags |= MLS_ABSLIGHT;
		ex->abslight = 128;
		ex->frameFunc = HWCL_TelEffectUpdate;
		ex->skin = 0;
		ex->scale = 0;
		ex->endTime = ex->startTime + duration;
		return;
	}

	case HWTE_LINE_EXPLOSION:
	{
		int	distance, pass;
		vec3_t	endPos, midPos, curPos, distVec;
		float	ratio;

		HWCL_ReadCoords (pos);
		HWCL_ReadCoords (endPos);
		if (msg_badread)
			return;
		VectorAdd (pos, endPos, midPos);
		VectorScale (midPos, 0.5, midPos);
		VectorSubtract (midPos, pos, distVec);
		distance = (int)(VectorNormalizeFast (distVec) * 0.025);
		if (distance <= 0)
			return;
		VectorScale (distVec, 40, distVec);
		/* out from the middle one way, then the other */
		for (pass = 0; pass < 2; pass++)
		{
			VectorCopy (midPos, curPos);
			for (i = 0; i < distance; i++)
			{
				static const int	models[3] = {
					HWM_GEN_EXPL, HWM_BG_EXPLD, HWM_SM_EXPLD
				};
				ex = HWCL_NewExplosion (models[rand() % 3]);
				if (ex)
				{
					VectorCopy (curPos, ex->origin);
					ex->flags |= MLS_ABSLIGHT;
					ex->abslight = 128;
					ex->skin = 0;
					ratio = (float)i / (float)distance;
					ex->scale = 200 - (int)(150.0*ratio);
					ex->startTime = cl.time + ratio*0.75;
					ex->endTime = ex->startTime + ex->model->numframes * (0.025 + HWCL_FRANDOM()*0.01);
				}
				VectorAdd (curPos, distVec, curPos);
			}
			VectorScale (distVec, -1, distVec);
		}
		return;
	}

	case HWTE_METEOR_CRUSH:
	{
		float	maxDist;

		HWCL_ReadCoords (pos);
		maxDist = MSG_ReadLong ();
		if (msg_badread)
			return;
		ex = HWCL_NewExplosion (HWM_NULL);
		if (ex)
		{
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 0.4;
			ex->frameFunc = HWCL_MeteorCrushSpawnThink;
			ex->data = maxDist;
		}
		HWCL_TEntSound (HWS_EXPLODE, pos);
		return;
	}

	case HWTE_ACIDBALL:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		i = (host_frametime < 0.07) ? 0 : 2;
		for ( ; i < 5; i++)
		{
			static const int	model = HWM_AXPLSN_2;

			ex = HWCL_TEntBurstPiece (&model, 1, pos, 3, 6, NULL);
			if (!ex)
				continue;
			if (host_frametime < 0.07)
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 160 + (rand() % 24);
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		HWCL_TEntSound (HWS_ACIDHIT, pos);
		return;

	case HWTE_ACIDBLOB:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		i = (host_frametime < 0.07) ? 0 : 7;
		for ( ; i < 12; i++)
		{
			static const int	models[4] = {
				HWM_AXPLSN_2, HWM_AXPLSN_1, HWM_AXPLSN_5, HWM_AXPLSN_5
			};
			ex = HWCL_TEntBurstPiece (models, 4, pos, 3, 25, NULL);
			if (!ex)
				continue;
			if (host_frametime < 0.07)
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 1;
			ex->skin = 0;
			ex->scale = 80 + (rand() % 40);
			ex->startTime = cl.time + ((rand() % 50) / 200.0);
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		/* always make 8 meteors */
		i = (host_frametime < 0.07) ? 0 : 4;
		for ( ; i < 8; i++)
		{
			ex = HWCL_NewExplosion (HWM_SUCWP2P);
			if (!ex)
				break;
			VectorCopy (pos, ex->origin);
			ex->frameFunc = HWCL_ChunkThink;
			ex->velocity[0] = (rand() % 500) - 250;
			ex->velocity[1] = (rand() % 500) - 250;
			ex->velocity[2] = (rand() % 200) + 200;
			ex->angles[0] = rand() % 360;
			ex->angles[1] = rand() % 360;
			ex->angles[2] = rand() % 360;
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = (rand() % 850) - 425;
			ex->avel[1] = (rand() % 850) - 425;
			ex->avel[2] = (rand() % 850) - 425;
			ex->scale = 45 + (rand() % 10);
			ex->data = THINGTYPE_ACID;
			ex->skin = 0;
			VectorScale (ex->avel, 4.0, ex->avel);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + 4.0;
		}
		HWCL_TEntSound (HWS_ACIDHIT, pos);
		return;

	case HWTE_FIREWALL:
	{
		float	travelAng, travelPitch;
		int	fireCounts, steps;
		vec3_t	endPos, curPos, posAdd;

		HWCL_ReadCoords (pos);
		travelAng = MSG_ReadByte ()*6.28/256.0;
		travelPitch = MSG_ReadByte ()*6.28/256.0;
		fireCounts = MSG_ReadByte ();
		if (msg_badread)
			return;

		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 200 + (rand() & 31);
		dl->die = cl.time + 0.001;

		HWCL_TEntDirection (travelAng, travelPitch, 450, endPos);
		VectorAdd (endPos, pos, endPos);
		VectorCopy (pos, curPos);
		VectorSubtract (endPos, pos, posAdd);
		VectorScale (posAdd, .125, posAdd);

		for (i = 0; i < fireCounts; i++)
		{
			static const int	models[3] = {
				HWM_FIREWAL1, HWM_FIREWAL5, HWM_FIREWAL4
			};
			ex = HWCL_NewExplosion (models[rand() % 3]);
			if (ex)
			{
				VectorCopy (curPos, ex->origin);
				ex->startTime = cl.time + .3/8.0 * i;
				ex->endTime = ex->startTime + ex->model->numframes * 0.05;
				/* Down to the floor, then back up one step.  The
				 * original looped until it left empty space; the cap
				 * is so a wall cast off a ledge into a void cannot
				 * spin here forever. */
				for (steps = 0; steps < 256 && HWCL_TEntEmpty (ex->origin); steps++)
					ex->origin[2] -= 16;
				ex->origin[2] += 16;
				ex->origin[0] += (rand() % 8) - 4;
				ex->origin[1] += (rand() % 8) - 4;
				ex->origin[2] += (rand() % 6) + 21;
			}
			ex = HWCL_NewExplosion (HWM_FLAMESTR);
			if (ex)
			{
				VectorCopy (curPos, ex->origin);
				ex->origin[0] += (rand() % 8) - 4;
				ex->origin[1] += (rand() % 8) - 4;
				ex->origin[2] += (rand() % 6) - 3;
				ex->startTime = cl.time + .3/8.0 * i;
				ex->endTime = ex->startTime + ex->model->numframes * 0.05;
				ex->flags |= DRF_TRANSLUCENT;
			}
			VectorAdd (curPos, posAdd, curPos);
		}
		return;
	}

	case HWTE_FIREWALL_IMPACT:
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		i = (host_frametime < 0.07) ? 0 : 8;
		for ( ; i < 12; i++)
		{
			ex = HWCL_NewExplosion (HWM_FBOOM);
			if (!ex)
				break;
			VectorCopy (pos, ex->origin);
			ex->origin[0] += (rand() % 32) - 16;
			ex->origin[1] += (rand() % 32) - 16;
			ex->origin[2] += (rand() % 32) - 16;
			ex->startTime = cl.time + ((rand() % 150) / 200);	/* integer: 0, as in the original */
			ex->endTime = ex->startTime + ex->model->numframes * 0.05;
		}
		HWCL_TEntSound (HWS_FLAMEEND, pos);
		return;

	case HWTE_HWBONERIC:
		HWCL_ReadCoords (pos);
		cnt = MSG_ReadByte ();
		if (msg_badread)
			return;
		R_RunParticleEffect4 (pos, 3, 368 + (rand() % 16), pt_grav, cnt);
		rnd = rand() % 100;
		if (rnd > 95)
			HWCL_TEntSound (HWS_RIC1, pos);
		else if (rnd > 91)
			HWCL_TEntSound (HWS_RIC2, pos);
		else if (rnd > 87)
			HWCL_TEntSound (HWS_RIC3, pos);
		return;

	case HWTE_POWERFLAME:
	{
		float	travelAng, travelPitch, svTime;
		int	fireCounts, side;
		vec3_t	endPos, curPos, posAdd, angles, forward, right, up;

		HWCL_ReadCoords (pos);
		travelAng = MSG_ReadByte ()*6.28/256.0;
		travelPitch = MSG_ReadByte ()*6.28/256.0;
		fireCounts = MSG_ReadByte ();
		svTime = MSG_ReadLong ();
		if (msg_badread)
			return;

		angles[0] = travelPitch*360/(2*M_PI);
		angles[1] = travelAng*360/(2*M_PI);
		angles[2] = 0;
		AngleVectors (angles, forward, right, up);

		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 200 + (rand() & 31);
		dl->die = cl.time + 0.001;

		HWCL_TEntDirection (travelAng, travelPitch, 375, endPos);
		VectorAdd (endPos, pos, endPos);
		VectorCopy (pos, curPos);
		VectorSubtract (endPos, pos, posAdd);
		VectorScale (posAdd, .125, posAdd);

		for (i = 0; i < fireCounts; i++)
		{
			q_sincosrad ((svTime + (i*.3/8.0))*8, &sinval, &cosval);
			sinval *= 10;
			cosval *= 10;
			/* a flame on each side of the line, mirrored */
			for (side = 1; side >= -1; side -= 2)
			{
				ex = HWCL_NewExplosion (HWM_FLAMESTR);
				if (!ex)
					continue;
				VectorCopy (curPos, ex->origin);
				VectorMA (ex->origin, side * cosval, right, ex->origin);
				VectorMA (ex->origin, side * sinval, up, ex->origin);
				ex->origin[0] += (rand() % 8) - 4;
				ex->origin[1] += (rand() % 8) - 4;
				ex->origin[2] += (rand() % 6) - 3;
				ex->startTime = cl.time + .3/8.0 * i;
				ex->endTime = ex->startTime + ex->model->numframes * 0.05;
				ex->flags |= DRF_TRANSLUCENT;
				VectorMA (ex->velocity, side * cosval * 4.0, right, ex->velocity);
				VectorMA (ex->velocity, side * sinval * 4.0, up, ex->velocity);
			}
			VectorAdd (curPos, posAdd, curPos);
		}
		return;
	}

	case HWTE_BLOODRAIN:
	{
		float	travelAng, travelPitch, trailLen;
		unsigned char	health;
		vec3_t	angles, forward, right, up;

		HWCL_ReadCoords (pos);
		travelAng = MSG_ReadByte ()*6.28/256.0;
		travelPitch = MSG_ReadByte ()*6.28/256.0;
		trailLen = MSG_ReadByte ();
		health = MSG_ReadByte ();
		if (msg_badread)
			return;

		HWCL_TEntDirection (travelAng, travelPitch, 800, vel);
		angles[0] = travelPitch*360/(2*M_PI);
		angles[1] = travelAng*360/(2*M_PI);
		angles[2] = 0;
		AngleVectors (angles, forward, right, up);

		/* one drop, and a drop either side of it at over 90 health */
		for (i = 0; i < (health > 90 ? 3 : 1); i++)
		{
			ex = HWCL_NewExplosion (HWM_SUCWP1P);
			if (!ex)
				break;
			VectorCopy (pos, ex->origin);
			if (i)
				VectorMA (ex->origin, i == 1 ? 7 : -7, right, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + trailLen*.3/240.0;
			ex->angles[0] = travelPitch*360/6.28;
			ex->angles[1] = travelAng*360/6.28;
			ex->scale = i ? health - 90 : health;
			VectorCopy (vel, ex->velocity);
			ex->frameFunc = HWCL_UpdateBloodRain;
			ex->exflags |= HWEX_COLLIDE;
		}
		return;
	}

	case HWTE_AXE:
	case HWTE_PURIFY2_MISSILE:
	case HWTE_SWORD_SHOT:
	case HWTE_ICESHOT:
	case HWTE_METEOR:
	case HWTE_MEGAMETEOR:
	case HWTE_ACID_BALL_FLY:
	case HWTE_ACID_BLOB_FLY:
	{
		float	travelAng, travelPitch, trailLen, speed;
		hwcl_explosion_t	*ex2;

		HWCL_ReadCoords (pos);
		travelAng = MSG_ReadByte ()*6.28/256.0;
		travelPitch = MSG_ReadByte ()*6.28/256.0;
		trailLen = MSG_ReadByte () * .01;
		if (msg_badread)
			return;

		switch (type)
		{
		case HWTE_AXE:			speed = 1100; break;
		case HWTE_SWORD_SHOT:
		case HWTE_ICESHOT:		speed = 1200; break;
		case HWTE_MEGAMETEOR:		speed = 1600; break;
		case HWTE_ACID_BALL_FLY:	speed = 850; break;
		default:			speed = 1000; break;	/* purify2, meteor, acid blob */
		}
		HWCL_TEntDirection (travelAng, travelPitch, speed, vel);

		switch (type)
		{
		case HWTE_AXE:
			ex = HWCL_NewExplosion (HWM_AXBLADE);
			if (ex)
			{
				VectorCopy (pos, ex->origin);
				ex->startTime = cl.time;
				ex->endTime = ex->startTime + trailLen*.3;
				ex->angles[0] = travelPitch*360/6.28;
				ex->angles[1] = travelAng*360/6.28;
				VectorCopy (vel, ex->velocity);
				ex->exflags |= HWEX_COLLIDE;
			}
			ex = HWCL_NewExplosion (HWM_AXTAIL);
			if (ex)
			{
				VectorCopy (pos, ex->origin);
				ex->startTime = cl.time;
				ex->endTime = ex->startTime + trailLen*.3;
				ex->angles[0] = travelPitch*360/6.28;
				ex->angles[1] = travelAng*360/6.28;
				VectorCopy (vel, ex->velocity);
				ex->exflags |= HWEX_COLLIDE|HWEX_STILL_FRAME;
				ex->data = 0;
				ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
				ex->abslight = 128;
				ex->skin = 0;
			}
			return;

		case HWTE_ICESHOT:
			ex = HWCL_NewExplosion (HWM_ICESHOT1);
			if (!ex)
				return;
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + trailLen*.3;
			ex->angles[0] = travelPitch*360/6.28;
			ex->angles[1] = travelAng*360/6.28;
			VectorCopy (vel, ex->velocity);
			ex->frameFunc = HWCL_UpdateIceShot;
			ex->scale = 100;
			/* = not |=: the original dropped HWEX_COLLIDE here, so
			 * the shot flies through walls until its time is up */
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = 425;
			ex->avel[1] = 425;
			ex->avel[2] = 425;

			ex2 = ex;
			ex = HWCL_NewExplosion (HWM_ICESHOT2);
			if (!ex)
				return;
			VectorCopy (pos, ex->origin);
			ex->startTime = cl.time;
			ex->endTime = ex->startTime + trailLen*.3;
			ex->angles[0] = travelPitch*360/6.28;
			ex->angles[1] = travelAng*360/6.28;
			VectorCopy (vel, ex->velocity);
			ex->scale = 200;
			ex->flags |= MLS_ABSLIGHT|DRF_TRANSLUCENT;
			ex->abslight = 128;
			ex->exflags = HWEX_ROTATE;
			VectorCopy (ex2->avel, ex->avel);
			return;

		default:
			break;
		}

		/* the single-model flyers */
		switch (type)
		{
		case HWTE_PURIFY2_MISSILE:	ex = HWCL_NewExplosion (HWM_DRGNBALL); break;
		case HWTE_SWORD_SHOT:		ex = HWCL_NewExplosion (HWM_VORPSHOT); break;
		case HWTE_METEOR:
		case HWTE_MEGAMETEOR:		ex = HWCL_NewExplosion (HWM_TEMPMETR); break;
		default:			ex = HWCL_NewExplosion (HWM_SUCWP2P); break;	/* acid */
		}
		if (!ex)
			return;
		VectorCopy (pos, ex->origin);
		ex->startTime = cl.time;
		ex->endTime = ex->startTime + trailLen*.3;
		ex->angles[0] = travelPitch*360/6.28;
		ex->angles[1] = travelAng*360/6.28;
		VectorCopy (vel, ex->velocity);
		ex->exflags |= HWEX_COLLIDE;
		switch (type)
		{
		case HWTE_PURIFY2_MISSILE:
			ex->frameFunc = HWCL_UpdatePurify2;
			ex->scale = 150;
			break;
		case HWTE_SWORD_SHOT:
			ex->frameFunc = HWCL_UpdateSwordShot;
			ex->scale = 100;
			ex->exflags |= HWEX_STILL_FRAME;
			ex->data = 16 + ((int)(cl.time * 20.0) % 13);
			break;
		case HWTE_METEOR:
		case HWTE_MEGAMETEOR:
			ex->frameFunc = HWCL_UpdateMeteor;
			if (type == HWTE_MEGAMETEOR)
			{
				ex->scale = 230;
				ex->flags |= DRF_TRANSLUCENT;
			}
			else	ex->scale = 100;
			/* = not |=, dropping HWEX_COLLIDE, as the original did */
			ex->exflags = HWEX_ROTATE;
			ex->avel[0] = 200;
			ex->avel[1] = 200;
			ex->avel[2] = 200;
			break;
		case HWTE_ACID_BALL_FLY:
			ex->frameFunc = HWCL_UpdateAcidBall;
			break;
		case HWTE_ACID_BLOB_FLY:
			ex->frameFunc = HWCL_UpdateAcidBlob;
			ex->scale = 230;
			break;
		}
		return;
	}

	case HWTE_LIGHTNINGBALL:
	{
		float	travelAng, travelPitch, trailLen, speed;

		HWCL_ReadCoords (pos);
		travelAng = MSG_ReadByte ()*6.28/256.0;
		travelPitch = MSG_ReadByte ()*6.28/256.0;
		speed = MSG_ReadShort ();
		trailLen = MSG_ReadByte () * .01;
		if (msg_badread)
			return;

		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		dl->color[0] = 0.2;
		dl->color[1] = 0.1;
		dl->color[2] = 0.05;
		dl->color[3] = 0.7;

		HWCL_TEntDirection (travelAng, travelPitch, speed, vel);
		ex = HWCL_NewExplosion (HWM_LBALL);
		if (!ex)
			return;
		VectorCopy (pos, ex->origin);
		ex->startTime = cl.time;
		ex->endTime = ex->startTime + trailLen*.2;
		ex->angles[0] = travelPitch*360/6.28;
		ex->angles[1] = travelAng*360/6.28;
		VectorCopy (vel, ex->velocity);
		ex->exflags |= HWEX_COLLIDE;
		return;
	}

	case HWTE_CUBEBEAM:
	{
		int	ent2;
		qboolean	have1, have2;
		vec3_t	org2, source, dest;

		ent = MSG_ReadShort ();
		ent2 = MSG_ReadShort ();
		if (msg_badread)
			return;
		have1 = HWCL_TEntEntityOrigin (ent, org);
		have2 = HWCL_TEntEntityOrigin (ent2, org2);
		if (!have1 && !have2)
			return;
		if (have1)
			VectorCopy (org, source);
		else
		{	/* don't know where the cube is: put it all at the target */
			VectorCopy (org2, source);
			source[2] += 10;
		}
		if (have2)
		{
			VectorCopy (org2, dest);
			dest[2] += 30;
		}
		else
		{	/* don't know where the victim is: put it all at the cube */
			VectorCopy (source, dest);
			dest[2] += 10;
		}
		HWCL_TEntSound (HWS_SUNSTAFF, source);
		HWCL_TEntSound (HWS_SUNHIT, dest);
		R_SunStaffTrail (dest, source);
		HWCL_TEntStream (HWTE_STREAM_COLORBEAM, ent, 0, 0, 0.1, rand() % 5,
				HWM_STCLRBM, -1, -1, -1, source, dest, NULL);
		return;
	}

	case HWTE_LIGHTNINGEXPLODE:
		ent = MSG_ReadShort ();		/* the original looked it up and ignored it */
		HWCL_ReadCoords (pos);
		if (msg_badread)
			return;
		HWCL_TEntSound ((rand() & 1) ? HWS_LIGHTNING1 : HWS_LIGHTNING2, pos);
		for (i = 0; i < 10; i++)
		{	/* make some lightning */
			vec3_t	dest;
			float	tempAng, tempPitch;

			tempAng = (rand() % 628) / 100.0;
			tempPitch = (rand() % 628) / 100.0;
			HWCL_TEntDirection (tempAng, tempPitch, 75, dest);
			VectorAdd (dest, pos, dest);
			if (!HWCL_TEntStream (HWTE_STREAM_LIGHTNING, ent, i, i, 0.5, 0,
					HWM_STLGHTNG, -1, -1, -1, pos, dest, NULL))
				break;
		}
		return;
	}
}

/* TE_SUNSTAFF_CHEAP, after the reader has consumed it: points[0..n] are
 * the beam's start and each reflection, attached at the start to ent. */
static void HWCL_TEntSunstaffCheap (int ent, vec3_t points[4], int beams)
{
	vec3_t	org;
	int	i;

	if (!HWCL_TEntEntityOrigin (ent, org))
		return;
	for (i = 0; i < beams; i++)
	{
		if (!HWCL_TEntStream (HWTE_STREAM_SUNSTAFF1, ent,
				i ? i : HWCL_STREAM_ATTACHED, i, 0.5, 0,
				HWM_STSUNSF1, HWM_STSUNSF2, HWM_STSUNSF3,
				HWM_STSUNSF4, points[i], points[i + 1], org))
			return;
	}
}

/* TE_CHAINLIGHTNING: a bolt between each pair of points, and a shock at
 * every point it reaches. */
static void HWCL_TEntChainLightning (int ent, vec3_t *points, int numTargs)
{
	hwcl_explosion_t	*ex;
	int	i;

	for (i = 0; i < numTargs - 1; i++)
	{
		if (!HWCL_TEntStream (HWTE_STREAM_LIGHTNING, ent, i, i, 0.3, 0,
				HWM_STLGHTNG, -1, -1, -1, points[i], points[i + 1],
				NULL))
			return;
		ex = HWCL_NewExplosion (HWM_VORPSHOK);
		if (!ex)
			continue;
		VectorCopy (points[i + 1], ex->origin);
		ex->startTime = cl.time;
		ex->endTime = ex->startTime + .3;
		ex->flags |= MLS_ABSLIGHT;
		ex->abslight = 224;
		ex->skin = rand() & 1;
		ex->scale = 150;
		ex->frameFunc = HWCL_ZapFrameFunc;
	}
}

/* ------------------------------------------------------------------ */
/* per frame                                                           */
/* ------------------------------------------------------------------ */

/* CL_UpdateExplosions.  From CL_ReadFromServer while HexenWorld is active,
 * after CL_RelinkEntities has reset the visedict list. */
void HWCL_UpdateTempEntities (void)
{
	int	i, f, numframes, fresh = 0, linked = 0;
	unsigned int	gen;
	hwcl_explosion_t	*ex;
	entity_t	*ent;

	if (!cl.worldmodel)
		return;

	for (i = 0, ex = hwcl_explosions; i < HWCL_MAX_EXPLOSIONS; i++, ex++)
	{
		if (!ex->model)
			continue;

		if ((ex->exflags & HWEX_COLLIDE) && !HWCL_TEntEmpty (ex->origin))
		{
			ex->model = NULL;
			continue;
		}

		/* if we hit endTime, get rid of explosion */
		if (ex->endTime <= cl.time)
		{
			ex->model = NULL;
			continue;
		}

		VectorCopy (ex->origin, ex->oldorg);

		/* set the current frame so i finish anim at endTime */
		if (ex->exflags & HWEX_STILL_FRAME)
			f = (int)ex->data;
		else if (ex->endTime > ex->startTime)
			f = (ex->model->numframes - 1) * (cl.time - ex->startTime) /
					(ex->endTime - ex->startTime);
		else
			f = 0;

		VectorMA (ex->origin, host_frametime, ex->velocity, ex->origin);
		VectorMA (ex->velocity, host_frametime, ex->accel, ex->velocity);
		if (ex->exflags & HWEX_ROTATE)
			VectorMA (ex->angles, host_frametime, ex->avel, ex->angles);

		/* startTime in the future delays showing and thinking; it
		 * still moves */
		if (ex->startTime > cl.time)
			continue;

		gen = hwcl_explosion_gen[i];
		if (ex->frameFunc)
			ex->frameFunc (ex);

		/* The frame func may have delayed or removed it -- or, with the
		 * pool full, allocated this very slot for something new, which
		 * starts drawing next frame from its own first frame. */
		if (hwcl_explosion_gen[i] != gen || ex->startTime > cl.time ||
				!ex->model)
			continue;

		if (cl_numvisedicts >= MAX_VISEDICTS)
			continue;

		/* The original's frame numbers can overrun (a still frame past a
		 * short model, a delayed start); the renderer would complain every
		 * frame, so pin them. */
		numframes = ex->model->numframes;
		if (f >= numframes)
			f = numframes - 1;
		if (f < 0)
			f = 0;

		/* The one place an explosion entity is written.  Cleared on
		 * this explosion's first draw, so nothing (alias lerp state
		 * included) carries over from the slot's previous tenant;
		 * after that every field is refilled each frame. */
		ent = &hwcl_explosion_ents[i];
		if (!ex->drawn)
			memset (ent, 0, sizeof(*ent));
		VectorCopy (ex->origin, ent->origin);
		VectorCopy (ex->angles, ent->angles);
		ent->model = ex->model;
		ent->frame = f;
		ent->skinnum = ex->skin;
		ent->drawflags = ex->flags;
		ent->abslight = (ex->flags & MLS_ABSLIGHT) ? ex->abslight : 0;
		ent->scale = ex->scale;
		ent->colormap = vid.colormap;
		cl_visedicts[cl_numvisedicts++] = ent;
		HWCL_TENT_LINKED (ent);

		linked++;
		if (!ex->drawn)
		{
			ex->drawn = true;
			fresh++;
		}
	}

	hwcl_tent_linked += fresh;
	if (fresh && developer.integer >= 2)
		Con_DPrintf ("HW tents: drew %d new, %d linked this frame\n",
				fresh, linked);
}
