/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/world_trace.rs against the C original of
 * SV_HullPointContents and SV_RecursiveHullCheck, in one binary.
 *
 * run_diff_harness.sh builds this twice: once against world_trace_h2.c (the
 * Hexen II original, -DSERVERONLY as h2ded compiles it) with the archive's
 * world_trace feature, and once against world_trace_hw.c (-DH2W -DSERVERONLY,
 * as hwsv compiles it, -DWT_HW here) with world_trace_hw.  Each binary holds
 * the one Rust module to one original, with the error name that target links.
 *
 * Each binary links the original TWICE: c_* is plain IEEE C, the C source's
 * own semantics, and f_* is built with the flags every engine target ships
 * with (-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer).  The contract is the
 * one world_trace.rs states:
 *
 *   - against c_*, for every input: the same bytes wherever the C returns,
 *     and where it spins forever on a NaN frac, the port's "backup past 0"
 *     exit instead -- the port always returns;
 *   - against f_*: counted and printed, never enforced.  -ffast-math lets GCC
 *     regroup DotProductDBL and assume no NaN.  Rarely, that moves the low
 *     bits of a fraction where a far slanted plane's distance nearly
 *     cancels, and on a sub-ulp normal component it can move the side a
 *     point lands on.  The run prints how often it saw each, phase by
 *     phase (in the gate's run: nothing in the main phase, one trace in the
 *     cancellation phase; in the timed phase, the only one with sub-ulp
 *     normals and NaN, tens of traces and a couple of point contents, plus
 *     in the Hexen II run a few hangs of its own).  Those results belong to the
 *     compiler that built f_*; the C source is what the port follows.
 *
 * What is compared, per call, byte for byte (every NaN canonicalised to one
 * bit pattern first; see canon_nan):
 *   - the return value;
 *   - the whole trace_t, which both sides start from the same bytes -- either
 *     the engine's own initialisation (SV_ClipMoveToEntity: zero, fraction 1,
 *     allsolid, endpos = end) or the same random fill, so a field one side
 *     writes and the other does not shows up as well as a wrong value;
 *   - every CON_Printf the call makes ("backup past 0"), flags and text;
 *   - p1, p2 and the hull, which neither may write.
 * Fatal paths -- a node number outside [firstclipnode, lastclipnode], reached
 * from either function -- run in forked children, one per implementation, and
 * the error FUNCTION as well as its message must match: Sys_Error for Hexen
 * II, SV_Error for HexenWorld.
 *
 * Hulls are generated: random clipnode trees (acyclic, children always
 * deeper, so every walk terminates the way a qbsp tree does), the engine's own
 * box hull (SV_InitBoxHull), axial and slanted planes, slanted planes through
 * grid points (where DotProductDBL's double sum decides the side), points on
 * planes and coincident endpoints.  A separate phase feeds NaN, infinities,
 * -0.0 and denormals into points and plane distances.  Its traces run in
 * timed children, because the C can hang there: a NaN frac never falls below
 * zero, so the backup loop never ends.  There the port must NOT hang: it
 * takes the "backup past 0" exit instead, and everywhere the C returns it
 * must return the same thing.  The harness counts the outcomes it reached --
 * startsolid, inwater, impacts, the "backup past 0" exit -- and fails if any
 * falls below its floor, so a generator change cannot quietly stop reaching
 * a branch.  The coverage build proves the same from the C side: the runner
 * requires every line of the original to have run.
 */

#include "quakedef.h"

#include <float.h>
#include <stddef.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* the IEEE build of the C original, and the engine-flags (-ffast-math) one */
int c_SV_HullPointContents (hull_t *hull, int num, vec3_t p);
qboolean c_SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace);
int f_SV_HullPointContents (hull_t *hull, int num, vec3_t p);
qboolean f_SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace);

/* The Rust module's layout accessors (world_trace.rs). */
extern size_t WorldMClipNode_sizeof (void), WorldMClipNode_offsetof_planenum (void),
	WorldMClipNode_offsetof_children (void);
extern size_t WorldHull_sizeof (void), WorldHull_alignof (void), WorldHull_offsetof_clipnodes (void),
	WorldHull_offsetof_planes (void), WorldHull_offsetof_firstclipnode (void),
	WorldHull_offsetof_lastclipnode (void), WorldHull_offsetof_clip_mins (void),
	WorldHull_offsetof_clip_maxs (void);
extern size_t WorldTrace_sizeof (void), WorldTrace_alignof (void), WorldTrace_offsetof_allsolid (void),
	WorldTrace_offsetof_startsolid (void), WorldTrace_offsetof_inopen (void),
	WorldTrace_offsetof_inwater (void), WorldTrace_offsetof_fraction (void),
	WorldTrace_offsetof_endpos (void), WorldTrace_offsetof_plane_normal (void),
	WorldTrace_offsetof_plane_dist (void), WorldTrace_offsetof_ent (void);
extern size_t MathlibMPlane_sizeof (void), MathlibMPlane_offsetof_normal (void),
	MathlibMPlane_offsetof_dist (void), MathlibMPlane_offsetof_type (void);

#ifdef WT_HW
#define WANT_ERROR_FN	"SV_Error"
#else
#define WANT_ERROR_FN	"Sys_Error"
#endif

/* ------------------------------------------------------------------------ */
/* engine stubs                                                             */
/* ------------------------------------------------------------------------ */

static char	con_log[4096];
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

/* In a forked child, where a fatal call's report goes. */
static int	err_fd = -1;

static void fatal_report (const char *who, const char *fmt, va_list ap)
{
	char	buf[512];
	int	n;

	n = snprintf (buf, sizeof (buf), "%s|", who);
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
/* deterministic generator                                                  */
/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd (void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t) ((rng * 0x2545f4914f6cdd1dULL) >> 32);
}

static int	allow_odd;	/* NaN, inf, denormals: the odd phase only */
static int	far_faces;	/* the cancellation phase: every plane a far qbsp face */

static int chance (int pct)	{ return (int) (rnd () % 100) < pct; }
static float frand (float lo, float hi)	{ return lo + (hi - lo) * ((float) rnd () / 4294967296.0f); }

static const int	leaf_contents[] = {
	CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_SOLID, CONTENTS_SOLID, CONTENTS_SOLID,
	CONTENTS_WATER, CONTENTS_SLIME, CONTENTS_LAVA, CONTENTS_SKY,
	CONTENTS_CURRENT_0, CONTENTS_CURRENT_DOWN
};

