/*
 * Unit tests for the HexenWorld effect-message readers
 * (engine/hexen2/cl_hw_effects.inc).  GitHub #213.
 *
 * Companion to scripts/hw_tent_test.c, same duty for a different message
 * family: consume every svc_start_effect / svc_update_effect /
 * svc_turn_effect / svc_multieffect payload exactly.  One byte short or
 * long and everything after it in the packet -- reliable svc_update_inv
 * included -- is misread or dropped.
 *
 * Each case builds the payload the way the engine's server writes it
 * (SV_SendEffect in hexenworld/server/sv_effect.c, PF_updateeffect and
 * PF_turneffect in h2shared/pr_cmds.c, SV_ParseMultiEffect in
 * sv_effect.c), appends a sentinel byte, parses, and checks the reader
 * stopped exactly on the sentinel.
 *
 * scripts/hw_ce_layout_check.py derives the same sizes independently by
 * walking those writers, so the sizes below are a second, hand-written
 * copy of the spec and the reader is checked twice.
 *
 * Build and run:
 *     cc -Wall -Wextra -o /tmp/hwcetest scripts/hw_ce_test.c && /tmp/hwcetest
 * Exit status 0 on pass, 1 on any failed assertion.
 */

#include <stdio.h>
#include <string.h>

typedef int qboolean;
#define true	1
#define false	0
typedef float vec3_t[3];
typedef unsigned char byte;

/* The .inc translates HexenWorld wire numbers into the maintained Hexen II
 * client's own CE_* ids, so it needs both tables -- exactly as in the real
 * build, where quakedef.h supplies this one and the .inc redefines the
 * names it needs at HexenWorld's values. */
#include "../engine/hexen2/effects.h"

/* ------------------------------------------------------------------ */
/* stubs for the engine symbols the .inc uses                          */
/* ------------------------------------------------------------------ */

static unsigned char	buf[4096];
static int		buf_len, msg_readcount;
static qboolean		msg_badread;

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

static float MSG_ReadCoord (void)
{
	return MSG_ReadShort () * (1.0f / 8.0f);
}

static float MSG_ReadAngle (void)
{
	return MSG_ReadByte () * (360.0f / 256.0f);
}

static float MSG_ReadFloat (void)
{
	int i;
	float f = 0;

	/* only the byte count matters here */
	for (i = 0; i < 4; i++)
		MSG_ReadByte ();
	return f;
}

static void HWCL_SkipCoords (int count)
{
	int i;
	for (i = 0; i < count; i++)
		MSG_ReadCoord ();
}

static void HWCL_ReadCoords (vec3_t coords)
{
	int i;
	for (i = 0; i < 3; i++)
		coords[i] = MSG_ReadCoord ();
}

static void HWCL_SkipAngles (int count)
{
	int i;
	for (i = 0; i < count; i++)
		MSG_ReadAngle ();
}

static void HWCL_SkipFloats (int count)
{
	int i;
	for (i = 0; i < count; i++)
		MSG_ReadFloat ();
}

/* The real CL_ParseHWEffect re-reads the message from the rewound cursor;
 * HWCL_ParseStartEffect forces the cursor back to the end of the payload
 * afterwards, so for byte accounting all that matters is that it ran. */
static int parse_calls, last_parse_type;
static void CL_ParseHWEffect (int type)
{ parse_calls++; last_parse_type = type; }

static int update_calls, last_update_type;
static void CL_UpdateHWEffect (int idx, int type, int command, float value,
		vec3_t angles, vec3_t origin, int extra)
{
	(void)idx; (void)command; (void)value; (void)angles; (void)origin;
	(void)extra;
	update_calls++; last_update_type = type;
}

static int turn_calls;
static void CL_TurnHWEffect (int idx, const vec3_t origin, const vec3_t velocity)
{ (void)idx; (void)origin; (void)velocity; turn_calls++; }

