/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/world_move.rs against the C original of
 * SV_Move and the rest of world.c, in one binary.
 *
 * run_diff_harness.sh builds this against world_move_h2.c (-DSERVERONLY, as
 * h2ded compiles it) and world_move_hw.c (-DH2W -DSERVERONLY -DWT_HW, as
 * hwsv).  The real engine/rust/world_target.c is compiled in.
 *
 * Each world is built as the engine has it when a move runs: a world model
 * and three brush models, each with all eight clip hulls as random clipnode
 * trees; sv.models[1] the world, the world edict SOLID_BSP / MOVETYPE_PUSH;
 * and entities -- boxes, slideboxes, rotated brush entities, triggers,
 * SOLID_NOT, Hexen II's SOLID_GHOST, monsters, owners, point-sized movers --
 * linked into the area nodes by SV_LinkEdict, already ported and checked by
 * world_link's harness.  These functions only read the world, so each query
 * runs the C and the Rust on it in turn, and compares:
 *   - SV_Move's whole trace_t, returned by value, byte for byte, for every
 *     move type (always with a passedict: the world clip reads it);
 *   - SV_PointContents and SV_TestEntityPosition;
 *   - that the world is byte for byte untouched.
 * The fatal path -- a trigger left in a clipping list, which QuakeC makes by
 * changing an entity's solid without relinking it -- runs in forked children,
 * and the error function and message must match (Hexen II's names the
 * classname; HexenWorld's does not).
 *
 * A last phase feeds non-finite input -- NaN and infinite move ends and
 * entity origins, which QuakeC makes with setorigin on 0/0 or 1/0.  A
 * zero-length trace from a NaN origin no longer reaches a single leaf: its
 * distances are NaN, it takes the split path, and can end startsolid with
 * allsolid clear.  Those calls run in timed children (CPU time), so a hang
 * fails instead of stalling the run: both sides must return, or raise the
 * same fatal, with results equal byte for byte (NaN payloads
 * canonicalised).  A hang or a crash on either side fails.
 *
 * WHAT THE REFERENCE CALLS.  The C original's callees -- SV_RecursiveHullCheck,
 * SV_HullPointContents, SV_HullForEntity, SV_LinkEdict, AngleVectors -- are
 * the already-ported Rust functions, each held to its own C by its own
 * harness (world_trace, world_hull, world_link, mathlib).  So this compares
 * SV_Move's own logic over shared callees.  In particular the C trace's spin
 * on a NaN frac cannot happen here, since the trace both sides call is the
 * port's, which takes the "backup past 0" exit; world_trace's harness is
 * where that hang, and the port's exit from it, are proven.
 *
 * The C original is built once, as plain IEEE C, the source's semantics (see
 * world_trace.rs on the engine's -ffast-math build).  The coverage
 * build requires every line of the original to run but the one the runner
 * proves unreachable.
 */

#include "quakedef.h"
#include "world_target.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

trace_t	c_SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict);
int	c_SV_PointContents (vec3_t p);
edict_t	*c_SV_TestEntityPosition (edict_t *ent);
#ifdef WT_HW
#define WANT_ERROR_FN	"SV_Error"
#else
#define WANT_ERROR_FN	"Sys_Error"
#endif

/* the engine's storage */
server_t	sv;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];

/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0xd1b54a32d192ed03ULL;

static uint32_t rnd (void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t) ((rng * 0x2545f4914f6cdd1dULL) >> 32);
}

static int chance (int pct)	{ return (int) (rnd () % 100) < pct; }
static float frand (float lo, float hi)	{ return lo + (hi - lo) * ((float) rnd () / 4294967296.0f); }

/* ------------------------------------------------------------------------ */
/* engine stubs                                                             */
/* ------------------------------------------------------------------------ */

static char	con_log[1024];

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	(void) flags; (void) fmt;	/* nothing in these functions prints */
	snprintf (con_log, sizeof (con_log), "printed");
}

void PR_ExecuteProgram (func_t fnum, const char *funcname)
{
	(void) fnum; (void) funcname;	/* entities are linked without touching */
}

