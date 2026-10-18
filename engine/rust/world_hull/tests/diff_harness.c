/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/world_hull.rs against the C original of
 * the box hull and SV_HullForEntity, in one binary.
 *
 * run_diff_harness.sh builds this against world_hull_h2.c (-DSERVERONLY, as
 * h2ded compiles it; archive feature world_hull) and against world_hull_hw.c
 * (-DH2W -DSERVERONLY -DWT_HW, as hwsv; world_hull plus world_trace_hw).  The
 * binary also compiles the real engine/rust/world_target.c with the same
 * headers, so the Rust reads its layouts the way it does in the engine --
 * that shim is under test here too.  The original is linked twice: c_* is
 * plain IEEE C, the source's semantics, and the contract; f_* is built with
 * the engine's flags (-ffast-math) and only counted, as in world_trace's
 * harness.
 *
 * Per call, compared byte for byte:
 *   - which hull comes back: the same hull of the same model, or for a
 *     non-brush entity each side's own box hull, whose 6 clipnodes and 6
 *     planes must then be equal;
 *   - the offset written back;
 *   - the inputs (mins, maxs) and the entities, which neither may write;
 *   - every CON_Printf.
 * Fatal paths -- SOLID_BSP without MOVETYPE_PUSH, a SOLID_BSP entity whose
 * model is missing or not a brush -- run in forked children, and the error
 * function (Sys_Error in Hexen II, SV_Error in HexenWorld) and message must
 * match.
 *
 * Inputs walk the size thresholds the hull rules test (3, 8, 28, 32), the
 * `hull` field's whole range (0 for the size rules, 1..8, fractions that
 * truncate toward zero, and -0.5 -- index -1, which reads the bytes just
 * before the hulls array in both), FL_MONSTER among other flag bits, and the world entity's
 * spawnflags bit 0 -- including values past int range, which a map can set
 * and where x86_64's conversion gives INT_MIN.  The harness runs on x86_64,
 * so it checks the port's x86 rule; the saturating rule wasm32 builds use is
 * Rust's own `as` (see world_hull.rs).
 * Outcome floors make sure every rule is reached.  The
 * coverage build requires every line of the original to run, but the two the
 * runner proves unreachable.
 */

#include "quakedef.h"
#include "world_target.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void c_SV_InitBoxHull (void);
hull_t *c_SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs, vec3_t offset, edict_t *move_ent);
extern hull_t		c_box_hull;
extern mclipnode_t	c_box_clipnodes[6];
extern mplane_t		c_box_planes[6];
void f_SV_InitBoxHull (void);
hull_t *f_SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs, vec3_t offset, edict_t *move_ent);
extern hull_t		f_box_hull;

#ifdef WT_HW
#define WANT_ERROR_FN	"SV_Error"
#else
#define WANT_ERROR_FN	"Sys_Error"
#endif

/* the server the shim reads: defined in sv_main.c / sv_init.c in the engine;
 * sv_globals in pr_edict.c (world_target.c's accessors for area linking
 * read it, so it is linked even though hull selection never does) */
server_t	sv;
sv_globals_t	sv_globals;

/* ------------------------------------------------------------------------ */

static char	con_log[2048];
static size_t	con_len;

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list	ap;
	int	n;
	if (con_len >= sizeof (con_log) - 64)
		return;
	n = snprintf (con_log + con_len, sizeof (con_log) - con_len, "[%u]", flags);
	con_len += (size_t) n;
	va_start (ap, fmt);
	n = vsnprintf (con_log + con_len, sizeof (con_log) - con_len, fmt, ap);
	va_end (ap);
	if (n > 0)
		con_len += (size_t) n;
	if (con_len >= sizeof (con_log))
		con_len = sizeof (con_log) - 1;
}

static int	err_fd = -1;

static void fatal_report (const char *who, const char *fmt, va_list ap)
{
	char	buf[512];
	int	n = snprintf (buf, sizeof (buf), "%s|", who);
	vsnprintf (buf + n, sizeof (buf) - (size_t) n, fmt, ap);
	if (err_fd < 0)
	{
		fprintf (stderr, "harness bug: %s outside a child: %s\n", who, buf);
		abort ();
	}
	if (write (err_fd, buf, strlen (buf)) < 0)
		_exit (98);
	exit (77);	/* exit, not _exit: the coverage build flushes its counts */
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal_report ("Sys_Error", error, ap);
	va_end (ap);
	exit (99);
}