static float odd_value (void)
{
	switch (rnd () % 8)
	{
	case 0:	return NAN;
	case 1:	return INFINITY;
	case 2:	return -INFINITY;
	case 3:	return -0.0f;
	case 4:	return 1e-40f;		/* denormal */
	case 5:	return FLT_MIN;
	case 6:	return 1e6f;
	default: return -1e6f;
	}
}

/* Where the timed phase reaches past map scale: a normal with one component
 * far below the double rounding window of the others (GCC's -ffast-math
 * regrouping of DotProductDBL moves the sign there), and coordinates near
 * FLT_MAX (t1 = inf, t2 = -inf, frac = inf/inf: a NaN from finite input). */
static long	st_tiny_normal, st_overflow;
static long	st_qbsp;	/* qbsp-style slanted faces, every phase (the
			 * cancellation phase makes nothing else) */

static float rand_dist (void)
{
	switch (rnd () % 4)
	{
	case 0:	return (float) ((int) (rnd () % 257) - 128);
	case 1:	return (float) ((int) (rnd () % 513) - 256) * 0.5f;
	case 2:	return 0.0f;
	default: return frand (-200.0f, 200.0f);
	}
}

#define MAXNODES	48
#define MAXPLANES	24

typedef struct
{
	mclipnode_t	nodes[MAXNODES];
	mplane_t	planes[MAXPLANES];
	hull_t		hull;
	int		nplanes;
	int		root;
} world_t;

static void normalize3 (vec3_t v)
{
	float	l = sqrtf (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l == 0) { v[0] = 1; v[1] = v[2] = 0; return; }
	v[0] /= l; v[1] /= l; v[2] /= l;
}

static void gen_plane (mplane_t *p)
{
	vec3_t	g;
	int	i;
	int	k = far_faces ? 7 : (int) (rnd () % 8);

	if (k == 6 && !allow_odd)
		k = 7;		/* sub-ulp normals: the timed phase only */

	memset (p, 0, sizeof (*p));
	switch (k)
	{
	case 0:		/* axial, the common case */
		p->type = (byte) (rnd () % 3);
		p->normal[p->type] = chance (50) ? 1.0f : -1.0f;
		p->dist = rand_dist ();
		break;
	case 1:		/* slanted, arbitrary */
		p->normal[0] = frand (-1, 1); p->normal[1] = frand (-1, 1); p->normal[2] = frand (-1, 1);
		normalize3 (p->normal);
		p->type = (byte) (3 + rnd () % 3);
		p->dist = rand_dist ();
		break;
	case 5:		/* far from the origin: |t| beyond 2^18, where the f32 and
			 * f64 crosspoints part company */
		p->type = (byte) (rnd () % 6);
		if (p->type < 3)
			p->normal[p->type] = 1.0f;
		else
		{
			p->normal[0] = frand (-1, 1); p->normal[1] = frand (-1, 1); p->normal[2] = frand (-1, 1);
			normalize3 (p->normal);
		}
		p->dist = frand (-3e6f, 3e6f);
		break;
	case 7:		/* a qbsp face: three integer points, the normal from their
			 * cross product in double, normalised, kept slanted unless a
			 * component is exactly 1 -- utils/qbsp/brush.c does no snapping,
			 * so a slope of one unit in thousands ships as ANY* */
		{
			double	a[3], b[3], c[3], u[3], v[3], n[3], l;
			int	ax = (int) (rnd () % 3);
			double	span = (double) (64 + rnd () % 8192);
			/* a map-scale face, or one at the far edge of the float range a
			 * large map reaches -- where near-cancellation in the plane
			 * distance makes the sum's grouping observable */
			double	scale = (far_faces || chance (30)) ? 4e6 : 8192;
			for (i = 0; i < 3; i++)
			{
				a[i] = (double) (int) ((double) ((int) (rnd () % 16385) - 8192) * (scale / 8192));
				b[i] = a[i]; c[i] = a[i];
			}
			b[(ax + 1) % 3] += span;
			c[(ax + 2) % 3] += span;
			b[ax] += (double) ((int) (rnd () % 3) - 1);	/* off axial by a unit */
			c[ax] += (double) ((int) (rnd () % 5) - 2);
			for (i = 0; i < 3; i++) { u[i] = b[i] - a[i]; v[i] = c[i] - a[i]; }
			n[0] = u[1] * v[2] - u[2] * v[1];
			n[1] = u[2] * v[0] - u[0] * v[2];
			n[2] = u[0] * v[1] - u[1] * v[0];
			l = sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			for (i = 0; i < 3; i++) p->normal[i] = (float) (n[i] / l);
			p->dist = (float) (a[0] * n[0] / l + a[1] * n[1] / l + a[2] * n[2] / l);
			if (p->normal[0] == 1.0f) p->type = 0;
			else if (p->normal[1] == 1.0f) p->type = 1;
			else if (p->normal[2] == 1.0f) p->type = 2;
			else p->type = (byte) (3 + ax);
			st_qbsp++;
		}
		break;
	case 2:		/* 45 degrees through a grid point: the DBL sum decides */
		p->normal[rnd () % 3] = 0.70710677f;
		p->normal[(rnd () % 2) ? 1 : 2] = chance (50) ? 0.70710677f : -0.70710677f;
		normalize3 (p->normal);
		g[0] = (float) ((int) (rnd () % 65) - 32);
		g[1] = (float) ((int) (rnd () % 65) - 32);
		g[2] = (float) ((int) (rnd () % 65) - 32);
		p->dist = p->normal[0] * g[0] + p->normal[1] * g[1] + p->normal[2] * g[2];
		p->type = (byte) (3 + rnd () % 3);
		break;
	case 3:		/* axis-aligned normal on the slanted path */
		p->normal[rnd () % 3] = chance (50) ? 1.0f : -1.0f;
		p->type = (byte) (3 + rnd () % 3);
		p->dist = rand_dist ();
		break;
	case 6:		/* odd phase only: a sub-ulp normal component */
		{
			static const float	tiny[] = { 1e-15f, 1e-12f, 1e-9f, 5e-8f };
			int			ax = (int) (rnd () % 3);
			p->normal[ax] = chance (50) ? 1.0f : -1.0f;
			p->normal[(ax + 1 + (int) (rnd () % 2)) % 3] = tiny[rnd () % 4] * (chance (50) ? 1 : -1);
			p->type = (byte) (3 + rnd () % 3);
			p->dist = chance (50) ? rand_dist () : (float) (int) (rnd () % 4096);
			st_tiny_normal++;
		}
		break;
	case 4:		/* odd values in the plane itself */
		p->type = (byte) (rnd () % 6);
		p->normal[0] = frand (-1, 1); p->normal[1] = frand (-1, 1); p->normal[2] = frand (-1, 1);
		normalize3 (p->normal);
		if (p->type < 3)
		{
			memset (p->normal, 0, sizeof (p->normal));
			p->normal[p->type] = 1.0f;
		}
		p->dist = (allow_odd && chance (30)) ? odd_value () : rand_dist ();
		break;
	}
	p->signbits = (byte) (rnd () & 7);
}

