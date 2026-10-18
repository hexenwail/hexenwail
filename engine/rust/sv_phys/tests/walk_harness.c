/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * walk_harness.c -- engine/rust/src/sv_phys_walk.rs against the C original
 * of sv_phys.c's client movement, in one binary.
 *
 * run_walk_harness.sh builds this against sv_phys_walk_h2.c (-DSERVERONLY,
 * as h2ded compiles it) and sv_phys_walk_hw.c (-DH2W -DSERVERONLY -DSV_HW,
 * as hwsv).  The real engine/rust/world_target.c and phys_target.c are
 * compiled in.
 *
 * The worlds are the sv_phys harness's: random clipnode trees, floor, step,
 * ledge and corner worlds, some of them water.  Each case puts a player in
 * them -- a slidebox walking at the world's steps and walls (velocity,
 * FL_ONGROUND, FL_WATERJUMP on sv_player, waterlevel, v_angle, view_ofs),
 * sometimes stuck in solid with an oldorigin free or not, sometimes
 * SOLID_BSP, with sv_nostep on and off -- and calls one of Hexen II's
 * SV_CheckStuck, SV_CheckWater, SV_WallFriction, SV_TryUnstick,
 * SV_WalkMove and SV_Physics_Client (every movetype, a connected client
 * slot or not, a bad movetype, which is fatal), or HexenWorld's
 * SV_Physics_Client.  Once on the C and once on the Rust from the same
 * snapshot; byte for byte: every edict, the area nodes, self/other/time,
 * the return value, the trace passed in, and every console line and QuakeC
 * call (PlayerPreThink/PostThink, thinks, touches) in order.
 *
 * A case that can reach a fatal -- a bad movetype, or a NaN trace fraction
 * (SV_FlyMove's `!trace.ent') -- runs in forked children and must raise the
 * same fatal or leave the same state digest.
 *
 * WHAT THE REFERENCE CALLS.  SV_FlyMove, SV_PushEntity, SV_CheckVelocity,
 * SV_RunThink, SV_AddGravity, SV_FlyExtras and SV_Impact are the earlier
 * sv_phys slices' Rust; SV_Move, SV_PointContents, SV_TestEntityPosition,
 * SV_LinkEdict and AngleVectors the world ports'.  SV_Physics_Toss, still
 * C, is a logging stub here.  So this compares the client movement's own
 * logic over shared callees.
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

#ifdef SV_HW
void		c_SV_Physics_Client (edict_t *ent);
#define VARIANT_NAME	"hexenworld"
#define WANT_ERROR_FN	"SV_Error"
#else
void		c_SV_CheckStuck (edict_t *ent);
qboolean	c_SV_CheckWater (edict_t *ent);
void		c_SV_WallFriction (edict_t *ent, trace_t *trace);
int		c_SV_TryUnstick (edict_t *ent, vec3_t oldvel);
void		c_SV_WalkMove (edict_t *ent);
void		c_SV_Physics_Client (edict_t *ent, int num);
#define VARIANT_NAME	"hexen2"
#define WANT_ERROR_FN	"Sys_Error"
#endif

#define NCLIENTS_HARNESS	8

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
cvar_t		sv_nostep = { "sv_nostep", "0", CVAR_NONE };
#endif
server_static_t	svs;
int		pr_edict_size = sizeof (edict_t);
#ifndef SV_HW
static client_t	clients[NCLIENTS_HARNESS];
#endif

static struct
{
	int	self, other, world;
	float	time;
	func_t	prethink, postthink;
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
	/* a touch on the mover (other) changes what the move reads after it:
	 * its movetype ("gibbed by a trigger"; a SOLID_BSP brush keeps
	 * MOVETYPE_PUSH, SV_HullForEntity's rule) and its waterlevel */
	case 10:
		if (other->v.solid != SOLID_BSP)
			other->v.movetype = (n_qc / 16) % 2 ? MOVETYPE_TOSS : MOVETYPE_NONE;
		break;
	case 15:
		other->v.waterlevel = other->v.waterlevel == 0 ? (float) (1 + (n_qc / 16) % 3) : 0;
		break;
	case 14:	/* PlayerPreThink may change the movetype SV_Physics_Client
			 * then switches on */
	{
		static const int	mt[] = { MOVETYPE_WALK, MOVETYPE_FLY, MOVETYPE_NOCLIP, MOVETYPE_NONE, MOVETYPE_TOSS };
		if (self->v.solid != SOLID_BSP)
			self->v.movetype = (float) mt[(n_qc / 16) % 5];
		break;
	}
	case 13:	/* setorigin */
		self->v.origin[0] += 24;
		self->v.origin[2] += 8;
		if (!self->free && self != sv.edicts)
			SV_LinkEdict (self, false);
		break;
	default: break;
	}
}

