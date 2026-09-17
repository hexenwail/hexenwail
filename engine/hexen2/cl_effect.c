/* cl_effect.c -- Client side effects.
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

// HEADER FILES ------------------------------------------------------------

#include "quakedef.h"

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

#define MAX_EFFECT_ENTITIES		256

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

static int NewEffectEntity (void);
static void FreeEffectEntity (int idx);
static void CL_ParseEffectInternal (int override_type, qboolean hexenworld);

static unsigned int hw_effect_seed;

static float CL_HWEffectRandom (void)
{
	hw_effect_seed = (hw_effect_seed * 877 + 573) % 9968;
	return (float)hw_effect_seed / 9968;
}

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

// PUBLIC DATA DEFINITIONS -------------------------------------------------

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static entity_t	EffectEntities[MAX_EFFECT_ENTITIES];
static qboolean	EntityUsed[MAX_EFFECT_ENTITIES];
static int	EffectEntityCount;

// CODE --------------------------------------------------------------------


//==========================================================================
//
// CL_InitTEnts
//
//==========================================================================

void CL_InitEffects (void)
{
}

void CL_ClearEffects (void)
{
	memset(cl.Effects, 0, sizeof(cl.Effects));
	memset(EntityUsed, 0, sizeof(EntityUsed));
	EffectEntityCount = 0;
}

static void CL_FreeEffect (int idx)
{
	int		i;

	switch (cl.Effects[idx].type)
	{
	case CE_RAIN:
		break;

	case CE_SNOW:
		break;

	case CE_FOUNTAIN:
		break;

	case CE_QUAKE:
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
	case CE_ACID_MUZZFL:
	case CE_FLAMESTREAM:
	case CE_FLAMEWALL:
	case CE_FLAMEWALL2:
	case CE_ONFIRE:
		FreeEffectEntity(cl.Effects[idx].ef.Smoke.entity_index);
		break;

	/* Just go through animation and then remove */
	case CE_SM_WHITE_FLASH:
	case CE_YELLOWRED_FLASH:
	case CE_BLUESPARK:
	case CE_YELLOWSPARK:
	case CE_SM_CIRCLE_EXP:
	case CE_BG_CIRCLE_EXP:
	case CE_SM_EXPLOSION:
	case CE_LG_EXPLOSION:
	case CE_FLOOR_EXPLOSION:
	case CE_FLOOR_EXPLOSION3:
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
	case CE_BRN_BOUNCE:
	case CE_LSHOCK:
	case CE_ACID_HIT:
	case CE_ACID_SPLAT:
	case CE_ACID_EXPL:
	case CE_LBALL_EXPL:
	case CE_FBOOM:
	case CE_BOMB:
	case CE_FIREWALL_SMALL:
	case CE_FIREWALL_MEDIUM:
	case CE_FIREWALL_LARGE:
		FreeEffectEntity(cl.Effects[idx].ef.Smoke.entity_index);
		break;

	/* Go forward then backward through animation then remove */
	case CE_WHITE_FLASH:
	case CE_BLUE_FLASH:
	case CE_SM_BLUE_FLASH:
	case CE_RED_FLASH:
		FreeEffectEntity(cl.Effects[idx].ef.Flash.entity_index);
		break;

	case CE_RIDER_DEATH:
		break;

	case CE_GRAVITYWELL:
		break;

	case CE_TELEPORTERPUFFS:
		for (i = 0 ; i < 8 ; ++i)
			FreeEffectEntity(cl.Effects[idx].ef.Teleporter.entity_index[i]);
		break;

	case CE_TELEPORTERBODY:
		FreeEffectEntity(cl.Effects[idx].ef.Teleporter.entity_index[0]);
		break;

	case CE_BONESHARD:
	case CE_BONESHRAPNEL:
	case CE_HW_BONEBALL:
	case CE_HW_RAVENSTAFF:
	case CE_HW_RAVENPOWER:
	case CE_HW_DRILLA:
		FreeEffectEntity(cl.Effects[idx].ef.Missile.entity_index);
		break;

	case CE_HW_RIPPLE:
		FreeEffectEntity(cl.Effects[idx].ef.Smoke.entity_index);
		break;

	case CE_HW_TRIPMINE:
	case CE_HW_TRIPMINESTILL:
	case CE_HW_SCARABCHAIN:
		FreeEffectEntity(cl.Effects[idx].ef.Chain.ent1);
		break;

	case CE_HW_XBOWSHOOT:
	case CE_HW_SHEEPINATOR:
		for (i = 0; i < cl.Effects[idx].ef.Xbow.bolts; ++i)
			FreeEffectEntity(cl.Effects[idx].ef.Xbow.ent[i]);
		break;

	case CE_HW_MISSILESTAR:
	case CE_HW_EIDOLONSTAR:
		FreeEffectEntity(cl.Effects[idx].ef.Star.entity_index);
		FreeEffectEntity(cl.Effects[idx].ef.Star.ent1);
		FreeEffectEntity(cl.Effects[idx].ef.Star.ent2);
		break;

	case CE_HW_DEATHBUBBLES:
		break;

	case CE_CHUNK:
		for (i = 0 ; i < cl.Effects[idx].ef.Chunk.numChunks ; i++)
			FreeEffectEntity(cl.Effects[idx].ef.Chunk.entity_index[i]);
		break;
	}

	memset(&cl.Effects[idx], 0, sizeof(struct EffectT));
}

//==========================================================================
//
// CL_ParseEffect
//
//==========================================================================

// All changes need to be in SV_SendEffect(), SV_ParseEffect(),
// SV_SaveEffects(), SV_LoadEffects(), CL_ParseEffect()
void CL_ParseEffect (void)
{
	CL_ParseEffectInternal (-1, false);
}

void CL_ParseHWEffect (int type)
{
	CL_ParseEffectInternal (type, true);
}

static void CL_VectorToAngles (const vec3_t vec, vec3_t angles)
{
	float forward;

	if (vec[0] == 0 && vec[1] == 0)
	{
		angles[1] = 0;
		angles[0] = vec[2] > 0 ? 90 : 270;
	}
	else
	{
		angles[1] = atan2 (vec[1], vec[0]) * 180 / M_PI;
		if (angles[1] < 0)
			angles[1] += 360;
		forward = Q_sqrt (vec[0] * vec[0] + vec[1] * vec[1]);
		angles[0] = atan2 (vec[2], forward) * 180 / M_PI;
		if (angles[0] < 0)
			angles[0] += 360;
	}
	angles[2] = 0;
}

