/* es_config.c -- the entsearch configuration file.
 * Copyright (C) 2026  uHexen2 developers
 *
 * Inky's MapSearch keeps its settings in an XML file and this reads the same
 * shape, so a config written for it loads here unchanged.  It is not a
 * general XML parser and does not pretend to be; it walks elements and
 * accepts a datum written any of the four ways those configs use it:
 *
 *   <SearchIn>path</SearchIn>          text inside the section, one per line
 *   <SearchIn><Path>p</Path></...>     a leaf holding text; its name is ignored
 *   <FlagsProperties><spawnflags/>     a leaf whose NAME is the datum, with
 *                                      exclude="true" to park it
 *   <Output LogToFile="true" />        settings as attributes, not elements
 *
 * The last two are not hypothetical: they are how the sample config Inky
 * ships writes its flag properties and its output settings.
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
#include "entsearch.h"

#include <errno.h>

#define	CFG_MAXDEPTH	32
#define	CFG_MAXNAME	64

/*
 * The properties a value should also be read as a set of bits.  Everything
 * here is used with the (+) bitwise-or operator somewhere in the Hexen II
 * HexenC source, or is spawnflags, which every editor exposes as check
 * boxes.  Override the list from the config file for another game.
 */
static const char *const default_flagsprops[] =
{
	"spawnflags",
	"flags",
	"flags2",
	"effects",
	"items",
	"items2",
	"drawflags",
	"artifact_flags",
	NULL
};

void ES_StrListAdd (strlist_t *list, const char *str)
{
	if (list->count == list->max)
	{
		int	newmax = list->max ? list->max * 2 : 8;
		char	**grown = (char **) realloc (list->items, newmax * sizeof(char *));

		if (!grown)
			COM_Error ("%s: out of memory", __thisfunc__);
		list->items = grown;
		list->max = newmax;
	}
	list->items[list->count++] = SafeStrdup (str);
}

void ES_StrListFree (strlist_t *list)
{
	int	i;

	for (i = 0; i < list->count; i++)
		free (list->items[i]);
	if (list->items)
		free (list->items);
	list->items = NULL;
	list->count = list->max = 0;
}

void ES_ConfigDefaults (essettings_t *set)
{
	int	i;

	memset (set, 0, sizeof(*set));
	ES_StrListAdd (&set->searchin, ".");
	for (i = 0; default_flagsprops[i]; i++)
		ES_StrListAdd (&set->flagsprops, default_flagsprops[i]);

	set->opt_ignorecase = true;
	q_strlcpy (set->logfile, "entsearch.log", sizeof(set->logfile));
}

static void trim (char *s)
{
	char	*start = s;
	size_t	len;

	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;
	if (start != s)
		memmove (s, start, strlen(start) + 1);

	len = strlen (s);
	while (len > 0)
	{
		char	c = s[len - 1];

		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		s[--len] = '\0';
	}
}

/* The five predefined entities, in place.  Numeric references are left be:
 * nothing that belongs in a path or a property name needs them. */
static void decode_entities (char *s)
{
	char	*in = s, *out = s;

	while (*in)
	{
		if (*in == '&')
		{
			if (!strncmp (in, "&amp;", 5))		{ *out++ = '&';  in += 5; continue; }
			if (!strncmp (in, "&lt;", 4))		{ *out++ = '<';  in += 4; continue; }
			if (!strncmp (in, "&gt;", 4))		{ *out++ = '>';  in += 4; continue; }
			if (!strncmp (in, "&quot;", 6))		{ *out++ = '"';  in += 6; continue; }
			if (!strncmp (in, "&apos;", 6))		{ *out++ = '\''; in += 6; continue; }
		}
		*out++ = *in++;
	}
	*out = '\0';
}

static qboolean parse_bool (const char *s, qboolean *out)
{
	if (!q_strcasecmp (s, "true") || !q_strcasecmp (s, "yes") ||
	    !q_strcasecmp (s, "on") || !strcmp (s, "1"))
	{
		*out = true;
		return true;
	}
	if (!q_strcasecmp (s, "false") || !q_strcasecmp (s, "no") ||
	    !q_strcasecmp (s, "off") || !strcmp (s, "0"))
	{
		*out = false;
		return true;
	}
	return false;
}

/* Which of the three list sections is this element, if any? */
static strlist_t *section_list (const char *name, essettings_t *set)
{
	if (!q_strcasecmp (name, "SearchIn") || !q_strcasecmp (name, "SearchIns"))
		return &set->searchin;
	if (!q_strcasecmp (name, "Exclude") || !q_strcasecmp (name, "Excludes"))
		return &set->exclude;
	if (!q_strcasecmp (name, "FlagsProperties") || !q_strcasecmp (name, "FlagsProperty"))
		return &set->flagsprops;
	return NULL;
}