static int gen_child (int i, int last)
{
	if (i < last && chance (60))
		return i + 1 + (int) (rnd () % (unsigned) (last - i));
	return leaf_contents[rnd () % (sizeof (leaf_contents) / sizeof (leaf_contents[0]))];
}

/* A random clipnode tree over [first, last], children strictly deeper. */
static void gen_tree (world_t *w)
{
	int	i, first, count;

	memset (w, 0, sizeof (*w));
	w->nplanes = 1 + (int) (rnd () % MAXPLANES);
	for (i = 0; i < w->nplanes; i++)
		gen_plane (&w->planes[i]);

	first = (int) (rnd () % 3);
	count = 1 + (int) (rnd () % (MAXNODES - 3));
	for (i = 0; i < MAXNODES; i++)
	{
		w->nodes[i].planenum = (int) (rnd () % (unsigned) w->nplanes);
		w->nodes[i].children[0] = gen_child (i, first + count - 1);
		w->nodes[i].children[1] = gen_child (i, first + count - 1);
	}
	w->hull.clipnodes = w->nodes;
	w->hull.planes = w->planes;
	w->hull.firstclipnode = first;
	w->hull.lastclipnode = first + count - 1;
	w->root = first;
}

/* SV_InitBoxHull + SV_HullForBox, as world.c builds them. */
static void gen_box (world_t *w)
{
	int	i, side;
	vec3_t	mins, maxs;

	memset (w, 0, sizeof (*w));
	for (i = 0; i < 3; i++)
	{
		mins[i] = (float) ((int) (rnd () % 129) - 96);
		maxs[i] = mins[i] + (float) (1 + rnd () % 96);
	}
	for (i = 0; i < 6; i++)
	{
		w->nodes[i].planenum = i;
		side = i & 1;
		w->nodes[i].children[side] = CONTENTS_EMPTY;
		w->nodes[i].children[side ^ 1] = (i != 5) ? i + 1 : CONTENTS_SOLID;
		w->planes[i].type = (byte) (i >> 1);
		w->planes[i].normal[i >> 1] = 1;
	}
	w->planes[0].dist = maxs[0];
	w->planes[1].dist = mins[0];
	w->planes[2].dist = maxs[1];
	w->planes[3].dist = mins[1];
	w->planes[4].dist = maxs[2];
	w->planes[5].dist = mins[2];
	w->nplanes = 6;
	w->hull.clipnodes = w->nodes;
	w->hull.planes = w->planes;
	w->hull.firstclipnode = 0;
	w->hull.lastclipnode = 5;
	w->root = 0;
}

