/*
 * sv_main_ents_h2.c -- the C original of the server's entity updates, moved
 * verbatim out of engine/hexen2/sv_main.c: the fat PVS (SV_AddToFatPVS,
 * SV_FatPVS), SV_EntityUpdateSize, SV_EntityPriority, SV_PrepareClientEntities
 * and SV_CleanupEnts.  The differential harness beside this file includes it;
 * no engine target compiles it.  The engine's implementation is
 * engine/rust/src/sv_main_ents.rs (#469 slice B).  tools/test_sv_maxdatagram.py
 * pins SV_PrepareClientEntities' trailer reserve here.
 *
 * (from sv_main.c -- server main program)
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

/* sv_main.c's own include, quakedef.h, comes from the harness, and so do
 * sv, svs, the cvars and the engine functions these call. */

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static int	fatbytes;
static byte	fatpvs[MAX_MAP_LEAFS/8];

static void SV_AddToFatPVS (vec3_t org, mnode_t *node)
{
	int		i;
	byte	*pvs;
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != CONTENTS_SOLID)
			{
				pvs = Mod_LeafPVS ( (mleaf_t *)node, sv.worldmodel);
				for (i = 0; i < fatbytes; i++)
					fatpvs[i] |= pvs[i];
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_AddToFatPVS (org, node->children[0]);
			node = node->children[1];
		}
	}
}

/*
=============
SV_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
static byte *SV_FatPVS (vec3_t org)
{
	fatbytes = (sv.worldmodel->numleafs+31)>>3;
	memset (fatpvs, 0, fatbytes);
	SV_AddToFatPVS (org, sv.worldmodel->nodes);
	return fatpvs;
}

#define CLIENT_FRAME_INIT	255
#define CLIENT_FRAME_RESET	254

/*
 * Room held back at the end of the per-frame datagram for the svc_clear_edicts
 * trailer that SV_PrepareClientEntities always writes: the opcode, the count
 * byte, and up to MAX_CLEAR_EDICTS_PER_MSG shorts.  Removals are collected all
 * through the entity loop, so the worst case has to be reserved up front --
 * by the time the list is full the message is already written.
 */
#define DATAGRAM_TRAILER_RESERVE	(2 + 2 * MAX_CLEAR_EDICTS_PER_MSG)

/*
 * Exact size of the entity update that the writer below emits for `bits`.
 * Computed rather than estimated: U_SKIN and U_SCALE carry two bytes each,
 * U_LONGENTITY and the U_MOREBITS chain change the header length, and every
 * optional field is independently present or absent, so a single worst-case
 * number would shed entities that would have fit.  Must be kept in step with
 * the write block in SV_PrepareClientEntities.
 */
static int SV_EntityUpdateSize (int bits)
{
	int	len;

	len = 1;					/* bits | U_SIGNAL */
	if (bits & U_MOREBITS)		len += 1;
	if (bits & U_MOREBITS2)		len += 1;
	len += (bits & U_LONGENTITY) ? 2 : 1;	/* entity number */

	if (bits & U_MODEL)		len += 2;
	if (bits & U_FRAME)		len += 1;
	if (bits & U_COLORMAP)		len += 1;
	if (bits & U_SKIN)		len += 2;	/* skin + drawflags */
	if (bits & U_EFFECTS)		len += 1;
	if (bits & U_ORIGIN1)		len += 2;
	if (bits & U_ANGLE1)		len += 1;
	if (bits & U_ORIGIN2)		len += 2;
	if (bits & U_ANGLE2)		len += 1;
	if (bits & U_ORIGIN3)		len += 2;
	if (bits & U_ANGLE3)		len += 1;
	if (bits & U_SCALE)		len += 2;	/* scale + abslight */
	if (bits & U_ALPHA)		len += 1;

	return len;
}

/* Largest value SV_EntityUpdateSize() can return, i.e. every optional field
 * present at once.  Used only to decide, before doing any work, whether this
 * frame could possibly overflow the datagram at all. */
#define NETSORT_MAX_UPDATE	24

/* Ironwail compresses the priority key into 256 bins so a counting sort can
 * order the whole entity list in one pass.  We cannot sort the write order
 * (see SV_PrepareClientEntities), but the same 256 bins give us the cutoff. */
#define NETSORT_BINS		256

