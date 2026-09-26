/*
 * sv_checkbottom_test.c -- SV_CheckBottom's corner box comes from the entity
 * itself, not from a world clip hull.  Issue #278.
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
 * SV_CheckBottom samples the four corners under an entity and refuses the move
 * if any of them is off an edge.  Which corners it samples is decided by two
 * lines at the top of the function:
 *
 *	VectorAdd (ent->v.origin, ent->v.mins, mins);
 *	VectorAdd (ent->v.origin, ent->v.maxs, maxs);
 *
 * Raven left a block after those two that overwrites mins/maxs with the WORLD
 * CLIP HULL's fixed box -- +/-16 through +/-48 in x/y -- commented out.  It was
 * uncommented in this tree in "Hexenwail: SDL3/GL4.3 modernization (2025-2026)"
 * and shipped for six months, pushing the corner samples up to 32 units outside
 * the monster's real footprint, so monsters refused steps uHexen2 allows.
 * Reverted in #278; the long comment on the dead block in
 * engine/hexen2/sv_move.c has the full account.
 *
 * WHY SOURCE TEXT AND NOT A UNIT CALL
 *
 * Same reason as brush_render_state_test.c's ordering cases and
 * vid_restart_context_test.c: SV_CheckBottom cannot be linked here.  It reaches
 * sv.models[], sv.edicts, SV_Move, SV_PointContents and Con_Printf, which means
 * quakedef.h and most of the server.  A test that reimplemented the corner
 * arithmetic would only restate it and would have passed the whole time the bug
 * was live -- the bug was not wrong arithmetic, it was an extra assignment.  So
 * the invariant asserted is the one that actually broke: within the function,
 * before the corner loop, mins and maxs are each written exactly once, and from
 * the entity's own bbox.
 *
 * Both server copies are checked.  engine/hexenworld/server/sv_move.c has never
 * carried the block; pinning it too stops a future port from copying the wrong
 * one back across.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * CMake passes the engine source dir explicitly, which is cwd-independent.
 * CI may compile this file directly with plain cc and no -D, so fall back to
 * __FILE__ with the "tests/" tail stripped rather than failing to build.
 */
static const char *EngineSourceDir (void)
{
#ifdef SV_CHECKBOTTOM_TEST_ENGINE_SOURCE_DIR
	return SV_CHECKBOTTOM_TEST_ENGINE_SOURCE_DIR;
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
		fprintf(stderr, "checkbottom: path too long: %s\n", relative_path);
		return NULL;
	}

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "checkbottom: cannot open %s\n", path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0)
	{
		fprintf(stderr, "checkbottom: cannot size %s\n", path);
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
		fprintf(stderr, "checkbottom: cannot read %s\n", path);
		free(text);
		fclose(f);
		return NULL;
	}

	text[size] = '\0';
	fclose(f);
	return text;
}

/*
 * Blank out comments in place, so a dead block reads as absent while keeping
 * every byte offset and newline intact for the line numbers in failures.
 * String and char literals are tracked because live code may legitimately hold
 * comment punctuation inside one, and a stripper that ignored them could open a
 * comment that never closes and hide the rest of the function.
 */
static void StripComments (char *text)
{
	enum { CODE, LINE_COMMENT, BLOCK_COMMENT, STRING, CHARLIT } state = CODE;
	char	*p;

	for (p = text; *p; p++)
	{
		switch (state)
		{
		case CODE:
			if (p[0] == '/' && p[1] == '/')
			{
				state = LINE_COMMENT;
				p[0] = p[1] = ' ';
				p++;
			}
			else if (p[0] == '/' && p[1] == '*')
			{
				state = BLOCK_COMMENT;
				p[0] = p[1] = ' ';
				p++;
			}
			else if (p[0] == '"')
				state = STRING;
			else if (p[0] == '\'')
				state = CHARLIT;
			break;

		case LINE_COMMENT:
			if (p[0] == '\n')
				state = CODE;
			else
				p[0] = ' ';
			break;

		case BLOCK_COMMENT:
			if (p[0] == '*' && p[1] == '/')
			{
				state = CODE;
				p[0] = p[1] = ' ';
				p++;
			}
			else if (p[0] != '\n')
				p[0] = ' ';
			break;

		case STRING:
			if (p[0] == '\\' && p[1])
				p++;
			else if (p[0] == '"')
				state = CODE;
			break;

		case CHARLIT:
			if (p[0] == '\\' && p[1])
				p++;
			else if (p[0] == '\'')
				state = CODE;
			break;
		}
	}
}

/* Count non-overlapping occurrences of needle in [begin, end). */
static int CountIn (const char *begin, const char *end, const char *needle)
{
	size_t	len = strlen(needle);
	int	n = 0;

	while (begin + len <= end)
	{
		const char *hit = strstr(begin, needle);

		if (!hit || hit + len > end)
			break;
		n++;
		begin = hit + len;
	}

	return n;
}

/*
 * The region under test: from SV_CheckBottom's signature to the comment that
 * opens the corner sampling.  Everything that can redefine the corner box lives
 * in there; the traces below it are not this test's business.
 *
 * The anchor is a code line rather than the comment above it, because the
 * comments are blanked before this runs.
 */
