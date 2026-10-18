/* world.c -- world query functions
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

#ifndef __HX2_WORLD_H
#define __HX2_WORLD_H

typedef struct
{
	vec3_t	normal;
	float	dist;
} plane_t;

typedef struct
{
	qboolean	allsolid;	// if true, plane is not valid
	qboolean	startsolid;	// if true, the initial point was in a solid area
	qboolean	inopen, inwater;
	float	fraction;		// time completed, 1.0 = didn't hit anything
	vec3_t	endpos;			// final position
	plane_t	plane;			// surface normal at impact
	edict_t	*ent;			// entity the surface is on
} trace_t;


#define	MOVE_NORMAL		0
#define	MOVE_NOMONSTERS	1
#define	MOVE_MISSILE	2
#define	MOVE_WATER		3
#define	MOVE_PHASE		4

typedef struct areanode_s
{
	int		axis;		// -1 = leaf node
	float	dist;
	struct areanode_s	*children[2];
	link_t	trigger_edicts;
	link_t	solid_edicts;
} areanode_t;

#define	AREA_DEPTH	4
#define	AREA_NODES	32

extern	areanode_t	sv_areanodes[AREA_NODES];


void SV_ClearWorld (void);
// called after the world model has been loaded, before linking any entities

void SV_UnlinkEdict (edict_t *ent);
// call before removing an entity, and before trying to move one,
// so it doesn't clip against itself
// flags ent->v.modified

void SV_LinkEdict (edict_t *ent, qboolean touch_triggers);
// Needs to be called any time an entity changes origin, mins, maxs, or solid
// flags ent->v.modified
// sets ent->v.absmin and ent->v.absmax
// if touchtriggers, calls prog functions for the intersected triggers

int SV_PointContents (vec3_t p);
// returns the CONTENTS_* value from the world at the given point.
// does not check any entities at all

edict_t	*SV_TestEntityPosition (edict_t *ent);

trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict);
// mins and maxs are reletive

// if the entire move stays in a solid volume, trace.allsolid will be set

// if the starting point is in a solid, it will be allowed to move out
// to an open area

// nomonsters is used for line of sight or edge testing, where mosnters
// shouldn't be considered solid objects

// passedict is explicitly excluded from clipping checks (normally NULL)


/* Declared but defined nowhere: world.c had it under `#if 0` ("no callers"),
 * and it moved to engine/rust/world_move/tests/ with the rest, unported. */
edict_t	*SV_TestPlayerPosition (edict_t *ent, vec3_t origin);

/* Both defined in engine/rust/src/world_trace.rs (#455); until then they were
 * world.c's own, SV_HullPointContents a static. */
int SV_HullPointContents (hull_t *hull, int num, vec3_t p);
qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace);
/* Both defined in engine/rust/src/world_hull.rs (#455); world.c's statics
 * until then.  Their callers, SV_ClearWorld and SV_ClipMoveToEntity, are
 * Rust too now (world_link.rs, world_move.rs). */
void SV_InitBoxHull (void);
hull_t *SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs, vec3_t offset, edict_t *move_ent);

/* sv_move.c's sv_debugmovestep diagnostics, called by the monster movement in
 * engine/rust/src/sv_move.rs (#455) */
void SV_DebugMoveStep_Log (const edict_t *ent, const trace_t *trace, const char *fmt, ...) FUNC_PRINTF(3,4);
void SV_DebugMoveStep_DumpMovers (const vec3_t foot);
qboolean SV_DebugMoveStep_Call (const edict_t *ent, const vec3_t mins,
				const vec3_t maxs, qboolean ok, const char *why);

/* sv_phys.c's base helpers, now engine/rust/src/sv_phys.rs (#455) */
void SV_CheckVelocity (edict_t *ent);
qboolean SV_RunThink (edict_t *ent);
void SV_Impact (edict_t *e1, edict_t *e2);
int ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce);
int SV_FlyMove (edict_t *ent, float time, trace_t *steptrace);
void SV_AddGravity (edict_t *ent, float scale);
trace_t SV_PushEntity (edict_t *ent, vec3_t push);

/* sv_phys.c's pushers, now engine/rust/src/sv_phys_push.rs (#455) */
qboolean SV_Push (edict_t *pusher, vec3_t move);
void SV_PushMove (edict_t *pusher, float movetime, qboolean update_time);
void SV_PushRotate (edict_t *pusher, float movetime);
void SV_Physics_Pusher (edict_t *ent);

#endif	/* __HX2_WORLD_H */
