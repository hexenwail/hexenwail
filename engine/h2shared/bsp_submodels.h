/*
 * bsp_submodels.h -- LUMP_MODELS in either Hexen II's or Quake's layout
 *
 * Every BSP29/BSP2 struct in common/bspfile.h is byte-identical to Quake's
 * except one: Hexen II's dmodel_t carries MAX_MAP_HULLS (8) clip-hull
 * headnodes, 80 bytes a record, where Quake's carries 4, 64 bytes.  That
 * stride is the only thing that stopped a Quake map loading here
 * ("Mod_LoadSubmodels: funny lump size", GitHub #169), so the three model
 * loaders (gl_model.c, model.c, sv_model.c) ask this header which layout the
 * lump is in and read it through one reader.
 *
 * Header-only and engine-free, decoding little-endian bytes by hand, so
 * engine/tests/bsp_submodels_test.c can drive it without the engine.
 */
#ifndef H2_BSP_SUBMODELS_H
#define H2_BSP_SUBMODELS_H

#include <string.h>
#include "q_stdinc.h"
#include "bspfile.h"

#define BSP_SUBMODEL_STRIDE_H2	80	/* sizeof(dmodel_t): 8 headnodes */
#define BSP_SUBMODEL_STRIDE_Q1	64	/* Quake: 4 headnodes */

/* The counts a submodel record is checked against; all loaded before
 * LUMP_MODELS in every loader. */
typedef struct
{
	int	numfaces;
	int	numleafs;
	int	numnodes;
	int	numclipnodes;
} bsp_submodel_limits_t;

static inline int BSP_ReadLE32 (const unsigned char *p)
{
	return (int)((unsigned int)p[0] | ((unsigned int)p[1] << 8) |
		     ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24));
}

static inline float BSP_ReadLEFloat (const unsigned char *p)
{
	unsigned int	u = (unsigned int)BSP_ReadLE32 (p);
	float		f;

	memcpy (&f, &u, sizeof(f));
	return f;
}

/* Hulls a layout actually builds: Hexen II's Mod_MakeHulls fills 1-5; Quake's
 * qbsp writes 1 (player) and 2 (shambler) and leaves 3 zero. */
static inline int BSP_SubmodelClipHulls (int stride)
{
	return (stride == BSP_SUBMODEL_STRIDE_Q1) ? 2 : 5;
}

/*
 * Could `rec` be a submodel in this layout, given the map's own counts?  A
 * record read at the wrong stride lands its integer fields on float bits or
 * on the neighbouring record, which is what these ranges catch.
 */
static inline int BSP_SubmodelPlausible (const unsigned char *rec, int stride,
					 const bsp_submodel_limits_t *lim)
{
	const int	nheads = (stride == BSP_SUBMODEL_STRIDE_Q1) ? 4 : MAX_MAP_HULLS;
	const unsigned char	*tail = rec + 36 + 4 * nheads;
	int	j, h, visleafs, firstface, numfaces;

	for (j = 0; j < 9; j++)
	{
		float	v = BSP_ReadLEFloat (rec + 4 * j);

		if (!(v > -1.0e9f && v < 1.0e9f))	/* also rejects NaN */
			return 0;
	}

	h = BSP_ReadLE32 (rec + 36);			/* hull 0: a node */
	if (h < -1 - lim->numleafs || h >= lim->numnodes)
		return 0;
	for (j = 1; j <= BSP_SubmodelClipHulls (stride); j++)
	{
		h = BSP_ReadLE32 (rec + 36 + 4 * j);	/* a clipnode or contents */
		if (h < -15 || h >= lim->numclipnodes)
			return 0;
	}

	visleafs = BSP_ReadLE32 (tail);
	firstface = BSP_ReadLE32 (tail + 4);
	numfaces = BSP_ReadLE32 (tail + 8);
	if (visleafs < 0 || visleafs > lim->numleafs)
		return 0;
	if (firstface < 0 || numfaces < 0 || numfaces > lim->numfaces - firstface)
		return 0;
	return 1;
}

static inline int BSP_SubmodelsPlausible (const unsigned char *base, int filelen,
					  int stride, const bsp_submodel_limits_t *lim)
{
	int	i;

	if (filelen <= 0 || filelen % stride)
		return 0;
	for (i = 0; i < filelen / stride; i++)
	{
		if (!BSP_SubmodelPlausible (base + i * stride, stride, lim))
			return 0;
	}
	return 1;
}

/*
 * The record stride of a LUMP_MODELS lump, or 0 if it divides by neither.
 *
 * Regression-safe by construction: a lump only Hexen II's stride divides
 * loads exactly as it always has, unvalidated.  Only a lump that divides by
 * both (a multiple of 320 bytes) is decided on content, Hexen II winning
 * whenever its reading is plausible -- and also when neither is, which is
 * what the loader did before.
 */
static inline int BSP_SubmodelStride (const unsigned char *base, int filelen,
				      const bsp_submodel_limits_t *lim)
{
	const int	h2 = (filelen % BSP_SUBMODEL_STRIDE_H2) == 0;
	const int	q1 = (filelen % BSP_SUBMODEL_STRIDE_Q1) == 0;

	if (h2 && !q1)
		return BSP_SUBMODEL_STRIDE_H2;
	if (q1 && !h2)
		return BSP_SUBMODEL_STRIDE_Q1;
	if (!h2)
		return 0;
	if (!BSP_SubmodelsPlausible (base, filelen, BSP_SUBMODEL_STRIDE_H2, lim) &&
	     BSP_SubmodelsPlausible (base, filelen, BSP_SUBMODEL_STRIDE_Q1, lim))
		return BSP_SUBMODEL_STRIDE_Q1;
	return BSP_SUBMODEL_STRIDE_H2;
}

/*
 * One record into Hexen II's dmodel_t, mins/maxs spread by a pixel as the
 * loaders always have.  A Quake record has no tree for Hexen II's hulls 2-7:
 * they take the player hull's (hull 1, whose box is the same in both games),
 * and the loader gives those hulls hull 1's clip box to match the tree.
 */
static inline void BSP_ReadSubmodel (const unsigned char *rec, int stride, dmodel_t *out)
{
	const int	nheads = (stride == BSP_SUBMODEL_STRIDE_Q1) ? 4 : MAX_MAP_HULLS;
	const unsigned char	*tail = rec + 36 + 4 * nheads;
	int	j;

	for (j = 0; j < 3; j++)
	{
		out->mins[j] = BSP_ReadLEFloat (rec + 4 * j) - 1;
		out->maxs[j] = BSP_ReadLEFloat (rec + 12 + 4 * j) + 1;
		out->origin[j] = BSP_ReadLEFloat (rec + 24 + 4 * j);
	}
	for (j = 0; j < MAX_MAP_HULLS; j++)
	{
		if (stride == BSP_SUBMODEL_STRIDE_Q1 && j >= 2)
			out->headnode[j] = out->headnode[1];
		else
			out->headnode[j] = BSP_ReadLE32 (rec + 36 + 4 * j);
	}
	out->visleafs = BSP_ReadLE32 (tail);
	out->firstface = BSP_ReadLE32 (tail + 4);
	out->numfaces = BSP_ReadLE32 (tail + 8);
}

#endif	/* H2_BSP_SUBMODELS_H */
