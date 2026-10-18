/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/sv_move.rs against the C original of
 * sv_move.c's monster movement, in one binary.
 *
 * run_diff_harness.sh builds this against sv_move_h2.c (-DSERVERONLY, as
 * h2ded compiles it) and sv_move_hw.c (-DH2W -DSERVERONLY -DSV_HW, as hwsv).
 * The real engine/rust/world_target.c is compiled in.
 *
 * Each world is built as the engine has it when a monster moves: a world
 * model and three brush models with all eight clip hulls -- half the worlds
 * random clipnode trees, half floors, steps and walls on axial planes, so
 * walkers land, step and find ledges -- and entities linked by SV_LinkEdict:
 * monsters with every mix of FL_FLY, FL_SWIM, FL_ONGROUND, FL_PARTIALGROUND
 * and, in Hexen II, FL_HUNTFACE, FL_NOZ and FL_SET_TRACE; enemies and goals
 * (the world among them); brush entities and triggers.  Then each case calls
 * one of the three ways QuakeC reaches this code:
 *   - movetogoal, SV_MoveToGoal, with self and the distance in the globals;
 *   - walkmove's SV_movestep, with a move, relink, noenemy and set_trace;
 *   - the checkbottom builtin's SV_CheckBottom;
 * once on the C, once on the Rust, from the same snapshot with the same
 * rand() seed, and compares byte for byte: the return value, every edict,
 * the area nodes (SV_LinkEdict relinks), the progs globals (trace_*, the
 * return slot), the sequence of sv_debugmovestep calls with their formatted
 * text -- so a diagnostic run reads the same on both -- and the next rand(),
 * so a draw too many or too few fails even where this call does not use it.
 *
 * Some entities get NaN or infinite origins and yaws, as QuakeC's setorigin
 * on 0/0 makes them; the callees (SV_Move, SV_LinkEdict) are ported and
 * proven to return on those in their own harnesses.  NaN payloads and signs
 * are canonicalised before the compare -- in the entity fields, the globals
 * and the formatted log -- because C leaves them unspecified: either compiler
 * may swap the operands of a commutative add, and x86 keeps the first NaN
 * operand's sign.  Caught live: `-nan` from the C, `nan` from the Rust, for
 * the same NaN origin.
 *
 * WHAT THE REFERENCE CALLS.  SV_Move, SV_PointContents, SV_LinkEdict and
 * anglemod are the already-ported Rust, each held to its C by its own
 * harness; PF_changeyaw is pr_cmds.c's, copied below; rand, cos and sin are
 * the C library's, which the Rust calls too.  So this compares sv_move.c's
 * own logic over shared callees.
 */

#include "quakedef.h"
#include "world_target.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

qboolean	c_SV_CheckBottom (edict_t *ent);
qboolean	c_SV_movestep (edict_t *ent, vec3_t move, qboolean relink, qboolean noenemy, qboolean set_trace);
void		c_SV_MoveToGoal (void);

#ifdef SV_HW
#define VARIANT_NAME	"hexenworld"
#else
#define VARIANT_NAME	"hexen2"
#endif

/* the engine's storage */
server_t	sv;
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
static float	progs_globals[64];
float		*pr_globals = progs_globals;

/* what sv_globals points at */
static struct
{
	int	self, other, world;
	float	time;
	float	trace_allsolid, trace_startsolid, trace_fraction;
	vec3_t	trace_endpos, trace_plane_normal;
	float	trace_plane_dist;
	int	trace_ent;
	float	trace_inopen, trace_inwater;
} g;

/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0x9e3779b97f4a7c15ULL;

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

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	(void) flags; (void) fmt;	/* nothing reached here prints */
}

static long	n_touch;
static void touch_hook (void);

/* A relink touching a trigger: SV_TouchLinks runs its touch function with
 * self = the trigger and other = the mover.  QuakeC reads and writes the
 * mover there, so the stub does too: it logs a digest of what the mover
 * looks like at that moment, and on some calls changes its flags.  A port
 * that wrote a mover field after the relink instead of before it, or kept a
 * copy of the flags across it, then differs. */
