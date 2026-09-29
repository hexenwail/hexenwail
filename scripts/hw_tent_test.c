/*
 * Unit tests for the HexenWorld temp-entity parser
 * (engine/hexen2/cl_hw_tent.inc).  GitHub #213.
 *
 * The parser's one hard duty is to consume every svc_temp_entity payload
 * exactly: one byte short or long and everything after it in the packet --
 * reliable svc_update_inv included -- is misread or dropped.  Each case
 * here builds a payload the way gamecode/hc/hw writes it, appends a
 * sentinel byte, parses, and checks the reader stopped exactly on the
 * sentinel.  scripts/hw_tent_layout_check.py derives the fixed sizes from
 * the gamecode writers independently; the sizes below are this test's own
 * copy of that same spec, so the table in the .inc is checked twice.
 *
 * The effects half (cl_hw_tent_fx.inc, part 2 of #213) is driven through
 * the same parser.  Its cases check what can go wrong without a renderer:
 * that every effect still reads exactly its table size, that the explosion
 * pool is fixed and reuses the oldest slot, that entity numbers and skins
 * off the wire are range-checked before they index anything, that a
 * missing model means no explosion rather than a crash, and that a level
 * change or disconnect leaves nothing behind.
 *
 * Build and run:
 *     cc -Wall -Wextra -o /tmp/hwtenttest scripts/hw_tent_test.c -lm && /tmp/hwtenttest
 * Exit status 0 on pass, 1 on any failed assertion.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int qboolean;
#define true	1
#define false	0
typedef float vec3_t[3];
typedef unsigned char byte;

/* ------------------------------------------------------------------ */
/* stubs for the engine symbols the .inc uses                          */
/* ------------------------------------------------------------------ */

static unsigned char	buf[4096];
static int		buf_len, msg_readcount;
static qboolean		msg_badread;
static vec3_t		vec3_origin;

static int MSG_ReadByte (void)
{
	if (msg_readcount + 1 > buf_len)
	{
		msg_badread = true;
		return -1;
	}
	return buf[msg_readcount++];
}

static int MSG_ReadShort (void)
{
	int v;
	if (msg_readcount + 2 > buf_len)
	{
		msg_badread = true;
		msg_readcount = buf_len;
		return -1;
	}
	v = (short)(buf[msg_readcount] | (buf[msg_readcount + 1] << 8));
	msg_readcount += 2;
	return v;
}

static int MSG_ReadLong (void)
{
	int v;
	if (msg_readcount + 4 > buf_len)
	{
		msg_badread = true;
		msg_readcount = buf_len;
		return -1;
	}
	v = (int)((unsigned)buf[msg_readcount] | ((unsigned)buf[msg_readcount + 1] << 8) |
		((unsigned)buf[msg_readcount + 2] << 16) | ((unsigned)buf[msg_readcount + 3] << 24));
	msg_readcount += 4;
	return v;
}

static float MSG_ReadCoord (void)
{
	return MSG_ReadShort () * (1.0f / 8.0f);
}

static void HWCL_ReadCoords (vec3_t c)
{
	int i;
	for (i = 0; i < 3; i++)
		c[i] = MSG_ReadCoord ();
}

/* CL_ParseTEntType consumes the Hexen II layouts (cl_tent.c). */
static int last_h2type = -1, h2_calls;
static void CL_ParseTEntType (int type)
{
	int size;

	last_h2type = type;
	h2_calls++;
	switch (type)
	{
	case 0: case 1: case 3: case 7: case 8: case 10: case 11:
		size = 6; break;			/* pos */
	case 5: case 6: case 9:
		size = 2 + 12; break;			/* entity, start, end */
	default:
		if (type >= 24 && type <= 32)	/* ParseStream */
		{
			size = 2 + 1 + 1 + 12 + (type == 29 ? 1 : 0);
			break;
		}
		printf ("  CL_ParseTEntType got non-Hexen II type %d\n", type);
		size = 0;
	}
	while (size-- > 0)
		MSG_ReadByte ();
}

static int particle_calls, blob_calls;
static void R_RunParticleEffect (vec3_t o, vec3_t d, int c, int n)
{ (void)o; (void)d; (void)c; (void)n; particle_calls++; }
static void R_BlobExplosion (vec3_t o) { (void)o; blob_calls++; }

static int verbose;
static void Con_DPrintf (const char *fmt, ...)
{
	va_list ap;
	if (!verbose)
		return;
	va_start (ap, fmt);
	vprintf (fmt, ap);
	va_end (ap);
}

