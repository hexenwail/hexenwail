/*
 * hw_particle_mapping.h -- mapping from HexenWorld wire particle types
 * to Hexen II ptype_t for svc_particle2/3/4
 *
 * Copyright (C) 2026 Hexenwail contributors
 */

#ifndef __HW_PARTICLE_MAPPING_H
#define __HW_PARTICLE_MAPPING_H

#include "particle.h"

/*
 * HexenWorld wire particle types (0-28) to Hexen II ptype_t mapping.
 * 
 * HW pt_darken (24)   → H2 pt_darken (25)
 * HW pt_grensmoke (25) → H2 pt_grensmoke (29) 
 * HW pt_redfire (26)   → H2 pt_redfire (28)
 * HW pt_acidball (27)  → H2 pt_acidball (24)
 * HW pt_bluestep (28)  → H2 pt_grensmoke (29) [nearest equivalent]
 */
static const ptype_t hw_to_h2_particle_map[29] = {
	/* 0-23: direct mapping (same enums) */
	[0]  = pt_static,
	[1]  = pt_grav,
	[2]  = pt_fastgrav,
	[3]  = pt_slowgrav,
	[4]  = pt_fire,
	[5]  = pt_explode,
	[6]  = pt_explode2,
	[7]  = pt_blob,
	[8]  = pt_blob2,
	[9]  = pt_rain,
	[10] = pt_c_explode,
	[11] = pt_c_explode2,
	[12] = pt_spit,
	[13] = pt_fireball,
	[14] = pt_ice,
	[15] = pt_spell,
	[16] = pt_test,
	[17] = pt_quake,
	[18] = pt_rd,
	[19] = pt_vorpal,
	[20] = pt_setstaff,
	[21] = pt_magicmissile,
	[22] = pt_boneshard,
	[23] = pt_scarab,
	
	/* 24-28: need mapping or special handling */
	[24] = pt_darken,     /* HW pt_darken */
	[25] = pt_grensmoke,  /* HW pt_grensmoke */
	[26] = pt_redfire,    /* HW pt_redfire */
	[27] = pt_acidball,   /* HW pt_acidball */
	[28] = pt_grensmoke   /* HW pt_bluestep → nearest equivalent */
};

/*
 * Map a HexenWorld wire particle type (0-28) to Hexen II ptype_t.
 * Returns pt_static for out-of-range values.
 */
static inline ptype_t HW_H2ParticleMap(int hw_type)
{
	if (hw_type < 0 || hw_type >= 29)
		return pt_static;
	return hw_to_h2_particle_map[hw_type];
}

#endif	/* __HW_PARTICLE_MAPPING_H */