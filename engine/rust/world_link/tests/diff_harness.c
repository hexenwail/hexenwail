/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/world_link.rs against the C original of
 * world.c's area linking (SV_ClearWorld, SV_LinkEdict, SV_UnlinkEdict and the
 * touch and leaf walks under them), in one binary.
 *
 * run_diff_harness.sh builds this against world_link_h2.c (-DSERVERONLY, as
 * h2ded compiles it) and world_link_hw.c (-DH2W -DSERVERONLY -DWT_HW, as
 * hwsv).  The real engine/rust/world_target.c is compiled in, so the Rust
 * reads this build's layouts the way it does in the engine.
 *
 * Both implementations share the memory they work on, as they would in the
 * engine: the entities, the world model's BSP tree and world.c's
 * sv_areanodes, which stays C storage.  Each scenario -- SV_ClearWorld, then
 * a seeded sequence of moves, links (with and without touching triggers)
 * and unlinks -- runs once with the C, its end state captured; the memory is
 * restored to the same bytes; and it runs again with the Rust.  Every
 * pointer either side writes therefore lands in the same arrays, and the two
 * end states compare as raw bytes:
 *   - every entity, links, abs boxes and leaf lists included;
 *   - all of sv_areanodes;
 *   - sv_globals' self, other and time;
 *   - every call into QuakeC (function, self, other, time, name) in order,
 *     and every console line.
 *
 * PR_ExecuteProgram here stands in for QuakeC, and does what QuakeC may:
 * from a seeded decision it unlinks the toucher or another entity, moves and
 * relinks one, frees one, or relinks one touching triggers itself and then
 * unlinks another -- the nested case where the inner walk takes over
 * sv_link_next, and the outer walk can then meet a NULL link.  The decision
 * depends only on the scenario's seed and the call's position, so both
 * implementations meet the same QuakeC.
 *
 * Each implementation's run of a scenario happens in a forked child under a
 * timer, its end state piped back.  That is not caution for its own sake:
 * when QuakeC relinks the entity a walk will visit next into another node,
 * the walk follows the stale lnext into that node's circular list, reads
 * area-node memory as if it were an entity (EDICT_FROM_AREA of a list head),
 * and never reaches its own head again.  The C hangs or crashes there, and
 * it is undefined behaviour, so such a scenario is counted, not compared;
 * a ceiling on how many keeps the comparison meaningful.  Wherever the C
 * returns, the Rust must return the same end state.
 *
 * The C original is built once, as plain IEEE C: these functions only add,
 * subtract and compare floats, which -ffast-math reads the same way for
 * finite values, and the inputs here are finite.  The coverage build
 * requires every line of the original to run.
 */

#include "quakedef.h"
#include "world_target.h"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void c_SV_ClearWorld (void);
void c_SV_UnlinkEdict (edict_t *ent);
void c_SV_LinkEdict (edict_t *ent, qboolean touch_triggers);
extern link_t	**c_sv_link_next;
extern int	WorldLink_WalkPending (void);	/* the Rust's sv_link_next, set or not */
extern size_t	WorldAreaNodes_len (void);

/* the engine's storage, which both implementations use */
server_t	sv;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];

static int	g_self, g_other;
static float	g_time;

/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0x5851f42d4c957f2dULL;

static uint32_t rnd (void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t) ((rng * 0x2545f4914f6cdd1dULL) >> 32);
}

static int chance (int pct)	{ return (int) (rnd () % 100) < pct; }
static float frand (float lo, float hi)	{ return lo + (hi - lo) * ((float) rnd () / 4294967296.0f); }

static uint32_t mix (uint64_t a, uint64_t b)
{
	uint64_t	x = a * 0x9e3779b97f4a7c15ULL ^ (b + 0x632be59bd9b4e019ULL);
	x ^= x >> 31; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 29;
	return (uint32_t) x;
}

/* ------------------------------------------------------------------------ */
/* the world                                                                */
/* ------------------------------------------------------------------------ */