/* --- what cl_hw_tent_fx.inc needs ------------------------------------ */

#define M_PI			3.14159265358979323846
#define DRF_TRANSLUCENT		128
#define MLS_ABSLIGHT		7
#define SCALE_TYPE_UNIFORM	0
#define SCALE_TYPE_XYONLY	8
#define SCALE_ORIGIN_CENTER	0
#define HX_FRAME_TIME		0.05
#define CONTENTS_EMPTY		-1
#define CONTENTS_SOLID		-2
#define MAX_VISEDICTS		64
#define MAX_DYNAMIC_CHANNELS	128
#define MAX_EDICTS		8192
#define HWCL_MAX_ENTITIES	768	/* cl_hw.c */

#define THINGTYPE_GREYSTONE	1
#define THINGTYPE_WOOD		2
#define THINGTYPE_METAL		3
#define THINGTYPE_FLESH		4
#define THINGTYPE_CLAY		6
#define THINGTYPE_LEAVES	7
#define THINGTYPE_HAY		8
#define THINGTYPE_BROWNSTONE	9
#define THINGTYPE_CLOTH		10
#define THINGTYPE_WOOD_LEAF	11
#define THINGTYPE_WOOD_METAL	12
#define THINGTYPE_WOOD_STONE	13
#define THINGTYPE_METAL_STONE	14
#define THINGTYPE_METAL_CLOTH	15
#define THINGTYPE_WEBS		16
#define THINGTYPE_GLASS		17
#define THINGTYPE_ICE		18
#define THINGTYPE_CLEARGLASS	19
#define THINGTYPE_REDGLASS	20
#define THINGTYPE_ACID		21
#define THINGTYPE_METEOR	22
#define THINGTYPE_GREENFLESH	23

typedef enum { pt_static, pt_grav, pt_explode = 5, pt_darken = 25 } ptype_t;
enum { rt_smoke = 1, rt_blood = 2, rt_fireball = 7, rt_ice = 8,
	rt_vorpal = 11, rt_magicmissile = 13, rt_acidball = 16,
	rt_grensmoke = 18, rt_purify = 19 };

#define VectorCopy(a,b)		((b)[0] = (a)[0], (b)[1] = (a)[1], (b)[2] = (a)[2])
#define VectorSet(v,a,b,c)	((v)[0] = (a), (v)[1] = (b), (v)[2] = (c))
#define VectorClear(a)		((a)[0] = (a)[1] = (a)[2] = 0)
#define VectorAdd(a,b,c)	((c)[0] = (a)[0] + (b)[0], (c)[1] = (a)[1] + (b)[1], (c)[2] = (a)[2] + (b)[2])
#define VectorSubtract(a,b,c)	((c)[0] = (a)[0] - (b)[0], (c)[1] = (a)[1] - (b)[1], (c)[2] = (a)[2] - (b)[2])
#define VectorScale(a,s,c)	((c)[0] = (a)[0] * (s), (c)[1] = (a)[1] * (s), (c)[2] = (a)[2] * (s))
#define VectorMA(a,s,b,c)	((c)[0] = (a)[0] + (s) * (b)[0], (c)[1] = (a)[1] + (s) * (b)[1], (c)[2] = (a)[2] + (s) * (b)[2])

