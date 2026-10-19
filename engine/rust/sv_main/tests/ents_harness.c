/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ents_harness.c -- engine/rust/src/sv_main_ents.rs against the C original
 * of Hexen II's sv_main.c entity updates, in one binary.
 *
 * run_ents_harness.sh builds this against sv_main_ents_h2.c (-DSERVERONLY,
 * as h2ded compiles it), with the real engine/rust/world_target.c and
 * engine/rust/sv_main_target.c compiled in.
 *
 * Each case builds what SV_PrepareClientEntities reads:
 *   - a world: a random BSP tree of up to 31 nodes over up to 300 leafs,
 *     planes axial or oblique and often within 8 units of the viewer, and a
 *     random PVS per leaf (Mod_LeafPVS below);
 *   - up to 900 edicts: every mix of NODRAW and muzzle flash, models present
 *     and not, FL_CLIENT, FL_MONSTER and FL_CLASS_DEPENDENT, the missile
 *     movetypes and MOVETYPE_STEP, leaf lists empty, in the PVS, out of it
 *     and full (MAX_ENT_LEAFS, always sent), byte fields at and past 255,
 *     scale and abslight, an alpha field or none, baselines and ClearCount
 *     on each side of CLEAR_LIMIT, and odd floats throughout;
 *   - one client of four: its frame and sequence bytes in every branch of
 *     the frame state machine, its seven reference frames filled with sorted
 *     entries, some ENT_CLEARED, some equal to the edict's current state so
 *     zero deltas happen, its playerclass, and a camera on another edict;
 *   - the cvars: sv_update_* (0, negative, 1..3), sv_netsort, the protocol;
 *   - the datagram: maxsize from a 300-byte one to 65000, cursize from
 *     empty to three bytes short of full, allowoverflow off as the engine
 *     has it and sometimes on.
 * Four huge scenarios (8300 edicts) drive each `bcount >= MAX_CLIENT_STATES'
 * break: a full write, a full reference of zero deltas, a measuring pass,
 * and a full reference shed by the size guard.
 *
 * Then SV_PrepareClientEntities runs once on the C and once on the Rust from
 * the same snapshot, and SV_CleanupEnts on a third of the cases, and they
 * compare byte for byte: the datagram (cursize, overflowed, every byte up to
 * maxsize); every frame of the client's client_state2_t, field by field
 * with the padding left out (C leaves it unspecified), over every entry the
 * call can have written -- the build frame takes one per edict, whatever the
 * reference held, so the region is the whole of that, not the reference's
 * own fill; the four frame/sequence bytes; each edict's ClearCount for the
 * client and its effects, including the two edicts past num_edicts, so a
 * SV_CleanupEnts that runs one edict too many shows; the fat PVS; and the
 * log: every console line with its level, every SV_ModelIndex call with the
 * class-dependent name it was given.
 *
 * Host_Error (QuakeC flags past bit 23) and Sys_Error (SZ_GetSpace without
 * allowoverflow) do not return: a case that can reach either runs forked,
 * once per side, and the children report a digest of the same outputs.  A
 * fatal the generator did not foresee is caught rather than aborted on: it
 * is a difference between the two sides, not a harness bug, and a mutated
 * datagram size is exactly how one side reaches it.
 *
 * A NaN box is the one input where the two sides are meant to disagree: the
 * C dies on it (see SV_EntityPriority) and the port sheds the entity as the
 * least important bin.  Those cases run forked as well, and the runner
 * checks for exactly that instead of for agreement.
 *
 * After the random cases come the directed ones (below): values the
 * generator does not reach, each the kill of a mutant that survived every
 * random case.
 *
 * The comparison covers every entered frame entry, the client's four
 * frame/sequence bytes, each edict's ClearCount and effects, the fat PVS
 * and the log -- and the two edicts past num_edicts, so SV_CleanupEnts
 * stopping one edict late shows.
 *
 * Before the cases, the reference-frame structs sv_main_ents.rs mirrors are
 * checked against this compile of the Hexen II headers.
 */

#include "quakedef.h"
#include "sv_main_target.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void	c_SV_PrepareClientEntities (client_t *client, edict_t *clent, sizebuf_t *msg);
void	c_SV_CleanupEnts (void);
void	SV_PrepareClientEntities (client_t *client, edict_t *clent, sizebuf_t *msg);
void	SV_CleanupEnts (void);
extern int	c_fatbytes;
extern byte	c_fatpvs[MAX_MAP_LEAFS/8];

/* the engine's storage */
server_t	sv;
server_static_t	svs;
int		sv_protocol;
int		sv_max_edicts;
int		pr_edict_size = sizeof (edict_t);
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
sizebuf_t	net_message;
cvar_t	sv_update_player = { "sv_update_player", "1", CVAR_NONE };
cvar_t	sv_update_monsters = { "sv_update_monsters", "1", CVAR_NONE };
cvar_t	sv_update_missiles = { "sv_update_missiles", "1", CVAR_NONE };
cvar_t	sv_update_misc = { "sv_update_misc", "1", CVAR_NONE };
cvar_t	sv_netsort = { "sv_netsort", "1", CVAR_NONE };

#define MAXC		4
#define NE_MAX		8400
#define NE_NORMAL	700
#define NLEAF_MAX	301
#define NNODE_MAX	31
#define PVSBYTES	64
#define MSGMAX		65536

static client_t		clients[MAXC];
static byte		*edict_store;
static float		alpha_val[NE_MAX];
static byte		has_alpha[NE_MAX];
static qmodel_t		world;
static mnode_t		nodes[NNODE_MAX];
static mleaf_t		leafs[NLEAF_MAX];
static mplane_t		planes[NNODE_MAX];
static byte		leafpvs[NLEAF_MAX][PVSBYTES];
static byte		msgbuf[MSGMAX];
static sizebuf_t	msg;

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
static int irand (int n)	{ return n > 0 ? (int) (rnd () % (uint32_t) n) : 0; }

static float odd_float (void)
{
	static const float	v[] = { NAN, -NAN, INFINITY, -INFINITY, 1e9f, -1e9f, 3e38f,
		2147483648.0f, -2147483904.0f, 256.0f, 255.5f, -1.0f, -0.5f, 65535.0f, 1e-40f };
	return v[rnd () % (sizeof (v) / sizeof (v[0]))];
}

/* a byte-valued QuakeC float: usually an integer in range */
static float bytef (void)
{
	if (chance (3))
		return odd_float ();
	if (chance (10))
		return (float) (irand (600) - 100);
	return (float) irand (256);
}

/* ------------------------------------------------------------------------ */
/* the log, and the fatal calls                                             */
/* ------------------------------------------------------------------------ */

static char	logbuf[1 << 16];
static size_t	loglen;
static int	in_child = -1;

static void logv (const char *tag, const char *fmt, va_list ap)
{
	int	n;
	size_t	start = loglen, r, w;
	n = snprintf (logbuf + loglen, sizeof (logbuf) - loglen, "%s|", tag);
	if (n > 0)
		loglen += (size_t) n;
	if (loglen >= sizeof (logbuf))
		loglen = sizeof (logbuf) - 1;
	n = vsnprintf (logbuf + loglen, sizeof (logbuf) - loglen, fmt, ap);
	if (n > 0)
		loglen += (size_t) n;
	if (loglen >= sizeof (logbuf) - 1)
		loglen = sizeof (logbuf) - 2;
	logbuf[loglen++] = '\n';
	/* the reference's __func__ is its renamed name */
	for (r = w = start; r < loglen; )
	{
		if (loglen - r >= 5 && !memcmp (logbuf + r, "c_SV_", 5))
			r += 2;
		logbuf[w++] = logbuf[r++];
	}
	loglen = w;
}

static void logf_ (const char *tag, const char *fmt, ...)
{
	va_list	ap;
	va_start (ap, fmt);
	logv (tag, fmt, ap);
	va_end (ap);
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list	ap;
	char	tag[16];
	snprintf (tag, sizeof (tag), "con%u", flags);
	va_start (ap, fmt);
	logv (tag, fmt, ap);
	va_end (ap);
}

static void report_and_exit (void);

/* An unplanned fatal is a result, not a harness bug: a mutant whose size
 * bookkeeping is wrong drives the datagram over maxsize with allowoverflow
 * off, and only the side it is in dies.  The runner arms the jump around
 * each side's call, so the case is compared like any other, and only a
 * fatal in a side the generator did not predict reaching the process's exit
 * would still abort.  The jump leaves the port's frames without running
 * destructors, which cannot matter here: the port holds raw pointers and
 * arrays, and its crate is panic = abort. */
static jmp_buf	fatal_jmp;
static int	fatal_armed;
static int	fatal_hit;

static FUNC_NORETURN void fatal (const char *tag, const char *fmt, va_list ap)
{
	logv (tag, fmt, ap);
	if (in_child >= 0)
		report_and_exit ();
	if (fatal_armed)
	{
		fatal_hit = 1;
		longjmp (fatal_jmp, 1);
	}
	fprintf (stderr, "harness bug: %.*s", (int) loglen, logbuf);
	abort ();
}

FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal ("Host_Error", error, ap);
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal ("Sys_Error", error, ap);
}

void *Hunk_AllocName (int size, const char *name)
{
	(void) size; (void) name;
	fprintf (stderr, "harness bug: Hunk_AllocName\n");
	abort ();
}

void PR_ExecuteProgram (func_t fnum, const char *funcname)	{ (void) fnum; (void) funcname; abort (); }

/* the strings: 0 is "", then model names (class-dependent ones end in a
 * digit and ".mdl"), then class names */
#define NSTR	24
static const char *strs[NSTR] = {
	"", "models/paladin1.mdl", "models/crusader1.mdl", "models/x1.mdl",
	"models/assassin1.mdl", "progs/h_player.mdl", "maps/bsp1.bsp",
	"models/ab1.mdl", "*1", "*12", "models/spell.mdl", "abcde",
	"classname_monster", "classname_door", "player", "light",
	"models/succubus1.mdl", "models/necro1.mdl", "z1.mdl", "models/m9.mdl",
	"models/long_model_name_for_testing1.mdl", "q.spr", "models/a1.mdl", "monster_imp"
};

const char *PR_GetString (int num)
{
	if (num >= 0 && num < NSTR)
		return strs[num];
	return "unknown";
}

/* the alpha field: present on some edicts */
eval_t *GetEdictFieldValue (edict_t *ed, const char *field)
{
	int	n = ((byte *) ed - (byte *) sv.edicts) / pr_edict_size;
	if (strcmp (field, "alpha") != 0 || n < 0 || n >= NE_MAX || !has_alpha[n])
		return NULL;
	return (eval_t *) &alpha_val[n];
}

/* sv_main.c's SV_ModelIndex is outside this slice: log the name it is
 * asked for, and answer deterministically */
int SV_ModelIndex (const char *name)
{
	unsigned int	h = 5381;
	const char	*p;
	logf_ ("MI", "%s", name);
	for (p = name; *p; p++)
		h = h * 33 + (unsigned char) *p;
	return (int) (h % 700) - 100;
}

/* the PVS of a leaf */
byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model)
{
	(void) model;
	return leafpvs[leaf - leafs];
}

/* pr_edict.c's EDICT_NUM */
edict_t *EDICT_NUM (int n)
{
	if (n < 0 || n >= sv_max_edicts)
		Host_Error ("%s: bad number %i", __thisfunc__, n);
	return (edict_t *) ((byte *) sv.edicts + (n) * pr_edict_size);
}

/* sv_main_sound.rs, in the same archive, calls it; nothing here reaches it */
int q_strcasecmp (const char *s1, const char *s2)	{ (void) s1; (void) s2; abort (); }

int NUM_FOR_EDICT (edict_t *e)
{
	return ((byte *) e - (byte *) sv.edicts) / pr_edict_size;
}

static edict_t *edict (int n)	{ return (edict_t *) (edict_store + (size_t) n * pr_edict_size); }

/* ------------------------------------------------------------------------ */
/* layouts                                                                  */
/* ------------------------------------------------------------------------ */

extern size_t SvMainEnts_EntityState_sizeof (void), SvMainEnts_EntityState_offsetof_modelindex (void);
extern size_t SvMainEnts_EntityState_offsetof_alpha (void), SvMainEnts_EntityState_offsetof_clear_count (void);
extern size_t SvMainEnts_EntityState2_sizeof (void), SvMainEnts_EntityState2_offsetof_index (void);
extern size_t SvMainEnts_EntityState2_offsetof_origin (void), SvMainEnts_EntityState2_offsetof_modelindex (void);
extern size_t SvMainEnts_EntityState2_offsetof_alpha (void);
extern size_t SvMainEnts_ClientFrames_sizeof (void), SvMainEnts_ClientFrames_offsetof_count (void);
extern size_t SvMainEnts_ClientState2_sizeof (void);

static int check_layouts (void)
{
	int	bad = 0;
#define L(what, c, r) do { if ((size_t) (c) != (r)) { printf ("LAYOUT: %s: C %zu, Rust %zu\n", what, (size_t) (c), (size_t) (r)); bad++; } } while (0)
	L ("sizeof entity_state_t", sizeof (entity_state_t), SvMainEnts_EntityState_sizeof ());
	L ("entity_state_t.modelindex", offsetof (entity_state_t, modelindex), SvMainEnts_EntityState_offsetof_modelindex ());
	L ("entity_state_t.alpha", offsetof (entity_state_t, alpha), SvMainEnts_EntityState_offsetof_alpha ());
	L ("entity_state_t.ClearCount", offsetof (entity_state_t, ClearCount), SvMainEnts_EntityState_offsetof_clear_count ());
	L ("sizeof entity_state2_t", sizeof (entity_state2_t), SvMainEnts_EntityState2_sizeof ());
	L ("entity_state2_t.index", offsetof (entity_state2_t, index), SvMainEnts_EntityState2_offsetof_index ());
	L ("entity_state2_t.origin", offsetof (entity_state2_t, origin), SvMainEnts_EntityState2_offsetof_origin ());
	L ("entity_state2_t.modelindex", offsetof (entity_state2_t, modelindex), SvMainEnts_EntityState2_offsetof_modelindex ());
	L ("entity_state2_t.alpha", offsetof (entity_state2_t, alpha), SvMainEnts_EntityState2_offsetof_alpha ());
	L ("sizeof client_frames_t", sizeof (client_frames_t), SvMainEnts_ClientFrames_sizeof ());
	L ("client_frames_t.count", offsetof (client_frames_t, count), SvMainEnts_ClientFrames_offsetof_count ());
	L ("sizeof client_state2_t", sizeof (client_state2_t), SvMainEnts_ClientState2_sizeof ());
	L ("facts: sizeof entity_state2_t", SvMainEnts_TargetFacts.entity_state2_size, SvMainEnts_EntityState2_sizeof ());
	L ("facts: sizeof client_state2_t", SvMainEnts_TargetFacts.client_state2_size, SvMainEnts_ClientState2_sizeof ());
#undef L
	return bad;
}

/* ------------------------------------------------------------------------ */
/* the world                                                                */
/* ------------------------------------------------------------------------ */

static int	numleafs, numnodes;
static vec3_t	view_org;

static mnode_t *gen_node (int depth)
{
	mnode_t		*n;
	mplane_t	*p;
	int		k;

	if (depth == 0 || numnodes >= NNODE_MAX || chance (15))
		return (mnode_t *) &leafs[irand (numleafs + 1)];
	n = &nodes[numnodes];
	p = &planes[numnodes];
	numnodes++;
	memset (p, 0, sizeof (*p));
	if (chance (60))
	{
		k = irand (3);
		p->normal[k] = chance (50) ? 1 : -1;
		p->type = k;
	}
	else
	{
		float	l;
		for (k = 0; k < 3; k++)
			p->normal[k] = frand (-1, 1);
		l = sqrtf (DotProduct (p->normal, p->normal));
		if (l < 0.01f)
			p->normal[0] = l = 1;
		for (k = 0; k < 3; k++)
			p->normal[k] /= l;
		p->type = 3;
	}
	/* often within 8 of the viewer, so both sides are walked */
	p->dist = DotProduct (view_org, p->normal) + (chance (50) ? frand (-10, 10) : frand (-500, 500));
	if (chance (2))
		p->dist = DotProduct (view_org, p->normal) + (chance (50) ? 8.0f : -8.0f);
	n->contents = 0;
	n->plane = p;
	n->children[0] = gen_node (depth - 1);
	n->children[1] = gen_node (depth - 1);
	return n;
}

static void gen_world (void)
{
	int	i, j;

	numleafs = chance (20) ? 1 + irand (8) : 8 + irand (NLEAF_MAX - 9);
	numnodes = 0;
	memset (leafs, 0, sizeof (leafs));
	for (i = 0; i <= numleafs; i++)
	{
		leafs[i].contents = (i == 0 || chance (10)) ? CONTENTS_SOLID : (chance (80) ? CONTENTS_EMPTY : CONTENTS_WATER);
		for (j = 0; j < PVSBYTES; j++)
			leafpvs[i][j] = chance (30) ? 0xff : (byte) rnd ();
		if (chance (10))
			memset (leafpvs[i], 0, PVSBYTES);
	}
	memset (&world, 0, sizeof (world));
	world.numleafs = numleafs;
	world.leafs = leafs;
	world.nodes = gen_node (5);
	if ((mleaf_t *) world.nodes >= leafs && (mleaf_t *) world.nodes < leafs + NLEAF_MAX && chance (70))
	{
		/* a world of one leaf is legal, but keep most of them trees */
		numnodes = 0;
		world.nodes = gen_node (5);
	}
	world.numnodes = numnodes;
	sv.worldmodel = &world;
}

/* ------------------------------------------------------------------------ */
/* a case                                                                   */
/* ------------------------------------------------------------------------ */

enum { HUGE_NONE, HUGE_WRITE, HUGE_ZERO, HUGE_MEASURE, HUGE_SHED };

typedef struct
{
	int	huge;
	int	ne, k, clent, fatal;
	int	nonfinite;	/* an edict's origin or box is not finite */
	/* per case, so most cases stay in-process: odd boxes, odd flags, flags
	 * past bit 23 */
	int	want_oddbox, want_oddflags, want_badflags;
	int	cleanup;
	int	fill[MAX_FRAMES + 2];	/* entries filled per frame */
	int	hreg[MAX_FRAMES + 2];	/* entries compared and restored */
} scenario_t;

/* the pristine copies the two runs start from */
static client_state2_t	*pristine_states;	/* the client's, one */
static byte		pristine_client[4];
static byte		pristine_cc[NE_MAX];
static float		pristine_effects[NE_MAX];
static byte		pristine_msg[MSGMAX];
static int		pristine_cursize, pristine_maxsize, pristine_allowoverflow;
static int		prev_h[MAXC];	/* each client's region in its last case */

static void edict_state (edict_t *ent, entity_state2_t *s, int e)
{
	/* the state an edict would be recorded with: a zero delta against it */
	s->index = e;
	VectorCopy (ent->v.origin, s->origin);
	VectorCopy (ent->v.angles, s->angles);
	s->modelindex = (short) (int) ent->v.modelindex;
	s->frame = (byte) (int) ent->v.frame;
	s->colormap = (byte) (int) ent->v.colormap;
	s->skin = (byte) (int) ent->v.skin;
	s->drawflags = (byte) (int) ent->v.drawflags;
	s->effects = (byte) (int) ent->v.effects;
	s->scale = (int) (ent->v.scale * 100.0) & 255;
	s->abslight = (int) (ent->v.abslight * 255.0) & 255;
	s->alpha = 0;
}

static void gen_edict (scenario_t *sc, int e)
{
	edict_t	*ent = edict (e);
	int	k;
	float	*f;

	memset (ent, 0, pr_edict_size);
	if (sc->huge)
	{
		/* visible, modelled, misc, unchanging */
		ent->v.modelindex = 1 + irand (200);
		ent->v.model = 5;
		ent->num_leafs = MAX_ENT_LEAFS;
		for (k = 0; k < MAX_ENT_LEAFS; k++)
			ent->leafnums[k] = irand (numleafs);
		VectorSet (ent->v.origin, (float) irand (2000), (float) irand (2000), (float) irand (500));
		ent->v.frame = (float) irand (10);
		ent->v.scale = 1;
		VectorCopy (ent->v.origin, ent->baseline.origin);
		ent->baseline.modelindex = (short) (int) ent->v.modelindex;
		ent->baseline.frame = (byte) (int) ent->v.frame;
		ent->baseline.scale = 100;
		for (k = 0; k < 32; k++)
			ent->baseline.ClearCount[k] = 3;
		if (sc->huge == HUGE_MEASURE || sc->huge == HUGE_SHED)
			ent->v.origin[0] += 64;	/* a change on every one */
		has_alpha[e] = 0;
		return;
	}

	/* effects */
	switch (irand (8))
	{
	case 0: ent->v.effects = EF_NODRAW; break;
	case 1: ent->v.effects = EF_MUZZLEFLASH; break;
	case 2: ent->v.effects = EF_MUZZLEFLASH | 8; break;
	case 3: ent->v.effects = chance (10) ? odd_float () : (float) irand (256); break;
	default: ent->v.effects = 0; break;
	}
	ent->v.modelindex = chance (88) ? (float) (1 + irand (300)) : (chance (90) ? 0 : odd_float ());
	ent->v.model = chance (90) ? 1 + irand (NSTR - 1) : 0;
	/* flags */
	{
		int	fl = 0;
		if (chance (15)) fl |= FL_CLIENT;
		if (chance (25)) fl |= FL_MONSTER;
		if (chance (15)) fl |= FL_CLASS_DEPENDENT;
		if (chance (10)) fl |= 1 << irand (21);
		ent->v.flags = (float) fl;
		if (sc->want_oddflags && chance (2))
		{
			/* -1, 1e9 and 2^31 set bits past 23 too: fatal */
			ent->v.flags = odd_float ();
			sc->fatal = 1;
		}
		if (sc->want_badflags && chance (2))
		{
			ent->v.flags = (float) (fl | (1 << (24 + irand (7))));
			sc->fatal = 1;
		}
		/* the class-dependent rename needs a name the C handles defined:
		 * five characters at least */
		if (((int) ent->v.flags & FL_CLASS_DEPENDENT) && strlen (PR_GetString (ent->v.model)) < 5)
			ent->v.model = 1;
	}
	{
		static const float	mt[] = { MOVETYPE_NONE, MOVETYPE_STEP, MOVETYPE_FLYMISSILE,
			MOVETYPE_BOUNCEMISSILE, MOVETYPE_BOUNCE, MOVETYPE_WALK, MOVETYPE_PUSH, MOVETYPE_FLY, 4.5f };
		ent->v.movetype = mt[irand (sizeof (mt) / sizeof (mt[0]))];
	}
	ent->v.classname = 12 + irand (4);
	for (k = 0; k < 3; k++)
	{
		ent->v.origin[k] = (sc->want_oddbox && chance (3)) ? odd_float () : frand (-2000, 2000);
		ent->v.angles[k] = chance (3) ? odd_float () : (chance (40) ? 0 : frand (-360, 360));
		ent->v.absmin[k] = ent->v.origin[k] - frand (0, 64);
		ent->v.absmax[k] = ent->v.origin[k] + frand (0, 64);
		if (sc->want_oddbox && chance (2))
			ent->v.absmax[k] = odd_float ();
		if (!isfinite (ent->v.absmin[k]) || !isfinite (ent->v.absmax[k]))
			sc->nonfinite = 1;
	}
	ent->v.colormap = bytef ();
	ent->v.skin = bytef ();
	ent->v.drawflags = bytef ();
	ent->v.frame = bytef ();
	ent->v.scale = chance (60) ? 1.0f : (chance (90) ? frand (0, 3) : odd_float ());
	ent->v.abslight = chance (60) ? 0.0f : (chance (90) ? frand (0, 1.2f) : odd_float ());
	has_alpha[e] = chance (60);
	{
		static const float	al[] = { 0, 0.5f, 1, 2, -1, 0.001f, 0.999f, 1.0039f };
		alpha_val[e] = chance (5) ? odd_float () : (chance (50) ? al[irand (8)] : frand (0, 1));
	}

	/* leafs: none, a few, or a full (overflowed) list */
	switch (irand (6))
	{
	case 0: ent->num_leafs = 0; break;
	case 1: ent->num_leafs = MAX_ENT_LEAFS; break;
	default: ent->num_leafs = 1 + irand (MAX_ENT_LEAFS - 1); break;
	}
	for (k = 0; k < ent->num_leafs; k++)
		ent->leafnums[k] = irand (numleafs);

	/* baseline: the current state, or not */
	if (chance (40))
	{
		entity_state2_t	s;
		edict_state (ent, &s, e);
		VectorCopy (s.origin, ent->baseline.origin);
		VectorCopy (s.angles, ent->baseline.angles);
		ent->baseline.modelindex = s.modelindex;
		ent->baseline.frame = s.frame;
		ent->baseline.colormap = s.colormap;
		ent->baseline.skin = s.skin;
		ent->baseline.effects = s.effects;
		ent->baseline.scale = s.scale;
		ent->baseline.drawflags = s.drawflags;
		ent->baseline.abslight = s.abslight;
	}
	else
	{
		for (k = 0; k < 3; k++)
		{
			ent->baseline.origin[k] = frand (-2000, 2000);
			ent->baseline.angles[k] = frand (-360, 360);
		}
		ent->baseline.modelindex = (short) irand (300);
		ent->baseline.frame = (byte) rnd ();
		ent->baseline.colormap = (byte) rnd ();
		ent->baseline.skin = (byte) rnd ();
		ent->baseline.effects = (byte) rnd ();
		ent->baseline.scale = (byte) rnd ();
		ent->baseline.drawflags = (byte) rnd ();
		ent->baseline.abslight = (byte) rnd ();
		ent->baseline.alpha = (byte) rnd ();
	}
	for (k = 0; k < 32; k++)
	{
		static const byte	cc[] = { 0, 1, 2, 3, 3, 3, 200 };
		ent->baseline.ClearCount[k] = cc[irand (7)];
	}
	(void) f;
}

static void gen_frame (scenario_t *sc, client_frames_t *fr, int fi)
{
	int	n = 0, e, step;
	int	full = (sc->huge == HUGE_ZERO || sc->huge == HUGE_SHED) && fi == 3;

	if (full)
	{
		/* the reference: 8192 entries, every one of them an edict */
		for (e = 1; e < sc->ne && n < MAX_CLIENT_STATES; e++)
		{
			entity_state2_t	*s = &fr->states[n++];
			memset (s, 0, sizeof (*s));
			edict_state (edict (e), s, e);
			s->scale = 100;
			if (sc->huge == HUGE_SHED)
				s->origin[0] -= 64;	/* the edicts moved */
		}
		fr->count = n;
		sc->fill[fi] = n;
		return;
	}
	if (sc->huge || chance (8))
	{
		fr->count = 0;
		sc->fill[fi] = 0;
		return;
	}
	/* sorted, a random subset; dense or sparse */
	step = chance (50) ? 1 : 1 + irand (4);
	for (e = 1; e < sc->ne && n < MAX_CLIENT_STATES; e += 1 + irand (step))
	{
		entity_state2_t	*s;
		if (!chance (70))
			continue;
		s = &fr->states[n++];
		memset (s, 0, sizeof (*s));
		if (chance (60))
			edict_state (edict (e), s, e);
		else
		{
			int	k;
			s->index = e;
			for (k = 0; k < 3; k++)
			{
				s->origin[k] = chance (50) ? edict (e)->v.origin[k] + frand (-0.2f, 0.2f) : frand (-2000, 2000);
				s->angles[k] = chance (50) ? edict (e)->v.angles[k] : frand (-360, 360);
			}
			s->modelindex = (short) irand (300);
			s->frame = (byte) rnd ();
			s->colormap = (byte) rnd ();
			s->skin = (byte) rnd ();
			s->effects = (byte) rnd ();
			s->scale = (byte) rnd ();
			s->drawflags = (byte) rnd ();
			s->abslight = (byte) rnd ();
			s->alpha = (byte) rnd ();
		}
		s->flags = (byte) (chance (40) ? ENT_CLEARED : 0) | (chance (50) ? ENT_STATE_ON : 0);
	}
	fr->count = n;
	sc->fill[fi] = n;
}

/* ------------------------------------------------------------------------
 * Directed cases
 *
 * The random generator does not reach every input the port distinguishes.
 * Each case here is one that a mutant of sv_main_ents.rs survived every
 * random case without, so it needs the exact value where the C and the
 * mutation part company; what each side does with it is at the case.  The
 * scene is a small, fully determined one -- the client's edict at the
 * viewer, a handful of edicts, an empty reference so every update is built
 * from its own baseline -- laid over the case the generator built, so the
 * world and the fat PVS stay varied.
 */

enum {
	DIR_ORIGIN_POS,		/* a delta of exactly +0.1f */
	DIR_ORIGIN_NEG,		/* ... and -0.1f */
	DIR_ORIGIN_ULP,		/* the float either side of 0.1f, which both send */
	DIR_CLASS_ROUND,	/* the class-dependent model name's digit */
	DIR_ALPHA_ROUND,	/* ENTALPHA_ENCODE's Q_rint */
	DIR_PRIO_SQRT,		/* a distance whose float and double chains bin
				 * one apart */
	DIR_PRIO_254,		/* a distance in bin 254, where `>= 255' and
				 * `>= 254' part company */
	DIR_FRAME_TAIL,		/* a build frame past 700 entries, every one of
				 * them with a changed frame byte */
	DIR_IGNORE_TAIL,	/* an ignored edict on the build frame's last
				 * entry, the one count the loop breaks at */
	DIR_NAN_PRIO,		/* a NaN box, where the C dies and the port
				 * sheds the entity as the last bin */
	NDIRECTED
};

#define DIR_NE		7	/* 0 is the world, 1 the client's, 6 the case's */
#define DIR_TAIL_NE	900	/* the frame-tail case: past the 700 the write
				 * path's tail is at, and inside one datagram */
#define DIR_HUGE_NE	8300	/* the ignore case: past MAX_CLIENT_STATES */
#define DIR_CLIENT	1
#define DIR_TEST	6	/* last, so the small updates are written first */
#define DIR_MSGMAX	16	/* room for 1..5, not for 6: 3 + 5*2 + its own
				 * 2 + the 2-byte trailer overflows it */
#define DIR_ROOM	65000	/* the cases that only need the datagram */

/* the baseline an update is a zero delta against: the edict as it stands,
 * as edict_state () does it for a frame entry */
static void directed_zero_delta (edict_t *ent)
{
	int	k;

	for (k = 0; k < 3; k++)
	{
		ent->baseline.origin[k] = ent->v.origin[k];
		ent->baseline.angles[k] = ent->v.angles[k];
	}
	ent->baseline.modelindex = (short) (int) ent->v.modelindex;
	ent->baseline.frame = (byte) (int) ent->v.frame;
	ent->baseline.colormap = (byte) (int) ent->v.colormap;
	ent->baseline.skin = (byte) (int) ent->v.skin;
	ent->baseline.drawflags = (byte) (int) ent->v.drawflags;
	ent->baseline.effects = (byte) (int) ent->v.effects;
	ent->baseline.scale = (int) (ent->v.scale * 100.0) & 255;
	ent->baseline.abslight = (int) (ent->v.abslight * 255.0) & 255;
	ent->baseline.alpha = 0;
}

/* before the world, the edicts and the frames are built from sc */
static void directed_pre (scenario_t *sc, int d)
{
	sc->k = 0;
	sc->cleanup = 0;
	sc->want_oddbox = sc->want_oddflags = sc->want_badflags = 0;
	if (d == DIR_IGNORE_TAIL)
	{
		/* the build frame full, and an ignored edict on its last entry:
		 * the huge zero-delta scene, which fills a full reference and
		 * writes nothing, is the one that reaches bcount 8192 */
		sc->huge = HUGE_ZERO;
		sc->ne = DIR_HUGE_NE;
		sc->clent = DIR_CLIENT;
		return;
	}
	sc->huge = 0;
	sc->ne = (d == DIR_FRAME_TAIL) ? DIR_TAIL_NE : DIR_NE;
	sc->clent = DIR_CLIENT;
}

/* over the case the generator built, once its frames and datagram exist */
static void directed_scenario (scenario_t *sc, int d)
{
	client_t	*cl = &svs.clients[sc->k];
	client_state2_t	*st = &sv.states[sc->k];
	edict_t		*ce = edict (DIR_CLIENT);
	edict_t		*ent;
	float		delta = 0.0f;
	int		e, fi, k;

	if (d == DIR_IGNORE_TAIL)
	{
		/* Every edict is in the reference and ignored for its update
		 * type, so the call reaches the ignore path at bcount exactly
		 * MAX_CLIENT_STATES -- the 8192nd edict.  The C goes on to the
		 * edicts past the reference, which are monsters and so are
		 * written; a break at the ignored one stops an edict early, and
		 * the build frame's count and the datagram both show it. */
		sv_update_player.integer = 1;
		sv_update_monsters.integer = 1;
		sv_update_missiles.integer = 1;
		sv_update_misc.integer = 0;	/* no misc: every edict is ignored */
		sv_netsort.integer = 0;
		for (e = 1; e < sc->ne; e++)
			edict (e)->v.flags = (float) ((e == MAX_CLIENT_STATES) ? 0 : FL_MONSTER);
		msg.maxsize = DIR_ROOM;
		msg.cursize = 0;
		msg.allowoverflow = 1;
		return;
	}

	/* the client's edict sits at the viewer with no view offset, so the
	 * org the priority cases measure from is view_org exactly.  clent is
	 * the generator's by now, so pin it here: an entity that is the
	 * client's own has priority 0 by definition, and the case's edict
	 * must not be it. */
	sc->clent = DIR_CLIENT;
	cl->edict = ce;
	/* The viewer sits at the origin, and the client's edict with it, with no
	 * view offset.  SV_EntityPriority measures from `the box - org', and
	 * only a viewer at 0 makes that subtraction exact, so the distance a
	 * case chooses is the distance the port computes; placed at a random
	 * view_org the sum rounds at that magnitude and moves the bin. */
	VectorClear (view_org);
	ce->v.cameramode = 0;
	VectorSet (ce->v.view_ofs, 0, 0, 0);
	VectorCopy (view_org, ce->v.origin);

	/* An empty reference, so every update is built from its own baseline.
	 * The region stays the case's own: gen_case sized it for what the call
	 * can write, the build frame taking one entry per edict. */
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
	{
		memset (st->frames[fi].states, 0, (size_t) sc->hreg[fi] * sizeof (entity_state2_t));
		st->frames[fi].count = 0;
		sc->fill[fi] = 0;
	}
	prev_h[sc->k] = sc->hreg[0];

	/* every edict: visible, always sent, a unit box on the viewer and a
	 * zero delta against it.  A unit box the org is inside of makes its
	 * priority 0, so only the case's own edict can be shed. */
	for (e = 0; e < sc->ne; e++)
	{
		ent = edict (e);
		memset (ent, 0, pr_edict_size);
		ent->num_leafs = MAX_ENT_LEAFS;	/* always sent */
		for (k = 0; k < MAX_ENT_LEAFS; k++)
			ent->leafnums[k] = 0;
		ent->v.model = 1;		/* "models/paladin1.mdl" */
		ent->v.modelindex = 5.0f;
		ent->v.movetype = MOVETYPE_WALK;
		VectorCopy (view_org, ent->v.origin);
		VectorSet (ent->v.absmin, view_org[0] - 1.0f, view_org[1] - 1.0f, view_org[2] - 1.0f);
		VectorSet (ent->v.absmax, view_org[0] + 1.0f, view_org[1] + 1.0f, view_org[2] + 1.0f);
		directed_zero_delta (ent);
		for (k = 0; k < 32; k++)
			ent->baseline.ClearCount[k] = 3;	/* never a removal */
	}

	/* every update type on, sv_netsort on, and a datagram with room for
	 * the small updates and not for the case's own */
	sv_update_player.integer = sv_update_monsters.integer = 1;
	sv_update_missiles.integer = sv_update_misc.integer = 1;
	sv_netsort.integer = 1;
	sv_protocol = 21;
	msg.data = msgbuf;
	msg.maxsize = DIR_ROOM;
	msg.cursize = 0;
	msg.allowoverflow = 1;	/* what overflows is not what is under test */
	msg.overflowed = 0;
	msg.name = "datagram";

	ent = edict (DIR_TEST);
	switch (d)
	{
	case DIR_ORIGIN_POS:
	case DIR_ORIGIN_NEG:
		/* The origin is sent when the delta is outside +/-0.1 as a
		 * double.  0.1f is 0.100000001490116 as a double, so it is
		 * sent; compared as a float against 0.1f it is not (0.1f >
		 * 0.1f is false), and the update loses the origin. */
		ent->v.origin[0] = (d == DIR_ORIGIN_POS) ? 0.1f : -0.1f;
		ent->baseline.origin[0] = 0.0f;
		break;
	case DIR_ORIGIN_ULP:
		/* the float either side of 0.1f: neither crosses, in either
		 * compare, so these pin the boundary the cases above sit on */
		ent->v.origin[0] = -nextafterf (0.1f, 0.0f);
		ent->baseline.origin[0] = 0.0f;
		{
			edict_t	*o = edict (2);
			o->v.origin[0] = nextafterf (0.1f, 1.0f);
			o->baseline.origin[0] = 0.0f;
		}
		break;
	case DIR_CLASS_ROUND:
		/* FL_CLASS_DEPENDENT rewrites the name's fifth character from
		 * the end with `(int)(playerclass + 48.0)'.  At 0.99999994f
		 * that is 49, while truncating the class and then adding 48
		 * gives 48: the name, its SV_ModelIndex and the update's model
		 * index all differ. */
		ent->v.flags = FL_CLASS_DEPENDENT;
		cl->playerclass = 0.99999994f;
		break;
	case DIR_ALPHA_ROUND:
		/* ENTALPHA_ENCODE: Q_rint of a*254+1, computed in float.  At
		 * 0.00196850393f that is 2; computed in double it is 1. */
		sv_protocol = 100;	/* PROTOCOL_HEXENWAIL_1: U_ALPHA exists */
		has_alpha[DIR_TEST] = 1;
		alpha_val[DIR_TEST] = 0.00196850393f;
		ent->baseline.alpha = 0;
		break;
	case DIR_PRIO_SQRT:
		/* 8*sqrt(sqrt(dist/size)) is 19.999999 in double at this
		 * distance, and 20 in float: the bin, which the shed message
		 * names, is 19 and 20. */
		delta = 6.24999952f;
		break;
	case DIR_PRIO_254:
		/* ... and 254.243977 here: bin 254, where the clamp
		 * `dist >= NETSORT_BINS - 1' (255) parts company with the
		 * same test against 254, 254 against 255. */
		delta = 1010.0f;
		break;
	case DIR_FRAME_TAIL:
		/* A build frame past 700 entries -- the write path's tail -- with
		 * every entity's frame byte different from its baseline, so the
		 * frame write happens at every one of them and the whole tail is
		 * compared. */
		for (e = 1; e < sc->ne; e++)
		{
			edict_t	*x = edict (e);
			x->v.frame = 7.0f;
			x->baseline.frame = 3;
		}
		break;
	case DIR_NAN_PRIO:
		/* A box of three NaNs -- QuakeC's setorigin on 0/0, or an absmax -
		 * absmin of two infinities -- leaves the priority NaN.  The C's
		 * clamp is false for a NaN, so it returns (int) of one, INT_MIN, and
		 * the measuring pass writes binbytes[INT_MIN], far below the array,
		 * and the server dies.  The port takes it as the least important bin
		 * and sheds the entity, which the shed message names.  This case is
		 * forked, and the runner expects the two sides to disagree here: the
		 * one place in this file where they are meant to. */
		msg.maxsize = DIR_MSGMAX;
		VectorSet (ent->v.absmin, NAN, NAN, NAN);
		VectorSet (ent->v.absmax, NAN, NAN, NAN);
		directed_zero_delta (ent);
		sc->nonfinite = 1;	/* the C dies: run both sides forked */
		return;
	}

	if (delta != 0.0f)
	{
		/* a zero-size box the viewer is outside of: the distance is
		 * the whole delta, and the box's size is 0, clamped to 1 */
		msg.maxsize = DIR_MSGMAX;
		VectorSet (ent->v.absmin, view_org[0] + delta, view_org[1], view_org[2]);
		VectorSet (ent->v.absmax, view_org[0] + delta, view_org[1], view_org[2]);
		VectorSet (ent->v.origin, view_org[0] + delta, view_org[1], view_org[2]);
		directed_zero_delta (ent);
	}
}

