/* sv_main_datagram_h2.c -- the C originals of #469 slice C, verbatim.
 * Compiled by run_datagram_harness.sh with the six ported functions
 * renamed to c_*, against the Rust archive.  The supporting
 * functions (SV_ClientInventoryPage, INV_UpdateExItem, SV_ModelIndex,
 * the signon-buffer trio) keep their names: both sides call the
 * same C.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

==================
SV_ClientInventoryPage

Find, or claim, the extended-inventory page belonging to a client.

sv.ex_inventory_pages lives in the hunk, and Host_ClearMemory both frees
the hunk and zeroes sv on every map load -- so the array is a fresh
allocation at a fresh address each map.  client_t does not follow it:
svs.clients is allocated once in Host_Init and survives.  Any caller that
leaves a previous map's pointer in client->ex_inventory therefore leaves a
dangling pointer into freed hunk, which INV_UpdateExItem then walks as a
linked list -- reading ->next out of whatever the new map has written
there.  That was uhexen2-rutp: a nondeterministic changelevel crash that
retrying made go away, because the reused bytes differ every time.

Returns NULL when no page is available.  Callers must assign the result
unconditionally; INV_UpdateExItem treats NULL as an empty chain.
==================
*/
ex_inventory_page_t *SV_ClientInventoryPage (int clientnum)
{
	int	npages, i;

	if (!sv.ex_inventory_pages)
		return NULL;

	/* The array is maxclients * MAX_INVENTORY_EX_PAGES entries.  Two of
	 * the three original search sites bounded by maxclients alone, so in
	 * single player only page 0 was ever reachable out of the 8. */
	npages = svs.maxclients * MAX_INVENTORY_EX_PAGES;

	for (i = 0; i < npages; i++)
	{
		if (sv.ex_inventory_pages[i].id != 0 &&
		    sv.ex_inventory_pages[i].client_id != clientnum)
			continue;		/* in use by someone else */

		if (sv.ex_inventory_pages[i].id == 0)
			sv.ex_inventory_pages[i].id = ++sv.next_page_id;
		return &sv.ex_inventory_pages[i];
	}

	return NULL;
}

int INV_UpdateExItem(ex_inventory_page_t *startPage, int inv_id, int inv_cnt, qboolean inc)
{
	qboolean bFound = false;
	int i, j, result;
	ex_inventory_page_t *page = startPage;

	result = 0;

	while (page != NULL)
	{
		// try to find a matching slot
		for (i = 0; i < MAX_INVENTORY_EX; i++)
		{
			if (page->item_id[i] == inv_id)
			{
				if (inc)
					page->item_cnt[i] += inv_cnt;
				else
					page->item_cnt[i] = inv_cnt;

				page->changed_items |= (1 << i);
				result = page->item_cnt[i];
				bFound = true;

				break;
			}
		}
		page = page->next;
	}

	if (inv_cnt > 0)
	{
		// no matching slot found, create one at first empty
		if (bFound == false)
		{
			page = startPage;

			while (page != NULL)
			{
				for (i = 0; i < MAX_INVENTORY_EX; i++)
				{
					if (page->item_id[i] == 0)
					{
						page->item_id[i] = inv_id;

						if (inc)
							page->item_cnt[i] += inv_cnt;
						else
							page->item_cnt[i] = inv_cnt;

						page->changed_items |= (1 << i);
						page->new_items |= (1 << i);
						result = page->item_cnt[i];
						bFound = true;

						break;
					}
				}

				if (!bFound)
				{
					if (page->next == NULL)
					{
						/* The old search compared client_id against i,
						 * the inventory SLOT index, not a client id.
						 * The owner is whoever owns the chain we are
						 * extending. */
						page->next = SV_ClientInventoryPage (startPage->client_id);
						if (page->next == NULL)
							break;	/* out of pages; without this the
								 * loop spun forever, since page
								 * never advanced and ->next was
								 * never filled in */
					}
					page = page->next;
				}
				else
					break;
			}
		}
	}


	return result;
}

==================
SV_ClearDatagram

==================
*/
void SV_ClearDatagram (void)
{
	SZ_Clear (&sv.datagram);
	sv_datagram_dropped = false;
}

==================
SV_WriteClientdataToMessage