static int multi_calls;
static void CL_MultiHWEffect (const vec3_t origin, const vec3_t velocity,
		const int *slots)
{ (void)origin; (void)velocity; (void)slots; multi_calls++; }

static int dprintf_calls, printf_calls;
static void Con_DPrintf (const char *fmt, ...) { (void)fmt; dprintf_calls++; }
static void Con_Printf (const char *fmt, ...) { (void)fmt; printf_calls++; }

#include "../engine/hexen2/cl_hw_effects.inc"

/* ------------------------------------------------------------------ */
/* harness                                                             */
/* ------------------------------------------------------------------ */

#define SENTINEL	0xAB

static int failures, checks;

static void reset (void)
{
	memset (buf, 0, sizeof(buf));
	buf_len = 0;
	msg_readcount = 0;
	msg_badread = false;
}

static void put_byte (int b) { buf[buf_len++] = (unsigned char)b; }
static void put_short (int s) { put_byte (s & 255); put_byte ((s >> 8) & 255); }
static void put_bytes (int n, int v) { while (n-- > 0) put_byte (v); }

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

/* The reader must accept, stay in bounds, and stop on the sentinel. */
static void run_exact (qboolean ok, int type, const char *what)
{
	expect (ok, what, type);
	expect (!msg_badread, "no bad read", type);
	expect (msg_readcount == buf_len - 1 && buf[msg_readcount] == SENTINEL,
			"stopped exactly on the sentinel", type);
}

/* ------------------------------------------------------------------ */
/* svc_start_effect                                                     */
/* ------------------------------------------------------------------ */

/* Payload sizes after the index and type bytes, transcribed from
 * SV_SendEffect (engine/hexenworld/server/sv_effect.c).  -1 marks the two
 * bolt-loop types, which are driven separately below. */
/* Payload sizes in bytes after the index and type bytes, read off the
 * payload switch of SV_SendEffect (engine/hexenworld/server/sv_effect.c),
 * with the type numbers from engine/hexenworld/shared/effects.h.  On the
 * wire a coord and a short are 2 bytes, an angle or byte 1, a float 4.
 * The two bolt-loop types are data-dependent and are driven separately in
 * test_start_bolts below. */
