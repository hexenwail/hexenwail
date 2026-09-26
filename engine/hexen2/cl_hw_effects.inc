/*
 * cl_hw_effects.inc -- HexenWorld client effect messages for the
 * integrated client: svc_start_effect (54), svc_turn_effect (62),
 * svc_update_effect (63) and svc_multieffect (64).
 *
 * Textually included by cl_hw.c.  GitHub #213.
 *
 * Split out of cl_hw.c so scripts/hw_ce_test.c can drive these readers
 * directly, the same way cl_hw_tent.inc is driven by hw_tent_test.c.
 *
 * The one hard duty here is byte accounting.  A reader that stops early
 * leaves the rest of the datagram unread -- and because HexenWorld puts
 * the multicast datagram after entities, and sends effects reliably, the
 * casualty is whatever followed: sounds, particles, svc_update_inv.  That
 * is the same failure svc_temp_entity had.
 *
 * Unlike the TE_* temp entities, effects are not written by gamecode.
 * The writers are engine code, and they are the contract:
 *
 *     svc_start_effect   SV_SendEffect        hexenworld/server/sv_effect.c
 *     svc_turn_effect    PF_turneffect        h2shared/pr_cmds.c
 *     svc_update_effect  PF_updateeffect      h2shared/pr_cmds.c
 *     svc_multieffect    SV_ParseMultiEffect  hexenworld/server/sv_effect.c
 *
 * scripts/hw_ce_layout_check.py re-derives every size from those four
 * functions and diffs them against what this file reads.
 */

/* quakedef.h already includes Hexen II's effects.h.  HexenWorld uses the
 * same names for a different numeric table, so keep the values local to this
 * protocol reader instead of letting the two headers collide. */
#undef CE_ACID_MUZZFL
#define CE_ACID_MUZZFL 58
#undef CE_FLAMESTREAM
#define CE_FLAMESTREAM 74
#undef CE_BLDRN_EXPL
#define CE_BLDRN_EXPL 57
#undef CE_ACID_HIT
#define CE_ACID_HIT 59
#undef CE_FIREWALL_SMALL
#define CE_FIREWALL_SMALL 60
#undef CE_FIREWALL_MEDIUM
#define CE_FIREWALL_MEDIUM 61
#undef CE_FIREWALL_LARGE
#define CE_FIREWALL_LARGE 62
#undef CE_LBALL_EXPL
#define CE_LBALL_EXPL 63
#undef CE_ACID_SPLAT
#define CE_ACID_SPLAT 64
#undef CE_ACID_EXPL
#define CE_ACID_EXPL 65
#undef CE_FBOOM
#define CE_FBOOM 66
#undef CE_BOMB
#define CE_BOMB 67
#undef CE_BRN_BOUNCE
#define CE_BRN_BOUNCE 68
#undef CE_LSHOCK
#define CE_LSHOCK 69
#undef CE_FLAMEWALL
#define CE_FLAMEWALL 70
#undef CE_FLAMEWALL2
#define CE_FLAMEWALL2 71
#undef CE_ONFIRE
#define CE_ONFIRE 73
#undef CE_FLOOR_EXPLOSION3
#define CE_FLOOR_EXPLOSION3 72
#define CE_RIPPLE 56
#define CE_SM_EXPLOSION2 50
#define CE_HWMISSILESTAR 42
#define CE_HWEIDOLONSTAR 43
#define CE_HWSHEEPINATOR 44
#define CE_TRIPMINE 45
#define CE_HWBONEBALL 46
#define CE_HWRAVENSTAFF 47
#define CE_TRIPMINESTILL 48
#define CE_SCARABCHAIN 49
#define CE_HWSPLITFLASH 51
#define CE_HWXBOWSHOOT 52
#define CE_HWRAVENPOWER 53
#define CE_HWDRILLA 54
#define CE_DEATHBUBBLES 55

