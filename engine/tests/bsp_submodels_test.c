/*
 * bsp_submodels_test.c -- LUMP_MODELS layout detection (GitHub #169)
 *
 * Hexen II's dmodel_t is 80 bytes (8 hull headnodes), Quake's 64 (4).  The
 * loader must read either, must never change how a lump that only Hexen II's
 * stride divides is read, and must settle a lump both strides divide (a
 * multiple of 320 bytes) on content.  Records are built byte by byte in
 * little-endian, so the test also holds the reader to the file format
 * rather than to the host's byte order.
 */
#include <stdio.h>
#include <string.h>

#include "bsp_submodels.h"

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { printf ("FAIL: %s\n", what); failures++; } } while (0)

static void PutLE32 (unsigned char *p, int v)
{
	unsigned int u = (unsigned int)v;
	p[0] = u & 0xff; p[1] = (u >> 8) & 0xff; p[2] = (u >> 16) & 0xff; p[3] = (u >> 24) & 0xff;
}

static void PutFloat (unsigned char *p, float f)
{
	unsigned int u;
	memcpy (&u, &f, sizeof(u));
	PutLE32 (p, (int)u);
}

/* One plausible record: bounds, headnode 0 = node `i`, clip hulls = `i`,
 * `i` visleafs, faces [i*10, i*10+10). */
static void PutRecord (unsigned char *rec, int stride, int i)
{
	const int nheads = (stride == BSP_SUBMODEL_STRIDE_Q1) ? 4 : 8;
	int j;

	for (j = 0; j < 3; j++)
	{
		PutFloat (rec + 4 * j, -64.0f - i);
		PutFloat (rec + 12 + 4 * j, 64.0f + i);
		PutFloat (rec + 24 + 4 * j, 0.0f);
	}
	for (j = 0; j < nheads; j++)
		PutLE32 (rec + 36 + 4 * j, j == 0 ? i : (stride == BSP_SUBMODEL_STRIDE_Q1 && j == 3 ? 0 : i + j));
	PutLE32 (rec + 36 + 4 * nheads, i);
	PutLE32 (rec + 40 + 4 * nheads, i * 10);
	PutLE32 (rec + 44 + 4 * nheads, 10);
}

int main (void)
{
	static unsigned char	lump[1024];
	const bsp_submodel_limits_t lim = { 100, 50, 40, 60 };
	dmodel_t	m;
	int	i;

	/* 4 Hexen II records = 320 bytes, which 64 divides too: content decides. */
	memset (lump, 0, sizeof(lump));
	for (i = 0; i < 4; i++)
		PutRecord (lump + 80 * i, 80, i);
	CHECK (BSP_SubmodelStride (lump, 320, &lim) == 80, "ambiguous lump of Hexen II records reads as Hexen II");

	/* 5 Quake records = 320 bytes, the same ambiguity the other way. */
	memset (lump, 0, sizeof(lump));
	for (i = 0; i < 5; i++)
		PutRecord (lump + 64 * i, 64, i);
	CHECK (BSP_SubmodelStride (lump, 320, &lim) == 64, "ambiguous lump of Quake records reads as Quake");

	/* Only one stride divides: no content check, as before #169. */
	CHECK (BSP_SubmodelStride (lump, 192, &lim) == 64, "3 Quake records (192 bytes) are Quake");
	memset (lump, 0xff, sizeof(lump));	/* garbage must not matter here */
	CHECK (BSP_SubmodelStride (lump, 240, &lim) == 80, "240 bytes stays Hexen II, unvalidated");
	CHECK (BSP_SubmodelStride (lump, 100, &lim) == 0, "a lump neither divides is refused");
	CHECK (BSP_SubmodelStride (lump, 0, &lim) == 80, "an empty lump keeps the old behaviour");

	/* Ambiguous and implausible both ways: the old reading, Hexen II. */
	CHECK (BSP_SubmodelStride (lump, 320, &lim) == 80, "ambiguous garbage falls back to Hexen II");

	/* A Quake record converts: hulls 2-7 take hull 1's tree, the tail lands. */
	memset (lump, 0, sizeof(lump));
	PutRecord (lump, 64, 3);
	BSP_ReadSubmodel (lump, 64, &m);
	CHECK (m.headnode[0] == 3 && m.headnode[1] == 4, "Quake hulls 0 and 1 read as written");
	for (i = 2; i < MAX_MAP_HULLS; i++)
		CHECK (m.headnode[i] == m.headnode[1], "Quake hulls 2-7 take the player hull's tree");
	CHECK (m.visleafs == 3 && m.firstface == 30 && m.numfaces == 10, "Quake visleafs / faces read from offset 52");
	CHECK (m.mins[0] == -68.0f && m.maxs[2] == 68.0f, "bounds spread by a pixel");

	/* A Hexen II record keeps all eight headnodes. */
	memset (lump, 0, sizeof(lump));
	PutRecord (lump, 80, 2);
	BSP_ReadSubmodel (lump, 80, &m);
	for (i = 1; i < MAX_MAP_HULLS; i++)
		CHECK (m.headnode[i] == 2 + i, "Hexen II headnodes read as written");
	CHECK (m.visleafs == 2 && m.firstface == 20 && m.numfaces == 10, "Hexen II visleafs / faces read from offset 68");

	if (failures)
		return 1;
	printf ("PASS bsp_submodels: Hexen II and Quake LUMP_MODELS layouts (#169)\n");
	return 0;
}
