/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/sv_phys.rs against the C original of
 * sv_phys.c's base helpers, in one binary.
 *
 * run_diff_harness.sh builds this against sv_phys_base_h2.c (-DSERVERONLY,
 * as h2ded compiles it) and sv_phys_base_hw.c (-DH2W -DSERVERONLY -DSV_HW,
 * as hwsv).  The real engine/rust/world_target.c and phys_target.c are
 * compiled in.
 *
 * The worlds are sv_move's harness's: random clipnode trees, and floor, step
 * and ledge worlds, with entities linked by SV_LinkEdict.  Each case puts an
 * entity in a physics state -- velocity (sometimes past sv_maxvelocity,
 * sometimes NaN or infinite), origin, nextthink against sv.time and
 * host_frametime, movetype and solid (SOLID_PHASE among them), hoverz, a
 * gravity field -- and calls one helper the way sv_phys.c does, once on the
 * C and once on the Rust from the same snapshot:
 *   SV_CheckVelocity, SV_RunThink, SV_Impact, ClipVelocity (in and out the
 *   same vector, as SV_Physics_Toss calls it, or not), SV_FlyMove (with and
 *   without a steptrace), SV_FlyExtras, SV_AddGravity, SV_PushEntity.
 * It compares byte for byte: the return value (the whole trace_t for
 * SV_PushEntity, the steptrace for SV_FlyMove), every edict, the area
 * nodes, the progs globals, and the sequence of console lines and QuakeC
 * calls with their self, other and time.
 *
 * The QuakeC calls act, as QuakeC does: a think or touch function may free
 * its entity (SV_RunThink's return, SV_FlyMove's `removed by the impact
 * function' exit), change its velocity, or change the other entity's flags.
 *
 * Cases with non-finite input run in forked children, C and Rust each in its
 * own, because SV_FlyMove's `!trace.ent' fatal is reached only there: a NaN
 * trace fraction is neither 1 nor below 1, so SV_Move sets no entity.  Each
 * child reports a digest of the whole state, or the fatal's function and
 * message, and the two must match.  NaN payloads and signs are canonicalised
 * first: C leaves them unspecified (see sv_move's harness).
 *
 * WHAT THE REFERENCE CALLS.  SV_Move and SV_LinkEdict are the already-ported
 * Rust, each held to its C by its own harness; CON_Printf, PR_GetString,
 * PR_ExecuteProgram and GetEdictFieldValue are the harness's.  So this
 * compares sv_phys.c's own logic over shared callees.
 */

#include "quakedef.h"
#include "world_target.h"
#include "phys_target.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

void		c_SV_CheckVelocity (edict_t *ent);
qboolean	c_SV_RunThink (edict_t *ent);
void		c_SV_Impact (edict_t *e1, edict_t *e2);
int		c_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce);
int		c_SV_FlyMove (edict_t *ent, float time, trace_t *steptrace);
#ifndef SV_HW
void		c_SV_FlyExtras (edict_t *ent, float time, trace_t *steptrace);
#endif
trace_t		c_SV_PushEntity (edict_t *ent, vec3_t push);
#ifdef SV_HW
void		c_SV_AddGravity (edict_t *ent, float scale);
#define VARIANT_NAME	"hexenworld"
#define WANT_ERROR_FN	"SV_Error"
#else
void		c_SV_AddGravity (edict_t *ent);
#define VARIANT_NAME	"hexen2"
#define WANT_ERROR_FN	"Sys_Error"
#endif

/* the engine's storage */
server_t	sv;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
double		host_frametime;
edict_t		*sv_player;
cvar_t		sv_maxvelocity = { "sv_maxvelocity", "2000", CVAR_NONE };
#ifdef SV_HW
movevars_t	movevars;
#else
cvar_t		sv_gravity = { "sv_gravity", "800", CVAR_NONE };
/* phys_target.c's pusher accessors read these; nothing here calls them */
cvar_t		sv_gameplayfix_elevators = { "sv_gameplayfix_elevators", "1", CVAR_NONE };
cvar_t		map_checks = { "map_checks", "0", CVAR_NONE };
cvar_t		sv_nostep = { "sv_nostep", "0", CVAR_NONE };	/* phys_target.c's */
#endif
server_static_t	svs;
int		pr_edict_size = sizeof (edict_t);

static struct
{
	int	self, other, world;
	float	time;
} g;

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

/* ------------------------------------------------------------------------ */
/* the log: console lines and QuakeC calls, in order                        */
/* ------------------------------------------------------------------------ */

static char	dbg[16384];
static size_t	dbg_len;

/* "-nan" and "nan" are the same answer (see the header) */
static void canon_text (char *s)
{
	char	*p;
	while ((p = strstr (s, "-nan")) != NULL)
		memmove (p, p + 1, strlen (p + 1) + 1);
}

static void canon_floats (float *f, size_t n)
{
	size_t	i;
	for (i = 0; i < n; i++)
		if (isnan (f[i]))
			f[i] = NAN;
}

static void dbg_vadd (const char *fmt, va_list ap)
{
	int	n;
	if (dbg_len >= sizeof (dbg) - 1)
		return;
	n = vsnprintf (dbg + dbg_len, sizeof (dbg) - dbg_len, fmt, ap);
	if (n > 0)
	{
		canon_text (dbg + dbg_len);
		dbg_len = strlen (dbg);
	}
}

static void dbg_add (const char *fmt, ...)
{
	va_list	ap;
	va_start (ap, fmt);
	dbg_vadd (fmt, ap);
	va_end (ap);
}

/* ------------------------------------------------------------------------ */
/* engine stubs                                                             */
/* ------------------------------------------------------------------------ */

#define NE	24

static edict_t	edicts[NE];
static float	gravity_field[NE];
static int	gravity_has[NE];

static long	idx (const edict_t *e)	{ return e ? (long) (e - edicts) : -1L; }

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list	ap;
	dbg_add ("P%u ", flags);
	va_start (ap, fmt);
	dbg_vadd (fmt, ap);
	va_end (ap);
}

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
	if (strcmp (field, "gravity") != 0 || i < 0 || i >= NE || !gravity_has[i])
		return NULL;	/* no SOLID_GHOST here, and no other field */
	return (eval_t *) &gravity_field[i];
}