static const struct { int type, size; const char *name; } start_spec[] =
{
	/* CE_RAIN: 12 coords, 2 shorts, 1 float */
	{  1, 32, "CE_RAIN" },
	/* CE_FOUNTAIN: pos, 3 angles, movedir, color short, cnt byte */
	{  2, 18, "CE_FOUNTAIN" },
	/* CE_QUAKE: origin, radius float */
	{  3, 10, "CE_QUAKE" },
	/* smoke: origin, velocity[3] float, framelength float */
	{  4, 22, "CE_WHITE_SMOKE" },
	{ 20, 22, "CE_GREEN_SMOKE" },
	{ 21, 22, "CE_GREY_SMOKE" },
	{ 22, 22, "CE_RED_SMOKE" },
	{ 23, 22, "CE_SLOW_WHITE_SMOKE" },
	{ 26, 22, "CE_TELESMK1" },
	{ 27, 22, "CE_TELESMK2" },
	{ 35, 22, "CE_GHOST" },
	{ 37, 22, "CE_REDCLOUD" },
	{ 74, 22, "CE_FLAMESTREAM" },
	{ 58, 22, "CE_ACID_MUZZFL" },
	{ 70, 22, "CE_FLAMEWALL" },
	{ 71, 22, "CE_FLAMEWALL2" },
	{ 73, 22, "CE_ONFIRE" },
	{ 56, 22, "CE_RIPPLE" },
	/* origin only */
	{  9,  6, "CE_SM_WHITE_FLASH" },
	{ 11,  6, "CE_YELLOWRED_FLASH" },
	{  5,  6, "CE_BLUESPARK" },
	{  6,  6, "CE_YELLOWSPARK" },
	{  7,  6, "CE_SM_CIRCLE_EXP" },
	{  8,  6, "CE_BG_CIRCLE_EXP" },
	{ 15,  6, "CE_SM_EXPLOSION" },
	{ 50,  6, "CE_SM_EXPLOSION2" },
	{ 16,  6, "CE_LG_EXPLOSION" },
	{ 17,  6, "CE_FLOOR_EXPLOSION" },
	{ 19,  6, "CE_BLUE_EXPLOSION" },
	{ 24,  6, "CE_REDSPARK" },
	{ 25,  6, "CE_GREENSPARK" },
	{ 28,  6, "CE_ICEHIT" },
	{ 29,  6, "CE_MEDUSA_HIT" },
	{ 30,  6, "CE_MEZZO_REFLECT" },
	{ 31,  6, "CE_FLOOR_EXPLOSION2" },
	{ 32,  6, "CE_XBOW_EXPLOSION" },
	{ 33,  6, "CE_NEW_EXPLOSION" },
	{ 34,  6, "CE_MAGIC_MISSILE_EXPLOSION" },
	{ 36,  6, "CE_BONE_EXPLOSION" },
	{ 57,  6, "CE_BLDRN_EXPL" },
	{ 59,  6, "CE_ACID_HIT" },
	{ 64,  6, "CE_ACID_SPLAT" },
	{ 65,  6, "CE_ACID_EXPL" },
	{ 63,  6, "CE_LBALL_EXPL" },
	{ 60,  6, "CE_FIREWALL_SMALL" },
	{ 61,  6, "CE_FIREWALL_MEDIUM" },
	{ 62,  6, "CE_FIREWALL_LARGE" },
	{ 66,  6, "CE_FBOOM" },
	{ 67,  6, "CE_BOMB" },
	{ 68,  6, "CE_BRN_BOUNCE" },
	{ 69,  6, "CE_LSHOCK" },
	/* flash: origin only */
	{ 10,  6, "CE_WHITE_FLASH" },
	{ 12,  6, "CE_BLUE_FLASH" },
	{ 13,  6, "CE_SM_BLUE_FLASH" },
	{ 51,  6, "CE_HWSPLITFLASH" },
	{ 14,  6, "CE_RED_FLASH" },
	/* origin only */
	{ 18,  6, "CE_RIDER_DEATH" },
	{ 38,  6, "CE_TELEPORTERPUFFS" },
	/* origin, velocity[0] float[3], skinnum float */
	{ 39, 22, "CE_TELEPORTERBODY" },
	/* origin, velocity, angle, avelocity -- 9 floats */
	{ 41, 42, "CE_BONESHRAPNEL" },
	{ 46, 42, "CE_HWBONEBALL" },
	/* origin, velocity float[3] */
	{ 40, 18, "CE_BONESHARD" },
	{ 47, 18, "CE_HWRAVENSTAFF" },
	{ 42, 18, "CE_HWMISSILESTAR" },
	{ 43, 18, "CE_HWEIDOLONSTAR" },
	{ 53, 18, "CE_HWRAVENPOWER" },
	/* origin, 2 angles, speed short */
	{ 54, 10, "CE_HWDRILLA" },
	/* owner short, 3 offset bytes, count byte */
	{ 55,  6, "CE_DEATHBUBBLES" },
	/* origin, owner+material short, tag byte */
	{ 49,  9, "CE_SCARABCHAIN" },
	/* origin, velocity float[3] */
	{ 48, 18, "CE_TRIPMINESTILL" },
	{ 45, 18, "CE_TRIPMINE" },
};

static void test_start_fixed (void)
{
	size_t i;

	for (i = 0; i < sizeof(start_spec) / sizeof(start_spec[0]); i++)
	{
		reset ();
		put_byte (3);				/* effect slot index */
		put_byte (start_spec[i].type);
		put_bytes (start_spec[i].size, 1);
		put_byte (SENTINEL);
		run_exact (HWCL_ParseStartEffect (), start_spec[i].type,
				start_spec[i].name);
	}
}

