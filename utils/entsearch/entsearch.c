/* entsearch.c -- search .map sources for entities by classname and property.
 * Copyright (C) 2026  uHexen2 developers
 *
 * Answers the questions that come up constantly while mapping: how is this
 * property actually used, and where did Raven use it?
 *
 * The idea is not ours.  Inky's MapSearch does this, and does it well:
 *   http://earthday.free.fr/Inkys-Hexen-II-Mapping-Corner/
 * His is the reference implementation and the one to use on Windows.  This
 * is a separate tool that borrows his design -- the three-argument search,
 * the bit matching against flag properties, the point files -- and none of
 * his code, so that the uHexen2 toolchain has an entity search that builds
 * from source everywhere the rest of it does.
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
#include "pathutil.h"
#include "entsearch.h"

#include <errno.h>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <dirent.h>
#endif

#define	MAX_WALK_DEPTH	64
#define	FLAGSTR_SIZE	512

typedef struct
{
	char		*classname;
	char		*targetname;
	char		*key;
	char		*value;
	int		line;
	int		entseq;		/* entity ordinal, so a .pts lists each entity once */
	double		origin[3];
	qboolean	has_origin;
	qboolean	isflags;	/* the value is worth showing as bits */
	long		flagsval;
} match_t;

typedef struct
{
	essettings_t	*set;

	esregex_t	*re_class;
	esregex_t	*re_prop;
	esregex_t	*re_value;
	esregex_t	**re_exclude;
	int		numexclude;

	qboolean	value_is_int;
	long		value_int;

	/* per file */
	match_t		*matches;
	int		nummatches;
	int		maxmatches;
	int		entseq;

	/* totals */
	int		files_scanned;
	int		files_matched;
	long		entities_scanned;
	long		total_matches;
} search_t;

static FILE	*logfp;
static qboolean	developer;
static qboolean	sort_byline;

/*
 * ==========================================================================
 * Output
 *
 * Results go to stdout and, when asked for, to the log.  Diagnostics go to
 * stderr, so that piping the results somewhere stays clean.
 * ==========================================================================
 */

static void out (const char *fmt, ...) FUNC_PRINTF(1,2);
static void out (const char *fmt, ...)
{
	va_list	argptr;

	va_start (argptr, fmt);
	vfprintf (stdout, fmt, argptr);
	va_end (argptr);

	if (logfp)
	{
		va_start (argptr, fmt);
		vfprintf (logfp, fmt, argptr);
		va_end (argptr);
	}
}

static void dev (const char *fmt, ...) FUNC_PRINTF(1,2);
static void dev (const char *fmt, ...)
{
	va_list	argptr;

	if (!developer)
		return;
	fprintf (stderr, "[dev] ");
	va_start (argptr, fmt);
	vfprintf (stderr, fmt, argptr);
	va_end (argptr);
}