/* Maintained Hexen II client IDs; never confuse these with HW wire values. */
enum
{
	HWCL_H2_BLDRN_EXPL = 45,
	HWCL_H2_ACID_MUZZFL,
	HWCL_H2_ACID_HIT,
	HWCL_H2_FIREWALL_SMALL,
	HWCL_H2_FIREWALL_MEDIUM,
	HWCL_H2_FIREWALL_LARGE,
	HWCL_H2_LBALL_EXPL,
	HWCL_H2_ACID_SPLAT,
	HWCL_H2_ACID_EXPL,
	HWCL_H2_FBOOM,
	HWCL_H2_BOMB = 56,
	HWCL_H2_BRN_BOUNCE,
	HWCL_H2_LSHOCK,
	HWCL_H2_FLAMEWALL,
	HWCL_H2_FLAMEWALL2,
	HWCL_H2_FLOOR_EXPLOSION3,
	HWCL_H2_ONFIRE,
	HWCL_H2_FLAMESTREAM = 42
};
static void HWCL_SkipXbowBolts (int turned)
{
	int i;

	for (i = 0; i < 5; i++)
	{
		if (turned & (1 << i))
		{
			HWCL_SkipCoords (3);
			HWCL_SkipAngles (2);
		}
	}
}

static int HWCL_TranslateEffectType (int type)
{
	if (type >= 1 && type <= 41)
		return type;
	switch (type)
	{
	case CE_HWMISSILESTAR: return CE_HW_MISSILESTAR;
	case CE_HWEIDOLONSTAR: return CE_HW_EIDOLONSTAR;
	case CE_HWSHEEPINATOR: return CE_HW_SHEEPINATOR;
	case CE_TRIPMINE: return CE_HW_TRIPMINE;
	case CE_HWBONEBALL: return CE_HW_BONEBALL;
	case CE_HWRAVENSTAFF: return CE_HW_RAVENSTAFF;
	case CE_TRIPMINESTILL: return CE_HW_TRIPMINESTILL;
	case CE_SCARABCHAIN: return CE_HW_SCARABCHAIN;
	case CE_SM_EXPLOSION2: return CE_SM_EXPLOSION;
	case CE_HWSPLITFLASH: return CE_SM_BLUE_FLASH;
	case CE_HWXBOWSHOOT: return CE_HW_XBOWSHOOT;
	case CE_HWRAVENPOWER: return CE_HW_RAVENPOWER;
	case CE_HWDRILLA: return CE_HW_DRILLA;
	case CE_DEATHBUBBLES: return CE_HW_DEATHBUBBLES;
	case CE_RIPPLE: return CE_HW_RIPPLE;
	case CE_BLDRN_EXPL: return HWCL_H2_BLDRN_EXPL;
	case CE_ACID_MUZZFL: return HWCL_H2_ACID_MUZZFL;
	case CE_ACID_HIT: return HWCL_H2_ACID_HIT;
	case CE_FIREWALL_SMALL: return HWCL_H2_FIREWALL_SMALL;
	case CE_FIREWALL_MEDIUM: return HWCL_H2_FIREWALL_MEDIUM;
	case CE_FIREWALL_LARGE: return HWCL_H2_FIREWALL_LARGE;
	case CE_LBALL_EXPL: return HWCL_H2_LBALL_EXPL;
	case CE_ACID_SPLAT: return HWCL_H2_ACID_SPLAT;
	case CE_ACID_EXPL: return HWCL_H2_ACID_EXPL;
	case CE_FBOOM: return HWCL_H2_FBOOM;
	case CE_BOMB: return HWCL_H2_BOMB;
	case CE_BRN_BOUNCE: return HWCL_H2_BRN_BOUNCE;
	case CE_LSHOCK: return HWCL_H2_LSHOCK;
	case CE_FLAMEWALL: return HWCL_H2_FLAMEWALL;
	case CE_FLAMEWALL2: return HWCL_H2_FLAMEWALL2;
	case CE_FLOOR_EXPLOSION3: return HWCL_H2_FLOOR_EXPLOSION3;
	case CE_ONFIRE: return HWCL_H2_ONFIRE;
	case CE_FLAMESTREAM: return HWCL_H2_FLAMESTREAM;
	default: return CE_NONE;
	}
}