#ifdef WT_HW
FUNC_NORETURN void SV_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal_report ("SV_Error", error, ap);
	va_end (ap);
	exit (99);
}
#endif

/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0x2545f4914f6cdd1dULL;

static uint32_t rnd (void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t) ((rng * 0x2545f4914f6cdd1dULL) >> 32);
}

static int chance (int pct)	{ return (int) (rnd () % 100) < pct; }
static float frand (float lo, float hi)	{ return lo + (hi - lo) * ((float) rnd () / 4294967296.0f); }

#define NMODELS	8
#define NEDICTS	4

static qmodel_t	models[NMODELS];
static edict_t	edicts[NEDICTS];	/* 0 is the world, whose spawnflags hexen2 reads */

/* a size to straddle the rules: point < 3, pentacle <= 8, half/full <= 32,
 * half also needs size[2] <= 28 */
static float rule_size (void)
{
	static const float	at[] = { 0, 2.9f, 3, 3.1f, 7.9f, 8, 8.1f, 27.9f, 28, 28.1f, 31.9f, 32, 32.1f, 48, 64 };
	return chance (80) ? at[rnd () % (sizeof (at) / sizeof (at[0]))] : frand (0, 80);
}

static void gen_world (void)
{
	int	m, h, i;

	memset (models, 0, sizeof (models));
	memset (&sv, 0, sizeof (sv));
	memset (edicts, 0, sizeof (edicts));
	for (m = 0; m < NMODELS; m++)
	{
		models[m].type = chance (80) ? mod_brush : (chance (50) ? mod_sprite : mod_alias);
		for (h = 0; h < MAX_MAP_HULLS; h++)
		{
			for (i = 0; i < 3; i++)
			{
				models[m].hulls[h].clip_mins[i] = chance (30) ? 0 : (float) ((int) (rnd () % 65) - 32);
				models[m].hulls[h].clip_maxs[i] = frand (0, 64);
			}
			models[m].hulls[h].firstclipnode = (int) (rnd () % 100);
		}
		sv.models[m] = chance (90) ? &models[m] : NULL;
	}
	sv.edicts = edicts;
	edicts[0].v.spawnflags = (float) (rnd () % 4);	/* bit 0: pentacles */
	/* a map can set worldspawn's spawnflags to anything atof reads: past int
	 * range, x86_64's conversion gives INT_MIN (bit 0 clear), and the port
	 * must agree with that, not saturate.  (4294967295 rounds to 2^32 as a
	 * float.) */
	if (chance (5))
		edicts[0].v.spawnflags = chance (50) ? 2147483648.0f : 4294967295.0f;
}

static void gen_edict (edict_t *e, int brush)
{
	/* 0: the size rules.  1..8: hulls 0..7.  Fractions truncate toward
	 * zero, so 0.5 is hull 0 and -0.5 is index -1, the bytes just before
	 * hulls[] inside qmodel_t -- the same memory for both sides. */
	static const float	hulls[] = { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 1.9f, 4.5f, 0.5f, -0.5f, 8.9f };
	int			i;

	if (brush)
	{
		e->v.solid = SOLID_BSP;
		e->v.movetype = chance (95) ? MOVETYPE_PUSH : (float) (rnd () % 15);
	}
	else
	{
		e->v.solid = (float) (rnd () % 4);		/* not SOLID_BSP */
		e->v.movetype = (float) (rnd () % 15);
	}
	e->v.modelindex = (float) (rnd () % NMODELS);
	e->v.hull = hulls[rnd () % (sizeof (hulls) / sizeof (hulls[0]))];
	e->v.flags = (float) ((chance (50) ? FL_MONSTER : 0) | (int) (rnd () & 0x10f));
	if (chance (3))
		e->v.flags = chance (50) ? 3e9f : -3e9f;	/* past int range: INT_MIN on x86_64 */
	for (i = 0; i < 3; i++)
	{
		e->v.origin[i] = frand (-4096, 4096);
		e->v.mins[i] = -frand (0, 64);
		e->v.maxs[i] = frand (0, 64);
	}
}

/* ------------------------------------------------------------------------ */

static long	n_cases, n_fatal, n_fail;
static long	st_box, st_field, st_point, st_pent, st_half, st_full, st_golem, st_monster0;
static long	st_fast_differ, st_wide_flags;
static long	st_fatal_push, st_fatal_model;

static void report (const char *what, long id, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH %s #%ld: %s\n", what, id, detail);
}

