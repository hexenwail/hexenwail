/*
 * cl_hw_tent.inc -- HexenWorld svc_temp_entity (23) for the integrated client
 *
 * Textually included by cl_hw.c.  GitHub #213.
 *
 * The integrated client used to have no case for svc_temp_entity at all, so
 * HWCL_ParseServerMessage stopped reading the packet at the first one.  HW
 * gamecode sends 27 of its temp entities MULTICAST_PHS_R -- on the reliable
 * stream -- so whatever reliable data followed in that packet was acked and
 * lost for good, svc_update_inv included.  That is how an artifact icon
 * (the spawn-protection rook of #211) could stay on screen forever.
 *
 * The one hard requirement here is to consume every payload exactly as the
 * server wrote it.  The read layouts below are transcribed from the original
 * HexenWorld CL_ParseTEnt (engine/hexenworld/client/cl_tent.c, deleted in
 * 036a0b531, still in git) and cross-checked against the gamecode writers in
 * gamecode/hc/hw/.  scripts/hw_tent_test.c checks each layout against a byte
 * stream built the way those writers build it.
 *
 * Effects the maintained Hexen II code already draws, with the same number
 * and payload, go to CL_ParseTEntType.  A few cheap particle effects are
 * drawn here.  The rest -- HW's own explosions, chunks, streams tied to
 * entity state -- are consumed without visuals for now; porting their
 * visuals is the remaining half of #213.
 */

/* HexenWorld TE_* numbers (engine/hexenworld/shared/protocol.h), kept local
 * because several collide with the Hexen II client's own: 24 vs 62 for
 * TE_STREAM_LIGHTNING_SMALL, and 33 is TE_LIGHT_PULSE there but
 * TE_BIGGRENADE here. */
enum
{
	HWTE_SPIKE = 0, HWTE_SUPERSPIKE = 1, HWTE_GUNSHOT = 2, HWTE_EXPLOSION = 3,
	HWTE_TAREXPLOSION = 4, HWTE_LIGHTNING1 = 5, HWTE_LIGHTNING2 = 6,
	HWTE_WIZSPIKE = 7, HWTE_KNIGHTSPIKE = 8, HWTE_LIGHTNING3 = 9,
	HWTE_LAVASPLASH = 10, HWTE_TELEPORT = 11, HWTE_BLOOD = 12,
	HWTE_LIGHTNINGBLOOD = 13,
	HWTE_STREAM_CHAIN = 25, HWTE_STREAM_FAMINE = 32,
	HWTE_BIGGRENADE = 33, HWTE_CHUNK = 34, HWTE_HWBONEPOWER = 35,
	HWTE_HWBONEPOWER2 = 36, HWTE_METEORHIT = 37, HWTE_HWRAVENDIE = 38,
	HWTE_HWRAVENEXPLODE = 39, HWTE_XBOWHIT = 40, HWTE_CHUNK2 = 41,
	HWTE_ICEHIT = 42, HWTE_ICESTORM = 43, HWTE_HWMISSILEFLASH = 44,
	HWTE_SUNSTAFF_CHEAP = 45, HWTE_LIGHTNING_HAMMER = 46,
	HWTE_DRILLA_EXPLODE = 47, HWTE_DRILLA_DRILL = 48, HWTE_HWTELEPORT = 49,
	HWTE_SWORD_EXPLOSION = 50, HWTE_AXE_BOUNCE = 51, HWTE_AXE_EXPLODE = 52,
	HWTE_TIME_BOMB = 53, HWTE_FIREBALL = 54, HWTE_SUNSTAFF_POWER = 55,
	HWTE_PURIFY2_EXPLODE = 56, HWTE_PLAYER_DEATH = 57,
	HWTE_PURIFY1_EFFECT = 58, HWTE_TELEPORT_LINGER = 59,
	HWTE_LINE_EXPLOSION = 60, HWTE_METEOR_CRUSH = 61,
	HWTE_STREAM_LIGHTNING_SMALL = 62, HWTE_ACIDBALL = 63,
	HWTE_ACIDBLOB = 64, HWTE_FIREWALL = 65, HWTE_FIREWALL_IMPACT = 66,
	HWTE_HWBONERIC = 67, HWTE_POWERFLAME = 68, HWTE_BLOODRAIN = 69,
	HWTE_AXE = 70, HWTE_PURIFY2_MISSILE = 71, HWTE_SWORD_SHOT = 72,
	HWTE_ICESHOT = 73, HWTE_METEOR = 74, HWTE_LIGHTNINGBALL = 75,
	HWTE_MEGAMETEOR = 76, HWTE_CUBEBEAM = 77, HWTE_LIGHTNINGEXPLODE = 78,
	HWTE_ACID_BALL_FLY = 79, HWTE_ACID_BLOB_FLY = 80,
	HWTE_CHAINLIGHTNING = 81,
	HWTE_COUNT
};