static qboolean HWCL_ParseEffectPayload (int type)
{
	switch (type)
	{
	case CE_RAIN:
		HWCL_SkipCoords (12);
		MSG_ReadShort ();
		MSG_ReadShort ();
		MSG_ReadFloat ();
		break;
	case CE_FOUNTAIN:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (3);
		HWCL_SkipCoords (3);
		MSG_ReadShort ();
		MSG_ReadByte ();
		break;
	case CE_QUAKE:
		HWCL_SkipCoords (3);
		MSG_ReadFloat ();
		break;
	case CE_WHITE_SMOKE:
	case CE_GREEN_SMOKE:
	case CE_GREY_SMOKE:
	case CE_RED_SMOKE:
	case CE_SLOW_WHITE_SMOKE:
	case CE_TELESMK1:
	case CE_TELESMK2:
	case CE_GHOST:
	case CE_REDCLOUD:
	case CE_FLAMESTREAM:
	case CE_ACID_MUZZFL:
	case CE_FLAMEWALL:
	case CE_FLAMEWALL2:
	case CE_ONFIRE:
	case CE_RIPPLE:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (4);
		break;
	case CE_SM_WHITE_FLASH:
	case CE_YELLOWRED_FLASH:
	case CE_BLUESPARK:
	case CE_YELLOWSPARK:
	case CE_SM_CIRCLE_EXP:
	case CE_BG_CIRCLE_EXP:
	case CE_SM_EXPLOSION:
	case CE_SM_EXPLOSION2:
	case CE_LG_EXPLOSION:
	case CE_FLOOR_EXPLOSION:
	case CE_BLUE_EXPLOSION:
	case CE_REDSPARK:
	case CE_GREENSPARK:
	case CE_ICEHIT:
	case CE_MEDUSA_HIT:
	case CE_MEZZO_REFLECT:
	case CE_FLOOR_EXPLOSION2:
	case CE_XBOW_EXPLOSION:
	case CE_NEW_EXPLOSION:
	case CE_MAGIC_MISSILE_EXPLOSION:
	case CE_BONE_EXPLOSION:
	case CE_BLDRN_EXPL:
	case CE_ACID_HIT:
	case CE_ACID_SPLAT:
	case CE_ACID_EXPL:
	case CE_LBALL_EXPL:
	case CE_FIREWALL_SMALL:
	case CE_FIREWALL_MEDIUM:
	case CE_FIREWALL_LARGE:
	case CE_FBOOM:
	case CE_BOMB:
	case CE_BRN_BOUNCE:
	case CE_LSHOCK:
	case CE_FLOOR_EXPLOSION3:
		HWCL_SkipCoords (3);
		break;
	case CE_WHITE_FLASH:
	case CE_BLUE_FLASH:
	case CE_SM_BLUE_FLASH:
	case CE_HWSPLITFLASH:
	case CE_RED_FLASH:
	case CE_RIDER_DEATH:
	case CE_TELEPORTERPUFFS:
		HWCL_SkipCoords (3);
		break;
	case CE_TELEPORTERBODY:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (4);
		break;
	case CE_BONESHRAPNEL:
	case CE_HWBONEBALL:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (9);
		break;
	case CE_BONESHARD:
	case CE_HWRAVENSTAFF:
	case CE_HWMISSILESTAR:
	case CE_HWEIDOLONSTAR:
	case CE_HWRAVENPOWER:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (3);
		break;
	case CE_HWDRILLA:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		MSG_ReadShort ();
		break;
	case CE_DEATHBUBBLES:
		MSG_ReadShort ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		MSG_ReadByte ();
		break;
	case CE_SCARABCHAIN:
		HWCL_SkipCoords (3);
		MSG_ReadShort ();
		MSG_ReadByte ();
		break;
	case CE_TRIPMINESTILL:
	case CE_TRIPMINE:
		HWCL_SkipCoords (3);
		HWCL_SkipFloats (3);
		break;
	case CE_HWSHEEPINATOR:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		{
			int turned = MSG_ReadByte ();
			MSG_ReadByte ();
			HWCL_SkipXbowBolts (turned);
		}
		break;
	case CE_HWXBOWSHOOT:
		HWCL_SkipCoords (3);
		HWCL_SkipAngles (2);
		MSG_ReadByte ();
		MSG_ReadByte ();
		{
			int turned = MSG_ReadByte ();
			MSG_ReadByte ();
			HWCL_SkipXbowBolts (turned);
		}
		break;
	default:
		return false;
	}
	return !msg_badread;
}

/* These report from a network path that can repeat every frame, so each
 * distinct type is announced once per session and then stays quiet. */
static qboolean HWCL_EffectReportOnce (byte *seen, int type)
{
	if (type < 0 || type > 255 || seen[type])
		return false;
	seen[type] = 1;
	return true;
}

