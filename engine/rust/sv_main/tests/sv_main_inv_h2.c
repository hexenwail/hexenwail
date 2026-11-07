/*
 * sv_main_inv_h2.c -- the C original of the server's extended inventory,
 * worldspawn sky and savegame helpers, moved verbatim out of
 * engine/hexen2/sv_main.c: SV_ParseWorldspawnSky, SV_ClientInventoryPage,
 * INV_UpdateExItem, SV_GetLevelname, SV_SaveSpawnparms, INV_WritePage,
 * SV_LoadInventory and INV_SavePages.  The differential harness beside this
 * file includes it; no engine target compiles it.  The engine's
 * implementation is engine/rust/src/sv_main_inv.rs (#469 slice E, unit E1).
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
 * sv, svs, sv_globals, host_client and the engine functions these call. */

/*
==================
SV_ParseWorldspawnSky

Pull the sky name out of the world's entity lump so the server has an
authoritative answer for svc_skybox instead of the literal "" it used to
send (uhexen2-6dgk).  The client reads the same key itself in Sky_NewMap,
so on a listen server this agrees with what is already on screen and
Sky_LoadSkyBox early-returns; it earns its keep for a remote client whose
copy of the map differs, or which has no BSP at all.

Accepts the same three spellings Sky_NewMap does, so the two cannot
disagree about which key wins.
==================
*/
static void SV_ParseWorldspawnSky (void)
{
	char		key[128];
	const char	*data;

	sv.skybox[0] = 0;

	if (!sv.worldmodel || !sv.worldmodel->entities)
		return;

	data = COM_Parse (sv.worldmodel->entities);
	if (!data || com_token[0] != '{')
		return;

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return;
		if (com_token[0] == '}')
			return;			/* end of worldspawn */

		q_strlcpy (key, (com_token[0] == '_') ? com_token + 1 : com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ')
			key[strlen(key)-1] = 0;

		data = COM_Parse (data);
		if (!data)
			return;

		if (!strcmp("sky", key) ||		/* quake		*/
		    !strcmp("skyname", key) ||		/* half-life		*/
		    !strcmp("qlsky", key))		/* quake lives		*/
		{
			q_strlcpy (sv.skybox, com_token, sizeof(sv.skybox));
		}
	}
}

/*
==================
SV_UpdateExInventory
==================
*/
/*
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


/*
================
SV_GetLevelname

Return the full levelname
================
*/
const char *SV_GetLevelname (void)
{
	int idx = (int)sv.edicts->v.message;
	if (idx > 0 && idx <= host_string_count)
		return Host_GetString(idx - 1);

/*	return "";*/
/* Use netname on map if there is one, so they don't have to edit strings.txt */
	return PR_GetString(sv.edicts->v.netname);
}


/*
================
SV_SaveSpawnparms

Grabs the current state of each client for saving across the
transition to another level
================
*/
void SV_SaveSpawnparms (void)
{
	int		i;
//	int		j;

	svs.serverflags = *sv_globals.serverflags;

	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (!host_client->active)
			continue;

	// call the progs to get default spawn parms for the new client
//		*sv_globals.self = EDICT_TO_PROG(host_client->edict);
//		PR_ExecuteProgram (*sv_globals.SetChangeParms);
//		for (j = 0; j < NUM_SPAWN_PARMS; j++)
//			host_client->spawn_parms[j] = sv_globals.parm[j];
	}
}

/*
=============
INV_Write

For savegames
=============
*/
void INV_WritePage(FILE *f, ex_inventory_page_t *page, int clientId)
{
	int i;

	//Page: id, next_id, client_id, {item_id, item_cnt} ...
	fprintf(f, "Page: %i %i %i", page->id, (page->next != NULL ? page->next->id : -1), clientId); //page_id, next_id, client_id
	for (i = 0; i < MAX_INVENTORY_EX; i++)
	{
		if ((page->item_id[i] != 0) && (page->item_cnt[i] > 0))
		{
			fprintf(f, " %i %i", page->item_id[i], page->item_cnt[i]); //item_id, item_cnt
		}
	}
	fprintf(f, "\n");
}

// All changes need to be in SV_SaveInventory(), SV_LoadInventory(), CL_ParseInventory()
void SV_LoadInventory(FILE *FH)
{
	int Total = 0;
	int id, clientId, nextId, count, c, i;
	int item_id, item_cnt, item_idx;

	fscanf(FH, "Pages: %d\n", &Total);
	if (Total < 0 || Total > MAX_CLIENTS)
		Host_Error("%s: bad numpages", __func__);

	//Page: id, next_id, client_id, {item_id, item_cnt} ...
	for (count = 0; count < Total; count++)
	{
		fscanf(FH, "Page: %d %d %d", &id, &nextId, &clientId);
		for (i = 0; ((i < svs.maxclients) && (sv.ex_inventory_pages[i].id != 0) && (sv.ex_inventory_pages[i].client_id != clientId)); i++);
		memset(&sv.ex_inventory_pages[i], 0, sizeof(ex_inventory_page_t));
		sv.ex_inventory_pages[i].id = id;
		sv.ex_inventory_pages[i].client_id = clientId;
		if (id > sv.next_page_id)
			sv.next_page_id = id;

		item_idx = 0;
		c = fgetc(FH);	/* read one char, see what it is: */
		while ((c != '\n') && (c != '\r'))
		{
			fscanf(FH, "%d %d", &item_id, &item_cnt);
			sv.ex_inventory_pages[i].item_id[item_idx] = item_id;
			sv.ex_inventory_pages[i].item_cnt[item_idx] = item_cnt;
			sv.ex_inventory_pages[i].changed_items |= (1 << item_idx);
			sv.ex_inventory_pages[i].new_items |= (1 << item_idx);

			item_idx++;
			c = fgetc(FH);	/* read one char, see what it is: */
		}
	}

	Total = Total;
}

void INV_SavePages(FILE *FH)
{
	int i, j;
	client_t	*host_client;

	j = 0;
	host_client = svs.clients;
	for (i = 0; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->ex_inventory != NULL)
			j++;
	}

	fprintf(FH, "Pages: %i\n", j);

	host_client = svs.clients;
	for (i = 0; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->ex_inventory != NULL)
			INV_WritePage(FH, host_client->ex_inventory, i);
	}
}