static void usage (void)
{
	printf (
"entsearch %s -- search Quake/Hexen II .map sources for entities.\n"
"\n"
"usage: entsearch CLASSNAME [PROPERTY] [VALUE] [OPTIONS]\n"
"\n"
"CLASSNAME, PROPERTY and VALUE are regular expressions and must match a\n"
"whole name or value, not a part of one -- which is why \"func_train.*\" is\n"
"what also reaches func_train_mp.  Leave one out, or pass \"*\" or \".*\", to\n"
"accept anything.  Only CLASSNAME is required.\n"
"\n"
"  Supported syntax: . [abc] [^a-z] \\d \\D \\w \\W \\s \\S | (...) ^ $\n"
"                    * + ? {n} {n,} {n,m} and their lazy ? forms\n"
"\n"
"When VALUE is an integer it is also tested against the bits of the flag\n"
"properties -- spawnflags and its kin -- so \"96\" finds any entity with both\n"
"the 32 and 64 boxes checked, whatever else is checked alongside them.\n"
"\n"
"OPTIONS is a run of these letters, each switched on by a preceding '+'\n"
"(the default) or off by a '-'.  Letters left out keep their config value.\n"
"  l  write the results to a log file as well\n"
"  n  sort by line number instead of classname, targetname, property\n"
"  p  write a TrenchBroom .pts point file beside each matching map\n"
"  i  ignore case when matching (on unless the config says otherwise)\n"
"  d  report what is being read and skipped, on stderr\n"
"\n"
"  --config=FILE   read this config instead of looking for one\n"
"  --help          this text\n"
"  --version       version information\n"
"\n"
"Where to search, what to skip and which properties hold bit flags all come\n"
"from entsearch-config.xml; without one, the current directory is searched\n"
"recursively.  See utils/entsearch/README for the whole story.\n"
"\n"
"Note for Unix shells: quote the '*' shorthand, or the shell will expand it\n"
"to the file names in the current directory before entsearch ever sees it.\n"
"\n"
"examples:\n"
"  entsearch '*' spawnflags 64        entities with the 64 box checked\n"
"  entsearch '*' light 250            any classname whose light value is 250\n"
"  entsearch '*' '*' '.*\\.wav'        any property naming a wav file\n"
"  entsearch func_monsterspawner spawnflags 16    spawners set to spiders\n"
"  entsearch monster_fallen_angel_lord classname  one line per angel lord\n"
"\n"
"exit status: 0 if something matched, 1 if nothing did, 2 on error.\n",
	ENTSEARCH_VERSION);
}

/*
 * ==========================================================================
 * Value helpers
 * ==========================================================================
 */

/* "64" and "64.000000" are both the integer 64; "64.5" and "left" are not. */
static qboolean value_as_long (const char *s, long *out)
{
	char	*stop;
	double	d;

	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s)
		return false;

	d = strtod (s, &stop);
	if (stop == s)
		return false;
	while (*stop == ' ' || *stop == '\t')
		stop++;
	if (*stop)
		return false;

	if (d < -2147483647.0 || d > 2147483647.0)
		return false;
	if (d != (double)(long)d)
		return false;

	*out = (long) d;
	return true;
}

static qboolean is_flags_prop (const essettings_t *set, const char *key)
{
	int	i;

	for (i = 0; i < set->flagsprops.count; i++)
	{
		if (!q_strcasecmp (key, set->flagsprops.items[i]))
			return true;
	}
	return false;
}

/* "32960" -> "[64][128][32768]", the way an editor draws the check boxes. */
static void flags_string (long v, char *out, size_t size)
{
	int	bit;

	*out = '\0';
	if (v <= 0)
		return;
	for (bit = 0; bit < 31; bit++)
	{
		if (v & (1L << bit))
		{
			char	tmp[24];

			q_snprintf (tmp, sizeof(tmp), "[%ld]", 1L << bit);
			q_strlcat (out, tmp, size);
		}
	}
}

static void origin_string (const double *o, char *out, size_t size)
{
	q_snprintf (out, size, "%.6g %.6g %.6g", o[0], o[1], o[2]);
}

/*
 * ==========================================================================
 * Matching
 * ==========================================================================
 */

static void add_match (search_t *s, const mapentity_t *ent, const mapprop_t *prop,
		       qboolean isflags, long flagsval)
{
	match_t	*m;

	if (s->nummatches == s->maxmatches)
	{
		int	newmax = s->maxmatches ? s->maxmatches * 2 : 64;
		match_t	*grown = (match_t *) realloc (s->matches, newmax * sizeof(match_t));

		if (!grown)
			COM_Error ("%s: out of memory", __thisfunc__);
		s->matches = grown;
		s->maxmatches = newmax;
	}

	m = &s->matches[s->nummatches++];
	memset (m, 0, sizeof(*m));
	m->classname = SafeStrdup (ent->classname);
	m->targetname = SafeStrdup (ent->targetname);
	m->key = SafeStrdup (prop->key);
	m->value = SafeStrdup (prop->value);
	m->line = prop->line;
	m->entseq = s->entseq;
	m->origin[0] = ent->origin[0];
	m->origin[1] = ent->origin[1];
	m->origin[2] = ent->origin[2];
	m->has_origin = ent->has_origin;
	m->isflags = isflags;
	m->flagsval = flagsval;
}