==================
*/
void SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg)
{
	int	bits,sc1,sc2,sc3,sc4;
	byte	test;
	int	i;
	edict_t	*other;
	static	int next_update = 0;
	static	int next_count = 0;

//
// send a damage message
//
	if (ent->v.dmg_take || ent->v.dmg_save)
	{
		other = PROG_TO_EDICT(ent->v.dmg_inflictor);
		MSG_WriteByte (msg, svc_damage);
		MSG_WriteByte (msg, ent->v.dmg_save);
		MSG_WriteByte (msg, ent->v.dmg_take);
		for (i = 0; i < 3; i++)
			MSG_WriteCoord (msg, other->v.origin[i] + 0.5*(other->v.mins[i] + other->v.maxs[i]));

		ent->v.dmg_take = 0;
		ent->v.dmg_save = 0;
	}

//
// send the current viewpos offset from the view entity
//
	SV_SetIdealPitch ();	// how much to look up / down ideally

// a fixangle might get lost in a dropped packet.  Oh well.
	if (ent->v.fixangle)
	{
		MSG_WriteByte (msg, svc_setangle);
		for (i = 0; i < 3; i++)
			MSG_WriteAngle (msg, ent->v.angles[i] );
		ent->v.fixangle = 0;
	}

	bits = 0;

	if (client->send_all_v)
	{
		bits = SU_VIEWHEIGHT | SU_IDEALPITCH | SU_IDEALROLL | 
			SU_VELOCITY1 | (SU_VELOCITY1<<1) | (SU_VELOCITY1<<2) | 
			SU_PUNCH1 | (SU_PUNCH1<<1) | (SU_PUNCH1<<2) | SU_WEAPONFRAME | 
			SU_ARMOR | SU_WEAPON;
	}
	else
	{
		if (ent->v.view_ofs[2] != client->old_v.view_ofs[2])
			bits |= SU_VIEWHEIGHT;

		if (ent->v.idealpitch != client->old_v.idealpitch)
			bits |= SU_IDEALPITCH;

		if (ent->v.idealroll != client->old_v.idealroll)
			bits |= SU_IDEALROLL;

		for (i = 0; i < 3; i++)
		{
			if (ent->v.punchangle[i] != client->old_v.punchangle[i])
				bits |= (SU_PUNCH1<<i);
			if (ent->v.velocity[i] != client->old_v.velocity[i])
				bits |= (SU_VELOCITY1<<i);
		}

		if (ent->v.weaponframe != client->old_v.weaponframe)
			bits |= SU_WEAPONFRAME;

		if (ent->v.armorvalue != client->old_v.armorvalue)
			bits |= SU_ARMOR;

		if (ent->v.weaponmodel != client->old_v.weaponmodel)
			bits |= SU_WEAPON;
	}

// send the data

	//fjm: this wasn't in here b4, and the centerview command requires it.
	if ( (int)ent->v.flags & FL_ONGROUND)
		bits |= SU_ONGROUND;

	next_count++;
	if (next_count >= 3)
	{
		next_count = 0;
		next_update++;
		if (next_update > 11)
			next_update = 0;

		switch (next_update)
		{
			case 0:
				bits |= SU_VIEWHEIGHT;
				break;
			case 1:
				bits |= SU_IDEALPITCH;
				break;
			case 2:
				bits |= SU_IDEALROLL;
				break;
			case 3:
				bits |= SU_VELOCITY1;
				break;
			case 4:
				bits |= (SU_VELOCITY1<<1);
				break;
			case 5:
				bits |= (SU_VELOCITY1<<2);
				break;
			case 6:
				bits |= SU_PUNCH1;
				break;
			case 7:
				bits |= (SU_PUNCH1<<1);
				break;
			case 8:
				bits |= (SU_PUNCH1<<2);
				break;
			case 9:
				bits |= SU_WEAPONFRAME;
				break;
			case 10:
				bits |= SU_ARMOR;
				break;
			case 11:
				bits |= SU_WEAPON;
				break;
		}
	}

	MSG_WriteByte (msg, svc_clientdata);
	MSG_WriteShort (msg, bits);

	if (bits & SU_VIEWHEIGHT)
		MSG_WriteChar (msg, ent->v.view_ofs[2]);

	if (bits & SU_IDEALPITCH)
		MSG_WriteChar (msg, ent->v.idealpitch);

	if (bits & SU_IDEALROLL)
		MSG_WriteChar (msg, ent->v.idealroll);

	for (i = 0; i < 3; i++)
	{
		if (bits & (SU_PUNCH1<<i))
			MSG_WriteChar (msg, ent->v.punchangle[i]);
		if (bits & (SU_VELOCITY1<<i))
			MSG_WriteChar (msg, ent->v.velocity[i]/16);
	}

	if (bits & SU_WEAPONFRAME)
		MSG_WriteByte (msg, ent->v.weaponframe);
	if (bits & SU_ARMOR)
		MSG_WriteByte (msg, ent->v.armorvalue);
	if (bits & SU_WEAPON)
		MSG_WriteShort (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)));

	if (host_client->send_all_v)
	{
		sc1 = sc2 = 0xffffffff;
		host_client->send_all_v = false;
	}
	else
	{
		sc1 = sc2 = 0;
		if (sv_protocol >= PROTOCOL_UH2_114)
		{
			sc3 = host_client->ex_inventory->changed_items;
			sc4 = host_client->ex_inventory->new_items;
		}
		else
			sc3 = sc4 = 0;

		if (ent->v.health != host_client->old_v.health)
			sc1 |= SC1_HEALTH;
		if (ent->v.level != host_client->old_v.level)
			sc1 |= SC1_LEVEL;
		if (ent->v.intelligence != host_client->old_v.intelligence)
			sc1 |= SC1_INTELLIGENCE;
		if (ent->v.wisdom != host_client->old_v.wisdom)
			sc1 |= SC1_WISDOM;
		if (ent->v.strength != host_client->old_v.strength)
			sc1 |= SC1_STRENGTH;
		if (ent->v.dexterity != host_client->old_v.dexterity)
			sc1 |= SC1_DEXTERITY;
		if (ent->v.weapon != host_client->old_v.weapon)
			sc1 |= SC1_WEAPON;
		if (ent->v.bluemana != host_client->old_v.bluemana)
			sc1 |= SC1_BLUEMANA;
		if (ent->v.greenmana != host_client->old_v.greenmana)
			sc1 |= SC1_GREENMANA;
		if (ent->v.experience != host_client->old_v.experience)
			sc1 |= SC1_EXPERIENCE;
		if (ent->v.cnt_torch != host_client->old_v.cnt_torch)
			sc1 |= SC1_CNT_TORCH;
		if (ent->v.cnt_h_boost != host_client->old_v.cnt_h_boost)
			sc1 |= SC1_CNT_H_BOOST;
		if (ent->v.cnt_sh_boost != host_client->old_v.cnt_sh_boost)
			sc1 |= SC1_CNT_SH_BOOST;
		if (ent->v.cnt_mana_boost != host_client->old_v.cnt_mana_boost)
			sc1 |= SC1_CNT_MANA_BOOST;
		if (ent->v.cnt_teleport != host_client->old_v.cnt_teleport)
			sc1 |= SC1_CNT_TELEPORT;
		if (ent->v.cnt_tome != host_client->old_v.cnt_tome)
			sc1 |= SC1_CNT_TOME;
		if (ent->v.cnt_summon != host_client->old_v.cnt_summon)
			sc1 |= SC1_CNT_SUMMON;
		if (ent->v.cnt_invisibility != host_client->old_v.cnt_invisibility)
			sc1 |= SC1_CNT_INVISIBILITY;
		if (ent->v.cnt_glyph != host_client->old_v.cnt_glyph)
			sc1 |= SC1_CNT_GLYPH;
		if (ent->v.cnt_haste != host_client->old_v.cnt_haste)
			sc1 |= SC1_CNT_HASTE;
		if (ent->v.cnt_blast != host_client->old_v.cnt_blast)
			sc1 |= SC1_CNT_BLAST;
		if (ent->v.cnt_polymorph != host_client->old_v.cnt_polymorph)
			sc1 |= SC1_CNT_POLYMORPH;
		if (ent->v.cnt_flight != host_client->old_v.cnt_flight)
			sc1 |= SC1_CNT_FLIGHT;
		if (ent->v.cnt_cubeofforce != host_client->old_v.cnt_cubeofforce)
			sc1 |= SC1_CNT_CUBEOFFORCE;
		if (ent->v.cnt_invincibility != host_client->old_v.cnt_invincibility)
			sc1 |= SC1_CNT_INVINCIBILITY;
		if (ent->v.artifact_active != host_client->old_v.artifact_active)
			sc1 |= SC1_ARTIFACT_ACTIVE;
		if (ent->v.artifact_low != host_client->old_v.artifact_low)
			sc1 |= SC1_ARTIFACT_LOW;
		if (ent->v.movetype != host_client->old_v.movetype)
			sc1 |= SC1_MOVETYPE;
		if (ent->v.cameramode != host_client->old_v.cameramode)
			sc1 |= SC1_CAMERAMODE;
		if (ent->v.hasted != host_client->old_v.hasted)
			sc1 |= SC1_HASTED;
		if (ent->v.inventory != host_client->old_v.inventory)
			sc1 |= SC1_INVENTORY;
		if (ent->v.rings_active != host_client->old_v.rings_active)
			sc1 |= SC1_RINGS_ACTIVE;

		if (ent->v.rings_low != host_client->old_v.rings_low)
			sc2 |= SC2_RINGS_LOW;
		if (ent->v.armor_amulet != host_client->old_v.armor_amulet)
			sc2 |= SC2_AMULET;
		if (ent->v.armor_bracer != host_client->old_v.armor_bracer)
			sc2 |= SC2_BRACER;
		if (ent->v.armor_breastplate != host_client->old_v.armor_breastplate)
			sc2 |= SC2_BREASTPLATE;
		if (ent->v.armor_helmet != host_client->old_v.armor_helmet)
			sc2 |= SC2_HELMET;
		if (ent->v.ring_flight != host_client->old_v.ring_flight)
			sc2 |= SC2_FLIGHT_T;
		if (ent->v.ring_water != host_client->old_v.ring_water)
			sc2 |= SC2_WATER_T;
		if (ent->v.ring_turning != host_client->old_v.ring_turning)
			sc2 |= SC2_TURNING_T;
		if (ent->v.ring_regeneration != host_client->old_v.ring_regeneration)
			sc2 |= SC2_REGEN_T;
		if (ent->v.haste_time != host_client->old_v.haste_time)
			sc2 |= SC2_HASTE_T;
		if (ent->v.tome_time != host_client->old_v.tome_time)
			sc2 |= SC2_TOME_T;
		if (ent->v.puzzle_inv1 != host_client->old_v.puzzle_inv1)
			sc2 |= SC2_PUZZLE1;
		if (ent->v.puzzle_inv2 != host_client->old_v.puzzle_inv2)
			sc2 |= SC2_PUZZLE2;
		if (ent->v.puzzle_inv3 != host_client->old_v.puzzle_inv3)
			sc2 |= SC2_PUZZLE3;
		if (ent->v.puzzle_inv4 != host_client->old_v.puzzle_inv4)
			sc2 |= SC2_PUZZLE4;
		if (ent->v.puzzle_inv5 != host_client->old_v.puzzle_inv5)
			sc2 |= SC2_PUZZLE5;
		if (ent->v.puzzle_inv6 != host_client->old_v.puzzle_inv6)
			sc2 |= SC2_PUZZLE6;
		if (ent->v.puzzle_inv7 != host_client->old_v.puzzle_inv7)
			sc2 |= SC2_PUZZLE7;
		if (ent->v.puzzle_inv8 != host_client->old_v.puzzle_inv8)
			sc2 |= SC2_PUZZLE8;
		if (ent->v.max_health != host_client->old_v.max_health)
			sc2 |= SC2_MAXHEALTH;
		if (ent->v.max_mana != host_client->old_v.max_mana)
			sc2 |= SC2_MAXMANA;
		if (ent->v.flags != host_client->old_v.flags)
			sc2 |= SC2_FLAGS;

		// mission pack, objectives
		if (sv_protocol > PROTOCOL_RAVEN_111)
		{
			if (info_mask != client->info_mask)
				sc2 |= SC2_OBJ;
			if (info_mask2 != client->info_mask2)
				sc2 |= SC2_OBJ2;
		}
	}

	if (!sc1 && !sc2 && !sc3 && !sc4)
		goto end;

	MSG_WriteByte (&host_client->message, svc_update_inv);
	test = 0;
	if (sc1 & 0x000000ff)
		test |= 1;
	if (sc1 & 0x0000ff00)
		test |= 2;
	if (sc1 & 0x00ff0000)
		test |= 4;
	if (sc1 & 0xff000000)
		test |= 8;
	if (sc2 & 0x000000ff)
		test |= 16;
	if (sc2 & 0x0000ff00)
		test |= 32;
	if (sc2 & 0x00ff0000)
		test |= 64;
	if (sc2 & 0xff000000)
		test |= 128;

	MSG_WriteByte (&host_client->message, test);

	if (test & 1)
		MSG_WriteByte (&host_client->message, sc1 & 0xff);
	if (test & 2)
		MSG_WriteByte (&host_client->message, (sc1 >> 8) & 0xff);
	if (test & 4)
		MSG_WriteByte (&host_client->message, (sc1 >> 16) & 0xff);
	if (test & 8)
		MSG_WriteByte (&host_client->message, (sc1 >> 24) & 0xff);
	if (test & 16)
		MSG_WriteByte (&host_client->message, sc2 & 0xff);
	if (test & 32)
		MSG_WriteByte (&host_client->message, (sc2 >> 8) & 0xff);
	if (test & 64)
		MSG_WriteByte (&host_client->message, (sc2 >> 16) & 0xff);
	if (test & 128)
		MSG_WriteByte (&host_client->message, (sc2 >> 24) & 0xff);

	if (sc1 & SC1_HEALTH)
		MSG_WriteShort (&host_client->message, ent->v.health);
	if (sc1 & SC1_LEVEL)
		MSG_WriteByte (&host_client->message, ent->v.level);
	if (sc1 & SC1_INTELLIGENCE)
		MSG_WriteByte (&host_client->message, ent->v.intelligence);
	if (sc1 & SC1_WISDOM)
		MSG_WriteByte (&host_client->message, ent->v.wisdom);
	if (sc1 & SC1_STRENGTH)
		MSG_WriteByte (&host_client->message, ent->v.strength);
	if (sc1 & SC1_DEXTERITY)
		MSG_WriteByte (&host_client->message, ent->v.dexterity);
	if (sc1 & SC1_WEAPON)
		MSG_WriteByte (&host_client->message, ent->v.weapon);
	if (sc1 & SC1_BLUEMANA)
		MSG_WriteByte (&host_client->message, ent->v.bluemana);
	if (sc1 & SC1_GREENMANA)
		MSG_WriteByte (&host_client->message, ent->v.greenmana);
	if (sc1 & SC1_EXPERIENCE)
		MSG_WriteLong (&host_client->message, ent->v.experience);
	if (sc1 & SC1_CNT_TORCH)
		MSG_WriteByte(&host_client->message, ent->v.cnt_torch), INV_UpdateExItem(host_client->ex_inventory, 1, (int)ent->v.cnt_torch, false);
	if (sc1 & SC1_CNT_H_BOOST)
		MSG_WriteByte(&host_client->message, ent->v.cnt_h_boost), INV_UpdateExItem(host_client->ex_inventory, 2, (int)ent->v.cnt_h_boost, false);
	if (sc1 & SC1_CNT_SH_BOOST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_sh_boost), INV_UpdateExItem(host_client->ex_inventory, 3, (int)ent->v.cnt_sh_boost, false);
	if (sc1 & SC1_CNT_MANA_BOOST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_mana_boost), INV_UpdateExItem(host_client->ex_inventory, 4, (int)ent->v.cnt_mana_boost, false);
	if (sc1 & SC1_CNT_TELEPORT)
		MSG_WriteByte (&host_client->message, ent->v.cnt_teleport), INV_UpdateExItem(host_client->ex_inventory, 5, (int)ent->v.cnt_teleport, false);
	if (sc1 & SC1_CNT_TOME)
		MSG_WriteByte (&host_client->message, ent->v.cnt_tome), INV_UpdateExItem(host_client->ex_inventory, 6, (int)ent->v.cnt_tome, false);
	if (sc1 & SC1_CNT_SUMMON)
		MSG_WriteByte (&host_client->message, ent->v.cnt_summon), INV_UpdateExItem(host_client->ex_inventory, 7, (int)ent->v.cnt_summon, false);
	if (sc1 & SC1_CNT_INVISIBILITY)
		MSG_WriteByte (&host_client->message, ent->v.cnt_invisibility), INV_UpdateExItem(host_client->ex_inventory, 8, (int)ent->v.cnt_invisibility, false);
	if (sc1 & SC1_CNT_GLYPH)
		MSG_WriteByte (&host_client->message, ent->v.cnt_glyph), INV_UpdateExItem(host_client->ex_inventory, 9, (int)ent->v.cnt_glyph, false);
	if (sc1 & SC1_CNT_HASTE)
		MSG_WriteByte (&host_client->message, ent->v.cnt_haste), INV_UpdateExItem(host_client->ex_inventory, 10, (int)ent->v.cnt_haste, false);
	if (sc1 & SC1_CNT_BLAST)
		MSG_WriteByte (&host_client->message, ent->v.cnt_blast), INV_UpdateExItem(host_client->ex_inventory, 11, (int)ent->v.cnt_blast, false);
	if (sc1 & SC1_CNT_POLYMORPH)
		MSG_WriteByte (&host_client->message, ent->v.cnt_polymorph), INV_UpdateExItem(host_client->ex_inventory, 12, (int)ent->v.cnt_polymorph, false);
	if (sc1 & SC1_CNT_FLIGHT)
		MSG_WriteByte (&host_client->message, ent->v.cnt_flight), INV_UpdateExItem(host_client->ex_inventory, 13, (int)ent->v.cnt_flight, false);
	if (sc1 & SC1_CNT_CUBEOFFORCE)
		MSG_WriteByte (&host_client->message, ent->v.cnt_cubeofforce), INV_UpdateExItem(host_client->ex_inventory, 14, (int)ent->v.cnt_cubeofforce, false);
	if (sc1 & SC1_CNT_INVINCIBILITY)
		MSG_WriteByte (&host_client->message, ent->v.cnt_invincibility), INV_UpdateExItem(host_client->ex_inventory, 15, (int)ent->v.cnt_invincibility, false);
	if (sc1 & SC1_ARTIFACT_ACTIVE)
		MSG_WriteFloat (&host_client->message, ent->v.artifact_active);
	if (sc1 & SC1_ARTIFACT_LOW)
		MSG_WriteFloat (&host_client->message, ent->v.artifact_low);
	if (sc1 & SC1_MOVETYPE)
		MSG_WriteByte (&host_client->message, ent->v.movetype);
	if (sc1 & SC1_CAMERAMODE)
		MSG_WriteByte (&host_client->message, ent->v.cameramode);
	if (sc1 & SC1_HASTED)
		MSG_WriteFloat (&host_client->message, ent->v.hasted);
	if (sc1 & SC1_INVENTORY)
		MSG_WriteByte (&host_client->message, ent->v.inventory);
	if (sc1 & SC1_RINGS_ACTIVE)
		MSG_WriteFloat (&host_client->message, ent->v.rings_active);

	if (sc2 & SC2_RINGS_LOW)
		MSG_WriteFloat (&host_client->message, ent->v.rings_low);
	if (sc2 & SC2_AMULET)
		MSG_WriteByte (&host_client->message, ent->v.armor_amulet);
	if (sc2 & SC2_BRACER)
		MSG_WriteByte (&host_client->message, ent->v.armor_bracer);
	if (sc2 & SC2_BREASTPLATE)
		MSG_WriteByte (&host_client->message, ent->v.armor_breastplate);
	if (sc2 & SC2_HELMET)
		MSG_WriteByte (&host_client->message, ent->v.armor_helmet);
	if (sc2 & SC2_FLIGHT_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_flight);
	if (sc2 & SC2_WATER_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_water);
	if (sc2 & SC2_TURNING_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_turning);
	if (sc2 & SC2_REGEN_T)
		MSG_WriteByte (&host_client->message, ent->v.ring_regeneration);
	if (sc2 & SC2_HASTE_T)
		MSG_WriteFloat (&host_client->message, ent->v.haste_time);
	if (sc2 & SC2_TOME_T)
		MSG_WriteFloat (&host_client->message, ent->v.tome_time);
	if (sc2 & SC2_PUZZLE1)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv1));
	if (sc2 & SC2_PUZZLE2)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv2));
	if (sc2 & SC2_PUZZLE3)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv3));
	if (sc2 & SC2_PUZZLE4)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv4));
	if (sc2 & SC2_PUZZLE5)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv5));
	if (sc2 & SC2_PUZZLE6)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv6));
	if (sc2 & SC2_PUZZLE7)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv7));
	if (sc2 & SC2_PUZZLE8)
		MSG_WriteString (&host_client->message, PR_GetString(ent->v.puzzle_inv8));
	if (sc2 & SC2_MAXHEALTH)
		MSG_WriteShort (&host_client->message, ent->v.max_health);
	if (sc2 & SC2_MAXMANA)
		MSG_WriteByte (&host_client->message, ent->v.max_mana);
	if (sc2 & SC2_FLAGS)
		MSG_WriteFloat (&host_client->message, ent->v.flags);