static void gen_point (const world_t *w, vec3_t p)
{
	const mplane_t	*pl;
	float		d;
	int		i;

	switch (rnd () % 9)
	{
	case 0:
	case 1:
		for (i = 0; i < 3; i++) p[i] = frand (-160, 160);
		break;
	case 6:		/* large coordinates */
		for (i = 0; i < 3; i++) p[i] = frand (-4e6f, 4e6f);
		break;
	case 8:		/* a move's end just past a slanted plane, at map scale or
			 * millions of units out (the plane may be a far one) */
		pl = &w->planes[rnd () % (unsigned) w->nplanes];
		{
			float	r = chance (30) ? 4e6f : 8192.0f;
			for (i = 0; i < 3; i++) p[i] = frand (-r, r);
		}
		d = pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
		for (i = 0; i < 3; i++) p[i] -= pl->normal[i] * (d - frand (-1.0f, 1.0f));
		break;
	case 7:		/* a few ulps off a plane, possibly a far one */
		pl = &w->planes[rnd () % (unsigned) w->nplanes];
		for (i = 0; i < 3; i++) p[i] = frand (-64, 64);
		if (pl->type < 3)
		{
			p[pl->type] = pl->dist;
			for (i = (int) (rnd () % 4); i > 0; i--)
				p[pl->type] = nextafterf (p[pl->type], chance (50) ? INFINITY : -INFINITY);
		}
		break;
	case 2:		/* grid points; zero coordinates sometimes -0.0, which is
			 * finite and can come out of the midpoint arithmetic */
		for (i = 0; i < 3; i++)
		{
			p[i] = (float) ((int) (rnd () % 97) - 48);
			if (chance (20)) p[i] = chance (50) ? -0.0f : 0.0f;
		}
		break;
	case 3:		/* on a plane */
		pl = &w->planes[rnd () % (unsigned) w->nplanes];
		for (i = 0; i < 3; i++) p[i] = (float) ((int) (rnd () % 97) - 48);
		if (pl->type < 3)
			p[pl->type] = pl->dist;
		else
		{
			d = pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
			for (i = 0; i < 3; i++) p[i] -= pl->normal[i] * d;
		}
		break;
	case 4:		/* a hair off a plane */
		pl = &w->planes[rnd () % (unsigned) w->nplanes];
		for (i = 0; i < 3; i++) p[i] = frand (-64, 64);
		if (pl->type < 3)
			p[pl->type] = pl->dist + (chance (50) ? 1 : -1) * (float) DIST_EPSILON * frand (0, 2);
		break;
	default:
		for (i = 0; i < 3; i++) p[i] = frand (-160, 160);
		if (allow_odd && chance (40))
			p[rnd () % 3] = odd_value ();
		if (allow_odd && chance (15))
		{
			p[0] = chance (50) ? 3e38f : -3e38f;
			p[1] = chance (50) ? 3e38f : -3e38f;
			st_overflow++;
		}
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* comparison                                                               */
/* ------------------------------------------------------------------------ */

static long	n_traces, n_points, n_fatal, n_fail;
static long	st_clear, st_clip, st_startsolid, st_allsolid, st_inwater, st_inopen;
static long	st_impact, st_backup0, st_odd, st_fatal_hpc, st_fatal_rhc, st_box;
static long	st_odd_hung, st_odd_returned, n_odd;
static long	st_fast_hung, st_fast_agree, st_fast_differ, st_big;
/* The -ffast-math build against the Rust, counted per phase, never held. */
enum { PH_MAIN, PH_CANCEL, PH_TIMED, PH_COUNT };
static int	phase = PH_MAIN;
static const char *const phase_name[PH_COUNT] = { "main", "cancellation", "timed" };
static long	fast_ret[PH_COUNT], fast_bits[PH_COUNT], fast_contents[PH_COUNT], st_fatal_fast_differ;
/* Traces where, for some plane of the tree at p1 or p2, the float distance
 * rounds differently under the source's grouping of DotProductDBL than under
 * the one GCC's -ffast-math build uses ((n0p0 + n1p1) + (n2p2 - dist), read
 * off the f_ object's disassembly).  Computed here, independent of any
 * compiler.  It shows the inputs reach the regime where the grouping is a
 * different number, not that a trace's output changes: most such planes are
 * never visited, or the difference does not survive to the result.  What
 * output-level detection there is, is the mutant record in the commit that
 * added this: a port summing GCC's way fails directed_regrouping_case and
 * 1-6 generated traces, depending on the seed. */
static long	st_grouping_main, st_grouping_cancel;

static void report (const char *what, long id, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH %s #%ld: %s\n", what, id, detail);
}

static void hexdump (char *out, size_t sz, const void *p, size_t n)
{
	const unsigned char	*b = (const unsigned char *) p;
	size_t			i, o = 0;
	for (i = 0; i < n && o + 3 < sz; i++)
		o += (size_t) snprintf (out + o, sz - o, "%02x", b[i]);
}

/* Every NaN as one bit pattern.  Which NaN an operation returns when an
 * operand is already NaN is unspecified by IEEE 754, and x86 returns the
 * first operand's -- so `p1 + frac*d` gives +qNaN or -qNaN depending on
 * which way round the compiler emits a commutative add.  GCC and rustc pick
 * differently, and nothing in the engine reads a NaN's bits; a NaN where the
 * other side has a number is still a mismatch. */
static void canon_nan (float *f)
{
	if (isnan (*f))
	{
		uint32_t	q = 0x7fc00000u;
		memcpy (f, &q, sizeof (q));
	}
}

static void canon_trace (trace_t *t)
{
	int	i;
	canon_nan (&t->fraction);
	canon_nan (&t->plane.dist);
	for (i = 0; i < 3; i++)
	{
		canon_nan (&t->endpos[i]);
		canon_nan (&t->plane.normal[i]);
	}
}

static void init_trace (trace_t *t, const vec3_t p2)
{
	if (chance (75))
	{
		memset (t, 0, sizeof (*t));
		t->fraction = 1;
		t->allsolid = true;
		VectorCopy (p2, t->endpos);
	}
	else
	{
		unsigned char	*b = (unsigned char *) t;
		size_t		i;
		for (i = 0; i < sizeof (*t); i++)
			b[i] = (unsigned char) rnd ();
		/* qboolean fields as the engine leaves them: 0 or 1 */
		t->allsolid = (qboolean) (rnd () & 1);
		t->startsolid = (qboolean) (rnd () & 1);
		t->inopen = (qboolean) (rnd () & 1);
		t->inwater = (qboolean) (rnd () & 1);
	}
}

static int grouping_matters (const mplane_t *pl, const vec3_t p)
{
	double	a = (double) pl->normal[0] * (double) p[0];
	double	b = (double) pl->normal[1] * (double) p[1];
	double	c = (double) pl->normal[2] * (double) p[2];
	volatile float	src = (float) (((a + b) + c) - (double) pl->dist);
	volatile float	alt = (float) ((a + b) + (c - (double) pl->dist));
	return pl->type >= 3 && memcmp ((const void *) &src, (const void *) &alt, sizeof (float)) != 0;
}

static void compare_trace (world_t *w, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2)
{
	trace_t		init, tc, tr, tf;
	vec3_t		c1, c2, r1, r2, f1, f2;
	world_t		before;
	char		log_c[sizeof (con_log)], log_r[sizeof (con_log)], log_f[sizeof (con_log)];
	int		rc, rr, rf;
	char		a[256], b[256], msg[700 + 2 * sizeof (con_log)];

	n_traces++;
	init_trace (&init, p2);
	tc = init;
	tr = init;
	tf = init;
	VectorCopy (p1, c1); VectorCopy (p2, c2);
	VectorCopy (p1, r1); VectorCopy (p2, r2);
	VectorCopy (p1, f1); VectorCopy (p2, f2);
	before = *w;

	con_len = 0; con_log[0] = 0;
	rc = c_SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, c1, c2, &tc);
	memcpy (log_c, con_log, con_len + 1);

	con_len = 0; con_log[0] = 0;
	rr = SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, r1, r2, &tr);
	memcpy (log_r, con_log, con_len + 1);

	con_len = 0; con_log[0] = 0;
	rf = f_SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, f1, f2, &tf);
	memcpy (log_f, con_log, con_len + 1);

	if (rc != rr)
	{
		snprintf (msg, sizeof (msg), "return C=%d Rust=%d", rc, rr);
		report ("trace", n_traces, msg);
	}
	canon_trace (&tc);
	canon_trace (&tr);
	canon_trace (&tf);
	/* the engine-flags build: counted, not held (see the header) */
	if (rf != rr) fast_ret[phase]++;
	else if (memcmp (&tf, &tr, sizeof (tf)) != 0 || strcmp (log_f, log_r) != 0) fast_bits[phase]++;
	if (memcmp (&tc, &tr, sizeof (tc)) != 0)
	{
		hexdump (a, sizeof (a), &tc, sizeof (tc));
		hexdump (b, sizeof (b), &tr, sizeof (tr));
		snprintf (msg, sizeof (msg), "trace_t differs (frac C=%a Rust=%a)\n  C    %s\n  Rust %s",
			  tc.fraction, tr.fraction, a, b);
		report ("trace", n_traces, msg);
	}
	if (strcmp (log_c, log_r) != 0)
	{
		snprintf (msg, sizeof (msg), "console C=\"%.300s\" Rust=\"%.300s\"", log_c, log_r);
		report ("trace", n_traces, msg);
	}
	if (memcmp (f1, p1, sizeof (vec3_t)) || memcmp (f2, p2, sizeof (vec3_t)) ||
	    memcmp (c1, p1, sizeof (vec3_t)) || memcmp (c2, p2, sizeof (vec3_t)) ||
	    memcmp (r1, p1, sizeof (vec3_t)) || memcmp (r2, p2, sizeof (vec3_t)))
		report ("trace", n_traces, "an implementation wrote p1 or p2");
	if (memcmp (&before, w, sizeof (*w)) != 0)
		report ("trace", n_traces, "an implementation wrote the hull");

	{
		int	i;
		for (i = 0; i < w->nplanes; i++)
			if (grouping_matters (&w->planes[i], p1) || grouping_matters (&w->planes[i], p2))
			{
				if (far_faces) st_grouping_cancel++; else st_grouping_main++;
				break;
			}
	}

	/* outcome census, from the C side */
	if (rc) st_clear++; else st_clip++;
	if (tc.startsolid && !init.startsolid) st_startsolid++;
	if (init.allsolid && tc.allsolid) st_allsolid++;
	if (tc.inwater && !init.inwater) st_inwater++;
	if (tc.inopen && !init.inopen) st_inopen++;
	if (!rc && !tc.allsolid && tc.fraction < init.fraction) st_impact++;
	if (strstr (log_c, "backup past 0")) st_backup0++;
}