static void gen_case (scenario_t *sc, int directed)
{
	int		i, e, fi;
	client_t	*cl;
	client_state2_t	*st;

	memset (sc, 0, sizeof (*sc));
	if (chance (1))
		sc->huge = 1 + irand (4);
	/* 400..899: past 700 entries the write path's tail is a different one
	 * (the frame write at bcount > 700 is the mutation's own window), and
	 * the huge scenarios alone do not reach it with a frame in play */
	sc->ne = sc->huge ? 8300 : (chance (40) ? 2 + irand (80) : (chance (50) ? 200 + irand (200) : 400 + irand (NE_NORMAL + 200 - 400)));
	sc->k = irand (MAXC);
	sc->cleanup = chance (33);
	sc->want_oddbox = !sc->huge && chance (6);
	sc->want_oddflags = !sc->huge && chance (3);
	sc->want_badflags = !sc->huge && chance (3);
	/* a directed case pins its own scene before anything is built from it */
	if (directed >= 0)
		directed_pre (sc, directed);
	sv.num_edicts = sv_max_edicts = sc->ne;

	/* the viewer first: the planes are placed around it */
	for (i = 0; i < 3; i++)
		view_org[i] = frand (-1000, 1000);
	gen_world ();

	for (e = 0; e < sc->ne; e++)
		gen_edict (sc, e);
	/* The two edicts past num_edicts.  SV_CleanupEnts clears the effects of
	 * edicts 1..num_edicts-1, so a call that runs one edict too many -- from
	 * 0 instead of 1 -- touches the first of these; main compares effects
	 * this far.  The first gets the muzzle flash the call clears. */
	for (e = sc->ne; e < sc->ne + 2; e++)
	{
		edict_t	*x = edict (e);
		memset (x, 0, pr_edict_size);
		x->v.effects = (e == sc->ne) ? EF_MUZZLEFLASH : 0.0f;
	}
	/* the client's edict, and perhaps a camera */
	sc->clent = (sc->ne > sc->k + 1 && chance (85)) ? sc->k + 1 : irand (sc->ne);
	{
		edict_t	*ce = edict (sc->clent);
		VectorCopy (view_org, ce->v.origin);
		VectorSet (ce->v.view_ofs, 0, 0, chance (80) ? 22 : frand (-30, 30));
		ce->v.origin[2] -= ce->v.view_ofs[2];
		ce->v.cameramode = 0;
		if (chance (15) && sc->ne > 1)
		{
			int	cam = 1 + irand (sc->ne - 1);
			ce->v.cameramode = cam * pr_edict_size;
			VectorCopy (view_org, edict (cam)->v.origin);
		}
	}

	/* the client */
	cl = &svs.clients[sc->k];
	cl->edict = edict (sc->clent);
	cl->playerclass = chance (95) ? (float) (1 + irand (5)) : (chance (50) ? 0 : 7.5f);
	cl->current_frame = (byte) irand (MAX_FRAMES + 2);
	switch (irand (8))
	{
	case 0: cl->last_frame = 255; break;
	case 1: cl->last_frame = 254; break;
	case 2: cl->last_frame = 0; break;
	case 3: cl->last_frame = MAX_FRAMES + 1; break;
	case 4: cl->last_frame = (byte) (cl->current_frame + 1 + irand (2)); if (cl->last_frame > MAX_FRAMES + 1) cl->last_frame = MAX_FRAMES; break;
	default: cl->last_frame = cl->current_frame ? (byte) (1 + irand (cl->current_frame)) : 1; break;
	}
	cl->current_sequence = (byte) rnd ();
	cl->last_sequence = chance (75) ? cl->current_sequence : (byte) rnd ();
	if (sc->huge == HUGE_ZERO || sc->huge == HUGE_SHED)
	{
		/* a valid acknowledgement of frame 3 */
		cl->current_frame = 4;
		cl->last_frame = 3;
		cl->last_sequence = cl->current_sequence;
	}
	else if (sc->huge)
	{
		/* an expired reference: the empty frame 0 is the reference */
		cl->last_frame = 255;
		cl->last_sequence = cl->current_sequence;
	}

	/* the client's frames; the others are untouched by the call.  Clear
	 * what the client's last case left first: everything past this case's
	 * region must be zero, as restore () relies on */
	st = &sv.states[sc->k];
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
	{
		memset (st->frames[fi].states, 0, (size_t) prev_h[sc->k] * sizeof (entity_state2_t));
		st->frames[fi].count = 0;
	}
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
	{
		gen_frame (sc, &st->frames[fi], fi);
		sc->hreg[fi] = sc->fill[fi] + 2;
		/* The build frame takes one entry per edict -- whatever the
		 * reference held, and whether or not the reference had the
		 * edict at all -- so its region is the whole of what the call
		 * can write, not the reference's own fill.  A mutation in that
		 * tail (the frame byte past 700 entries) is otherwise outside
		 * every region compared, and invisible. */
		if (sc->hreg[fi] < sc->ne + 2)
			sc->hreg[fi] = sc->ne + 2;
		/* and no further: the guards stop the call at MAX_CLIENT_STATES
		 * entries, and the frames hold exactly that many */
		if (sc->hreg[fi] > MAX_CLIENT_STATES)
			sc->hreg[fi] = MAX_CLIENT_STATES;
	}
	/* the build frame is memset whole and the reference copied whole, so
	 * every frame's region has to cover the largest of them */
	{
		int	m = 0;
		for (fi = 0; fi < MAX_FRAMES + 2; fi++)
			if (sc->hreg[fi] > m)
				m = sc->hreg[fi];
		for (fi = 0; fi < MAX_FRAMES + 2; fi++)
			sc->hreg[fi] = m;
		prev_h[sc->k] = m;
	}

	/* the cvars */
	{
		static const int	up[] = { 1, 1, 1, 1, 1, 1, 2, 3, 0, -2 };
		sv_update_player.integer = up[irand (10)];
		sv_update_monsters.integer = up[irand (10)];
		sv_update_missiles.integer = up[irand (10)];
		sv_update_misc.integer = up[irand (10)];
		sv_netsort.integer = chance (70);
		sv_protocol = (int[]) { 18, 19, 21, 100, 100 }[irand (5)];
	}
	if (sc->huge)
	{
		sv_update_player.integer = sv_update_monsters.integer = 1;
		sv_update_missiles.integer = sv_update_misc.integer = 1;
		sv_netsort.integer = (sc->huge == HUGE_MEASURE);
		sv_protocol = 21;
	}

	/* the datagram */
	{
		static const int	ms[] = { 4096, 4096, 1400, 1024, 600, 300, 2048, 65000 };
		int	ms_ = ms[irand (8)];
		if (sc->huge == HUGE_WRITE || sc->huge == HUGE_ZERO)
			ms_ = 65000;
		if (sc->huge == HUGE_MEASURE || sc->huge == HUGE_SHED)
			ms_ = 4096;
		msg.data = msgbuf;
		msg.maxsize = ms_;
		msg.cursize = irand (24);
		if (chance (3))
			msg.cursize = ms_ - 3 - irand (6);
		msg.allowoverflow = chance (8);
		msg.overflowed = 0;
		msg.name = "datagram";
		if (!msg.allowoverflow && msg.maxsize - msg.cursize < 8)
			sc->fatal = 1;
	}
	for (i = 0; i < MSGMAX; i++)
		msgbuf[i] = (byte) (i * 7 + 3);
	/* the directed case's own scene, over what the generator built */
	if (directed >= 0)
		directed_scenario (sc, directed);

	/* A NaN box makes SV_EntityPriority's key NaN.  The C's clamp is false
	 * for it, so (int) of it is INT_MIN and the measuring pass writes
	 * binbytes[INT_MIN], far outside the array, and the reference dies.  The
	 * port takes a non-finite key as the least important bin instead and
	 * sheds the entity: the case is forked, and the runner expects exactly
	 * that disagreement.  Where the frame cannot overflow the key is never
	 * computed and both sides still agree. */
	if (sc->nonfinite && sv_netsort.integer)
		sc->fatal = 1;

	/* the snapshot: the region, and every frame's count */
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
	{
		memcpy (pristine_states->frames[fi].states, st->frames[fi].states,
			(size_t) sc->hreg[fi] * sizeof (entity_state2_t));
		pristine_states->frames[fi].count = st->frames[fi].count;
	}
	pristine_client[0] = cl->current_frame;
	pristine_client[1] = cl->last_frame;
	pristine_client[2] = cl->current_sequence;
	pristine_client[3] = cl->last_sequence;
	for (e = 0; e < sc->ne + 2; e++)
	{
		pristine_cc[e] = edict (e)->baseline.ClearCount[sc->k];
		pristine_effects[e] = edict (e)->v.effects;
	}
	memcpy (pristine_msg, msgbuf, MSGMAX);
	pristine_cursize = msg.cursize;
	pristine_maxsize = msg.maxsize;
	pristine_allowoverflow = msg.allowoverflow;
}

