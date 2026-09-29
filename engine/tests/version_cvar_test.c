/*
 * version_cvar_test.c -- HW_VERSION_* must agree with HW_BASE_VERSION.
 * Issue #279.
 *
 * Copyright (C) 2026  Hexenwail contributors
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

/*
 * WHAT THIS PINS
 *
 * `hexenwail' is a CVAR_ROM cvar carrying HW_VERSION_NUM, and it is the only
 * way a HexenC mod can ask which engine it is running under (sv_main.c, and
 * the long comment on HW_VERSION_MAJOR in hexen2/quakedef.h for why it is a
 * number and not checkextension()).  Mods branch on it with `>=', so a value
 * that lags the release it claims to be is worse than no value: the mod
 * enables the older behaviour on an engine that has the newer.
 *
 * HW_BASE_VERSION is a string and the four components are integers, so nothing
 * in C couples them -- the preprocessor cannot parse "0.8.0-beta.r31".  The
 * version-bump skill edits the string, and forgetting the numbers is the
 * obvious failure mode.  This test is that coupling: it reads the header as
 * text, parses the string, and compares.
 *
 * It is a text test for a reason the other text tests here do not share: there
 * is no runtime to link.  Both sides are preprocessor tokens in one header, so
 * reading the header IS reading the thing under test, not a proxy for it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * CMake passes the engine source dir explicitly, which is cwd-independent.
 * CI compiles this file directly with plain cc and no -D, so fall back to
 * __FILE__ with the "tests/" tail stripped rather than failing to build.
 */
static const char *EngineSourceDir (void)
{
#ifdef VERSION_TEST_ENGINE_SOURCE_DIR
	return VERSION_TEST_ENGINE_SOURCE_DIR;
#else
	static char	dir[1024];
	const char	*self = __FILE__;
	const char	*tail = strstr(self, "tests/");
	size_t		len;

	if (!tail || tail == self)
		return ".";

	len = (size_t)(tail - self) - 1;	/* drop the separator too */
	if (len >= sizeof(dir))
		return ".";

	memcpy(dir, self, len);
	dir[len] = '\0';
	return dir;
#endif
}

static char *ReadSourceFile (const char *relative_path)
{
	char	path[1024];
	FILE	*f;
	long	size;
	char	*text;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", EngineSourceDir(),
			     relative_path) >= sizeof(path))
	{
		fprintf(stderr, "version: path too long: %s\n", relative_path);
		return NULL;
	}

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "version: cannot open %s\n", path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0)
	{
		fprintf(stderr, "version: cannot size %s\n", path);
		fclose(f);
		return NULL;
	}

	text = (char *) malloc((size_t)size + 1);
	if (!text)
	{
		fclose(f);
		return NULL;
	}

	if (fread(text, 1, (size_t)size, f) != (size_t)size)
	{
		fprintf(stderr, "version: cannot read %s\n", path);
		free(text);
		fclose(f);
		return NULL;
	}

	text[size] = '\0';
	fclose(f);
	return text;
}

/*
 * Value of `#define <name> <integer>'.  Returns -1 when the macro is missing,
 * which is itself a failure: every one of these must exist.
 */
static long DefineValue (const char *text, const char *name)
{
	const char	*p = text;
	size_t		len = strlen(name);

	while ((p = strstr(p, "#define")) != NULL)
	{
		const char *q = p + 7;

		p += 7;
		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, name, len) != 0)
			continue;
		q += len;
		if (*q != ' ' && *q != '\t')
			continue;	/* HW_VERSION_MAJOR vs HW_VERSION_MAJORITY */
		while (*q == ' ' || *q == '\t')
			q++;
		return strtol(q, NULL, 10);
	}

	return -1;
}

/* The version string out of `#define HW_BASE_VERSION "..."'. */
static int BaseVersionString (const char *text, char *out, size_t outsz)
{
	const char	*p = strstr(text, "#define\tHW_BASE_VERSION");
	const char	*q, *end;

	if (!p)
		p = strstr(text, "#define HW_BASE_VERSION");
	if (!p)
		return 0;

	q = strchr(p, '"');
	if (!q)
		return 0;
	q++;
	end = strchr(q, '"');
	if (!end || (size_t)(end - q) >= outsz)
		return 0;

	memcpy(out, q, (size_t)(end - q));
	out[end - q] = '\0';
	return 1;
}

/*
 * MAJOR.MINOR.PATCH, optionally followed by a phase and ".rN".  A string with
 * no revision means revision 0.  Anything this cannot parse is a failure
 * rather than a shrug -- an unparseable version string would silently stop
 * this gate from checking anything.
 */
static int ParseVersion (const char *s, long *major, long *minor, long *patch,
			 long *rev)
{
	const char	*r;
	char		*end;

	*major = strtol(s, &end, 10);
	if (end == s || *end != '.')
		return 0;
	s = end + 1;
	*minor = strtol(s, &end, 10);
	if (end == s || *end != '.')
		return 0;
	s = end + 1;
	*patch = strtol(s, &end, 10);
	if (end == s)
		return 0;

	*rev = 0;
	r = strstr(end, ".r");
	if (r)
	{
		s = r + 2;
		*rev = strtol(s, &end, 10);
		if (end == s)
			return 0;
	}

	return 1;
}

int main (void)
{
	char	*header = ReadSourceFile("hexen2/quakedef.h");
	char	version[64];
	long	smaj, smin, spat, srev;
	long	dmaj, dmin, dpat, drev;
	int	failed = 0;

	if (!header)
		return 1;

	if (!BaseVersionString(header, version, sizeof(version)))
	{
		fprintf(stderr, "version: no HW_BASE_VERSION string in "
			"hexen2/quakedef.h\n");
		free(header);
		return 1;
	}

	if (!ParseVersion(version, &smaj, &smin, &spat, &srev))
	{
		fprintf(stderr, "version: cannot parse HW_BASE_VERSION \"%s\" as "
			"MAJOR.MINOR.PATCH[-phase.rN].  If the scheme changed on "
			"purpose, update ParseVersion here too -- otherwise this "
			"gate silently stops checking anything.\n", version);
		free(header);
		return 1;
	}

	dmaj = DefineValue(header, "HW_VERSION_MAJOR");
	dmin = DefineValue(header, "HW_VERSION_MINOR");
	dpat = DefineValue(header, "HW_VERSION_PATCH");
	drev = DefineValue(header, "HW_VERSION_REV");

	if (dmaj < 0 || dmin < 0 || dpat < 0 || drev < 0)
	{
		fprintf(stderr, "version: HW_VERSION_MAJOR/MINOR/PATCH/REV must all "
			"be defined in hexen2/quakedef.h (got %ld/%ld/%ld/%ld, -1 "
			"means missing)\n", dmaj, dmin, dpat, drev);
		free(header);
		return 1;
	}

	if (smaj != dmaj || smin != dmin || spat != dpat || srev != drev)
	{
		fprintf(stderr,
			"version: HW_VERSION_* does not match HW_BASE_VERSION.\n"
			"  HW_BASE_VERSION  \"%s\"  ->  %ld.%ld.%ld rev %ld\n"
			"  HW_VERSION_*                 %ld.%ld.%ld rev %ld\n"
			"A version bump edited the string and not the numbers.  The\n"
			"`hexenwail' cvar is stamped from the numbers, so mods would\n"
			"be told the wrong engine version -- see issue #279.\n",
			version, smaj, smin, spat, srev, dmaj, dmin, dpat, drev);
		failed = 1;
	}

	free(header);
	return failed;
}