/* the Rust's box, found through what it returned for a non-brush entity */
static int same_box (const hull_t *c, const hull_t *r)
{
	hull_t	hc = *c, hr = *r;
	if (memcmp (c->clipnodes, r->clipnodes, 6 * sizeof (mclipnode_t)) != 0 ||
	    memcmp (c->planes, r->planes, 6 * sizeof (mplane_t)) != 0)
		return 0;
	hc.clipnodes = NULL; hc.planes = NULL;
	hr.clipnodes = NULL; hr.planes = NULL;
	return memcmp (&hc, &hr, sizeof (hc)) == 0;
}

/* which hull a model-hull pointer names: index into its model's hulls[] */
static long hull_index (const hull_t *h, int m)
{
	return (long) (h - &models[m].hulls[0]);
}

static void classify (const edict_t *ent, const edict_t *mover, const hull_t *h, const vec3_t mins,
		      const vec3_t maxs, const vec3_t offset)
{
	vec3_t	size;
	if (h == &c_box_hull) { st_box++; return; }
	if (mover->v.hull) { st_field++; }
	else
	{
		long	i = hull_index (h, (int) ent->v.modelindex);
		VectorSubtract (maxs, mins, size);
		/* the pentacle decision, with spawnflags past int range */
		if (size[0] >= 3 && size[0] <= 8 && (edicts[0].v.spawnflags >= 2147483648.0f))
			st_wide_flags++;
		if (i == 0) st_point++;
		else if (i == 4) st_pent++;
		else if (i == 3) st_half++;
		else if (i == 1) st_full++;
		else if (i == 5) st_golem++;
	}
	if (((int) mover->v.flags & FL_MONSTER) && offset[0] == ent->v.origin[0] && offset[1] == ent->v.origin[1])
		st_monster0++;
}

static void compare_case (edict_t *ent, edict_t *mover, const vec3_t mins, const vec3_t maxs)
{
	vec3_t		mc, xc, oc, mr, xr, orr, mf, xf, of;
	hull_t		*hc, *hr, *hf;
	edict_t		before[NEDICTS];
	char		log_c[sizeof (con_log)], log_r[sizeof (con_log)];
	char		msg[512];

	n_cases++;
	memcpy (before, edicts, sizeof (edicts));
	VectorCopy (mins, mc); VectorCopy (maxs, xc);
	VectorCopy (mins, mr); VectorCopy (maxs, xr);
	VectorCopy (mins, mf); VectorCopy (maxs, xf);
	memset (oc, 0x5a, sizeof (oc)); memset (orr, 0x5a, sizeof (orr)); memset (of, 0x5a, sizeof (of));

	con_len = 0; con_log[0] = 0;
	hc = c_SV_HullForEntity (ent, mc, xc, oc, mover);
	memcpy (log_c, con_log, con_len + 1);
	con_len = 0; con_log[0] = 0;
	hr = SV_HullForEntity (ent, mr, xr, orr, mover);
	memcpy (log_r, con_log, con_len + 1);
	con_len = 0; con_log[0] = 0;
	hf = f_SV_HullForEntity (ent, mf, xf, of, mover);

	if (hc == &c_box_hull)
	{
		if (hr == hc || !same_box (hc, hr))
			report ("hull", n_cases, "C returned its box; Rust's box differs or is not a box");
	}
	else if (hr != hc)
	{
		snprintf (msg, sizeof (msg), "C returned hull %ld, Rust %p (C %p)",
			  hull_index (hc, (int) ent->v.modelindex), (void *) hr, (void *) hc);
		report ("hull", n_cases, msg);
	}
	if (memcmp (oc, orr, sizeof (oc)) != 0)
	{
		snprintf (msg, sizeof (msg), "offset C (%a %a %a) Rust (%a %a %a)",
			  oc[0], oc[1], oc[2], orr[0], orr[1], orr[2]);
		report ("offset", n_cases, msg);
	}
	if (strcmp (log_c, log_r) != 0)
	{
		snprintf (msg, sizeof (msg), "console C \"%.200s\" Rust \"%.200s\"", log_c, log_r);
		report ("console", n_cases, msg);
	}
	if (memcmp (mc, mins, sizeof (vec3_t)) || memcmp (xc, maxs, sizeof (vec3_t)) ||
	    memcmp (mr, mins, sizeof (vec3_t)) || memcmp (xr, maxs, sizeof (vec3_t)))
		report ("inputs", n_cases, "an implementation wrote mins or maxs");
	if (memcmp (before, edicts, sizeof (edicts)) != 0)
		report ("edicts", n_cases, "an implementation wrote an entity");

	/* the engine-flags build: counted, not held */
	if ((hf == &f_box_hull) != (hc == &c_box_hull) || (hf != &f_box_hull && hf != hc) ||
	    memcmp (of, oc, sizeof (of)) != 0)
		st_fast_differ++;

	classify (ent, mover, hc, mins, maxs, oc);
}