// mission pack, objectives
	if (sv_protocol > PROTOCOL_RAVEN_111)
	{
		if (sc2 & SC2_OBJ)
		{
			MSG_WriteLong (&host_client->message, info_mask);
			client->info_mask = info_mask;
		}
		if (sc2 & SC2_OBJ2)
		{
			MSG_WriteLong (&host_client->message, info_mask2);
			client->info_mask2 = info_mask2;
		}
	}

// extended inventory
	if (sv_protocol >= PROTOCOL_UH2_114)
	{
		//shan page loop here
		ex_inventory_page_t *page = host_client->ex_inventory;

		while (page != NULL)
		{
			sc3 = page->changed_items;
			sc4 = page->new_items;

			test = 0;
			if (sc3 & 0x000000ff)
				test |= 1;
			if (sc3 & 0x0000ff00)
				test |= 2;
			if (sc3 & 0x00ff0000)
				test |= 4;
			if (sc3 & 0xff000000)
				test |= 8;
			if (page->next != NULL)
				test |= 16;

			MSG_WriteByte(&host_client->message, test);

			if (test)
			{
				if (test & 1)
					MSG_WriteByte(&host_client->message, sc3 & 0xff);
				if (test & 2)
					MSG_WriteByte(&host_client->message, (sc3 >> 8) & 0xff);
				if (test & 4)
					MSG_WriteByte(&host_client->message, (sc3 >> 16) & 0xff);
				if (test & 8)
					MSG_WriteByte(&host_client->message, (sc3 >> 24) & 0xff);

				for (i = 0; i < MAX_INVENTORY_EX; i++)
				{
					if (sc3 & (1 << i))
					{
						MSG_WriteByte(&host_client->message, (page->item_cnt[i] + (((sc4 & (1 << i)) != 0)) * 128));
						if (sc4 & (1 << i))
						{
							MSG_WriteByte(&host_client->message, (page->item_id[i]));
						}
					}
				}

				page->changed_items = 0;
				page->new_items = 0;
			}

			page = page->next;
		}

	}

	