void PR_ExecuteProgram (func_t fnum, const char *funcname)
{
	(void) fnum; (void) funcname;
	n_touch++;
	touch_hook ();
}

#define NE	24

static edict_t	edicts[NE];

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
	(void) ed; (void) field;	/* no SOLID_GHOST here */
	return NULL;
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	fprintf (stderr, "harness bug: Sys_Error: ");
	va_start (ap, error);
	vfprintf (stderr, error, ap);
	va_end (ap);
	fprintf (stderr, "\n");
	abort ();
}

#ifdef SV_HW
FUNC_NORETURN void SV_Error (const char *error, ...)
{
	va_list	ap;
	fprintf (stderr, "harness bug: SV_Error: ");
	va_start (ap, error);
	vfprintf (stderr, error, ap);
	va_end (ap);
	fprintf (stderr, "\n");
	abort ();
}
#endif

/* pr_cmds.c's PF_changeyaw, verbatim: it turns self, not its argument, and
 * writes the return slot, which HexenWorld's SV_MoveToGoal then leaves */
void PF_changeyaw (void)
{
	edict_t		*ent;
	float		ideal, current, move, speed;

	ent = PROG_TO_EDICT(*sv_globals.self);
	current = anglemod( ent->v.angles[1] );
	ideal = ent->v.ideal_yaw;
	speed = ent->v.yaw_speed;

	if (current == ideal)
	{
		G_FLOAT(OFS_RETURN) = 0;
		return;
	}
	move = ideal - current;

	if (ideal > current)
	{
		if (move >= 180)
			move = move - 360;
	}
	else
	{
		if (move <= -180)
			move = move + 360;
	}

	G_FLOAT(OFS_RETURN) = move;

	if (move > 0)
	{
		if (move > speed)
			move = speed;
	}
	else
	{
		if (move < -speed)
			move = -speed;
	}

	ent->v.angles[1] = anglemod (current + move);
}

/* ------------------------------------------------------------------------ */
/* the sv_debugmovestep helpers: each call is recorded, with its text       */
/* ------------------------------------------------------------------------ */

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

static char	dbg[8192];
static size_t	dbg_len;

static void dbg_add (const char *fmt, ...)
{
	va_list	ap;
	int	n;
	if (dbg_len >= sizeof (dbg) - 1)
		return;
	va_start (ap, fmt);
	n = vsnprintf (dbg + dbg_len, sizeof (dbg) - dbg_len, fmt, ap);
	va_end (ap);
	if (dbg_len + (size_t) (n > 0 ? n : 0) > sizeof (dbg) - 1)
		n = (int) (sizeof (dbg) - 1 - dbg_len);
	if (n > 0)
	{
		canon_text (dbg + dbg_len);
		dbg_len = strlen (dbg);
	}
}

static long	idx (const edict_t *e)	{ return e ? (long) (e - edicts) : -1L; }

static void touch_hook (void)
{
	edict_t	*other = PROG_TO_EDICT(*sv_globals.other);
	char	o[3][48];
	int	k;
	for (k = 0; k < 3; k++)
		snprintf (o[k], sizeof (o[k]), "%a", other->v.origin[k]);
	for (k = 0; k < 3; k++)
		canon_text (o[k]);
	dbg_add ("T e%ld self %d other e%ld flags %a ground %d org %s %s %s\n",
		 (long) (n_touch & 0xffff), *sv_globals.self, idx (other),
		 other->v.flags, other->v.groundentity, o[0], o[1], o[2]);
	/* deterministic in the touch count, which both sides start equal.  Bits
	 * a later flags store keeps (FL_FLY, FL_SWIM), so a store computed from
	 * flags read before the relink shows, as well as ones it rewrites */
	switch (n_touch % 7)
	{
	case 1: other->v.flags = (float) ((int) other->v.flags ^ FL_FLY); break;
	case 3: other->v.flags = (float) ((int) other->v.flags ^ FL_SWIM); break;
	case 5: other->v.flags = (float) ((int) other->v.flags ^ FL_ONGROUND); break;
	default: break;
	}
}