/* The Hexen II client's number for its own TE_STREAM_LIGHTNING_SMALL
 * (cl_tent.c). */
#define HWTE_H2_STREAM_LIGHTNING_SMALL	24

/* Fixed payload sizes in bytes after the type byte, for every type whose
 * layout does not depend on its own contents.  0 means "not fixed": either
 * handled explicitly below or not a HexenWorld temp entity at all.  On the
 * wire a coord and a short are 2 bytes, a long 4. */
static const unsigned char hwte_fixed_size[HWTE_COUNT] =
{
	[HWTE_TAREXPLOSION]	= 6,		/* pos */
	[HWTE_GUNSHOT]		= 1 + 6,	/* count, pos */
	[HWTE_BLOOD]		= 1 + 6,	/* count, pos */
	[HWTE_LIGHTNINGBLOOD]	= 6,		/* pos */
	[HWTE_BIGGRENADE]	= 6,		/* pos */
	[HWTE_CHUNK]		= 6 + 6 + 1 + 1,	/* pos, vel, type, count */
	[HWTE_CHUNK2]		= 6 + 6 + 1,	/* pos, volume, type */
	[HWTE_HWBONEPOWER]	= 1 + 6 + 6,	/* count, pos, movedir */
	[HWTE_HWBONEPOWER2]	= 1 + 6,	/* hit, pos */
	[HWTE_METEORHIT]	= 6,
	[HWTE_HWRAVENDIE]	= 6,
	[HWTE_HWRAVENEXPLODE]	= 6,
	[HWTE_XBOWHIT]		= 6 + 6 + 1 + 1,	/* pos, vel, type, damage */
	[HWTE_ICEHIT]		= 6 + 1,	/* pos, person-or-wall */
	[HWTE_ICESTORM]		= 2,		/* entity */
	[HWTE_HWMISSILEFLASH]	= 6 + 4,	/* pos, two angle coords */
	[HWTE_LIGHTNING_HAMMER]	= 2,		/* entity */
	[HWTE_DRILLA_EXPLODE]	= 6,
	[HWTE_HWTELEPORT]	= 6 + 2,	/* pos, skin */
	[HWTE_SWORD_EXPLOSION]	= 6 + 2,	/* pos, entity */
	[HWTE_AXE_BOUNCE]	= 6,
	[HWTE_AXE_EXPLODE]	= 6,
	[HWTE_TIME_BOMB]	= 6,
	[HWTE_FIREBALL]		= 6,
	[HWTE_SUNSTAFF_POWER]	= 2 + 6 + 6,	/* entity, vel, pos */
	[HWTE_PURIFY2_EXPLODE]	= 6,
	[HWTE_PLAYER_DEATH]	= 6 + 4,	/* pos, angle pitch force style */
	[HWTE_PURIFY1_EFFECT]	= 6 + 1 + 1 + 2,	/* pos, angle, pitch, dist */
	[HWTE_TELEPORT_LINGER]	= 6 + 2,	/* pos, duration coord */
	[HWTE_LINE_EXPLOSION]	= 6 + 6,	/* start, end */
	[HWTE_METEOR_CRUSH]	= 6 + 4,	/* pos, long max distance */
	[HWTE_ACIDBALL]		= 6,
	[HWTE_ACIDBLOB]		= 6,
	[HWTE_FIREWALL]		= 6 + 3,	/* pos, angle, pitch, count */
	[HWTE_FIREWALL_IMPACT]	= 6,
	[HWTE_HWBONERIC]	= 6 + 1,	/* pos, count */
	[HWTE_POWERFLAME]	= 6 + 3 + 4,	/* pos, angle pitch count, long time */
	[HWTE_BLOODRAIN]	= 6 + 4,	/* pos, angle pitch length health */
	[HWTE_AXE]		= 6 + 3,	/* pos, angle, pitch, trail */
	[HWTE_PURIFY2_MISSILE]	= 6 + 3,
	[HWTE_SWORD_SHOT]	= 6 + 3,
	[HWTE_ICESHOT]		= 6 + 3,
	[HWTE_METEOR]		= 6 + 3,
	[HWTE_LIGHTNINGBALL]	= 6 + 1 + 1 + 2 + 1,	/* pos, ang, pitch, speed, trail */
	[HWTE_MEGAMETEOR]	= 6 + 3,
	[HWTE_CUBEBEAM]		= 2 + 2,	/* two entities */
	[HWTE_LIGHTNINGEXPLODE]	= 2 + 6,	/* entity, pos */
	[HWTE_ACID_BALL_FLY]	= 6 + 3,
	[HWTE_ACID_BLOB_FLY]	= 6 + 3,
};