static int finite3 (const vec3_t v)
{
	return isfinite (v[0]) && isfinite (v[1]) && isfinite (v[2]);
}

static void compare_point (world_t *w, int num, const vec3_t p)
{
	vec3_t	pc, pr, pf;
	int	c, r, f;
	char	msg[160];

	n_points++;
	VectorCopy (p, pc); VectorCopy (p, pr); VectorCopy (p, pf);
	c = c_SV_HullPointContents (&w->hull, num, pc);
	r = SV_HullPointContents (&w->hull, num, pr);
	f = f_SV_HullPointContents (&w->hull, num, pf);
	if (f != r) fast_contents[phase]++;	/* counted, not held */
	if (c != r)
	{
		snprintf (msg, sizeof (msg), "contents C=%d Rust=%d at (%a %a %a) from %d", c, r, p[0], p[1], p[2], num);
		report ("point", n_points, msg);
	}
}

/* ------------------------------------------------------------------------ */
/* fatal paths, one child per implementation                                */
/* ------------------------------------------------------------------------ */

typedef enum { CALL_HPC, CALL_RHC } call_t;

/* Run one call in a child; return what it reported: "<error fn>|<message>"
 * for a fatal call, "RETURNED|<rc>|<trace bytes>" for a normal return. */
enum { IMPL_C, IMPL_RUST, IMPL_FAST };

static void run_child (int impl, call_t call, world_t *w, int num, float p1f, float p2f,
		       const vec3_t p1, const vec3_t p2, const trace_t *init, char *out, size_t outsz)
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
		trace_t	t = *init;
		vec3_t	a, b;
		char	buf[400];
		int	rc;

		struct itimerval	it;

		close (fds[0]);
		err_fd = fds[1];
		/* A NaN frac never drops below 0, so the C's backup loop can spin
		 * forever; 100 ms is ample for any finite trace here.  SIGALRM's
		 * default action ends the child, and the parent calls that HUNG. */
		memset (&it, 0, sizeof (it));
		it.it_value.tv_usec = 100000;
		setitimer (ITIMER_REAL, &it, NULL);
		con_len = 0; con_log[0] = 0;
		VectorCopy (p1, a); VectorCopy (p2, b);
		if (call == CALL_HPC)
		{
			rc = impl == IMPL_RUST ? SV_HullPointContents (&w->hull, num, a)
			   : impl == IMPL_FAST ? f_SV_HullPointContents (&w->hull, num, a)
			   : c_SV_HullPointContents (&w->hull, num, a);
			snprintf (buf, sizeof (buf), "RETURNED|%d", rc);
		}
		else
		{
			char	hx[200];
			rc = impl == IMPL_RUST ? SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, a, b, &t)
			   : impl == IMPL_FAST ? f_SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, a, b, &t)
			   : c_SV_RecursiveHullCheck (&w->hull, num, p1f, p2f, a, b, &t);
			canon_trace (&t);
			hexdump (hx, sizeof (hx), &t, sizeof (t));
			snprintf (buf, sizeof (buf), "RETURNED|%d|%s|%.80s", rc, hx, con_log);
		}
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
	if (WIFSIGNALED (status) && WTERMSIG (status) == SIGALRM)
		snprintf (out, outsz, "HUNG");
	else if (!WIFEXITED (status) || (WEXITSTATUS (status) != 0 && WEXITSTATUS (status) != 77))
		snprintf (out, outsz, "CRASHED|status %d", status);
}

/* The C's __thisfunc__ names the renamed c_ function; strip that prefix,
 * which is the harness's renaming, not the original's text. */
static void unrename (char *s)
{
	char	*p;
	while ((p = strstr (s, "c_SV_")) != NULL || (p = strstr (s, "f_SV_")) != NULL)
		memmove (p, p + 2, strlen (p + 2) + 1);
}

static void compare_fatal (call_t call, world_t *w, int num)
{
	char	oc[512], orr[512], of[512], msg[1700];
	vec3_t	p1, p2;
	trace_t	init;

	n_fatal++;
	gen_point (w, p1);
	gen_point (w, p2);
	init_trace (&init, p2);
	run_child (IMPL_C, call, w, num, 0, 1, p1, p2, &init, oc, sizeof (oc));
	run_child (IMPL_RUST, call, w, num, 0, 1, p1, p2, &init, orr, sizeof (orr));
	run_child (IMPL_FAST, call, w, num, 0, 1, p1, p2, &init, of, sizeof (of));
	unrename (oc);
	unrename (of);
	if (strcmp (of, orr) != 0)
		st_fatal_fast_differ++;	/* the engine-flags build: counted, not held */
	if (strcmp (oc, orr) != 0)
	{
		snprintf (msg, sizeof (msg), "C \"%s\" Rust \"%s\"", oc, orr);
		report ("fatal", n_fatal, msg);
		return;
	}
	if (strncmp (oc, "RETURNED|", 9) == 0)
		return;
	if (strncmp (oc, WANT_ERROR_FN "|", strlen (WANT_ERROR_FN) + 1) != 0 ||
	    !strstr (oc, ": bad node number"))
	{
		snprintf (msg, sizeof (msg), "both report \"%s\", want %s(\"...: bad node number\")", oc, WANT_ERROR_FN);
		report ("fatal", n_fatal, msg);
		return;
	}
	if (strstr (oc, "|SV_HullPointContents:")) st_fatal_hpc++;
	if (strstr (oc, "|SV_RecursiveHullCheck:")) st_fatal_rhc++;
}