/*
 * Ironwail's entity priority key, from SV_WriteEntitiesToClient: the scaled
 * fourth root of (squared distance from the viewer to the entity's bounding
 * box) over (the box's squared diagonal), so that a large distant brush entity
 * -- a lift, a rotator -- outranks a small nearby gib.  The fourth root is
 * what squeezes a map-sized range of distances into 0..255.
 *
 * Lower is more important.  The client's own entity is pinned at 0.
 */
static int SV_EntityPriority (edict_t *ent, const vec3_t org, qboolean is_clent)
{
	float	dist, size, delta;
	int	i;

	if (is_clent)
		return 0;

	dist = size = 0.0f;
	for (i = 0; i < 3; i++)
	{
		delta = CLAMP(ent->v.absmin[i], org[i], ent->v.absmax[i]) - org[i];
		dist += delta * delta;
		delta = ent->v.absmax[i] - ent->v.absmin[i];
		size += delta * delta;
	}

	if (size < 1.0f)
		size = 1.0f;

	dist = 8.0f * sqrt(sqrt(dist / size));
	if (dist >= (float)(NETSORT_BINS - 1))
		return NETSORT_BINS - 1;

	return (int) dist;
}

static void SV_PrepareClientEntities (client_t *client, edict_t	*clent, sizebuf_t *msg)
{
	int		e, i;
	int		bits;
	byte	*pvs;
	vec3_t	org;
	float	miss;
	edict_t	*ent;
	int		temp_index;
	char	NewName[MAX_QPATH];
	long	flagtest;
	int			position = 0;
	int			client_num;
	client_frames_t	*reference, *build;
	client_state2_t	*state;
	entity_state2_t	*ref_ent, *set_ent, build_ent, saved_ref;
	qboolean		FoundInList,DoRemove,DoPlayer,DoMonsters,DoMissiles,DoMisc,IgnoreEnt;
	short			RemoveList[MAX_CLIENT_STATES],NumToRemove;
	int			NumDeferred;
	int			NumShed;
	int			NumSorted;
	int			ShedNearest;
	qboolean		measuring;
	int			trailer_reserve = DATAGRAM_TRAILER_RESERVE;
	int			netsort_cutoff, bcount, cursize, prio, bytes;
	int			binbytes[NETSORT_BINS];
	entity_state2_t		scratch_ref, scratch_set;

	client_num = client-svs.clients;
	state = &sv.states[client_num];
	reference = &state->frames[0];

	if (client->last_sequence != client->current_sequence)
	{	// Old sequence
	//	Con_Printf("SV: Old sequence SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame++;
		if (client->current_frame > MAX_FRAMES+1)
			client->current_frame = MAX_FRAMES+1;
	}
	else if (client->last_frame == CLIENT_FRAME_INIT ||
			 client->last_frame == 0 ||
			 client->last_frame == MAX_FRAMES+1)
	{	// Reference expired in current sequence
	//	Con_Printf("SV: Expired SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame = 1;
		client->current_sequence++;
	}
	else if (client->last_frame >= 1 && client->last_frame <= client->current_frame)
	{	// Got a valid frame
	//	Con_Printf("SV: Valid SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		*reference = state->frames[client->last_frame];

		for (i = 0; i < reference->count; i++)
		{
			if (reference->states[i].flags & ENT_CLEARED)
			{
				e = reference->states[i].index;
				ent = EDICT_NUM(e);
				if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
				{
					ent->baseline.ClearCount[client_num]++;
				}
				else if (ent->baseline.ClearCount[client_num] == CLEAR_LIMIT)
				{
					ent->baseline.ClearCount[client_num] = 3;
					reference->states[i].flags &= ~ENT_CLEARED;
				}
			}
		}
		client->current_frame = 1;
		client->current_sequence++;
	}
	else
	{	// Normal frame advance
	//	Con_Printf("SV: Normal SV(%d,%d) CL(%d,%d)\n",client->current_sequence, client->current_frame, client->last_sequence, client->last_frame);
		client->current_frame++;
		if (client->current_frame > MAX_FRAMES+1)
			client->current_frame = MAX_FRAMES+1;
	}

	DoPlayer = DoMonsters = DoMissiles = DoMisc = false;

	if (sv_update_player.integer)
		DoPlayer = (client->current_sequence % sv_update_player.integer) == 0;
	if (sv_update_monsters.integer)
		DoMonsters = (client->current_sequence % sv_update_monsters.integer) == 0;
	if (sv_update_missiles.integer)
		DoMissiles = (client->current_sequence % sv_update_missiles.integer) == 0;
	if (sv_update_misc.integer)
		DoMisc = (client->current_sequence % sv_update_misc.integer) == 0;

	build = &state->frames[client->current_frame];
	memset(build, 0, sizeof(*build));
	client->last_frame = CLIENT_FRAME_RESET;

	MSG_WriteByte (msg, svc_reference);
	MSG_WriteByte (msg, client->current_frame);
	MSG_WriteByte (msg, client->current_sequence);

	// find the client's PVS
	if (clent->v.cameramode)
	{
		ent = PROG_TO_EDICT(clent->v.cameramode);
		VectorCopy(ent->v.origin, org);
	}
	else
		VectorAdd (clent->v.origin, clent->v.view_ofs, org);

	pvs = SV_FatPVS (org);

	/*
	 * sv_netsort.  Ironwail counting-sorts the entities by priority and then
	 * writes them best-first, so the packet filling up IS the cutoff.  We
	 * cannot do that: this is a reference-frame delta protocol, the walk over
	 * reference->states[] below is a monotonic cursor, and build->states[]
	 * becomes the next reference -- both have to stay in ascending edict
	 * order.  The write order is not ours to choose.
	 *
	 * The membership is.  So the loop below runs twice when the frame might
	 * overflow: once measuring, which commits nothing -- no writes to msg, no
	 * ENT_CLEARED rewrite of the reference frame, no build->states[] -- and
	 * only totals the exact byte cost of every update per priority bin; then
	 * once for real, shedding the entities whose bin falls past the point
	 * where those totals ran out of datagram.
	 *
	 * Measuring is skipped when the frame cannot overflow even if every edict
	 * sent its largest possible update, which is the ordinary case.  The
	 * cutoff then stays past the last bin, nothing is shed for priority, and
	 * the write pass is byte-for-byte what it was before -- as it also is
	 * under sv_netsort 0.
	 *
	 * Only whole bins are admitted.  Letting the bin the budget runs out in
	 * through as well and leaning on the guard to truncate it measurably
	 * loses entities that netsort is there to keep: its members are the worst
	 * of the admitted set, and any of them early in edict order spends budget
	 * that a better entity later in edict order then has to be shed for.
	 * Stopping a bin short instead leaves a sliver of the datagram unused,
	 * which costs nothing anyone can see.
	 *
	 * The exact per-entity guard further down remains the backstop either
	 * way, and carries the one case the bins cannot help with: a first bin
	 * that overflows the budget on its own, where there is nothing to choose
	 * between the candidates and edict order is as fair a tiebreak as any.
	 */
	netsort_cutoff = NETSORT_BINS;
	measuring = sv_netsort.integer &&
			msg->cursize + sv.num_edicts * NETSORT_MAX_UPDATE
				+ DATAGRAM_TRAILER_RESERVE > msg->maxsize;

restart:
	NumToRemove = 0;
	NumDeferred = 0;
	NumShed = 0;
	NumSorted = 0;
	ShedNearest = NETSORT_BINS;
	position = 0;
	bcount = 0;
	cursize = msg->cursize;
	memset (binbytes, 0, sizeof(binbytes));

	// send over all entities (except the client) that touch the pvs
	ent = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, ent = NEXT_EDICT(ent))
	{
		DoRemove = false;
		// don't send if flagged for NODRAW and there are no lighting effects
		if (ent->v.effects == EF_NODRAW)
		{
			DoRemove = true;
			goto skipA;
		}

		// ignore if not touching a PV leaf
		if (ent != clent)	// clent is ALWAYS sent
		{	// ignore ents without visible models
			if (!ent->v.modelindex || !*PR_GetString(ent->v.model))
			{
				DoRemove = true;
				goto skipA;
			}

			for (i = 0; i < ent->num_leafs; i++)
			{
				if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i] & 7)) )
					break;
			}

			/* If num_leafs == MAX_ENT_LEAFS the leaf list overflowed —
			 * we don't know what the rest of the entity's leaves are,
			 * so we can't safely PVS-cull.  Always send.  Common with
			 * long lifts, rotators, and big brush ents that span many
			 * leaves.  Matches ericw/Ironwail behaviour. */
			if (i == ent->num_leafs && ent->num_leafs < MAX_ENT_LEAFS)
			{
				DoRemove = true;
				goto skipA;
			}
		}

