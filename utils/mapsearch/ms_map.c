/* ms_map.c -- a tolerant reader for Quake/Hexen II .map sources.
 * Copyright (C) 2026  uHexen2 developers
 *
 * Only what a search needs is extracted: the key/value pairs of every
 * entity, the line each pair sits on, and a position to jump to.  Brush
 * geometry is skipped, save for the one point the origin of a brush entity
 * is guessed from.  Nothing here refuses a file: a map that qbsp would
 * reject still yields every entity ahead of the damage.
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
#include "mapsearch.h"

#include <errno.h>

typedef struct
{
	const char	*p;
	const char	*end;
	int		line;
} mapreader_t;

static void ent_reset (mapentity_t *ent)
{
	int	i;

	for (i = 0; i < ent->numprops; i++)
	{
		free (ent->props[i].key);
		free (ent->props[i].value);
	}
	ent->numprops = 0;
	ent->line = 0;
	ent->classname = "";
	ent->targetname = "";
	ent->origin[0] = ent->origin[1] = ent->origin[2] = 0.0;
	ent->has_origin = false;
}

static void ent_free (mapentity_t *ent)
{
	ent_reset (ent);
	if (ent->props)
		free (ent->props);
	ent->props = NULL;
	ent->maxprops = 0;
}

static void ent_addprop (mapentity_t *ent, char *key, char *value, int line)
{
	if (ent->numprops == ent->maxprops)
	{
		int	newmax = ent->maxprops ? ent->maxprops * 2 : 16;
		mapprop_t *grown = (mapprop_t *) realloc (ent->props, newmax * sizeof(mapprop_t));
		if (!grown)
			COM_Error ("%s: out of memory", __thisfunc__);
		ent->props = grown;
		ent->maxprops = newmax;
	}
	ent->props[ent->numprops].key = key;
	ent->props[ent->numprops].value = value;
	ent->props[ent->numprops].line = line;
	ent->numprops++;
}

/* Skips whitespace and // comments, keeping the line counter honest.
 * Returns false at end of data. */
static qboolean skip_blank (mapreader_t *rd)
{
	while (rd->p < rd->end)
	{
		if (*rd->p == '\n')
		{
			rd->line++;
			rd->p++;
		}
		else if (*rd->p == ' ' || *rd->p == '\t' || *rd->p == '\r' ||
			 *rd->p == '\f' || *rd->p == '\v')
		{
			rd->p++;
		}
		else if (rd->p + 1 < rd->end && rd->p[0] == '/' && rd->p[1] == '/')
		{
			/* Editors park their metadata in these -- worldmix.map
			 * opens with a dozen //BSPGROUP lines. */
			while (rd->p < rd->end && *rd->p != '\n')
				rd->p++;
		}
		else
		{
			return true;
		}
	}
	return false;
}

/* Reads a "quoted string", the opening quote already in hand.  Returns NULL
 * if the closing quote never arrives. */
static char *read_quoted (mapreader_t *rd)
{
	const char	*start;
	char		*out;
	size_t		len;

	rd->p++;			/* the opening quote */
	start = rd->p;
	while (rd->p < rd->end && *rd->p != '"')
	{
		if (*rd->p == '\n')	/* unusual, but keep counting */
			rd->line++;
		rd->p++;
	}
	if (rd->p >= rd->end)
		return NULL;

	len = (size_t)(rd->p - start);
	out = (char *) SafeMalloc (len + 1);
	memcpy (out, start, len);
	out[len] = '\0';
	rd->p++;			/* the closing quote */
	return out;
}

/* Anything that is not a quoted string, a brace or a comment. */
static void skip_bare_token (mapreader_t *rd)
{
	while (rd->p < rd->end)
	{
		char	c = *rd->p;

		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
		    c == '\f' || c == '\v' || c == '"' || c == '{' || c == '}')
			break;
		if (c == '/' && rd->p + 1 < rd->end && rd->p[1] == '/')
			break;
		rd->p++;
	}
}

/*
 * The first point of the first plane of the first brush.  It is what the
 * original tool uses to place a brush entity, and it comes with the same
 * caveat: the point defines a plane, so it need not lie on the brush and can
 * land some way off.  Better than nothing, which is the alternative.
 */
static qboolean read_brush_point (mapreader_t *rd, double *out)
{
	int	i;

	rd->p++;			/* '(' */
	for (i = 0; i < 3; i++)
	{
		char	*stop;
		double	v;

		if (!skip_blank (rd))
			return false;
		v = strtod (rd->p, &stop);
		if (stop == rd->p)
			return false;
		rd->p = stop;
		out[i] = v;
	}
	return true;
}