/* A trace whose inputs may be non-finite, each implementation in its own
 * timed child.  Where the IEEE C returns or reports a fatal error, the port
 * must do exactly the same; where the IEEE C hangs in the backup loop, the
 * port must take the "backup past 0" exit.  The port never hangs.  The
 * -ffast-math build's outcome is counted only. */
static void compare_odd (world_t *w, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2)
{
	char	oc[512], orr[512], of[512], msg[1700];
	trace_t	init;

	n_odd++;
	init_trace (&init, p2);
	run_child (IMPL_C, CALL_RHC, w, num, p1f, p2f, p1, p2, &init, oc, sizeof (oc));
	run_child (IMPL_RUST, CALL_RHC, w, num, p1f, p2f, p1, p2, &init, orr, sizeof (orr));
	run_child (IMPL_FAST, CALL_RHC, w, num, p1f, p2f, p1, p2, &init, of, sizeof (of));
	unrename (oc);
	unrename (of);

	/* the port returns, always */
	if (strncmp (orr, "RETURNED|", 9) != 0 && strncmp (orr, WANT_ERROR_FN "|", strlen (WANT_ERROR_FN) + 1) != 0)
	{
		snprintf (msg, sizeof (msg), "Rust did not return: \"%s\" (C \"%s\")", orr, oc);
		report ("odd", n_odd, msg);
		return;
	}
	if (strcmp (oc, "HUNG") == 0)
	{
		/* where the C spins on a NaN frac, the port takes the backup-past-0
		 * exit: a clip (0) and that console line */
		st_odd_hung++;
		if (strncmp (orr, "RETURNED|0|", 11) != 0 || !strstr (orr, "backup past 0"))
		{
			snprintf (msg, sizeof (msg), "C hangs, Rust \"%s\", want the backup-past-0 exit", orr);
			report ("odd", n_odd, msg);
		}
		return;
	}
	if (strcmp (oc, orr) != 0)
	{
		snprintf (msg, sizeof (msg), "C \"%s\" Rust \"%s\"", oc, orr);
		report ("odd", n_odd, msg);
		return;
	}
	st_odd_returned++;
	/* the shipped build: noted, not held */
	if (strcmp (of, "HUNG") == 0) st_fast_hung++;
	else if (strcmp (of, orr) == 0) st_fast_agree++;
	else
	{
		/* both "RETURNED|<rc>|...": a differing rc is a return difference,
		 * anything else (bytes, console, a fatal against a return) a
		 * difference in the trace's result */
		st_fast_differ++;
		if (strncmp (of, "RETURNED|", 9) == 0 && strncmp (orr, "RETURNED|", 9) == 0 &&
		    atoi (of + 9) != atoi (orr + 9))
			fast_ret[PH_TIMED]++;
		else
			fast_bits[PH_TIMED]++;
	}
}

/* A tree with one node pointing outside the valid range. */
static void break_tree (world_t *w)
{
	int	i = w->hull.firstclipnode + (int) (rnd () % (unsigned) (w->hull.lastclipnode - w->hull.firstclipnode + 1));
	int	bad;

	if (w->hull.firstclipnode > 0 && chance (40))
		bad = w->hull.firstclipnode - 1;
	else
		bad = w->hull.lastclipnode + 1 + (int) (rnd () % 3);
	w->nodes[i].children[rnd () & 1] = bad;
}

/* ------------------------------------------------------------------------ */

/* The case the second review found, kept as a fixed regression: normal
 * (1e-15, 0, 1), dist 1000, the point (-1, 0, 1000) on the plane.  The C
 * source says d = 0, so child 0 (EMPTY); GCC's -ffast-math build regroups
 * the sum and lands on child 1 (SOLID).  The port must say what the source
 * says.  The engine-flags answer is printed, so a compiler that stops
 * regrouping shows up here rather than silently. */
static void directed_regrouping_case (void)
{
	world_t	w;
	vec3_t	pt = { -1, 0, 1000 }, a = { -1, 0, 1001 }, b = { -1, 0, 1000 };

	memset (&w, 0, sizeof (w));
	w.planes[0].normal[0] = 1e-15f;
	w.planes[0].normal[2] = 1.0f;
	w.planes[0].type = 5;
	w.planes[0].dist = 1000;
	w.nodes[0].planenum = 0;
	w.nodes[0].children[0] = CONTENTS_EMPTY;
	w.nodes[0].children[1] = CONTENTS_SOLID;
	w.nplanes = 1;
	w.hull.clipnodes = w.nodes;
	w.hull.planes = w.planes;
	w.hull.firstclipnode = 0;
	w.hull.lastclipnode = 0;
	{
		vec3_t	q;
		int	c, r, f;
		VectorCopy (pt, q); c = c_SV_HullPointContents (&w.hull, 0, q);
		VectorCopy (pt, q); r = SV_HullPointContents (&w.hull, 0, q);
		VectorCopy (pt, q); f = f_SV_HullPointContents (&w.hull, 0, q);
		printf ("directed: sub-ulp normal, point on plane: C source %d, Rust %d, -ffast-math C %d\n", c, r, f);
		if (c != r || c != CONTENTS_EMPTY)
			report ("directed", 1, "the port does not give the C source's contents for a point on a sub-ulp-normal plane");
	}
	{
		trace_t	tc, tr;
		vec3_t	a1, b1;
		memset (&tc, 0, sizeof (tc)); tc.fraction = 1; tc.allsolid = true; VectorCopy (b, tc.endpos);
		tr = tc;
		VectorCopy (a, a1); VectorCopy (b, b1);
		c_SV_RecursiveHullCheck (&w.hull, 0, 0, 1, a1, b1, &tc);
		VectorCopy (a, a1); VectorCopy (b, b1);
		SV_RecursiveHullCheck (&w.hull, 0, 0, 1, a1, b1, &tr);
		if (memcmp (&tc, &tr, sizeof (tc)) != 0)
			report ("directed", 2, "the port's trace differs from the C source's on a sub-ulp-normal plane");
	}
}

static void floor_check (const char *name, long have, long want)
{
	if (have < want)
	{
		fprintf (stderr, "COVERAGE: %s reached %ld time(s), floor %ld -- the generator stopped reaching it\n",
			 name, have, want);
		n_fail++;
	}
}

