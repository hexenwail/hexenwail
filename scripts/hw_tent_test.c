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
 * Build and run:
 *     cc -Wall -Wextra -o /tmp/hwtenttest scripts/hw_tent_test.c && /tmp/hwtenttest
 * Exit status 0 on pass, 1 on any failed assertion.
 */

#include <stdio.h>
#include <string.h>

typedef int qboolean;
#define true	1
#define false	0
typedef float vec3_t[3];

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
static void Con_DPrintf (const char *fmt, ...) { (void)fmt; }

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

	printf ("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS",
			checks, failures);
	return failures ? 1 : 0;
}
