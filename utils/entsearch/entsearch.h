/* entsearch.h -- shared declarations for the entsearch utility.
 * Copyright (C) 2026  uHexen2 developers
 *
 * Searches Quake/Hexen II .map sources for entities by classname, property
 * name and property value.  The design follows Inky's MapSearch, which is
 * the reference implementation:
 *   http://earthday.free.fr/Inkys-Hexen-II-Mapping-Corner/
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

#ifndef __ENTSEARCH_H
#define __ENTSEARCH_H

#define	ENTSEARCH_VERSION	"1.0"

/*
 * ==========================================================================
 * Regular expressions (es_regex.c)
 *
 * A small self-contained backtracking engine.  It exists because there is no
 * portable system regex: POSIX <regex.h> is absent from the mingw CRTs this
 * toolchain cross-compiles against, and the original tool's .NET flavour is
 * not something we can lean on either.
 *
 * Supported: literals, '.', character classes with ranges and negation,
 * the escapes \d \D \w \W \s \S \t \n \r plus any punctuation escape,
 * alternation, groups (capturing syntax is accepted but nothing is
 * captured), the quantifiers * + ? {n} {n,} {n,m} and their lazy '?'
 * variants, and the ^ $ anchors.
 *
 * Matching is whole-string: the pattern must consume the entire subject.
 * That is what the original does -- it is why its documented examples write
 * "func_train.*" to also reach func_train_mp instead of relying on a
 * substring search.
 * ==========================================================================
 */

typedef struct esregex_s	esregex_t;

esregex_t	*ES_RegexCompile (const char *pattern, qboolean ignorecase,
				  char *errbuf, size_t errlen);
qboolean	ES_RegexMatch (esregex_t *re, const char *text);
void		ES_RegexFree (esregex_t *re);

/*
 * ==========================================================================
 * .map parsing (es_map.c)
 * ==========================================================================
 */

typedef struct
{
	char	*key;
	char	*value;
	int	line;		/* 1-based line the key/value pair sits on */
} mapprop_t;

typedef struct
{
	mapprop_t	*props;
	int		numprops;
	int		maxprops;

	int		line;		/* line of the entity's opening brace */
	const char	*classname;	/* points into props, never NULL */
	const char	*targetname;	/* points into props, never NULL */
	double		origin[3];
	qboolean	has_origin;	/* false: origin[] is a brush guess */
} mapentity_t;

/* Called once per entity.  Return false to abort the scan of this file. */
typedef qboolean (*mapentity_cb) (const mapentity_t *ent, void *userdata);

/* Returns false on I/O failure (reason left in errbuf).  A truncated or
 * malformed file still reports the entities parsed before the damage, and
 * describes the damage in errbuf. */
qboolean	ES_ScanMapFile (const char *filename, mapentity_cb callback,
				void *userdata, char *errbuf, size_t errlen);

/*
 * ==========================================================================
 * Configuration (es_config.c)
 * ==========================================================================
 */

typedef struct
{
	char	**items;
	int	count;
	int	max;
} strlist_t;

void	ES_StrListAdd (strlist_t *list, const char *str);
void	ES_StrListFree (strlist_t *list);

typedef struct
{
	strlist_t	searchin;	/* directories to walk, recursively */
	strlist_t	exclude;	/* regexes; a matching path is skipped */
	strlist_t	flagsprops;	/* property names holding bit flags */

	qboolean	opt_log;
	qboolean	opt_sortbyline;
	qboolean	opt_pts;
	qboolean	opt_developer;
	qboolean	opt_ignorecase;

	char		logfile[1024];
	char		ptsdir[1024];
} essettings_t;

void		ES_ConfigDefaults (essettings_t *set);
/* Returns false if the file could not be read or parsed; errbuf says why. */
qboolean	ES_ConfigLoad (const char *filename, essettings_t *set,
			       char *errbuf, size_t errlen);

#endif	/* __ENTSEARCH_H */
