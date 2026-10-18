/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/sv_phys_push.rs against the C original of
 * sv_phys.c's pushers, in one binary.
 *
 * run_diff_harness.sh (with SLICE=push) builds this against
 * sv_phys_push_h2.c (-DSERVERONLY, as h2ded compiles it) and
 * sv_phys_push_hw.c (-DH2W -DSERVERONLY -DSV_HW, as hwsv).  The real
 * engine/rust/world_target.c and phys_target.c are compiled in.
 *
 * The worlds are the sv_phys harness's.  Each case makes one of the brush
 * entities a pusher -- SOLID_BSP, MOVETYPE_PUSH, a velocity and sometimes an
 * avelocity (zero, small, large, odd), ltime and nextthink around the frame,
 * a `blocked' function or not, sometimes a liquid (watertype) -- and gathers
 * entities around it: riders standing on it (FL_ONGROUND, groundentity),
 * entities it overlaps or will, players (index <= svs.maxclients, for
 * sv_gameplayfix_elevators), corpses (SOLID_NOT, SOLID_TRIGGER), point-sized
 * entities, and every movetype the pushers skip.  Then it calls
 * SV_Physics_Pusher (most cases), SV_PushMove, SV_PushRotate, or
 * HexenWorld's SV_Push, once on the C and once on the Rust from the same
 * snapshot, and compares byte for byte: every edict, the area nodes,
 * self/other/time, the return value, and every console line and QuakeC call
 * in order (the blocked, think and touch functions).
 *
 * sv_gameplayfix_elevators takes 0, 1 and 2, map_checks 0 and 1, and
 * svs.maxclients spans the entity range, so the lift nudge runs both ways.
 * The QuakeC stub acts as in the sv_phys harness: it frees self or other the
 * way ED_Free does, moves self, takes other's touch or solid away, and
 * changes velocity and flags -- here inside a push, from a touch
 * (SV_PushEntity's SV_Impact) or a blocked function.
 *
 * WHAT THE REFERENCE CALLS.  SV_PushEntity is the sv_phys slice's Rust;
 * SV_Move, SV_TestEntityPosition, SV_LinkEdict and AngleVectors the earlier
 * ports'; each is held to its C by its own harness.  So this compares the
 * pushers' own logic over shared callees.  None of them has a fatal of its
 * own and the worlds keep SV_Move's out, so every case runs in-process; NaN
 * signs are canonicalised as in the other harnesses.
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

void		c_SV_PushMove (edict_t *pusher, float movetime, qboolean update_time);
void		c_SV_PushRotate (edict_t *pusher, float movetime);
void		c_SV_Physics_Pusher (edict_t *ent);
#ifdef SV_HW
qboolean	c_SV_Push (edict_t *pusher, vec3_t move);
#define VARIANT_NAME	"hexenworld"
#else
#define VARIANT_NAME	"hexen2"
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

/* pr_edict.c's NUM_FOR_EDICT, for a valid entity; the pushers only ever
 * pass one */
int NUM_FOR_EDICT (edict_t *e)
{
	return (int) (e - edicts);
}

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
 * touch or make it SOLID_NOT, change other's flags, spawn an entity (into
 * the slots reserved past sv.num_edicts), and move self by a hair: the
 * edge of HexenWorld's 1/64 post-think snap.  A port that cached a field
 * across the call -- sv.num_edicts among them -- instead of re-reading it as
 * the C does, differs. */
void PR_ExecuteProgram (func_t fnum, const char *funcname)
{
	edict_t	*self = PROG_TO_EDICT(*sv_globals.self);
	edict_t	*other = PROG_TO_EDICT(*sv_globals.other);
	char	t[48];

	n_qc++;
	snprintf (t, sizeof (t), "%a", *sv_globals.time);
	canon_text (t);
	dbg_add ("Q %d %s self e%ld other e%ld time %s\n", fnum, funcname, idx (self), idx (other), t);
	switch (n_qc % 18)
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
	case 14:	/* spawn () */
		if (sv.num_edicts < NE)
		{
			edict_t	*e = &edicts[sv.num_edicts];
			memset (&e->v, 0, sizeof (e->v));
			e->free = false;
			e->v.solid = SOLID_SLIDEBOX;
			e->v.movetype = MOVETYPE_STEP;
			VectorSet (e->v.mins, -8, -8, 0);
			VectorSet (e->v.maxs, 8, 8, 16);
			VectorSubtract (e->v.maxs, e->v.mins, e->v.size);
			VectorCopy (self->v.origin, e->v.origin);
			e->v.origin[2] += (n_qc / 18) % 2 ? 4 : 20;
			e->v.classname = 900 + sv.num_edicts;
			sv.num_edicts++;
			SV_LinkEdict (e, false);
		}
		break;
	case 15:	/* a hair: at, above and below 1/64 */
	{
		static const float	hair[] = { 0.015625f, 0.015625002f, 0.0156249991f, 0.01f, 0.02f };
		self->v.origin[0] += hair[(n_qc / 18) % 5];
		if (!self->free && self != sv.edicts)
			SV_LinkEdict (self, false);
		break;
	}
	case 16:	/* from 0, so the move is exact: just over 1/64 */
		if (self->v.origin[0] == 0 && self->v.origin[1] == 0 && self->v.origin[2] == 0)
		{
			VectorSet (self->v.origin, 0.015625f, (n_qc / 18) % 2 ? 5e-6f : 0, 0);
			if (!self->free && self != sv.edicts)
				SV_LinkEdict (self, false);
		}
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


/* ------------------------------------------------------------------------ */
/* the pusher and its neighbours                                            */
/* ------------------------------------------------------------------------ */

static long	st_liquid, st_push_false, st_qc_in_push, st_plat, st_crush, st_spawned, st_pusher_not_bsp;

/* The last NSPARE slots are past sv.num_edicts, free and unlinked, for the
 * QuakeC stub's spawn () to fill. */
#define NSPARE	4

static void reset_spares (void)
{
	int	k;
	for (k = NE - NSPARE; k < NE; k++)
	{
		if (edicts[k].area.prev)
			SV_UnlinkEdict (&edicts[k]);
		edicts[k].free = true;
	}
	sv.num_edicts = NE - NSPARE;
}

static int is_brush (const edict_t *e)
{
	return e->v.modelindex >= 2 && e->v.movetype == MOVETYPE_PUSH;
}

static void gen_neighbour (edict_t *e, edict_t *pusher)
{
	static const int	movetypes[] = { MOVETYPE_WALK, MOVETYPE_STEP, MOVETYPE_STEP, MOVETYPE_TOSS,
					MOVETYPE_FLY, MOVETYPE_BOUNCE, MOVETYPE_FLYMISSILE, MOVETYPE_NONE,
					MOVETYPE_NOCLIP, MOVETYPE_FOLLOW, MOVETYPE_PUSH };
	int	k;

	if (e->v.solid == SOLID_BSP || is_brush (e))
		return;		/* the other brush entities stay as they are */
	e->free = chance (3);
	e->v.movetype = (float) movetypes[rnd () % 11];
	if (e->v.movetype == MOVETYPE_PUSH)
		e->v.movetype = MOVETYPE_STEP;	/* SV_HullForEntity: a pusher is SOLID_BSP */
	e->v.flags = chance (50) ? FL_ONGROUND : 0;
	if (chance (30)) e->v.flags = (float) ((int) e->v.flags | FL_MONSTER);
	/* standing on the pusher, on something else, or on nothing */
	e->v.groundentity = chance (50) ? (int) ((byte *) pusher - (byte *) sv.edicts)
					 : (int) (rnd () % NE) * (int) sizeof (edict_t);
	e->v.touch = chance (40) ? 100 + (int) (e - edicts) : 0;
	/* near the pusher's box: on top, inside, beside, or anywhere */
	switch (rnd () % 4)
	{
	case 0:
		for (k = 0; k < 2; k++)
			e->v.origin[k] = frand (pusher->v.absmin[k], pusher->v.absmax[k]);
		e->v.origin[2] = pusher->v.absmax[2] - e->v.mins[2] + (chance (50) ? 0 : frand (-2, 2));
		break;
	case 1:
		for (k = 0; k < 3; k++)
			e->v.origin[k] = frand (pusher->v.absmin[k], pusher->v.absmax[k]);
		break;
	case 2:
		for (k = 0; k < 3; k++)
			e->v.origin[k] = (chance (50) ? pusher->v.absmin[k] - e->v.maxs[k] : pusher->v.absmax[k] - e->v.mins[k])
					 + frand (-4, 4);
		break;
	default:
		break;
	}
	if (chance (8))	/* point-sized: mins == maxs */
	{
		VectorClear (e->v.mins);
		VectorClear (e->v.maxs);
	}
	if (chance (2))
		e->v.origin[rnd () % 3] = odd_value ();
	SV_LinkEdict (e, false);
	/* corpses: SOLID_NOT / SOLID_TRIGGER, in place (a corpse is linked as
	 * a trigger only if it was one when linked) */
	if (chance (10) && e->v.solid != SOLID_TRIGGER)
		e->v.solid = SOLID_NOT;
}

static void gen_pusher (edict_t *p)
{
	int	k;
	static const float	speeds[] = { 0, 0, 8, -8, 32, 100, -100, 250 };

	p->free = false;
	/* mostly SOLID_BSP, but MOVETYPE_PUSH does not require it (only the
	 * reverse is fatal): what the pushers restore while they push is the
	 * pusher's own solid in Hexen II (uhexen2-3aus), SOLID_BSP in HexenWorld */
	p->v.solid = chance (75) ? SOLID_BSP : (chance (50) ? SOLID_NOT : SOLID_TRIGGER);
	p->v.movetype = MOVETYPE_PUSH;
	/* at the origin, as many a func_door is: a think's move from there is
	 * exact, which reaches the 1/64 snap's edge */
	if (chance (15))
		VectorClear (p->v.origin);
	for (k = 0; k < 3; k++)
	{
		p->v.velocity[k] = chance (60) ? speeds[rnd () % 8] : frand (-300, 300);
		p->v.avelocity[k] = 0;
	}
	if (chance (35))	/* a rotator */
		for (k = 0; k < 3; k++)
			p->v.avelocity[k] = chance (50) ? 0 : (chance (50) ? frand (-200, 200) : (float) ((int) (rnd () % 7) - 3) * 45);
	if (chance (2))
		p->v.velocity[rnd () % 3] = odd_value ();
	p->v.ltime = chance (30) ? 0 : frand (0, 50);
	{
		/* nextthink: before, at and after this frame's push, in ltime */
		double	edges[] = { -1, 0, p->v.ltime, p->v.ltime + host_frametime, p->v.ltime + 0.5 * host_frametime,
				    p->v.ltime + host_frametime + 0.001, p->v.ltime - 0.01, p->v.ltime + 10 };
		p->v.nextthink = chance (80) ? (float) edges[rnd () % 8] : frand (-1, p->v.ltime + 1);
		if (chance (2))
			p->v.nextthink = NAN;
	}
	p->v.think = 300 + (int) (p - edicts);
	p->v.blocked = chance (60) ? 400 + (int) (p - edicts) : 0;
	p->v.watertype = chance (10) ? (chance (50) ? CONTENTS_WATER : CONTENTS_LAVA) : 0;
	SV_LinkEdict (p, false);
}

/* A plat: model 2 a 64 x 64 x 16 box, in two hulls built consistently --
 * the point hull the box itself, the player hull (clip box the rider's own,
 * -16 -16 -24 .. 16 16 32) the box grown by it -- and an empty world, so
 * nothing but the plat can block its rider.  The rider stands on it sunk
 * 0.01 into the top, inside DIST_EPSILON: the lift case
 * sv_gameplayfix_elevators exists for. */
static void box_tree (int m, int h, int first_plane, const vec3_t lo, const vec3_t hi)
{
	mplane_t	*pl = &planes[m][first_plane];
	mclipnode_t	*n = clipnodes[m][h];
	int		k;

	for (k = 0; k < 6; k++)
	{
		memset (&pl[k], 0, sizeof (pl[k]));
		pl[k].type = (byte) (k / 2);
		pl[k].normal[k / 2] = 1;
		pl[k].dist = (k & 1) ? lo[k / 2] : hi[k / 2];
	}
	/* even planes: beyond the high face is empty; odd: beyond the low face
	 * is inside the box, below it empty; the last is solid */
	for (k = 0; k < 6; k++)
	{
		n[k].planenum = first_plane + k;
		if (k & 1)
		{
			n[k].children[0] = (k == 5) ? CONTENTS_SOLID : k + 1;
			n[k].children[1] = CONTENTS_EMPTY;
		}
		else
		{
			n[k].children[0] = CONTENTS_EMPTY;
			n[k].children[1] = k + 1;
		}
	}
	models[m].hulls[h].clipnodes = n;
	models[m].hulls[h].planes = planes[m];
	models[m].hulls[h].firstclipnode = 0;
	models[m].hulls[h].lastclipnode = 5;
}

/* WHY THE WORLD.  Hexen II's SV_TestEntityPosition returns sv.edicts for
 * any stuck entity, never the entity it is stuck in (Ironwail's returns
 * trace.ent; that half of its elevator fix was not taken).  So SV_PushMove's
 * `block == pusher' holds only when the pusher is the world itself, and that
 * is the one way sv_gameplayfix_elevators' nudge runs at all -- the world
 * made a moving plat.  The case does that half the time; the other half the
 * plat is a brush entity, where the nudge cannot run. */