static void restore (const scenario_t *sc)
{
	client_state2_t	*st = &sv.states[sc->k];
	client_t	*cl = &svs.clients[sc->k];
	int		fi, e;

	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
	{
		memcpy (st->frames[fi].states, pristine_states->frames[fi].states,
			(size_t) sc->hreg[fi] * sizeof (entity_state2_t));
		st->frames[fi].count = pristine_states->frames[fi].count;
	}
	cl->current_frame = pristine_client[0];
	cl->last_frame = pristine_client[1];
	cl->current_sequence = pristine_client[2];
	cl->last_sequence = pristine_client[3];
	for (e = 0; e < sc->ne + 2; e++)
	{
		edict (e)->baseline.ClearCount[sc->k] = pristine_cc[e];
		edict (e)->v.effects = pristine_effects[e];
	}
	memcpy (msgbuf, pristine_msg, MSGMAX);
	msg.cursize = pristine_cursize;
	msg.maxsize = pristine_maxsize;
	msg.allowoverflow = pristine_allowoverflow;
	msg.overflowed = 0;
	memset (c_fatpvs, 0xa5, sizeof (c_fatpvs));
	memset (SvMain_FatPvs (), 0xa5, SvMain_FatPvsSize ());
	loglen = 0;
}

static void run_side (int rust, const scenario_t *sc)
{
	client_t	*cl = &svs.clients[sc->k];
	if (rust)
	{
		SV_PrepareClientEntities (cl, edict (sc->clent), &msg);
		if (sc->cleanup)
			SV_CleanupEnts ();
	}
	else
	{
		c_SV_PrepareClientEntities (cl, edict (sc->clent), &msg);
		if (sc->cleanup)
			c_SV_CleanupEnts ();
	}
}