/* CE_HWSHEEPINATOR (44) and CE_HWXBOWSHOOT (52): after the header, one
 * 8-byte block (3 coords + 2 angles) per set bit in the low 5 of
 * turnedbolts. */
static void test_start_bolts (void)
{
	int turned, extra_header;
	int type, k, n;

	for (k = 0; k < 2; k++)
	{
		type = k ? 52 : 44;
		/* CE_HWXBOWSHOOT writes bolts and randseed first */
		extra_header = k ? 2 : 0;
		for (turned = 0; turned < 32; turned++)
		{
			reset ();
			put_byte (3);
			put_byte (type);
			put_bytes (6, 1);		/* origin[5] */
			put_bytes (2, 1);		/* angle[0..1] */
			put_bytes (extra_header, 1);
			put_byte (turned);		/* turnedbolts */
			put_byte (0x1f);		/* activebolts */
			for (n = 0; n < 5; n++)
				if (turned & (1 << n))
					put_bytes (8, 1);
			put_byte (SENTINEL);
			run_exact (HWCL_ParseStartEffect (), type,
					k ? "CE_HWXBOWSHOOT bolts" : "CE_HWSHEEPINATOR bolts");
		}
	}
}

/* A type SV_SendEffect never emits must not be guessed at: the reader has
 * to refuse rather than misread the rest of the packet. */
static void test_start_unknown (void)
{
	int before = printf_calls;

	reset ();
	put_byte (3);
	put_byte (200);				/* no such effect */
	put_bytes (8, 1);
	put_byte (SENTINEL);
	checks++;
	if (HWCL_ParseStartEffect ())
	{
		failures++;
		printf ("FAIL: start_effect accepted unknown type 200\n");
	}
	checks++;
	if (printf_calls == before)
	{
		failures++;
		printf ("FAIL: start_effect did not report unknown type 200\n");
	}
}

/* ------------------------------------------------------------------ */
/* svc_update_effect                                                    */
/* ------------------------------------------------------------------ */

static void test_update (void)
{
	int cmd, before;

	/* CE_SCARABCHAIN (49): one short. */
	reset ();
	put_byte (3); put_byte (49);
	put_short (17);
	put_byte (SENTINEL);
	run_exact (HWCL_ParseUpdateEffect (), 49, "update CE_SCARABCHAIN");

	/* CE_HWSHEEPINATOR (44) / CE_HWXBOWSHOOT (52) / CE_HWDRILLA (54):
	 * a command byte then a command-dependent tail. */
	for (cmd = 0; cmd < 256; cmd++)
	{
		int t, types[2];
		types[0] = 44; types[1] = 52;
		for (t = 0; t < 2; t++)
		{
			reset ();
			put_byte (3); put_byte (types[t]);
			put_byte (cmd);
			if (cmd & 1)
				put_bytes (2, 1);		/* one coord */
			else
			{
				put_bytes (2, 1);		/* two angles */
				if (cmd & 128)
					put_bytes (6, 1);	/* origin */
			}
			put_byte (SENTINEL);
			run_exact (HWCL_ParseUpdateEffect (), types[t],
					"update xbow/sheep command");
		}

		reset ();
		put_byte (3); put_byte (54);
		put_byte (cmd);
		if (cmd == 0)
		{
			put_bytes (6, 1);			/* origin */
			put_byte (1);			/* count */
		}
		else
		{
			put_bytes (2, 1);			/* two angles */
			put_bytes (6, 1);			/* origin */
		}
		put_byte (SENTINEL);
		run_exact (HWCL_ParseUpdateEffect (), 54, "update CE_HWDRILLA command");
	}

	/* Every other type: PF_updateeffect writes no payload at all, so the
	 * reader must consume nothing more and keep going.  This is the arm
	 * that used to drop the rest of the packet. */
	before = dprintf_calls;
	reset ();
	put_byte (3); put_byte (16);		/* CE_SM_EXPLOSION, no update payload */
	put_byte (SENTINEL);
	run_exact (HWCL_ParseUpdateEffect (), 16, "update with no payload");
	checks++;
	if (dprintf_calls == before)
	{
		failures++;
		printf ("FAIL: update_effect did not report payload-less type 16\n");
	}

	/* ...including a type number that is not an effect at all. */
	reset ();
	put_byte (3); put_byte (200);
	put_byte (SENTINEL);
	run_exact (HWCL_ParseUpdateEffect (), 200, "update with unknown type");

	/* The report fires once per type, not once per packet. */
	before = dprintf_calls;
	reset ();
	put_byte (3); put_byte (16);
	put_byte (SENTINEL);
	(void) HWCL_ParseUpdateEffect ();
	checks++;
	if (dprintf_calls != before)
	{
		failures++;
		printf ("FAIL: update_effect re-reported a type it had already reported\n");
	}
}