static void gen_plat (edict_t *p, edict_t *rider)
{
	static const vec3_t	box_lo = { -32, -32, 0 }, box_hi = { 32, 32, 16 };
	static const vec3_t	rmins = { -16, -16, -24 }, rmaxs = { 16, 16, 32 };
	vec3_t			lo, hi;
	int			h, k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = box_lo[k] - rmaxs[k];
		hi[k] = box_hi[k] - rmins[k];
	}
	int	m = (p == sv.edicts) ? 1 : 2;

	box_tree (m, 0, 0, box_lo, box_hi);
	box_tree (m, 1, 6, lo, hi);
	VectorCopy (rmins, models[m].hulls[1].clip_mins);
	VectorCopy (rmaxs, models[m].hulls[1].clip_maxs);
	VectorClear (models[m].hulls[0].clip_mins);
	VectorClear (models[m].hulls[0].clip_maxs);
	for (h = 2; h < MAX_MAP_HULLS; h++)
		models[m].hulls[h] = models[m].hulls[1];
	/* a brush plat: the world one empty leaf in every hull */
	if (m == 2)
		for (h = 0; h < MAX_MAP_HULLS; h++)
		{
			models[1].hulls[h].lastclipnode = 0;
			models[1].hulls[h].firstclipnode = 0;
			clipnodes[1][h][0].children[0] = CONTENTS_EMPTY;
			clipnodes[1][h][0].children[1] = CONTENTS_EMPTY;
			models[1].hulls[h].clipnodes = clipnodes[1][h];
		}

	int	riding = (m == 2) || chance (60);

	p->v.modelindex = (float) m;
	p->v.solid = SOLID_BSP;
	p->v.movetype = MOVETYPE_PUSH;
	p->v.ltime = 0;
	p->v.nextthink = 0;
	VectorCopy (box_lo, p->v.mins);
	VectorCopy (box_hi, p->v.maxs);
	VectorSubtract (box_hi, box_lo, p->v.size);
	VectorSet (p->v.origin, frand (-100, 100), frand (-100, 100), frand (-50, 50));
	/* A stuck entity that is not riding reaches the nudge's `riding' test
	 * only past SV_PushMove's box test, against the world's absmin/absmax --
	 * never linked, so the move's point.  Placed so its box covers that
	 * point: the world plat near the origin, the rider over it. */
	if (!riding)
		VectorSet (p->v.origin, frand (-6, 6), frand (-6, 6), frand (-17, -15));
	VectorSet (p->v.velocity, 0, 0, chance (80) ? frand (20, 200) : frand (-200, -20));
	VectorClear (p->v.avelocity);
	p->v.watertype = 0;
	SV_LinkEdict (p, false);

	rider->free = false;
	rider->v.solid = SOLID_SLIDEBOX;
	rider->v.movetype = chance (60) ? MOVETYPE_WALK : MOVETYPE_STEP;
	rider->v.flags = (riding ? FL_ONGROUND : 0) | (chance (30) ? FL_CLIENT : 0);
	rider->v.groundentity = riding ? (int) ((byte *) p - (byte *) sv.edicts) : (int) sizeof (edict_t);
	rider->v.hull = 0;
	VectorCopy (rmins, rider->v.mins);
	VectorCopy (rmaxs, rider->v.maxs);
	VectorSubtract (rmaxs, rmins, rider->v.size);
	VectorSet (rider->v.origin, p->v.origin[0] + frand (-8, 8), p->v.origin[1] + frand (-8, 8),
		   p->v.origin[2] + box_hi[2] - rmins[2] - (chance (70) ? 0.01f : frand (0, 0.5f)));
	rider->v.touch = 0;
	SV_LinkEdict (rider, false);
}