static long	n_qc;

/* pr_edict.c's ED_Free, as far as these functions can see it: unlinked,
 * free, origin and angles cleared, nextthink -1, SOLID_NOT */
static void harness_free (edict_t *ed)
{
	SV_UnlinkEdict (ed);
	ed->free = true;
	ed->v.modelindex = 0;
	VectorClear (ed->v.origin);
	VectorClear (ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;
}

/* A think or touch function.  It logs what it was called with, then acts as
 * QuakeC can, on self or on other, deterministically in the call count (both
 * sides start a case with the same count): remove either (ED_Free), change
 * self's velocity, move self (setorigin and relink), take away other's
 * touch or make it SOLID_NOT, change other's flags.  A port that cached a
 * field across the call, instead of re-reading it as the C does, differs. */
void PR_ExecuteProgram (func_t fnum, const char *funcname)
{
	edict_t	*self = PROG_TO_EDICT(*sv_globals.self);
	edict_t	*other = PROG_TO_EDICT(*sv_globals.other);
	char	t[48];

	n_qc++;
	snprintf (t, sizeof (t), "%a", *sv_globals.time);
	canon_text (t);
	dbg_add ("Q %d %s self e%ld other e%ld time %s\n", fnum, funcname, idx (self), idx (other), t);
	switch (n_qc % 16)
	{
	case 1: harness_free (self); break;
	case 3: self->v.velocity[0] = -self->v.velocity[0]; break;
	case 5: VectorClear (self->v.velocity); break;
	case 7: other->v.flags = (float) ((int) other->v.flags ^ FL_ONGROUND); break;
	case 9:
		if (other != sv.edicts)
			harness_free (other);
		break;
	case 11: other->v.touch = 0; break;
	case 12: other->v.solid = SOLID_NOT; break;
	case 13:	/* setorigin */
		self->v.origin[0] += 24;
		self->v.origin[2] += 8;
		if (!self->free && self != sv.edicts)
			SV_LinkEdict (self, false);
		break;
	default: break;
	}
}

static int	err_fd = -1;

static void fatal_report (const char *who, const char *fmt, va_list ap)
{
	char	buf[512];
	int	n = snprintf (buf, sizeof (buf), "%s|", who);
	vsnprintf (buf + n, sizeof (buf) - (size_t) n, fmt, ap);
	/* __thisfunc__ is __func__, which in the C reference is the renamed
	 * c_SV_FlyMove; the engine's own message names SV_FlyMove */
	if (strncmp (buf + n, "c_", 2) == 0)
		memmove (buf + n, buf + n + 2, strlen (buf + n + 2) + 1);
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

#ifdef SV_HW
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
/* the world (sv_move's harness's)                                          */
/* ------------------------------------------------------------------------ */

#define NMODELS		5	/* 0 unused, 1 the world, 2..4 brush entities */
#define NCLIP		40
#define NPLANES		24

static qmodel_t		models[NMODELS];
static mclipnode_t	clipnodes[NMODELS][MAX_MAP_HULLS][NCLIP];
static mplane_t		planes[NMODELS][NPLANES];
static mleaf_t		one_leaf;		/* the world's render tree: one empty leaf */
static int		terrain;

static const float	floor_z[] = { -64, -40, -36, -24, -18, -17, -8, -1, 0, 0, 0, 8, 16, 18, 19, 36, 54 };

static void gen_plane (mplane_t *p)
{
	memset (p, 0, sizeof (*p));
	if (terrain)
	{
		/* floors and steps on z, walls on x and y */
		p->type = chance (65) ? 2 : (byte) (rnd () % 2);
		p->normal[p->type] = 1;
		p->dist = (p->type == 2) ? floor_z[rnd () % (sizeof (floor_z) / sizeof (floor_z[0]))]
					 : (float) ((int) (rnd () % 9) * 16 - 64);
	}
	else if (chance (70))
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

static int leaf_contents (int back)
{
	static const int	c[] = { CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_EMPTY, CONTENTS_SOLID,
					CONTENTS_SOLID, CONTENTS_WATER, CONTENTS_CURRENT_0, CONTENTS_CURRENT_DOWN };
	if (terrain)
		/* below a floor (the back side) is mostly solid, above mostly open */
		return back ? (chance (80) ? CONTENTS_SOLID : CONTENTS_WATER)
			    : (chance (80) ? CONTENTS_EMPTY : CONTENTS_WATER);
	return c[rnd () % 8];
}

static void gen_tree (mclipnode_t *nodes, int *nodes_used)
{
	int	n = 1 + (int) (rnd () % (NCLIP - 1)), i, k;

	for (i = 0; i < n; i++)
	{
		nodes[i].planenum = (int) (rnd () % NPLANES);
		for (k = 0; k < 2; k++)
			nodes[i].children[k] = (i < n - 1 && chance (60))
				? i + 1 + (int) (rnd () % (unsigned) (n - 1 - i)) : leaf_contents (k);
	}
	*nodes_used = n;
}

/* A ledge: a wall plane splitting two floors 1 to 40 apart, the drop that
 * decides SV_CheckBottom's corner test (more than STEPSIZE refuses).  The
 * model's last three planes are given over to it. */
static void gen_ledge (int m, mclipnode_t *nodes, int *nodes_used)
{
	mplane_t	*wall = &planes[m][NPLANES - 3];
	mplane_t	*hi = &planes[m][NPLANES - 2], *lo = &planes[m][NPLANES - 1];
	int		k;

	memset (wall, 0, sizeof (*wall));
	wall->type = (byte) (rnd () % 2);
	wall->normal[wall->type] = 1;
	wall->dist = (float) ((int) (rnd () % 9) * 16 - 64);
	memset (hi, 0, sizeof (*hi));
	memset (lo, 0, sizeof (*lo));
	hi->type = lo->type = 2;
	hi->normal[2] = lo->normal[2] = 1;
	hi->dist = floor_z[rnd () % (sizeof (floor_z) / sizeof (floor_z[0]))];
	/* mostly inside the band that refuses (STEPSIZE < drop, and still
	 * within the corner trace's 36), then either side of it */
	lo->dist = hi->dist - (chance (60) ? (float) (19 + rnd () % 18) : (float) (1 + rnd () % 40));

	nodes[0].planenum = NPLANES - 3;
	nodes[0].children[0] = 1;
	nodes[0].children[1] = 2;
	for (k = 1; k <= 2; k++)
	{
		nodes[k].planenum = NPLANES - 3 + k;
		nodes[k].children[0] = chance (90) ? CONTENTS_EMPTY : CONTENTS_WATER;
		nodes[k].children[1] = CONTENTS_SOLID;
	}
	*nodes_used = 3;
}

static void gen_hull (int m, int h)
{
	hull_t	*hull = &models[m].hulls[h];
	int	n, k;

	hull->planes = planes[m];
	hull->firstclipnode = 0;
	if (terrain && h > 0)
	{
		/* one tree for every hull of a terrain model, so the hull a mover
		 * clips against and the point hull SV_CheckBottom samples agree */
		hull->clipnodes = clipnodes[m][0];
		hull->lastclipnode = models[m].hulls[0].lastclipnode;
	}
	else
	{
		hull->clipnodes = clipnodes[m][h];
		if (terrain && chance (60))
			gen_ledge (m, clipnodes[m][h], &n);
		else
			gen_tree (clipnodes[m][h], &n);
		hull->lastclipnode = n - 1;
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

	terrain = chance (50);
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
	sv.time = frand (0, 100);

	edicts[0].v.solid = SOLID_BSP;
	edicts[0].v.movetype = MOVETYPE_PUSH;
	edicts[0].v.modelindex = 1;
}

static float odd_value (void)
{
	static const float	v[] = { NAN, -NAN, INFINITY, -INFINITY, 1e30f, -1e30f };
	return v[rnd () % 6];
}

static void place (edict_t *e)
{
	int	k;
	for (k = 0; k < 2; k++)
		e->v.origin[k] = chance (50) ? (float) ((int) (rnd () % 257) - 128) : frand (-160, 160);
	/* astride a ledge's wall, which stands on a multiple of 16 */
	if (chance (30))
		e->v.origin[rnd () % 2] = (float) ((int) (rnd () % 9) * 16 - 64) + frand (-20, 20);
	/* on a floor of the terrain, or anywhere */
	e->v.origin[2] = chance (60)
		? floor_z[rnd () % (sizeof (floor_z) / sizeof (floor_z[0]))] - e->v.mins[2] + (chance (60) ? 0 : frand (-2, 4))
		: frand (-120, 120);
	if (chance (2))
		e->v.origin[rnd () % 3] = odd_value ();
}

static void gen_edicts (void)
{
	int	i, k;

	SV_ClearWorld ();
	for (i = 1; i < NE; i++)
	{
		edict_t	*e = &edicts[i];
		int	kind = (int) (rnd () % 10);

		e->free = chance (3);
		e->v.classname = 1000 + i;
		switch (kind)
		{
		case 0: e->v.solid = SOLID_NOT; break;
		case 1: e->v.solid = SOLID_TRIGGER; e->v.touch = chance (70) ? 1 : 0; break;
		case 2: case 3: case 4: e->v.solid = SOLID_SLIDEBOX; break;
		case 5: case 6: e->v.solid = SOLID_BBOX; break;
		default:
			if (i < 6)
			{
				e->v.solid = SOLID_BSP;
				e->v.movetype = MOVETYPE_PUSH;
				e->v.modelindex = (float) (2 + rnd () % 3);
			}
			else
				e->v.solid = SOLID_SLIDEBOX;
			break;
		}
		for (k = 0; k < 3; k++)
		{
			/* monster boxes: the shipped ones, a point, or anything */
			static const float	half[] = { 16, 16, 24, 40, 55, 8, 0, 12 };
			float	h = chance (80) ? half[rnd () % 8] : frand (0, 48);
			e->v.mins[k] = (k == 2) ? (chance (50) ? 0 : -h) : -h;
			e->v.maxs[k] = (k == 2) ? (chance (50) ? 2 * h : h) : h;
			e->v.size[k] = e->v.maxs[k] - e->v.mins[k];
		}
		place (e);
		SV_LinkEdict (e, false);
	}
}


/* A corner: a floor at z = 0 and walls at x = 64 and y = 64, solid beyond
 * them, as every hull of the world.  SV_FlyMove's dead stop in a crease of
 * three planes (it returns 7) needs a mover already at rest in one and still
 * pushing into it. */
static int	corner;

static void gen_corner (void)
{
	mplane_t	*pl = planes[1];
	mclipnode_t	*n = clipnodes[1][0];
	int		h, k;

	for (k = 0; k < 3; k++)
	{
		memset (&pl[k], 0, sizeof (pl[k]));
		pl[k].type = (byte) ((k + 2) % 3);	/* z, x, y */
		pl[k].normal[pl[k].type] = 1;
		pl[k].dist = k ? 64 : 0;
	}
	n[0].planenum = 0; n[0].children[0] = 1; n[0].children[1] = CONTENTS_SOLID;
	n[1].planenum = 1; n[1].children[0] = CONTENTS_SOLID; n[1].children[1] = 2;
	n[2].planenum = 2; n[2].children[0] = CONTENTS_SOLID; n[2].children[1] = CONTENTS_EMPTY;
	for (h = 0; h < MAX_MAP_HULLS; h++)
	{
		models[1].hulls[h].clipnodes = n;
		models[1].hulls[h].planes = pl;
		models[1].hulls[h].firstclipnode = 0;
		models[1].hulls[h].lastclipnode = 2;
	}
}

/* an entity's physics state before a helper runs */
static void gen_phys (edict_t *e)
{
	static const int	movetypes[] = { MOVETYPE_NONE, MOVETYPE_WALK, MOVETYPE_STEP, MOVETYPE_FLY,
					MOVETYPE_TOSS, MOVETYPE_PUSH, MOVETYPE_NOCLIP, MOVETYPE_FLYMISSILE,
					MOVETYPE_BOUNCE, MOVETYPE_BOUNCEMISSILE, MOVETYPE_SWIM };
	int	k, i = (int) (e - edicts);

	e->free = false;
	for (k = 0; k < 3; k++)
	{
		float	speed = chance (10) ? frand (1500, 4000) : frand (-600, 600);
		e->v.velocity[k] = chance (15) ? 0 : (chance (10) ? (float) ((int) (rnd () % 21) - 10) : speed);
	}
	if (chance (3))
		e->v.velocity[rnd () % 3] = odd_value ();
	e->v.movetype = (float) movetypes[rnd () % 11];
	if (chance (25))
	{
		static const int	solids[] = { SOLID_NOT, SOLID_TRIGGER, SOLID_BBOX, SOLID_SLIDEBOX, SOLID_PHASE };
		/* in-place, as QuakeC does it; the entity's links stay where they
		 * are.  Not to or from SOLID_BSP, which needs a brush model, and not
		 * to or from SOLID_TRIGGER: SOLID_NOT is linked in a solid list too, and
		 * a trigger in one is SV_Move's fatal (world_move's harness covers it) */
		int	s = solids[rnd () % 5];
		if (e->v.solid != SOLID_BSP && e->v.solid != SOLID_TRIGGER && s != SOLID_TRIGGER)
			e->v.solid = (float) s;
	}
	/* SV_HullForEntity's rule: a SOLID_BSP entity is a pusher */
	if (e->v.solid == SOLID_BSP)
		e->v.movetype = MOVETYPE_PUSH;
	e->v.flags = chance (40) ? FL_MONSTER : (chance (50) ? FL_ONGROUND : 0);
	e->v.touch = chance (60) ? 100 + i : 0;
	e->v.think = 200 + i;
	e->v.hoverz = chance (50) ? 0 : frand (-1, 1);
	gravity_has[i] = chance (50);
	gravity_field[i] = chance (30) ? 0 : frand (-2, 3);
	/* `val->_float' is true for NaN, so the entity falls at NaN */
	if (chance (3))
		gravity_field[i] = chance (50) ? NAN : -0.0f;
	{
		/* nextthink around the frame: past, now, this frame, the edge, next */
		double	edges[] = { -1, 0, sv.time - 1, sv.time, sv.time + host_frametime,
				    sv.time + host_frametime + 0.001, sv.time + 0.5 * host_frametime, sv.time + 5 };
		e->v.nextthink = chance (70) ? (float) edges[rnd () % 8] : frand (-1, (float) sv.time + 2);
		/* NaN fails both of SV_RunThink's tests, so the think runs */
		if (chance (3))
			e->v.nextthink = chance (50) ? NAN : -NAN;
	}
}

/* ------------------------------------------------------------------------ */
/* comparison                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
	edict_t		edicts[NE];
	areanode_t	nodes[AREA_NODES];
	__typeof__ (g)	g;
	char		dbg[16384];
	size_t		dbg_len;
	long		qc;
	int		ret;
	trace_t		trace;		/* SV_PushEntity's result, or SV_FlyMove's steptrace */
	vec3_t		clip_out;	/* ClipVelocity's out, when it is not the input */
} state_t;

static state_t	before, after_c, after_rust;

static void save (state_t *s, int ret)
{
	memcpy (s->edicts, edicts, sizeof (edicts));
	memcpy (s->nodes, sv_areanodes, sizeof (sv_areanodes));
	s->g = g;
	memcpy (s->dbg, dbg, sizeof (dbg));
	s->dbg_len = dbg_len;
	s->qc = n_qc;
	s->ret = ret;
}

static void restore (const state_t *s)
{
	memcpy (edicts, s->edicts, sizeof (edicts));
	memcpy (sv_areanodes, s->nodes, sizeof (sv_areanodes));
	g = s->g;
	memcpy (dbg, s->dbg, sizeof (dbg));
	dbg_len = s->dbg_len;
	n_qc = s->qc;
}

/* for the compare only (see the header) */
static void canon_state (state_t *s)
{
	int	k;
	for (k = 0; k < NE; k++)
	{
		entvars_t	*v = &s->edicts[k].v;
		canon_floats (v->origin, 3);
		canon_floats (v->velocity, 3);
		canon_floats (&v->flags, 1);
		canon_floats (v->absmin, 3);
		canon_floats (v->absmax, 3);
		canon_floats (&v->nextthink, 1);
		canon_floats (&v->hoverz, 1);
	}
	canon_floats (&s->g.time, 1);
	canon_floats (&s->trace.fraction, 1);
	canon_floats (s->trace.endpos, 3);
	canon_floats (s->trace.plane.normal, 3);
	canon_floats (&s->trace.plane.dist, 1);
	canon_floats (s->clip_out, 3);
}

static uint64_t digest (const state_t *s)
{
	const unsigned char	*p = (const unsigned char *) s;
	uint64_t		h = 1469598103934665603ULL;
	size_t			i;
	for (i = 0; i < offsetof (state_t, dbg); i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	for (i = 0; i < s->dbg_len; i++)
		h = (h ^ (unsigned char) s->dbg[i]) * 1099511628211ULL;
	for (i = offsetof (state_t, ret); i < sizeof (*s); i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	return h;
}

enum { K_CHECKVEL, K_RUNTHINK, K_IMPACT, K_CLIP, K_FLYMOVE, K_FLYEXTRAS, K_GRAVITY, K_PUSH, NKINDS };
static const char *const kind_names[NKINDS] =
	{ "SV_CheckVelocity", "SV_RunThink", "SV_Impact", "ClipVelocity", "SV_FlyMove",
	  "SV_FlyExtras", "SV_AddGravity", "SV_PushEntity" };

typedef struct
{
	int	kind;
	edict_t	*ent, *e2;
	vec3_t	vec, normal;
	float	f;
	int	flag;	/* ClipVelocity: out is the input; SV_FlyMove: pass a steptrace */
} call_t;

/* runs the call; the result lands in the live state plus *s */
static int run_one (const call_t *c, int rust, state_t *s)
{
	vec3_t	vec, normal;
	VectorCopy (c->vec, vec);
	VectorCopy (c->normal, normal);
	memset (&s->trace, 0, sizeof (s->trace));
	memset (s->clip_out, 0, sizeof (s->clip_out));
	switch (c->kind)
	{
	case K_CHECKVEL:
		if (rust) SV_CheckVelocity (c->ent); else c_SV_CheckVelocity (c->ent);
		return 0;
	case K_RUNTHINK:
		return rust ? SV_RunThink (c->ent) : c_SV_RunThink (c->ent);
	case K_IMPACT:
		if (rust) SV_Impact (c->ent, c->e2); else c_SV_Impact (c->ent, c->e2);
		return 0;
	case K_CLIP:
		if (c->flag)	/* SV_Physics_Toss's call: in and out are one vector */
			return rust ? ClipVelocity (c->ent->v.velocity, normal, c->ent->v.velocity, c->f)
				    : c_ClipVelocity (c->ent->v.velocity, normal, c->ent->v.velocity, c->f);
		return rust ? ClipVelocity (vec, normal, s->clip_out, c->f)
			    : c_ClipVelocity (vec, normal, s->clip_out, c->f);
	case K_FLYMOVE:
		return rust ? SV_FlyMove (c->ent, c->f, c->flag ? &s->trace : NULL)
			    : c_SV_FlyMove (c->ent, c->f, c->flag ? &s->trace : NULL);
	case K_FLYEXTRAS:
#ifndef SV_HW	/* HexenWorld's is under #if 0 (see sv_phys.rs) */
		if (rust) SV_FlyExtras (c->ent, c->f, NULL); else c_SV_FlyExtras (c->ent, c->f, NULL);
#endif
		return 0;
	case K_GRAVITY:
#ifdef SV_HW
		if (rust) SV_AddGravity (c->ent, c->f); else c_SV_AddGravity (c->ent, c->f);
#else
		if (rust) SV_AddGravity (c->ent); else c_SV_AddGravity (c->ent);
#endif
		return 0;
	default:
		s->trace = rust ? SV_PushEntity (c->ent, vec) : c_SV_PushEntity (c->ent, vec);
		return 0;
	}
}

static long	n_cases, n_fail, n_forked;
static long	st_kind[NKINDS], st_ret_hist[NKINDS][8], st_fatal, st_freed, st_qc, st_printed, st_capped;
static long	st_phase_through, st_bounce_stop, st_steptrace, st_crease;

static void report (long id, const char *what, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH case #%ld (%s): %s\n", id, what, detail);
}

/* Anything odd the call can reach: its own inputs, and any entity a trace
 * can meet -- a NaN origin elsewhere in the world gives the mover a NaN
 * fraction too.  "Odd" is non-finite or huge: odd_value's 1e30 is finite,
 * but the trace's plane distances overflow on it, and inf - inf is a NaN
 * fraction all the same. */
static int odd (float x)
{
	return !isfinite (x) || fabsf (x) > 1e9f;
}

static int has_odd (const call_t *c)
{
	int	k, e;
	for (k = 0; k < 3; k++)
		if (odd (c->vec[k]) || odd (c->normal[k]))
			return 1;
	for (e = 0; e < NE; e++)
		for (k = 0; k < 3; k++)
			if (odd (edicts[e].v.velocity[k]) || odd (edicts[e].v.origin[k]) ||
			    odd (edicts[e].v.absmin[k]) || odd (edicts[e].v.absmax[k]))
				return 1;
	return odd (c->f);
}

/* the call in a child; the result as text */
static void run_child (const call_t *c, int rust, char *out, size_t outsz)
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
		static state_t	s;
		char		buf[64];
		struct itimerval it = { { 0, 0 }, { 5, 0 } };
		int		ret;
		close (fds[0]);
		err_fd = fds[1];
		setitimer (ITIMER_PROF, &it, NULL);
		ret = run_one (c, rust, &s);
		{
			trace_t	t = s.trace;
			vec3_t	o;
			VectorCopy (s.clip_out, o);
			save (&s, ret);
			s.trace = t;
			VectorCopy (o, s.clip_out);
		}
		canon_state (&s);
		snprintf (buf, sizeof (buf), "RETURNED|%d|%016llx", ret, (unsigned long long) digest (&s));
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

static int has_odd (const call_t *c);

static void gen_call (call_t *c)
{
	int	k;
	memset (c, 0, sizeof (*c));
	c->kind = (int) (rnd () % NKINDS);
	/* SV_FlyMove has the most paths and is the one that forks; it gets a
	 * larger share */
	if ((c->kind == K_CHECKVEL || c->kind == K_IMPACT) && chance (25))
		c->kind = K_FLYMOVE;
#ifdef SV_HW
	if (c->kind == K_FLYEXTRAS)
		c->kind = K_FLYMOVE;
#endif
	c->ent = &edicts[1 + (int) (rnd () % (NE - 1))];
	c->e2 = &edicts[(int) (rnd () % NE)];
	gen_phys (c->ent);
	if (chance (50))
		gen_phys (c->e2), c->e2->free = false;
	for (k = 0; k < 3; k++)
		c->vec[k] = chance (20) ? 0 : (chance (50) ? (float) ((int) (rnd () % 81) - 40) : frand (-300, 300));
	/* a clip plane: axial, a floor at the 0.7 edge, a wall, or any unit */
	switch (rnd () % 5)
	{
	case 0: c->normal[rnd () % 3] = chance (50) ? 1 : -1; break;
	case 1: c->normal[2] = chance (50) ? 0.7f : 0.70000005f; c->normal[0] = sqrtf (1 - c->normal[2] * c->normal[2]); break;
	case 2: c->normal[0] = 0.6f; c->normal[1] = 0.8f; break;
	default:
		for (k = 0; k < 3; k++)
			c->normal[k] = frand (-1, 1);
		{
			float	l = sqrtf (DotProduct (c->normal, c->normal));
			if (l > 0)
				VectorScale (c->normal, 1 / l, c->normal);
		}
		break;
	}
	if (chance (2))
		c->vec[rnd () % 3] = odd_value ();
	c->flag = chance (50);
	switch (c->kind)
	{
	case K_CLIP:
		c->f = chance (50) ? 1 : (chance (50) ? 1.5f : frand (0, 2));
		/* components at ClipVelocity's STOP_EPSILON edge, which is the
		 * double 0.1: the floats either side of it, and 0.1f itself */
		if (chance (40))
		{
			static const float	edge[] = { 0.1f, 0.099999994f, 0.10000001f, 0.09999999f, 0.100000024f };
			for (k = 0; k < 3; k++)
				c->vec[k] = (chance (50) ? 1 : -1) * edge[rnd () % 5];
			if (chance (60))
			{
				VectorClear (c->normal);
				c->normal[rnd () % 3] = chance (50) ? 1 : -1;
			}
		}
		break;
	case K_FLYMOVE: c->f = chance (60) ? (float) host_frametime : frand (0, 0.5f); break;
	default: c->f = chance (70) ? 1 : frand (0, 2); break;
	}
	if (chance (1))
		c->f = odd_value ();
	/* ClipVelocity in place clips the entity's velocity: make it the input */
	if (c->kind == K_CLIP && c->flag)
		VectorCopy (c->vec, c->ent->v.velocity);
	/* SV_FlyMove into a floor, a wall, or the crease where two meet: on a
	 * terrain floor, astride a ledge's wall, falling and moving sideways */
	if (c->kind == K_FLYMOVE && chance (60))
	{
		place (c->ent);
		if (isfinite (c->ent->v.origin[2]))
			c->ent->v.origin[2] += chance (50) ? 0.5f : frand (1, 30);
		c->ent->v.velocity[2] = -frand (20, 600);
		for (k = 0; k < 2; k++)
			c->ent->v.velocity[k] = chance (30) ? 0 : frand (-400, 400);
		c->f = chance (50) ? (float) host_frametime : frand (0.02f, 0.3f);
		SV_LinkEdict (c->ent, false);
	}
	/* in a corner world, settle the mover into the corner first (on the
	 * Rust, before the snapshot both sides start from), then push on */
	if (c->kind == K_FLYMOVE && corner && chance (70) && !has_odd (c))
	{
		int	n;
		VectorSet (c->ent->v.origin, frand (0, 40), frand (0, 40), frand (40, 80));
		c->ent->v.solid = SOLID_SLIDEBOX;
		c->ent->v.movetype = MOVETYPE_FLY;
		c->ent->v.touch = 0;
		SV_LinkEdict (c->ent, false);
		/* the settling runs here, outside a child, which is why it is
		 * skipped when anything odd is in the world (has_odd): a NaN
		 * fraction there would be SV_FlyMove's fatal */
		for (n = 0; n < 4; n++)
		{
			VectorSet (c->ent->v.velocity, 400, 400, -400);
			SV_FlyMove (c->ent, 0.5f, NULL);
		}
		VectorSet (c->ent->v.velocity, frand (50, 400), frand (50, 400), -frand (50, 400));
		c->f = 0.1f;
	}
	/* hovering is decided at |velocity[2]| <= 6 */
	if (c->kind == K_FLYEXTRAS && chance (60))
		c->ent->v.velocity[2] = chance (30) ? (float) ((int) (rnd () % 15) - 7) : frand (-6.5f, 6.5f);
}

static void compare_case (void)
{
	call_t	c;
	int	rc, rr;
	char	msg[700];

	n_cases++;
	gen_call (&c);
	g.self = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.other = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.time = frand (0, 10);
	dbg_len = 0;
	dbg[0] = 0;
	save (&before, 0);

	/* only SV_FlyMove can reach a fatal (SV_Move's own are kept out of
	 * these worlds), so only it needs a child */
	if (c.kind == K_FLYMOVE && has_odd (&c))
	{
		char	oc[600], orr[600];
		n_forked++;
		run_child (&c, 0, oc, sizeof (oc));
		restore (&before);
		run_child (&c, 1, orr, sizeof (orr));
		restore (&before);
		if (strcmp (oc, orr) != 0)
		{
			snprintf (msg, sizeof (msg), "%s, non-finite: C \"%s\" Rust \"%s\"", kind_names[c.kind], oc, orr);
			report (n_cases, "forked", msg);
		}
		if (strncmp (oc, "CRASHED", 7) == 0)
			report (n_cases, "forked", "the C crashed or hung");
		if (strncmp (oc, WANT_ERROR_FN "|", sizeof (WANT_ERROR_FN)) == 0)
			st_fatal++;
		st_kind[c.kind]++;
		return;
	}

	rc = run_one (&c, 0, &after_c);
	{
		trace_t	t = after_c.trace;
		vec3_t	o;
		VectorCopy (after_c.clip_out, o);
		save (&after_c, rc);
		after_c.trace = t;
		VectorCopy (o, after_c.clip_out);
	}
	restore (&before);
	rr = run_one (&c, 1, &after_rust);
	{
		trace_t	t = after_rust.trace;
		vec3_t	o;
		VectorCopy (after_rust.clip_out, o);
		save (&after_rust, rr);
		after_rust.trace = t;
		VectorCopy (o, after_rust.clip_out);
	}
	canon_state (&after_c);
	canon_state (&after_rust);

	if (rc != rr)
	{
		snprintf (msg, sizeof (msg), "%s returns C %d Rust %d", kind_names[c.kind], rc, rr);
		report (n_cases, "return", msg);
	}
	if (memcmp (&after_c.trace, &after_rust.trace, sizeof (trace_t)) != 0)
	{
		snprintf (msg, sizeof (msg), "%s trace differs: frac C %a Rust %a", kind_names[c.kind],
			  after_c.trace.fraction, after_rust.trace.fraction);
		report (n_cases, "trace", msg);
	}
	if (memcmp (after_c.clip_out, after_rust.clip_out, sizeof (vec3_t)) != 0)
	{
		snprintf (msg, sizeof (msg), "ClipVelocity out C %a %a %a Rust %a %a %a",
			  after_c.clip_out[0], after_c.clip_out[1], after_c.clip_out[2],
			  after_rust.clip_out[0], after_rust.clip_out[1], after_rust.clip_out[2]);
		report (n_cases, "clip", msg);
	}
	if (memcmp (after_c.edicts, after_rust.edicts, sizeof (edicts)) != 0)
	{
		int	k;
		for (k = 0; k < NE; k++)
			if (memcmp (&after_c.edicts[k], &after_rust.edicts[k], sizeof (edict_t)) != 0)
				break;
		snprintf (msg, sizeof (msg), "%s: edict %d differs (velocity C %a %a %a, Rust %a %a %a; origin z C %a Rust %a)",
			  kind_names[c.kind], k,
			  after_c.edicts[k].v.velocity[0], after_c.edicts[k].v.velocity[1], after_c.edicts[k].v.velocity[2],
			  after_rust.edicts[k].v.velocity[0], after_rust.edicts[k].v.velocity[1], after_rust.edicts[k].v.velocity[2],
			  after_c.edicts[k].v.origin[2], after_rust.edicts[k].v.origin[2]);
		report (n_cases, "edicts", msg);
	}
	if (memcmp (after_c.nodes, after_rust.nodes, sizeof (sv_areanodes)) != 0)
		report (n_cases, "area nodes", "the relinks differ");
	if (memcmp (&after_c.g, &after_rust.g, sizeof (g)) != 0)
		report (n_cases, "globals", "self, other or time differ");
	if (after_c.dbg_len != after_rust.dbg_len || memcmp (after_c.dbg, after_rust.dbg, after_c.dbg_len) != 0)
	{
		snprintf (msg, sizeof (msg), "%s: the console and QuakeC calls differ\n--- C\n%.250s\n--- Rust\n%.250s",
			  kind_names[c.kind], after_c.dbg, after_rust.dbg);
		report (n_cases, "log", msg);
	}

	/* outcomes, from the C side */
	st_kind[c.kind]++;
	if (rc >= 0 && rc < 8)
		st_ret_hist[c.kind][rc]++;
	if (after_c.edicts[c.ent - edicts].free)
		st_freed++;
	if (after_c.qc != before.qc)
		st_qc++;
	if (strstr (after_c.dbg, "Got a NaN"))
		st_printed++;
	if (c.kind == K_CHECKVEL && VectorLength (before.edicts[c.ent - edicts].v.velocity) > sv_maxvelocity.value)
		st_capped++;
	if (c.kind == K_PUSH && before.edicts[c.ent - edicts].v.solid == SOLID_PHASE && after_c.trace.ent &&
	    ((int) after_c.trace.ent->v.flags & FL_MONSTER))
		st_phase_through++;
	if (c.kind == K_PUSH && before.edicts[c.ent - edicts].v.movetype == MOVETYPE_BOUNCE && after_c.trace.fraction == 0 &&
	    (after_c.trace.allsolid || after_c.trace.startsolid))
		st_bounce_stop++;
	if (c.kind == K_FLYMOVE && c.flag && after_c.trace.ent)
		st_steptrace++;
	if (c.kind == K_FLYMOVE && rc == 7)
		st_crease++;
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

	if (Phys_TargetFacts.v_hoverz != offsetof (entvars_t, hoverz) ||
	    Phys_TargetFacts.movetype_swim != MOVETYPE_SWIM ||
	    World_TargetFacts.v_classname != offsetof (entvars_t, classname))
	{
		fprintf (stderr, "harness bug: the target shims are not this build's\n");
		return 2;
	}
	sv_globals.self = &g.self;
	sv_globals.other = &g.other;
	sv_globals.world = &g.world;
	sv_globals.time = &g.time;
	sv_maxvelocity.value = 2000;
#ifdef SV_HW
	movevars.gravity = 800;
#else
	sv_gravity.value = 800;
#endif

	for (iw = 0; iw < 3000; iw++)
	{
		gen_world ();
		corner = chance (12);
		if (corner)
			gen_corner ();
		gen_edicts ();
		host_frametime = chance (60) ? 1.0 / 72 : (chance (50) ? 0.05 : (double) frand (0.001f, 0.1f));
		sv.time = chance (50) ? 1.0 + iw * 0.1 : (double) frand (0, 100);
		sv_player = &edicts[1 + (int) (rnd () % (NE - 1))];
		sv_maxvelocity.value = chance (80) ? 2000 : frand (100, 3000);
#ifdef SV_HW
		movevars.gravity = chance (80) ? 800 : frand (0, 1600);
#else
		sv_gravity.value = chance (80) ? 800 : frand (0, 1600);
#endif
		for (k = 0; k < 60; k++)
			compare_case ();
	}

	printf ("outcomes: calls by helper:");
	for (k = 0; k < NKINDS; k++)
		printf (" %s %ld", kind_names[k], st_kind[k]);
	printf ("\n          SV_FlyMove returns 0/1/2/3/7: %ld/%ld/%ld/%ld/%ld; crease stops %ld; steptraces %ld\n"
		"          SV_RunThink true/false %ld/%ld; QuakeC calls in %ld cases; entities freed %ld\n"
		"          NaN reports %ld; velocities capped %ld; phased through a monster %ld; bounce stops %ld\n"
		"          non-finite: %ld forked cases, %ld reached the fatal\n",
		st_ret_hist[K_FLYMOVE][0], st_ret_hist[K_FLYMOVE][1], st_ret_hist[K_FLYMOVE][2], st_ret_hist[K_FLYMOVE][3],
		st_ret_hist[K_FLYMOVE][7], st_crease, st_steptrace,
		st_ret_hist[K_RUNTHINK][1], st_ret_hist[K_RUNTHINK][0], st_qc, st_freed,
		st_printed, st_capped, st_phase_through, st_bounce_stop, n_forked, st_fatal);

	for (k = 0; k < NKINDS; k++)
	{
#ifdef SV_HW
		if (k == K_FLYEXTRAS)
			continue;
#endif
		floor_check (kind_names[k], st_kind[k], 15000);
	}
	floor_check ("an SV_FlyMove that returns 1 (floor)", st_ret_hist[K_FLYMOVE][1], 500);
	floor_check ("an SV_FlyMove that returns 2 (wall)", st_ret_hist[K_FLYMOVE][2], 500);
	floor_check ("an SV_FlyMove that returns 3", st_ret_hist[K_FLYMOVE][3], 500);
	floor_check ("an SV_FlyMove crease stop (7)", st_crease, 50);
	floor_check ("a stored steptrace", st_steptrace, 200);
	floor_check ("an SV_RunThink that returns false", st_ret_hist[K_RUNTHINK][0], 200);
	floor_check ("a freed entity", st_freed, 500);
	floor_check ("a NaN report", st_printed, 200);
	floor_check ("a capped velocity", st_capped, 500);
	floor_check ("a phase through a monster", st_phase_through, 20);
	floor_check ("a bounce stop", st_bounce_stop, 50);
	floor_check ("a forked non-finite case", n_forked, 3000);
	floor_check ("SV_FlyMove's !trace.ent fatal", st_fatal, 50);

	if (n_fail)
	{
		printf ("sv_phys differential harness (%s): FAIL, %ld problem(s) in %ld cases\n",
			VARIANT_NAME, n_fail, n_cases);
		return 1;
	}
	printf ("sv_phys differential harness (%s): PASS (%ld cases: %ld in-process, %ld forked non-finite)\n",
		VARIANT_NAME, n_cases, n_cases - n_forked, n_forked);
	return 0;
}
