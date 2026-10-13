/*
 * hw_particle_mapping.h -- HexenWorld wire particle types to Hexen II
 * ptype_t, for svc_particle2/3/4 (issue #309)
 *
 * Copyright (C) 2026 Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __HW_PARTICLE_MAPPING_H
#define __HW_PARTICLE_MAPPING_H

#include "particle.h"

/*
 * A HexenWorld server sends the particle type as its own client numbered it.
 * The two enums agree up to pt_scarab (23) and differ after it:
 *
 *	wire	HexenWorld	Hexen II has there
 *	24	pt_darken	pt_acidball
 *	25	pt_grensmoke	pt_darken
 *	26	pt_redfire	pt_snow
 *	27	pt_acidball	pt_gravwell
 *	28	pt_bluestep	pt_redfire
 *
 * Indexed by wire value, one entry per HexenWorld type.
 */
#define	HW_PARTICLE_TYPES	29

static const ptype_t hw_to_h2_particle_map[HW_PARTICLE_TYPES] = {
	pt_static,
	pt_grav,
	pt_fastgrav,
	pt_slowgrav,
	pt_fire,
	pt_explode,
	pt_explode2,
	pt_blob,
	pt_blob2,
	pt_rain,
	pt_c_explode,
	pt_c_explode2,
	pt_spit,
	pt_fireball,
	pt_ice,
	pt_spell,
	pt_test,
	pt_quake,
	pt_rd,
	pt_vorpal,
	pt_setstaff,
	pt_magicmissile,
	pt_boneshard,
	pt_scarab,
	pt_darken,
	pt_grensmoke,
	pt_redfire,
	pt_acidball,
	pt_bluestep
};

/* A value HexenWorld never defined becomes pt_static, as an unknown type
 * should not pick up some other effect's physics. */
static inline ptype_t HW_H2ParticleMap (int hw_type)
{
	if (hw_type < 0 || hw_type >= HW_PARTICLE_TYPES)
		return pt_static;
	return hw_to_h2_particle_map[hw_type];
}

#endif	/* __HW_PARTICLE_MAPPING_H */