/* The crush.  The plat (model 2, gen_plat's box) rises at velocity v; the
 * world is a ceiling over its +y half only (a y plane, then a z plane), so
 * rider a, on the -y half, rides up freely, and entity b, on the +y half,
 * not riding but inside the plat's next position, is pinned by the ceiling.
 * a is visited first (lower index), so the push of b fails with a already
 * moved, and the restore has to put a back too. */
static void gen_crush (edict_t *p, edict_t *a, edict_t *b)
{
	static const vec3_t	rmins = { -16, -16, -24 }, rmaxs = { 16, 16, 32 };
	float			m, ceiling;
	int			h;

	gen_plat (p, a);	/* the plat, and a riding it */
	if (p == sv.edicts)
		return;
	a->v.origin[1] = p->v.origin[1] - 20;
	/* half a unit above the top, still riding (FL_ONGROUND, groundentity):
	 * flush, `(a.z + m) - (p.z + m)' can round a hair inside the plat, and
	 * then a blocks first and is the only entity moved */
	a->v.origin[2] = p->v.origin[2] + 16 - rmins[2] + 0.5f;
	a->v.flags = FL_ONGROUND;
	SV_LinkEdict (a, false);

	p->v.velocity[2] = frand (100, 300);
	m = p->v.velocity[2] * (float) host_frametime;

	b->free = false;
	b->v.solid = SOLID_SLIDEBOX;
	b->v.movetype = MOVETYPE_STEP;
	b->v.flags = 0;
	b->v.hull = 0;
	b->v.touch = 0;
	VectorCopy (rmins, b->v.mins);
	VectorCopy (rmaxs, b->v.maxs);
	VectorSubtract (rmaxs, rmins, b->v.size);
	VectorSet (b->v.origin, p->v.origin[0], p->v.origin[1] + 20, p->v.origin[2] + 16 - rmins[2] + 0.5f * m);
	SV_LinkEdict (b, false);

	/* the world: y > plat's y and z > ceiling is solid, in every hull, with
	 * the player hull's clip box the movers' own, so the planes bound their
	 * origins exactly */
	ceiling = b->v.origin[2] + 0.25f * m;
	memset (&planes[1][0], 0, 2 * sizeof (mplane_t));
	planes[1][0].type = 1; planes[1][0].normal[1] = 1; planes[1][0].dist = p->v.origin[1];
	planes[1][1].type = 2; planes[1][1].normal[2] = 1; planes[1][1].dist = ceiling;
	clipnodes[1][0][0].planenum = 0;
	clipnodes[1][0][0].children[0] = 1;
	clipnodes[1][0][0].children[1] = CONTENTS_EMPTY;
	clipnodes[1][0][1].planenum = 1;
	clipnodes[1][0][1].children[0] = CONTENTS_SOLID;
	clipnodes[1][0][1].children[1] = CONTENTS_EMPTY;
	for (h = 0; h < MAX_MAP_HULLS; h++)
	{
		models[1].hulls[h].clipnodes = clipnodes[1][0];
		models[1].hulls[h].planes = planes[1];
		models[1].hulls[h].firstclipnode = 0;
		models[1].hulls[h].lastclipnode = 1;
		VectorCopy (rmins, models[1].hulls[h].clip_mins);
		VectorCopy (rmaxs, models[1].hulls[h].clip_maxs);
	}
	st_crush++;
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
	int		num_edicts;
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
	s->num_edicts = sv.num_edicts;
}