/* ------------------------------------------------------------------ */
/* svc_turn_effect and svc_multieffect                                  */
/* ------------------------------------------------------------------ */

static void test_turn (void)
{
	/* PF_turneffect: index, sv.time float, origin, direction. */
	reset ();
	put_byte (3);
	put_bytes (4, 1);		/* sv.time */
	put_bytes (6, 1);		/* origin */
	put_bytes (6, 1);		/* direction */
	put_byte (SENTINEL);
	run_exact (HWCL_ParseTurnEffect (), 62, "turn_effect");
}

static void test_multi (void)
{
	int before;

	/* SV_ParseMultiEffect: type, origin, velocity, three slot indices. */
	reset ();
	put_byte (53);			/* CE_HWRAVENPOWER */
	put_bytes (6, 1);		/* origin */
	put_bytes (6, 1);		/* velocity */
	put_bytes (3, 7);		/* three effect slots */
	put_byte (SENTINEL);
	run_exact (HWCL_ParseMultiEffect (), 53, "multieffect CE_HWRAVENPOWER");

	/* Any other type is unreachable from a real server and has no knowable
	 * length, so the reader must refuse loudly rather than guess. */
	before = printf_calls;
	reset ();
	put_byte (16);
	put_bytes (8, 1);
	put_byte (SENTINEL);
	checks++;
	if (HWCL_ParseMultiEffect ())
	{
		failures++;
		printf ("FAIL: multieffect accepted a non-CE_HWRAVENPOWER type\n");
	}
	checks++;
	if (printf_calls == before)
	{
		failures++;
		printf ("FAIL: multieffect did not report the bad type\n");
	}
}

/* ------------------------------------------------------------------ */
/* truncation: a short packet must be refused, never over-read          */
/* ------------------------------------------------------------------ */

static void test_truncation (void)
{
	size_t i;
	int n;

	for (i = 0; i < sizeof(start_spec) / sizeof(start_spec[0]); i++)
	{
		for (n = 0; n < start_spec[i].size; n++)
		{
			reset ();
			put_byte (3);
			put_byte (start_spec[i].type);
			put_bytes (n, 1);	/* one or more bytes short */
			(void) HWCL_ParseStartEffect ();
			checks++;
			if (msg_readcount > buf_len)
			{
				failures++;
				printf ("FAIL: %s read past the end of a truncated packet\n",
						start_spec[i].name);
			}
		}
	}
}

int main (void)
{
	test_start_fixed ();
	test_start_bolts ();
	test_start_unknown ();
	test_update ();
	test_turn ();
	test_multi ();
	test_truncation ();

	if (failures)
	{
		printf ("FAILED: %d of %d checks\n", failures, checks);
		return 1;
	}
	printf ("PASS: %d checks, 0 failed\n", checks);
	return 0;
}