int main (void)
{
	world_t	w;
	vec3_t	p1, p2;
	int	iw, k, num;
	float	p1f, p2f, t;

	/* The shared structs, from this target's own headers, against the Rust
	 * module's layout.  abi_layout.c checks its copies of them on every
	 * target, wasm32 included; this is what ties those copies to the real
	 * headers. */
#define LAYOUT(what, c, r) do { if ((size_t) (c) != (r)) { \
		fprintf (stderr, "LAYOUT: %s is %zu in C, %zu in Rust\n", what, (size_t) (c), (size_t) (r)); \
		return 2; } } while (0)
	LAYOUT ("sizeof(mclipnode_t)", sizeof (mclipnode_t), WorldMClipNode_sizeof ());
	LAYOUT ("mclipnode_t.planenum", offsetof (mclipnode_t, planenum), WorldMClipNode_offsetof_planenum ());
	LAYOUT ("mclipnode_t.children", offsetof (mclipnode_t, children), WorldMClipNode_offsetof_children ());
	LAYOUT ("sizeof(mclipnode_t.children[0])", sizeof (((mclipnode_t *) 0)->children[0]), sizeof (int));
	LAYOUT ("sizeof(hull_t)", sizeof (hull_t), WorldHull_sizeof ());
	LAYOUT ("alignof(hull_t)", _Alignof (hull_t), WorldHull_alignof ());
	LAYOUT ("hull_t.clipnodes", offsetof (hull_t, clipnodes), WorldHull_offsetof_clipnodes ());
	LAYOUT ("hull_t.planes", offsetof (hull_t, planes), WorldHull_offsetof_planes ());
	LAYOUT ("hull_t.firstclipnode", offsetof (hull_t, firstclipnode), WorldHull_offsetof_firstclipnode ());
	LAYOUT ("hull_t.lastclipnode", offsetof (hull_t, lastclipnode), WorldHull_offsetof_lastclipnode ());
	LAYOUT ("hull_t.clip_mins", offsetof (hull_t, clip_mins), WorldHull_offsetof_clip_mins ());
	LAYOUT ("hull_t.clip_maxs", offsetof (hull_t, clip_maxs), WorldHull_offsetof_clip_maxs ());
	LAYOUT ("sizeof(trace_t)", sizeof (trace_t), WorldTrace_sizeof ());
	LAYOUT ("alignof(trace_t)", _Alignof (trace_t), WorldTrace_alignof ());
	LAYOUT ("trace_t.allsolid", offsetof (trace_t, allsolid), WorldTrace_offsetof_allsolid ());
	LAYOUT ("trace_t.startsolid", offsetof (trace_t, startsolid), WorldTrace_offsetof_startsolid ());
	LAYOUT ("trace_t.inopen", offsetof (trace_t, inopen), WorldTrace_offsetof_inopen ());
	LAYOUT ("trace_t.inwater", offsetof (trace_t, inwater), WorldTrace_offsetof_inwater ());
	LAYOUT ("trace_t.fraction", offsetof (trace_t, fraction), WorldTrace_offsetof_fraction ());
	LAYOUT ("trace_t.endpos", offsetof (trace_t, endpos), WorldTrace_offsetof_endpos ());
	LAYOUT ("trace_t.plane.normal", offsetof (trace_t, plane.normal), WorldTrace_offsetof_plane_normal ());
	LAYOUT ("trace_t.plane.dist", offsetof (trace_t, plane.dist), WorldTrace_offsetof_plane_dist ());
	LAYOUT ("trace_t.ent", offsetof (trace_t, ent), WorldTrace_offsetof_ent ());
	LAYOUT ("sizeof(qboolean)", sizeof (qboolean), sizeof (int));
	LAYOUT ("sizeof(mplane_t)", sizeof (mplane_t), MathlibMPlane_sizeof ());
	LAYOUT ("mplane_t.normal", offsetof (mplane_t, normal), MathlibMPlane_offsetof_normal ());
	LAYOUT ("mplane_t.dist", offsetof (mplane_t, dist), MathlibMPlane_offsetof_dist ());
	LAYOUT ("mplane_t.type", offsetof (mplane_t, type), MathlibMPlane_offsetof_type ());