#define	CORNER_LOOP_ANCHOR	"start[2] = mins[2] - 1;"

static int CheckOneCopy (const char *path)
{
	char		*text = ReadSourceFile(path);
	const char	*head, *end;
	int		failed = 0;
	int		n;

	if (!text)
		return 1;

	StripComments(text);

	head = strstr(text, "qboolean SV_CheckBottom (edict_t *ent)");
	if (!head)
	{
		fprintf(stderr, "checkbottom: SV_CheckBottom not found in %s\n", path);
		free(text);
		return 1;
	}

	end = strstr(head, CORNER_LOOP_ANCHOR);
	if (!end)
	{
		fprintf(stderr, "checkbottom: %s: cannot find `%s' after "
			"SV_CheckBottom -- the corner loop was rewritten, so this "
			"test no longer knows what it is checking\n",
			path, CORNER_LOOP_ANCHOR);
		free(text);
		return 1;
	}

	/*
	 * 1. The clip-hull box must not reach the corner samples at all.  Naming
	 *    any of these in live code before the corner loop is the regression.
	 */
	{
		static const char *const banned[] =
		{
			"clip_mins", "clip_maxs", "wclip_hull", "->hulls["
		};
		unsigned int i;

		for (i = 0; i < sizeof(banned) / sizeof(banned[0]); i++)
		{
			if (CountIn(head, end, banned[i]) != 0)
			{
				fprintf(stderr, "checkbottom: %s: SV_CheckBottom "
					"references `%s' in live code before the corner "
					"loop.  The corner box must be the entity's own "
					"bbox, not a world clip hull -- see issue #278.\n",
					path, banned[i]);
				failed = 1;
			}
		}
	}

	/*
	 * 2. mins and maxs are each written exactly once before the corner loop,
	 *    and from the entity's own bbox.  This is the invariant the bug broke:
	 *    the first pair of VectorAdds was correct the whole time and a second
	 *    pair overwrote them.
	 */
	n = CountIn(head, end, "VectorAdd (ent->v.origin, ent->v.mins, mins);");
	if (n != 1)
	{
		fprintf(stderr, "checkbottom: %s: expected exactly 1 "
			"`VectorAdd (ent->v.origin, ent->v.mins, mins);' before the "
			"corner loop, found %d\n", path, n);
		failed = 1;
	}

	n = CountIn(head, end, "VectorAdd (ent->v.origin, ent->v.maxs, maxs);");
	if (n != 1)
	{
		fprintf(stderr, "checkbottom: %s: expected exactly 1 "
			"`VectorAdd (ent->v.origin, ent->v.maxs, maxs);' before the "
			"corner loop, found %d\n", path, n);
		failed = 1;
	}

	/*
	 * 3. Nothing else assigns mins or maxs in between.  Catches a revival
	 *    written with different spelling -- VectorCopy, a bare index write, or
	 *    another VectorAdd from some other source.
	 */
	if (CountIn(head, end, ", mins);") != 1 ||
	    CountIn(head, end, ", maxs);") != 1)
	{
		fprintf(stderr, "checkbottom: %s: SV_CheckBottom writes mins/maxs "
			"more than once before the corner loop (%d/%d writes).  The "
			"corner box must be the entity's own bbox -- see issue #278.\n",
			path, CountIn(head, end, ", mins);"),
			CountIn(head, end, ", maxs);"));
		failed = 1;
	}
	if (CountIn(head, end, "mins[0] =") != 0 ||
	    CountIn(head, end, "mins[1] =") != 0 ||
	    CountIn(head, end, "mins[2] =") != 0 ||
	    CountIn(head, end, "maxs[0] =") != 0 ||
	    CountIn(head, end, "maxs[1] =") != 0 ||
	    CountIn(head, end, "maxs[2] =") != 0)
	{
		fprintf(stderr, "checkbottom: %s: SV_CheckBottom assigns a mins/maxs "
			"component directly before the corner loop -- see issue #278.\n",
			path);
		failed = 1;
	}

	free(text);
	return failed;
}

/*
 * Self-test for the stripper.  A stripper that silently failed open would make
 * every case above pass vacuously, which is the one way this test could lie.
 */
static int CheckStripper (void)
{
	char	buf[] =
		"a /* x */ b // y\n"		/* block then line comment */
		"c \"/* not a comment */\" d '\\''\n"	/* literals are not comments */
		"e /* z\nzz */ f";		/* block comment across a newline */
	static const char expect[] =
		"a" "         " "b" "     " "\n"
		"c \"/* not a comment */\" d '\\''\n"
		"e" "     " "\n" "      " "f";

	StripComments(buf);
	if (strcmp(buf, expect) != 0)
	{
		fprintf(stderr, "checkbottom: comment stripper is wrong.\n"
			"  got:      [%s]\n  expected: [%s]\n", buf, expect);
		return 1;
	}

	return 0;
}

int main (void)
{
	static const char *const copies[] =
	{
		"hexen2/sv_move.c",
		"hexenworld/server/sv_move.c"
	};
	unsigned int i;
	int failed = CheckStripper();

	for (i = 0; i < sizeof(copies) / sizeof(copies[0]); i++)
	{
		if (CheckOneCopy(copies[i]) != 0)
			failed = 1;
	}

	return failed;
}