/* ------------------------------------------------------------------------ */

static void run_child (int rust, edict_t *ent, edict_t *mover, const vec3_t mins, const vec3_t maxs,
		       char *out, size_t outsz)
{
	int	fds[2], status;
	pid_t	pid;
	ssize_t	n;
	size_t	got = 0;

	if (pipe (fds) != 0) { perror ("pipe"); exit (2); }
	fflush (NULL);
	pid = fork ();
	if (pid < 0) { perror ("fork"); exit (2); }
	if (pid == 0)
	{
		vec3_t	a, b, o;
		hull_t	*h;
		char	buf[200];
		close (fds[0]);
		err_fd = fds[1];
		VectorCopy (mins, a); VectorCopy (maxs, b);
		h = rust ? SV_HullForEntity (ent, a, b, o, mover) : c_SV_HullForEntity (ent, a, b, o, mover);
		snprintf (buf, sizeof (buf), "RETURNED|%s", h == &c_box_hull ? "box" : "model");
		if (write (err_fd, buf, strlen (buf)) < 0)
			_exit (98);
		exit (0);
	}
	close (fds[1]);
	while (got + 1 < outsz && (n = read (fds[0], out + got, outsz - 1 - got)) > 0)
		got += (size_t) n;
	out[got] = 0;
	close (fds[0]);
	if (waitpid (pid, &status, 0) < 0) { perror ("waitpid"); exit (2); }
	if (!WIFEXITED (status) || (WEXITSTATUS (status) != 0 && WEXITSTATUS (status) != 77))
		snprintf (out, outsz, "CRASHED|status %d", status);
}

static void compare_fatal (edict_t *ent, edict_t *mover, const vec3_t mins, const vec3_t maxs, int want_fatal)
{
	char	oc[512], orr[512], msg[1100];

	n_fatal++;
	run_child (0, ent, mover, mins, maxs, oc, sizeof (oc));
	run_child (1, ent, mover, mins, maxs, orr, sizeof (orr));
	/* the Rust returns its own box, never c_box_hull; a returned box shows
	 * up as "model" there, so compare fatal outcomes only */
	if (strncmp (oc, "RETURNED|", 9) == 0 && strncmp (orr, "RETURNED|", 9) == 0)
	{
		if (want_fatal)
			report ("fatal", n_fatal, "neither implementation reported the fatal case");
		return;
	}
	if (strcmp (oc, orr) != 0)
	{
		snprintf (msg, sizeof (msg), "C \"%s\" Rust \"%s\"", oc, orr);
		report ("fatal", n_fatal, msg);
		return;
	}
	if (strncmp (oc, WANT_ERROR_FN "|", strlen (WANT_ERROR_FN) + 1) != 0)
	{
		snprintf (msg, sizeof (msg), "both report \"%s\", want %s", oc, WANT_ERROR_FN);
		report ("fatal", n_fatal, msg);
		return;
	}
	if (strstr (oc, "without MOVETYPE_PUSH")) st_fatal_push++;
	if (strstr (oc, "non bsp model")) st_fatal_model++;
}

static void floor_check (const char *name, long have, long want)
{
	if (have < want)
	{
		fprintf (stderr, "COVERAGE: %s reached %ld time(s), floor %ld\n", name, have, want);
		n_fail++;
	}
}

