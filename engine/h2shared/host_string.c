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

const char *Host_GetString (int idx)
{
	return &host_strings[host_string_index[idx]];
}