void SV_DebugMoveStep_Log (const edict_t *ent, const trace_t *trace, const char *fmt, ...)
{
	char	buf[256];
	va_list	ap;
	va_start (ap, fmt);
	vsnprintf (buf, sizeof (buf), fmt, ap);
	va_end (ap);
	canon_text (buf);
	if (trace)
		dbg_add ("L e%ld [%a %ld %d %d] %s\n", idx (ent), trace->fraction,
			 idx (trace->ent), trace->allsolid, trace->startsolid, buf);
	else
		dbg_add ("L e%ld [-] %s\n", idx (ent), buf);
}

void SV_DebugMoveStep_DumpMovers (const vec3_t foot)
{
	dbg_add ("D %a %a %a\n", foot[0], foot[1], foot[2]);
}

qboolean SV_DebugMoveStep_Call (const edict_t *ent, const vec3_t mins,
				const vec3_t maxs, qboolean ok, const char *why)
{
	dbg_add ("C e%ld %d %s [%a %a %a]..[%a %a %a]\n", idx (ent), ok, why,
		 mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]);
	return ok;
}

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

/* QuakeC's state on a monster before it moves */
static void gen_monster (edict_t *e)
{
	int	fl = 0;
	static const float	yaws[] = { 0, 45, 90, 135, 180, 225, 270, 315, 360, -45, 22.5f, 359.9f, 7 };

	if (chance (55)) fl |= FL_ONGROUND;
	if (chance (18)) fl |= FL_FLY;
	if (chance (14)) fl |= FL_SWIM;
	if (chance (20)) fl |= FL_PARTIALGROUND;
	if (chance (60)) fl |= FL_MONSTER;
#ifndef SV_HW
	if (chance (12)) fl |= FL_HUNTFACE;
	if (chance (12)) fl |= FL_NOZ;
	if (chance (30)) fl |= FL_SET_TRACE;
#endif
	if (chance (3)) fl |= (int) (rnd () & 0xffffff);
	e->v.flags = (float) fl;
	/* QuakeC can store any float in .flags; `(int)` of one out of range is
	 * the target's conversion (x86: INT_MIN, no FL_ bit set), which the port
	 * must reproduce rather than saturate (0x7fffffff, every bit set) */
	if (chance (3))
	{
		static const float	big[] = { 3e9f, -3e9f, 2147483648.0f, -2147483904.0f, 1e30f, -1e30f };
		e->v.flags = chance (25) ? odd_value () : big[rnd () % 6];
	}
	e->v.enemy = chance (35) ? 0 : (int) (rnd () % NE) * (int) sizeof (edict_t);
	e->v.goalentity = chance (10) ? 0 : (int) (rnd () % NE) * (int) sizeof (edict_t);
	e->v.ideal_yaw = chance (80) ? yaws[rnd () % 13] : frand (-720, 720);
	if (chance (1))
		e->v.ideal_yaw = chance (50) ? 3e9f : odd_value ();
	e->v.angles[1] = chance (40) ? e->v.ideal_yaw : (chance (70) ? yaws[rnd () % 13] : frand (-400, 400));
	e->v.yaw_speed = chance (20) ? 360 : frand (0, 40);
	/* SV_StepDirection's "not turned far enough" test at its edges: with
	 * yaw_speed 0, PF_changeyaw leaves the angle where it is, and these
	 * offsets survive anglemod exactly, so delta lands on 45 or 315 */
	if (chance (12))
	{
		static const float	off[] = { 45, 315, -45, 44, 46, 314, 316, 0 };
		static const float	base[] = { 0, 45, 90, 135, 180, 225, 270 };
		e->v.ideal_yaw = base[rnd () % 7];
		e->v.angles[1] = e->v.ideal_yaw + off[rnd () % 8];
		e->v.yaw_speed = 0;
	}
	e->v.hull = (float) (rnd () % 7);
	e->v.view_ofs[2] = frand (0, 50);
	if (chance (15))
		place (e), SV_LinkEdict (e, false);
}