/* An element that is not inside a list section names an output option. */
static qboolean apply_option (const char *name, const char *value, essettings_t *set)
{
	if (!q_strcasecmp (name, "Log") || !q_strcasecmp (name, "WriteLog") ||
	    !q_strcasecmp (name, "LogToFile"))
		return parse_bool (value, &set->opt_log);
	if (!q_strcasecmp (name, "SortByLine") || !q_strcasecmp (name, "SortByLineNumber"))
		return parse_bool (value, &set->opt_sortbyline);
	if (!q_strcasecmp (name, "Pts") || !q_strcasecmp (name, "WritePts") ||
	    !q_strcasecmp (name, "PtsFiles") || !q_strcasecmp (name, "MakePts"))
		return parse_bool (value, &set->opt_pts);
	if (!q_strcasecmp (name, "IgnoreCase") || !q_strcasecmp (name, "CaseInsensitive"))
		return parse_bool (value, &set->opt_ignorecase);
	if (!q_strcasecmp (name, "Developer"))
		return parse_bool (value, &set->opt_developer);
	if (!q_strcasecmp (name, "LogFile") || !q_strcasecmp (name, "LogFileName"))
	{
		q_strlcpy (set->logfile, value, sizeof(set->logfile));
		return true;
	}
	if (!q_strcasecmp (name, "PtsOutputDirectory") || !q_strcasecmp (name, "PtsDirectory"))
	{
		q_strlcpy (set->ptsdir, value, sizeof(set->ptsdir));
		return true;
	}
	return false;
}

/* Walks the name="value" pairs in the attribute span of a tag.  Returns the
 * position after the pair, or NULL when there are none left.  Bare and
 * single-quoted attributes are both tolerated. */
static const char *next_attr (const char *p, const char *end,
			      char *name, size_t namesize,
			      char *value, size_t valuesize)
{
	size_t	n;
	char	quote = 0;

	*name = *value = '\0';
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (p >= end || *p == '/' || *p == '>')
		return NULL;

	n = 0;
	while (p < end && *p != '=' && *p != ' ' && *p != '\t' &&
	       *p != '/' && *p != '>')
	{
		if (n + 1 < namesize)
			name[n++] = *p;
		p++;
	}
	name[n] = '\0';
	if (!n)
		return NULL;		/* no progress; stop rather than spin */

	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p >= end || *p != '=')
		return p;		/* valueless attribute */
	p++;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p < end && (*p == '"' || *p == '\''))
		quote = *p++;

	n = 0;
	while (p < end && (quote ? (*p != quote)
				 : (*p != ' ' && *p != '\t' && *p != '/' && *p != '>')))
	{
		if (n + 1 < valuesize)
			value[n++] = *p;
		p++;
	}
	value[n] = '\0';
	if (quote && p < end && *p == quote)
		p++;
	return p;
}

static int list_index (const strlist_t *list, const essettings_t *set)
{
	if (list == &set->searchin)
		return 0;
	if (list == &set->exclude)
		return 1;
	return 2;
}

/* A section named in the config replaces the built-in list rather than
 * extending it, so the first entry of each clears it. */
static void list_add (strlist_t *list, essettings_t *set, const char *entry,
		      qboolean *cleared)
{
	int	which = list_index (list, set);

	if (!cleared[which])
	{
		ES_StrListFree (list);
		cleared[which] = true;
	}
	ES_StrListAdd (list, entry);
}

/* Text inside a list section is one entry per line: that is how the config
 * for Inky's MapSearch writes its search and exclude paths, and it is the
 * obvious reading of a section written across several lines whatever wrote
 * it.  Taking the whole block as a single entry would silently produce one
 * nonsense path instead of three real ones. */
static void list_add_lines (strlist_t *list, essettings_t *set, char *text,
			    qboolean *cleared)
{
	char	*line = text;

	while (line && *line)
	{
		char	*nl = strchr (line, '\n');

		if (nl)
			*nl = '\0';
		trim (line);
		decode_entities (line);
		if (*line)
			list_add (list, set, line, cleared);
		line = nl ? nl + 1 : NULL;
	}
}