skipA:
		IgnoreEnt = false;
		flagtest = (long)ent->v.flags;
		if (!DoRemove)
		{
			if (flagtest & FL_CLIENT)
			{
				if (!DoPlayer)
					IgnoreEnt = true;
			}
			else if (flagtest & FL_MONSTER)
			{
				if (!DoMonsters)
					IgnoreEnt = true;
			}
			else if (ent->v.movetype == MOVETYPE_FLYMISSILE ||
					 ent->v.movetype == MOVETYPE_BOUNCEMISSILE ||
					 ent->v.movetype == MOVETYPE_BOUNCE)
			{
				if (!DoMissiles)
					IgnoreEnt = true;
			}
			else
			{
				if (!DoMisc)
					IgnoreEnt = true;
			}
		}

		bits = 0;

		while (position < reference->count && 
			   e > reference->states[position].index)
			position++;

		if (position < reference->count && reference->states[position].index == e)
		{
			FoundInList = true;
			if (DoRemove)
			{
				if (NumToRemove < MAX_CLEAR_EDICTS_PER_MSG)
				{
					RemoveList[NumToRemove] = e;
					NumToRemove++;
					continue;
				}

				/* Remove list is full for this message.  svc_clear_edicts
				 * describes its count in one byte, so removals past
				 * MAX_CLEAR_EDICTS_PER_MSG cannot be expressed on the wire.
				 * Fall through as if the entity were merely ignored: it is
				 * kept in the build list, so it survives in the client's
				 * reference frame and is removed by a later message.
				 * Dropping it here instead would leave it out of the
				 * reference with BE_ON still set on the client, stranding it
				 * as a permanent ghost. */
				NumDeferred++;
				IgnoreEnt = true;
			}
			ref_ent = &reference->states[position];
			if (measuring)
			{	/* Off a copy: the ENT_CLEARED rewrite below must not reach
				 * the real reference frame from a pass whose updates are
				 * never sent. */
				scratch_ref = *ref_ent;
				ref_ent = &scratch_ref;
			}
		}
		else
		{
			if (DoRemove || IgnoreEnt)
				continue;

			ref_ent = &build_ent;

			build_ent.index = e;
			build_ent.origin[0] = ent->baseline.origin[0];
			build_ent.origin[1] = ent->baseline.origin[1];
			build_ent.origin[2] = ent->baseline.origin[2];
			build_ent.angles[0] = ent->baseline.angles[0];
			build_ent.angles[1] = ent->baseline.angles[1];
			build_ent.angles[2] = ent->baseline.angles[2];
			build_ent.modelindex = ent->baseline.modelindex;
			build_ent.frame = ent->baseline.frame;
			build_ent.colormap = ent->baseline.colormap;
			build_ent.skin = ent->baseline.skin;
			build_ent.effects = ent->baseline.effects;
			build_ent.scale = ent->baseline.scale;
			build_ent.drawflags = ent->baseline.drawflags;
			build_ent.abslight = ent->baseline.abslight;
			build_ent.alpha = ent->baseline.alpha;
			build_ent.flags = 0;

			FoundInList = false;
		}

		/* Snapshot the reference entry before the ENT_CLEARED memset below
		 * rewrites it: if this entity's update turns out not to fit in the
		 * datagram, nothing was sent, so the reference has to go back to
		 * describing what the client actually holds. */
		saved_ref = *ref_ent;

		set_ent = measuring ? &scratch_set : &build->states[bcount];
		bcount++;
		if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
		{
			memset(ref_ent, 0, sizeof(*ref_ent));
			ref_ent->index = e;
		}
		*set_ent = *ref_ent;

		if (IgnoreEnt)
			continue;

		// send an update
		for (i = 0; i < 3; i++)
		{
			miss = ent->v.origin[i] - ref_ent->origin[i];
			if ( miss < -0.1 || miss > 0.1 )
			{
				bits |= U_ORIGIN1<<i;
				set_ent->origin[i] = ent->v.origin[i];
			}
		}

		if ( ent->v.angles[0] != ref_ent->angles[0] )
		{
			bits |= U_ANGLE1;
			set_ent->angles[0] = ent->v.angles[0];
		}

		if ( ent->v.angles[1] != ref_ent->angles[1] )
		{
			bits |= U_ANGLE2;
			set_ent->angles[1] = ent->v.angles[1];
		}

		if ( ent->v.angles[2] != ref_ent->angles[2] )
		{
			bits |= U_ANGLE3;
			set_ent->angles[2] = ent->v.angles[2];
		}

		if (ent->v.movetype == MOVETYPE_STEP)
			bits |= U_NOLERP;	// don't mess up the step animation

		if (ref_ent->colormap != ent->v.colormap)
		{
			bits |= U_COLORMAP;
			set_ent->colormap = ent->v.colormap;
		}

		if (ref_ent->skin != ent->v.skin
			|| ref_ent->drawflags != ent->v.drawflags)
		{
			bits |= U_SKIN;
			set_ent->skin = ent->v.skin;
			set_ent->drawflags = ent->v.drawflags;
		}

		if (ref_ent->frame != ent->v.frame)
		{
			bits |= U_FRAME;
			set_ent->frame = ent->v.frame;
		}

		if (ref_ent->effects != ent->v.effects)
		{
			bits |= U_EFFECTS;
			set_ent->effects = ent->v.effects;
		}

	//	flagtest = (long)ent->v.flags;
		if (flagtest & 0xff000000)
		{
			Host_Error("Invalid flags setting for class %s", PR_GetString(ent->v.classname));
			return;
		}

		temp_index = ent->v.modelindex;
		if (((int)ent->v.flags & FL_CLASS_DEPENDENT) && ent->v.model)
		{
			strcpy (NewName, PR_GetString(ent->v.model));
			NewName[strlen(NewName)-5] = client->playerclass + 48;
			temp_index = SV_ModelIndex (NewName);
		}

		if (ref_ent->modelindex != temp_index)
		{
			bits |= U_MODEL;
			set_ent->modelindex = temp_index;
		}

		if ( ref_ent->scale != ((int)(ent->v.scale * 100.0) & 255)
			|| ref_ent->abslight != ((int)(ent->v.abslight * 255.0) & 255) )
		{
			bits |= U_SCALE;
			set_ent->scale = ((int)(ent->v.scale * 100.0) & 255);
			set_ent->abslight = (int)(ent->v.abslight * 255.0) & 255;
		}

		/* Protocols 18-21 have no U_ALPHA -- Shanjaq's 21 stops at
		 * U_COLORMAP exactly like 19 does -- and a client that does not
		 * know to consume the alpha byte reads the next entity's bits out
		 * of the middle of this one.  Note the reference frame still
		 * carries alpha either way, so nothing has to be re-derived when
		 * a listen server is restarted on protocol 100. */
		if (sv_protocol >= PROTOCOL_HEXENWAIL_1)
		{
			byte newalpha;
			eval_t *val = GetEdictFieldValue(ent, "alpha");
			newalpha = val ? ENTALPHA_ENCODE(val->_float) : ENTALPHA_DEFAULT;
			if (ref_ent->alpha != newalpha)
			{
				bits |= U_ALPHA;
				set_ent->alpha = newalpha;
			}
		}

		if (ent->baseline.ClearCount[client_num] < CLEAR_LIMIT)
		{
			bits |= U_CLEAR_ENT;
			set_ent->flags |= ENT_CLEARED;
		}

		if (!bits && FoundInList)
		{
			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

		if (e >= 256)
			bits |= U_LONGENTITY;

		if (bits >= 256)
			bits |= U_MOREBITS;

		if (bits >= 65536)
			bits |= U_MOREBITS2;

		bytes = SV_EntityUpdateSize(bits);

		/* Only worth the two square roots when it can change the outcome:
		 * while measuring, or when measuring came back with a real cutoff. */
		prio = (measuring || netsort_cutoff < NETSORT_BINS)
			? SV_EntityPriority (ent, org, ent == clent) : 0;

		if (measuring)
		{
			/* Total the demand; do not truncate it.  The cutoff can only be
			 * computed from what the frame actually wants to send, so this
			 * pass deliberately runs past the point where the datagram is
			 * full. */
			binbytes[prio] += bytes;

			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

		/* Datagram size guard, and the netsort cutoff that decides who meets
		 * it first.  The buffer is a fixed NET_MAXMESSAGE stack buffer with
		 * allowoverflow false, so without the size test the first delta that
		 * does not fit takes the server down inside SZ_GetSpace.  Shed the
		 * entity instead -- and prefer to shed it for being unimportant,
		 * while there is still room to spare, over shedding whoever happens
		 * to arrive once the datagram is already full.
		 *
		 * This has to undo the build->states[] bookkeeping in the same breath
		 * as it skips the write: build->states[] is the client's next
		 * reference frame and must record exactly the set that went out.
		 * Recording a delta that was never written desynchronises the
		 * reference and reaches the player as "Illegible server message"
		 * (uhexen2-6ugh).  How it is undone depends on whether the client
		 * already knows the entity:
		 *
		 *   FoundInList  - it is in the client's reference and may later need
		 *                  an svc_clear_edicts removal, which only happens for
		 *                  entities still in the reference.  Keep it, holding
		 *                  its unmodified reference state, exactly as the
		 *                  IgnoreEnt path does.  Dropping it would strand it
		 *                  as a permanent ghost.
		 *   !FoundInList - the client has never heard of it.  Drop it, so the
		 *                  next frame offers it again as a new entity.  Keeping
		 *                  it would claim the client holds it at baseline, and
		 *                  a subsequent zero-delta frame would then skip it and
		 *                  leave it invisible until it next changed.
		 *
		 * trailer_reserve keeps room for the svc_clear_edicts trailer written
		 * after the loop: the worst case, or the exact size once a measuring
		 * pass has counted it. */
		if (prio >= netsort_cutoff ||
			cursize + bytes + trailer_reserve > msg->maxsize)
		{
			*ref_ent = saved_ref;
			NumShed++;
			if (prio >= netsort_cutoff)
				NumSorted++;
			/* The whole point of the sort, as one number: the best-priority
			 * entity this frame threw away.  Under sv_netsort it tracks the
			 * cutoff bin, since whole bins are admitted and the guard is left
			 * with nothing to do; under sv_netsort 0 it is whatever edict
			 * order happened to leave until last, which is routinely
			 * something in the player's face. */
			if (prio < ShedNearest)
				ShedNearest = prio;

			if (!FoundInList)
			{
				bcount--;
				continue;
			}

			*set_ent = saved_ref;
			/* No U_CLEAR_ENT went out, so this frame must not be counted
			 * as a delivered clear by the ClearCount bookkeeping above. */
			set_ent->flags &= ~ENT_CLEARED;

			if (bcount >= MAX_CLIENT_STATES)
				break;

			continue;
		}

	//
	// write the message
	//
		MSG_WriteByte (msg,bits | U_SIGNAL);

		if (bits & U_MOREBITS)
			MSG_WriteByte (msg, bits >> 8);
		if (bits & U_MOREBITS2)
			MSG_WriteByte (msg, bits >> 16);

		if (bits & U_LONGENTITY)
			MSG_WriteShort (msg, e);
		else
			MSG_WriteByte (msg, e);

		if (bits & U_MODEL)
			MSG_WriteShort (msg, temp_index);
		if (bits & U_FRAME)
			MSG_WriteByte (msg, ent->v.frame);
		if (bits & U_COLORMAP)
			MSG_WriteByte (msg, ent->v.colormap);
		if (bits & U_SKIN)
		{ // Used for skin and drawflags
			MSG_WriteByte(msg, ent->v.skin);
			MSG_WriteByte(msg, ent->v.drawflags);
		}
		if (bits & U_EFFECTS)
			MSG_WriteByte (msg, ent->v.effects);
		if (bits & U_ORIGIN1)
			MSG_WriteCoord (msg, ent->v.origin[0]);
		if (bits & U_ANGLE1)
			MSG_WriteAngle (msg, ent->v.angles[0]);
		if (bits & U_ORIGIN2)
			MSG_WriteCoord (msg, ent->v.origin[1]);
		if (bits & U_ANGLE2)
			MSG_WriteAngle (msg, ent->v.angles[1]);
		if (bits & U_ORIGIN3)
			MSG_WriteCoord (msg, ent->v.origin[2]);
		if (bits & U_ANGLE3)
			MSG_WriteAngle (msg, ent->v.angles[2]);
		if (bits & U_SCALE)
		{ // Used for scale and abslight
			MSG_WriteByte (msg, (int)(ent->v.scale * 100.0) & 255);
			MSG_WriteByte (msg, (int)(ent->v.abslight * 255.0) & 255);
		}
		if (bits & U_ALPHA)
			MSG_WriteByte (msg, set_ent->alpha);

		/* Resync rather than add: this keeps SV_EntityUpdateSize() a
		 * prediction the guard makes, not a second source of truth for how
		 * much of the datagram is gone. */
		cursize = msg->cursize;

		if (bcount >= MAX_CLIENT_STATES)
			break;
	}

	if (measuring)
	{
		int	budget, total;

		/* Nothing was written, so msg->cursize is still just the
		 * svc_reference header: everything after it, less the trailer, is
		 * what the entity updates have to fit into. */
		/* The measuring pass ran the same removal decisions the write
		 * pass will -- they read only entity state, the PVS and the
		 * client's reference frame, none of which measuring touches -- so
		 * the trailer's size is now known exactly rather than reserved for
		 * the worst case.  512 bytes was 12.5% of a 4096 datagram and 37%
		 * of an sv_maxdatagram 1400 one.  GitHub #82. */
		trailer_reserve = 2 + 2 * NumToRemove;
		budget = msg->maxsize - trailer_reserve - msg->cursize;

		/* netsort_cutoff ends as the number of whole bins that fit, so the
		 * write pass sheds everything from that bin outwards.  Reaching
		 * NETSORT_BINS means the frame fits after all and nothing is shed
		 * for priority, which is the same write pass as sv_netsort 0. */
		total = 0;
		for (netsort_cutoff = 0; netsort_cutoff < NETSORT_BINS; netsort_cutoff++)
		{
			if (total + binbytes[netsort_cutoff] > budget)
				break;
			total += binbytes[netsort_cutoff];
		}

		/* Admit the nearest bin even when it alone does not fit: shedding the
		 * whole frame would be worse than letting the guard truncate it. */
		if (netsort_cutoff == 0)
			netsort_cutoff = 1;

		measuring = false;
		goto restart;
	}

	build->count = bcount;

	/* The loop above caps NumToRemove precisely because this count goes out
	 * as a byte while the entries go out as shorts.  Clamp again here so the
	 * two writes can never disagree: a truncated count followed by the full
	 * run of shorts desynchronises every later message in the packet, which
	 * reaches the player as an "Illegible server message" host error rather
	 * than as anything diagnosable (uhexen2-6ugh). */
	if (NumToRemove > MAX_CLEAR_EDICTS_PER_MSG)
		NumToRemove = MAX_CLEAR_EDICTS_PER_MSG;

	/* The measured reserve is exact only if both passes made the same
	 * removals, which they do (see the measuring block).  Should that ever
	 * stop being true, an svc_clear_edicts that does not fit would overflow
	 * the message and drop the client; say so loudly instead and send the
	 * removals that fit.  The ones cut would linger on the client as
	 * ghosts, which is the lesser failure and one this message points at. */
	if (msg->cursize + 2 + 2 * NumToRemove > msg->maxsize)
	{
		Con_Printf ("%s: removal trailer (%d) exceeds measured reserve (%d)\n",
				__thisfunc__, 2 + 2 * NumToRemove, trailer_reserve);
		NumToRemove = (msg->maxsize - msg->cursize - 2) / 2;
		if (NumToRemove < 0)
			NumToRemove = 0;
	}

	if (NumDeferred)
		Con_DPrintf ("%s: remove list full, %d edict(s) deferred\n",
				__thisfunc__, NumDeferred);

	if (NumShed)
		Con_DPrintf ("%s: datagram full at %d/%d bytes, %d edict(s) shed "
				"(%d by netsort, cutoff bin %d), nearest shed bin %d\n",
				__thisfunc__, msg->cursize, msg->maxsize, NumShed,
				NumSorted, netsort_cutoff, ShedNearest);

	MSG_WriteByte (msg, svc_clear_edicts);
	MSG_WriteByte (msg, NumToRemove);
	for (i = 0; i < NumToRemove; i++)
		MSG_WriteShort (msg, RemoveList[i]);
}

/*
=============
SV_CleanupEnts

=============
*/
static void SV_CleanupEnts (void)
{
	int		e;
	edict_t	*ent;

	ent = NEXT_EDICT(sv.edicts);
	for (e = 1; e < sv.num_edicts; e++, ent = NEXT_EDICT(ent))
	{
		ent->v.effects = (int)ent->v.effects & ~EF_MUZZLEFLASH;
	}
}