#define NE		16
#define MAXNODES	128
#define MAXLEAFS	(MAXNODES + 2)

static edict_t	edicts[NE];
static qmodel_t	worldmodel;
static mnode_t	nodes[MAXNODES];
static mleaf_t	leafs[MAXLEAFS];
static mplane_t	planes[MAXNODES];
static int	nnodes, nleafs;

static mnode_t *gen_bsp (int depth, int maxdepth)
{
	mnode_t		*n;
	mplane_t	*p;

	if (depth >= maxdepth || nnodes >= MAXNODES || nleafs >= MAXLEAFS - 1 || (depth > 1 && chance (20)))
	{
		mleaf_t	*l = &leafs[nleafs++];
		static const int	c[] = { CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_EMPTY,
						CONTENTS_SOLID, CONTENTS_WATER, CONTENTS_SLIME };
		l->contents = c[rnd () % 6];
		return (mnode_t *) l;
	}
	n = &nodes[nnodes];
	p = &planes[nnodes];
	nnodes++;
	memset (p, 0, sizeof (*p));
	if (chance (75))
	{
		p->type = (byte) (rnd () % 3);
		p->normal[p->type] = 1;
		/* integer half the time, so a box edge can sit exactly on it */
		p->dist = chance (50) ? (float) ((int) (rnd () % 257) - 128) : frand (-2048, 2048);
	}
	else
	{
		p->normal[0] = frand (-1, 1); p->normal[1] = frand (-1, 1); p->normal[2] = frand (-1, 1);
		{
			float	l = sqrtf (p->normal[0] * p->normal[0] + p->normal[1] * p->normal[1] + p->normal[2] * p->normal[2]);
			if (l == 0) { p->normal[0] = 1; l = 1; }
			p->normal[0] /= l; p->normal[1] /= l; p->normal[2] /= l;
		}
		p->type = (byte) (3 + rnd () % 3);
		p->dist = frand (-1024, 1024);
	}
	p->signbits = (byte) ((p->normal[0] < 0) | ((p->normal[1] < 0) << 1) | ((p->normal[2] < 0) << 2));
	n->contents = 0;
	n->plane = p;
	n->children[0] = gen_bsp (depth + 1, maxdepth);
	n->children[1] = gen_bsp (depth + 1, maxdepth);
	return n;
}

static void gen_world (void)
{
	int	i;
	memset (nodes, 0, sizeof (nodes));
	memset (leafs, 0, sizeof (leafs));
	memset (&worldmodel, 0, sizeof (worldmodel));
	nnodes = 0;
	nleafs = 1;			/* leaf 0 is the generic solid leaf */
	leafs[0].contents = CONTENTS_SOLID;
	worldmodel.nodes = gen_bsp (0, chance (30) ? 8 : 5);
	worldmodel.leafs = leafs;
	for (i = 0; i < 3; i++)
	{
		worldmodel.mins[i] = -frand (256, 4096);
		worldmodel.maxs[i] = frand (256, 4096);
	}
	if (chance (30))
	{
		/* square in x and y: the area tree's axis choice is a tie */
		worldmodel.mins[1] = worldmodel.mins[0];
		worldmodel.maxs[1] = worldmodel.maxs[0];
	}
	memset (&sv, 0, sizeof (sv));
	sv.worldmodel = &worldmodel;
	sv.edicts = edicts;
	sv.time = frand (0, 1000);
	sv_globals.self = &g_self;
	sv_globals.other = &g_other;
	sv_globals.time = &g_time;
	g_self = g_other = 0;
	g_time = 0;
}

