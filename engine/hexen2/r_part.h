/* r_part.h -- exported functions from r_part.c
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

#ifndef __R_PART_H
#define __R_PART_H

/* Map leak path from maps/<map>.pts, loaded by the `pointfile` command and
 * drawn as direction arrows by R_ShowPointFile (Ironwail 26902e0e2).  The
 * cap is on stored points, which R_ReadPointFile_f has already collapsed
 * along straight runs, so it bounds direction changes rather than raw file
 * lines.  Stored by r_part.c, which every renderer compiles; only the
 * drawing side is GL-specific. */
#define MAX_POINTFILE_POINTS	16384
extern vec3_t	r_pointfile[MAX_POINTFILE_POINTS];
extern int	r_numpointfile;
/* True only for the `pointfile leak` auto-load R_NewMap issues on a world
 * with no visdata, i.e. when the path is known to be a real leak rather than
 * a stale .pts a mapper asked for by hand.  Gates the on-screen label. */
extern qboolean	r_pointfile_isleak;

void R_DrawParticles (void);
void R_InitParticles (void);
void R_ClearParticles (void);
#ifdef GLQUAKE
void R_GPU_Particles_Shutdown (void);
#endif
void R_UpdateParticles (void);

void R_ParseParticleEffect (void);
void R_ParseParticleEffect2 (void);
void R_ParseParticleEffect3 (void);
void R_ParseParticleEffect4 (void);

void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count);
void R_RunParticleEffect2 (vec3_t org, vec3_t dmin, vec3_t dmax, int color, ptype_t effect, int count);
					/* for ptype_t, d_iface.h or glquake.h must be included before. */

void R_ParticleExplosion (vec3_t org);
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength);
void R_ColoredParticleExplosion (vec3_t org, int color, int radius, int counter);
void R_BlobExplosion (vec3_t org);

void R_RocketTrail (vec3_t start, vec3_t end, int type);
void R_SunStaffTrail (vec3_t source, vec3_t dest);

void R_LavaSplash (vec3_t org);
void R_TeleportSplash (vec3_t org);

/* z_dir: fall speed in units/sec, 0 for the stock random 256-955 spread */
void R_RainEffect (vec3_t org, vec3_t e_size, int x_dir, int y_dir, int z_dir, int color, int count);
void R_SnowEffect (vec3_t org1, vec3_t org2, int flags, vec3_t alldir, int count);

void R_RunQuakeEffect (vec3_t org, float distance);

void RiderParticle (int count, vec3_t origin);
void GravityWellParticle (int count, vec3_t origin, int color);
void R_DarkFieldParticles (entity_t *ent);

void R_EntityParticles (entity_t *ent);

/*
 * NOTES: R_EntityParticles, R_ParticleExplosion2
 *	and R_BlobExplosion actually are not used.
 */

#endif	/* __R_PART_H */