static void free_matches (search_t *s)
{
	int	i;

	for (i = 0; i < s->nummatches; i++)
	{
		free (s->matches[i].classname);
		free (s->matches[i].targetname);
		free (s->matches[i].key);
		free (s->matches[i].value);
	}
	s->nummatches = 0;
}

static qboolean entity_cb (const mapentity_t *ent, void *userdata)
{
	search_t	*s = (search_t *) userdata;
	int		i;

	s->entities_scanned++;
	s->entseq++;

	if (!ES_RegexMatch (s->re_class, ent->classname))
		return true;

	for (i = 0; i < ent->numprops; i++)
	{
		const mapprop_t	*prop = &ent->props[i];
		qboolean	isflags;
		long		flagsval = 0;
		qboolean	hit;

		if (!ES_RegexMatch (s->re_prop, prop->key))
			continue;

		isflags = is_flags_prop (s->set, prop->key) &&
			  value_as_long (prop->value, &flagsval);

		hit = ES_RegexMatch (s->re_value, prop->value);
		if (!hit && isflags && s->value_is_int && s->value_int != 0)
		{
			/* every bit the search asks for is checked here, and
			 * the entity is free to have others checked too */
			hit = ((flagsval & s->value_int) == s->value_int);
		}

		if (hit)
			add_match (s, ent, prop, isflags, flagsval);
	}

	return true;
}

static int match_cmp (const void *a, const void *b)
{
	const match_t	*ma = (const match_t *) a;
	const match_t	*mb = (const match_t *) b;
	int		r;

	if (sort_byline)
	{
		if (ma->line != mb->line)
			return (ma->line < mb->line) ? -1 : 1;
		return strcmp (ma->key, mb->key);
	}

	r = q_strcasecmp (ma->classname, mb->classname);
	if (r)
		return r;
	r = q_strcasecmp (ma->targetname, mb->targetname);
	if (r)
		return r;
	r = q_strcasecmp (ma->key, mb->key);
	if (r)
		return r;
	return (ma->line < mb->line) ? -1 : (ma->line > mb->line);
}

/*
 * ==========================================================================
 * Reporting one file
 * ==========================================================================
 */

static void write_pts (search_t *s, const char *mapfile)
{
	char	path[1024];
	char	base[256];
	FILE	*f;
	int	i, written = 0;
	int	lastseq = -1;

	ExtractFileBase (mapfile, base, sizeof(base));
	if (s->set->ptsdir[0])
		q_snprintf (path, sizeof(path), "%s/%s.pts", s->set->ptsdir, base);
	else
	{
		q_strlcpy (path, mapfile, sizeof(path));
		StripExtension (path);
		q_strlcat (path, ".pts", sizeof(path));
	}

	f = fopen (path, "w");
	if (!f)
	{
		fprintf (stderr, "entsearch: cannot write %s: %s\n", path, strerror(errno));
		return;
	}

	/* One point per matching entity, in map order, so that stepping
	 * through the point file in TrenchBroom visits each hit once. */
	for (i = 0; i < s->nummatches; i++)
	{
		const match_t *m = &s->matches[i];

		if (m->entseq == lastseq)
			continue;
		lastseq = m->entseq;
		fprintf (f, "%f %f %f\n", m->origin[0], m->origin[1], m->origin[2]);
		written++;
	}
	fclose (f);
	dev ("wrote %s (%d point%s)\n", path, written, written == 1 ? "" : "s");
}