static void restore (const state_t *s)
{
	memcpy (edicts, s->edicts, sizeof (edicts));
	memcpy (sv_areanodes, s->nodes, sizeof (sv_areanodes));
	g = s->g;
	memcpy (dbg, s->dbg, sizeof (dbg));
	dbg_len = s->dbg_len;
	n_qc = s->qc;
	sv.num_edicts = s->num_edicts;
}

static void canon_state (state_t *s)
{
	int	k;
	for (k = 0; k < NE; k++)
	{
		entvars_t	*v = &s->edicts[k].v;
		canon_floats (v->origin, 3);
		canon_floats (v->angles, 3);
		canon_floats (v->velocity, 3);
		canon_floats (&v->flags, 1);
		canon_floats (v->absmin, 3);
		canon_floats (v->absmax, 3);
		canon_floats (v->mins, 3);
		canon_floats (v->maxs, 3);
		canon_floats (&v->ltime, 1);
		canon_floats (&v->nextthink, 1);
	}
	canon_floats (&s->g.time, 1);
}

enum { K_PHYSICS, K_PUSHMOVE, K_PUSHROTATE, K_PUSH, NKINDS };
static const char *const kind_names[NKINDS] =
	{ "SV_Physics_Pusher", "SV_PushMove", "SV_PushRotate", "SV_Push" };

