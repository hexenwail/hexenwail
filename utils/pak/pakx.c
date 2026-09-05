/* pakx.c -- pack file extraction tool.
 * Copyright (C) 1996-2001 Id Software, Inc.
 * Copyright (C) 2010 Ozkan Sezer <sezero@users.sourceforge.net>
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

#include "q_stdinc.h"
#include "compiler.h"
#include "arch_def.h"
#include "cmdlib.h"
#include "util_io.h"
#include "byteordr.h"
#include "pathutil.h"
#include "pakfile.h"
#include "pak.h"
#include "filenames.h"

//======================================================================

static int ExtractFile (pack_t *pak, const char *filename, const char *destdir)
{
	char   dest[MAX_OSPATH], *dptr;
	size_t filenamelen = 0;
	int    fileisdir = 0, i;

	if (filename)
	{
		filenamelen = strlen(filename);
		if (filenamelen)
			fileisdir = filename[filenamelen - 1] == '/';
	}

	if (!destdir || !*destdir)
		dptr = dest;

	else
	{
		strcpy (dest, destdir);
		dptr = strchr(dest, '\0');
		if (!IS_DIR_SEPARATOR(*(dptr - 1)))
			*dptr++ = DIR_SEPARATOR_CHAR;
	}

	*dptr = '\0';

	for (i = 0; i < pak->numfiles; i++)
	{
		if (!filename || (!strncmp (pak->files[i].name, filename, filenamelen) &&
			(fileisdir || pak->files[i].name[filenamelen] == '\0')))
		{
			fseek (pak->handle, pak->files[i].filepos, SEEK_SET);
			strcpy (dptr, pak->files[i].name);
			dest[sizeof(dest) - 1] = '\0';
			printf ("%s --> %s\n", pak->files[i].name, dest);
			if (Q_WriteFileFromHandle(pak->handle, dest, pak->files[i].filelen) != 0)
				COM_Error ("I/O errors during copy.");

			if (filename && !fileisdir)
				break;
		}
	}
	if (filename && *dptr == '\0')
	{
		fprintf (stderr, "** %s not in %s\n", filename, pak->filename);
		return 1;
	}
	return 0;
}

FUNC_NORETURN static void usage (int ret) {
	printf ("Usage:  pakx [-outdir <destdir>] <pakfile> [file [file ....]]\n");
	printf ("        pakx  -h  to display this help message.\n");
	printf ("<destdir> :  Optional. Output directory to extract the files into.\n");
	printf ("Without the [file] arguments, all pak file contents get extracted.\n");
	printf ("\n");
	exit (ret);
}

int main (int argc, char **argv)
{
	pack_t 	*pak;
	const char	*destdir;
	int	i, res = 0;

	if (argc < 2)
		usage (1);

	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-h"))
			usage (0);
	}

	if (!strcmp(argv[1], "-outdir"))
	{
		if (argc < 4)
			usage (1);
		i = 3;
		destdir = argv[2];
	}
	else
	{
		i = 1;
		destdir = NULL;
	}

	ValidateByteorder ();

	pak = LoadPackFile (argv[i]);
	if (!pak)
		COM_Error ("Unable to open file %s", argv[i]);
	printf ("Opened %s (%i files)\n", pak->filename, pak->numfiles);
	if (!pak->numfiles)
		COM_Error ("%s has no files.", pak->filename);
	if (++i >= argc)
		res |= ExtractFile (pak, NULL, destdir);
	else
	{
		for ( ; i < argc; i++)
			res |= ExtractFile (pak, argv[i], destdir);
	}

	return res;
}
