/*
 * Fast hash table for indexes and arrays
 * from the Doom 3 GPL Source Code source adapted for uhexen2.
 *
 * Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "quakedef.h"
#include "hashindex.h"

#define NULL_INDEX (-1)

static inline qboolean Hash_IsPowerOfTwo(int x)
{
	return (x > 0) && !(x & (x - 1));
}

/*
================
Hash_Allocate
================
*/
void Hash_Allocate(hashindex_t *hi, int hashSize)
{
	if (!Hash_IsPowerOfTwo(hashSize))
		Sys_Error("%s: has size %d is not power of two", __thisfunc__, hashSize);

	if (hi->hash != NULL)
		Sys_Error("%s: hash is already initialized", __thisfunc__);

	/* malloc, NOT Z_Malloc.  uhexen2-mm4l, and the same lesson as uhexen2-oq51:
	 * the zone is a small FIXED pool -- 2 MB on the client, 1 MB on h2ded --
	 * and Z_Malloc's failure is a Sys_Error, not a NULL return.  hashSize is
	 * content-scaled at two of this function's call sites: FS_LoadPackFile and
	 * FS_LoadZipFile size it to the archive's entry count, so a pk3 at the
	 * MAX_FILES_IN_ZIP ceiling asks for 2 * 65536 * 4 = 512 KB of a 2 MB pool
	 * for its hash alone.  Nothing here needs zone semantics; the arrays live
	 * exactly as long as the hashindex_t and Hash_Free is their only release.
	 * The two memsets below are why calloc buys nothing. */
	hi->hashSize = hashSize;
	hi->hash = (int *) malloc(sizeof(int) * hi->hashSize);
	hi->indexChain = (int *) malloc(sizeof(int) * hi->hashSize);
	if (!hi->hash || !hi->indexChain)
		Sys_Error("%s: out of memory for %d entries", __thisfunc__, hashSize);
	memset(hi->hash, NULL_INDEX, hi->hashSize * sizeof(hi->hash[0]));
	memset(hi->indexChain, NULL_INDEX, hi->hashSize * sizeof(hi->indexChain[0]));
	hi->hashMask = hashSize - 1;
}

/*
================
Hash_Free

free allocated memory
================
*/
void Hash_Free(hashindex_t *hi)
{
	if (hi->hash != NULL) {
		free(hi->hash);
		hi->hash = NULL;
	}
	if (hi->indexChain != NULL) {
		free(hi->indexChain);
		hi->indexChain = NULL;
	}
}

/*
================
Hash_Add

add an index to the hash, assumes the index has not yet been added to the hash
================
*/
void Hash_Add(hashindex_t *hi, int key, int index)
{
	int h;

	if (hi->hash == NULL)
		Sys_Error("%s: hash not initialized", __thisfunc__);

	if (index < 0 || index >= hi->hashSize)
		Sys_Error("%s: hash index out of range %d", __thisfunc__, index);

	h = key & hi->hashMask;
	hi->indexChain[index] = hi->hash[h];
	hi->hash[h] = index;
}

/*
================
Hash_Remove

remove an index from the hash
================
*/
void Hash_Remove(hashindex_t *hi, int key, int index)
{
	int i;
	int k = key & hi->hashMask;

	if (hi->hash == NULL)
		Sys_Error("%s: hash not initialized", __thisfunc__);

	if (hi->hash[k] == index) {
		hi->hash[k] = hi->indexChain[index];
	} else {
		for (i = hi->hash[k]; i != NULL_INDEX; i = hi->indexChain[i]) {
			if (hi->indexChain[i] == index) {
				hi->indexChain[i] = hi->indexChain[index];
				break;
			}
		}
	}
	hi->indexChain[index] = NULL_INDEX;
}

/*
================
Hash_Clear

clear the hash
================
*/
void Hash_Clear(hashindex_t *hi)
{
	if (hi->hash == NULL)
		return;

	// only clear the hash table because clearing the indexChain is not really needed
	memset(hi->hash, NULL_INDEX, hi->hashSize * sizeof(hi->hash[0]));
}