static void CL_ParseEffectInternal (int override_type, qboolean hexenworld)
{
	int		i, idx, wire_type;
	qboolean	ImmediateFree;
	entity_t	*ent;
	int		dir;
	float		sinval, cosval;
	float		skinnum;
	float		final;
	vec3_t		origin, forward, forward2, right, up, bolt_angles, side;

	ImmediateFree = false;

	idx = MSG_ReadByte();
	wire_type = MSG_ReadByte();
	if (cl.Effects[idx].type)
		CL_FreeEffect(idx);

	memset(&cl.Effects[idx], 0, sizeof(struct EffectT));

	cl.Effects[idx].type = override_type >= 0 ? override_type : wire_type;

	switch (cl.Effects[idx].type)
	{
	case CE_RAIN:
		cl.Effects[idx].ef.Rain.min_org[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.min_org[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.min_org[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.e_size[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.e_size[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.e_size[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.dir[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.dir[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.dir[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.color = MSG_ReadShort();
		cl.Effects[idx].ef.Rain.count = MSG_ReadShort();
		cl.Effects[idx].ef.Rain.wait = MSG_ReadFloat();
		break;

	case CE_SNOW:
		cl.Effects[idx].ef.Rain.min_org[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.min_org[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.min_org[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.max_org[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.flags = MSG_ReadByte();
		cl.Effects[idx].ef.Rain.dir[0] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.dir[1] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.dir[2] = MSG_ReadCoord();
		cl.Effects[idx].ef.Rain.count = MSG_ReadByte();
		//cl.Effects[idx].ef.Rain.veer = MSG_ReadShort();
		break;

	case CE_FOUNTAIN:
		cl.Effects[idx].ef.Fountain.pos[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.pos[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.pos[2] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.angle[0] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Fountain.angle[1] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Fountain.angle[2] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Fountain.movedir[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.movedir[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.movedir[2] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Fountain.color = MSG_ReadShort ();
		cl.Effects[idx].ef.Fountain.cnt = MSG_ReadByte ();
		AngleVectors (cl.Effects[idx].ef.Fountain.angle,
				cl.Effects[idx].ef.Fountain.vforward,
				cl.Effects[idx].ef.Fountain.vright,
				cl.Effects[idx].ef.Fountain.vup);
		break;

	case CE_QUAKE:
		cl.Effects[idx].ef.Quake.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Quake.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Quake.origin[2] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Quake.radius = MSG_ReadFloat ();
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
	case CE_ACID_MUZZFL:
	case CE_FLAMESTREAM:
	case CE_FLAMEWALL:
	case CE_FLAMEWALL2:
	case CE_ONFIRE:
		cl.Effects[idx].ef.Smoke.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Smoke.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Smoke.origin[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Smoke.velocity[0] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Smoke.velocity[1] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Smoke.velocity[2] = MSG_ReadFloat ();

		cl.Effects[idx].ef.Smoke.framelength = MSG_ReadFloat ();
		/* smoke frame is a mission pack thing only. */
		if (!hexenworld && cl_protocol > PROTOCOL_RAVEN_111)
			cl.Effects[idx].ef.Smoke.frame = MSG_ReadFloat ();

		if ((cl.Effects[idx].ef.Smoke.entity_index = NewEffectEntity()) != -1)
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];
			VectorCopy(cl.Effects[idx].ef.Smoke.origin, ent->origin);

			if ((cl.Effects[idx].type == CE_WHITE_SMOKE) ||
					(cl.Effects[idx].type == CE_SLOW_WHITE_SMOKE))
				ent->model = Mod_ForName("models/whtsmk1.spr", true);
			else if (cl.Effects[idx].type == CE_GREEN_SMOKE)
				ent->model = Mod_ForName("models/grnsmk1.spr", true);
			else if (cl.Effects[idx].type == CE_GREY_SMOKE)
				ent->model = Mod_ForName("models/grysmk1.spr", true);
			else if (cl.Effects[idx].type == CE_RED_SMOKE)
				ent->model = Mod_ForName("models/redsmk1.spr", true);
			else if (cl.Effects[idx].type == CE_TELESMK1)
				ent->model = Mod_ForName("models/telesmk1.spr", true);
			else if (cl.Effects[idx].type == CE_TELESMK2)
				ent->model = Mod_ForName("models/telesmk2.spr", true);
			else if (cl.Effects[idx].type == CE_REDCLOUD)
				ent->model = Mod_ForName("models/rcloud.spr", true);
			else if (cl.Effects[idx].type == CE_FLAMESTREAM)
				ent->model = Mod_ForName("models/flamestr.spr", true);
			else if (cl.Effects[idx].type == CE_ACID_MUZZFL)
			{
				ent->model = Mod_ForName("models/muzzle1.spr", true);
				ent->drawflags = DRF_TRANSLUCENT | MLS_ABSLIGHT;
				ent->abslight = 51;
			}
			else if (cl.Effects[idx].type == CE_FLAMEWALL)
				ent->model = Mod_ForName("models/firewal1.spr", true);
			else if (cl.Effects[idx].type == CE_FLAMEWALL2)
				ent->model = Mod_ForName("models/firewal2.spr", true);
			else if (cl.Effects[idx].type == CE_ONFIRE)
			{
				float	rdm = rand() & 3;

				if (rdm < 1)
					ent->model = Mod_ForName("models/firewal1.spr", true);
				else if (rdm < 2)
					ent->model = Mod_ForName("models/firewal2.spr", true);
				else
					ent->model = Mod_ForName("models/firewal3.spr", true);

				ent->drawflags = DRF_TRANSLUCENT;
				ent->abslight = 255;
				ent->frame = cl.Effects[idx].ef.Smoke.frame;
			}

			if (cl.Effects[idx].type != CE_REDCLOUD &&
					cl.Effects[idx].type != CE_ACID_MUZZFL &&
					cl.Effects[idx].type != CE_FLAMEWALL)
				ent->drawflags = DRF_TRANSLUCENT;

			if (cl.Effects[idx].type == CE_FLAMESTREAM)
			{
				ent->drawflags = DRF_TRANSLUCENT | MLS_ABSLIGHT;
				ent->abslight = 255;
				ent->frame = cl.Effects[idx].ef.Smoke.frame;
			}

			if (cl.Effects[idx].type == CE_GHOST)
			{
				ent->model = Mod_ForName("models/ghost.spr", true);
				ent->drawflags = DRF_TRANSLUCENT | MLS_ABSLIGHT;
				ent->abslight = 127;
			}
		}
		else
			ImmediateFree = true;
		break;

	case CE_SM_WHITE_FLASH:
	case CE_YELLOWRED_FLASH:
	case CE_BLUESPARK:
	case CE_YELLOWSPARK:
	case CE_SM_CIRCLE_EXP:
	case CE_BG_CIRCLE_EXP:
	case CE_SM_EXPLOSION:
	case CE_LG_EXPLOSION:
	case CE_FLOOR_EXPLOSION:
	case CE_FLOOR_EXPLOSION3:
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
	case CE_BRN_BOUNCE:
	case CE_LSHOCK:
	case CE_ACID_HIT:
	case CE_ACID_SPLAT:
	case CE_ACID_EXPL:
	case CE_LBALL_EXPL:
	case CE_FBOOM:
	case CE_BOMB:
	case CE_FIREWALL_SMALL:
	case CE_FIREWALL_MEDIUM:
	case CE_FIREWALL_LARGE:
		cl.Effects[idx].ef.Smoke.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Smoke.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Smoke.origin[2] = MSG_ReadCoord ();
		if ((cl.Effects[idx].ef.Smoke.entity_index = NewEffectEntity()) != -1)
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];
			VectorCopy(cl.Effects[idx].ef.Smoke.origin, ent->origin);

			if (cl.Effects[idx].type == CE_BLUESPARK)
				ent->model = Mod_ForName("models/bspark.spr", true);
			else if (cl.Effects[idx].type == CE_YELLOWSPARK)
				ent->model = Mod_ForName("models/spark.spr", true);
			else if (cl.Effects[idx].type == CE_SM_CIRCLE_EXP)
				ent->model = Mod_ForName("models/fcircle.spr", true);
			else if (cl.Effects[idx].type == CE_BG_CIRCLE_EXP)
				ent->model = Mod_ForName("models/xplod29.spr", true);
			else if (cl.Effects[idx].type == CE_SM_WHITE_FLASH)
				ent->model = Mod_ForName("models/sm_white.spr", true);
			else if (cl.Effects[idx].type == CE_YELLOWRED_FLASH)
			{
				ent->model = Mod_ForName("models/yr_flsh.spr", true);
				ent->drawflags = DRF_TRANSLUCENT;
			}
			else if (cl.Effects[idx].type == CE_SM_EXPLOSION)
				ent->model = Mod_ForName("models/sm_expld.spr", true);
			else if (cl.Effects[idx].type == CE_LG_EXPLOSION)
				ent->model = Mod_ForName("models/bg_expld.spr", true);
			else if (cl.Effects[idx].type == CE_FLOOR_EXPLOSION)
				ent->model = Mod_ForName("models/fl_expld.spr", true);
			else if (cl.Effects[idx].type == CE_FLOOR_EXPLOSION3)
				ent->model = Mod_ForName("models/biggy.spr", true);
			else if (cl.Effects[idx].type == CE_BLUE_EXPLOSION)
				ent->model = Mod_ForName("models/xpspblue.spr", true);
			else if (cl.Effects[idx].type == CE_REDSPARK)
				ent->model = Mod_ForName("models/rspark.spr", true);
			else if (cl.Effects[idx].type == CE_GREENSPARK)
				ent->model = Mod_ForName("models/gspark.spr", true);
			else if (cl.Effects[idx].type == CE_ICEHIT)
				ent->model = Mod_ForName("models/icehit.spr", true);
			else if (cl.Effects[idx].type == CE_MEDUSA_HIT)
				ent->model = Mod_ForName("models/medhit.spr", true);
			else if (cl.Effects[idx].type == CE_MEZZO_REFLECT)
				ent->model = Mod_ForName("models/mezzoref.spr", true);
			else if (cl.Effects[idx].type == CE_FLOOR_EXPLOSION2)
				ent->model = Mod_ForName("models/flrexpl2.spr", true);
			else if (cl.Effects[idx].type == CE_XBOW_EXPLOSION)
				ent->model = Mod_ForName("models/xbowexpl.spr", true);
			else if (cl.Effects[idx].type == CE_NEW_EXPLOSION)
				ent->model = Mod_ForName("models/gen_expl.spr", true);
			else if (cl.Effects[idx].type == CE_MAGIC_MISSILE_EXPLOSION)
				ent->model = Mod_ForName("models/mm_expld.spr", true);
			else if (cl.Effects[idx].type == CE_BONE_EXPLOSION)
				ent->model = Mod_ForName("models/bonexpld.spr", true);
			else if (cl.Effects[idx].type == CE_BLDRN_EXPL)
				ent->model = Mod_ForName("models/xplsn_1.spr", true);
			else if (cl.Effects[idx].type == CE_ACID_HIT)
				ent->model = Mod_ForName("models/axplsn_2.spr", true);
			else if (cl.Effects[idx].type == CE_ACID_SPLAT)
				ent->model = Mod_ForName("models/axplsn_1.spr", true);
			else if (cl.Effects[idx].type == CE_ACID_EXPL)
			{
				ent->model = Mod_ForName("models/axplsn_5.spr", true);
				ent->drawflags = MLS_ABSLIGHT;
				ent->abslight = 255;
			}
			else if (cl.Effects[idx].type == CE_FBOOM)
				ent->model = Mod_ForName("models/fboom.spr", true);
			else if (cl.Effects[idx].type == CE_BOMB)
				ent->model = Mod_ForName("models/pow.spr", true);
			else if (cl.Effects[idx].type == CE_LBALL_EXPL)
				ent->model = Mod_ForName("models/Bluexp3.spr", true);
			else if (cl.Effects[idx].type == CE_FIREWALL_SMALL)
				ent->model = Mod_ForName("models/firewal1.spr", true);
			else if (cl.Effects[idx].type == CE_FIREWALL_MEDIUM)
				ent->model = Mod_ForName("models/firewal5.spr", true);
			else if (cl.Effects[idx].type == CE_FIREWALL_LARGE)
				ent->model = Mod_ForName("models/firewal4.spr", true);
			else if (cl.Effects[idx].type == CE_BRN_BOUNCE)
				ent->model = Mod_ForName("models/spark.spr", true);
			else if (cl.Effects[idx].type == CE_LSHOCK)
			{
				ent->model = Mod_ForName("models/vorpshok.mdl", true);
				ent->drawflags = MLS_TORCH;
				ent->angles[2] = 90;
				ent->scale = 255;
			}
		}
		else
		{
			ImmediateFree = true;
		}
		break;

	case CE_WHITE_FLASH:
	case CE_BLUE_FLASH:
	case CE_SM_BLUE_FLASH:
	case CE_RED_FLASH:
		cl.Effects[idx].ef.Flash.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Flash.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Flash.origin[2] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Flash.reverse = 0;
		if ((cl.Effects[idx].ef.Flash.entity_index = NewEffectEntity()) != -1)
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Flash.entity_index];
			VectorCopy(cl.Effects[idx].ef.Flash.origin, ent->origin);

			if (cl.Effects[idx].type == CE_WHITE_FLASH)
				ent->model = Mod_ForName("models/gryspt.spr", true);
			else if (cl.Effects[idx].type == CE_BLUE_FLASH)
				ent->model = Mod_ForName("models/bluflash.spr", true);
			else if (cl.Effects[idx].type == CE_SM_BLUE_FLASH)
				ent->model = Mod_ForName("models/sm_blue.spr", true);
			else if (cl.Effects[idx].type == CE_RED_FLASH)
				ent->model = Mod_ForName("models/redspt.spr", true);

			ent->drawflags = DRF_TRANSLUCENT;
		}
		else
		{
			ImmediateFree = true;
		}
		break;

	case CE_RIDER_DEATH:
		cl.Effects[idx].ef.RD.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.RD.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.RD.origin[2] = MSG_ReadCoord ();
		break;

	case CE_GRAVITYWELL:
		cl.Effects[idx].ef.RD.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.RD.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.RD.origin[2] = MSG_ReadCoord ();
		cl.Effects[idx].ef.RD.color = MSG_ReadShort ();
		cl.Effects[idx].ef.RD.lifetime = MSG_ReadFloat ();
		break;

	case CE_TELEPORTERPUFFS:
		cl.Effects[idx].ef.Teleporter.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Teleporter.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Teleporter.origin[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Teleporter.framelength = .05;
		dir = 0;
		for (i = 0 ; i < 8 ; ++i)
		{
			if ((cl.Effects[idx].ef.Teleporter.entity_index[i] = NewEffectEntity()) != -1)
			{
				ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[i]];
				VectorCopy(cl.Effects[idx].ef.Teleporter.origin, ent->origin);

				q_sincosdeg(dir, &sinval, &cosval);

				cl.Effects[idx].ef.Teleporter.velocity[i][0] = 10*cosval;
				cl.Effects[idx].ef.Teleporter.velocity[i][1] = 10*sinval;
				cl.Effects[idx].ef.Teleporter.velocity[i][2] = 0;
				dir += 45;

				ent->model = Mod_ForName("models/telesmk2.spr", true);
				ent->drawflags = DRF_TRANSLUCENT;
			}
			else
			{
				ImmediateFree = true;
				break;
			}
		}
		if (ImmediateFree) {
			for (i = 0 ; i < 8 ; ++i) {
				if (cl.Effects[idx].ef.Teleporter.entity_index[i] == -1)
					break;
				FreeEffectEntity(cl.Effects[idx].ef.Teleporter.entity_index[i]);
			}
			break;
		}
		break;

	case CE_TELEPORTERBODY:
		cl.Effects[idx].ef.Teleporter.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Teleporter.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Teleporter.origin[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Teleporter.velocity[0][0] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Teleporter.velocity[0][1] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Teleporter.velocity[0][2] = MSG_ReadFloat ();

		skinnum = MSG_ReadFloat ();

		cl.Effects[idx].ef.Teleporter.framelength = .05;
		dir = 0;
		if ((cl.Effects[idx].ef.Teleporter.entity_index[0] = NewEffectEntity()) != -1)
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[0]];
			VectorCopy(cl.Effects[idx].ef.Teleporter.origin, ent->origin);

			ent->model = Mod_ForName("models/teleport.mdl", true);
			ent->drawflags = SCALE_TYPE_XYONLY | DRF_TRANSLUCENT;
			ent->scale = 100;
			ent->skinnum = skinnum;
		}
		else
		{
			ImmediateFree = true;
		}
		break;

	case CE_BONESHARD:
	case CE_BONESHRAPNEL:
		cl.Effects[idx].ef.Missile.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Missile.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Missile.origin[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Missile.velocity[0] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Missile.velocity[1] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Missile.velocity[2] = MSG_ReadFloat ();

		if (hexenworld && cl.Effects[idx].type == CE_BONESHARD)
		{
			CL_VectorToAngles (cl.Effects[idx].ef.Missile.velocity,
					cl.Effects[idx].ef.Missile.angle);
			cl.Effects[idx].ef.Missile.avelocity[0] = (rand() % 1554) - 777;
		}
		else
		{
			cl.Effects[idx].ef.Missile.angle[0] = MSG_ReadFloat ();
			cl.Effects[idx].ef.Missile.angle[1] = MSG_ReadFloat ();
			cl.Effects[idx].ef.Missile.angle[2] = MSG_ReadFloat ();

			cl.Effects[idx].ef.Missile.avelocity[0] = MSG_ReadFloat ();
			cl.Effects[idx].ef.Missile.avelocity[1] = MSG_ReadFloat ();
			cl.Effects[idx].ef.Missile.avelocity[2] = MSG_ReadFloat ();
		}

		if ((cl.Effects[idx].ef.Missile.entity_index = NewEffectEntity()) != -1)
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
			VectorCopy(cl.Effects[idx].ef.Missile.origin, ent->origin);
			if (cl.Effects[idx].type == CE_BONESHARD)
				ent->model = Mod_ForName("models/boneshot.mdl", true);
			else if (cl.Effects[idx].type == CE_BONESHRAPNEL)
				ent->model = Mod_ForName("models/boneshrd.mdl", true);
		}
		else
			ImmediateFree = true;
		break;

	case CE_HW_RIPPLE:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Smoke.origin[i] = MSG_ReadCoord ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Smoke.velocity[i] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Smoke.framelength = MSG_ReadFloat ();
		cl.Effects[idx].ef.Smoke.entity_index = NewEffectEntity ();
		if (cl.Effects[idx].ef.Smoke.entity_index == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];
			VectorCopy (cl.Effects[idx].ef.Smoke.origin, ent->origin);
			ent->model = Mod_ForName ("models/ripple.spr", true);
			ent->drawflags = DRF_TRANSLUCENT;
			ent->angles[0] = 90;
			cl.Effects[idx].ef.Smoke.framelength = 0.05;
		}
		break;

	case CE_HW_BONEBALL:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.origin[i] = MSG_ReadCoord ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.velocity[i] = MSG_ReadFloat ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.angle[i] = MSG_ReadFloat ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.avelocity[i] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Missile.entity_index = NewEffectEntity ();
		if (cl.Effects[idx].ef.Missile.entity_index == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
			VectorCopy (cl.Effects[idx].ef.Missile.origin, ent->origin);
			VectorCopy (cl.Effects[idx].ef.Missile.angle, ent->angles);
			ent->model = Mod_ForName ("models/bonelump.mdl", true);
		}
		break;

	case CE_HW_RAVENSTAFF:
	case CE_HW_RAVENPOWER:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.origin[i] = MSG_ReadCoord ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.velocity[i] = MSG_ReadFloat ();
		CL_VectorToAngles (cl.Effects[idx].ef.Missile.velocity,
				cl.Effects[idx].ef.Missile.angle);
		cl.Effects[idx].ef.Missile.entity_index = NewEffectEntity ();
		if (cl.Effects[idx].ef.Missile.entity_index == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
			VectorCopy (cl.Effects[idx].ef.Missile.origin, ent->origin);
			VectorCopy (cl.Effects[idx].ef.Missile.angle, ent->angles);
			if (cl.Effects[idx].type == CE_HW_RAVENSTAFF)
			{
				cl.Effects[idx].ef.Missile.avelocity[2] = 1000;
				ent->model = Mod_ForName ("models/vindsht1.mdl", true);
			}
			else
				ent->model = Mod_ForName ("models/ravproj.mdl", true);
		}
		break;

	case CE_HW_DRILLA:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Missile.origin[i] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Missile.angle[0] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Missile.angle[1] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Missile.speed = MSG_ReadShort ();
		AngleVectors (cl.Effects[idx].ef.Missile.angle, forward, right, up);
		VectorScale (forward, cl.Effects[idx].ef.Missile.speed,
				cl.Effects[idx].ef.Missile.velocity);
		cl.Effects[idx].ef.Missile.entity_index = NewEffectEntity ();
		if (cl.Effects[idx].ef.Missile.entity_index == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
			VectorCopy (cl.Effects[idx].ef.Missile.origin, ent->origin);
			VectorCopy (cl.Effects[idx].ef.Missile.angle, ent->angles);
			ent->model = Mod_ForName ("models/scrbstp1.mdl", true);
		}
		break;

	case CE_HW_TRIPMINE:
	case CE_HW_TRIPMINESTILL:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Chain.origin[i] = MSG_ReadCoord ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Chain.velocity[i] = MSG_ReadFloat ();
		cl.Effects[idx].ef.Chain.ent1 = NewEffectEntity ();
		if (cl.Effects[idx].ef.Chain.ent1 == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Chain.ent1];
			/* HW encodes the stationary spike endpoint in this vector. */
			if (cl.Effects[idx].type == CE_HW_TRIPMINESTILL)
				VectorCopy (cl.Effects[idx].ef.Chain.velocity, ent->origin);
			else
				VectorCopy (cl.Effects[idx].ef.Chain.origin, ent->origin);
			ent->model = Mod_ForName ("models/twspike.mdl", true);
		}
		break;

	case CE_HW_SCARABCHAIN:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Chain.origin[i] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Chain.owner = MSG_ReadShort ();
		cl.Effects[idx].ef.Chain.tag = MSG_ReadByte ();
		cl.Effects[idx].ef.Chain.material = cl.Effects[idx].ef.Chain.owner >> 12;
		cl.Effects[idx].ef.Chain.owner &= 0xfff;
		cl.Effects[idx].ef.Chain.height = 16;
		cl.Effects[idx].ef.Chain.ent1 = NewEffectEntity ();
		if (cl.Effects[idx].ef.Chain.ent1 == -1)
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Chain.ent1];
			VectorCopy (cl.Effects[idx].ef.Chain.origin, ent->origin);
			ent->model = Mod_ForName ("models/scrbpbdy.mdl", true);
		}
		break;

	case CE_HW_XBOWSHOOT:
	case CE_HW_SHEEPINATOR:
		for (i = 0; i < 3; ++i)
			origin[i] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Xbow.angle[0] = MSG_ReadAngle ();
		cl.Effects[idx].ef.Xbow.angle[1] = MSG_ReadAngle ();
		if (cl.Effects[idx].type == CE_HW_XBOWSHOOT)
		{
			cl.Effects[idx].ef.Xbow.bolts = MSG_ReadByte ();
			cl.Effects[idx].ef.Xbow.randseed = MSG_ReadByte ();
		}
		else
			cl.Effects[idx].ef.Xbow.bolts = 5;
		cl.Effects[idx].ef.Xbow.turnedbolts = MSG_ReadByte () & 31;
		cl.Effects[idx].ef.Xbow.activebolts = MSG_ReadByte () & 31;
		if (cl.Effects[idx].ef.Xbow.bolts > 5)
			cl.Effects[idx].ef.Xbow.bolts = 5;
		AngleVectors (cl.Effects[idx].ef.Xbow.angle, forward, right, up);
		VectorNormalizeFast (forward);
		hw_effect_seed = cl.Effects[idx].ef.Xbow.randseed;
		VectorCopy (forward, cl.Effects[idx].ef.Xbow.velocity);
		for (i = 0; i < 5; ++i)
			cl.Effects[idx].ef.Xbow.ent[i] = -1;
		for (i = 0; i < cl.Effects[idx].ef.Xbow.bolts; ++i)
		{
			cl.Effects[idx].ef.Xbow.gonetime[i] = 1 + CL_HWEffectRandom () * 2;
			if (cl.Effects[idx].ef.Xbow.turnedbolts & (1 << i))
			{
				cl.Effects[idx].ef.Xbow.origin[i][0] = MSG_ReadCoord ();
				cl.Effects[idx].ef.Xbow.origin[i][1] = MSG_ReadCoord ();
				cl.Effects[idx].ef.Xbow.origin[i][2] = MSG_ReadCoord ();
				bolt_angles[0] = MSG_ReadAngle ();
				bolt_angles[1] = MSG_ReadAngle ();
				bolt_angles[2] = 0;
				AngleVectors (bolt_angles, forward2, NULL, NULL);
				VectorScale (forward2,
						cl.Effects[idx].type == CE_HW_SHEEPINATOR ? 700 :
						800 + CL_HWEffectRandom () * 500,
						cl.Effects[idx].ef.Xbow.vel[i]);
			}
			else
			{
				VectorCopy (origin, cl.Effects[idx].ef.Xbow.origin[i]);
				VectorScale (forward,
						cl.Effects[idx].type == CE_HW_SHEEPINATOR ? 700 :
						800 + CL_HWEffectRandom () * 500,
						cl.Effects[idx].ef.Xbow.vel[i]);
				VectorScale (right,
						i * (cl.Effects[idx].type == CE_HW_SHEEPINATOR ? 75 : 100) -
						(cl.Effects[idx].type == CE_HW_SHEEPINATOR ? 150 :
						 (cl.Effects[idx].ef.Xbow.bolts - 1) * 50), side);
				if (cl.Effects[idx].type == CE_HW_XBOWSHOOT)
				{
					VectorScale (side, 0.333, side);
					VectorMA (origin, 0.05, side,
							cl.Effects[idx].ef.Xbow.origin[i]);
				}
				VectorAdd (cl.Effects[idx].ef.Xbow.vel[i], side,
						cl.Effects[idx].ef.Xbow.vel[i]);
			}
			cl.Effects[idx].ef.Xbow.ent[i] = NewEffectEntity ();
			if (cl.Effects[idx].ef.Xbow.ent[i] == -1)
			{
				ImmediateFree = true;
				break;
			}
			ent = &EffectEntities[cl.Effects[idx].ef.Xbow.ent[i]];
			VectorCopy (cl.Effects[idx].ef.Xbow.origin[i], ent->origin);
			CL_VectorToAngles (cl.Effects[idx].ef.Xbow.vel[i], ent->angles);
			ent->model = Mod_ForName (
				cl.Effects[idx].type == CE_HW_SHEEPINATOR ?
				"models/polymrph.spr" :
				(cl.Effects[idx].ef.Xbow.bolts == 5 ?
				 "models/flaming.mdl" : "models/arrow.mdl"), true);
		}
		break;

	case CE_HW_DEATHBUBBLES:
		cl.Effects[idx].ef.Bubble.owner = MSG_ReadShort ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Bubble.offset[i] = MSG_ReadByte ();
		cl.Effects[idx].ef.Bubble.count = MSG_ReadByte ();
		break;

	case CE_HW_MISSILESTAR:
	case CE_HW_EIDOLONSTAR:
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Star.origin[i] = MSG_ReadCoord ();
		for (i = 0; i < 3; ++i)
			cl.Effects[idx].ef.Star.velocity[i] = MSG_ReadFloat ();
		CL_VectorToAngles (cl.Effects[idx].ef.Star.velocity,
				cl.Effects[idx].ef.Star.angle);
		cl.Effects[idx].ef.Star.avelocity[2] = 300 + rand() % 300;
		cl.Effects[idx].ef.Star.entity_index = NewEffectEntity ();
		cl.Effects[idx].ef.Star.ent1 = NewEffectEntity ();
		cl.Effects[idx].ef.Star.ent2 = cl.Effects[idx].type == CE_HW_MISSILESTAR ?
			NewEffectEntity () : -1;
		if (cl.Effects[idx].ef.Star.entity_index == -1 ||
				cl.Effects[idx].ef.Star.ent1 == -1 ||
				(cl.Effects[idx].type == CE_HW_MISSILESTAR &&
				 cl.Effects[idx].ef.Star.ent2 == -1))
			ImmediateFree = true;
		else
		{
			ent = &EffectEntities[cl.Effects[idx].ef.Star.entity_index];
			VectorCopy (cl.Effects[idx].ef.Star.origin, ent->origin);
			VectorCopy (cl.Effects[idx].ef.Star.angle, ent->angles);
			ent->model = Mod_ForName ("models/ball.mdl", true);
			ent = &EffectEntities[cl.Effects[idx].ef.Star.ent1];
			VectorCopy (cl.Effects[idx].ef.Star.origin, ent->origin);
			ent->model = Mod_ForName (cl.Effects[idx].type == CE_HW_MISSILESTAR ?
					"models/star.mdl" : "models/glowball.mdl", true);
			ent->drawflags = MLS_ABSLIGHT;
			ent->abslight = 127;
			if (cl.Effects[idx].ef.Star.ent2 != -1)
			{
				ent = &EffectEntities[cl.Effects[idx].ef.Star.ent2];
				VectorCopy (cl.Effects[idx].ef.Star.origin, ent->origin);
				ent->model = Mod_ForName ("models/star.mdl", true);
				ent->drawflags = MLS_ABSLIGHT;
				ent->abslight = 127;
			}
			cl.Effects[idx].ef.Star.scale = 0.3;
			cl.Effects[idx].ef.Star.scale_dir = 1;
		}
		break;

	case CE_CHUNK:
		cl.Effects[idx].ef.Chunk.origin[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Chunk.origin[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Chunk.origin[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Chunk.type = MSG_ReadByte ();

		cl.Effects[idx].ef.Chunk.srcVel[0] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Chunk.srcVel[1] = MSG_ReadCoord ();
		cl.Effects[idx].ef.Chunk.srcVel[2] = MSG_ReadCoord ();

		cl.Effects[idx].ef.Chunk.numChunks = MSG_ReadByte ();

		cl.Effects[idx].ef.Chunk.time_amount = 12.0;

		cl.Effects[idx].ef.Chunk.aveScale =
					30 + 100 * (cl.Effects[idx].ef.Chunk.numChunks / 40.0);

		if (cl.Effects[idx].ef.Chunk.numChunks > 16)
			cl.Effects[idx].ef.Chunk.numChunks = 16;

		for (i = 0 ; i < cl.Effects[idx].ef.Chunk.numChunks ; i++)
		{
			if ((cl.Effects[idx].ef.Chunk.entity_index[i] = NewEffectEntity()) != -1)
			{
				ent = &EffectEntities[cl.Effects[idx].ef.Chunk.entity_index[i]];
				VectorCopy(cl.Effects[idx].ef.Chunk.origin, ent->origin);

				VectorCopy(cl.Effects[idx].ef.Chunk.srcVel,
							cl.Effects[idx].ef.Chunk.velocity[i]);
				VectorScale(cl.Effects[idx].ef.Chunk.velocity[i],
							.80 + ((rand() % 4) / 10.0),
							cl.Effects[idx].ef.Chunk.velocity[i]);
				/* temp modify them... */
				cl.Effects[idx].ef.Chunk.velocity[i][0] += (rand() % 140) - 70;
				cl.Effects[idx].ef.Chunk.velocity[i][1] += (rand() % 140) - 70;
				cl.Effects[idx].ef.Chunk.velocity[i][2] += (rand() % 140) - 70;

				/* are these in degrees or radians? */
				ent->angles[0] = rand() % 360;
				ent->angles[1] = rand() % 360;
				ent->angles[2] = rand() % 360;

				ent->scale = cl.Effects[idx].ef.Chunk.aveScale + (rand() % 40);

				/* make this overcomplicated */
				final = (rand() % 100) * .01;
				if ((cl.Effects[idx].ef.Chunk.type == THINGTYPE_GLASS) ||
					(cl.Effects[idx].ef.Chunk.type == THINGTYPE_REDGLASS) ||
					(cl.Effects[idx].ef.Chunk.type == THINGTYPE_CLEARGLASS) ||
					(cl.Effects[idx].ef.Chunk.type == THINGTYPE_WEBS))
				{
					if (final < 0.20)
						ent->model = Mod_ForName ("models/shard1.mdl", true);
					else if (final < 0.40)
						ent->model = Mod_ForName ("models/shard2.mdl", true);
					else if (final < 0.60)
						ent->model = Mod_ForName ("models/shard3.mdl", true);
					else if (final < 0.80)
						ent->model = Mod_ForName ("models/shard4.mdl", true);
					else
						ent->model = Mod_ForName ("models/shard5.mdl", true);

					if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_CLEARGLASS)
					{
						ent->skinnum = 1;
						ent->drawflags |= DRF_TRANSLUCENT;
					}
					else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_REDGLASS)
					{
						ent->skinnum = 2;
					}
					else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_WEBS)
					{
						ent->skinnum = 3;
						ent->drawflags |= DRF_TRANSLUCENT;
					}
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_WOOD)
				{
					if (final < 0.25)
						ent->model = Mod_ForName ("models/splnter1.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/splnter2.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/splnter3.mdl", true);
					else
						ent->model = Mod_ForName ("models/splnter4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_METAL)
				{
					if (final < 0.25)
						ent->model = Mod_ForName ("models/metlchk1.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/metlchk2.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/metlchk3.mdl", true);
					else
						ent->model = Mod_ForName ("models/metlchk4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_FLESH)
				{
					if (final < 0.33)
						ent->model = Mod_ForName ("models/flesh1.mdl", true);
					else if (final < 0.66)
						ent->model = Mod_ForName ("models/flesh2.mdl", true);
					else
						ent->model = Mod_ForName ("models/flesh3.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_BROWNSTONE)
				{
					if (final < 0.25)
						ent->model = Mod_ForName ("models/schunk1.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/schunk2.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/schunk3.mdl", true);
					else
						ent->model = Mod_ForName ("models/schunk4.mdl", true);
					ent->skinnum = 1;
				}
				else if ((cl.Effects[idx].ef.Chunk.type == THINGTYPE_CLAY) ||
						(cl.Effects[idx].ef.Chunk.type == THINGTYPE_BONE))
				{
					if (final < 0.25)
						ent->model = Mod_ForName ("models/clshard1.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/clshard2.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/clshard3.mdl", true);
					else
						ent->model = Mod_ForName ("models/clshard4.mdl", true);
					if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_BONE)
					{
						ent->skinnum = 1;	/* bone skin is second */
					}
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_LEAVES)
				{
					if (final < 0.33)
						ent->model = Mod_ForName ("models/leafchk1.mdl", true);
					else if (final < 0.66)
						ent->model = Mod_ForName ("models/leafchk2.mdl", true);
					else
						ent->model = Mod_ForName ("models/leafchk3.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_HAY)
				{
					if (final < 0.33)
						ent->model = Mod_ForName ("models/hay1.mdl", true);
					else if (final < 0.66)
						ent->model = Mod_ForName ("models/hay2.mdl", true);
					else
						ent->model = Mod_ForName ("models/hay3.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_CLOTH)
				{
					if (final < 0.33)
						ent->model = Mod_ForName ("models/clthchk1.mdl", true);
					else if (final < 0.66)
						ent->model = Mod_ForName ("models/clthchk2.mdl", true);
					else
						ent->model = Mod_ForName ("models/clthchk3.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_WOOD_LEAF)
				{
					if (final < 0.14)
						ent->model = Mod_ForName ("models/splnter1.mdl", true);
					else if (final < 0.28)
						ent->model = Mod_ForName ("models/leafchk1.mdl", true);
					else if (final < 0.42)
						ent->model = Mod_ForName ("models/splnter2.mdl", true);
					else if (final < 0.56)
						ent->model = Mod_ForName ("models/leafchk2.mdl", true);
					else if (final < 0.70)
						ent->model = Mod_ForName ("models/splnter3.mdl", true);
					else if (final < 0.84)
						ent->model = Mod_ForName ("models/leafchk3.mdl", true);
					else
						ent->model = Mod_ForName ("models/splnter4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_WOOD_METAL)
				{
					if (final < 0.125)
						ent->model = Mod_ForName ("models/splnter1.mdl", true);
					else if (final < 0.25)
						ent->model = Mod_ForName ("models/metlchk1.mdl", true);
					else if (final < 0.375)
						ent->model = Mod_ForName ("models/splnter2.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/metlchk2.mdl", true);
					else if (final < 0.625)
						ent->model = Mod_ForName ("models/splnter3.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/metlchk3.mdl", true);
					else if (final < 0.875)
						ent->model = Mod_ForName ("models/splnter4.mdl", true);
					else
						ent->model = Mod_ForName ("models/metlchk4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_WOOD_STONE)
				{
					if (final < 0.125)
						ent->model = Mod_ForName ("models/splnter1.mdl", true);
					else if (final < 0.25)
						ent->model = Mod_ForName ("models/schunk1.mdl", true);
					else if (final < 0.375)
						ent->model = Mod_ForName ("models/splnter2.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/schunk2.mdl", true);
					else if (final < 0.625)
						ent->model = Mod_ForName ("models/splnter3.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/schunk3.mdl", true);
					else if (final < 0.875)
						ent->model = Mod_ForName ("models/splnter4.mdl", true);
					else
						ent->model = Mod_ForName ("models/schunk4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_METAL_STONE)
				{
					if (final < 0.125)
						ent->model = Mod_ForName ("models/metlchk1.mdl", true);
					else if (final < 0.25)
						ent->model = Mod_ForName ("models/schunk1.mdl", true);
					else if (final < 0.375)
						ent->model = Mod_ForName ("models/metlchk2.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/schunk2.mdl", true);
					else if (final < 0.625)
						ent->model = Mod_ForName ("models/metlchk3.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/schunk3.mdl", true);
					else if (final < 0.875)
						ent->model = Mod_ForName ("models/metlchk4.mdl", true);
					else
						ent->model = Mod_ForName ("models/schunk4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_METAL_CLOTH)
				{
					if (final < 0.14)
						ent->model = Mod_ForName ("models/metlchk1.mdl", true);
					else if (final < 0.28)
						ent->model = Mod_ForName ("models/clthchk1.mdl", true);
					else if (final < 0.42)
						ent->model = Mod_ForName ("models/metlchk2.mdl", true);
					else if (final < 0.56)
						ent->model = Mod_ForName ("models/clthchk2.mdl", true);
					else if (final < 0.70)
						ent->model = Mod_ForName ("models/metlchk3.mdl", true);
					else if (final < 0.84)
						ent->model = Mod_ForName ("models/clthchk3.mdl", true);
					else
						ent->model = Mod_ForName ("models/metlchk4.mdl", true);
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_ICE)
				{
					ent->model = Mod_ForName("models/shard.mdl", true);
					ent->skinnum = 0;
					ent->frame = rand() % 2;
					ent->drawflags |= DRF_TRANSLUCENT|MLS_ABSLIGHT;
					ent->abslight = 127;
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_METEOR)
				{
					ent->model = Mod_ForName("models/tempmetr.mdl", true);
					ent->skinnum = 0;
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_ACID)
				{	/* no spinning if possible... */
					ent->model = Mod_ForName("models/sucwp2p.mdl", true);
					ent->skinnum = 0;
				}
				else if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_GREENFLESH)
				{	/* spider guts */
					if (final < 0.33)
						ent->model = Mod_ForName ("models/sflesh1.mdl", true);
					else if (final < 0.66)
						ent->model = Mod_ForName ("models/sflesh2.mdl", true);
					else
						ent->model = Mod_ForName ("models/sflesh3.mdl", true);

					ent->skinnum = 0;
				}
				else// if (cl.Effects[idx].ef.Chunk.type == THINGTYPE_GREYSTONE)
				{
					if (final < 0.25)
						ent->model = Mod_ForName ("models/schunk1.mdl", true);
					else if (final < 0.50)
						ent->model = Mod_ForName ("models/schunk2.mdl", true);
					else if (final < 0.75)
						ent->model = Mod_ForName ("models/schunk3.mdl", true);
					else
						ent->model = Mod_ForName ("models/schunk4.mdl", true);
					ent->skinnum = 0;
				}
			}
			else
			{
				ImmediateFree = true;
				break;
			}
		}
		if (ImmediateFree) {
			for (i = 0 ; i < cl.Effects[idx].ef.Chunk.numChunks ; i++) {
				if (cl.Effects[idx].ef.Chunk.entity_index[i] == -1)
					break;
				FreeEffectEntity(cl.Effects[idx].ef.Chunk.entity_index[i]);
			}
			break;
		}

		for (i = 0 ; i < 3 ; i++)
		{
			cl.Effects[idx].ef.Chunk.avel[i][0] = (rand() % 850) - 425;
			cl.Effects[idx].ef.Chunk.avel[i][1] = (rand() % 850) - 425;
			cl.Effects[idx].ef.Chunk.avel[i][2] = (rand() % 850) - 425;
		}

		break;

	default:
		Host_Error ("%s: bad type", __thisfunc__);
	}

	if (ImmediateFree)
		CL_FreeEffect (idx);
}

void CL_EndEffect (void)
{
	int		idx;

	idx = MSG_ReadByte();

	CL_FreeEffect(idx);
}

void CL_EndHWEffect (int idx)
{
	entity_t *ent;

	if (idx < 0 || idx >= MAX_EFFECTS)
		return;
	if (cl.Effects[idx].type == CE_HW_RAVENPOWER ||
			cl.Effects[idx].type == CE_HW_RAVENSTAFF)
	{
		ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
		R_ParticleExplosion (ent->origin);
	}
	CL_FreeEffect (idx);
}

void CL_TurnHWEffect (int idx, const vec3_t origin, const vec3_t velocity)
{
	entity_t *ent;

	if (idx < 0 || idx >= MAX_EFFECTS)
		return;
	switch (cl.Effects[idx].type)
	{
	case CE_BONESHARD:
	case CE_BONESHRAPNEL:
	case CE_HW_BONEBALL:
	case CE_HW_RAVENSTAFF:
	case CE_HW_RAVENPOWER:
		ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
		/* Compatibility: HW historically snaps to the raw server position. */
		VectorCopy (origin, ent->origin);
		VectorCopy (velocity, cl.Effects[idx].ef.Missile.velocity);
		CL_VectorToAngles (velocity, cl.Effects[idx].ef.Missile.angle);
		break;
	case CE_HW_MISSILESTAR:
	case CE_HW_EIDOLONSTAR:
		ent = &EffectEntities[cl.Effects[idx].ef.Star.entity_index];
		VectorCopy (origin, ent->origin);
		VectorCopy (velocity, cl.Effects[idx].ef.Star.velocity);
		break;
	}
}

void CL_UpdateHWEffect (int idx, int type, int command,
		float value, const vec3_t angles, const vec3_t origin, int extra)
{
	entity_t *ent;
	vec3_t forward, right, up;
	float speed;
	int bolt;

	if (idx < 0 || idx >= MAX_EFFECTS || cl.Effects[idx].type != type)
		return;
	if (type == CE_HW_SCARABCHAIN)
	{
		cl.Effects[idx].ef.Chain.material = extra >> 12;
		cl.Effects[idx].ef.Chain.owner = extra & 0xfff;
		cl.Effects[idx].ef.Chain.state = cl.Effects[idx].ef.Chain.owner ? 1 : 2;
		return;
	}
	if (type == CE_HW_DRILLA)
	{
		if (!command)
		{
			R_RunParticleEffect4 (origin, 24, 256 + 3 * 16 + 4,
					pt_fastgrav, 20);
			return;
		}
		ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
		VectorCopy (angles, ent->angles);
		VectorCopy (origin, cl.Effects[idx].ef.Missile.origin);
		AngleVectors (angles, forward, right, up);
		speed = VectorLengthFast (cl.Effects[idx].ef.Missile.velocity);
		VectorScale (forward, speed, cl.Effects[idx].ef.Missile.velocity);
		VectorCopy (origin, ent->origin);
		return;
	}

	bolt = (command >> 4) & 7;
	if (bolt >= 5 || bolt >= cl.Effects[idx].ef.Xbow.bolts)
		return;
	ent = &EffectEntities[cl.Effects[idx].ef.Xbow.ent[bolt]];
	if (command & 1)
	{
		cl.Effects[idx].ef.Xbow.activebolts &= ~(1 << bolt);
		if (cl.Effects[idx].ef.Xbow.bolts == 5)
		{
			if (command & 128)
				cl.Effects[idx].ef.Xbow.gonetime[bolt] = cl.time;
			else
				cl.Effects[idx].ef.Xbow.gonetime[bolt] += cl.time;
		}
		VectorCopy (cl.Effects[idx].ef.Xbow.vel[bolt], forward);
		VectorNormalizeFast (forward);
		VectorMA (cl.Effects[idx].ef.Xbow.origin[bolt], value, forward,
				ent->origin);
		R_RunParticleEffect4 (ent->origin, 20,
				type == CE_HW_SHEEPINATOR ? 144 + rand() % 16 :
				256 + 3 * 16 + 4, pt_fastgrav, 20);
		return;
	}
	VectorCopy (angles, ent->angles);
	if (command & 128)
		VectorCopy (origin, cl.Effects[idx].ef.Xbow.origin[bolt]);
	AngleVectors (angles, forward, right, up);
	speed = VectorLengthFast (cl.Effects[idx].ef.Xbow.vel[bolt]);
	VectorScale (forward, speed, cl.Effects[idx].ef.Xbow.vel[bolt]);
	VectorCopy (cl.Effects[idx].ef.Xbow.origin[bolt], ent->origin);
}

void CL_MultiHWEffect (const vec3_t origin, const vec3_t velocity,
		const int slots[3])
{
	entity_t *ent;
	int i, idx;

	for (i = 0; i < 3; ++i)
	{
		idx = slots[i];
		if (cl.Effects[idx].type)
			CL_FreeEffect (idx);
		memset (&cl.Effects[idx], 0, sizeof(cl.Effects[idx]));
		cl.Effects[idx].type = CE_HW_RAVENPOWER;
		VectorCopy (origin, cl.Effects[idx].ef.Missile.origin);
		VectorCopy (velocity, cl.Effects[idx].ef.Missile.velocity);
		CL_VectorToAngles (velocity, cl.Effects[idx].ef.Missile.angle);
		cl.Effects[idx].ef.Missile.entity_index = NewEffectEntity ();
		if (cl.Effects[idx].ef.Missile.entity_index == -1)
		{
			memset (&cl.Effects[idx], 0, sizeof(cl.Effects[idx]));
			continue;
		}
		ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
		VectorCopy (origin, ent->origin);
		VectorCopy (cl.Effects[idx].ef.Missile.angle, ent->angles);
		ent->model = Mod_ForName ("models/ravproj.mdl", true);
	}
	R_ParticleExplosion ((vec3_t) {origin[0], origin[1], origin[2]});
}

static void CL_LinkEntity (entity_t *ent)
{
	if (cl_numvisedicts < MAX_VISEDICTS)
	{
		cl_visedicts[cl_numvisedicts++] = ent;
	}
}

void CL_UpdateEffects (void)
{
	int		i;
	int		idx, cur_frame;
	vec3_t		mymin, mymax;
	float		frametime;
//	edict_t		test;
//	trace_t		trace;
	vec3_t		org, org2, alldir;
	int		x_dir, y_dir, z_dir;
	entity_t	*ent;
	mleaf_t		*leaf;
	float		snow_dx, snow_dy, snow_distsq, smoketime;

	if (cls.state == ca_disconnected)
		return;

	frametime = host_frametime;
	if (frametime <= 0)
		return;
//	Con_Printf("Here at %f\n",cl.time);

	for (idx = 0 ; idx < MAX_EFFECTS ; idx++)
	{
		if (!cl.Effects[idx].type)
			continue;

		switch (cl.Effects[idx].type)
		{
		case CE_RAIN:
			org[0] = cl.Effects[idx].ef.Rain.min_org[0];
			org[1] = cl.Effects[idx].ef.Rain.min_org[1];
			org[2] = cl.Effects[idx].ef.Rain.max_org[2];

			org2[0] = cl.Effects[idx].ef.Rain.e_size[0];
			org2[1] = cl.Effects[idx].ef.Rain.e_size[1];
			org2[2] = cl.Effects[idx].ef.Rain.e_size[2];

			x_dir = cl.Effects[idx].ef.Rain.dir[0];
			y_dir = cl.Effects[idx].ef.Rain.dir[1];
			/* Z of the direction vector has always been sent and parsed
			 * and then dropped on the floor for rain.  Spend it on the
			 * fall speed, so a mapper can pick drizzle or downpour with
			 * no protocol change.  Zero keeps the stock random spread. */
			z_dir = cl.Effects[idx].ef.Rain.dir[2];

			cl.Effects[idx].ef.Rain.next_time += frametime;
			if (cl.Effects[idx].ef.Rain.next_time >= cl.Effects[idx].ef.Rain.wait)
			{
				R_RainEffect(org, org2, x_dir, y_dir, z_dir, cl.Effects[idx].ef.Rain.color,
								cl.Effects[idx].ef.Rain.count);
				cl.Effects[idx].ef.Rain.next_time = 0;
			}
			break;

		case CE_SNOW:
			VectorCopy(cl.Effects[idx].ef.Rain.min_org, org);
			VectorCopy(cl.Effects[idx].ef.Rain.max_org, org2);
			VectorCopy(cl.Effects[idx].ef.Rain.dir, alldir);

			/* Horizontal distance from the camera to the nearest point of
			 * the volume's footprint, which is zero while you stand inside
			 * it.  Raven measured to the volume's CENTRE instead: fine for
			 * the small brushes the original maps use, but it defeats a
			 * map-scale one, because most of such a brush's own footprint
			 * then lies outside its own cutoff.  On SoT's winter not one of
			 * its fifteen volumes passed the old test from any player
			 * start -- the correct stock behaviour there was no snow at
			 * all.
			 *
			 * The 1024 budget stays, and still does its job: on that map
			 * this leaves 2-5 volumes live depending on where you stand,
			 * ~17.5k of the 32k particle pool at worst, where spawning
			 * every volume unconditionally would want ~41.7k and starve
			 * every other particle effect in the game.  Height is still
			 * ignored, as before -- snow falls, so a volume overhead should
			 * reach you.  uhexen2-ykqw. */
			snow_dx = (r_origin[0] < org[0])  ? org[0] - r_origin[0] :
				  (r_origin[0] > org2[0]) ? r_origin[0] - org2[0] : 0;
			snow_dy = (r_origin[1] < org[1])  ? org[1] - r_origin[1] :
				  (r_origin[1] > org2[1]) ? r_origin[1] - org2[1] : 0;
			snow_distsq = snow_dx * snow_dx + snow_dy * snow_dy;

			cl.Effects[idx].ef.Rain.next_time += frametime;
			if (cl.Effects[idx].ef.Rain.next_time >= 0.10 && snow_distsq < 1024 * 1024)
			{
				R_SnowEffect(org, org2, cl.Effects[idx].ef.Rain.flags, alldir,
								cl.Effects[idx].ef.Rain.count);

				cl.Effects[idx].ef.Rain.next_time = 0;
			}
			break;

		case CE_FOUNTAIN:
			mymin[0] = (-3 * cl.Effects[idx].ef.Fountain.vright[0] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (-3 * cl.Effects[idx].ef.Fountain.vforward[0] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (2 * cl.Effects[idx].ef.Fountain.vup[0] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymin[1] = (-3 * cl.Effects[idx].ef.Fountain.vright[1] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (-3 * cl.Effects[idx].ef.Fountain.vforward[1] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (2 * cl.Effects[idx].ef.Fountain.vup[1] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymin[2] = (-3 * cl.Effects[idx].ef.Fountain.vright[2] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (-3 * cl.Effects[idx].ef.Fountain.vforward[2] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (2 * cl.Effects[idx].ef.Fountain.vup[2] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymin[0] *= 15;
			mymin[1] *= 15;
			mymin[2] *= 15;

			mymax[0] = (3 * cl.Effects[idx].ef.Fountain.vright[0] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (3 * cl.Effects[idx].ef.Fountain.vforward[0] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (10 * cl.Effects[idx].ef.Fountain.vup[0] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymax[1] = (3 * cl.Effects[idx].ef.Fountain.vright[1] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (3 * cl.Effects[idx].ef.Fountain.vforward[1] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (10 * cl.Effects[idx].ef.Fountain.vup[1] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymax[2] = (3 * cl.Effects[idx].ef.Fountain.vright[2] * cl.Effects[idx].ef.Fountain.movedir[0]) +
				   (3 * cl.Effects[idx].ef.Fountain.vforward[2] * cl.Effects[idx].ef.Fountain.movedir[1]) +
				   (10 * cl.Effects[idx].ef.Fountain.vup[2] * cl.Effects[idx].ef.Fountain.movedir[2]);
			mymax[0] *= 15;
			mymax[1] *= 15;
			mymax[2] *= 15;

			R_RunParticleEffect2 (cl.Effects[idx].ef.Fountain.pos, mymin, mymax,
						cl.Effects[idx].ef.Fountain.color, pt_fastgrav,
						cl.Effects[idx].ef.Fountain.cnt);

			/*
			memset(&test, 0, sizeof(test));
			trace = SV_Move (cl.Effects[idx].ef.Fountain.pos, mymin, mymax, mymin, false, &test);
			Con_Printf("Fraction is %f\n", trace.fraction);
			*/
			break;

		case CE_QUAKE:
			R_RunQuakeEffect (cl.Effects[idx].ef.Quake.origin,
						cl.Effects[idx].ef.Quake.radius);
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
			cl.Effects[idx].ef.Smoke.time_amount += frametime;
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];

			smoketime = cl.Effects[idx].ef.Smoke.framelength;
			if (!smoketime)
				smoketime = HX_FRAME_TIME;

			ent->origin[0] += (frametime/smoketime) * cl.Effects[idx].ef.Smoke.velocity[0];
			ent->origin[1] += (frametime/smoketime) * cl.Effects[idx].ef.Smoke.velocity[1];
			ent->origin[2] += (frametime/smoketime) * cl.Effects[idx].ef.Smoke.velocity[2];

			while (cl.Effects[idx].ef.Smoke.time_amount >= smoketime)
			{
				ent->frame++;
				cl.Effects[idx].ef.Smoke.time_amount -= smoketime;
			}

			if (!ent->model || ent->frame >= ent->model->numframes)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				/* suppress muzzle flash sprite when glows are off */
				if (cl.Effects[idx].type == CE_ACID_MUZZFL && !gl_missile_glows.integer)
					break;
				CL_LinkEntity(ent);
			}

			break;

		/* Just go through animation and then remove */
		case CE_SM_WHITE_FLASH:
		case CE_YELLOWRED_FLASH:
		case CE_BLUESPARK:
		case CE_YELLOWSPARK:
		case CE_SM_CIRCLE_EXP:
		case CE_BG_CIRCLE_EXP:
		case CE_SM_EXPLOSION:
		case CE_LG_EXPLOSION:
		case CE_FLOOR_EXPLOSION:
		case CE_FLOOR_EXPLOSION3:
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
		case CE_BRN_BOUNCE:
		case CE_ACID_HIT:
		case CE_ACID_SPLAT:
		case CE_ACID_EXPL:
		case CE_LBALL_EXPL:
		case CE_FBOOM:
		case CE_BOMB:
		case CE_FIREWALL_SMALL:
		case CE_FIREWALL_MEDIUM:
		case CE_FIREWALL_LARGE:
			cl.Effects[idx].ef.Smoke.time_amount += frametime;
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];

			if (cl.Effects[idx].type != CE_BG_CIRCLE_EXP)
			{
				while (cl.Effects[idx].ef.Smoke.time_amount >= HX_FRAME_TIME)
				{
					ent->frame++;
					cl.Effects[idx].ef.Smoke.time_amount -= HX_FRAME_TIME;
				}
			}
			else
			{
				while (cl.Effects[idx].ef.Smoke.time_amount >= HX_FRAME_TIME * 2)
				{
					ent->frame++;
					cl.Effects[idx].ef.Smoke.time_amount -= HX_FRAME_TIME * 2;
				}
			}
			if (!ent->model || ent->frame >= ent->model->numframes)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				CL_LinkEntity(ent);
			}
			break;

		case CE_LSHOCK:
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];
			if (ent->skinnum == 0)
				ent->skinnum = 1;
			else if (ent->skinnum == 1)
				ent->skinnum = 0;
			ent->scale -= 10;
			if (ent->scale <= 10)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				CL_LinkEntity(ent);
			}
			break;

		/* Go forward then backward through animation then remove */
		case CE_WHITE_FLASH:
		case CE_BLUE_FLASH:
		case CE_SM_BLUE_FLASH:
		case CE_RED_FLASH:
			cl.Effects[idx].ef.Flash.time_amount += frametime;
			ent = &EffectEntities[cl.Effects[idx].ef.Flash.entity_index];

			while (ent->model && cl.Effects[idx].ef.Flash.time_amount >= HX_FRAME_TIME)
			{
				if (!cl.Effects[idx].ef.Flash.reverse)
				{
					if (ent->frame >= ent->model->numframes-1)
					{
					/* Ran through forward animation */
						cl.Effects[idx].ef.Flash.reverse = 1;
						ent->frame--;
					}
					else
						ent->frame++;
				}
				else
				{
					ent->frame--;
				}

				cl.Effects[idx].ef.Flash.time_amount -= HX_FRAME_TIME;
			}

			if ((ent->frame <= 0) && (cl.Effects[idx].ef.Flash.reverse))
			{
				CL_FreeEffect(idx);
			}
			else
			{
				CL_LinkEntity(ent);
			}
			break;

		case CE_RIDER_DEATH: {
			float sinval, cosval;
			cl.Effects[idx].ef.RD.time_amount += frametime;
			if (cl.Effects[idx].ef.RD.time_amount >= 1)
			{
				cl.Effects[idx].ef.RD.stage++;
				cl.Effects[idx].ef.RD.time_amount -= 1;
			}

			VectorCopy(cl.Effects[idx].ef.RD.origin, org);
			q_sincosrad(cl.Effects[idx].ef.RD.time_amount * 2 * M_PI, &sinval, &cosval);
			org[0] += sinval * 30;
			org[1] += cosval * 30;

			if (cl.Effects[idx].ef.RD.stage <= 6)
			{
			//	RiderParticle(cl.Effects[idx].ef.RD.stage + 1, cl.Effects[idx].ef.RD.origin);
				RiderParticle(cl.Effects[idx].ef.RD.stage + 1, org);
			}
			else
			{
				/* To set the rider's origin point for the particles */
				RiderParticle(0, org);
				if (cl.Effects[idx].ef.RD.stage == 7)
				{
					cl.cshifts[CSHIFT_BONUS].destcolor[0] = 255;
					cl.cshifts[CSHIFT_BONUS].destcolor[1] = 255;
					cl.cshifts[CSHIFT_BONUS].destcolor[2] = 255;
					cl.cshifts[CSHIFT_BONUS].percent = 256;
				}
				else if (cl.Effects[idx].ef.RD.stage > 13)
				{
				//	cl.Effects[idx].ef.RD.stage = 0;
					CL_FreeEffect(idx);
				}
			} }
			break;

		case CE_GRAVITYWELL: {
			float sinval, cosval;
			cl.Effects[idx].ef.RD.time_amount += frametime*2;
			if (cl.Effects[idx].ef.RD.time_amount >= 1)
				cl.Effects[idx].ef.RD.time_amount -= 1;

			VectorCopy(cl.Effects[idx].ef.RD.origin, org);
			q_sincosrad(cl.Effects[idx].ef.RD.time_amount * 2 * M_PI, &sinval, &cosval);
			org[0] += sinval * 30;
			org[1] += cosval * 30;

			if (cl.Effects[idx].ef.RD.lifetime < cl.time)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				GravityWellParticle(rand() % 8, org, cl.Effects[idx].ef.RD.color);
			} }
			break;

		case CE_TELEPORTERPUFFS:
			cl.Effects[idx].ef.Teleporter.time_amount += frametime;
			smoketime = cl.Effects[idx].ef.Teleporter.framelength;

			ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[0]];
			while (cl.Effects[idx].ef.Teleporter.time_amount >= HX_FRAME_TIME)
			{
				ent->frame++;
				cl.Effects[idx].ef.Teleporter.time_amount -= HX_FRAME_TIME;
			}
			cur_frame = ent->frame;

			if (!ent->model || cur_frame >= ent->model->numframes)
			{
				CL_FreeEffect(idx);
				break;
			}

			for (i = 0 ; i < 8 ; ++i)
			{
				ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[i]];

				ent->origin[0] += (frametime/smoketime) * cl.Effects[idx].ef.Teleporter.velocity[i][0];
				ent->origin[1] += (frametime/smoketime) * cl.Effects[idx].ef.Teleporter.velocity[i][1];
				ent->origin[2] += (frametime/smoketime) * cl.Effects[idx].ef.Teleporter.velocity[i][2];
				ent->frame = cur_frame;

				CL_LinkEntity(ent);
			}
			break;

		case CE_TELEPORTERBODY:
			cl.Effects[idx].ef.Teleporter.time_amount += frametime;
			smoketime = cl.Effects[idx].ef.Teleporter.framelength;

			ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[0]];
			while (cl.Effects[idx].ef.Teleporter.time_amount >= HX_FRAME_TIME)
			{
				ent->scale -= 15;
				cl.Effects[idx].ef.Teleporter.time_amount -= HX_FRAME_TIME;
			}

			ent = &EffectEntities[cl.Effects[idx].ef.Teleporter.entity_index[0]];
			ent->angles[1] += 45;

			if (ent->scale <= 10)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				CL_LinkEntity(ent);
			}
			break;

		case CE_BONESHARD:
		case CE_BONESHRAPNEL:
			cl.Effects[idx].ef.Missile.time_amount += frametime;
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];

		//	ent->angles[0] = cl.Effects[idx].ef.Missile.angle[0];
		//	ent->angles[1] = cl.Effects[idx].ef.Missile.angle[1];
		//	ent->angles[2] = cl.Effects[idx].ef.Missile.angle[2];

			ent->angles[0] += frametime * cl.Effects[idx].ef.Missile.avelocity[0];
			ent->angles[1] += frametime * cl.Effects[idx].ef.Missile.avelocity[1];
			ent->angles[2] += frametime * cl.Effects[idx].ef.Missile.avelocity[2];

			ent->origin[0] += frametime * cl.Effects[idx].ef.Missile.velocity[0];
			ent->origin[1] += frametime * cl.Effects[idx].ef.Missile.velocity[1];
			ent->origin[2] += frametime * cl.Effects[idx].ef.Missile.velocity[2];

			CL_LinkEntity(ent);
			break;

		case CE_HW_RIPPLE:
			cl.Effects[idx].ef.Smoke.time_amount += frametime;
			ent = &EffectEntities[cl.Effects[idx].ef.Smoke.entity_index];
			while (cl.Effects[idx].ef.Smoke.time_amount >=
					cl.Effects[idx].ef.Smoke.framelength)
			{
				ent->frame++;
				ent->angles[1] += 1;
				cl.Effects[idx].ef.Smoke.time_amount -=
					cl.Effects[idx].ef.Smoke.framelength;
			}
			if (ent->frame >= 10)
				CL_FreeEffect (idx);
			else
				CL_LinkEntity (ent);
			break;

		case CE_HW_DRILLA:
		case CE_HW_BONEBALL:
		case CE_HW_RAVENSTAFF:
		case CE_HW_RAVENPOWER:
			ent = &EffectEntities[cl.Effects[idx].ef.Missile.entity_index];
			VectorCopy (ent->origin, org);
			VectorMA (ent->origin, frametime,
					cl.Effects[idx].ef.Missile.velocity, ent->origin);
			VectorMA (ent->angles, frametime,
					cl.Effects[idx].ef.Missile.avelocity, ent->angles);
			if (cl.Effects[idx].type == CE_HW_DRILLA)
				R_RocketTrail (org, ent->origin, rt_setstaff);
			else if (cl.Effects[idx].type == CE_HW_BONEBALL)
				R_RunParticleEffect4 (ent->origin, 10, 368 + rand() % 16,
						pt_slowgrav, 3);
			else if (cl.Effects[idx].type == CE_HW_RAVENPOWER)
			{
				cl.Effects[idx].ef.Missile.time_amount += frametime;
				while (cl.Effects[idx].ef.Missile.time_amount >= HX_FRAME_TIME)
				{
					ent->frame = (ent->frame + 1) & 7;
					cl.Effects[idx].ef.Missile.time_amount -= HX_FRAME_TIME;
				}
			}
			CL_LinkEntity (ent);
			break;

		case CE_HW_XBOWSHOOT:
		case CE_HW_SHEEPINATOR:
			for (i = 0; i < cl.Effects[idx].ef.Xbow.bolts; ++i)
			{
				if (cl.Effects[idx].ef.Xbow.ent[i] == -1)
					continue;
				ent = &EffectEntities[cl.Effects[idx].ef.Xbow.ent[i]];
				if (cl.Effects[idx].ef.Xbow.activebolts & (1 << i))
				{
					VectorMA (ent->origin, frametime,
							cl.Effects[idx].ef.Xbow.vel[i], ent->origin);
					if (cl.Effects[idx].type == CE_HW_SHEEPINATOR)
						R_RunParticleEffect4 (ent->origin, 7, 144 + rand() % 15,
								pt_explode2, 1 + rand() % 5);
					CL_LinkEntity (ent);
				}
				else if (cl.Effects[idx].type == CE_HW_XBOWSHOOT &&
						cl.Effects[idx].ef.Xbow.bolts == 5 &&
						cl.Effects[idx].ef.Xbow.state[i] < 2)
				{
					if (cl.Effects[idx].ef.Xbow.state[i] == 0 &&
							cl.Effects[idx].ef.Xbow.gonetime[i] <= cl.time)
					{
						cl.Effects[idx].ef.Xbow.state[i] = 1;
						ent->model = Mod_ForName ("models/xbowexpl.spr", true);
						ent->frame = 0;
						cl.Effects[idx].ef.Xbow.gonetime[i] =
							cl.time + HX_FRAME_TIME * 2;
					}
					if (cl.Effects[idx].ef.Xbow.state[i] == 1)
					{
						while (cl.Effects[idx].ef.Xbow.gonetime[i] <= cl.time)
						{
							ent->frame++;
							cl.Effects[idx].ef.Xbow.gonetime[i] +=
								HX_FRAME_TIME * 0.75;
						}
						if (!ent->model || ent->frame >= ent->model->numframes)
							cl.Effects[idx].ef.Xbow.state[i] = 2;
					}
					if (cl.Effects[idx].ef.Xbow.state[i] < 2)
						CL_LinkEntity (ent);
				}
			}
			break;

		case CE_HW_DEATHBUBBLES:
			cl.Effects[idx].ef.Bubble.time_amount += frametime;
			if (cl.Effects[idx].ef.Bubble.time_amount > 0.1)
			{
				cl.Effects[idx].ef.Bubble.time_amount = 0;
				cl.Effects[idx].ef.Bubble.count--;
				i = cl.Effects[idx].ef.Bubble.owner;
				if (i > 0 && i < cl.num_entities &&
						(cl_entities[i].baseline.flags & BE_ON))
				{
					VectorAdd (cl_entities[i].origin,
							cl.Effects[idx].ef.Bubble.offset, org);
					leaf = Mod_PointInLeaf (org, cl.worldmodel);
					if (leaf->contents != CONTENTS_WATER)
					{
						CL_FreeEffect (idx);
						break;
					}
					R_RunParticleEffect4 (org, 2, 406 + rand() % 8,
							pt_slowgrav, 1);
				}
			}
			if (cl.Effects[idx].ef.Bubble.count <= 0)
				CL_FreeEffect (idx);
			break;

		case CE_HW_TRIPMINE:
		case CE_HW_TRIPMINESTILL:
			ent = &EffectEntities[cl.Effects[idx].ef.Chain.ent1];
			if (cl.Effects[idx].type == CE_HW_TRIPMINE)
				VectorMA (ent->origin, frametime,
						cl.Effects[idx].ef.Chain.velocity, ent->origin);
			CL_LinkEntity (ent);
			CL_CreateEffectStream (idx, 1, cl.Effects[idx].ef.Chain.origin,
					ent->origin);
			break;

		case CE_HW_SCARABCHAIN:
			ent = &EffectEntities[cl.Effects[idx].ef.Chain.ent1];
			i = cl.Effects[idx].ef.Chain.owner;
			if (cl.Effects[idx].ef.Chain.state != 2 && i > 0 &&
					i < cl.num_entities &&
					(cl_entities[i].baseline.flags & BE_ON))
			{
				VectorCopy (cl_entities[i].origin, org);
				org[2] += cl.Effects[idx].ef.Chain.height;
				if (cl.Effects[idx].ef.Chain.state == 0)
				{
					VectorSubtract (org, ent->origin, org2);
					if (VectorNormalizeFast (org2) <= 500 * frametime)
						cl.Effects[idx].ef.Chain.state = 1;
					else
						VectorMA (ent->origin, 500 * frametime, org2,
								ent->origin);
				}
				if (cl.Effects[idx].ef.Chain.state == 1)
					VectorCopy (org, ent->origin);
			}
			else if (cl.Effects[idx].ef.Chain.state == 2)
			{
				VectorSubtract (cl.Effects[idx].ef.Chain.origin,
						ent->origin, org2);
				if (VectorNormalizeFast (org2) <= 350 * frametime)
				{
					VectorCopy (ent->origin, org);
					cl.Effects[idx].type = CE_RED_FLASH;
					cl.Effects[idx].ef.Flash.entity_index =
						cl.Effects[idx].ef.Chain.ent1;
					VectorCopy (org, cl.Effects[idx].ef.Flash.origin);
					cl.Effects[idx].ef.Flash.reverse = 0;
					cl.Effects[idx].ef.Flash.time_amount = 0;
					ent->model = Mod_ForName ("models/redspt.spr", true);
					ent->frame = 0;
					ent->drawflags = DRF_TRANSLUCENT;
					break;
				}
				VectorMA (ent->origin, 350 * frametime, org2, ent->origin);
			}
			CL_LinkEntity (ent);
			CL_CreateEffectStream (idx, cl.Effects[idx].ef.Chain.tag,
					cl.Effects[idx].ef.Chain.origin, ent->origin);
			break;

		case CE_HW_MISSILESTAR:
		case CE_HW_EIDOLONSTAR:
			if (cl.Effects[idx].ef.Star.scale_dir)
			{
				cl.Effects[idx].ef.Star.scale += 0.05;
				if (cl.Effects[idx].ef.Star.scale >= 1)
					cl.Effects[idx].ef.Star.scale_dir = 0;
			}
			else
			{
				cl.Effects[idx].ef.Star.scale -= 0.05;
				if (cl.Effects[idx].ef.Star.scale <= 0.01)
					cl.Effects[idx].ef.Star.scale_dir = 1;
			}
			ent = &EffectEntities[cl.Effects[idx].ef.Star.entity_index];
			VectorMA (ent->origin, frametime, cl.Effects[idx].ef.Star.velocity,
					ent->origin);
			VectorMA (ent->angles, frametime, cl.Effects[idx].ef.Star.avelocity,
					ent->angles);
			CL_LinkEntity (ent);
			ent = &EffectEntities[cl.Effects[idx].ef.Star.ent1];
			VectorCopy (EffectEntities[cl.Effects[idx].ef.Star.entity_index].origin,
					ent->origin);
			ent->scale = cl.Effects[idx].ef.Star.scale * 100;
			CL_LinkEntity (ent);
			if (cl.Effects[idx].ef.Star.ent2 != -1)
			{
				ent = &EffectEntities[cl.Effects[idx].ef.Star.ent2];
				VectorCopy (EffectEntities[cl.Effects[idx].ef.Star.entity_index].origin,
						ent->origin);
				ent->scale = cl.Effects[idx].ef.Star.scale * 100;
				CL_LinkEntity (ent);
			}
			break;

		case CE_CHUNK:
			cl.Effects[idx].ef.Chunk.time_amount -= frametime;
			if (cl.Effects[idx].ef.Chunk.time_amount < 0)
			{
				CL_FreeEffect(idx);
			}
			else
			{
				for (i = 0 ; i < cl.Effects[idx].ef.Chunk.numChunks ; i++)
				{
					vec3_t		oldorg;
					mleaf_t		*l;
					int		moving = 1;

					ent = &EffectEntities[cl.Effects[idx].ef.Chunk.entity_index[i]];

					VectorCopy(ent->origin, oldorg);

					ent->origin[0] += frametime * cl.Effects[idx].ef.Chunk.velocity[i][0];
					ent->origin[1] += frametime * cl.Effects[idx].ef.Chunk.velocity[i][1];
					ent->origin[2] += frametime * cl.Effects[idx].ef.Chunk.velocity[i][2];

					l = Mod_PointInLeaf (ent->origin, cl.worldmodel);
					if (l->contents != CONTENTS_EMPTY) //|| in_solid == true
					{	/* bouncing prolly won't work... */
						VectorCopy(oldorg, ent->origin);

						cl.Effects[idx].ef.Chunk.velocity[i][0] = 0;
						cl.Effects[idx].ef.Chunk.velocity[i][1] = 0;
						cl.Effects[idx].ef.Chunk.velocity[i][2] = 0;

						moving = 0;
					}
					else
					{
						ent->angles[0] += frametime * cl.Effects[idx].ef.Chunk.avel[i%3][0];
						ent->angles[1] += frametime * cl.Effects[idx].ef.Chunk.avel[i%3][1];
						ent->angles[2] += frametime * cl.Effects[idx].ef.Chunk.avel[i%3][2];
					}

					if (cl.Effects[idx].ef.Chunk.time_amount < frametime * 3)
					{	/* chunk leaves in 3 frames */
						ent->scale *= .7;
					}

					CL_LinkEntity(ent);

					cl.Effects[idx].ef.Chunk.velocity[i][2] -= frametime * 500; /* apply gravity */

					switch (cl.Effects[idx].ef.Chunk.type)
					{
					case THINGTYPE_GREYSTONE:
						break;
					case THINGTYPE_WOOD:
						break;
					case THINGTYPE_METAL:
						break;
					case THINGTYPE_FLESH:
						if (moving)
							R_RocketTrail (oldorg, ent->origin, 17);
						break;
					case THINGTYPE_FIRE:
						break;
					case THINGTYPE_CLAY:
					case THINGTYPE_BONE:
						break;
					case THINGTYPE_LEAVES:
						break;
					case THINGTYPE_HAY:
						break;
					case THINGTYPE_BROWNSTONE:
						break;
					case THINGTYPE_CLOTH:
						break;
					case THINGTYPE_WOOD_LEAF:
						break;
					case THINGTYPE_WOOD_METAL:
						break;
					case THINGTYPE_WOOD_STONE:
						break;
					case THINGTYPE_METAL_STONE:
						break;
					case THINGTYPE_METAL_CLOTH:
						break;
					case THINGTYPE_WEBS:
						break;
					case THINGTYPE_GLASS:
						break;
					case THINGTYPE_ICE:
						if (moving)
							R_RocketTrail (oldorg, ent->origin, rt_ice);
						break;
					case THINGTYPE_CLEARGLASS:
						break;
					case THINGTYPE_REDGLASS:
						break;
					case THINGTYPE_ACID:
						if (moving)
							R_RocketTrail (oldorg, ent->origin, rt_acidball);
						break;
					case THINGTYPE_METEOR:
						R_RocketTrail (oldorg, ent->origin, 1);
						break;
					case THINGTYPE_GREENFLESH:
						if (moving)
							R_RocketTrail (oldorg, ent->origin, rt_acidball);
						break;
					}
				}
			}
			break;
		}
	}
}

//==========================================================================
//
// NewEffectEntity
//
//==========================================================================

static int NewEffectEntity (void)
{
	entity_t	*ent;
	int		counter;

	if (cl_numvisedicts == MAX_VISEDICTS)
		return -1;

	if (EffectEntityCount == MAX_EFFECT_ENTITIES)
		return -1;

	for (counter = 0 ; counter < MAX_EFFECT_ENTITIES ; counter++)
	{
		if (!EntityUsed[counter])
			break;
	}

	EntityUsed[counter] = true;
	EffectEntityCount++;
	ent = &EffectEntities[counter];
	memset(ent, 0, sizeof(*ent));
	ent->colormap = vid.colormap;

	return counter;
}

static void FreeEffectEntity (int idx)
{
	if (idx < 0 || idx >= MAX_EFFECT_ENTITIES || !EntityUsed[idx])
		return;
	EntityUsed[idx] = false;
	EffectEntityCount--;
}