#undef LAYOUT

	for (iw = 0; iw < 6000; iw++)
	{
		if (iw % 5 == 0) { gen_box (&w); st_box++; }
		else gen_tree (&w);

		for (k = 0; k < 24; k++)
		{
			gen_point (&w, p1);
			switch (rnd () % 5)
			{
			case 0:	VectorCopy (p1, p2); break;
			case 1:	gen_point (&w, p2); p2[rnd () % 3] = p1[rnd () % 3]; break;
			default: gen_point (&w, p2); break;
			}
			if (chance (75)) { p1f = 0; p2f = 1; }
			else
			{
				p1f = frand (0, 1); p2f = frand (0, 1);
				if (chance (70) && p1f > p2f) { t = p1f; p1f = p2f; p2f = t; }
			}
			if (fabsf (p1[0]) > 1e6f || fabsf (p1[1]) > 1e6f || fabsf (p1[2]) > 1e6f)
				st_big++;
			num = chance (85) ? w.root
				: (chance (50) ? leaf_contents[rnd () % (sizeof (leaf_contents) / sizeof (leaf_contents[0]))]
					       : w.hull.firstclipnode + (int) (rnd () % (unsigned) (w.hull.lastclipnode - w.hull.firstclipnode + 1)));
			compare_trace (&w, num, p1f, p2f, p1, p2);

			gen_point (&w, p1);
			compare_point (&w, num, p1);
		}
	}

	directed_regrouping_case ();

	/* The cancellation phase.  Every plane is a qbsp face millions of units
	 * out, and every move crosses one a few units either side of it: there
	 * the plane distance is a near-cancelling sum, and its grouping decides
	 * the low bits of t1/t2.  This is where a port that summed DotProductDBL
	 * any other way than the source would be caught. */
	far_faces = 1;
	phase = PH_CANCEL;
	/* At any scale the float t carries the sum's rounding error past a
	 * rounding boundary in only a percent or two of crossings (the point
	 * itself is quantised to a fraction of a unit out there), so this phase
	 * needs volume: 60,000 traces, for a census floor of 400 (observed:
	 * 547 in this phase; the main phase adds about 75 of its own). */
	for (iw = 0; iw < 5000; iw++)
	{
		gen_tree (&w);
		for (k = 0; k < 12; k++)
		{
			const mplane_t	*pl = &w.planes[rnd () % (unsigned) w.nplanes];
			vec3_t		base;
			float		d, u1, u2;
			int		i;

			for (i = 0; i < 3; i++) base[i] = frand (-4e6f, 4e6f);
			d = pl->normal[0] * base[0] + pl->normal[1] * base[1] + pl->normal[2] * base[2] - pl->dist;
			for (i = 0; i < 3; i++) base[i] -= pl->normal[i] * d;
			/* log-uniform from 1e-5 to 8 units: the closer to the plane,
			 * the smaller t's float ulp, and the likelier the sum's own
			 * rounding error shows in it */
			u1 = powf (10.0f, frand (-5.0f, 0.9f));
			u2 = powf (10.0f, frand (-5.0f, 0.9f));
			for (i = 0; i < 3; i++)
			{
				p1[i] = base[i] + pl->normal[i] * u1;
				p2[i] = base[i] - pl->normal[i] * u2;
			}
			if (chance (50)) { vec3_t t; VectorCopy (p1, t); VectorCopy (p2, p1); VectorCopy (t, p2); }
			compare_trace (&w, w.root, 0, 1, p1, p2);
			compare_point (&w, w.root, p1);
		}
	}
	far_faces = 0;
	phase = PH_TIMED;

	/* the odd phase: NaN, infinities and denormals in points and plane
	 * distances.  Point walks always terminate, so they stay in-process;
	 * traces go to timed children: where the IEEE C hangs, the port must
	 * take its "backup past 0" exit instead, and it must never hang. */
	allow_odd = 1;
	for (iw = 0; iw < 400; iw++)
	{
		gen_tree (&w);
		for (k = 0; k < 2; k++)
		{
			gen_point (&w, p1);
			gen_point (&w, p2);
			if (chance (60))
				p1[rnd () % 3] = odd_value ();
			if (!finite3 (p1) || !finite3 (p2))
				st_odd++;
			compare_odd (&w, w.root, 0, 1, p1, p2);
		}
		for (k = 0; k < 16; k++)
		{
			gen_point (&w, p1);
			compare_point (&w, w.root, p1);
		}
	}
	allow_odd = 0;
	phase = PH_MAIN;	/* the fatal phase compares f_ separately */

	/* fatal paths: bad children reached from either function, and bad
	 * starting numbers handed straight to each */
	for (iw = 0; iw < 400; iw++)
	{
		gen_tree (&w);
		break_tree (&w);
		compare_fatal (CALL_RHC, &w, w.root);
		compare_fatal (CALL_HPC, &w, w.root);
		num = chance (50) ? w.hull.lastclipnode + 1 + (int) (rnd () % 4)
			: (w.hull.firstclipnode > 0 ? w.hull.firstclipnode - 1 : w.hull.lastclipnode + 1);
		compare_fatal (chance (50) ? CALL_HPC : CALL_RHC, &w, num);
	}

	printf ("timed phase: %ld sub-ulp-normal planes, %ld near-overflow points\n", st_tiny_normal, st_overflow);
	printf ("generated: %ld qbsp-style faces; traces where the two DotProductDBL groupings round apart:\n"
		"            %ld in the main phase, %ld in the cancellation phase\n",
		st_qbsp, st_grouping_main, st_grouping_cancel);
	{
		int	ph;
		for (ph = 0; ph < PH_COUNT; ph++)
			printf ("            -ffast-math C vs the port, %s phase (counted, not held): %ld returns, %ld trace results, %ld point contents\n",
				phase_name[ph], fast_ret[ph], fast_bits[ph], fast_contents[ph]);
		printf ("            -ffast-math C, timed phase: %ld hang(s) where the IEEE C and the port agree (and neither hangs)\n", st_fast_hung);
		printf ("            -ffast-math C vs the port, fatal phase (counted, not held): %ld outcomes\n", st_fatal_fast_differ);
	}
	printf ("outcomes: clear %ld, clipped %ld, startsolid %ld, allsolid %ld, inwater %ld, inopen %ld,\n"
		"          impacts %ld, backup-past-0 %ld, box hulls %ld,\n"
		"          timed-phase traces %ld: IEEE C hangs %ld (Rust exits), returns %ld (Rust agrees);\n"
		"          there the -ffast-math C agrees %ld, differs %ld, hangs %ld (not compared)\n"
		"          fatal in SV_HullPointContents %ld, in SV_RecursiveHullCheck %ld\n",
		st_clear, st_clip, st_startsolid, st_allsolid, st_inwater, st_inopen,
		st_impact, st_backup0, st_box, n_odd, st_odd_hung, st_odd_returned,
		st_fast_agree, st_fast_differ, st_fast_hung, st_fatal_hpc, st_fatal_rhc);

	floor_check ("a clear trace", st_clear, 1000);
	floor_check ("a clipped trace", st_clip, 1000);
	floor_check ("startsolid", st_startsolid, 500);
	floor_check ("allsolid kept", st_allsolid, 500);
	floor_check ("inwater", st_inwater, 500);
	floor_check ("inopen", st_inopen, 500);
	floor_check ("an impact", st_impact, 1000);
	floor_check ("the \"backup past 0\" exit", st_backup0, 20);
	floor_check ("a non-finite segment", st_odd, 100);
	floor_check ("a non-finite trace the IEEE C hangs on (the port's exit)", st_odd_hung, 5);
	floor_check ("a non-finite trace the IEEE C returns from", st_odd_returned, 50);
	floor_check ("a coordinate beyond 1e6", st_big, 2000);
	floor_check ("a qbsp-style slanted face", st_qbsp, 10000);
	floor_check ("a cancellation-phase trace where the two groupings round apart", st_grouping_cancel, 400);
	floor_check ("a sub-ulp normal component (timed phase)", st_tiny_normal, 100);
	floor_check ("a coordinate near FLT_MAX (timed phase)", st_overflow, 50);
	floor_check ("a fatal call in SV_HullPointContents", st_fatal_hpc, 50);
	floor_check ("a fatal call in SV_RecursiveHullCheck", st_fatal_rhc, 50);

	if (n_fail)
	{
		printf ("world_trace differential harness (%s): FAIL, %ld mismatch(es) in %ld traces, %ld points, %ld fatal cases\n",
			WANT_ERROR_FN, n_fail, n_traces, n_points, n_fatal);
		return 1;
	}
	printf ("world_trace differential harness (%s): PASS (%ld cases: %ld traces, %ld points, %ld fatal, %ld timed-phase)\n",
		WANT_ERROR_FN, n_traces + n_points + n_fatal + n_odd, n_traces, n_points, n_fatal, n_odd);
	return 0;
}