/* sv_phys.c's SV_Physics_Toss, which stays C until the movetype slice:
 * both sides call this, so it logs the call and moves the entity one fixed,
 * visible step */
void SV_Physics_Toss (edict_t *ent)
{
	dbg_add ("TOSS e%ld\n", idx (ent));
	ent->v.origin[2] -= 1;
	ent->v.velocity[2] -= 10;
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
/* directed worlds: a pool, and a stair                                     */
/* ------------------------------------------------------------------------ */

static const vec3_t	pmins = { -16, -16, -24 }, pmaxs = { 16, 16, 32 };

/* every hull of the world one small tree, with the player hull's clip box
 * the player's own, so the planes bound the origin exactly */
static void world_tree (int nnodes, int nplanes)
{
	int	h;
	for (h = 0; h < MAX_MAP_HULLS; h++)
	{
		models[1].hulls[h].clipnodes = clipnodes[1][0];
		models[1].hulls[h].planes = planes[1];
		models[1].hulls[h].firstclipnode = 0;
		models[1].hulls[h].lastclipnode = nnodes - 1;
		if (h)
		{
			VectorCopy (pmins, models[1].hulls[h].clip_mins);
			VectorCopy (pmaxs, models[1].hulls[h].clip_maxs);
		}
	}
	(void) nplanes;
}

static void axial (int k, int type, float dist)
{
	memset (&planes[1][k], 0, sizeof (mplane_t));
	planes[1][k].type = (byte) type;
	planes[1][k].normal[type] = 1;
	planes[1][k].dist = dist;
}

/* water from z = 0 down to a floor, open air above: SV_CheckWater's three
 * levels are its feet, its middle and its eyes against the surface */
static float	pool_floor;
static void gen_pool (void)
{
	pool_floor = -frand (60, 200);
	axial (0, 2, 0);
	axial (1, 2, pool_floor);
	clipnodes[1][0][0].planenum = 0;
	clipnodes[1][0][0].children[0] = CONTENTS_EMPTY;
	clipnodes[1][0][0].children[1] = 1;
	clipnodes[1][0][1].planenum = 1;
	clipnodes[1][0][1].children[0] = CONTENTS_WATER;
	clipnodes[1][0][1].children[1] = CONTENTS_SOLID;
	world_tree (2, 2);
}

/* a step: a low floor, and past x = 0 a floor `rise' higher */
static float	stair_rise;
static int	stair_axis, stair_slope;
static void gen_stair (void)
{
	static const float	rises[] = { 1, 4, 8, 12, 16, 17.5f, 18, 18.5f, 24, 40 };
	stair_rise = rises[rnd () % 10];
	stair_axis = (int) (rnd () % 2);	/* the step runs along x or along y */
	stair_slope = 0;
	if (host_frametime == 1.0 / 32)	/* the exact-threshold case wants a wall */
		stair_rise = chance (50) ? 24 : 40;
	else if (chance (35))
		stair_slope = chance (50) ? 1 : 2;
	if (stair_slope == 1)	/* a step too high to clear, so the step-down
				 * lands back on the slope */
		stair_rise = chance (50) ? 24 : 40;
	if (stair_slope == 2)	/* a low step, cleared onto a ramp */
		stair_rise = (float) (4 + rnd () % 13);
	axial (0, stair_axis, 0);
	axial (1, 2, stair_rise);
	axial (2, 2, 0);
	if (stair_slope == 1)
	{
		/* the low floor a slope whose normal's z is exactly 0.7f: the
		 * step-down's `> 0.7' (a double) refuses it, `>= 0.7f' would not */
		memset (&planes[1][2], 0, sizeof (mplane_t));
		planes[1][2].normal[stair_axis] = 0.71414284f;
		planes[1][2].normal[2] = 0.7f;
		planes[1][2].type = (byte) (3 + stair_axis);
		planes[1][2].dist = 0;
	}
	else if (stair_slope == 2)
	{
		/* the high floor a ramp rising away from the step: the step-up's
		 * forward move hits only it, a floor, so its clip is 1 -- no bit 2
		 * -- while the first move's wall trace is still stored */
		memset (&planes[1][1], 0, sizeof (mplane_t));
		planes[1][1].normal[stair_axis] = -0.6f;
		planes[1][1].normal[2] = 0.8f;
		planes[1][1].type = (byte) (3 + stair_axis);
		planes[1][1].dist = 0.8f * stair_rise;
	}
	clipnodes[1][0][0].planenum = 0;
	clipnodes[1][0][0].children[0] = 1;	/* x > 0: the high floor */
	clipnodes[1][0][0].children[1] = 2;
	clipnodes[1][0][1].planenum = 1;
	clipnodes[1][0][1].children[0] = CONTENTS_EMPTY;
	clipnodes[1][0][1].children[1] = CONTENTS_SOLID;
	clipnodes[1][0][2].planenum = 2;
	clipnodes[1][0][2].children[0] = CONTENTS_EMPTY;
	clipnodes[1][0][2].children[1] = CONTENTS_SOLID;
	world_tree (3, 3);
}

/* empty: every hull one empty leaf, for SV_TryUnstick's exact 4 */
static void gen_empty (void)
{
	axial (0, 2, -100000);
	clipnodes[1][0][0].planenum = 0;
	clipnodes[1][0][0].children[0] = CONTENTS_EMPTY;
	clipnodes[1][0][0].children[1] = CONTENTS_EMPTY;
	world_tree (1, 1);
}

static int	world_kind;	/* 0 random, 1 pool, 2 stair, 3 empty */

/* ------------------------------------------------------------------------ */
/* the player                                                               */
/* ------------------------------------------------------------------------ */

#define PL	1	/* the player's edict, client slot 1 */

static float odd_or (float x)
{
	return chance (2) ? odd_value () : x;
}

static void gen_player (edict_t *e)
{
	static const vec3_t	cmins = { -16, -16, 0 }, cmaxs = { 16, 16, 28 };
	int	k, crouch = chance (25) && world_kind == 0;

	e->free = false;
	e->v.solid = chance (5) ? SOLID_BSP : SOLID_SLIDEBOX;
	if (e->v.solid == SOLID_BSP)
		e->v.modelindex = 0;	/* a BSP mover, as only the mover */
	e->v.movetype = MOVETYPE_WALK;
	VectorCopy (crouch ? cmins : pmins, e->v.mins);
	VectorCopy (crouch ? cmaxs : pmaxs, e->v.maxs);
	VectorSubtract (e->v.maxs, e->v.mins, e->v.size);
	e->v.hull = 0;
	place (e);
	for (k = 0; k < 2; k++)
		e->v.velocity[k] = chance (15) ? 0 : (chance (50) ? (chance (50) ? 320 : -320) : frand (-450, 450));
	e->v.velocity[2] = chance (60) ? 0 : frand (-300, 270);
	e->v.velocity[rnd () % 3] = odd_or (e->v.velocity[0]);
	e->v.flags = (float) ((chance (65) ? FL_ONGROUND : 0) | (chance (8) ? FL_WATERJUMP : 0) | FL_CLIENT);
	e->v.waterlevel = (float) (rnd () % 4);
	/* whatever it was last frame: SV_CheckWater resets it */
	e->v.watertype = chance (50) ? CONTENTS_EMPTY : (chance (50) ? CONTENTS_WATER : CONTENTS_LAVA);
	for (k = 0; k < 3; k++)
		e->v.v_angle[k] = (k == 2) ? 0 : frand (-90, 360);
	e->v.view_ofs[2] = crouch ? 24 : 50;
	if (world_kind == 1)	/* in the pool, at every depth */
	{
		e->v.origin[2] = frand (pool_floor + 25, 40);
		if (chance (40))
			e->v.velocity[2] = frand (-100, 100);
	}
	else if (world_kind == 2)	/* on the low floor, walking at the step */
	{
		int	a = stair_axis, b = 1 - stair_axis;
		e->v.origin[a] = -frand (0.5f, 24);
		e->v.origin[b] = frand (-100, 100);
		e->v.origin[2] = chance (70) ? 0.03125f : frand (0, 3);
		if (stair_slope == 1)	/* on the slope near the step, falling */
		{
			e->v.origin[a] = -frand (0.2f, 2);
			e->v.origin[2] = 0.03125f - 0.71414284f / 0.7f * e->v.origin[a];
		}
		e->v.velocity[a] = chance (70) ? 320 : frand (50, 450);
		e->v.velocity[b] = chance (50) ? 0 : frand (-200, 200);
		e->v.velocity[2] = chance (70) ? 0 : frand (-50, 270);
		if (stair_slope == 1)
			e->v.velocity[2] = -frand (30, 300);
		/* facing the step: SV_WallFriction cuts the slide (d < 0) */
		if (chance (40))
		{
			VectorClear (e->v.v_angle);
			e->v.v_angle[1] = a ? 90 : 0;
		}
		e->v.flags = (float) ((int) e->v.flags | FL_ONGROUND);
		e->v.waterlevel = 0;
		/* SV_WalkMove's `< 0.03125' progress test, exactly at the edge: with
		 * host_frametime 1/32 and velocity 4 the move is 0.125; from x
		 * -0.0625 against the wall at 0, the trace backs off DIST_EPSILON to
		 * frac 0.25 exactly, so the step-up's forward move (the wall is
		 * higher than a step here) ends exactly 0.03125 from where it began */
		if (host_frametime == 1.0 / 32 && chance (70))
		{
			static const float	vx[] = { 4, 4, 4, 2, 8, 3.9999998f, 4.0000005f };
			e->v.origin[a] = -0.0625f;
			e->v.origin[2] = 0;
			e->v.velocity[a] = vx[rnd () % 7];
			e->v.velocity[b] = chance (70) ? 0 : (chance (50) ? 1 : 4);
			e->v.velocity[2] = 0;
		}
	}
	else if (world_kind == 3)	/* open space, at integer positions */
	{
		for (k = 0; k < 3; k++)
			e->v.origin[k] = (float) ((int) (rnd () % 401) - 200);
	}
	VectorCopy (e->v.origin, e->v.oldorigin);
	if (chance (40))	/* where it was last frame */
		for (k = 0; k < 3; k++)
			e->v.oldorigin[k] += chance (50) ? 0 : frand (-24, 24);
	e->v.nextthink = chance (70) ? 0 : (float) sv.time;
	e->v.think = 500;
	e->v.touch = chance (50) ? 600 : 0;	/* its own touch: self is the player */
	SV_LinkEdict (e, false);
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
	trace_t		trace;		/* SV_WallFriction's, as passed in */
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

static void canon_state (state_t *s)
{
	int	k;
	for (k = 0; k < NE; k++)
	{
		entvars_t	*v = &s->edicts[k].v;
		canon_floats (v->origin, 3);
		canon_floats (v->oldorigin, 3);
		canon_floats (v->velocity, 3);
		canon_floats (&v->flags, 1);
		canon_floats (v->absmin, 3);
		canon_floats (v->absmax, 3);
		canon_floats (&v->nextthink, 1);
		canon_floats (&v->hoverz, 1);
		canon_floats (&v->waterlevel, 1);
		canon_floats (&v->movetype, 1);
	}
	canon_floats (&s->g.time, 1);
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

enum { K_CHECKSTUCK, K_CHECKWATER, K_WALLFRICTION, K_TRYUNSTICK, K_WALKMOVE, K_CLIENT, NKINDS };
static const char *const kind_names[NKINDS] =
	{ "SV_CheckStuck", "SV_CheckWater", "SV_WallFriction", "SV_TryUnstick", "SV_WalkMove", "SV_Physics_Client" };

typedef struct
{
	int	kind;
	edict_t	*ent;
	vec3_t	oldvel;
	trace_t	trace;
	int	num;
} call_t;

static int run_one (const call_t *c, int rust, state_t *s)
{
	vec3_t	oldvel;
	VectorCopy (c->oldvel, oldvel);
	s->trace = c->trace;
	switch (c->kind)
	{
#ifndef SV_HW
	case K_CHECKSTUCK:
		if (rust) SV_CheckStuck (c->ent); else c_SV_CheckStuck (c->ent);
		return 0;
	case K_CHECKWATER:
		return rust ? SV_CheckWater (c->ent) : c_SV_CheckWater (c->ent);
	case K_WALLFRICTION:
		if (rust) SV_WallFriction (c->ent, &s->trace); else c_SV_WallFriction (c->ent, &s->trace);
		return 0;
	case K_TRYUNSTICK:
		return rust ? SV_TryUnstick (c->ent, oldvel) : c_SV_TryUnstick (c->ent, oldvel);
	case K_WALKMOVE:
		if (rust) SV_WalkMove (c->ent); else c_SV_WalkMove (c->ent);
		return 0;
	default:
		if (rust) SV_Physics_Client (c->ent, c->num); else c_SV_Physics_Client (c->ent, c->num);
		return 0;
#else
	default:
		if (rust) SV_Physics_Client (c->ent); else c_SV_Physics_Client (c->ent);
		return 0;
#endif
	}
}

static long	n_cases, n_fail, n_forked;
static long	st_kind[NKINDS], st_fatal, st_qc, st_moved, st_stepped, st_stuck_msg[3], st_unstick[2];
static long	st_water[4], st_friction, st_walk_exit[6], st_toss, st_inactive, st_edge4, st_edge32;

static void report (long id, const char *what, const char *detail)
{
	n_fail++;
	if (n_fail <= 12)
		fprintf (stderr, "MISMATCH case #%ld (%s): %s\n", id, what, detail);
}

static int odd (float x)
{
	return !isfinite (x) || fabsf (x) > 1e9f;
}

static int has_odd (void)
{
	int	k, e;
	for (e = 0; e < NE; e++)
		for (k = 0; k < 3; k++)
			if (odd (edicts[e].v.velocity[k]) || odd (edicts[e].v.origin[k]) ||
			    odd (edicts[e].v.oldorigin[k]) ||
			    odd (edicts[e].v.absmin[k]) || odd (edicts[e].v.absmax[k]))
				return 1;
	return 0;
}

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
		trace_t		t;
		close (fds[0]);
		err_fd = fds[1];
		setitimer (ITIMER_PROF, &it, NULL);
		ret = run_one (c, rust, &s);
		t = s.trace;
		save (&s, ret);
		s.trace = t;
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

static void gen_call (call_t *c)
{
	int	k;
	edict_t	*e = &edicts[PL];

	memset (c, 0, sizeof (*c));
#ifdef SV_HW
	c->kind = K_CLIENT;
#else
	c->kind = (int) (rnd () % NKINDS);
	if (c->kind == K_CHECKWATER && chance (30))
		c->kind = K_WALKMOVE;
	if (world_kind == 3 && chance (40))	/* the exact-4 edge (see below) */
		c->kind = K_TRYUNSTICK;
#endif
	c->ent = e;
	gen_player (e);
	/* what the player runs into has touch functions, which run with the
	 * player as other */
	for (k = 2; k < NE; k++)
		if (!edicts[k].free && edicts[k].v.solid != SOLID_TRIGGER)
			edicts[k].v.touch = chance (60) ? 650 + k : 0;
	for (k = 0; k < 3; k++)
		c->oldvel[k] = chance (50) ? e->v.velocity[k] : frand (-400, 400);
	/* SV_TryUnstick's `> 4', exactly at the edge: a 2-unit push and a free
	 * 0.1f * 20 = 2 move (rounded at these origins) make exactly 4 */
	if (world_kind == 3 && c->kind == K_TRYUNSTICK)
	{
		static const float	ov[] = { 20, -20, 0, 19.99f, 20.01f, 40 };
		c->oldvel[0] = ov[rnd () % 6];
		c->oldvel[1] = chance (50) ? 0 : ov[rnd () % 6];
		c->oldvel[2] = 0;
	}
	/* SV_WallFriction's trace: a wall (the case it runs for) or anything */
	c->trace.fraction = frand (0, 1);
	switch (rnd () % 4)
	{
	case 3:
		/* d exactly 0, SV_WallFriction's `>= 0' edge: v_angle 0 makes
		 * forward exactly (1 0 0), and this normal's x is -0.5 */
		VectorClear (e->v.v_angle);
		c->trace.plane.normal[0] = -0.5f;
		c->trace.plane.normal[1] = chance (50) ? 0.8660254f : -0.8660254f;
		break;
	case 0: c->trace.plane.normal[rnd () % 2] = chance (50) ? 1 : -1; break;
	case 1: c->trace.plane.normal[0] = 0.6f; c->trace.plane.normal[1] = -0.8f; break;
	default:
		for (k = 0; k < 3; k++)
			c->trace.plane.normal[k] = frand (-1, 1);
		break;
	}
	c->num = chance (90) ? PL : 2;	/* slot 2 is never active */
	if (c->kind == K_CLIENT)
	{
		static const int	mt[] = { MOVETYPE_WALK, MOVETYPE_WALK, MOVETYPE_WALK, MOVETYPE_NONE, MOVETYPE_TOSS,
					MOVETYPE_BOUNCE, MOVETYPE_FLY, MOVETYPE_SWIM, MOVETYPE_NOCLIP, MOVETYPE_STEP };
		e->v.movetype = (float) mt[rnd () % 10];	/* STEP is a bad movetype here */
		if (chance (1))
			e->v.movetype = odd_value ();
	}
	/* stuck: the origin in solid, oldorigin free or not */
	if (c->kind == K_CHECKSTUCK && chance (50))
	{
		e->v.origin[2] -= frand (0, 30);
		SV_LinkEdict (e, false);
	}
#ifdef SV_HW
	/* HexenWorld's: oldorigin to origin, the client's move, swept */
	for (k = 0; k < 3; k++)
		e->v.oldorigin[k] = e->v.origin[k] - e->v.velocity[k] * (float) host_frametime;
	/* a long sweep stopped just short of its end by the step's wall: a
	 * fraction above 0.999, at the `< 1.0' test's edge */
	if (world_kind == 2 && chance (50))
	{
		e->v.oldorigin[stair_axis] = -100;
		e->v.origin[stair_axis] = chance (50) ? 0.01f : 0.05f;
		e->v.oldorigin[1 - stair_axis] = e->v.origin[1 - stair_axis];
		e->v.oldorigin[2] = e->v.origin[2] = 0.5f;
	}
	e->v.movetype = chance (80) ? MOVETYPE_WALK : MOVETYPE_BOUNCE;
	e->v.touch = chance (50) ? 600 : 0;
#endif
	sv_player = chance (85) ? e : &edicts[2 + (int) (rnd () % (NE - 2))];
#ifndef SV_HW
	sv_nostep.integer = chance (10);
#endif
}

static void compare_case (void)
{
	call_t	c;
	int	rc, rr, fork_it, mt;
	char	msg[900];
	trace_t	t;

	n_cases++;
	gen_call (&c);
	g.self = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.other = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.time = frand (0, 10);
	dbg_len = 0;
	dbg[0] = 0;
	save (&before, 0);

	mt = isfinite (c.ent->v.movetype) ? (int) c.ent->v.movetype : -1;
	fork_it = has_odd () || (c.kind == K_CLIENT && (mt == MOVETYPE_STEP || mt < 0));
	if (fork_it)
	{
		char	oc[600], orr[600];
		n_forked++;
		run_child (&c, 0, oc, sizeof (oc));
		restore (&before);
		run_child (&c, 1, orr, sizeof (orr));
		restore (&before);
		if (strcmp (oc, orr) != 0)
		{
			snprintf (msg, sizeof (msg), "%s, forked: C \"%s\" Rust \"%s\"", kind_names[c.kind], oc, orr);
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
	t = after_c.trace;
	save (&after_c, rc);
	after_c.trace = t;
	restore (&before);
	rr = run_one (&c, 1, &after_rust);
	t = after_rust.trace;
	save (&after_rust, rr);
	after_rust.trace = t;
	canon_state (&after_c);
	canon_state (&after_rust);

	if (rc != rr)
	{
		snprintf (msg, sizeof (msg), "%s returns C %d Rust %d", kind_names[c.kind], rc, rr);
		report (n_cases, "return", msg);
	}
	if (memcmp (&after_c.trace, &after_rust.trace, sizeof (trace_t)) != 0)
		report (n_cases, "trace", "the trace passed in was written differently");
	if (memcmp (after_c.edicts, after_rust.edicts, sizeof (edicts)) != 0)
	{
		int	k;
		for (k = 0; k < NE; k++)
			if (memcmp (&after_c.edicts[k], &after_rust.edicts[k], sizeof (edict_t)) != 0)
				break;
		snprintf (msg, sizeof (msg), "%s: edict %d differs (origin C %a %a %a, Rust %a %a %a; velocity C %a %a %a, Rust %a %a %a)",
			  kind_names[c.kind], k,
			  after_c.edicts[k].v.origin[0], after_c.edicts[k].v.origin[1], after_c.edicts[k].v.origin[2],
			  after_rust.edicts[k].v.origin[0], after_rust.edicts[k].v.origin[1], after_rust.edicts[k].v.origin[2],
			  after_c.edicts[k].v.velocity[0], after_c.edicts[k].v.velocity[1], after_c.edicts[k].v.velocity[2],
			  after_rust.edicts[k].v.velocity[0], after_rust.edicts[k].v.velocity[1], after_rust.edicts[k].v.velocity[2]);
		report (n_cases, "edicts", msg);
	}
	if (memcmp (after_c.nodes, after_rust.nodes, sizeof (sv_areanodes)) != 0)
		report (n_cases, "area nodes", "the relinks differ");
	if (memcmp (&after_c.g, &after_rust.g, sizeof (g)) != 0)
		report (n_cases, "globals", "self, other or time differ");
	if (after_c.dbg_len != after_rust.dbg_len || memcmp (after_c.dbg, after_rust.dbg, after_c.dbg_len) != 0)
	{
		snprintf (msg, sizeof (msg), "%s: the console and QuakeC calls differ\n--- C\n%.300s\n--- Rust\n%.300s",
			  kind_names[c.kind], after_c.dbg, after_rust.dbg);
		report (n_cases, "log", msg);
	}

	/* outcomes, from the C side */
	st_kind[c.kind]++;
	if (after_c.qc != before.qc)
		st_qc++;
	if (memcmp (before.edicts[PL].v.origin, after_c.edicts[PL].v.origin, sizeof (vec3_t)) != 0)
		st_moved++;
	if (strstr (after_c.dbg, "moved back Unstuck")) st_stuck_msg[0]++;
	if (strstr (after_c.dbg, "scooted Unstuck")) st_stuck_msg[1]++;
	if (strstr (after_c.dbg, "player is stuck")) st_stuck_msg[2]++;
	if (strstr (after_c.dbg, "TOSS")) st_toss++;
	if (c.kind == K_TRYUNSTICK)
		st_unstick[rc == 7]++;
	/* the designed exact-4 case (at exactly 4 the C does not return: it
	 * puts the player back and tries the next push, so the end state is
	 * the evidence only against a port that stops there) */
	if (c.kind == K_TRYUNSTICK && world_kind == 3 && (fabsf (c.oldvel[0]) == 20 || fabsf (c.oldvel[1]) == 20))
		st_edge4++;
	if (c.kind == K_WALKMOVE && world_kind == 2 && host_frametime == 1.0 / 32 && before.edicts[PL].v.origin[0] == -0.0625f)
		st_edge32++;
	if (c.kind == K_CHECKWATER)
		st_water[(int) after_c.edicts[PL].v.waterlevel & 3]++;
	if (c.kind == K_WALLFRICTION && memcmp (before.edicts[PL].v.velocity, after_c.edicts[PL].v.velocity, sizeof (vec3_t)) != 0)
		st_friction++;
	/* stepped up: SV_WalkMove sets FL_ONGROUND only for a SOLID_BSP mover,
	 * so the rise is the evidence */
	if (c.kind == K_WALKMOVE && after_c.edicts[PL].v.origin[2] > before.edicts[PL].v.origin[2] + 1 &&
	    after_c.edicts[PL].v.velocity[2] == 0)
		st_stepped++;
	if (c.kind == K_CLIENT && c.num != PL)
		st_inactive++;
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

	if (Phys_TargetFacts.v_v_angle != offsetof (entvars_t, v_angle) ||
	    Phys_TargetFacts.fl_waterjump != FL_WATERJUMP ||
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
	sv_globals.PlayerPreThink = &g.prethink;
	sv_globals.PlayerPostThink = &g.postthink;
	g.prethink = 700;
	g.postthink = 701;
	svs.clients = clients;
	svs.maxclients = NCLIENTS_HARNESS;
	clients[PL - 1].active = true;
#endif

	for (iw = 0; iw < 3000; iw++)
	{
		gen_world ();
		corner = chance (12);
		if (corner)
			gen_corner ();
		world_kind = chance (15) ? 1 : (chance (20) ? 2 : (chance (10) ? 3 : 0));
		host_frametime = chance (60) ? 1.0 / 72 : (chance (50) ? 0.05 : (double) frand (0.001f, 0.1f));
		if (world_kind == 2 && chance (40))
			host_frametime = 1.0 / 32;
		if (world_kind == 1)
			gen_pool ();
		else if (world_kind == 2)
			gen_stair ();
		else if (world_kind == 3)
			gen_empty ();
		gen_edicts ();
		sv.time = chance (50) ? 1.0 + iw * 0.1 : (double) frand (0, 100);
		for (k = 0; k < 40; k++)
			compare_case ();
	}

	printf ("outcomes: calls:");
	for (k = 0; k < NKINDS; k++)
		printf (" %s %ld", kind_names[k], st_kind[k]);
	printf ("\n          player moved %ld, stepped up onto ground %ld, QuakeC calls in %ld cases, tosses %ld\n"
		"          CheckStuck moved back/scooted/stuck %ld/%ld/%ld; TryUnstick freed/7 %ld/%ld\n"
		"          waterlevel 0/1/2/3 %ld/%ld/%ld/%ld; wall friction %ld; inactive slots %ld\n"
		"          forked %ld, %ld reached the fatal\n",
		st_moved, st_stepped, st_qc, st_toss, st_stuck_msg[0], st_stuck_msg[1], st_stuck_msg[2],
		st_unstick[0], st_unstick[1], st_water[0], st_water[1], st_water[2], st_water[3],
		st_friction, st_inactive, n_forked, st_fatal);

#ifndef SV_HW
	for (k = 0; k < NKINDS; k++)
		floor_check (kind_names[k], st_kind[k], 10000);
	floor_check ("a player that moved", st_moved, 10000);
	floor_check ("a step up onto ground", st_stepped, 300);
	floor_check ("CheckStuck: moved back", st_stuck_msg[0], 100);
	floor_check ("CheckStuck: scooted", st_stuck_msg[1], 100);
	floor_check ("CheckStuck: still stuck", st_stuck_msg[2], 100);
	floor_check ("TryUnstick: freed", st_unstick[0], 300);
	floor_check ("TryUnstick: still not moving", st_unstick[1], 300);
	floor_check ("waterlevel 1", st_water[1], 100);
	floor_check ("waterlevel 2", st_water[2], 100);
	floor_check ("waterlevel 3", st_water[3], 100);
	floor_check ("wall friction", st_friction, 1000);
	floor_check ("a tossed player", st_toss, 300);
	floor_check ("an inactive slot", st_inactive, 300);
	floor_check ("the bad-movetype fatal", st_fatal, 300);
	floor_check ("a TryUnstick at the exact-4 edge", st_edge4, 300);
	floor_check ("a WalkMove at the 0.03125 edge", st_edge32, 100);
#else
	floor_check ("SV_Physics_Client", st_kind[K_CLIENT], 50000);
	/* the sweep moves the player only when both traces are clear */
	floor_check ("a player that moved", st_moved, 3000);
	floor_check ("a touch on the sweep", st_qc, 2000);
#endif

	if (n_fail)
	{
		printf ("sv_phys_walk differential harness (%s): FAIL, %ld problem(s) in %ld cases\n",
			VARIANT_NAME, n_fail, n_cases);
		return 1;
	}
	printf ("sv_phys_walk differential harness (%s): PASS (%ld cases: %ld in-process, %ld forked)\n",
		VARIANT_NAME, n_cases, n_cases - n_forked, n_forked);
	return 0;
}