/* Types the Hexen II renderer already handles with the same payload. */
static int HWCL_TEntHexen2Type (int type)
{
	switch (type)
	{
	case HWTE_SPIKE: case HWTE_SUPERSPIKE: case HWTE_EXPLOSION:
	case HWTE_LIGHTNING1: case HWTE_LIGHTNING2: case HWTE_LIGHTNING3:
	case HWTE_WIZSPIKE: case HWTE_KNIGHTSPIKE:
	case HWTE_LAVASPLASH: case HWTE_TELEPORT:
		return type;
	case HWTE_STREAM_LIGHTNING_SMALL:
		return HWTE_H2_STREAM_LIGHTNING_SMALL;
	default:
		if (type >= HWTE_STREAM_CHAIN && type <= HWTE_STREAM_FAMINE)
			return type;
		return -1;
	}
}

static void HWCL_SkipBytes (int count)
{
	while (count-- > 0 && !msg_badread)
		MSG_ReadByte ();
}

/* Returns false only for a type byte no HexenWorld server sends, where the
 * payload size is unknowable and the caller must drop the rest of the
 * packet rather than misread it. */
static qboolean HWCL_ParseTempEntity (void)
{
	vec3_t	pos;
	int	type, h2type, count, i;

	type = MSG_ReadByte ();
	if (msg_badread)
		return false;

	h2type = HWCL_TEntHexen2Type (type);
	if (h2type >= 0)
	{
		CL_ParseTEntType (h2type);
		return true;
	}

	switch (type)
	{
	case HWTE_GUNSHOT:
	case HWTE_BLOOD:
		count = MSG_ReadByte ();
		HWCL_ReadCoords (pos);
		if (!msg_badread)
			R_RunParticleEffect (pos, vec3_origin,
					type == HWTE_GUNSHOT ? 0 : 73, 20 * count);
		return true;

	case HWTE_LIGHTNINGBLOOD:
		HWCL_ReadCoords (pos);
		if (!msg_badread)
			R_RunParticleEffect (pos, vec3_origin, 225, 50);
		return true;

	case HWTE_TAREXPLOSION:
		HWCL_ReadCoords (pos);
		if (!msg_badread)
			R_BlobExplosion (pos);
		return true;

	case HWTE_SUNSTAFF_CHEAP:
		/* entity, reflect count, then 2 + reflect_count points.  The
		 * original client read these as coords when it found the entity
		 * and as shorts when it did not -- the same 2 bytes either way. */
		MSG_ReadShort ();
		count = MSG_ReadByte ();
		HWCL_SkipBytes ((2 + count) * 3 * 2);
		return true;

	case HWTE_CHAINLIGHTNING:
		/* entity, then points until an all-zero one, which ends the list
		 * and is itself on the wire (lightwp.hc writes 0 0 0 last). */
		MSG_ReadShort ();
		for (i = 0; !msg_badread; i++)
		{
			HWCL_ReadCoords (pos);
			if (!pos[0] && !pos[1] && !pos[2])
				break;
		}
		return true;

	default:
		if (type >= 0 && type < HWTE_COUNT && hwte_fixed_size[type])
		{
			HWCL_SkipBytes (hwte_fixed_size[type]);
			return true;
		}
		Con_DPrintf ("HexenWorld temp entity %d is not a HexenWorld type\n", type);
		return false;
	}
}
