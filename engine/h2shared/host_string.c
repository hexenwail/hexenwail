/*
 * host_string.c --
 * internationalized string resource shared between client and server
 *
 * Copyright (C) 1997-1998 Raven Software Corp.
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

#include "quakedef.h"
#include "hw_strings_data.h"	/* generated: scripts/gen_hw_strings.py */

static char	*host_strings = NULL;
static int	*host_string_index = NULL;
int		host_string_count = 0;


void Host_ClearStrings (void)
{
	host_strings = NULL;
	host_string_index = NULL;
	host_string_count = 0;
}

/* Index the lines of a strings.txt already in memory, in place.  `data` must
 * live on the hunk for as long as the table is used (the index is allocated
 * there too).  caret_newlines turns '^' into '\n', which is how HexenWorld's
 * table marks the line break at the end of an indexed print.  Returns the
 * number of lines; on 0 the table is left empty. */
int Host_ParseStrings (char *data, qboolean caret_newlines)
{
	int		i, count, start;
	signed char	newline_char;

	Host_ClearStrings ();
	newline_char = -1;

	for (i = count = 0; data[i] != 0; i++)
	{
		if (data[i] == '\r' || data[i] == '\n')
		{
			if (newline_char == data[i] || newline_char == -1)
			{
				newline_char = data[i];
				count++;
			}
		}
	}

	if (!count)
		return 0;

	host_string_index = (int *)Hunk_AllocName ((count + 1)*sizeof(int), "string_index");

	for (i = count = start = 0; data[i] != 0; i++)
	{
		if (data[i] == '\r' || data[i] == '\n')
		{
			if (newline_char == data[i])
			{
				host_string_index[count] = start;
				start = i + 1;
				count++;
			}
			else
			{
				start++;
			}

			data[i] = 0;
		}
		else if (caret_newlines && data[i] == '^')
		{
			data[i] = '\n';
		}
	}

	host_strings = data;
	host_string_count = count;
	Con_DPrintf("Read in %d string lines\n", count);
	return count;
}

void Host_LoadStrings (void)
{
	char	*data;

	data = (char *)FS_LoadHunkFile ("strings.txt", NULL);
	if (!data)
		Host_Error ("%s: couldn't load strings.txt", __thisfunc__);
#if defined(H2W)
	if (!Host_ParseStrings (data, true))
#else
	if (!Host_ParseStrings (data, false))
#endif
		Host_Error ("%s: no string lines found", __thisfunc__);
}

/* HexenWorld's strings.txt, for the integrated client and hwsv alike.  Its
 * indices are not Hexen II's -- STR_SUICIDES is 468 and the obituaries run
 * to 592, while data1's table has 409 lines and portals' 562 -- and Siege's
 * differs again (lines 402-404, 432, and five more).  No pak ships one and a
 * retail install usually has none, so a plain FS lookup lands on data1's or
 * portals' table: wrong text on the client, and on the server PF_print_indexed
 * PR_RunErrors for every index past the end.  Tiers, most specific first:
 *   1. installed in the current gamedir (siege, hw, or a mod over hw)
 *   2. shipped beside the engine for that gamedir (PR_BundleDir; NULL in
 *      hwsv, which reads only the game directory)
 *   3. installed in a HexenWorld layer below it (hw under a mod)
 *   4. hw's, shipped beside the engine
 *   5. compiled into the binary (hw_strings_data.h), so a bare retail
 *      install needs nothing copied anywhere
 * Never data1's (path_id 1) or portals'.  Loaded on the hunk like
 * Host_LoadStrings.  Only a table with no lines can fail it now.
 * GitHub #214. */
qboolean Host_LoadHWStrings (void)
{
	char		path[MAX_OSPATH];
	const char	*bundle = PR_BundleDir ();
	unsigned int	path_id = 0;
	qboolean	hw_family;
	char		*data = NULL;

	hw_family = FS_FileExists ("strings.txt", &path_id) &&
		    path_id != 1U && path_id != FS_GetPortalsPathID ();

	if (hw_family && path_id == FS_GetGamedirPathID ())
		data = (char *)FS_LoadHunkFile ("strings.txt", NULL);
	if (!data && bundle)
	{
		q_snprintf (path, sizeof(path), "%s/%s/strings.txt", bundle,
				fs_gamedir_nopath);
		data = (char *)FS_LoadHunkFileFromOSPath (path);
	}
	if (!data && hw_family)
		data = (char *)FS_LoadHunkFile ("strings.txt", NULL);
	if (!data && bundle)
	{
		q_snprintf (path, sizeof(path), "%s/hw/strings.txt", bundle);
		data = (char *)FS_LoadHunkFileFromOSPath (path);
	}
	if (data && Host_ParseStrings (data, true))
		return true;

	/* 5. compiled in -- also when a file above was found but empty.  A
	 *    legacy install (retail data1 and hw/pak4.pak, nothing else) must
	 *    work with no manual install step, on the client and on hwsv, so
	 *    the tables ship inside the binary.  Copied to the hunk because
	 *    Host_ParseStrings rewrites in place. */
	{
		const char *src = q_strcasecmp (fs_gamedir_nopath, "siege") ?
					hw_strings_hw : hw_strings_siege;
		size_t len = strlen (src) + 1;

		data = (char *)Hunk_AllocName ((int)len, "hwstrings");
		memcpy (data, src, len);
	}

	if (!Host_ParseStrings (data, true))
	{
		Host_ClearStrings ();
		return false;
	}
	return true;
}

const char *Host_GetString (int idx)
{
	return &host_strings[host_string_index[idx]];
}