qboolean ES_ConfigLoad (const char *filename, essettings_t *set,
			char *errbuf, size_t errlen)
{
	FILE		*f;
	char		*buffer, *p;
	long		length;
	size_t		got;
	char		stack[CFG_MAXDEPTH][CFG_MAXNAME];
	int		depth = 0;
	char		*text = NULL;
	size_t		textlen = 0, textmax = 0;
	qboolean	cleared_defaults[3];

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

	/* A section that appears in the config replaces the built-in list
	 * rather than adding to it -- otherwise there would be no way to drop
	 * spawnflags, which the original documents as a real technique for
	 * forcing an exact-value search. */
	memset (cleared_defaults, 0, sizeof(cleared_defaults));

	textmax = 256;
	text = (char *) SafeMalloc (textmax);
	textlen = 0;
	text[0] = '\0';

	p = buffer;
	while (*p)
	{
		if (*p != '<')
		{	/* character data */
			if (textlen + 2 > textmax)
			{
				char *grown;
				textmax *= 2;
				grown = (char *) realloc (text, textmax);
				if (!grown)
					COM_Error ("%s: out of memory", __thisfunc__);
				text = grown;
			}
			text[textlen++] = *p++;
			text[textlen] = '\0';
			continue;
		}

		/* comments, declarations and doctypes carry nothing we want */
		if (!strncmp (p, "<!--", 4))
		{
			char *close = strstr (p + 4, "-->");
			if (!close)
				break;
			p = close + 3;
			continue;
		}
		if (p[1] == '?' || p[1] == '!')
		{
			char *close = strchr (p, '>');
			if (!close)
				break;
			p = close + 1;
			continue;
		}

		if (p[1] == '/')
		{	/* closing tag */
			char		name[CFG_MAXNAME];
			char		*close = strchr (p, '>');
			size_t		n = 0;
			strlist_t	*list;

			if (!close)
				break;
			p += 2;
			while (p < close && *p != ' ' && *p != '\t' && n + 1 < sizeof(name))
				name[n++] = *p++;
			name[n] = '\0';
			p = close + 1;

			/* text sitting straight inside a section element, or
			 * inside a leaf whose parent is one */
			list = section_list (name, set);
			if (!list && depth >= 2)
				list = section_list (stack[depth - 2], set);

			if (list)
			{
				list_add_lines (list, set, text, cleared_defaults);
			}
			else
			{
				trim (text);
				decode_entities (text);
				if (*text && !apply_option (name, text, set))
				{
					if (errbuf && !*errbuf)
						q_snprintf (errbuf, errlen,
							    "ignoring <%s> (unknown setting, or a value it cannot take)",
							    name);
				}
			}

			textlen = 0;
			text[0] = '\0';
			if (depth > 0)
				depth--;
			continue;
		}

		{	/* opening tag */
			char	name[CFG_MAXNAME];
			char	*close = strchr (p, '>');
			size_t	n = 0;
			qboolean selfclosing;

			const char	*ap;
			char		aname[CFG_MAXNAME], aval[512];
			qboolean	attr_exclude = false;
			strlist_t	*leaflist;

			if (!close)
				break;
			p++;
			while (p < close && *p != ' ' && *p != '\t' &&
			       *p != '/' && n + 1 < sizeof(name))
				name[n++] = *p++;
			name[n] = '\0';
			selfclosing = (close > p && close[-1] == '/');

			/* Attributes carry settings in the config Inky's tool
			 * ships: <Output LogToFile="true" MakePts="false" />,
			 * and exclude="true" to park a flag property without
			 * deleting the line. */
			ap = p;
			while ((ap = next_attr (ap, close, aname, sizeof(aname),
						aval, sizeof(aval))) != NULL)
			{
				if (!q_strcasecmp (aname, "exclude"))
					parse_bool (aval, &attr_exclude);
				else
					apply_option (aname, aval, set);
			}

			/* <spawnflags/> inside <FlagsProperties>: the element
			 * name is the datum.  Without this such a section
			 * clears the built-in list and adds nothing, and bit
			 * matching quietly stops working. */
			if (selfclosing && depth >= 1)
			{
				leaflist = section_list (stack[depth - 1], set);
				if (leaflist && !attr_exclude)
					list_add (leaflist, set, name, cleared_defaults);
			}

			p = close + 1;

			textlen = 0;
			text[0] = '\0';
			if (!selfclosing)
			{
				if (depth < CFG_MAXDEPTH)
					q_strlcpy (stack[depth], name, CFG_MAXNAME);
				depth++;
				if (depth > CFG_MAXDEPTH)
				{
					if (errbuf && !*errbuf)
						q_snprintf (errbuf, errlen,
							    "nesting is too deep, giving up at <%s>", name);
					break;
				}
			}
		}
	}

	free (text);
	free (buffer);

	/* An empty <SearchIn> section would otherwise leave nothing to walk. */
	if (set->searchin.count == 0)
		ES_StrListAdd (&set->searchin, ".");

	/* Worth saying out loud: with no flag properties left, an integer
	 * search silently stops matching bits and only matches exactly. */
	if (set->flagsprops.count == 0 && errbuf && !*errbuf)
		q_snprintf (errbuf, errlen, "no flag properties are left; "
			    "an integer value will only match exactly");

	return true;
}