int main (void)
{
	int	iw, k, i;
	vec3_t	mins, maxs;

	/* the layouts the Rust reads through the shim, against this build's own */
	if (World_TargetFacts.edict_v != offsetof (edict_t, v) ||
	    World_TargetFacts.model_hulls != offsetof (qmodel_t, hulls) ||
	    World_TargetFacts.hull_size != sizeof (hull_t) ||
	    World_TargetFacts.solid_bsp != SOLID_BSP)
	{
		fprintf (stderr, "harness bug: world_target.c is not this build's\n");
		return 2;
	}

	/* the box hull right after SV_InitBoxHull, before any use */
	c_SV_InitBoxHull ();
	SV_InitBoxHull ();
	f_SV_InitBoxHull ();

	for (iw = 0; iw < 4000; iw++)
	{
		gen_world ();
		for (k = 0; k < 40; k++)
		{
			edict_t	*ent = &edicts[1 + (int) (rnd () % (NEDICTS - 1))];
			edict_t	*mover = &edicts[1 + (int) (rnd () % (NEDICTS - 1))];
			int	brush = chance (60);

			gen_edict (ent, brush);
			if (mover != ent)
				gen_edict (mover, chance (50));
			if (brush && chance (95))
			{
				/* keep the fatal cases for the children */
				ent->v.movetype = MOVETYPE_PUSH;
				if (!sv.models[(int) ent->v.modelindex] || sv.models[(int) ent->v.modelindex]->type != mod_brush)
				{
					models[(int) ent->v.modelindex].type = mod_brush;
					sv.models[(int) ent->v.modelindex] = &models[(int) ent->v.modelindex];
				}
			}
			else if (brush)
				continue;	/* a fatal shape: the fatal phase covers it */
			for (i = 0; i < 3; i++)
			{
				float	s = rule_size ();
				mins[i] = chance (50) ? -s / 2 : -frand (0, s);
				maxs[i] = mins[i] + s;
				if (chance (10)) { mins[i] = 0; maxs[i] = 0; }	/* CheckBottom's '0 0 0' */
			}
			compare_case (ent, mover, mins, maxs);
		}
	}

	/* fatal phase */
	for (iw = 0; iw < 300; iw++)
	{
		edict_t	*ent = &edicts[1], *mover = &edicts[2];
		int	which = (int) (rnd () % 3);
		gen_world ();
		gen_edict (ent, 1);
		gen_edict (mover, 0);
		for (i = 0; i < 3; i++) { mins[i] = -16; maxs[i] = 16; }
		if (which == 0)
			ent->v.movetype = MOVETYPE_STEP;			/* SOLID_BSP without MOVETYPE_PUSH */
		else
		{
			ent->v.movetype = MOVETYPE_PUSH;
			if (which == 1)
				sv.models[(int) ent->v.modelindex] = NULL;	/* no model */
			else
			{
				models[(int) ent->v.modelindex].type = mod_alias;	/* not a brush */
				sv.models[(int) ent->v.modelindex] = &models[(int) ent->v.modelindex];
			}
		}
		compare_fatal (ent, mover, mins, maxs, 1);
	}

	printf ("outcomes: box %ld, hull field %ld, point %ld, pentacle %ld, half %ld, full %ld, golem %ld,\n"
		"          FL_MONSTER offsets zeroed %ld; fatal: without PUSH %ld, non-bsp model %ld;\n"
		"          size-rule decisions with spawnflags past int range %ld;\n"
		"          -ffast-math C differs (counted, not held) in %ld\n",
		st_box, st_field, st_point, st_pent, st_half, st_full, st_golem, st_monster0,
		st_fatal_push, st_fatal_model, st_wide_flags, st_fast_differ);

	floor_check ("the box hull", st_box, 10000);
	floor_check ("the hull field", st_field, 10000);
	floor_check ("the point hull", st_point, 1000);
#ifndef WT_HW
	floor_check ("the pentacle hull", st_pent, 200);
	floor_check ("an FL_MONSTER offset zeroed", st_monster0, 500);
#else
	if (st_pent)
	{
		fprintf (stderr, "HexenWorld has no pentacle rule, yet %ld moves chose hull 4 by size\n", st_pent);
		n_fail++;
	}
#endif
	floor_check ("the half-player hull", st_half, 1000);
	floor_check ("a size-rule decision with spawnflags past int range", st_wide_flags, 50);
	floor_check ("the player hull", st_full, 1000);
	floor_check ("the golem hull", st_golem, 1000);
	floor_check ("SOLID_BSP without MOVETYPE_PUSH", st_fatal_push, 50);
	floor_check ("a SOLID_BSP entity without a brush model", st_fatal_model, 100);

	if (n_fail)
	{
		printf ("world_hull differential harness (%s): FAIL, %ld problem(s) in %ld cases, %ld fatal\n",
			WANT_ERROR_FN, n_fail, n_cases, n_fatal);
		return 1;
	}
	printf ("world_hull differential harness (%s): PASS (%ld cases: %ld calls, %ld fatal)\n",
		WANT_ERROR_FN, n_cases + n_fatal, n_cases, n_fatal);
	return 0;
}