static void gen_edict (int i)
{
	edict_t	*e = &edicts[i];
	int	k;

	memset (e, 0, sizeof (*e));
	e->free = chance (8);
	/* NOT, TRIGGER, BBOX, SLIDEBOX, BSP -- triggers often, so walks find work */
	e->v.solid = chance (45) ? SOLID_TRIGGER : (float) (rnd () % 5);
	e->v.touch = chance (70) ? 100 + i : 0;
	e->v.modelindex = chance (20) ? 0 : (float) (1 + rnd () % 3);
	e->v.flags = (float) ((chance (25) ? FL_ITEM : 0) | (chance (30) ? FL_MONSTER : 0));
	if (chance (3))
		e->v.flags = chance (50) ? 3e9f : -3e9f;
	for (k = 0; k < 3; k++)
	{
		float	half = chance (10) ? frand (512, 3000) : frand (4, 64);
		/* mostly a crowd around the middle, where boxes overlap */
		e->v.origin[k] = chance (75) ? frand (-96, 96) : frand (-2000, 2000);
		e->v.mins[k] = -half;
		e->v.maxs[k] = half * frand (0.5f, 1.5f);
		if (chance (50))
		{
			/* integers, so boxes meet exactly at edges and planes */
			e->v.origin[k] = (float) (int) e->v.origin[k];
			e->v.mins[k] = (float) (int) e->v.mins[k];
			e->v.maxs[k] = (float) (int) e->v.maxs[k];
		}
		e->v.angles[k] = chance (20) ? frand (-180, 180) : 0;
	}
}

/* ------------------------------------------------------------------------ */
/* the implementation under test, and QuakeC                                */
/* ------------------------------------------------------------------------ */

typedef struct
{
	void	(*clear) (void);
	void	(*link) (edict_t *, qboolean);
	void	(*unlink) (edict_t *);
} impl_t;

static const impl_t	impl_c = { c_SV_ClearWorld, c_SV_LinkEdict, c_SV_UnlinkEdict };
static const impl_t	impl_rust = { SV_ClearWorld, SV_LinkEdict, SV_UnlinkEdict };
static const impl_t	*cur;

typedef struct
{
	int		fnum, self, other;
	uint32_t	time_bits;
	char		name[8];
} call_t;

#define MAXCALLS	400
static call_t	calls[MAXCALLS];
static int	ncalls, ndepth;
static uint64_t	scenario_seed;
static char	con_log[2048];
static size_t	con_len;

static long	st_calls, st_nested, st_nullink, st_leafcap, st_item, st_rotated, st_free, st_world;

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

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	fprintf (stderr, "harness bug: Sys_Error (%s)\n", error);
	abort ();
}

#ifdef WT_HW
FUNC_NORETURN void SV_Error (const char *error, ...)
{
	fprintf (stderr, "harness bug: SV_Error (%s)\n", error);
	abort ();
}
#endif

static void move_edict (edict_t *e, uint32_t h)
{
	e->v.origin[0] = (float) (int) (h % 257) - 128;
	e->v.origin[1] = (float) (int) ((h >> 12) % 257) - 128;
}