/* ------------------------------------------------------------------------ */
/* the outputs                                                              */
/* ------------------------------------------------------------------------ */

/* an entity_state2_t's fields, without its padding */
static void canon (const entity_state2_t *s, byte out[37])
{
	out[0] = s->flags;
	memcpy (out + 1, &s->index, 2);
	memcpy (out + 3, s->origin, 12);
	memcpy (out + 15, s->angles, 12);
	memcpy (out + 27, &s->modelindex, 2);
	out[29] = s->frame;
	out[30] = s->colormap;
	out[31] = s->skin;
	out[32] = s->effects;
	out[33] = s->scale;
	out[34] = s->drawflags;
	out[35] = s->abslight;
	out[36] = s->alpha;
}

typedef struct
{
	int	cursize, overflowed;
	byte	client[4];
	int	counts[MAX_FRAMES + 2];
	int	fatbytes;
} outcome_t;

static uint64_t fnv (uint64_t h, const void *p, size_t n)
{
	const byte	*b = p;
	size_t		i;
	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ULL;
	return h;
}

/* the frames' entries, as canonical bytes */
static byte	*frames_c;	/* the C side's, kept for the compare */

static size_t frames_bytes (const scenario_t *sc, byte *out)
{
	const client_state2_t	*st = &sv.states[sc->k];
	size_t			o = 0;
	int			fi, i;
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
		for (i = 0; i < sc->hreg[fi]; i++, o += 37)
			canon (&st->frames[fi].states[i], out + o);
	return o;
}