end:
	memcpy (&client->old_v, &ent->v, sizeof(client->old_v));
}

=======================
SV_SendClientDatagram
=======================
*/
static qboolean SV_SendClientDatagram (client_t *client)
{
	byte		buf[NET_MAXMESSAGE];
	sizebuf_t	msg;
	int		wire, pending;

	/* Build into what this connection can actually carry, not into the size
	 * of the stack buffer.  Datagram_SendUnreliableMessage memcpys the result
	 * straight into a fixed MAX_DATAGRAM packet buffer and its only bounds
	 * check is compiled out of release builds, so the 32 KB that sizeof(buf)
	 * would authorise is not a generous limit -- it is memory corruption
	 * eight times over.  SV_PrepareClientEntities sheds entities to fit,
	 * worst-priority first, instead. */
	wire = NET_MaxUnreliableMessage (client->netconnection);

	/* ...and no larger than the negotiated protocol lets this client
	 * receive.  Skipped for a local connection: both ends are this process,
	 * so there is no other build's buffer to fit inside and single player
	 * keeps the full entity budget whatever sv_protocol says. */
	if (!NET_IsLocalConnection (client->netconnection))
	{
		int max = SV_MaxDatagram ();
		if (wire > max)
			wire = max;
	}

	/* ...and no larger than sv_maxdatagram, when set (GitHub #82). */
	if (sv_maxdatagram.integer > 0)
	{
		int cap = sv_maxdatagram.integer;
		if (cap < SV_MAXDATAGRAM_FLOOR)
			cap = SV_MAXDATAGRAM_FLOOR;
		if (wire > cap)
			wire = cap;
	}

	SZ_Init (&msg, buf, wire);
	msg.name = "client datagram";

	MSG_WriteByte (&msg, svc_time);
	MSG_WriteFloat (&msg, sv.time);

// add the client specific data to the datagram
	SV_WriteClientdataToMessage (client, client->edict, &msg);

	/* Hold room for the two datagrams appended below before handing the rest
	 * to the entity deltas.  Entity state is re-sent every frame and survives
	 * being shed; a temp entity or a sound is sent once and is simply gone if
	 * it does not fit.  Never squeeze the entity section below half, though:
	 * QC that keeps sv.datagram permanently full would otherwise freeze the
	 * visible world rather than lose an effect. */
	pending = sv.datagram.cursize + client->datagram.cursize;
	if (pending > wire / 2)
		pending = wire / 2;
	msg.maxsize = wire - pending;

	SV_PrepareClientEntities (client, client->edict, &msg);

	msg.maxsize = wire;

/*	if ((rand() & 0xff) < 200)
	{
		return true;
	}
*/

// copy the server datagram if there is space
	if (msg.cursize + sv.datagram.cursize < msg.maxsize)
		SZ_Write (&msg, sv.datagram.data, sv.datagram.cursize);

	if (msg.cursize + client->datagram.cursize < msg.maxsize)
		SZ_Write (&msg, client->datagram.data, client->datagram.cursize);

	SZ_Clear(&client->datagram);

/*	if (msg.cursize > 300)
	{
		Con_DPrintf("WARNING: packet size is %i\n",msg.cursize);
	}
*/

#ifndef SERVERONLY
	/* devstats: the size this client's datagram actually reached, next to
	 * the wire limit it was built against.  There is already a commented-out
	 * "packet size is %i" warning just above from whoever wanted this
	 * number; this is the version you can leave on.
	 *
	 * Client build only, and not an oversight: devstats_t lives in client.h,
	 * which the dedicated build does not compile, and a dedicated server has
	 * no overlay to put the number on.  uhexen2-a5nn.34 */
	dev_stats.packetsize = msg.cursize;
#endif

// send the datagram
	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
	{
		SV_DropClient (true);// if the message couldn't send, kick off
		return false;
	}

	return true;
}