void PR_ExecuteProgram (func_t fnum, const char *funcname)
{
	call_t		*c;
	uint32_t	h;
	edict_t		*a, *b;

	if (ncalls >= MAXCALLS)
		return;			/* both sides stop acting at the same call */
	c = &calls[ncalls++];
	c->fnum = fnum;
	c->self = *sv_globals.self;
	c->other = *sv_globals.other;
	memcpy (&c->time_bits, sv_globals.time, sizeof (float));
	snprintf (c->name, sizeof (c->name), "%s", funcname);

	h = mix (scenario_seed, (uint64_t) ncalls);
	a = &edicts[1 + h % (NE - 1)];
	b = &edicts[1 + (h >> 8) % (NE - 1)];
	switch ((h >> 16) % 100)
	{
	case 0 ... 14:		/* the toucher removes itself */
		cur->unlink ((edict_t *) ((byte *) sv.edicts + *sv_globals.self));
		break;
	case 15 ... 24:		/* another entity goes */
		cur->unlink (a);
		break;
	case 25 ... 31:		/* one moves */
		move_edict (a, h);
		cur->link (a, false);
		break;
	case 32 ... 49:		/* one moves and touches, then another goes */
		if (ndepth == 0)
		{
			ndepth++;
			move_edict (a, h);
			cur->link (a, true);
			cur->unlink (b);
			ndepth--;
			if (cur == &impl_c) st_nested++;
		}
		break;
	case 50 ... 53:		/* one is removed */
		a->free = true;
		cur->unlink (a);
		break;
	case 54 ... 59:		/* one changes solid in place, as QuakeC does (self.solid =
				 * SOLID_NOT in a touch), without relinking: a trigger list
				 * then holds an entity that is no longer a trigger */
		a->v.solid = (h >> 24) & 1 ? SOLID_NOT : SOLID_BBOX;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */

typedef struct
{
	int		e;
	int		kind;		/* 0 link, 1 link + touch, 2 unlink, 3 move + link + touch */
	uint32_t	h;
} op_t;

#define MAXOPS	48

static long	n_scen, n_fail, n_compared, st_ub_hung, st_ub_crashed;

static void report (long id, const char *what)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH scenario #%ld: %s\n", id, what);
}

typedef struct
{
	edict_t		edicts[NE];
	areanode_t	areanodes[AREA_NODES];
	int		self, other;
	float		time;
	call_t		calls[MAXCALLS];
	int		ncalls;
	char		con[sizeof (con_log)];
	long		st[6];		/* the C run's census, from its child */
	int		link_next_set;
} state_t;

static state_t	start, end_c, end_r;

static void save (state_t *s)
{
	memcpy (s->edicts, edicts, sizeof (edicts));
	memcpy (s->areanodes, sv_areanodes, sizeof (sv_areanodes));
	s->self = g_self; s->other = g_other; s->time = g_time;
	memcpy (s->calls, calls, sizeof (calls));
	s->ncalls = ncalls;
	memcpy (s->con, con_log, sizeof (con_log));
}

static void restore (const state_t *s)
{
	memcpy (edicts, s->edicts, sizeof (edicts));
	memcpy (sv_areanodes, s->areanodes, sizeof (sv_areanodes));
	g_self = s->self; g_other = s->other; g_time = s->time;
	memset (calls, 0, sizeof (calls));
	ncalls = 0;
	memset (con_log, 0, sizeof (con_log));
	con_len = 0;
}

static void run (const impl_t *impl, const op_t *ops, int nops)
{
	int	i;
	cur = impl;
	impl->clear ();
	for (i = 0; i < nops; i++)
	{
		edict_t	*e = &edicts[ops[i].e];
		switch (ops[i].kind)
		{
		case 0: impl->link (e, false); break;
		case 1: impl->link (e, true); break;
		case 2: impl->unlink (e); break;
		default: move_edict (e, ops[i].h); impl->link (e, true); break;
		}
		if (impl == &impl_c)
		{
			if (e->num_leafs == MAX_ENT_LEAFS) st_leafcap++;
			if (!e->free && e != sv.edicts && ((int) e->v.flags & FL_ITEM)) st_item++;
			if (!e->free && e->v.solid == SOLID_BSP && (e->v.angles[0] || e->v.angles[1] || e->v.angles[2])) st_rotated++;
			if (e->free) st_free++;
			if (e == sv.edicts) st_world++;
		}
	}
}

enum { RUN_OK, RUN_HUNG, RUN_CRASHED };

/* run() in a child; the end state comes back through a pipe */
static int run_child (const impl_t *impl, const op_t *ops, int nops, state_t *out)
{
	int	fds[2], status;
	pid_t	pid;
	size_t	got = 0;
	ssize_t	n;

	if (pipe (fds) != 0) { perror ("pipe"); exit (2); }
	fflush (NULL);
	pid = fork ();
	if (pid < 0) { perror ("fork"); exit (2); }
	if (pid == 0)
	{
		struct itimerval	it;
		const char		*p;
		size_t			left;

		close (fds[0]);
		memset (&it, 0, sizeof (it));
		it.it_value.tv_sec = 2;		/* a scenario takes microseconds */
		setitimer (ITIMER_REAL, &it, NULL);
		run (impl, ops, nops);
		memset (&it, 0, sizeof (it));
		setitimer (ITIMER_REAL, &it, NULL);
		save (out);
		/* the census and the C's cursor go back with the state */
		out->st[0] = st_leafcap; out->st[1] = st_item; out->st[2] = st_rotated;
		out->st[3] = st_free; out->st[4] = st_world; out->st[5] = st_nested;
		out->link_next_set = impl == &impl_c ? (c_sv_link_next != NULL) : WorldLink_WalkPending ();
		for (p = (const char *) out, left = sizeof (*out); left; )
		{
			n = write (fds[1], p, left);
			if (n <= 0) _exit (98);
			p += n; left -= (size_t) n;
		}
		exit (0);	/* exit, not _exit: the coverage build flushes its counts */
	}
	close (fds[1]);
	while (got < sizeof (*out) && (n = read (fds[0], (char *) out + got, sizeof (*out) - got)) > 0)
		got += (size_t) n;
	close (fds[0]);
	if (waitpid (pid, &status, 0) < 0) { perror ("waitpid"); exit (2); }
	if (WIFSIGNALED (status) && WTERMSIG (status) == SIGALRM)
		return RUN_HUNG;
	/* only a memory fault is the C's undefined walk; anything else (an
	 * abort from a harness stub, a short write) is the harness's own bug */
	if (WIFSIGNALED (status) && (WTERMSIG (status) == SIGSEGV || WTERMSIG (status) == SIGBUS))
		return RUN_CRASHED;
	if (!WIFEXITED (status) || WEXITSTATUS (status) != 0 || got != sizeof (*out))
	{
		fprintf (stderr, "harness bug: a %s child ended with status %d\n",
			 impl == &impl_c ? "C" : "Rust", status);
		exit (2);
	}
	return RUN_OK;
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
	int	iw, i, nops;
	op_t	ops[MAXOPS];
	char	msg[160];

	if (World_TargetFacts.edict_area != offsetof (edict_t, area) ||
	    World_TargetFacts.model_leafs != offsetof (qmodel_t, leafs) ||
	    World_TargetFacts.leaf_size != sizeof (mleaf_t) ||
	    (size_t) World_TargetFacts.area_nodes * sizeof (areanode_t) != sizeof (sv_areanodes) ||
	    WorldAreaNodes_len () != AREA_NODES)
	{
		fprintf (stderr, "harness bug: world_target.c is not this build's\n");
		return 2;
	}

	for (iw = 0; iw < 4000; iw++)
	{
		gen_world ();
		for (i = 0; i < NE; i++)
			gen_edict (i);
		nops = 8 + (int) (rnd () % (MAXOPS - 8));
		for (i = 0; i < nops; i++)
		{
			ops[i].e = chance (5) ? 0 : 1 + (int) (rnd () % (NE - 1));
			ops[i].kind = (int) (rnd () % 4);
			ops[i].h = rnd ();
		}
		scenario_seed = ((uint64_t) rnd () << 32) | rnd ();
		n_scen++;

		/* the fresh world, with empty logs, is the scenario's start */
		memset (calls, 0, sizeof (calls));
		ncalls = 0;
		memset (con_log, 0, sizeof (con_log));
		con_len = 0;
		save (&start);
		{
			int	rc, rr;

			rc = run_child (&impl_c, ops, nops, &end_c);
			if (rc != RUN_OK)
			{
				/* undefined in the C (see the header): counted, not compared */
				if (rc == RUN_HUNG) st_ub_hung++; else st_ub_crashed++;
				continue;
			}
			n_compared++;
			/* the child's census is cumulative: it began from these values */
			st_leafcap = end_c.st[0]; st_item = end_c.st[1]; st_rotated = end_c.st[2];
			st_free = end_c.st[3]; st_world = end_c.st[4]; st_nested = end_c.st[5];
			st_calls += end_c.ncalls;
			if (strstr (end_c.con, "encountered NULL link")) st_nullink++;
			if (end_c.link_next_set)
				report (n_scen, "the C left sv_link_next set");

			restore (&start);
			rr = run_child (&impl_rust, ops, nops, &end_r);
			if (rr != RUN_OK)
			{
				snprintf (msg, sizeof (msg), "the C returned, the Rust %s", rr == RUN_HUNG ? "hung" : "crashed");
				report (n_scen, msg);
				continue;
			}
			if (end_r.link_next_set)
				report (n_scen, "the Rust left its sv_link_next cursor set after the walk");
		}

		if (memcmp (end_c.edicts, end_r.edicts, sizeof (edicts)) != 0)
		{
			for (i = 0; i < NE && !memcmp (&end_c.edicts[i], &end_r.edicts[i], sizeof (edict_t)); i++)
				;
			snprintf (msg, sizeof (msg), "entity %d differs", i);
			report (n_scen, msg);
		}
		if (memcmp (end_c.areanodes, end_r.areanodes, sizeof (sv_areanodes)) != 0)
			report (n_scen, "sv_areanodes differs");
		if (end_c.self != end_r.self || end_c.other != end_r.other ||
		    memcmp (&end_c.time, &end_r.time, sizeof (float)) != 0)
			report (n_scen, "sv_globals self/other/time differ");
		if (end_c.ncalls != end_r.ncalls || memcmp (end_c.calls, end_r.calls, sizeof (calls)) != 0)
		{
			snprintf (msg, sizeof (msg), "QuakeC calls differ (C %d, Rust %d)", end_c.ncalls, end_r.ncalls);
			report (n_scen, msg);
		}
		/* the C's __thisfunc__ names the renamed c_ function */
		{
			char	*p;
			while ((p = strstr (end_c.con, "c_SV_")) != NULL)
				memmove (p, p + 2, strlen (p + 2) + 1);
		}
		if (strcmp (end_c.con, end_r.con) != 0)
		{
			snprintf (msg, sizeof (msg), "console C \"%.60s\" Rust \"%.60s\"", end_c.con, end_r.con);
			report (n_scen, msg);
		}
	}

	printf ("undefined in the C, not compared: %ld hung, %ld crashed (of %ld scenarios)\n",
		st_ub_hung, st_ub_crashed, n_scen);
	printf ("outcomes: %ld scenarios, %ld QuakeC touch calls (%ld nested walks), %ld NULL-link walks,\n"
		"          %ld links capped at MAX_ENT_LEAFS, %ld FL_ITEM boxes, %ld rotated BSP boxes,\n"
		"          %ld links of freed entities, %ld of the world entity\n",
		n_scen, st_calls, st_nested, st_nullink, st_leafcap, st_item, st_rotated, st_free, st_world);

	floor_check ("a compared scenario", n_compared, 3500);
	if ((st_ub_hung + st_ub_crashed) * 10 > n_scen)
	{
		fprintf (stderr, "COVERAGE: %ld of %ld scenarios undefined in the C, over the 10%% ceiling\n",
			 st_ub_hung + st_ub_crashed, n_scen);
		n_fail++;
	}
	floor_check ("a QuakeC touch call", st_calls, 10000);
	floor_check ("a nested touching relink", st_nested, 200);
	floor_check ("a walk meeting a NULL link", st_nullink, 5);
	floor_check ("a link capped at MAX_ENT_LEAFS", st_leafcap, 20);
	floor_check ("an FL_ITEM box", st_item, 1000);
	floor_check ("a rotated BSP box", st_rotated, 500);
	floor_check ("a link of a freed entity", st_free, 500);
	floor_check ("a link of the world entity", st_world, 200);

	if (n_fail)
	{
		printf ("world_link differential harness (%s): FAIL, %ld problem(s) in %ld scenarios\n",
#ifdef WT_HW
			"SV_Error",
#else
			"Sys_Error",
#endif
			n_fail, n_scen);
		return 1;
	}
	printf ("world_link differential harness: PASS (%ld cases: %ld scenarios, %ld QuakeC calls)\n",
		n_scen + st_calls, n_scen, st_calls);
	return 0;
}