static qboolean HWCL_ParseStartEffect (void)
{
	static byte reported[256];
	int start = msg_readcount;
	int idx = MSG_ReadByte ();
	int wire_type = MSG_ReadByte ();
	int type = HWCL_TranslateEffectType (wire_type);
	int end;

	(void) idx;
	/* Consume the payload before deciding whether it can be drawn.  Its
	 * length follows from the wire type alone, so an effect this client has
	 * no renderer for still leaves the read cursor where the server stopped
	 * writing, and the rest of the datagram survives (GitHub #213). */
	if (!HWCL_ParseEffectPayload (wire_type))
	{
		if (!msg_badread && HWCL_EffectReportOnce (reported, wire_type))
			Con_Printf ("HexenWorld: no payload layout for start_effect type %d, "
					"dropping the rest of the packet\n", wire_type);
		return false;
	}
	end = msg_readcount;
	if (type == CE_NONE)
	{
		if (HWCL_EffectReportOnce (reported, wire_type))
			Con_DPrintf ("HexenWorld: start_effect type %d consumed but not drawn\n",
					wire_type);
		return true;
	}
	/* Validation above is deliberately complete before replacing the slot. */
	msg_readcount = start;
	CL_ParseHWEffect (type);
	msg_readcount = end;
	return true;
}

static qboolean HWCL_ParseUpdateEffect (void)
{
	static byte reported[256];
	int idx = MSG_ReadByte ();
	int wire_type = MSG_ReadByte ();
	int type = HWCL_TranslateEffectType (wire_type);
	int command = 0, extra = 0;
	float value = 0;
	vec3_t angles = {0, 0, 0}, origin = {0, 0, 0};

	switch (wire_type)
	{
	case CE_SCARABCHAIN:
		extra = MSG_ReadShort ();
		break;
	case CE_HWSHEEPINATOR:
	case CE_HWXBOWSHOOT:
		command = MSG_ReadByte ();
		if (command & 1)
			value = MSG_ReadCoord ();
		else
		{
			angles[0] = -MSG_ReadAngle ();
			angles[1] = MSG_ReadAngle ();
			if (command & 128)
				HWCL_ReadCoords (origin);
		}
		break;
	case CE_HWDRILLA:
		command = MSG_ReadByte ();
		if (!command)
		{
			HWCL_ReadCoords (origin);
			extra = MSG_ReadByte ();
		}
		else
		{
			angles[0] = -MSG_ReadAngle ();
			angles[1] = MSG_ReadAngle ();
			HWCL_ReadCoords (origin);
		}
		break;
	default:
		/* PF_updateeffect (engine/h2shared/pr_cmds.c) has no default arm:
		 * every type outside the three above writes the index and the type
		 * byte and no payload at all.  So there is nothing left to consume
		 * and the datagram can carry on being read. */
		if (HWCL_EffectReportOnce (reported, wire_type))
			Con_DPrintf ("HexenWorld: update_effect type %d carries no payload, ignored\n",
					wire_type);
		return !msg_badread;
	}
	if (msg_badread || type == CE_NONE)
		return !msg_badread;
	CL_UpdateHWEffect (idx, type, command, value, angles, origin, extra);
	return true;
}

static qboolean HWCL_ParseTurnEffect (void)
{
	int idx = MSG_ReadByte ();
	vec3_t origin, velocity;

	(void) MSG_ReadFloat (); /* legacy interpolation timestamp */
	HWCL_ReadCoords (origin);
	HWCL_ReadCoords (velocity);
	if (msg_badread)
		return false;
	CL_TurnHWEffect (idx, origin, velocity);
	return true;
}

static qboolean HWCL_ParseMultiEffect (void)
{
	static byte reported[256];
	int type = MSG_ReadByte ();
	int slots[3], i;
	vec3_t origin, velocity;

	if (type != CE_HWRAVENPOWER)
	{
		/* SV_ParseMultiEffect (engine/hexenworld/server/sv_effect.c) has a
		 * PR_RunError default arm, so CE_HWRAVENPOWER is the only type any
		 * HexenWorld server can put on the wire.  Anything else has no
		 * knowable payload length, which is the one case left where the
		 * rest of the packet has to go. */
		if (!msg_badread && HWCL_EffectReportOnce (reported, type))
			Con_Printf ("HexenWorld: multieffect type %d is not a HexenWorld type, "
					"dropping the rest of the packet\n", type);
		return false;
	}
	HWCL_ReadCoords (origin);
	HWCL_ReadCoords (velocity);
	for (i = 0; i < 3; i++)
		slots[i] = MSG_ReadByte ();
	if (msg_badread)
		return false;
	CL_MultiHWEffect (origin, velocity, slots);
	return true;
}