=======================
SV_UpdateToReliableMessages
=======================
*/
static void SV_UpdateToReliableMessages (void)
{
	int		i, j;
	client_t	*client;
	edict_t		*ent;

// check for changes to be sent over the reliable streams
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		ent = host_client->edict;
		if (host_client->old_frags != ent->v.frags)
		{
			for (j = 0, client = svs.clients; j < svs.maxclients; j++, client++)
			{
				if (!client->active)
					continue;

				MSG_WriteByte (&client->message, svc_updatefrags);
				MSG_WriteByte (&client->message, i);
				MSG_WriteShort (&client->message, host_client->edict->v.frags);
			}

			host_client->old_frags = ent->v.frags;
		}
	}

	for (j = 0, client = svs.clients; j < svs.maxclients; j++, client++)
	{
		if (!client->active)
			continue;
		SZ_Write (&client->message, sv.reliable_datagram.data, sv.reliable_datagram.cursize);
	}

	SZ_Clear (&sv.reliable_datagram);
}

=======================
SV_SendNop

Send a nop message without trashing or sending the accumulated client
message buffer
=======================
*/
static void SV_SendNop (client_t *client)
{
	sizebuf_t	msg;
	byte		buf[4];

	SZ_Init (&msg, buf, sizeof(buf));

	MSG_WriteChar (&msg, svc_nop);

	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
		SV_DropClient (true);	// if the message couldn't send, kick off
	client->last_message = realtime;
}