static void report_file (search_t *s, const char *mapfile)
{
	int	i;

	if (!s->nummatches)
		return;

	s->files_matched++;
	s->total_matches += s->nummatches;

	/* The point file has to follow the map, not the display order, so it
	 * is written while the matches are still in the order they were found. */
	if (s->set->opt_pts)
		write_pts (s, mapfile);

	qsort (s->matches, s->nummatches, sizeof(match_t), match_cmp);

	out ("\n%s -- %d match%s\n", mapfile, s->nummatches,
	     s->nummatches == 1 ? "" : "es");

	for (i = 0; i < s->nummatches; i++)
	{
		const match_t	*m = &s->matches[i];
		char		cname[128];
		char		qual[512];
		char		flagstr[FLAGSTR_SIZE];
		char		originstr[128];

		if (m->targetname[0])
			q_snprintf (qual, sizeof(qual), "%s.%s", m->targetname, m->key);
		else
			q_strlcpy (qual, m->key, sizeof(qual));

		*flagstr = '\0';
		if (m->isflags && m->flagsval > 0)
		{
			char	bits[FLAGSTR_SIZE];

			flags_string (m->flagsval, bits, sizeof(bits));
			q_snprintf (flagstr, sizeof(flagstr), " %s", bits);
		}

		origin_string (m->origin, originstr, sizeof(originstr));
		q_snprintf (cname, sizeof(cname), "(%s)", m->classname);

		out ("    Line %6d:  %-24s %-30s == %s%s   at  %s%s\n",
		     m->line, cname, qual, m->value, flagstr, originstr,
		     m->has_origin ? "" : "  [brush]");
	}
}

/*
 * ==========================================================================
 * Walking the map directories
 * ==========================================================================
 */

/* Excludes are matched against the whole path with either separator, and
 * against the bare entry name, so that a config written on one platform (or
 * one that just says "autosave") keeps working on the other. */
static qboolean is_excluded (search_t *s, const char *path, const char *name)
{
	char	fwd[1024], back[1024];
	int	i;
	char	*c;

	if (!s->numexclude)
		return false;

	q_strlcpy (fwd, path, sizeof(fwd));
	for (c = fwd; *c; c++)
	{
		if (*c == '\\')
			*c = '/';
	}
	q_strlcpy (back, fwd, sizeof(back));
	for (c = back; *c; c++)
	{
		if (*c == '/')
			*c = '\\';
	}

	for (i = 0; i < s->numexclude; i++)
	{
		if (ES_RegexMatch (s->re_exclude[i], fwd) ||
		    ES_RegexMatch (s->re_exclude[i], back) ||
		    (name && ES_RegexMatch (s->re_exclude[i], name)))
			return true;
	}
	return false;
}

static qboolean has_map_extension (const char *name)
{
	const char	*dot = strrchr (name, '.');

	return (dot && !q_strcasecmp (dot, ".map"));
}

static void scan_file (search_t *s, const char *path)
{
	char	errbuf[512];

	s->files_scanned++;
	s->entseq = 0;
	free_matches (s);

	if (!ES_ScanMapFile (path, entity_cb, s, errbuf, sizeof(errbuf)))
	{
		fprintf (stderr, "entsearch: %s: %s\n", path, errbuf);
		return;
	}
	if (errbuf[0])
		fprintf (stderr, "entsearch: %s: %s\n", path, errbuf);

	report_file (s, path);
	free_matches (s);
}

/* Reads every name in a directory.  Q_FindFirstFile cannot be used here: it
 * hides directories, and there is only one find handle to go round. */
static void list_directory (const char *dir, strlist_t *names)
{
#if defined(PLATFORM_WINDOWS)
	char			pattern[1024];
	WIN32_FIND_DATA		data;
	HANDLE			handle;

	q_snprintf (pattern, sizeof(pattern), "%s\\*", dir);
	handle = FindFirstFile (pattern, &data);
	if (handle == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (strcmp (data.cFileName, ".") && strcmp (data.cFileName, ".."))
			ES_StrListAdd (names, data.cFileName);
	} while (FindNextFile (handle, &data));
	FindClose (handle);
#else
	DIR			*d = opendir (dir);
	struct dirent		*ent;

	if (!d)
		return;
	while ((ent = readdir (d)) != NULL)
	{
		if (strcmp (ent->d_name, ".") && strcmp (ent->d_name, ".."))
			ES_StrListAdd (names, ent->d_name);
	}
	closedir (d);
#endif
}