static void capture (const scenario_t *sc, outcome_t *oc, int rust)
{
	client_t	*cl = &svs.clients[sc->k];
	int		fi;
	oc->cursize = msg.cursize;
	oc->overflowed = msg.overflowed;
	oc->client[0] = cl->current_frame;
	oc->client[1] = cl->last_frame;
	oc->client[2] = cl->current_sequence;
	oc->client[3] = cl->last_sequence;
	for (fi = 0; fi < MAX_FRAMES + 2; fi++)
		oc->counts[fi] = sv.states[sc->k].frames[fi].count;
	(void) rust;
}

static uint64_t digest (const scenario_t *sc, int rust)
{
	outcome_t	oc;
	uint64_t	h = 0xcbf29ce484222325ULL;
	static byte	*tmp;
	size_t		n;
	int		e;

	if (!tmp)
		tmp = malloc ((size_t) (MAX_FRAMES + 2) * MAX_CLIENT_STATES * 37);
	memset (&oc, 0, sizeof (oc));
	capture (sc, &oc, rust);
	h = fnv (h, &oc, sizeof (oc));
	h = fnv (h, msgbuf, (size_t) msg.maxsize);
	n = frames_bytes (sc, tmp);
	h = fnv (h, tmp, n);
	for (e = 0; e < sc->ne + 2; e++)
	{
		h = fnv (h, &edict (e)->baseline.ClearCount[sc->k], 1);
		h = fnv (h, &edict (e)->v.effects, 4);
	}
	return h;
}