=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i;

// update frags, names, etc
	SV_UpdateToReliableMessages ();

// build individual updates
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (!host_client->active)
			continue;

		if (host_client->spawned)
		{
			if (!SV_SendClientDatagram (host_client))
				continue;
		}
		else
		{
		// the player isn't totally in the game yet
		// send small keepalive messages if too much time has passed
		// send a full message when the next signon stage has been requested
		// some other message data (name changes, etc) may accumulate
		// between signon stages

			/* Feed out the signon list, one buffer per reliable
			 * message.  Only once the previous one has actually
			 * gone -- a signon buffer is half of client->message,
			 * so stacking two, or one on top of queued name/colour
			 * updates, would overflow it.  uhexen2-z5wt. */
			if (host_client->signon_buffer >= 0 && !host_client->message.cursize)
				SV_SendSignonBuffer (host_client);

			if (!host_client->sendsignon)
			{
				if (realtime - host_client->last_message > 5)
					SV_SendNop (host_client);
				continue;	// don't send out non-signon messages
			}
		}

		// check for an overflowed message.  Should only happen
		// on a very fucked up connection that backs up a lot, then
		// changes level
		if (host_client->message.overflowed)
		{
			SV_DropClient (true);
			host_client->message.overflowed = false;
			continue;
		}

		if (host_client->message.cursize || host_client->dropasap)
		{
			if (!NET_CanSendMessage (host_client->netconnection))
			{
			//	I_Printf ("can't write\n");
				continue;
			}

			if (host_client->dropasap)
				SV_DropClient (false);	// went to another level
			else if (!NET_IsLocalConnection (host_client->netconnection) &&
				 host_client->message.cursize > SV_MaxReliableMessage ())
			{
				/* Sending this would overrun the far end's
				 * receiveMessage[] rather than merely fail, so drop
				 * the client instead -- the same outcome the
				 * overflowed check above produces, but named, and
				 * before the damage rather than after.  QC writing
				 * this much reliable data in one frame is the usual
				 * cause; sv_protocol 100 raises the ceiling from
				 * 16 KB to 32 KB for clients that can take it. */
				Con_Printf ("%s: %d byte reliable message exceeds the "
					    "%d byte limit of protocol %i; dropping %s\n",
					    __thisfunc__, host_client->message.cursize,
					    SV_MaxReliableMessage (), sv_protocol,
					    host_client->name);
				SV_DropClient (true);
			}
			else
			{
				if (NET_SendMessage (host_client->netconnection,
						&host_client->message) == -1)
					SV_DropClient (true);	// if the message couldn't send, kick off
				SZ_Clear (&host_client->message);
				host_client->last_message = realtime;
				host_client->sendsignon = false;
			}
		}
	}

