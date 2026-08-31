/* mapsearch.h -- shared declarations for the mapsearch utility.
 * Copyright (C) 2026  uHexen2 developers
 *
 * A reimplementation of Inky's MapSearch (http://earthday.free.fr/), which
 * searches Quake/Hexen II .map sources for entities by classname, property
 * name and property value.
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

#ifndef __MAPSEARCH_H
#define __MAPSEARCH_H

#define	MAPSEARCH_VERSION	"1.0"

/*
 * ==========================================================================
 * Regular expressions (ms_regex.c)
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

typedef struct msregex_s	msregex_t;

msregex_t	*MS_RegexCompile (const char *pattern, qboolean ignorecase,
				  char *errbuf, size_t errlen);
qboolean	MS_RegexMatch (msregex_t *re, const char *text);
void		MS_RegexFree (msregex_t *re);

/*
 * ==========================================================================
 * .map parsing (ms_map.c)
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
qboolean	MS_ScanMapFile (const char *filename, mapentity_cb callback,
				void *userdata, char *errbuf, size_t errlen);

/*
 * ==========================================================================
 * Configuration (ms_config.c)
 * ==========================================================================
 */

typedef struct
{
	char	**items;
	int	count;
	int	max;
} strlist_t;

void	MS_StrListAdd (strlist_t *list, const char *str);
void	MS_StrListFree (strlist_t *list);

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
} mssettings_t;

void		MS_ConfigDefaults (mssettings_t *set);
/* Returns false if the file could not be read or parsed; errbuf says why. */
qboolean	MS_ConfigLoad (const char *filename, mssettings_t *set,
			       char *errbuf, size_t errlen);

#endif	/* __MAPSEARCH_H */