#define NE	24

static edict_t	edicts[NE];
static float	ghost_damage[NE];
static int	ghost_has[NE];

const char *PR_GetString (int num)
{
	static char	buf[4][32];
	static int	k;
	k = (k + 1) & 3;
	snprintf (buf[k], sizeof (buf[k]), "class_%d", num);
	return buf[k];
}

eval_t *GetEdictFieldValue (edict_t *ed, const char *field)
{
	int	i = (int) (ed - edicts);
	if (strcmp (field, "ghost_damage") != 0 || i < 0 || i >= NE || !ghost_has[i])
		return NULL;
	return (eval_t *) &ghost_damage[i];
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
	exit (77);
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
/* the world                                                                */
/* ------------------------------------------------------------------------ */

#define NMODELS		5	/* 0 unused, 1 the world, 2..4 brush entities */
#define NCLIP		40
#define NPLANES		24

static qmodel_t		models[NMODELS];
static mclipnode_t	clipnodes[NMODELS][MAX_MAP_HULLS][NCLIP];
static mplane_t		planes[NMODELS][NPLANES];
static mleaf_t		one_leaf;		/* the world's render tree: one empty leaf */

static void gen_plane (mplane_t *p)
{
	memset (p, 0, sizeof (*p));
	if (chance (70))
	{
		p->type = (byte) (rnd () % 3);
		p->normal[p->type] = 1;
		p->dist = chance (50) ? (float) ((int) (rnd () % 129) - 64) : frand (-512, 512);
	}
	else
	{
		float	l;
		p->normal[0] = frand (-1, 1); p->normal[1] = frand (-1, 1); p->normal[2] = frand (-1, 1);
		l = sqrtf (p->normal[0] * p->normal[0] + p->normal[1] * p->normal[1] + p->normal[2] * p->normal[2]);
		if (l == 0) { p->normal[0] = 1; l = 1; }
		p->normal[0] /= l; p->normal[1] /= l; p->normal[2] /= l;
		p->type = (byte) (3 + rnd () % 3);
		p->dist = frand (-256, 256);
	}
}

static int leaf_contents (void)
{
	static const int	c[] = { CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_SOLID,
					CONTENTS_SOLID, CONTENTS_WATER, CONTENTS_CURRENT_0, CONTENTS_CURRENT_DOWN };
	return c[rnd () % 8];
}

static void gen_hull (int m, int h)
{
	hull_t	*hull = &models[m].hulls[h];
	int	n = 1 + (int) (rnd () % (NCLIP - 1)), i, k;

	hull->clipnodes = clipnodes[m][h];
	hull->planes = planes[m];
	hull->firstclipnode = 0;
	hull->lastclipnode = n - 1;
	for (i = 0; i < n; i++)
	{
		clipnodes[m][h][i].planenum = (int) (rnd () % NPLANES);
		for (k = 0; k < 2; k++)
			clipnodes[m][h][i].children[k] = (i < n - 1 && chance (60))
				? i + 1 + (int) (rnd () % (unsigned) (n - 1 - i)) : leaf_contents ();
	}
	for (k = 0; k < 3; k++)
	{
		hull->clip_mins[k] = (h == 0) ? 0 : -(float) (8 + rnd () % 24);
		hull->clip_maxs[k] = (h == 0) ? 0 : (float) (8 + rnd () % 40);
	}
}

static void gen_world (void)
{
	int	m, h, i;

	memset (models, 0, sizeof (models));
	memset (&sv, 0, sizeof (sv));
	memset (edicts, 0, sizeof (edicts));
	for (m = 1; m < NMODELS; m++)
	{
		models[m].type = mod_brush;
		for (i = 0; i < NPLANES; i++)
			gen_plane (&planes[m][i]);
		for (h = 0; h < MAX_MAP_HULLS; h++)
			gen_hull (m, h);
		sv.models[m] = &models[m];
	}
	/* the world model also needs bounds and a render tree for linking */
	one_leaf.contents = CONTENTS_EMPTY;
	models[1].nodes = (mnode_t *) &one_leaf;
	models[1].leafs = &one_leaf;
	for (i = 0; i < 3; i++)
	{
		models[1].mins[i] = -1024;
		models[1].maxs[i] = 1024;
	}
	sv.worldmodel = &models[1];
	sv.edicts = edicts;
	sv.num_edicts = NE;
	sv_globals.self = sv_globals.other = NULL;

	/* the world entity, as the engine spawns it */
	edicts[0].v.solid = SOLID_BSP;
	edicts[0].v.movetype = MOVETYPE_PUSH;
	edicts[0].v.modelindex = 1;
	edicts[0].v.spawnflags = (float) (rnd () & 1);
}

static void gen_edicts (void)
{
	int	i, k;

	SV_ClearWorld ();
	for (i = 1; i < NE; i++)
	{
		edict_t	*e = &edicts[i];
		int	kind = (int) (rnd () % 8);

		e->free = chance (5);
		switch (kind)
		{
		case 0: e->v.solid = SOLID_NOT; break;
		case 1: e->v.solid = SOLID_TRIGGER; break;
		case 2: case 3: e->v.solid = SOLID_BBOX; break;
		case 4: e->v.solid = SOLID_SLIDEBOX; break;
#ifndef WT_HW
		case 5: e->v.solid = SOLID_GHOST; break;
#endif
		default:
			e->v.solid = SOLID_BSP;
			e->v.movetype = MOVETYPE_PUSH;
			e->v.modelindex = (float) (2 + rnd () % 3);
			break;
		}
		e->v.flags = chance (35) ? FL_MONSTER : 0;
		e->v.classname = 1000 + i;
		e->v.owner = chance (20) ? (int) (rnd () % NE) * (int) sizeof (edict_t) : 0;
		for (k = 0; k < 3; k++)
		{
			float	half = chance (10) ? 0 : frand (4, 48);
			e->v.origin[k] = chance (50) ? (float) ((int) (rnd () % 257) - 128) : frand (-200, 200);
			e->v.mins[k] = -half;
			e->v.maxs[k] = half;
			e->v.size[k] = e->v.maxs[k] - e->v.mins[k];
			if (e->v.solid == SOLID_BSP && chance (30))
			{
				/* rotated, or exactly at the C's `fabs (angle) > 1` edge */
				static const float	edge[] = { 1, -1, 1.0000001f, -1.0000001f, 0.9999999f, 2, -2 };
				e->v.angles[k] = chance (40) ? edge[rnd () % 7] : frand (-180, 180);
			}
			else
				e->v.angles[k] = 0;
		}
		ghost_has[i] = chance (60);
		ghost_damage[i] = chance (50) ? 0 : frand (1, 50);
		SV_LinkEdict (e, false);
		/* QuakeC's `self.solid = SOLID_NOT` without a relink: the entity
		 * stays in its clipping list, and SV_ClipToLinks must skip it */
		if (chance (6))
			e->v.solid = SOLID_NOT;
	}
}

/* ------------------------------------------------------------------------ */
/* comparison                                                               */
/* ------------------------------------------------------------------------ */

static long	n_moves, n_points, n_tests, n_fatal, n_fail;
static long	st_type[5], st_hit_ent, st_hit_world, st_startsolid, st_allsolid, st_water_cut;
static long	st_rotated_hit, st_ghost_hit, st_tep_stuck;

static void report (const char *what, long id, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH %s #%ld: %s\n", what, id, detail);
}

static void gen_point (vec3_t p)
{
	int	k;
	for (k = 0; k < 3; k++)
		p[k] = chance (50) ? (float) ((int) (rnd () % 401) - 200) : frand (-250, 250);
}

static void gen_box (vec3_t mins, vec3_t maxs)
{
	int	k;
	int	point = chance (25);
	for (k = 0; k < 3; k++)
	{
		float	half = point ? 0 : frand (2, 32);
		mins[k] = -half;
		maxs[k] = half * frand (0.5f, 1.8f);
	}
}

static void compare_move (edict_t *pass, int type)
{
	vec3_t	s, e, mn, mx, s2, e2, mn2, mx2;
	trace_t	tc, tr;
	char	msg[400];
	static edict_t	before[NE];

	n_moves++;
	gen_point (s);
	gen_point (e);
	if (chance (20)) VectorCopy (s, e);
	gen_box (mn, mx);
	VectorCopy (s, s2); VectorCopy (e, e2); VectorCopy (mn, mn2); VectorCopy (mx, mx2);
	memcpy (before, edicts, sizeof (edicts));

	tc = c_SV_Move (s, mn, mx, e, type, pass);
	tr = SV_Move (s2, mn2, mx2, e2, type, pass);

	if (memcmp (&tc, &tr, sizeof (tc)) != 0)
	{
		snprintf (msg, sizeof (msg), "type %d trace differs: frac C %a Rust %a, ent C %ld Rust %ld, startsolid %d/%d",
			  type, tc.fraction, tr.fraction,
			  tc.ent ? (long) (tc.ent - edicts) : -1L, tr.ent ? (long) (tr.ent - edicts) : -1L,
			  tc.startsolid, tr.startsolid);
		report ("move", n_moves, msg);
	}
	if (memcmp (before, edicts, sizeof (edicts)) != 0)
		report ("move", n_moves, "an implementation wrote the world");

	st_type[type]++;
	if (tc.ent == sv.edicts) st_hit_world++;
	else if (tc.ent) st_hit_ent++;
	if (tc.startsolid) st_startsolid++;
	if (tc.allsolid) st_allsolid++;
	if (type == MOVE_WATER && tc.fraction == 0) st_water_cut++;
	if (tc.ent && tc.ent != sv.edicts && tc.ent->v.solid == SOLID_BSP &&
	    (tc.ent->v.angles[0] || tc.ent->v.angles[1] || tc.ent->v.angles[2]) && tc.fraction < 1)
		st_rotated_hit++;
#ifndef WT_HW
	if (tc.ent && tc.ent->v.solid == SOLID_GHOST)
		st_ghost_hit++;
#endif
}

static void compare_point (void)
{
	vec3_t	p, q;
	int	c, r;
	char	msg[160];

	n_points++;
	gen_point (p);
	VectorCopy (p, q);
	c = c_SV_PointContents (p);
	r = SV_PointContents (q);
	if (c != r)
	{
		snprintf (msg, sizeof (msg), "contents C %d Rust %d at (%a %a %a)", c, r, p[0], p[1], p[2]);
		report ("point", n_points, msg);
	}
}

static void compare_tests (edict_t *ent)
{
	edict_t	*c, *r;
	char	msg[160];

	n_tests++;
	c = c_SV_TestEntityPosition (ent);
	r = SV_TestEntityPosition (ent);
	if (c != r)
	{
		snprintf (msg, sizeof (msg), "SV_TestEntityPosition C %p Rust %p", (void *) c, (void *) r);
		report ("test", n_tests, msg);
	}
	if (c) st_tep_stuck++;
}

/* ------------------------------------------------------------------------ */
/* the non-finite phase                                                     */
/* ------------------------------------------------------------------------ */

static float odd_value (void)
{
	switch (rnd () % 4)
	{
	case 0: case 1: return NAN;
	case 2: return INFINITY;
	default: return -INFINITY;
	}
}

static void canon_nan (float *f)
{
	if (isnan (*f)) { uint32_t q = 0x7fc00000u; memcpy (f, &q, sizeof (q)); }
}

static void canon_trace (trace_t *t)
{
	int	i;
	canon_nan (&t->fraction);
	canon_nan (&t->plane.dist);
	for (i = 0; i < 3; i++) { canon_nan (&t->endpos[i]); canon_nan (&t->plane.normal[i]); }
}

enum { ODD_MOVE, ODD_TEST };

static int	odd_corrupt = -1;	/* an entity whose abs box goes non-finite in the child */

/* one call in a child under a timer: "RETURNED|<bytes>", "HUNG" or "CRASHED" */
static void odd_child (int rust, int kind, edict_t *pass, int type, const vec3_t s0, const vec3_t e0,
		       const vec3_t mn0, const vec3_t mx0, edict_t *ent, const vec3_t origin, char *out, size_t outsz)
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
		struct itimerval	it;
		char			buf[400];
		size_t			o;
		const unsigned char	*b;
		size_t			i;

		close (fds[0]);
		err_fd = fds[1];
		/* CPU time, not wall-clock: a loaded machine must not turn a slow
		 * return into a "hang".  A call takes microseconds. */
		memset (&it, 0, sizeof (it));
		it.it_value.tv_sec = 1;
		setitimer (ITIMER_PROF, &it, NULL);
		if (odd_corrupt >= 0)
		{
			/* a linked entity whose abs box QuakeC made non-finite */
			edicts[odd_corrupt].v.absmin[0] = NAN;
			edicts[odd_corrupt].v.absmax[2] = INFINITY;
		}
		if (kind == ODD_MOVE)
		{
			vec3_t	s, e, mn, mx;
			trace_t	t;
			VectorCopy (s0, s); VectorCopy (e0, e); VectorCopy (mn0, mn); VectorCopy (mx0, mx);
			t = rust ? SV_Move (s, mn, mx, e, type, pass) : c_SV_Move (s, mn, mx, e, type, pass);
			canon_trace (&t);
			o = (size_t) snprintf (buf, sizeof (buf), "RETURNED|");
			for (b = (const unsigned char *) &t, i = 0; i < offsetof (trace_t, ent) && o + 3 < sizeof (buf); i++)
				o += (size_t) snprintf (buf + o, sizeof (buf) - o, "%02x", b[i]);
			snprintf (buf + o, sizeof (buf) - o, "|ent %ld", t.ent ? (long) (t.ent - edicts) : -1L);
		}
		else
		{
			edict_t	*r;
			VectorCopy (origin, ent->v.origin);	/* QuakeC's setorigin, unlinked */
			r = rust ? SV_TestEntityPosition (ent) : c_SV_TestEntityPosition (ent);
			snprintf (buf, sizeof (buf), "RETURNED|%ld", r ? (long) (r - edicts) : -1L);
		}
		memset (&it, 0, sizeof (it));
		setitimer (ITIMER_PROF, &it, NULL);	/* off before writing and exiting */
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
	if (WIFSIGNALED (status) && WTERMSIG (status) == SIGPROF)
		snprintf (out, outsz, "HUNG");
	else if (!WIFEXITED (status) || (WEXITSTATUS (status) != 0 && WEXITSTATUS (status) != 77))
		snprintf (out, outsz, "CRASHED|status %d", status);
}

