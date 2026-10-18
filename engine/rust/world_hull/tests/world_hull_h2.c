/*
 * world_hull_h2.c -- the C original of the box hull and SV_HullForEntity,
 * moved verbatim out of engine/hexen2/world.c.  The differential harness
 * beside this file includes it; no engine target compiles it.  The
 * engine's implementation is engine/rust/src/world_hull.rs.
 *
 * (from world.c -- world query functions)
 *
 * entities never clip against themselves, or their owner
 * line of sight checks trace->crosscontent, but bullets don't
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

/* world.c's own include, quakedef.h, comes from the harness. */

static	hull_t		box_hull;
static	mclipnode_t	box_clipnodes[6];
static	mplane_t	box_planes[6];

/*
===================
SV_InitBoxHull

Set up the planes and clipnodes so that the six floats of a bounding box
can just be stored out and get a proper hull_t structure.
===================
*/
static void SV_InitBoxHull (void)
{
	int		i;
	int		side;

	box_hull.clipnodes = box_clipnodes;
	box_hull.planes = box_planes;
	box_hull.firstclipnode = 0;
	box_hull.lastclipnode = 5;

	for (i = 0; i < 6; i++)
	{
		box_clipnodes[i].planenum = i;

		side = i & 1;

		box_clipnodes[i].children[side] = CONTENTS_EMPTY;
		if (i != 5)
			box_clipnodes[i].children[side^1] = i + 1;
		else
			box_clipnodes[i].children[side^1] = CONTENTS_SOLID;

		box_planes[i].type = i>>1;
		box_planes[i].normal[i>>1] = 1;
	}
}


/*
===================
SV_HullForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
static hull_t	*SV_HullForBox (vec3_t mins, vec3_t maxs)
{
	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];

	return &box_hull;
}


/*
================
SV_HullForEntity

Returns a hull that can be used for testing or clipping an object of mins/maxs
size.
Offset is filled in to contain the adjustment that must be added to the
testing object's origin to get a point to use with the returned hull.
================
*/
static hull_t *SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs, vec3_t offset, edict_t *move_ent)
{
	qmodel_t	*model;
	vec3_t		size;
	vec3_t		hullmins, hullmaxs;
	hull_t		*hull;
	int			idx;

	// decide which clipping hull to use, based on the size
	if (ent->v.solid == SOLID_BSP)
	{	// explicit hulls in the BSP model
		if (ent->v.movetype != MOVETYPE_PUSH)
			Sys_Error ("SOLID_BSP without MOVETYPE_PUSH");

		model = sv.models[ (int)ent->v.modelindex ];

		if (!model || model->type != mod_brush)
			Sys_Error ("SOLID_BSP with a non bsp model");

		VectorSubtract (maxs, mins, size);

		//THIS IS WHERE THE MONSTER STEPPING ERROR WAS- IN CHECKBOTTOM,
		//A '0 0 0' MINS AND MAXS WERE SENT, BUT HERE, IT LOOKS TO SEE
		//IF THE MONSTER HAS A HULL AND CALCULATES THE OFFSET FROM
		//THE HULL MINS AND MAXS AND THE PASSED MINS AND MAXS,
		//THIS WILL INCORRECTLY OFFSET THE TEST MOVE BY THE MINS AND
		//MAXS OF THE MONSTER!  WILL CHECK FOR SIDE EFFECTS...

		if (move_ent->v.hull)	// Entity is specifying which hull to use
		{
			idx = move_ent->v.hull-1;
			hull = &model->hulls[idx];
			if (!hull)  // Invalid hull
			{
				Con_Printf ("ERROR: hull %d is null.\n", idx);
				hull = &model->hulls[0];
			}
		}
		else	// Using the old way uses size to determine hull to use
		{
		//	if ((int)move_ent->v.flags & FL_MONSTER)
		//		Con_DPrintf("ERROR: auto-detecing hull for monster!\n");
			if (size[0] < 3) // Point
				hull = &model->hulls[0];
			else if ( (size[0] <= 8) && ((int)(sv.edicts->v.spawnflags) & 1) )  // Pentacles
				hull = &model->hulls[4];
			else if (size[0] <= 32 && size[2] <= 28) // Half Player
				hull = &model->hulls[3];
			else if (size[0] <= 32) // Full Player
				hull = &model->hulls[1];
			else // Golem
				hull = &model->hulls[5];
		}

	// calculate an offset value to center the origin:
		VectorSubtract (hull->clip_mins, mins, offset);
		if ((int)move_ent->v.flags & FL_MONSTER)
		{
			if (offset[0] != 0 || offset[1] != 0)
			{
			//	Con_DPrintf("ERROR: Non-zero offset (%f,%f,%f)!!!\n",
			//			offset[0], offset[1], offset[2]);
			// 524288 (FL_MISMATCHEDBOUNDS ?) is an abandoned H2MP flag?
			// if ((int)move_ent->v.flags2 & 524288)
				offset[0] = 0;
				offset[1] = 0;
			}
		}
		VectorAdd (offset, ent->v.origin, offset);
	}
	else
	{	// create a temp hull from bounding box sizes
		VectorSubtract (ent->v.mins, maxs, hullmins);
		VectorSubtract (ent->v.maxs, mins, hullmaxs);
		hull = SV_HullForBox (hullmins, hullmaxs);

		VectorCopy (ent->v.origin, offset);
	}

	return hull;
}