static void finish_entity (mapentity_t *ent)
{
	int	i;

	for (i = 0; i < ent->numprops; i++)
	{
		const char	*k = ent->props[i].key;

		if (!q_strcasecmp (k, "classname"))
			ent->classname = ent->props[i].value;
		else if (!q_strcasecmp (k, "targetname"))
			ent->targetname = ent->props[i].value;
		else if (!q_strcasecmp (k, "origin"))
		{
			const char	*s = ent->props[i].value;
			int		j;

			for (j = 0; j < 3; j++)
			{
				char	*stop;
				double	v = strtod (s, &stop);

				if (stop == s)
					break;
				ent->origin[j] = v;
				s = stop;
			}
			if (j == 3)
				ent->has_origin = true;
		}
	}
}

qboolean MS_ScanMapFile (const char *filename, mapentity_cb callback,
			 void *userdata, char *errbuf, size_t errlen)
{
	FILE		*f;
	char		*buffer;
	long		length;
	size_t		got;
	mapreader_t	rd;
	mapentity_t	ent;
	int		depth = 0;
	qboolean	got_brush_point = false;
	qboolean	keep_going = true;

	if (errbuf && errlen)
		*errbuf = '\0';

	f = fopen (filename, "rb");
	if (!f)
	{
		if (errbuf)
			q_snprintf (errbuf, errlen, "cannot open: %s", strerror(errno));
		return false;
	}
	length = Q_filelength (f);
	if (length < 0)
	{
		fclose (f);
		if (errbuf)
			q_snprintf (errbuf, errlen, "cannot determine the file size");
		return false;
	}

	buffer = (char *) SafeMalloc ((size_t)length + 1);
	got = fread (buffer, 1, (size_t)length, f);
	fclose (f);
	buffer[got] = '\0';

	memset (&ent, 0, sizeof(ent));
	ent_reset (&ent);

	rd.p = buffer;
	rd.end = buffer + got;
	rd.line = 1;

	while (keep_going && skip_blank (&rd))
	{
		char	c = *rd.p;

		if (c == '{')
		{
			rd.p++;
			depth++;
			if (depth == 1)
			{
				ent_reset (&ent);
				ent.line = rd.line;
				got_brush_point = false;
			}
			continue;
		}

		if (c == '}')
		{
			rd.p++;
			depth--;
			if (depth == 0)
			{
				finish_entity (&ent);
				if (callback)
					keep_going = callback (&ent, userdata);
				ent_reset (&ent);
			}
			else if (depth < 0)
			{
				if (errbuf && !*errbuf)
					q_snprintf (errbuf, errlen,
						    "stray } on line %d", rd.line);
				depth = 0;
			}
			continue;
		}

		if (c == '"')
		{
			int	line = rd.line;
			char	*key, *value;

			key = read_quoted (&rd);
			if (!key)
			{
				if (errbuf && !*errbuf)
					q_snprintf (errbuf, errlen,
						    "unterminated string on line %d", line);
				break;
			}
			if (depth != 1)
			{	/* a quoted token inside a brush: not ours */
				free (key);
				continue;
			}
			if (!skip_blank (&rd) || *rd.p != '"')
			{
				if (errbuf && !*errbuf)
					q_snprintf (errbuf, errlen,
						    "key \"%s\" on line %d has no value",
						    key, line);
				free (key);
				continue;
			}
			value = read_quoted (&rd);
			if (!value)
			{
				if (errbuf && !*errbuf)
					q_snprintf (errbuf, errlen,
						    "unterminated string on line %d", rd.line);
				free (key);
				break;
			}
			ent_addprop (&ent, key, value, line);
			continue;
		}

		if (c == '(' && depth >= 2 && !got_brush_point)
		{
			if (read_brush_point (&rd, ent.origin))
				got_brush_point = true;
			else if (errbuf && !*errbuf)
				q_snprintf (errbuf, errlen,
					    "malformed brush plane on line %d", rd.line);
			continue;
		}

		{	/* brush geometry, texture names, numbers: not searchable */
			const char	*before = rd.p;

			skip_bare_token (&rd);
			if (rd.p == before)
				rd.p++;		/* never stall on a stray byte */
		}
	}

	if (depth > 0 && errbuf && !*errbuf)
		q_snprintf (errbuf, errlen, "unexpected end of file inside an entity");

	ent_free (&ent);
	free (buffer);
	return true;
}