static int name_cmp (const void *a, const void *b)
{
	return strcmp (*(const char * const *) a, *(const char * const *) b);
}

static void walk_dir (search_t *s, const char *dir, int depth)
{
	strlist_t	names;
	int		i;

	if (depth > MAX_WALK_DEPTH)
	{
		fprintf (stderr, "entsearch: %s: too deep, not descending further\n", dir);
		return;
	}

	memset (&names, 0, sizeof(names));
	list_directory (dir, &names);
	if (names.count > 1)
		qsort (names.items, names.count, sizeof(char *), name_cmp);

	for (i = 0; i < names.count; i++)
	{
		char	path[1024];
		int	type;

		q_snprintf (path, sizeof(path), "%s/%s", dir, names.items[i]);
		type = Q_FileType (path);

		if (type == FS_ENT_DIRECTORY)
		{
			if (is_excluded (s, path, names.items[i]))
			{
				dev ("skipping excluded directory %s\n", path);
				continue;
			}
			walk_dir (s, path, depth + 1);
		}
		else if (type == FS_ENT_FILE && has_map_extension (names.items[i]))
		{
			if (is_excluded (s, path, names.items[i]))
			{
				dev ("skipping excluded file %s\n", path);
				continue;
			}
			dev ("reading %s\n", path);
			scan_file (s, path);
		}
	}

	ES_StrListFree (&names);
}

/*
 * ==========================================================================
 * Startup
 * ==========================================================================
 */

/* The original's shorthand: nothing, "*" or ".*" all mean "anything". */
static const char *pattern_or_any (const char *arg)
{
	if (!arg || !*arg || !strcmp (arg, "*"))
		return ".*";
	return arg;
}

static esregex_t *compile_or_die (const char *pattern, qboolean ignorecase,
				  const char *what)
{
	char		err[256];
	esregex_t	*re = ES_RegexCompile (pattern, ignorecase, err, sizeof(err));

	if (!re)
	{
		fprintf (stderr, "entsearch: bad %s pattern \"%s\": %s\n",
			 what, pattern, err);
		exit (2);
	}
	return re;
}

#define	OPT_LETTERS	"lnpdi"

static qboolean is_option_token (const char *arg, qboolean positionals_full)
{
	const char	*c;
	qboolean	sawletter = false;

	if (!*arg)
		return false;
	if (*arg != '+' && *arg != '-' && !positionals_full)
		return false;

	for (c = arg; *c; c++)
	{
		if (*c == '+' || *c == '-')
			continue;
		if (!strchr (OPT_LETTERS, *c))
			return false;
		sawletter = true;
	}
	return sawletter;
}

/* A leading '-' is only a typo'd option when what follows is all letters.
 * Values genuinely start with a minus -- "angle" is often -1. */
static qboolean looks_like_option (const char *arg)
{
	const char	*c;

	if (!strncmp (arg, "--", 2))
		return true;
	if (arg[0] != '-' || !arg[1])
		return false;
	for (c = arg + 1; *c; c++)
	{
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')))
			return false;
	}
	return true;
}

/* -1 means "not mentioned on the command line; keep whatever the config says" */
static int opt_state[5];

static int opt_index (char letter)
{
	const char	*at = strchr (OPT_LETTERS, letter);

	return at ? (int)(at - OPT_LETTERS) : -1;
}

static void parse_option_token (const char *arg)
{
	qboolean	on = true;
	const char	*c;

	for (c = arg; *c; c++)
	{
		if (*c == '+')
			on = true;
		else if (*c == '-')
			on = false;
		else
			opt_state[opt_index (*c)] = on ? 1 : 0;
	}
}