// clear muzzle flashes
	SV_CleanupEnts ();
}

================
SV_ModelIndex

================
*/
int SV_ModelIndex (const char *name)
{
	int		i;

	if (!name || !name[0])
		return 0;

	for (i = 0; i < MAX_MODELS && sv.model_precache[i]; i++)
	{
		if (!strcmp(sv.model_precache[i], name))
			return i;
	}

	if (i == MAX_MODELS || !sv.model_precache[i])
	{
		Con_Printf("%s: model %s not precached\n", __func__, name);
		return 0;
	}

	return i;
}

================
SV_SignonBufferCount
SV_SignonBufferSize

How many signon buffers there are and how long each one is.  The buffer being
filled is the last, and its length is sv.signon.cursize rather than an entry in
signon_size[] -- QC can still write to MSG_INIT after the map has spawned, so
sealing the length at spawn time would short a client that connected later.
================
*/
static int SV_SignonBufferCount (void)
{
	return sv.num_signon_buffers + 1;
}

static int SV_SignonBufferSize (int i)
{
	return (i < sv.num_signon_buffers) ? sv.signon_size[i] : sv.signon.cursize;
}

================
SV_SendSignonBuffer

Hand a client the next signon buffer it is owed, and the signon-stage marker
once the last one has gone.  One buffer per reliable message: they do not all
fit in client->message at once, and the client is happy to take baselines and
statics spread over several messages -- only svc_signonnum advances its state.

Called from Host_PreSpawn_f for the first buffer and from SV_SendClientMessages
for the rest.
================
*/
void SV_SendSignonBuffer (client_t *client)
{
	int	i = client->signon_buffer;

	if (i < 0 || i >= SV_SignonBufferCount())
	{
		client->signon_buffer = -1;
		return;
	}

	SZ_Write (&client->message, sv.signon_bufs[i], SV_SignonBufferSize(i));

	if (i + 1 < SV_SignonBufferCount())
	{
		client->signon_buffer = i + 1;
	}
	else
	{
		client->signon_buffer = -1;
		MSG_WriteByte (&client->message, svc_signonnum);
		MSG_WriteByte (&client->message, 2);
	}

	client->sendsignon = true;
}