static long	n_odd, st_odd_returned, st_odd_tests, st_odd_stuck;

static void compare_odd (int kind)
{
	vec3_t	s, e, mn, mx, origin;
	char	oc[512], orr[512], msg[1100];
	edict_t	*pass = &edicts[1 + (int) (rnd () % (NE - 1))];
	edict_t	*ent = &edicts[1 + (int) (rnd () % (NE - 1))];
	int	type = (int) (rnd () % 5);

	n_odd++;
	gen_point (s); gen_point (e); gen_box (mn, mx); gen_point (origin);
	if (kind == ODD_MOVE)
	{
		if (chance (50)) s[rnd () % 3] = odd_value (); else e[rnd () % 3] = odd_value ();
		if (chance (30)) VectorCopy (s, e);	/* zero-length, from a non-finite point */
		if (chance (15)) { mn[rnd () % 3] = odd_value (); }	/* a non-finite mover box */
	}
	else
		origin[rnd () % 3] = odd_value ();
	odd_corrupt = chance (15) ? 1 + (int) (rnd () % (NE - 1)) : -1;
	odd_child (0, kind, pass, type, s, e, mn, mx, ent, origin, oc, sizeof (oc));
	odd_child (1, kind, pass, type, s, e, mn, mx, ent, origin, orr, sizeof (orr));
	odd_corrupt = -1;

	if (strncmp (oc, "CRASHED", 7) == 0 || strncmp (orr, "CRASHED", 7) == 0)
	{
		/* nothing here may crash either side: a crash is never agreement */
		snprintf (msg, sizeof (msg), "a crash: C \"%s\" Rust \"%s\"", oc, orr);
		report ("odd", n_odd, msg);
		return;
	}
	if (strcmp (oc, "HUNG") == 0 || strcmp (orr, "HUNG") == 0)
	{
		/* no hang can come from these functions' own code, and the trace
		 * under both is the port's, which always returns (see the header) */
		snprintf (msg, sizeof (msg), "a hang: C \"%.100s\" Rust \"%.100s\"", oc, orr);
		report ("odd", n_odd, msg);
		return;
	}
	/* otherwise the same outcome -- a return, or the same fatal call (a
	 * stray trigger left by the fatal phase) -- byte for byte */
	if (strcmp (oc, orr) != 0)
	{
		snprintf (msg, sizeof (msg), "C \"%.200s\" Rust \"%.200s\"", oc, orr);
		report ("odd", n_odd, msg);
		return;
	}
	if (strncmp (oc, "RETURNED|", 9) != 0)
		return;
	st_odd_returned++;
	if (kind == ODD_TEST)
	{
		st_odd_tests++;
		if (strcmp (oc, "RETURNED|-1") != 0)
			st_odd_stuck++;
	}
}

