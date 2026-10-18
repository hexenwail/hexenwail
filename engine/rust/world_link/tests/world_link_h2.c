/*
 * world_link_h2.c -- the C original of world.c's area linking, moved
 * verbatim out of engine/hexen2/world.c.  The differential harness
 * beside this file includes it; no engine target compiles it.  The
 * engine's implementation is engine/rust/src/world_link.rs.
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

/* world.c's own include, quakedef.h, comes from the harness, and so does
 * sv_areanodes, which stays world.c's storage. */

static	int			sv_numareanodes;

/*
===============
SV_CreateAreaNode

===============
*/
static areanode_t *SV_CreateAreaNode (int depth, vec3_t mins, vec3_t maxs)
{
	areanode_t	*anode;
	vec3_t		size;
	vec3_t		mins1, maxs1, mins2, maxs2;

	anode = &sv_areanodes[sv_numareanodes];
	sv_numareanodes++;

	ClearLink (&anode->trigger_edicts);
	ClearLink (&anode->solid_edicts);

	if (depth == AREA_DEPTH)
	{
		anode->axis = -1;
		anode->children[0] = anode->children[1] = NULL;
		return anode;
	}

	VectorSubtract (maxs, mins, size);
	if (size[0] > size[1])
		anode->axis = 0;
	else
		anode->axis = 1;

	anode->dist = 0.5 * (maxs[anode->axis] + mins[anode->axis]);
	VectorCopy (mins, mins1);
	VectorCopy (mins, mins2);
	VectorCopy (maxs, maxs1);
	VectorCopy (maxs, maxs2);

	maxs1[anode->axis] = mins2[anode->axis] = anode->dist;

	anode->children[0] = SV_CreateAreaNode (depth+1, mins2, maxs2);
	anode->children[1] = SV_CreateAreaNode (depth+1, mins1, maxs1);

	return anode;
}

/*
===============
SV_ClearWorld

===============
*/
void SV_ClearWorld (void)
{
	SV_InitBoxHull ();

	memset (sv_areanodes, 0, sizeof(sv_areanodes));
	sv_numareanodes = 0;
	SV_CreateAreaNode (0, sv.worldmodel->mins, sv.worldmodel->maxs);
}


/*
===============
SV_UnlinkEdict

===============
*/
static link_t	**sv_link_next;
static link_t	**sv_link_prev;

void SV_UnlinkEdict (edict_t *ent)
{
	if (!ent->area.prev)
		return;		// not linked in anywhere
	RemoveLink (&ent->area);
	if (sv_link_next && *sv_link_next == &ent->area)
		*sv_link_next = ent->area.next;
	if (sv_link_prev && *sv_link_prev == &ent->area)
		*sv_link_prev = ent->area.prev;
	ent->area.prev = ent->area.next = NULL;
}


/*
====================
SV_TouchLinks
====================
*/
static void SV_TouchLinks (edict_t *ent, areanode_t *node)
{
	link_t		*l, *lnext;
	edict_t		*touch;
	int			old_self, old_other;

loc0:
	// touch linked edicts
	sv_link_next = &lnext;
	for (l = node->trigger_edicts.next ; l != &node->trigger_edicts ; l = lnext)
	{
		if (!l)
		{
		// my area got removed out from under me!
			Con_Printf ("%s: encountered NULL link!\n", __thisfunc__);
			break;
		}

		lnext = l->next;
		touch = EDICT_FROM_AREA(l);
		if (touch == ent)
			continue;
		if (!touch->v.touch || touch->v.solid != SOLID_TRIGGER)
			continue;
		if (ent->v.absmin[0] > touch->v.absmax[0]
				|| ent->v.absmin[1] > touch->v.absmax[1]
				|| ent->v.absmin[2] > touch->v.absmax[2]
				|| ent->v.absmax[0] < touch->v.absmin[0]
				|| ent->v.absmax[1] < touch->v.absmin[1]
				|| ent->v.absmax[2] < touch->v.absmin[2] )
			continue;

		old_self = *sv_globals.self;
		old_other = *sv_globals.other;

		*sv_globals.self = EDICT_TO_PROG(touch);
		*sv_globals.other = EDICT_TO_PROG(ent);
		*sv_globals.time = sv.time;
		PR_ExecuteProgram (touch->v.touch, "touch");

		*sv_globals.self = old_self;
		*sv_globals.other = old_other;
	}

	sv_link_next = NULL;

	// recurse down both sides
	if (node->axis == -1)
		return;

	// LordHavoc: optimized recursion
	//if (ent->v.absmax[node->axis] > node->dist) SV_TouchLinks (ent, node->children[0]);
	//if (ent->v.absmin[node->axis] < node->dist) SV_TouchLinks (ent, node->children[1]);
	if (ent->v.absmax[node->axis] > node->dist)
	{
		if (ent->v.absmin[node->axis] < node->dist)
			SV_TouchLinks(ent, node->children[1]); // order reversed to reduce code
		node = node->children[0];
		goto loc0;
	}
	else
	{
		if (ent->v.absmin[node->axis] < node->dist)
		{
			node = node->children[1];
			goto loc0;
		}
	}
}