static char	child_report[sizeof (logbuf) + 128];
static const scenario_t	*child_sc;
static int	child_rust;

static void report_and_exit (void)
{
	int	n = snprintf (child_report, sizeof (child_report), "%016llx|%.*s",
			(unsigned long long) digest (child_sc, child_rust), (int) loglen, logbuf);
	ssize_t	w = write (in_child, child_report, (size_t) n);
	(void) w;
	exit (0);	/* exit, not _exit: the coverage run's counters are written at exit */
}

static int forked_side (int rust, const scenario_t *sc, char *out, size_t outsize)
{
	int	fd[2], status;
	pid_t	pid;
	size_t	got = 0;
	ssize_t	r;

	if (pipe (fd) != 0)
	{
		perror ("pipe");
		exit (2);
	}
	fflush (NULL);
	pid = fork ();
	if (pid < 0)
	{
		perror ("fork");
		exit (2);
	}
	if (pid == 0)
	{
		close (fd[0]);
		in_child = fd[1];
		child_sc = sc;
		child_rust = rust;
		restore (sc);
		run_side (rust, sc);
		report_and_exit ();
	}
	close (fd[1]);
	while (got < outsize - 1 && (r = read (fd[0], out + got, outsize - 1 - got)) > 0)
		got += (size_t) r;
	out[got] = 0;
	close (fd[0]);
	waitpid (pid, &status, 0);
	if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
	{
		snprintf (out + got, outsize - got, "|DIED %d", status);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------ */

static long	n_cases, n_forked, n_problems;
static long	o_huge[5], o_measured, o_shed, o_shed_sort, o_deferred, o_trailer, o_fatal;
static long	o_long, o_morebits2, o_alpha, o_classdep, o_clear, o_removals, o_cleanup;
static long	o_branch[4], o_nan_shed;

static void problem (long i, const scenario_t *sc, const char *what)
{
	n_problems++;
	if (n_problems <= 10)
		printf ("MISMATCH case #%ld (huge %d, %d edicts, client %d): %s\n", i, sc->huge, sc->ne, sc->k, what);
}

static void floor_check (const char *what, long have, long want)
{
	if (have < want)
	{
		printf ("COVERAGE: %s reached %ld time(s), floor %ld\n", what, have, want);
		n_problems++;
	}
}

/* the frame state machine's branch, from the inputs */
static int frame_branch (void)
{
	if (pristine_client[1 + 2] != pristine_client[2])
		return 0;	/* old sequence */
	if (pristine_client[1] == 255 || pristine_client[1] == 0 || pristine_client[1] == MAX_FRAMES + 1)
		return 1;	/* expired */
	if (pristine_client[1] >= 1 && pristine_client[1] <= pristine_client[0])
		return 2;	/* valid */
	return 3;		/* normal advance */
}

/* what the C side wrote, for the floors */
static void count_outcome (const scenario_t *sc, const char *log, int before)
{
	int	p, bits, e;

	if (strstr (log, "by netsort"))
		o_shed++;
	if (strstr (log, "shed (") && !strstr (log, "(0 by netsort"))
		o_shed_sort++;
	if (strstr (log, "deferred"))
		o_deferred++;
	if (strstr (log, "exceeds measured reserve"))
		o_trailer++;
	if (strstr (log, "MI|"))
		o_classdep++;
	if (sc->huge)
		o_huge[sc->huge]++;
	if (sv_netsort.integer && pristine_cursize + sc->ne * 24 + (2 + 2 * MAX_CLEAR_EDICTS_PER_MSG) > pristine_maxsize)
		o_measured++;
	if (sc->cleanup)
		o_cleanup++;
	o_branch[frame_branch ()]++;

	/* walk the updates: svc_reference, then update after update */
	if (msg.overflowed || msg.cursize < before + 3 || msgbuf[before] != svc_reference)
		return;
	p = before + 3;
	while (p < msg.cursize && (msgbuf[p] & U_SIGNAL))
	{
		bits = msgbuf[p];
		if (bits & U_MOREBITS)
			bits |= msgbuf[p + 1] << 8;
		if (bits & U_MOREBITS2)
			bits |= msgbuf[p + 2] << 16;
		if (bits & U_LONGENTITY)
			o_long++;
		if (bits & U_MOREBITS2)
			o_morebits2++;
		if (bits & U_ALPHA)
			o_alpha++;
		if (bits & U_CLEAR_ENT)
			o_clear++;
		{
			/* the update's size, as the writer emits it */
			int	len = 1 + !!(bits & U_MOREBITS) + !!(bits & U_MOREBITS2) + ((bits & U_LONGENTITY) ? 2 : 1);
			len += (bits & U_MODEL) ? 2 : 0;
			len += !!(bits & U_FRAME) + !!(bits & U_COLORMAP) + ((bits & U_SKIN) ? 2 : 0) + !!(bits & U_EFFECTS);
			len += ((bits & U_ORIGIN1) ? 2 : 0) + !!(bits & U_ANGLE1) + ((bits & U_ORIGIN2) ? 2 : 0) + !!(bits & U_ANGLE2);
			len += ((bits & U_ORIGIN3) ? 2 : 0) + !!(bits & U_ANGLE3) + ((bits & U_SCALE) ? 2 : 0) + !!(bits & U_ALPHA);
			p += len;
		}
	}
	if (p + 1 < msg.cursize && msgbuf[p] == svc_clear_edicts && msgbuf[p + 1])
		o_removals++;
	(void) e;
}

int main (void)
{
	/* ENTS_CASES overrides the count, for timing and debugging; the gate's
	 * floors are set for the default */
	const long	NCASES = getenv ("ENTS_CASES") ? atol (getenv ("ENTS_CASES")) : 12000;
	static byte	msg_c[MSGMAX];
	static char	log_c[sizeof (logbuf)];
	static byte	cc_c[NE_MAX];
	static float	fx_c[NE_MAX];
	static byte	fat_c[MAX_MAP_LEAFS / 8];
	static char	fork_c[sizeof (child_report) + 64], fork_r[sizeof (child_report) + 64];
	static byte	*frames_r;
	long		i;
	int		k;

	if (check_layouts ())
	{
		printf ("sv_main_ents differential harness (hexen2): FAIL, the mirrored layouts differ\n");
		return 1;
	}
	printf ("layouts: entity_state_t, entity_state2_t, client_frames_t, client_state2_t agree\n");

	edict_store = calloc (NE_MAX, (size_t) pr_edict_size);
	sv.states = calloc (MAXC, sizeof (client_state2_t));
	pristine_states = calloc (1, sizeof (client_state2_t));
	frames_c = malloc ((size_t) (MAX_FRAMES + 2) * MAX_CLIENT_STATES * 37);
	frames_r = malloc ((size_t) (MAX_FRAMES + 2) * MAX_CLIENT_STATES * 37);
	if (!edict_store || !sv.states || !pristine_states || !frames_c || !frames_r)
		return 2;
	sv.edicts = (edict_t *) edict_store;
	svs.clients = clients;
	svs.maxclients = MAXC;
	svs.maxclientslimit = MAXC;

	for (i = 0; i < NCASES; i++)
	{
		scenario_t	sc;
		outcome_t	oc, orr;
		size_t		nfc, nfr;
		int		before, fb;
		int		c_hit, r_hit;

		gen_case (&sc, (int) i < NCASES - NDIRECTED ? -1 : (int) (i - (NCASES - NDIRECTED)));
		n_cases++;

		if (sc.fatal)
		{
			int	okc, okr;
			n_forked++;
			okc = forked_side (0, &sc, fork_c, sizeof (fork_c));
			okr = forked_side (1, &sc, fork_r, sizeof (fork_r));
			if (sc.nonfinite)
			{
				/* A NaN box: the key is NaN, the C's (int) of it is INT_MIN,
				 * and the measuring pass writes binbytes[] with it, so the
				 * reference dies.  The port takes a non-finite key as the
				 * least important bin and sheds that entity instead, which is
				 * the one place the two are meant to disagree; a case whose
				 * frame cannot overflow never computes the key, and there they
				 * must still agree. */
				if (okc)
				{
					if (!okr || strcmp (fork_c, fork_r) != 0)
						problem (i, &sc, "forked, with no NaN key computed");
					continue;
				}
				if (!okr)
					problem (i, &sc, "a NaN priority: the C died and the Rust must not");
				else if (i == NCASES - NDIRECTED + DIR_NAN_PRIO
					 && !strstr (fork_r, "nearest shed bin 255"))
					/* the directed case's scene leaves that entity the only
					 * one that can be shed, so its message has to name the bin
					 * the port gave it.  A generated case sheds others too,
					 * and `nearest shed bin' is the least of them. */
					problem (i, &sc, "the NaN priority case must shed bin 255");
				else
					o_nan_shed++;
				continue;
			}
			if (!okc || !okr || strcmp (fork_c, fork_r) != 0)
			{
				problem (i, &sc, "forked");
				if (n_problems <= 10)
					printf ("    C    \"%.240s\"\n    Rust \"%.240s\"\n", fork_c, fork_r);
			}
			if (strstr (fork_c, "Host_Error|") || strstr (fork_c, "Sys_Error|"))
				o_fatal++;
			continue;
		}

		/* the C */
		restore (&sc);
		before = msg.cursize;
		fatal_hit = 0;
		fatal_armed = 1;
		if (setjmp (fatal_jmp) == 0)
			run_side (0, &sc);
		fatal_armed = 0;
		c_hit = fatal_hit;
		memset (&oc, 0, sizeof (oc));
		capture (&sc, &oc, 0);
		oc.fatbytes = c_fatbytes;
		memcpy (msg_c, msgbuf, MSGMAX);
		memcpy (log_c, logbuf, loglen);
		log_c[loglen] = 0;
		nfc = frames_bytes (&sc, frames_c);
		for (k = 0; k < sc.ne + 2; k++)
		{
			cc_c[k] = edict (k)->baseline.ClearCount[sc.k];
			fx_c[k] = edict (k)->v.effects;
		}
		fb = c_fatbytes > 0 && c_fatbytes <= (int) sizeof (fat_c) ? c_fatbytes : 0;
		memcpy (fat_c, c_fatpvs, (size_t) fb);
		count_outcome (&sc, log_c, before);

		/* the Rust */
		restore (&sc);
		fatal_hit = 0;
		fatal_armed = 1;
		if (setjmp (fatal_jmp) == 0)
			run_side (1, &sc);
		fatal_armed = 0;
		r_hit = fatal_hit;
		memset (&orr, 0, sizeof (orr));
		capture (&sc, &orr, 1);
		orr.fatbytes = oc.fatbytes;
		nfr = frames_bytes (&sc, frames_r);

		if (c_hit != r_hit)
			problem (i, &sc, "a fatal call: one side died and the other did not");
		else if (oc.cursize != orr.cursize || oc.overflowed != orr.overflowed)
			problem (i, &sc, "datagram cursize/overflowed");
		else if (memcmp (msg_c, msgbuf, (size_t) msg.maxsize) != 0)
		{
			int	at = 0;
			while (msg_c[at] == msgbuf[at])
				at++;
			problem (i, &sc, "datagram bytes");
			if (n_problems <= 10)
				printf ("    first difference at byte %d of %d: C %02x Rust %02x\n", at, oc.cursize, msg_c[at], msgbuf[at]);
		}
		else if (memcmp (oc.client, orr.client, 4) != 0)
			problem (i, &sc, "client frame/sequence bytes");
		else if (memcmp (oc.counts, orr.counts, sizeof (oc.counts)) != 0)
			problem (i, &sc, "frame counts");
		else if (nfc != nfr || memcmp (frames_c, frames_r, nfc) != 0)
		{
			size_t	at = 0;
			while (frames_c[at] == frames_r[at])
				at++;
			problem (i, &sc, "client frames");
			if (n_problems <= 10)
				printf ("    first difference: frame %zu entry %zu field byte %zu\n",
					at / 37 / (size_t) sc.hreg[0], (at / 37) % (size_t) sc.hreg[0], at % 37);
		}
		else if (memcmp (fat_c, SvMain_FatPvs (), (size_t) fb) != 0)
			problem (i, &sc, "fat PVS");
		else if (strlen (log_c) != loglen || memcmp (log_c, logbuf, loglen) != 0)
		{
			problem (i, &sc, "log");
			if (n_problems <= 10)
				printf ("    C    \"%.240s\"\n    Rust \"%.*s\"\n", log_c, (int) (loglen > 240 ? 240 : loglen), logbuf);
		}
		else
		{
			for (k = 0; k < sc.ne + 2; k++)
				if (cc_c[k] != edict (k)->baseline.ClearCount[sc.k]
				    || memcmp (&fx_c[k], &edict (k)->v.effects, 4) != 0)
				{
					problem (i, &sc, "edict ClearCount/effects");
					if (n_problems <= 10)
						printf ("    edict %d\n", k);
					break;
				}
		}
	}

	printf ("outcomes: frame branches old %ld, expired %ld, valid %ld, advance %ld;\n"
		"          huge write %ld, zero %ld, measure %ld, shed %ld; measured %ld,\n"
		"          shed %ld (by netsort %ld), deferred removals %ld, trailer clamp %ld,\n"
		"          removals %ld, U_LONGENTITY %ld, U_MOREBITS2 %ld, U_ALPHA %ld, U_CLEAR_ENT %ld,\n"
		"          class-dependent models %ld, cleanups %ld, fatal %ld, NaN priorities shed %ld\n",
		o_branch[0], o_branch[1], o_branch[2], o_branch[3],
		o_huge[1], o_huge[2], o_huge[3], o_huge[4], o_measured,
		o_shed, o_shed_sort, o_deferred, o_trailer, o_removals,
		o_long, o_morebits2, o_alpha, o_clear, o_classdep, o_cleanup, o_fatal, o_nan_shed);

	floor_check ("the old-sequence branch", o_branch[0], 1000);
	floor_check ("the expired branch", o_branch[1], 1000);
	floor_check ("the valid-frame branch", o_branch[2], 1000);
	floor_check ("the advance branch", o_branch[3], 1000);
	floor_check ("a huge write", o_huge[1], 15);
	floor_check ("a huge zero-delta reference", o_huge[2], 15);
	floor_check ("a huge measuring pass", o_huge[3], 15);
	floor_check ("a huge shed reference", o_huge[4], 15);
	floor_check ("a measuring pass", o_measured, 2000);
	floor_check ("a shed frame", o_shed, 1000);
	floor_check ("a netsort shed", o_shed_sort, 500);
	floor_check ("deferred removals", o_deferred, 80);
	floor_check ("the trailer clamp", o_trailer, 20);
	floor_check ("removals sent", o_removals, 2000);
	floor_check ("U_LONGENTITY", o_long, 2000);
	floor_check ("U_MOREBITS2", o_morebits2, 2000);
	floor_check ("U_ALPHA", o_alpha, 2000);
	floor_check ("U_CLEAR_ENT", o_clear, 2000);
	floor_check ("a class-dependent model", o_classdep, 1000);
	floor_check ("SV_CleanupEnts", o_cleanup, 2000);
	floor_check ("a fatal call", o_fatal, 100);
	floor_check ("a NaN priority shed as bin 255", o_nan_shed, 100);

	if (n_problems)
	{
		printf ("sv_main_ents differential harness (hexen2): FAIL, %ld problem(s) in %ld cases\n", n_problems, n_cases);
		return 1;
	}
	printf ("sv_main_ents differential harness (hexen2): PASS (%ld cases: %ld in-process, %ld forked)\n",
		n_cases, n_cases - n_forked, n_forked);
	return 0;
}