/* ------------------------------------------------------------------------ */
/* comparison                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
	edict_t		edicts[NE];
	areanode_t	nodes[AREA_NODES];
	__typeof__ (g)	g;
	float		progs[64];
	char		dbg[8192];
	size_t		dbg_len;
	long		touch;
	int		ret;
	int		next_rand;	/* where each side left the rand() stream */
} state_t;

static state_t	before, after_c, after_rust;

static void save (state_t *s, int ret)
{
	memcpy (s->edicts, edicts, sizeof (edicts));
	memcpy (s->nodes, sv_areanodes, sizeof (sv_areanodes));
	s->g = g;
	memcpy (s->progs, progs_globals, sizeof (progs_globals));
	memcpy (s->dbg, dbg, sizeof (dbg));
	s->dbg_len = dbg_len;
	s->touch = n_touch;
	s->ret = ret;
}

/* For the compare only, never for a state that is restored: the float
 * fields of what this code writes, NaNs made one value (see the header).
 * The edicts' int fields (enemy, goalentity, groundentity, owner) are left
 * as they are. */
static void canon_state (state_t *s)
{
	int	k;
	for (k = 0; k < NE; k++)
	{
		entvars_t	*v = &s->edicts[k].v;
		canon_floats (v->origin, 3);
		canon_floats (v->angles, 3);
		canon_floats (&v->flags, 1);
		canon_floats (&v->ideal_yaw, 1);
		canon_floats (&v->hull, 1);
		canon_floats (v->absmin, 3);
		canon_floats (v->absmax, 3);
	}
	canon_floats (&s->g.time, 1);
	canon_floats (&s->g.trace_allsolid, 1);
	canon_floats (&s->g.trace_startsolid, 1);
	canon_floats (&s->g.trace_fraction, 1);
	canon_floats (s->g.trace_endpos, 3);
	canon_floats (s->g.trace_plane_normal, 3);
	canon_floats (&s->g.trace_plane_dist, 1);
	canon_floats (&s->g.trace_inopen, 1);
	canon_floats (&s->g.trace_inwater, 1);
	canon_floats (s->progs, sizeof (progs_globals) / sizeof (float));
}

static void restore (const state_t *s)
{
	memcpy (edicts, s->edicts, sizeof (edicts));
	memcpy (sv_areanodes, s->nodes, sizeof (sv_areanodes));
	g = s->g;
	memcpy (progs_globals, s->progs, sizeof (progs_globals));
	memcpy (dbg, s->dbg, sizeof (dbg));
	dbg_len = s->dbg_len;
	n_touch = s->touch;
}

static long	n_cases, n_fail;
static long	st_kind[3], st_ret[2], st_moved, st_relinked, st_touch, st_trace_set, st_log_lines;
static long	st_msg[16];

static const char *const msgs[] =
{
	"all corners over solid",
	"midpoint has no ground",
	"a corner is off an edge",
	"a corner drops > STEPSIZE",
	"traced, corners within STEPSIZE",
	"swim monster left water",
	"flier blocked",
	"step-down trace allsolid",
	"step-down still allsolid",
	"step-down still startsolid",
	"walked off edge",
	"CheckBottom failed at landing pos",
};
#define NMSGS	(int) (sizeof (msgs) / sizeof (msgs[0]))

static void report (long id, const char *what, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH case #%ld (%s): %s\n", id, what, detail);
}

static int run_one (int kind, edict_t *ent, const vec3_t move0, int relink, int noenemy, int set_trace, int rust)
{
	vec3_t	move;
	VectorCopy (move0, move);
	switch (kind)
	{
	case 0:
		if (rust) SV_MoveToGoal (); else c_SV_MoveToGoal ();
		return 0;
	case 1:
		return rust ? SV_movestep (ent, move, relink, noenemy, set_trace)
			    : c_SV_movestep (ent, move, relink, noenemy, set_trace);
	default:
		return rust ? SV_CheckBottom (ent) : c_SV_CheckBottom (ent);
	}
}