static float VectorNormalizeFast (vec3_t v)
{
	float len = sqrtf (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (len)
	{
		v[0] /= len; v[1] /= len; v[2] /= len;
	}
	return len;
}

static void q_sincosrad (float a, float *s, float *c) { *s = sinf (a); *c = cosf (a); }
static void q_sincosdeg (float a, float *s, float *c) { q_sincosrad (a * (float)M_PI / 180, s, c); }

static void AngleVectors (vec3_t angles, vec3_t f, vec3_t r, vec3_t u)
{
	(void)angles;
	VectorSet (f, 1, 0, 0); VectorSet (r, 0, -1, 0); VectorSet (u, 0, 0, 1);
}

typedef struct { const char *name; int numframes; } qmodel_t;
typedef struct { const char *name; } sfx_t;
typedef struct { int contents; } mleaf_t;
typedef struct { vec3_t origin; float radius, die, decay, color[4]; } dlight_t;
typedef struct
{
	vec3_t origin, angles;
	qmodel_t *model;
	int frame, skinnum, drawflags, abslight, scale;
	byte *colormap;
} entity_t;

static struct { double time; qmodel_t *worldmodel; } cl;
static struct { byte *colormap; } vid;
static struct { float gravity; } movevars = { 800 };
static double host_frametime = 0.01;
static entity_t *cl_visedicts[MAX_VISEDICTS];
static int cl_numvisedicts;

/* Mod_ForName: every name loads, with 5 frames, unless models_missing. */
static qboolean models_missing;
static int mod_calls;
static qmodel_t mod_pool[256];
static int mod_pool_used;
static qmodel_t worldmodel = { "maps/test.bsp", 1 };
static qmodel_t *Mod_ForName (const char *name, qboolean crash)
{
	(void)crash;
	mod_calls++;
	if (models_missing)
		return NULL;
	if (mod_pool_used == 256)
		return NULL;
	mod_pool[mod_pool_used].name = name;
	mod_pool[mod_pool_used].numframes = 5;
	return &mod_pool[mod_pool_used++];
}

static mleaf_t test_leaf = { CONTENTS_EMPTY };
static mleaf_t *Mod_PointInLeaf (vec3_t p, qmodel_t *m)
{
	(void)p; (void)m;
	return &test_leaf;
}

static sfx_t test_sfx[64];
static int sfx_count, sound_calls;
static sfx_t *S_PrecacheSound (const char *name)
{
	sfx_t *sfx = &test_sfx[sfx_count++ & 63];
	sfx->name = name;
	return sfx;
}
static void S_StartSound (int e, int ch, sfx_t *sfx, vec3_t o, float v, float a)
{ (void)e; (void)ch; (void)sfx; (void)o; (void)v; (void)a; sound_calls++; }

static dlight_t test_dlight;
static dlight_t *CL_AllocDlight (int key) { (void)key; return &test_dlight; }

static void R_RocketTrail (vec3_t a, vec3_t b, int t) { (void)a; (void)b; (void)t; }
static void R_RunParticleEffect2 (vec3_t o, vec3_t a, vec3_t b, int c, ptype_t e, int n)
{ (void)o; (void)a; (void)b; (void)c; (void)e; (void)n; }
static void R_RunParticleEffect4 (vec3_t o, float r, int c, ptype_t e, int n)
{ (void)o; (void)r; (void)c; (void)e; (void)n; }
static void R_ColoredParticleExplosion (vec3_t o, int c, int r, int n)
{ (void)o; (void)c; (void)r; (void)n; }
static void R_SunStaffTrail (vec3_t a, vec3_t b) { (void)a; (void)b; }

/* cl_tent.c's CL_CreateStream: records every call, and flags any entity
 * number that would have indexed cl_entities out of range. */
static int stream_calls, stream_bad_ent;
static qboolean CL_CreateStream (int type, int ent, int flags, int tag,
		float duration, int skin, qmodel_t *const models[4],
		const vec3_t source, const vec3_t dest)
{
	(void)type; (void)flags; (void)tag; (void)duration; (void)skin;
	(void)source; (void)dest;
	if (ent < 0 || ent >= MAX_EDICTS)
		stream_bad_ent++;
	if (!models[0])
		return false;
	stream_calls++;
	return true;
}

/* cl_hw.c's lookup.  The index it is handed must already be in range; a
 * call outside [1, HWCL_MAX_ENTITIES) would be an out-of-bounds read of
 * hwcl_server_state there. */
static qboolean entities_present = true;
static int state_calls, state_bad_ent;
static qboolean HWCL_EntityStateOrigin (int ent, vec3_t origin)
{
	state_calls++;
	if (ent < 1 || ent >= HWCL_MAX_ENTITIES)
	{
		state_bad_ent++;
		return false;
	}
	if (!entities_present)
		return false;
	VectorSet (origin, 10, 20, 30);
	return true;
}

#include "../engine/hexen2/cl_hw_tent.inc"

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

#define SENTINEL	0xAB

static int failures, checks;

static void begin (int type)
{
	memset (buf, 0, sizeof(buf));
	buf_len = 0;
	msg_readcount = 0;
	msg_badread = false;
	buf[buf_len++] = (unsigned char)type;
}

static void put_byte (int b) { buf[buf_len++] = (unsigned char)b; }
static void put_short (int s) { put_byte (s & 255); put_byte ((s >> 8) & 255); }
static void put_bytes (int n, int v) { while (n-- > 0) put_byte (v); }
static void put_coord3 (int x, int y, int z) { put_short (x * 8); put_short (y * 8); put_short (z * 8); }

static void expect (int cond, const char *what, int type)
{
	checks++;
	if (!cond)
	{
		failures++;
		printf ("FAIL: type %d: %s (read %d of %d)\n", type, what,
				msg_readcount, buf_len);
	}
}

/* Parse; the reader must accept, stay in bounds and stop on the sentinel. */
static void run_exact (int type, const char *what)
{
	qboolean ok;

	put_byte (SENTINEL);
	ok = HWCL_ParseTempEntity ();
	expect (ok, what, type);
	expect (!msg_badread, "no bad read", type);
	expect (msg_readcount == buf_len - 1 && buf[msg_readcount] == SENTINEL,
			"stopped exactly on the sentinel", type);
}

/* Writer-derived payload sizes (bytes after the type byte), from the
 * gamecode/hc/hw writers; -1 = variable, tested separately. */
static const struct { int type, size; } spec[] =
{
	{0, 6}, {1, 6}, {2, 7}, {3, 6}, {4, 6}, {5, 14}, {6, 14}, {7, 6},
	{8, 6}, {9, 14}, {10, 6}, {11, 6}, {12, 7}, {13, 6},
	{25, 16}, {26, 16}, {27, 16}, {28, 16}, {29, 17}, {30, 16}, {31, 16},
	{32, 16},
	{33, 6}, {34, 14}, {35, 13}, {36, 7}, {37, 6}, {38, 6}, {39, 6},
	{40, 14}, {41, 13}, {42, 7}, {43, 2}, {44, 10}, {46, 2}, {47, 6},
	{49, 8}, {50, 8}, {51, 6}, {52, 6}, {53, 6}, {54, 6}, {55, 14},
	{56, 6}, {57, 10}, {58, 10}, {59, 8}, {60, 12}, {61, 10}, {62, 16},
	{63, 6}, {64, 6}, {65, 9}, {66, 6}, {67, 7}, {68, 13}, {69, 10},
	{70, 9}, {71, 9}, {72, 9}, {73, 9}, {74, 9}, {75, 11}, {76, 9},
	{77, 4}, {78, 8}, {79, 9}, {80, 9},
};

/* ------------------------------------------------------------------ */
/* effects (cl_hw_tent_fx.inc)                                         */
/* ------------------------------------------------------------------ */

static void reset_world (void)
{
	HWCL_ClearTempEntities ();
	memset (mod_pool, 0, sizeof(mod_pool));
	mod_pool_used = 0;
	mod_calls = 0;
	models_missing = false;
	entities_present = true;
	test_leaf.contents = CONTENTS_EMPTY;
	cl.time = 100;
	cl.worldmodel = &worldmodel;
	cl_numvisedicts = 0;
	host_frametime = 0.01;
}

static int live_explosions (void)
{
	int i, n = 0;

	for (i = 0; i < HWCL_MAX_EXPLOSIONS; i++)
		if (hwcl_explosions[i].model)
			n++;
	return n;
}

/* One frame as the client runs it: visedicts reset, then the update. */
static int run_frame (double dt)
{
	cl.time += dt;
	host_frametime = dt;
	cl_numvisedicts = 0;
	HWCL_UpdateTempEntities ();
	return cl_numvisedicts;
}

/* A fixed-size payload whose entity shorts, where it has any, read `ent'. */
static void put_fixed (int type, int size, int ent)
{
	int n;

	begin (type);
	switch (type)
	{
	case HWTE_ICESTORM: case HWTE_LIGHTNING_HAMMER:
		put_short (ent);
		break;
	case HWTE_CUBEBEAM:
		put_short (ent);
		put_short (ent);
		break;
	case HWTE_LIGHTNINGEXPLODE: case HWTE_SUNSTAFF_POWER:
		put_short (ent);
		put_bytes (size - 2, 1);
		break;
	case HWTE_SWORD_EXPLOSION:
		put_coord3 (1, 2, 3);
		put_short (ent);
		break;
	default:
		for (n = 0; n < size; n++)
			put_byte (1 + (n % 7));
	}
}

static void put_gibs (void)
{
	begin (HWTE_PLAYER_DEATH);
	put_coord3 (1, 2, 3);
	put_bytes (4, 10);	/* angle, pitch, force, style */
}

static void test_effects (void)
{
	static const int bad_ents[] = {-1, -32768, 0, HWCL_MAX_ENTITIES,
			HWCL_MAX_ENTITIES + 1, 32767};
	size_t i, b;
	int n, r, allocs, calls;
	hwcl_explosion_t *ex;

	/* 8. every fixed type still reads exactly its table size through its
	 * effect: with models, without them, and with no entity found */
	for (r = 0; r < 3; r++)
	{
		reset_world ();
		models_missing = (r == 1);
		entities_present = (r != 2);
		for (i = 0; i < sizeof(spec) / sizeof(spec[0]); i++)
		{
			if (spec[i].type >= HWTE_COUNT || !hwte_fixed_size[spec[i].type])
				continue;
			put_fixed (spec[i].type, spec[i].size, 5);
			run_exact (spec[i].type, "effect reads its table size");
			run_frame (0.01);
		}
	}
	expect (hwte_size_mismatches == 0, "no effect read off its table size", -1);

	/* 9. the pool: fixed size, free slots first, then the oldest start */
	reset_world ();
	for (n = 0; n < HWCL_MAX_EXPLOSIONS; n++)
	{
		ex = HWCL_NewExplosion (HWM_GEN_EXPL);
		ex->startTime = 50 + n;
		ex->endTime = 200;
	}
	expect (live_explosions () == HWCL_MAX_EXPLOSIONS, "pool fills", -1);
	hwcl_explosions[57].startTime = 10;	/* the oldest */
	ex = HWCL_AllocExplosion ();
	expect (ex == &hwcl_explosions[57], "full pool reuses the oldest start", -1);
	expect (!ex->model && !ex->frameFunc && ex->startTime == 0,
			"a reused slot comes back zeroed", -1);
	ex->model = &worldmodel;
	hwcl_explosions[90].model = NULL;
	expect (HWCL_AllocExplosion () == &hwcl_explosions[90],
			"a free slot wins over the oldest", -1);
	hwcl_explosions[90].model = &worldmodel;
	for (n = 0; n < HWCL_MAX_EXPLOSIONS; n++)
		hwcl_explosions[n].startTime = cl.time + 5;	/* all in the future */
	expect (HWCL_AllocExplosion () == &hwcl_explosions[0],
			"nothing older than now: slot 0, as the original", -1);
	hwcl_explosions[0].model = &worldmodel;
	for (n = 0; n < 500; n++)
	{
		ex = HWCL_NewExplosion (HWM_SM_EXPLD);
		expect (ex >= hwcl_explosions && ex < hwcl_explosions + HWCL_MAX_EXPLOSIONS,
				"allocation stays in the pool", -1);
		ex->startTime = cl.time - n;
	}
	expect (live_explosions () == HWCL_MAX_EXPLOSIONS,
			"overflow reuses, never grows", -1);

	/* 10. entity numbers off the wire are refused before they index
	 * anything, and the payload is still consumed exactly */
	{
		static const int types[] = {
			HWTE_ICESTORM, HWTE_LIGHTNING_HAMMER, HWTE_SWORD_EXPLOSION,
			HWTE_SUNSTAFF_POWER, HWTE_CUBEBEAM, HWTE_LIGHTNINGEXPLODE
		};
		size_t t;

		for (t = 0; t < sizeof(types) / sizeof(types[0]); t++)
		{
			for (b = 0; b < sizeof(bad_ents) / sizeof(bad_ents[0]); b++)
			{
				reset_world ();
				state_bad_ent = stream_bad_ent = stream_calls = 0;
				put_fixed (types[t], hwte_fixed_size[types[t]], bad_ents[b]);
				run_exact (types[t], "bad entity: payload consumed");
				expect (state_bad_ent == 0, "no state lookup out of range", types[t]);
				expect (stream_bad_ent == 0, "no stream keyed out of range", types[t]);
				/* entity 0 is a legal stream key (LIGHTNINGEXPLODE never
				 * looked its entity up); anything else out of range
				 * must make no stream at all */
				if (bad_ents[b] != 0)
					expect (stream_calls == 0, "no stream for a bad entity", types[t]);
			}
			/* the positive control: a present entity makes streams */
			reset_world ();
			stream_calls = 0;
			put_fixed (types[t], hwte_fixed_size[types[t]], 5);
			run_exact (types[t], "good entity");
			expect (stream_calls > 0, "a present entity makes its streams", types[t]);
		}
	}

	/* SUNSTAFF_CHEAP and CHAINLIGHTNING carry an entity too */
	for (b = 0; b < sizeof(bad_ents) / sizeof(bad_ents[0]); b++)
	{
		reset_world ();
		state_bad_ent = stream_bad_ent = stream_calls = 0;
		begin (HWTE_SUNSTAFF_CHEAP);
		put_short (bad_ents[b]);
		put_byte (1);
		for (n = 0; n < 3; n++)
			put_coord3 (10 + n, 20, 30);
		run_exact (HWTE_SUNSTAFF_CHEAP, "sunstaff, bad entity");
		expect (state_bad_ent == 0 && stream_bad_ent == 0 && stream_calls == 0,
				"sunstaff: bad entity refused", HWTE_SUNSTAFF_CHEAP);

		begin (HWTE_CHAINLIGHTNING);
		put_short (bad_ents[b]);
		put_coord3 (1, 2, 3);
		put_coord3 (4, 5, 6);
		put_coord3 (0, 0, 0);
		run_exact (HWTE_CHAINLIGHTNING, "chain, bad entity");
		expect (stream_bad_ent == 0, "chain: no stream out of range",
				HWTE_CHAINLIGHTNING);
		if (bad_ents[b] != 0)
			expect (stream_calls == 0, "chain: bad entity makes no bolt",
					HWTE_CHAINLIGHTNING);
	}

	/* a reflect count far past the original's four-point array */
	reset_world ();
	stream_calls = 0;
	begin (HWTE_SUNSTAFF_CHEAP);
	put_short (5);
	put_byte (200);
	for (n = 0; n < 202; n++)
		put_coord3 (n + 1, 20, 30);
	run_exact (HWTE_SUNSTAFF_CHEAP, "sunstaff, 200 reflections");
	expect (stream_calls == 3, "sunstaff: at most 3 beams", HWTE_SUNSTAFF_CHEAP);

	/* a chain longer than the original kept */
	reset_world ();
	stream_calls = 0;
	begin (HWTE_CHAINLIGHTNING);
	put_short (5);
	for (n = 0; n < 40; n++)
		put_coord3 (n + 1, 1, 1);
	put_coord3 (0, 0, 0);
	run_exact (HWTE_CHAINLIGHTNING, "chain, 40 targets");
	expect (stream_calls == 8, "chain: 9 targets kept, 8 bolts",
			HWTE_CHAINLIGHTNING);

	/* 11. teleport skins: the wire value is held to the model skin range */
	{
		static const struct { int wire, want; } skins[] = {
			{0, 0}, {1, 1}, {99, 99}, {100, 0}, {255, 0}, {256, 0},
			{32767, 0}, {-1, 0}, {-32768, 0}
		};

		for (i = 0; i < sizeof(skins) / sizeof(skins[0]); i++)
		{
			reset_world ();
			begin (HWTE_HWTELEPORT);
			put_coord3 (1, 2, 3);
			put_short (skins[i].wire);
			run_exact (HWTE_HWTELEPORT, "teleport skin");
			expect (hwcl_explosions[0].model &&
					hwcl_explosions[0].skin == skins[i].want,
					"teleport skin clamped", skins[i].wire);
		}
	}

	/* 12. a missing model: no explosion, no entity, and one lookup */
	reset_world ();
	models_missing = true;
	allocs = hwcl_tent_allocs;
	put_gibs ();
	run_exact (HWTE_PLAYER_DEATH, "gibs without models");
	calls = mod_calls;
	put_gibs ();
	run_exact (HWTE_PLAYER_DEATH, "gibs without models, again");
	expect (hwcl_tent_allocs == allocs, "missing model allocates nothing", -1);
	expect (live_explosions () == 0, "missing model leaves the pool empty", -1);
	expect (mod_calls == calls, "a missing model is looked up once per map", -1);
	expect (run_frame (0.01) == 0, "missing model links nothing", -1);

	/* 13. drawing: gibs link, and are gone by their end time */
	reset_world ();
	put_gibs ();
	run_exact (HWTE_PLAYER_DEATH, "gibs");
	n = live_explosions ();
	expect (n == 12, "fast frame: all 12 gibs", -1);
	r = hwcl_tent_linked;
	expect (run_frame (0.01) == n, "every gib is linked", -1);
	expect (hwcl_tent_linked == r + n, "first draw counted once each", -1);
	expect (cl_visedicts[0] && cl_visedicts[0]->model &&
			cl_visedicts[0]->frame >= 0 && cl_visedicts[0]->frame < 5,
			"linked entity has a model and a frame in range", -1);
	expect (run_frame (0.01) == n && hwcl_tent_linked == r + n,
			"second frame links again, counts nothing new", -1);
	expect (run_frame (5.0) == 0 && live_explosions () == 0,
			"gibs gone after their 4 seconds", -1);

	/* a slow frame makes 4 of the 12, as the original did */
	reset_world ();
	host_frametime = 0.1;
	put_gibs ();
	run_exact (HWTE_PLAYER_DEATH, "gibs, slow frame");
	expect (live_explosions () == 4, "slow frame: 4 gibs", -1);

	/* the visedict list is never overrun */
	reset_world ();
	for (n = 0; n < HWCL_MAX_EXPLOSIONS; n++)
	{
		ex = HWCL_NewExplosion (HWM_GEN_EXPL);
		ex->startTime = cl.time;
		ex->endTime = cl.time + 10;
	}
	expect (run_frame (0.01) == MAX_VISEDICTS, "visedicts capped, not overrun", -1);

	/* 14. a still frame past the model's end is pinned for the renderer */
	reset_world ();
	ex = HWCL_NewExplosion (HWM_AXTAIL);
	ex->startTime = cl.time;
	ex->endTime = cl.time + 1;
	ex->exflags = HWEX_STILL_FRAME;
	ex->data = 50;
	run_frame (0.01);
	expect (cl_numvisedicts == 1 && cl_visedicts[0]->frame == 4,
			"still frame clamped to numframes - 1", -1);

	/* 15. a colliding flyer dies in a wall */
	reset_world ();
	begin (HWTE_AXE);
	put_coord3 (1, 2, 3);
	put_byte (0);
	put_byte (0);
	put_byte (100);
	run_exact (HWTE_AXE, "axe");
	expect (live_explosions () == 2, "axe: blade and tail", HWTE_AXE);
	test_leaf.contents = CONTENTS_SOLID;
	expect (run_frame (0.01) == 0 && live_explosions () == 0,
			"axe removed on hitting solid", HWTE_AXE);

	/* a fire wall over a floorless void stops looking for the floor */
	reset_world ();
	begin (HWTE_FIREWALL);
	put_coord3 (1, 2, 3);
	put_byte (0);
	put_byte (0);
	put_byte (8);
	run_exact (HWTE_FIREWALL, "fire wall over a void");
	expect (live_explosions () == 16, "fire wall: 8 flames, 8 streaks", -1);

	/* 16. level change / disconnect: nothing survives the clear */
	reset_world ();
	HWCL_PrecacheTEntSounds ();
	begin (HWTE_TIME_BOMB);
	put_coord3 (1, 2, 3);
	run_exact (HWTE_TIME_BOMB, "time bomb");
	expect (live_explosions () > 0, "time bomb made explosions", -1);
	HWCL_ClearTempEntities ();
	expect (live_explosions () == 0, "clear empties the pool", -1);
	for (n = 0, r = 0; n < HWM_COUNT; n++)
		r += hwcl_tent_models[n] != NULL || hwcl_tent_model_tried[n];
	expect (r == 0, "clear forgets every model lookup", -1);
	for (n = 0, r = 0; n < HWS_COUNT; n++)
		r += hwcl_tent_sfx[n] != NULL;
	expect (r == 0, "clear drops every sound handle", -1);
	expect (run_frame (0.01) == 0, "nothing drawn after the clear", -1);

	/* no world yet (before signon): update is a no-op, not a NULL deref */
	reset_world ();
	begin (HWTE_ICEHIT);
	put_coord3 (1, 2, 3);
	put_byte (1);
	run_exact (HWTE_ICEHIT, "ice hit");
	cl.worldmodel = NULL;
	expect (run_frame (0.01) == 0, "no world: nothing linked", -1);

	/* 17. every type byte, random payloads, random frames: never reads
	 * past the message, never hands a lookup an out-of-range entity */
	srand (1234);
	state_bad_ent = stream_bad_ent = 0;
	for (r = 0; r < 20000; r++)
	{
		int len = rand () % 64;

		if (!(r % 500))
			reset_world ();
		memset (buf, 0, sizeof(buf));
		buf_len = 0;
		msg_readcount = 0;
		msg_badread = false;
		buf[buf_len++] = (unsigned char)(rand () % HWTE_COUNT);
		for (n = 0; n < len; n++)
			buf[buf_len++] = (unsigned char)rand ();
		entities_present = rand () & 1;
		models_missing = !(rand () % 8);
		(void)HWCL_ParseTempEntity ();
		checks++;
		if (msg_readcount > buf_len)
		{
			failures++;
			printf ("FAIL: fuzz read past the end (type %d)\n", buf[0]);
		}
		if (!(r % 3))
			run_frame (0.013 * (1 + rand () % 8));
	}
	expect (state_bad_ent == 0 && stream_bad_ent == 0,
			"fuzz: no out-of-range entity reached a lookup", -1);
	expect (hwte_size_mismatches == 0, "fuzz: no effect read off its table size", -1);
}

int main (void)
{
	size_t i;
	int r, n;

	/* 1. every fixed layout, filled with 0x01 so no short reads as zero */
	for (i = 0; i < sizeof(spec) / sizeof(spec[0]); i++)
	{
		begin (spec[i].type);
		put_bytes (spec[i].size, 1);
		run_exact (spec[i].type, "fixed layout accepted");
	}

	/* 2. SUNSTAFF_CHEAP: entity, reflect count, (2 + reflect) points */
	for (r = 0; r <= 3; r++)
	{
		begin (HWTE_SUNSTAFF_CHEAP);
		put_short (12);
		put_byte (r);
		for (n = 0; n < 2 + r; n++)
			put_coord3 (10 + n, 20, 30);
		run_exact (HWTE_SUNSTAFF_CHEAP, "sunstaff reflect layout");
	}

	/* 3. CHAINLIGHTNING: entity, points, all-zero terminator */
	for (r = 0; r <= 10; r += 5)
	{
		begin (HWTE_CHAINLIGHTNING);
		put_short (7);
		for (n = 0; n < r; n++)
			put_coord3 (n + 1, 0, 0);
		put_coord3 (0, 5, 0);	/* zero x alone does not end the list */
		put_coord3 (0, 0, 0);
		run_exact (HWTE_CHAINLIGHTNING, "chain lightning list");
	}

	/* 4. a truncated chain (no terminator) must stop at the end */
	begin (HWTE_CHAINLIGHTNING);
	put_short (7);
	put_coord3 (1, 2, 3);
	(void)HWCL_ParseTempEntity ();
	expect (msg_badread && msg_readcount == buf_len,
			"unterminated chain stops at end of message", HWTE_CHAINLIGHTNING);

	/* 5. types no HexenWorld server sends are refused, nothing consumed */
	{
		static const int bad[] = {14, 20, 24, 48, 82, 200, 255};
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		{
			begin (bad[i]);
			put_bytes (8, 1);
			expect (!HWCL_ParseTempEntity (), "unknown type refused", bad[i]);
			expect (msg_readcount == 1, "nothing past the type byte", bad[i]);
		}
	}

	/* 6. routing to the Hexen II renderer */
	begin (HWTE_STREAM_LIGHTNING_SMALL);
	put_bytes (16, 1);
	last_h2type = -1;
	run_exact (HWTE_STREAM_LIGHTNING_SMALL, "stream lightning small");
	expect (last_h2type == 24, "HW 62 reaches Hexen II as its 24", 62);

	begin (HWTE_BIGGRENADE);
	put_bytes (6, 1);
	n = h2_calls;
	run_exact (HWTE_BIGGRENADE, "big grenade");
	expect (h2_calls == n, "HW 33 is NOT Hexen II's TE_LIGHT_PULSE", 33);

	begin (HWTE_GUNSHOT);
	put_byte (3);
	put_coord3 (1, 2, 3);
	n = h2_calls;
	r = particle_calls;
	run_exact (HWTE_GUNSHOT, "gunshot");
	expect (h2_calls == n, "HW gunshot (count byte) not sent to Hexen II", 2);
	expect (particle_calls == r + 1, "gunshot draws particles", 2);

	begin (HWTE_TAREXPLOSION);
	put_coord3 (1, 2, 3);
	r = blob_calls;
	run_exact (HWTE_TAREXPLOSION, "tar explosion");
	expect (blob_calls == r + 1, "tar explosion draws a blob", 4);

	/* 7. packet survival: a reliable temp entity followed by the
	 * svc_update_inv that clears the rook -- the #211 chain */
	begin (HWTE_ICEHIT);
	put_coord3 (100, 200, 300);
	put_byte (1);
	put_byte (58);		/* svc_update_inv follows in the same packet */
	(void)HWCL_ParseTempEntity ();
	expect (MSG_ReadByte () == 58, "the next svc is still readable", HWTE_ICEHIT);

	test_effects ();

	printf ("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS",
			checks, failures);
	return failures ? 1 : 0;
}
