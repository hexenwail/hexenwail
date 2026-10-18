/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * move_harness.c -- engine/rust/src/sv_phys_move.rs against the C original
 * of sv_phys.c's movetype dispatch, in one binary.
 *
 * run_move_harness.sh builds this against sv_phys_move_h2.c (-DSERVERONLY,
 * as h2ded compiles it) and sv_phys_move_hw.c (-DH2W -DSERVERONLY -DSV_HW,
 * as hwsv).  The real engine/rust/world_target.c and phys_target.c are
 * compiled in.
 *
 * The worlds are the sv_phys harness's.  Every entity gets a movetype --
 * none, noclip, step, pushpull, toss, bounce, bouncemissile, fly,
 * flymissile, swim, push (the brush entities), and in HexenWorld walk and
 * follow -- with velocities and avelocities, thinks around the frame,
 * watertypes (0, empty, water), movechains up to 12 long with and without
 * FL_MOVECHAIN_ANGLE and chainmoved, players (Hexen II's run through
 * SV_Physics_Client), and spawnwalk stamps of this walk or not (#138).
 * Each case calls one of SV_Physics_None, _Noclip, _Toss, _Step,
 * SV_CheckWaterTransition, the whole SV_Physics frame, and in HexenWorld
 * SV_ProgStartFrame, SV_RunEntity, SV_RunNewmis -- once on the C and once on
 * the Rust from the same snapshot, under force_retouch, sv_freezenonclients
 * (Hexen II) or sv_mintic/sv_maxtic against realtime (HexenWorld), and
 * compares byte for byte: every edict, the area nodes, self/other/time,
 * host_frametime, sv.time, sv_physics_walk, force_retouch, the frametime and
 * newmis globals, and every console line, sound and QuakeC call in order.
 *
 * Both sides keep their own per-function statics (the walk counter, and
 * HexenWorld's old_time); every case calls each side once, so they advance
 * together.  A case that can reach a fatal -- a bad movetype, a NaN or huge
 * value anywhere -- runs in forked children.
 *
 * WHAT THE REFERENCE CALLS.  The earlier sv_phys slices' Rust (SV_RunThink,
 * SV_PushEntity, SV_FlyMove, SV_Physics_Pusher, SV_Physics_Client, ...) and
 * the world ports; SV_StartSound and the QuakeC are this harness's.
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

/* the Rust exports these functions; nothing in C calls them, so no engine
 * header declares them */
void		SV_Physics_None (edict_t *ent);
void		SV_Physics_Noclip (edict_t *ent);
void		SV_CheckWaterTransition (edict_t *ent);
void		SV_Physics_Step (edict_t *ent);
#ifdef SV_HW
void		SV_RunEntity (edict_t *ent);
#endif
/* the C original's static functions, through c_ref_move.c's wrappers */
void		cw_SV_Physics_None (edict_t *ent);
void		cw_SV_Physics_Noclip (edict_t *ent);
void		cw_SV_CheckWaterTransition (edict_t *ent);
void		cw_SV_Physics_Step (edict_t *ent);
void		c_SV_Physics_Toss (edict_t *ent);
void		c_SV_Physics (void);
#ifdef SV_HW
void		c_SV_ProgStartFrame (void);
void		cw_SV_RunEntity (edict_t *ent);
void		c_SV_RunNewmis (void);
#define VARIANT_NAME	"hexenworld"
#define WANT_ERROR_FN	"SV_Error"
#else
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
double		realtime;
int		sv_physics_walk;
#ifdef SV_HW
cvar_t		sv_mintic = { "sv_mintic", "0.03", CVAR_NONE };
cvar_t		sv_maxtic = { "sv_maxtic", "0.1", CVAR_NONE };
#else
cvar_t		sv_freezenonclients = { "sv_freezenonclients", "0", CVAR_NONE };
#endif
#ifndef SV_HW
static client_t	clients[NCLIENTS_HARNESS];
#endif

static struct
{
	int	self, other, world;
	float	time;
	func_t	prethink, postthink;
	float	force_retouch;
	func_t	startframe;
	float	frametime;
	int	newmis;
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

/* more than HexenWorld's MAX_CLIENTS (32) + 1: its SV_Physics skips edicts
 * 1..32 as clients, so the entities it runs through SV_RunEntity sit above */
#define NE	40

static edict_t	edicts[NE];
static float	gravity_field[NE];
static int	gravity_has[NE];

static long	idx (const edict_t *e)	{ return e ? (long) (e - edicts) : -1L; }

/* pr_edict.c's NUM_FOR_EDICT, for a valid entity (the pushers') */
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
	case 4:	/* spawn (): a free slot, stamped as ED_Alloc stamps it (#138) */
	{
		int	k;
		for (k = NE - 4; k < NE; k++)
			if (edicts[k].free)
			{
				edict_t	*e = &edicts[k];
				memset (&e->v, 0, sizeof (e->v));
				e->free = false;
				e->spawnwalk = sv_physics_walk;
				e->v.movetype = (n_qc / 16) % 2 ? MOVETYPE_TOSS : MOVETYPE_FLY;
				e->v.solid = SOLID_NOT;
				VectorCopy (self->v.origin, e->v.origin);
				e->v.velocity[2] = 100;
				if (sv.num_edicts <= k)
					sv.num_edicts = k + 1;
				break;
			}
		break;
	}
	case 6:	/* HexenWorld's newmis: the entity it just made */
		g.newmis = (int) ((byte *) self - (byte *) sv.edicts);
		break;
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
		/* only movetypes the next dispatch takes, since this runs
		 * mid-frame, outside a child: a client slot gets
		 * SV_Physics_Client's, any other entity SV_Physics' (no walk) */
		static const int	cmt[] = { MOVETYPE_WALK, MOVETYPE_FLY, MOVETYPE_NOCLIP, MOVETYPE_NONE, MOVETYPE_TOSS };
		static const int	mt[] = { MOVETYPE_STEP, MOVETYPE_FLY, MOVETYPE_NOCLIP, MOVETYPE_NONE, MOVETYPE_TOSS };
		long	si = idx (self);
#ifdef SV_HW
		(void) cmt; (void) si;
		if (self->v.solid != SOLID_BSP)
			self->v.movetype = (float) mt[(n_qc / 16) % 5];
#else
		if (self->v.solid != SOLID_BSP)
			self->v.movetype = (float) ((si >= 1 && si <= svs.maxclients) ? cmt : mt)[(n_qc / 16) % 5];
#endif
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

/* sv_main.c's SV_StartSound: logged */
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation)
{
	dbg_add ("SOUND e%ld %d %s %d %a\n", idx (entity), channel, sample, volume, attenuation);
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
/* the entities                                                             */
/* ------------------------------------------------------------------------ */

#define NSPARE	4	/* slots past sv.num_edicts, for the stub's spawn () */

static int is_brush (const edict_t *e)
{
	return e->v.solid == SOLID_BSP;
}

static void gen_mover (edict_t *e, int i)
{
	static const int	mts[] = { MOVETYPE_NONE, MOVETYPE_NOCLIP, MOVETYPE_STEP, MOVETYPE_PUSHPULL,
					MOVETYPE_TOSS, MOVETYPE_BOUNCE, MOVETYPE_BOUNCEMISSILE, MOVETYPE_FLY,
					MOVETYPE_FLYMISSILE, MOVETYPE_SWIM, MOVETYPE_WALK, MOVETYPE_FOLLOW, MOVETYPE_TOSS };
	int	k;

	if (is_brush (e))
	{
		e->v.movetype = MOVETYPE_PUSH;
		for (k = 0; k < 3; k++)
			e->v.velocity[k] = chance (50) ? 0 : frand (-100, 100);
		e->v.ltime = chance (50) ? 0 : (float) sv.time;
	}
	else
	{
		e->v.movetype = (float) mts[rnd () % 13];
#ifndef SV_HW
		/* walk and follow are HexenWorld's: a Hexen II non-client with
		 * either is fatal, which the rare bad movetypes below cover */
		if (e->v.movetype == MOVETYPE_WALK || e->v.movetype == MOVETYPE_FOLLOW)
			e->v.movetype = MOVETYPE_TOSS;
#endif
		for (k = 0; k < 3; k++)
			e->v.velocity[k] = chance (15) ? 0 : frand (-400, 400);
		if (chance (30))
			e->v.velocity[2] = frand (-600, -100);
		/* falling onto a floor: landings, bounces, water transitions */
		if (chance (35))
		{
			place (e);
			e->v.velocity[2] = -frand (50, 400);
			if (chance (40))
				e->v.movetype = chance (50) ? MOVETYPE_BOUNCE : MOVETYPE_BOUNCEMISSILE;
			SV_LinkEdict (e, false);
		}
		/* a movetype the dispatch rejects: fatal (rare per entity: a world
		 * holding one forks every case).  Fractional ones too: Hexen II
		 * compares the float, so 4.5 is fatal there; HexenWorld switches on
		 * (int), so 4.5 is MOVETYPE_STEP */
		if ((rnd () % 1000) < 3)
		{
			static const float	bad[] = { 1, 2, 15, 99, 4.5f, 6.25f, 9.75f, -0.5f, 13.5f, 0.5f };
			e->v.movetype = bad[rnd () % 10];
			if (chance (10))
				e->v.movetype = NAN;
		}
	}
#ifndef SV_HW
	/* the client slots: SV_Physics runs them through SV_Physics_Client */
	if (i >= 1 && i <= svs.maxclients && !is_brush (e))
	{
		static const int	cm[] = { MOVETYPE_WALK, MOVETYPE_WALK, MOVETYPE_NONE, MOVETYPE_TOSS, MOVETYPE_FLY, MOVETYPE_NOCLIP };
		e->v.movetype = (float) cm[rnd () % 6];
	}
#endif
	for (k = 0; k < 3; k++)
		e->v.avelocity[k] = chance (60) ? 0 : frand (-200, 200);
	e->v.flags = (float) ((chance (40) ? FL_ONGROUND : 0) | (chance (20) ? FL_FLY : 0) | (chance (15) ? FL_SWIM : 0) |
			      (chance (40) ? FL_MONSTER : 0) | (chance (20) ? FL_MOVECHAIN_ANGLE : 0));
	e->v.nextthink = chance (40) ? 0 : (float) (sv.time + (chance (50) ? frand (-0.1f, 0.1f) : 0));
	e->v.think = chance (85) ? 800 + i : 0;
	e->v.touch = chance (40) ? 850 + i : 0;
	e->v.watertype = chance (30) ? 0 : (chance (50) ? CONTENTS_EMPTY : CONTENTS_WATER);
	e->v.waterlevel = (float) (rnd () % 4);
	e->v.movechain = 0;
	e->v.chainmoved = chance (50) ? 900 + i : 0;
#ifdef SV_HW
	e->v.lastruntime = chance (20) ? (float) realtime : 0;
#endif
	e->spawnwalk = 0;
	if (e->v.solid == SOLID_PHASE || chance (5))
		e->v.solid = SOLID_PHASE;
	if ((rnd () % 1000) < 1)
		e->v.velocity[rnd () % 3] = odd_value ();
	/* place () leaves an odd origin 2% of the time, which over 40 entities
	 * would fork most cases: keep one in six */
	for (k = 0; k < 3; k++)
		if ((!isfinite (e->v.origin[k]) || fabsf (e->v.origin[k]) > 1e9f) && !chance (15))
		{
			e->v.origin[k] = frand (-100, 100);
			SV_LinkEdict (e, false);
		}
}

/* movechains: a few entities each drag a chain of up to 12 others (the C
 * walks 10), and some chains loop back to the world early */
static void gen_chains (void)
{
	int	n = 1 + (int) (rnd () % 6), c;
	for (c = 0; c < n; c++)
	{
		int	head = 1 + (int) (rnd () % (NE - NSPARE - 1)), len = 1 + (int) (rnd () % 12), k, at = head;
		for (k = 0; k < len; k++)
		{
			int	next = 1 + (int) (rnd () % (NE - NSPARE - 1));
			edicts[at].v.movechain = (k == len - 1 && chance (50)) ? 0 : next * (int) sizeof (edict_t);
			at = next;
		}
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
	double		host_frametime, sv_time;
	int		walk, num_edicts;
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
	s->host_frametime = host_frametime;
	s->sv_time = sv.time;
	s->walk = sv_physics_walk;
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
	host_frametime = s->host_frametime;
	sv.time = s->sv_time;
	sv_physics_walk = s->walk;
	sv.num_edicts = s->num_edicts;
}

static void canon_state (state_t *s)
{
	int	k;
	for (k = 0; k < NE; k++)
	{
		entvars_t	*v = &s->edicts[k].v;
		canon_floats (v->origin, 3);
		canon_floats (v->oldorigin, 3);
		canon_floats (v->angles, 3);
		canon_floats (v->velocity, 3);
		canon_floats (v->avelocity, 3);
		canon_floats (&v->flags, 1);
		canon_floats (v->absmin, 3);
		canon_floats (v->absmax, 3);
		canon_floats (&v->nextthink, 1);
		canon_floats (&v->hoverz, 1);
		canon_floats (&v->waterlevel, 1);
		canon_floats (&v->watertype, 1);
		canon_floats (&v->movetype, 1);
		canon_floats (&v->ltime, 1);
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

enum { K_NONE, K_NOCLIP, K_WATER, K_TOSS, K_STEP, K_PHYSICS, K_STARTFRAME, K_RUNENTITY, K_NEWMIS, NKINDS };
static const char *const kind_names[NKINDS] =
	{ "SV_Physics_None", "SV_Physics_Noclip", "SV_CheckWaterTransition", "SV_Physics_Toss",
	  "SV_Physics_Step", "SV_Physics", "SV_ProgStartFrame", "SV_RunEntity", "SV_RunNewmis" };

typedef struct
{
	int	kind;
	edict_t	*ent;
} call_t;

static int run_one (const call_t *c, int rust, state_t *s)
{
	(void) s;
	switch (c->kind)
	{
	case K_NONE: if (rust) SV_Physics_None (c->ent); else cw_SV_Physics_None (c->ent); break;
	case K_NOCLIP: if (rust) SV_Physics_Noclip (c->ent); else cw_SV_Physics_Noclip (c->ent); break;
	case K_WATER: if (rust) SV_CheckWaterTransition (c->ent); else cw_SV_CheckWaterTransition (c->ent); break;
	case K_TOSS: if (rust) SV_Physics_Toss (c->ent); else c_SV_Physics_Toss (c->ent); break;
	case K_STEP: if (rust) SV_Physics_Step (c->ent); else cw_SV_Physics_Step (c->ent); break;
#ifdef SV_HW
	case K_STARTFRAME: if (rust) SV_ProgStartFrame (); else c_SV_ProgStartFrame (); break;
	case K_RUNENTITY: if (rust) SV_RunEntity (c->ent); else cw_SV_RunEntity (c->ent); break;
	case K_NEWMIS: if (rust) SV_RunNewmis (); else c_SV_RunNewmis (); break;
#endif
	default: if (rust) SV_Physics (); else c_SV_Physics (); break;
	}
	return 0;
}

static long	n_cases, n_fail, n_forked;
static long	st_kind[NKINDS], st_fatal, st_qc, st_sound, st_chainmoved, st_spawn_skipped, st_bounced, st_landed;
static long	st_tic_skip, st_lastrun_skip, st_newmis, st_frozen, st_retouch;

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
	{
		float	fmt = edicts[e].v.movetype;
		int	mt = isfinite (fmt) ? (int) fmt : -1;
		/* a fractional movetype: Hexen II's float compares reject it, so
		 * it is fatal there, and HexenWorld's (int) may not be; fork it */
		if (!edicts[e].free && isfinite (fmt) && fmt != (float) mt)
			return 1;
		/* a movetype the dispatch rejects as fatal: Hexen II has no walk
		 * (but for its clients) or follow branch, HexenWorld has both;
		 * Hexen II's clients go through SV_Physics_Client, which takes
		 * only none, walk, toss, bounce, fly, swim and noclip */
		int	bad = mt < 0 || mt > MOVETYPE_SWIM || mt == 1 || mt == 2;
#ifndef SV_HW
		if (e >= 1 && e <= svs.maxclients)
			bad = !(mt == MOVETYPE_NONE || mt == MOVETYPE_WALK || mt == MOVETYPE_TOSS || mt == MOVETYPE_BOUNCE ||
				mt == MOVETYPE_FLY || mt == MOVETYPE_SWIM || mt == MOVETYPE_NOCLIP);
		else
			bad = bad || mt == MOVETYPE_WALK || mt == MOVETYPE_FOLLOW;
#endif
		if (!edicts[e].free && bad)
			return 1;
		for (k = 0; k < 3; k++)
			if (odd (edicts[e].v.velocity[k]) || odd (edicts[e].v.origin[k]) || odd (edicts[e].v.avelocity[k]) ||
			    odd (edicts[e].v.absmin[k]) || odd (edicts[e].v.absmax[k]))
				return 1;
	}
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
		close (fds[0]);
		err_fd = fds[1];
		setitimer (ITIMER_PROF, &it, NULL);
		ret = run_one (c, rust, &s);
		save (&s, ret);
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

static long	n_physics;	/* SV_Physics calls each side has made: the walk counter */
#ifdef SV_HW
/* HexenWorld's SV_Physics' static old_time, mirrored: both sides advance
 * theirs together, on every in-process frame that is not a tic skip */
static double	h_old_time;
#endif

/* the gravity this world was given, which is SV_Physics_Step's threshold */
static float gravity_now (void)
{
#ifdef SV_HW
	return movevars.gravity;
#else
	return sv_gravity.value;
#endif
}

static void gen_call (call_t *c)
{
	int	k;

	memset (c, 0, sizeof (*c));
	c->kind = (int) (rnd () % NKINDS);
#ifndef SV_HW
	if (c->kind >= K_STARTFRAME)
		c->kind = K_PHYSICS;
#endif
	/* realtime first: lastruntime is set from it, for SV_RunEntity's
	 * once-per-realtime test */
	realtime = chance (50) ? sv.time + frand (0, 0.2f) : realtime + (chance (50) ? 0.02 : frand (0, 0.15f));
#ifdef SV_HW
	/* exactly sv_mintic since the last frame: the `<' edge */
	if (chance (10))
		realtime = h_old_time + (double) sv_mintic.value;
#endif
#ifndef SV_HW
	svs.maxclients = 1 + (int) (rnd () % 3);
#endif
	/* the spares, free and unlinked, past sv.num_edicts */
	for (k = NE - NSPARE; k < NE; k++)
	{
		if (edicts[k].area.prev)
			SV_UnlinkEdict (&edicts[k]);
		edicts[k].free = true;
	}
	sv.num_edicts = NE - NSPARE;
#ifndef SV_HW
	/* the client slots are players, never brushes */
	for (k = 1; k <= svs.maxclients; k++)
		if (is_brush (&edicts[k]))
		{
			edicts[k].v.solid = SOLID_SLIDEBOX;
			edicts[k].v.modelindex = 0;
			edicts[k].v.movetype = MOVETYPE_WALK;
			SV_LinkEdict (&edicts[k], false);
		}
#endif
	for (k = 0; k < NE - NSPARE; k++)
		if (!edicts[k].free && k > 0)
			gen_mover (&edicts[k], k);
	gen_chains ();
	c->ent = &edicts[1 + (int) (rnd () % (NE - NSPARE - 1))];
	c->ent->free = false;
	/* a phased bouncing missile fired at a brush entity flagged FL_MONSTER:
	 * SV_PushEntity's MOVE_PHASE still clips SOLID_BSP, so the trace ends
	 * on the "monster", and SV_Physics_Toss returns without bouncing */
	if (chance (6))
	{
		int	b;
		for (b = 1; b < NE - NSPARE; b++)
			if (is_brush (&edicts[b]) && &edicts[b] != c->ent)
				break;
		if (b < NE - NSPARE && !is_brush (c->ent))
		{
			edict_t	*br = &edicts[b];
			br->v.flags = (float) ((int) br->v.flags | FL_MONSTER);
			c->ent->v.solid = SOLID_PHASE;
			c->ent->v.movetype = MOVETYPE_BOUNCEMISSILE;
			c->ent->v.flags = 0;
			c->ent->v.nextthink = 0;
			for (k = 0; k < 2; k++)
				c->ent->v.origin[k] = (br->v.absmin[k] + br->v.absmax[k]) * 0.5f;
			c->ent->v.origin[2] = br->v.absmin[2] - 8;
			VectorSet (c->ent->v.velocity, 0, 0, 3000);
			SV_LinkEdict (c->ent, false);
			c->kind = K_TOSS;
		}
	}
#ifndef SV_HW
	/* sv_freezenonclients with fewer edicts than the clients' cap: the
	 * walk stops at what is allocated */
	if (chance (4))
	{
		sv_freezenonclients.integer = 1;
		sv.num_edicts = 1 + (int) (rnd () % 3);
		c->kind = K_PHYSICS;
	}
#endif
	/* edges, on the entity run:
	 *   - SV_Physics_Step's landing sound threshold: velocity[2] exactly at
	 *     gravity*-0.1, where float and double rounding part;
	 *   - SV_Physics_Toss's bounce stop: a bounce leaving the floor at just
	 *     over 60;
	 *   - its `normal[2] > 0.7': a slope of normal z 0.705 under it */
	if (chance (8) && !is_brush (c->ent))
	{
		switch (rnd () % 3)
		{
		case 0:
		{
			float	t = (float) ((double) gravity_now () * -0.1);
			place (c->ent);	/* over a floor, so it lands and the sound plays */
			c->ent->v.origin[2] += 0.5f;
			SV_LinkEdict (c->ent, false);
			c->ent->v.movetype = MOVETYPE_STEP;
			c->ent->v.flags = 0;
			/* t is the double threshold rounded to float; tf the float
			 * product, which lands on the other side of the double one
			 * whenever the two roundings disagree */
			float	tf = gravity_now () * -0.1f;
			switch (rnd () % 4)
			{
			case 0: c->ent->v.velocity[2] = t; break;
			case 1: c->ent->v.velocity[2] = tf; break;
			case 2: c->ent->v.velocity[2] = nextafterf (t, 0); break;
			default: c->ent->v.velocity[2] = nextafterf (t, -1000); break;
			}
			c->kind = K_STEP;
			break;
		}
		case 1:
			place (c->ent);
			c->ent->v.movetype = MOVETYPE_BOUNCE;
			c->ent->v.flags = 0;
			c->ent->v.nextthink = 0;
			c->ent->v.solid = SOLID_BBOX;
			/* -40.5 after this frame's gravity; 1.5 backoff off a floor
			 * leaves 60.75, which bounces under `< 60' but not `< 61' */
			c->ent->v.velocity[2] = frand (-40.67f, -40.0f) * 3.0f + gravity_now () * (float) host_frametime;
			c->ent->v.velocity[0] = c->ent->v.velocity[1] = 0;
			c->ent->v.origin[2] += 1;
			SV_LinkEdict (c->ent, false);
			c->kind = K_TOSS;
			break;
		default:
		{
			/* a slope through the origin, normal (0.709, 0, 0.705) */
			int	h;
			memset (&planes[1][0], 0, sizeof (mplane_t));
			planes[1][0].normal[0] = 0.70920376f;
			planes[1][0].normal[2] = 0.705f;
			planes[1][0].type = 3;
			clipnodes[1][0][0].planenum = 0;
			clipnodes[1][0][0].children[0] = CONTENTS_EMPTY;
			clipnodes[1][0][0].children[1] = CONTENTS_SOLID;
			for (h = 0; h < MAX_MAP_HULLS; h++)
			{
				models[1].hulls[h].clipnodes = clipnodes[1][0];
				models[1].hulls[h].planes = planes[1];
				models[1].hulls[h].firstclipnode = 0;
				models[1].hulls[h].lastclipnode = 0;
			}
			VectorSet (c->ent->v.origin, 0, 0, 4);
			VectorSet (c->ent->v.velocity, 0, 0, -400);
			c->ent->v.movetype = chance (50) ? MOVETYPE_TOSS : MOVETYPE_BOUNCE;
			c->ent->v.flags = 0;
			c->ent->v.nextthink = 0;
			c->ent->v.solid = SOLID_BBOX;
			SV_LinkEdict (c->ent, false);
			c->kind = K_TOSS;
			break;
		}
		}
	}
	/* the dispatch's fatal, directly: the entity run has a bad movetype */
	if (chance (2) && !is_brush (c->ent))
	{
		c->ent->v.movetype = chance (50) ? 1 : 99;
		c->kind = chance (50) ? K_PHYSICS : K_RUNENTITY;
#ifndef SV_HW
		c->kind = K_PHYSICS;
#endif
	}
	/* stamped by this walk, which is the next one: skipped unless a client */
	for (k = 1; k < NE - NSPARE; k++)
		if (chance (5))
			edicts[k].spawnwalk = (int) ((n_physics + 1) & 0x7fffffff);
	g.force_retouch = chance (35) ? (float) (1 + rnd () % 3) : 0;
	/* (sv_freezenonclients is drawn below, unless the cap case above set it) */
	g.newmis = chance (30) ? (int) (rnd () % NE) * (int) sizeof (edict_t) : 0;
#ifndef SV_HW
	if (!(sv_freezenonclients.integer && sv.num_edicts < 4))
		sv_freezenonclients.integer = chance (10);
#endif
}

static void compare_case (void)
{
	call_t	c;
	int	rc, rr;
	char	msg[900];

	n_cases++;
	gen_call (&c);
	g.self = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.other = (int) (rnd () % NE) * (int) sizeof (edict_t);
	g.time = frand (0, 10);
	dbg_len = 0;
	dbg[0] = 0;
	save (&before, 0);

	if (has_odd ())
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
		if (strncmp (oc, WANT_ERROR_FN "|", sizeof (WANT_ERROR_FN)) == 0)
			st_fatal++;
		st_kind[c.kind]++;
		return;
	}

	rc = run_one (&c, 0, &after_c);
	save (&after_c, rc);
	restore (&before);
	rr = run_one (&c, 1, &after_rust);
	save (&after_rust, rr);
	if (c.kind == K_PHYSICS)
	{
		n_physics++;
#ifdef SV_HW
		/* a frame that ran (not a tic skip) moved old_time to realtime */
		if (after_c.host_frametime >= sv_mintic.value || before.host_frametime != after_c.host_frametime)
			if (!(realtime - h_old_time < sv_mintic.value))
				h_old_time = realtime;
#endif
	}
	canon_state (&after_c);
	canon_state (&after_rust);

	if (memcmp (after_c.edicts, after_rust.edicts, sizeof (edicts)) != 0)
	{
		int	k;
		for (k = 0; k < NE; k++)
			if (memcmp (&after_c.edicts[k], &after_rust.edicts[k], sizeof (edict_t)) != 0)
				break;
		snprintf (msg, sizeof (msg), "%s: edict %d differs (origin C %a %a %a, Rust %a %a %a; movetype %g)",
			  kind_names[c.kind], k,
			  after_c.edicts[k].v.origin[0], after_c.edicts[k].v.origin[1], after_c.edicts[k].v.origin[2],
			  after_rust.edicts[k].v.origin[0], after_rust.edicts[k].v.origin[1], after_rust.edicts[k].v.origin[2],
			  before.edicts[k].v.movetype);
		report (n_cases, "edicts", msg);
	}
	if (memcmp (after_c.nodes, after_rust.nodes, sizeof (sv_areanodes)) != 0)
		report (n_cases, "area nodes", "the relinks differ");
	if (memcmp (&after_c.g, &after_rust.g, sizeof (g)) != 0)
		report (n_cases, "globals", "self/other/time, force_retouch, frametime or newmis differ");
	if (after_c.host_frametime != after_rust.host_frametime || after_c.sv_time != after_rust.sv_time ||
	    after_c.walk != after_rust.walk || after_c.num_edicts != after_rust.num_edicts)
	{
		snprintf (msg, sizeof (msg), "%s: frametime C %a Rust %a, sv.time C %a Rust %a, walk C %d Rust %d",
			  kind_names[c.kind], after_c.host_frametime, after_rust.host_frametime,
			  after_c.sv_time, after_rust.sv_time, after_c.walk, after_rust.walk);
		report (n_cases, "frame", msg);
	}
	if (after_c.dbg_len != after_rust.dbg_len || memcmp (after_c.dbg, after_rust.dbg, after_c.dbg_len) != 0)
	{
		snprintf (msg, sizeof (msg), "%s: the console, sounds and QuakeC calls differ\n--- C\n%.300s\n--- Rust\n%.300s",
			  kind_names[c.kind], after_c.dbg, after_rust.dbg);
		report (n_cases, "log", msg);
	}

	/* outcomes, from the C side */
	st_kind[c.kind]++;
	if (after_c.qc != before.qc) st_qc++;
	if (strstr (after_c.dbg, "SOUND")) st_sound++;
	if (strstr (after_c.dbg, "chainmoved")) st_chainmoved++;
	if (after_c.g.force_retouch != before.g.force_retouch) st_retouch++;
	if (c.kind == K_TOSS || c.kind == K_STEP)
	{
		int	k = (int) (c.ent - edicts);
		if (((int) after_c.edicts[k].v.flags & FL_ONGROUND) && !((int) before.edicts[k].v.flags & FL_ONGROUND))
			st_landed++;
		if ((before.edicts[k].v.movetype == MOVETYPE_BOUNCE || before.edicts[k].v.movetype == MOVETYPE_BOUNCEMISSILE) &&
		    after_c.edicts[k].v.velocity[2] * before.edicts[k].v.velocity[2] < 0)
			st_bounced++;
	}
	if (c.kind == K_PHYSICS || c.kind == K_RUNENTITY)
	{
		int	k;
		for (k = 1; k < NE - NSPARE; k++)
		{
			if (before.edicts[k].spawnwalk && !before.edicts[k].free &&
			    memcmp (&before.edicts[k].v, &after_c.edicts[k].v, sizeof (entvars_t)) == 0)
				st_spawn_skipped++;
			if (((int) after_c.edicts[k].v.flags & FL_ONGROUND) && !((int) before.edicts[k].v.flags & FL_ONGROUND))
				st_landed++;
			if ((before.edicts[k].v.movetype == MOVETYPE_BOUNCE || before.edicts[k].v.movetype == MOVETYPE_BOUNCEMISSILE) &&
			    after_c.edicts[k].v.velocity[2] * before.edicts[k].v.velocity[2] < 0)
				st_bounced++;
		}
	}
#ifdef SV_HW
	if (c.kind == K_PHYSICS && after_c.qc == before.qc)
		st_tic_skip++;
	if (c.kind == K_RUNENTITY && memcmp (&before.edicts[c.ent - edicts], &after_c.edicts[c.ent - edicts], sizeof (edict_t)) == 0)
		st_lastrun_skip++;
	if (c.kind == K_NEWMIS && before.g.newmis)
		st_newmis++;
#else
	if (c.kind == K_PHYSICS && sv_freezenonclients.integer)
		st_frozen++;
#endif
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

	if (Phys_TargetFacts.v_movechain != offsetof (entvars_t, movechain) ||
	    Phys_TargetFacts.edict_spawnwalk != offsetof (edict_t, spawnwalk) ||
	    Phys_TargetFacts.fl_movechain_angle != FL_MOVECHAIN_ANGLE)
	{
		fprintf (stderr, "harness bug: the target shims are not this build's\n");
		return 2;
	}
	sv_globals.self = &g.self;
	sv_globals.other = &g.other;
	sv_globals.world = &g.world;
	sv_globals.time = &g.time;
	sv_globals.force_retouch = &g.force_retouch;
	sv_globals.StartFrame = &g.startframe;
	g.startframe = 990;
	sv_maxvelocity.value = 2000;
#ifdef SV_HW
	movevars.gravity = 800;
	sv_globals.frametime = &g.frametime;
	sv_globals.newmis = &g.newmis;
#else
	sv_gravity.value = 800;
	sv_globals.PlayerPreThink = &g.prethink;
	sv_globals.PlayerPostThink = &g.postthink;
	g.prethink = 700;
	g.postthink = 701;
	svs.clients = clients;
	svs.maxclients = 2;
	for (k = 0; k < NCLIENTS_HARNESS; k++)
		clients[k].active = k < 2;
#endif

	for (iw = 0; iw < 2500; iw++)
	{
		gen_world ();
		corner = chance (12);
		if (corner)
			gen_corner ();
		gen_edicts ();
		host_frametime = chance (60) ? 1.0 / 72 : (chance (50) ? 0.05 : (double) frand (0.001f, 0.1f));
		sv.time = chance (50) ? 1.0 + iw * 0.1 : (double) frand (0, 100);
		sv_player = &edicts[1];
		/* gravity other than 800 too: where gravity*-0.1 rounds differently
		 * in float and in double */
		{
			float	gr = chance (50) ? 800 : frand (100, 1600);
#ifdef SV_HW
			movevars.gravity = gr;
#else
			sv_gravity.value = gr;
#endif
		}
		for (k = 0; k < 24; k++)
			compare_case ();
	}

	printf ("outcomes: calls:");
	for (k = 0; k < NKINDS; k++)
		printf (" %s %ld", kind_names[k], st_kind[k]);
	printf ("\n          QuakeC calls in %ld cases, sounds %ld, chainmoved %ld, spawn-walk skips %ld\n"
		"          landed %ld, bounced %ld, force_retouch counted down %ld; forked %ld, fatal %ld\n"
		"          HexenWorld tic skips %ld, lastruntime skips %ld, newmis %ld; Hexen II frozen frames %ld\n",
		st_qc, st_sound, st_chainmoved, st_spawn_skipped, st_landed, st_bounced, st_retouch,
		n_forked, st_fatal, st_tic_skip, st_lastrun_skip, st_newmis, st_frozen);

	floor_check ("a QuakeC call", st_qc, 8000);
	floor_check ("a water sound", st_sound, 500);
	floor_check ("a chainmoved call", st_chainmoved, 500);
	floor_check ("a landing", st_landed, 300);
	floor_check ("a bounce", st_bounced, 150);
	floor_check ("a force_retouch countdown", st_retouch, 200);
	floor_check ("the bad-movetype fatal", st_fatal, 200);
	floor_check ("an SV_Physics frame", st_kind[K_PHYSICS], 5000);
#ifdef SV_HW
	floor_check ("an sv_mintic skip", st_tic_skip, 200);
	floor_check ("a lastruntime skip", st_lastrun_skip, 100);
	floor_check ("a newmis run", st_newmis, 500);
#else
	floor_check ("a frozen frame", st_frozen, 300);
	floor_check ("a spawn-walk skip", st_spawn_skipped, 200);
#endif

	if (n_fail)
	{
		printf ("sv_phys_move differential harness (%s): FAIL, %ld problem(s) in %ld cases\n",
			VARIANT_NAME, n_fail, n_cases);
		return 1;
	}
	printf ("sv_phys_move differential harness (%s): PASS (%ld cases: %ld in-process, %ld forked)\n",
		VARIANT_NAME, n_cases, n_cases - n_forked, n_forked);
	return 0;
}