/* config next to the binary, for a toolchain that is not on the PATH */
static qboolean config_beside_exe (const char *argv0, char *out_path, size_t size)
{
	char	dir[1024];

	if (!argv0 || !*argv0)
		return false;
	q_strlcpy (dir, argv0, sizeof(dir));
	StripFilename (dir);
	if (!dir[0])
		return false;
	q_snprintf (out_path, size, "%s/entsearch-config.xml", dir);
	return (Q_FileType (out_path) == FS_ENT_FILE);
}

static qboolean find_config (const char *argv0, char *out_path, size_t size)
{
	const char	*env = getenv ("ENTSEARCH_CONFIG");

	if (env && *env)
	{
		q_strlcpy (out_path, env, size);
		return true;
	}
	if (Q_FileType ("entsearch-config.xml") == FS_ENT_FILE)
	{
		q_strlcpy (out_path, "entsearch-config.xml", size);
		return true;
	}
	return config_beside_exe (argv0, out_path, size);
}

int main (int argc, char **argv)
{
	essettings_t	settings;
	search_t	search;
	const char	*positional[3];
	int		numpositional = 0;
	const char	*configarg = NULL;
	char		configpath[1024];
	char		errbuf[512];
	int		i;

	for (i = 0; i < (int)(sizeof(opt_state) / sizeof(opt_state[0])); i++)
		opt_state[i] = -1;

	for (i = 1; i < argc; i++)
	{
		const char	*arg = argv[i];

		if (!strcmp (arg, "--help") || !strcmp (arg, "-help") ||
		    !strcmp (arg, "/?"))
		{
			usage ();
			return 0;
		}
		if (!strcmp (arg, "--version"))
		{
			printf ("entsearch %s (uHexen2 utilities)\n", ENTSEARCH_VERSION);
			printf ("search design after Inky's MapSearch --\n");
			printf ("http://earthday.free.fr/Inkys-Hexen-II-Mapping-Corner/\n");
			return 0;
		}
		if (!strncmp (arg, "--config=", 9))
		{
			configarg = arg + 9;
			continue;
		}
		if (!strcmp (arg, "--config") && i + 1 < argc)
		{
			configarg = argv[++i];
			continue;
		}
		if (is_option_token (arg, numpositional >= 3))
		{
			parse_option_token (arg);
			continue;
		}
		if (looks_like_option (arg))
		{
			fprintf (stderr, "entsearch: unknown option \"%s\"; "
					 "option letters are [%s]\n", arg, OPT_LETTERS);
			return 2;
		}
		if (numpositional < 3)
		{
			positional[numpositional++] = arg;
		}
		else
		{
			fprintf (stderr, "entsearch: unexpected argument \"%s\"\n", arg);
			return 2;
		}
	}

	if (!numpositional)
	{
		usage ();
		return 2;
	}

	/* so that -d also reports how the config was found */
	developer = (opt_state[opt_index('d')] == 1);

	while (numpositional < 3)
		positional[numpositional++] = "";

	ES_ConfigDefaults (&settings);

	if (configarg)
		q_strlcpy (configpath, configarg, sizeof(configpath));
	else if (!find_config (argv[0], configpath, sizeof(configpath)))
		configpath[0] = '\0';

	if (configpath[0])
	{
		if (!ES_ConfigLoad (configpath, &settings, errbuf, sizeof(errbuf)))
		{
			fprintf (stderr, "entsearch: %s: %s\n", configpath, errbuf);
			if (configarg)		/* asked for by name: do not guess */
				return 2;
		}
		else
		{
			dev ("config: %s\n", configpath);
			if (errbuf[0])
				fprintf (stderr, "entsearch: %s: %s\n", configpath, errbuf);
		}
	}

	/* command line options win over the config */
	if (opt_state[opt_index('l')] >= 0)
		settings.opt_log = opt_state[opt_index('l')];
	if (opt_state[opt_index('n')] >= 0)
		settings.opt_sortbyline = opt_state[opt_index('n')];
	if (opt_state[opt_index('p')] >= 0)
		settings.opt_pts = opt_state[opt_index('p')];
	if (opt_state[opt_index('d')] >= 0)
		settings.opt_developer = opt_state[opt_index('d')];
	if (opt_state[opt_index('i')] >= 0)
		settings.opt_ignorecase = opt_state[opt_index('i')];

	developer = settings.opt_developer;
	sort_byline = settings.opt_sortbyline;

	if (!configpath[0])
		dev ("no config file found; searching \".\" with the built-in defaults\n");

	memset (&search, 0, sizeof(search));
	search.set = &settings;
	search.re_class = compile_or_die (pattern_or_any (positional[0]),
					  settings.opt_ignorecase, "classname");
	search.re_prop  = compile_or_die (pattern_or_any (positional[1]),
					  settings.opt_ignorecase, "property");
	search.re_value = compile_or_die (pattern_or_any (positional[2]),
					  settings.opt_ignorecase, "value");
	search.value_is_int = value_as_long (positional[2], &search.value_int);

	if (settings.exclude.count)
	{
		search.re_exclude = (esregex_t **)
			SafeMalloc (settings.exclude.count * sizeof(esregex_t *));
		for (i = 0; i < settings.exclude.count; i++)
		{
			search.re_exclude[i] =
				compile_or_die (pattern_or_any (settings.exclude.items[i]),
						true, "exclude");
		}
		search.numexclude = settings.exclude.count;
	}

	if (settings.opt_log)
	{
		logfp = fopen (settings.logfile, "w");
		if (!logfp)
		{
			fprintf (stderr, "entsearch: cannot write %s: %s\n",
				 settings.logfile, strerror(errno));
			return 2;
		}
	}

	out ("entsearch: classname \"%s\", property \"%s\", value \"%s\"%s\n",
	     pattern_or_any (positional[0]), pattern_or_any (positional[1]),
	     pattern_or_any (positional[2]),
	     search.value_is_int ? "  (an integer: also matched against flag bits)" : "");

	for (i = 0; i < settings.searchin.count; i++)
	{
		const char	*dir = settings.searchin.items[i];
		int		type = Q_FileType (dir);

		if (type == FS_ENT_DIRECTORY)
		{
			dev ("searching %s\n", dir);
			walk_dir (&search, dir, 0);
		}
		else if (type == FS_ENT_FILE)
		{	/* a single map named directly is convenient */
			dev ("reading %s\n", dir);
			scan_file (&search, dir);
		}
		else
		{
			fprintf (stderr, "entsearch: %s: no such directory\n", dir);
		}
	}

	out ("\nsearched %d map file%s (%ld entit%s); %d file%s matched, %ld result%s\n",
	     search.files_scanned, search.files_scanned == 1 ? "" : "s",
	     search.entities_scanned, search.entities_scanned == 1 ? "y" : "ies",
	     search.files_matched, search.files_matched == 1 ? "" : "s",
	     search.total_matches, search.total_matches == 1 ? "" : "s");

	if (logfp)
	{
		fclose (logfp);
		logfp = NULL;
		fprintf (stderr, "entsearch: results also written to %s\n", settings.logfile);
	}

	free_matches (&search);
	if (search.matches)
		free (search.matches);
	ES_RegexFree (search.re_class);
	ES_RegexFree (search.re_prop);
	ES_RegexFree (search.re_value);
	for (i = 0; i < search.numexclude; i++)
		ES_RegexFree (search.re_exclude[i]);
	if (search.re_exclude)
		free (search.re_exclude);
	ES_StrListFree (&settings.searchin);
	ES_StrListFree (&settings.exclude);
	ES_StrListFree (&settings.flagsprops);

	return search.total_matches ? 0 : 1;
}