static void compare_case (void)
{
	int		kind, ec = 1 + (int) (rnd () % (NE - 1));
	edict_t		*ent = &edicts[ec];
	vec3_t		move;
	int		relink = chance (50), noenemy = chance (30), set_trace = chance (50);
	unsigned int	seed = rnd ();
	int		k, rc, rr;
	char		msg[600];

	n_cases++;
	kind = chance (50) ? 0 : (chance (70) ? 1 : 2);
	ent->free = false;
	gen_monster (ent);
	for (k = 0; k < 2; k++)
		move[k] = chance (60) ? (float) ((int) (rnd () % 41) - 20) : frand (-40, 40);
	move[2] = chance (70) ? 0 : frand (-20, 20);

	/* the QuakeC calling convention: self, the distance in parm 0, and
	 * trace globals left by whatever ran before */
	g.self = ec * (int) sizeof (edict_t);
	g.other = 0;
	g.trace_fraction = frand (0, 1);
	g.trace_plane_normal[0] = 1;
	G_FLOAT(OFS_PARM0) = chance (85) ? (float) (rnd () % 32) : frand (-8, 64);
	G_FLOAT(OFS_RETURN) = -7;
	dbg_len = 0;
	dbg[0] = 0;

	save (&before, 0);
	srand (seed);
	rc = run_one (kind, ent, move, relink, noenemy, set_trace, 0);
	save (&after_c, rc);
	after_c.next_rand = rand ();
	restore (&before);
	srand (seed);
	rr = run_one (kind, ent, move, relink, noenemy, set_trace, 1);
	save (&after_rust, rr);
	after_rust.next_rand = rand ();
	canon_state (&after_c);
	canon_state (&after_rust);

	if (rc != rr)
	{
		snprintf (msg, sizeof (msg), "returns C %d Rust %d", rc, rr);
		report (n_cases, "return", msg);
	}
	if (memcmp (after_c.edicts, after_rust.edicts, sizeof (edicts)) != 0)
	{
		for (k = 0; k < NE; k++)
			if (memcmp (&after_c.edicts[k], &after_rust.edicts[k], sizeof (edict_t)) != 0)
				break;
		snprintf (msg, sizeof (msg), "kind %d: edict %d differs (origin C %a %a %a, Rust %a %a %a; flags C %a Rust %a)",
			  kind, k, after_c.edicts[k].v.origin[0], after_c.edicts[k].v.origin[1], after_c.edicts[k].v.origin[2],
			  after_rust.edicts[k].v.origin[0], after_rust.edicts[k].v.origin[1], after_rust.edicts[k].v.origin[2],
			  after_c.edicts[k].v.flags, after_rust.edicts[k].v.flags);
		report (n_cases, "edicts", msg);
	}
	if (memcmp (after_c.nodes, after_rust.nodes, sizeof (sv_areanodes)) != 0)
		report (n_cases, "area nodes", "the relinks differ");
	if (memcmp (&after_c.g, &after_rust.g, sizeof (g)) != 0)
		report (n_cases, "globals", "trace_* or self/other differ");
	if (memcmp (after_c.progs, after_rust.progs, sizeof (progs_globals)) != 0)
	{
		snprintf (msg, sizeof (msg), "kind %d: return slot C %a Rust %a", kind, after_c.progs[OFS_RETURN], after_rust.progs[OFS_RETURN]);
		report (n_cases, "progs globals", msg);
	}
	if (after_c.dbg_len != after_rust.dbg_len || memcmp (after_c.dbg, after_rust.dbg, after_c.dbg_len) != 0)
	{
		snprintf (msg, sizeof (msg), "kind %d: sv_debugmovestep calls differ\n--- C\n%.250s\n--- Rust\n%.250s",
			  kind, after_c.dbg, after_rust.dbg);
		report (n_cases, "diagnostics", msg);
	}
	if (after_c.next_rand != after_rust.next_rand)
		report (n_cases, "rand", "the two drew rand() a different number of times");
	if (after_c.touch != after_rust.touch)
		report (n_cases, "touches", "a relink ran a different number of touch functions");

	/* outcomes, from the C side */
	st_kind[kind]++;
	if (kind != 0)
		st_ret[rc != 0]++;
	if (memcmp (before.edicts[ec].v.origin, after_c.edicts[ec].v.origin, sizeof (vec3_t)) != 0)
		st_moved++;
	if (memcmp (before.nodes, after_c.nodes, sizeof (sv_areanodes)) != 0)
		st_relinked++;
	if (after_c.touch != before.touch)
		st_touch++;
	if (after_c.g.trace_fraction != before.g.trace_fraction || after_c.g.trace_ent != before.g.trace_ent)
		st_trace_set++;
	{
		const char	*p;
		for (p = after_c.dbg; *p; p++)
			if (*p == '\n')
				st_log_lines++;
		for (k = 0; k < NMSGS; k++)
			if (strstr (after_c.dbg, msgs[k]))
				st_msg[k]++;
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

	if (World_TargetFacts.v_goalentity != offsetof (entvars_t, goalentity) ||
	    World_TargetFacts.fl_partialground != FL_PARTIALGROUND)
	{
		fprintf (stderr, "harness bug: world_target.c is not this build's\n");
		return 2;
	}
	sv_globals.self = &g.self;
	sv_globals.other = &g.other;
	sv_globals.world = &g.world;
	sv_globals.time = &g.time;
	sv_globals.trace_allsolid = &g.trace_allsolid;
	sv_globals.trace_startsolid = &g.trace_startsolid;
	sv_globals.trace_fraction = &g.trace_fraction;
	sv_globals.trace_endpos = &g.trace_endpos;
	sv_globals.trace_plane_normal = &g.trace_plane_normal;
	sv_globals.trace_plane_dist = &g.trace_plane_dist;
	sv_globals.trace_ent = &g.trace_ent;
	sv_globals.trace_inopen = &g.trace_inopen;
	sv_globals.trace_inwater = &g.trace_inwater;

	for (iw = 0; iw < 3000; iw++)
	{
		gen_world ();
		gen_edicts ();
		for (k = 0; k < 60; k++)
			compare_case ();
	}

	printf ("outcomes: movetogoal %ld, walkmove %ld (true %ld, false %ld), checkbottom %ld;\n"
		"          moved %ld, relinked %ld, ran a touch %ld, set the trace globals %ld; %ld log lines\n",
		st_kind[0], st_kind[1], st_ret[1], st_ret[0], st_kind[2],
		st_moved, st_relinked, st_touch, st_trace_set, st_log_lines);
	for (k = 0; k < NMSGS; k++)
		printf ("          %-36s %ld\n", msgs[k], st_msg[k]);

	floor_check ("a movetogoal call", st_kind[0], 50000);
	floor_check ("a walkmove call", st_kind[1], 30000);
	floor_check ("a checkbottom call", st_kind[2], 10000);
	floor_check ("a step that returns true", st_ret[1], 5000);
	floor_check ("a step that returns false", st_ret[0], 5000);
	floor_check ("a monster that moved", st_moved, 10000);
	floor_check ("a relink", st_relinked, 10000);
	floor_check ("a relink that ran a touch", st_touch, 100);
	floor_check ("a call that set the trace globals", st_trace_set, 5000);
	for (k = 0; k < NMSGS; k++)
		floor_check (msgs[k], st_msg[k], 100);

	if (n_fail)
	{
		printf ("sv_move differential harness (%s): FAIL, %ld problem(s) in %ld cases\n",
			VARIANT_NAME, n_fail, n_cases);
		return 1;
	}
	printf ("sv_move differential harness (%s): PASS (%ld cases: %ld movetogoal, %ld walkmove, %ld checkbottom)\n",
		VARIANT_NAME, n_cases, st_kind[0], st_kind[1], st_kind[2]);
	return 0;
}