static int run_one (int kind, edict_t *p, float movetime, int update_time, const vec3_t move0, int rust)
{
	vec3_t	move;
	VectorCopy (move0, move);
	switch (kind)
	{
	case K_PHYSICS:
		if (rust) SV_Physics_Pusher (p); else c_SV_Physics_Pusher (p);
		return 0;
	case K_PUSHMOVE:
		if (rust) SV_PushMove (p, movetime, update_time); else c_SV_PushMove (p, movetime, update_time);
		return 0;
	case K_PUSHROTATE:
		if (rust) SV_PushRotate (p, movetime); else c_SV_PushRotate (p, movetime);
		return 0;
	default:
#ifdef SV_HW
		return rust ? SV_Push (p, move) : c_SV_Push (p, move);
#else
		(void) move;
		return 0;
#endif
	}
}

static long	n_cases, n_fail;
static long	st_kind[NKINDS], st_blocked, st_pushed, st_rider, st_nudged, st_corpse, st_rotated, st_think;


static void report (long id, const char *what, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH case #%ld (%s): %s\n", id, what, detail);
}

static void compare_case (void)
{
	int	kind, k, rc, rr, pi, update_time = chance (80);
	edict_t	*p;
	float	movetime;
	vec3_t	move;
	char	msg[900];

	n_cases++;
	kind = chance (55) ? K_PHYSICS : (int) (1 + rnd () % 3);
#ifndef SV_HW
	if (kind == K_PUSH)
		kind = K_PUSHMOVE;
#endif
	reset_spares ();
	/* the pusher: one of the brush entities (gen_edicts makes some of 1..5),
	 * found by its model, since its solid may not be SOLID_BSP */
	for (pi = 1 + (int) (rnd () % 5), k = 0; k < 5 && !is_brush (&edicts[pi]); k++)
		pi = 1 + (pi % 5);
	if (!is_brush (&edicts[pi]))
		return;
	p = &edicts[pi];
	gen_pusher (p);
	for (k = 1; k < NE - NSPARE; k++)
		if (k != pi && chance (70))
			gen_neighbour (&edicts[k], p);
	/* a crush: a plat carrying one rider past a ceiling that pins a second
	 * entity, so the push fails with another entity already moved */
	int	crush = 0;
	if (chance (8) && pi < NE - NSPARE - 2)
	{
		gen_crush (p, &edicts[pi + 1], &edicts[pi + 2]);
		if (kind == K_PUSHROTATE)
			kind = K_PUSHMOVE;
		crush = 1;
	}
	/* the lift case, sometimes; the rider is a player index or not */
#ifndef SV_HW
	if (chance (12))
	{
		int	ri = 1 + (int) (rnd () % (NE - 1));
		if (chance (50))	/* the world as the plat (see gen_plat) */
		{
			pi = 0;
			p = sv.edicts;
		}
		if (ri == pi)
			ri = 1 + (ri % (NE - 1));
		gen_plat (p, &edicts[ri]);
		if (kind == K_PUSHROTATE)
			kind = K_PUSHMOVE;
	}
#endif
	movetime = chance (60) ? (float) host_frametime : (chance (50) ? 0 : frand (0.001f, 0.2f));
	/* the crush's geometry is laid out for one frame's move, so every call
	 * kind gets one: SV_PushMove's movetime, and SV_Physics_Pusher's (a
	 * nextthink past the frame makes it host_frametime) */
	if (crush)
	{
		movetime = (float) host_frametime;
		update_time = 1;
		p->v.nextthink = p->v.ltime + 10;
	}
	for (k = 0; k < 3; k++)
		move[k] = p->v.velocity[k] * (float) host_frametime;
#ifndef SV_HW
	svs.maxclients = (int) (rnd () % (NE + 1));
	{
		/* the C reads it as a float: 0.5 and 1.5 are "players only" */
		static const float	ev[] = { 0, 1, 2, 0.5f, 1.5f, 3 };
		sv_gameplayfix_elevators.value = ev[rnd () % 6];
	}
	map_checks.integer = chance (30);
#endif

	g.self = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.other = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.time = frand (0, 10);
	dbg_len = 0;
	dbg[0] = 0;
	save (&before, 0);

	rc = run_one (kind, p, movetime, update_time, move, 0);
	save (&after_c, rc);
	restore (&before);
	rr = run_one (kind, p, movetime, update_time, move, 1);
	save (&after_rust, rr);
	canon_state (&after_c);
	canon_state (&after_rust);

	if (rc != rr)
	{
		snprintf (msg, sizeof (msg), "%s returns C %d Rust %d", kind_names[kind], rc, rr);
		report (n_cases, "return", msg);
	}
	if (memcmp (after_c.edicts, after_rust.edicts, sizeof (edicts)) != 0)
	{
		for (k = 0; k < NE; k++)
			if (memcmp (&after_c.edicts[k], &after_rust.edicts[k], sizeof (edict_t)) != 0)
				break;
		snprintf (msg, sizeof (msg), "%s: edict %d differs (origin C %a %a %a, Rust %a %a %a; ltime C %a Rust %a)",
			  kind_names[kind], k,
			  after_c.edicts[k].v.origin[0], after_c.edicts[k].v.origin[1], after_c.edicts[k].v.origin[2],
			  after_rust.edicts[k].v.origin[0], after_rust.edicts[k].v.origin[1], after_rust.edicts[k].v.origin[2],
			  after_c.edicts[k].v.ltime, after_rust.edicts[k].v.ltime);
		report (n_cases, "edicts", msg);
	}
	if (memcmp (after_c.nodes, after_rust.nodes, sizeof (sv_areanodes)) != 0)
		report (n_cases, "area nodes", "the relinks differ");
	if (memcmp (&after_c.g, &after_rust.g, sizeof (g)) != 0)
		report (n_cases, "globals", "self, other or time differ");
	if (after_c.dbg_len != after_rust.dbg_len || memcmp (after_c.dbg, after_rust.dbg, after_c.dbg_len) != 0)
	{
		snprintf (msg, sizeof (msg), "%s: the console and QuakeC calls differ\n--- C\n%.300s\n--- Rust\n%.300s",
			  kind_names[kind], after_c.dbg, after_rust.dbg);
		report (n_cases, "log", msg);
	}

	/* outcomes, from the C side */
	st_kind[kind]++;
	if (strstr (after_c.dbg, " blocked "))
		st_blocked++;
	if (strstr (after_c.dbg, " think in "))
		st_think++;
	if (strstr (after_c.dbg, "nudged"))
		st_nudged++;
	if (strstr (after_c.dbg, " touch "))
		st_qc_in_push++;
	if (kind == K_PUSH && rc == 0)
		st_push_false++;
	if (before.edicts[pi].v.modelindex <= 2 && before.edicts[pi].v.mins[0] == -32 && before.edicts[pi].v.maxs[2] == 16)
		st_plat++;
	if (after_c.num_edicts > before.num_edicts)
		st_spawned++;
	if (before.edicts[pi].v.solid != SOLID_BSP)
		st_pusher_not_bsp++;
	if (p->v.watertype != 0 && kind != K_PUSHROTATE)
		st_liquid++;
	if (before.edicts[pi].v.avelocity[0] || before.edicts[pi].v.avelocity[1] || before.edicts[pi].v.avelocity[2])
		st_rotated++;
	for (k = 1; k < NE; k++)
	{
		if (k == pi)
			continue;
		if (memcmp (before.edicts[k].v.origin, after_c.edicts[k].v.origin, sizeof (vec3_t)) != 0)
			st_pushed++;
		if (before.edicts[k].v.mins[0] != after_c.edicts[k].v.mins[0] && after_c.edicts[k].v.mins[0] == 0)
			st_corpse++;
		if (((int) before.edicts[k].v.flags & FL_ONGROUND) && !((int) after_c.edicts[k].v.flags & FL_ONGROUND))
			st_rider++;
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

	if (Phys_TargetFacts.v_blocked != offsetof (entvars_t, blocked) ||
	    Phys_TargetFacts.max_edicts != MAX_EDICTS ||
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

	for (iw = 0; iw < 3000; iw++)
	{
		gen_world ();
		corner = 0;
		gen_edicts ();
		host_frametime = chance (60) ? 1.0 / 72 : (chance (50) ? 0.05 : (double) frand (0.001f, 0.1f));
		sv.time = chance (50) ? 1.0 + iw * 0.1 : (double) frand (0, 100);
		sv_player = &edicts[1 + (int) (rnd () % (NE - 1))];
		for (k = 0; k < 40; k++)
			compare_case ();
	}

	printf ("outcomes: calls: SV_Physics_Pusher %ld, SV_PushMove %ld, SV_PushRotate %ld, SV_Push %ld\n"
		"          blocked functions %ld, thinks %ld, touches inside a push %ld; entities moved %ld,\n"
		"          riders lifted off %ld, corpses flattened %ld, elevator nudges %ld; rotators %ld;\n"
		"          liquid pushers %ld; SV_Push false %ld\n",
		st_kind[K_PHYSICS], st_kind[K_PUSHMOVE], st_kind[K_PUSHROTATE], st_kind[K_PUSH],
		st_blocked, st_think, st_qc_in_push, st_pushed, st_rider, st_corpse, st_nudged, st_rotated,
		st_liquid, st_push_false);

	floor_check ("an SV_Physics_Pusher call", st_kind[K_PHYSICS], 30000);
	floor_check ("an SV_PushMove call", st_kind[K_PUSHMOVE], 10000);
	floor_check ("an SV_PushRotate call", st_kind[K_PUSHROTATE], 10000);
	floor_check ("a blocked function", st_blocked, 1000);
	floor_check ("a pusher think", st_think, 1000);
	floor_check ("a touch inside a push", st_qc_in_push, 1000);
	floor_check ("an entity moved", st_pushed, 5000);
	floor_check ("a rider lifted off", st_rider, 1000);
	floor_check ("a corpse flattened", st_corpse, 100);
	floor_check ("a rotating pusher", st_rotated, 5000);
	floor_check ("a crush case", st_crush, 1000);
	floor_check ("a spawn inside a push", st_spawned, 300);
	floor_check ("a pusher that is not SOLID_BSP", st_pusher_not_bsp, 5000);
#ifdef SV_HW
	floor_check ("an SV_Push call", st_kind[K_PUSH], 10000);
	floor_check ("an SV_Push that fails", st_push_false, 500);
#else
	floor_check ("an elevator nudge", st_nudged, 50);
	floor_check ("a plat case", st_plat, 1000);
	floor_check ("a liquid pusher", st_liquid, 500);
#endif

	if (n_fail)
	{
		printf ("sv_phys_push differential harness (%s): FAIL, %ld problem(s) in %ld cases\n",
			VARIANT_NAME, n_fail, n_cases);
		return 1;
	}
	printf ("sv_phys_push differential harness (%s): PASS (%ld cases)\n", VARIANT_NAME, n_cases);
	return 0;
}