/* the fatal path, one child per implementation */
static void run_child (int rust, edict_t *pass, int type, const vec3_t s0, const vec3_t e0, char *out, size_t outsz)
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
		vec3_t	s, e, mn = { -16, -16, -16 }, mx = { 16, 16, 16 };
		trace_t	t;
		char	buf[200];
		close (fds[0]);
		err_fd = fds[1];
		VectorCopy (s0, s); VectorCopy (e0, e);
		t = rust ? SV_Move (s, mn, mx, e, type, pass) : c_SV_Move (s, mn, mx, e, type, pass);
		snprintf (buf, sizeof (buf), "RETURNED|%a|%ld", t.fraction, t.ent ? (long) (t.ent - edicts) : -1L);
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

static long	st_fatal;

static void compare_fatal (void)
{
	char	oc[512], orr[512], msg[1100];
	vec3_t	s = { -200, -200, -200 }, e = { 200, 200, 200 };
	int	i = 1 + (int) (rnd () % (NE - 1));

	/* QuakeC's in-place solid change: a linked solid entity becomes a
	 * trigger without being relinked, so a clipping list now holds one */
	int	j, h;

	edicts[i].free = false;
	if (edicts[i].v.solid == SOLID_NOT || edicts[i].v.solid == SOLID_TRIGGER)
		return;
	/* An allsolid world clip, or an allsolid hit on an earlier entity, ends
	 * the walk before it reaches the trigger -- as the C means it to.  So
	 * this world is opened up (every world hull one node, empty both sides)
	 * and only the converted entity is left linked. */
	for (h = 0; h < MAX_MAP_HULLS; h++)
	{
		models[1].hulls[h].lastclipnode = 0;
		clipnodes[1][h][0].children[0] = CONTENTS_EMPTY;
		clipnodes[1][h][0].children[1] = CONTENTS_EMPTY;
	}
	for (j = 1; j < NE; j++)
		if (j != i)
			SV_UnlinkEdict (&edicts[j]);
	SV_LinkEdict (&edicts[i], false);
	edicts[i].v.solid = SOLID_TRIGGER;
	n_fatal++;
	{
		edict_t	*pass = &edicts[1 + (i % (NE - 1))];	/* another entity: i + 1, wrapping */
		run_child (0, pass, MOVE_NORMAL, s, e, oc, sizeof (oc));
		run_child (1, pass, MOVE_NORMAL, s, e, orr, sizeof (orr));
	}
	if (strcmp (oc, orr) != 0)
	{
		snprintf (msg, sizeof (msg), "C \"%s\" Rust \"%s\"", oc, orr);
		report ("fatal", n_fatal, msg);
		return;
	}
	{
		static const char	want[] = WANT_ERROR_FN "|Trigger in clipping list";
		if (strncmp (oc, want, sizeof (want) - 1) == 0)
			st_fatal++;
		else if (strncmp (oc, "RETURNED|", 9) != 0)
		{
			snprintf (msg, sizeof (msg), "both report \"%s\", want %s(\"Trigger in clipping list...\")", oc, WANT_ERROR_FN);
			report ("fatal", n_fatal, msg);
		}
	}
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
	int	iw, k;

	if (World_TargetFacts.v_owner != offsetof (entvars_t, owner) ||
	    World_TargetFacts.v_size != offsetof (entvars_t, size) ||
	    World_TargetFacts.move_water != MOVE_WATER)
	{
		fprintf (stderr, "harness bug: world_target.c is not this build's\n");
		return 2;
	}

	for (iw = 0; iw < 1500; iw++)
	{
		gen_world ();
		gen_edicts ();
		for (k = 0; k < 60; k++)
		{
			/* never NULL: the world clip hands passedict to
			 * SV_HullForEntity as move_ent, which reads move_ent->v.hull,
			 * so the C itself cannot take a NULL mover; nor does the engine
			 * pass one */
			edict_t	*pass = &edicts[1 + (int) (rnd () % (NE - 1))];
			compare_move (pass, (int) (rnd () % 5));
			compare_point ();
			if (k % 6 == 0)
				compare_tests (&edicts[1 + (int) (rnd () % (NE - 1))]);
		}
		if (iw % 5 == 0)
			compare_fatal ();
		for (k = 0; k < 2; k++)
		{
			compare_odd (ODD_MOVE);
			compare_odd (ODD_TEST);
		}
	}

	printf ("outcomes: moves by type %ld/%ld/%ld/%ld/%ld; hit an entity %ld, the world %ld;\n"
		"          startsolid %ld, allsolid %ld, MOVE_WATER cut %ld, rotated brush hits %ld,\n"
		"          ghost hits %ld; stuck entities %ld; fatal %ld\n"
		"          non-finite: %ld calls, %ld returned on both sides; %ld position tests (%ld stuck)\n",
		st_type[0], st_type[1], st_type[2], st_type[3], st_type[4], st_hit_ent, st_hit_world,
		st_startsolid, st_allsolid, st_water_cut, st_rotated_hit, st_ghost_hit,
		st_tep_stuck, st_fatal, n_odd, st_odd_returned, st_odd_tests, st_odd_stuck);

	for (k = 0; k < 5; k++)
		floor_check ("a move of each type", st_type[k], 10000);
	floor_check ("a move that hits an entity", st_hit_ent, 2000);
	floor_check ("a move that hits the world", st_hit_world, 2000);
	floor_check ("a startsolid move", st_startsolid, 1000);
	floor_check ("an allsolid move", st_allsolid, 500);
	floor_check ("a MOVE_WATER cut", st_water_cut, 100);
	floor_check ("a hit on a rotated brush entity", st_rotated_hit, 50);
	floor_check ("a stuck entity", st_tep_stuck, 100);
	floor_check ("the trigger-in-clipping-list fatal", st_fatal, 100);
	floor_check ("a non-finite call both sides return from", st_odd_returned, 3000);
	floor_check ("a non-finite SV_TestEntityPosition", st_odd_tests, 1000);
	floor_check ("a non-finite position test that finds the entity stuck", st_odd_stuck, 100);
#ifndef WT_HW
	floor_check ("a SOLID_GHOST hit", st_ghost_hit, 20);
#endif
	(void) con_log;

	if (n_fail)
	{
		printf ("world_move differential harness (%s): FAIL, %ld problem(s) in %ld moves, %ld points, %ld tests, %ld fatal\n",
			WANT_ERROR_FN, n_fail, n_moves, n_points, n_tests, n_fatal);
		return 1;
	}
	printf ("world_move differential harness (%s): PASS (%ld cases: %ld moves, %ld points, %ld tests, %ld fatal, %ld non-finite)\n",
		WANT_ERROR_FN, n_moves + n_points + n_tests + n_fatal + n_odd, n_moves, n_points, n_tests, n_fatal, n_odd);
	return 0;
}
