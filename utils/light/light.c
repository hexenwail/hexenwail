/* light.c
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

#include "q_stdinc.h"
#include "compiler.h"
#include "arch_def.h"
#include "cmdlib.h"
#include "q_endian.h"
#include "byteordr.h"
#include "pathutil.h"
#include "mathlib.h"
#include "bspfile.h"
#include "entities.h"
#include "threads.h"
#include "light.h"

qboolean	extrasamples;

float		scaledist	= 1.0F;
float		scalecos	= 0.5F;
float		rangescale	= 0.5F;

//dmodel_t	*bspmodel;
int		bspfileface;	// next surface to dispatch
vec3_t		bsp_origin;

byte		*filebase;
static byte	*file_p, *file_end;

float		minlights[MAX_MAP_FACES];


/* Space in dlightdata used to be handed out as each thread finished a
 * face, so with more than one thread the layout followed the schedule
 * (#530).  Each face now lights into a buffer of its own, and
 * PlaceFaceSpace lays the buffers out in face order once the threads are
 * done: the order, and the bytes, of a -threads 1 run. */
static byte	*facespace[MAX_MAP_FACES];
static int	facespacesize[MAX_MAP_FACES];

/* no lock: each surfnum belongs to the one thread that took it */
byte *GetFaceSpace (int surfnum, int size)
{
	facespace[surfnum] = (byte *) SafeMalloc (size);
	facespacesize[surfnum] = size;
	return facespace[surfnum];
}

static void PlaceFaceSpace (void)
{
	int	i;

	for (i = 0; i < numfaces; i++)
	{
		if (!facespace[i])
			continue;
		file_p = (byte *)(((intptr_t)file_p + 3) & ~3);
		if (file_p + facespacesize[i] > file_end)
			COM_Error ("%s: overrun", __thisfunc__);
		memcpy (file_p, facespace[i], facespacesize[i]);
		if (is_bsp2)
			dfaces2[i].lightofs = file_p - filebase;
		else
			dfaces[i].lightofs = file_p - filebase;
		file_p += facespacesize[i];
		free (facespace[i]);
		facespace[i] = NULL;
	}
}


void LightThread (void *junk)
{
	int			i;

	printf("Thread %d started\n", (int)(intptr_t)junk);
	while (1)
	{
		ThreadLock();
		i = bspfileface++;
		ThreadUnlock();
		if (i >= numfaces)
			return;

		if (is_bsp2)
			LightFace2 (i);
		else
			LightFace (i);
	}
}

/*
=============
LightWorld
=============
*/
static void LightWorld (void)
{
	filebase = file_p = dlightdata;
	file_end = filebase + MAX_MAP_LIGHTING;

	RunThreadsOn (LightThread);
	PlaceFaceSpace ();

	lightdatasize = file_p - filebase;

	printf ("lightdatasize: %i\n", lightdatasize);
}


/*
========
main

light modelfile
========
*/
int main (int argc, char **argv)
{
	int		i;
	int		wantthreads;
	double		start, end;
	char		source[1024];

	printf ("----- LightFaces ----\n");

	ValidateByteorder ();

	wantthreads = -1;		// default to auto-detect.

	for (i = 1 ; i < argc ; i++)
	{
		if (!strcmp(argv[i],"-threads"))
		{
			if (i >= argc - 1)
				COM_Error("Missing argument to \"%s\"", argv[i]);
			wantthreads = atoi (argv[++i]);
		}
		else if (!strcmp(argv[i],"-extra"))
		{
			extrasamples = true;
			printf ("extra sampling enabled\n");
		}
		else if (!strcmp(argv[i],"-dist"))
		{
			if (i >= argc - 1)
				COM_Error("Missing argument to \"%s\"", argv[i]);
			scaledist = (float)atof (argv[++i]);
		}
		else if (!strcmp(argv[i],"-range"))
		{
			if (i >= argc - 1)
				COM_Error("Missing argument to \"%s\"", argv[i]);
			rangescale = (float)atof (argv[++i]);
		}
		else if (argv[i][0] == '-')
			COM_Error ("Unknown option \"%s\"", argv[i]);
		else
			break;
	}

	if (i != argc - 1)
		COM_Error ("usage: light [-threads num] [-extra] [-dist ?] [-range ?] bspfile");

	InitThreads (wantthreads, 0);

	start = COM_GetTime ();

	strcpy (source, argv[i]);
	StripExtension (source);
	DefaultExtension (source, ".bsp", sizeof(source));

	LoadBSPFile (source);
	LoadEntities ();

	MakeTnodes (&dmodels[0]);

	LightWorld ();

	WriteEntitiesToString ();
	WriteBSPFile (source, is_bsp2);

	end = COM_GetTime ();
	printf ("%5.1f seconds elapsed\n", end-start);

	return 0;
}