/*
===============
SV_FindTouchedLeafs

===============
*/
static void SV_FindTouchedLeafs (edict_t *ent, mnode_t *node)
{
	mplane_t	*splitplane;
	mleaf_t		*leaf;
	int			sides;
	int			leafnum;

loc0:
	if (node->contents == CONTENTS_SOLID)
		return;

	// add an efrag if the node is a leaf

	if (node->contents < 0)
	{
		if (ent->num_leafs == MAX_ENT_LEAFS)
			return;

		leaf = (mleaf_t *)node;
		leafnum = leaf - sv.worldmodel->leafs - 1;

		ent->leafnums[ent->num_leafs] = leafnum;
		ent->num_leafs++;
		return;
	}

	// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(ent->v.absmin, ent->v.absmax, splitplane);

	// recurse down the contacted sides
	// LordHavoc: optimized recursion
	//if (sides & 1) SV_FindTouchedLeafs (ent, node->children[0]);
	//if (sides & 2) SV_FindTouchedLeafs (ent, node->children[1]);
	switch (sides)
	{
	case 1:
		node = node->children[0];
		goto loc0;
	case 2:
		node = node->children[1];
		goto loc0;
	default: // 3
		if (node->children[0]->contents != CONTENTS_SOLID)
			SV_FindTouchedLeafs (ent, node->children[0]);
		node = node->children[1];
		goto loc0;
	}
}

/*
===============
SV_LinkEdict

===============
*/
void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)
{
	areanode_t	*node;

	if (ent->area.prev)
		SV_UnlinkEdict (ent);	// unlink from old position

	if (ent == sv.edicts)
		return;		// don't add the world

	if (ent->free)
		return;

	// set the abs box
	if (ent->v.solid == SOLID_BSP && 
		(ent->v.angles[0] || ent->v.angles[1] || ent->v.angles[2]) )
	{	// expand for rotation
		float		v, maxv;
		int			i;

		maxv = 0;
		for (i = 0; i < 3; i++)
		{
			v = fabs(ent->v.mins[i]);
			if (v > maxv)
				maxv = v;
			v = fabs(ent->v.maxs[i]);
			if (v > maxv)
				maxv = v;
		}
		for (i = 0; i < 3; i++)
		{
			ent->v.absmin[i] = ent->v.origin[i] - maxv;
			ent->v.absmax[i] = ent->v.origin[i] + maxv;
		}
	}
	else
	{
		VectorAdd (ent->v.origin, ent->v.mins, ent->v.absmin);
		VectorAdd (ent->v.origin, ent->v.maxs, ent->v.absmax);
	}

//
// to make items easier to pick up and allow them to be grabbed off
// of shelves, the abs sizes are expanded
//
	if ((int)ent->v.flags & FL_ITEM)
	{
		ent->v.absmin[0] -= 15;
		ent->v.absmin[1] -= 15;
		ent->v.absmax[0] += 15;
		ent->v.absmax[1] += 15;
	}
	else
	{	// because movement is clipped an epsilon away from an actual edge,
		// we must fully check even when bounding boxes don't quite touch
		ent->v.absmin[0] -= 1;
		ent->v.absmin[1] -= 1;
		ent->v.absmin[2] -= 1;
		ent->v.absmax[0] += 1;
		ent->v.absmax[1] += 1;
		ent->v.absmax[2] += 1;
	}

	// link to PVS leafs
	ent->num_leafs = 0;
	if (ent->v.modelindex)
		SV_FindTouchedLeafs (ent, sv.worldmodel->nodes);

	if (ent->v.solid == SOLID_NOT)
		return;

	// find the first node that the ent's box crosses
	node = sv_areanodes;
	while (1)
	{
		if (node->axis == -1)
			break;
		if (ent->v.absmin[node->axis] > node->dist)
			node = node->children[0];
		else if (ent->v.absmax[node->axis] < node->dist)
			node = node->children[1];
		else
			break;		// crosses the node
	}

	// link it in

	if (ent->v.solid == SOLID_TRIGGER)
		InsertLinkBefore (&ent->area, &node->trigger_edicts);
	else
		InsertLinkBefore (&ent->area, &node->solid_edicts);

	// if touch_triggers, touch all entities at this node and decend for more
	if (touch_triggers)
		SV_TouchLinks ( ent, sv_areanodes );
}
