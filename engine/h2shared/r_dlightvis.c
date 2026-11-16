/* r_dlightvis.c -- can a dynamic light see a surface or a model? (GitHub #97)
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Phase P2 of #97, for the CPU dynamic-light path: GLES/WebGL2, WEBSOFT, and
 * desktop with r_lightclusters 0.  Compiled into both the GL and the software
 * renderer, so it uses only what both model layers have (mnode_t, mleaf_t,
 * msurface_t, the edge and vertex arrays, Mod_PointInLeaf, Mod_LeafPVS) and
 * nothing of the server's.
 *
 * Under r_dlight_occlusion >= 1, R_MarkLights asks R_DlightVis_Surface
 * whether a world surface the light reaches and faces can be seen from the
 * light, and the CPU alias loops ask R_DlightVis_Entity about a model.  The
 * cascade, cheapest first:
 *
 *   one-sided  the light is not behind the face (R_MarkLights, P1: before us)
 *   PVS        some leaf holding the face is in the light leaf's PVS
 *   segments   one of 2n+1 samples on the face -- the n vertices, the n edge
 *              midpoints and the vertex average, each pulled a unit toward
 *              that average and lifted a unit off the face -- is reached by
 *              a segment from the light that crosses no solid or sky leaf of
 *              the world BSP.  First clear sample wins.
 *
 * The samples are tools/bsp_dlight_leak.py's, so r_dlightvis_dump can be
 * checked against that oracle face by face.
 *
 * A light's answers are cached per surface, and kept while the light's key is
 * the same and its origin within DV_SLOP of the one they were computed from,
 * so a stationary light pays for its traces once.  PVS and the cache are
 * per cl_dlights[] slot.
 *
 * Not judged (vanilla, lit): brush-entity surfaces, whose geometry moves with
 * the entity, so neither the world's leaves nor a per-light cache describe
 * them, and which are a handful of faces against the world's thousands; sky,
 * which takes no lightmap; and any light whose origin is in a solid or sky
 * leaf -- a projectile that has entered a wall, or a torch mounted a unit
 * inside its own -- from which every segment would be blocked.
 */

#include "quakedef.h"
#if defined(WEBSOFT)
#include "r_local.h"
#endif
#include "r_dlightvis.h"

/* how far a light may drift before its cached answers are recomputed */
#define	DV_SLOP		0.5f
/* How far a light reaches across a surface is measured in TEXELS
 * (R_MarkLights, R_AddDynamicLights): a texture scaled by k takes the light up
 * to k * radius world units away, and a luxel and its bilinear tap reach a
 * further 16 texels or so past the polygon.  The reach precheck allows
 * (radius + DV_REACH_SLOP) * k, k = dv_scale[] (never below 1). */
#define	DV_REACH_SLOP	32.0f

typedef struct
{
	vec3_t		origin;		/* what the cached answers were computed from */
	int		key;
	int		frame;		/* dv_frame it was last validated in */
	qboolean	valid;
	qboolean	novis;		/* origin in solid or sky: not judged */
} dvlight_t;

static qmodel_t		*dv_model;	/* the world the tables below describe */
static msurface_t	*dv_surf0;	/* its first world surface */
static int		dv_nsurf;	/* its number of world surfaces */
static vec3_t		*dv_samp;	/* per surface, 2n+1 samples, unlifted */
static int		*dv_sampfirst;	/* [dv_nsurf + 1] */
static float		*dv_bound;	/* per surface, sample-sphere radius about its last sample */
static float		*dv_scale;	/* per surface, world units per texel, at least 1 */
static int		*dv_leaffirst;	/* [dv_nsurf + 1], into dv_leaflist */
static int		*dv_leaflist;	/* leaf indices, 1 .. numleafs */
static int		dv_pvsrow;	/* bytes in one PVS row */
static byte		*dv_pvs;	/* [MAX_DLIGHTS][dv_pvsrow] */
static int		dv_words;	/* unsigned ints in one per-light surface bitset */
static unsigned int	*dv_tested;	/* [MAX_DLIGHTS][dv_words] */
static unsigned int	*dv_visible;	/* [MAX_DLIGHTS][dv_words] */
static dvlight_t	dv_lights[MAX_DLIGHTS];
static int		dv_frame;

static struct
{
	int	frames, rekeys, surf_queries, surf_tests, surf_occluded, traces;
	int	point_queries, point_occluded;
	double	seconds;
} dv_stats;
static qboolean		dv_timing;	/* r_dlightvis_stats has been asked: time the traces */

#define	DV_BIT_TEST(b,i)	((b)[(i) >> 5] & (1u << ((i) & 31)))
#define	DV_BIT_SET(b,i)		((b)[(i) >> 5] |= (1u << ((i) & 31)))


/*
=============
DV_Blocked

Does the segment p1 -> p2 cross a solid or sky leaf of the BSP under node?
The near side first, so the walk stops at the first wall.  A point on a plane
is on its front, as in tools/bsp_dlight_leak.py.
=============
*/
static qboolean DV_Blocked (mnode_t *node, const vec3_t p1, const vec3_t p2)
{
	mplane_t	*plane;
	float		t1, t2, frac;
	vec3_t		a, mid;
	int		side;

	VectorCopy (p1, a);
	while (node->contents >= 0)
	{
		plane = node->plane;
		if (plane->type < 3)
		{
			t1 = a[plane->type] - plane->dist;
			t2 = p2[plane->type] - plane->dist;
		}
		else
		{
			t1 = DotProduct (a, plane->normal) - plane->dist;
			t2 = DotProduct (p2, plane->normal) - plane->dist;
		}
		if (t1 >= 0 && t2 >= 0)
			node = node->children[0];
		else if (t1 < 0 && t2 < 0)
			node = node->children[1];
		else
		{
			side = (t1 < 0);
			frac = t1 / (t1 - t2);
			mid[0] = a[0] + frac * (p2[0] - a[0]);
			mid[1] = a[1] + frac * (p2[1] - a[1]);
			mid[2] = a[2] + frac * (p2[2] - a[2]);
			if (DV_Blocked (node->children[side], a, mid))
				return true;
			node = node->children[side ^ 1];
			VectorCopy (mid, a);
		}
	}
	return node->contents == CONTENTS_SOLID || node->contents == CONTENTS_SKY;
}

/* Is any leaf holding world surface idx in this PVS row?  A surface no leaf
 * holds cannot be judged by PVS, so it passes. */
static qboolean DV_InPVS (const byte *pvs, int idx)
{
	int	k, leaf;

	if (dv_leaffirst[idx] == dv_leaffirst[idx + 1])
		return true;
	for (k = dv_leaffirst[idx]; k < dv_leaffirst[idx + 1]; k++)
	{
		leaf = dv_leaflist[k] - 1;
		if (pvs[leaf >> 3] & (1 << (leaf & 7)))
			return true;
	}
	return false;
}

/* Sample k of world surface idx, lifted a unit off the face: off its front,
 * or for a liquid, which is lit from both sides, toward the light. */
static void DV_Sample (int idx, const msurface_t *surf, const vec3_t org, int k, vec3_t out)
{
	const float	*n = surf->plane->normal;
	float		lift = (surf->flags & SURF_PLANEBACK) ? -1.0f : 1.0f;

	if ((surf->flags & SURF_DRAWTURB) &&
	    lift * (DotProduct (org, n) - surf->plane->dist) < 0)
		lift = -lift;
	out[0] = dv_samp[dv_sampfirst[idx] + k][0] + lift * n[0];
	out[1] = dv_samp[dv_sampfirst[idx] + k][1] + lift * n[1];
	out[2] = dv_samp[dv_sampfirst[idx] + k][2] + lift * n[2];
}

/* The cascade below the one-sided test: PVS (pvs NULL: no PVS to use), then
 * the samples, first clear one wins. */
static qboolean DV_SeesSurface (const vec3_t org, const byte *pvs, int idx, const msurface_t *surf)
{
	int	k, n;
	vec3_t	p;

	if (pvs && !DV_InPVS (pvs, idx))
		return false;
	n = dv_sampfirst[idx + 1] - dv_sampfirst[idx];
	for (k = 0; k < n; k++)
	{
		DV_Sample (idx, surf, org, k, p);
		dv_stats.traces++;
		if (!DV_Blocked (cl.worldmodel->nodes, org, p))
			return true;
	}
	return false;
}

static qboolean DV_Ready (void)
{
	return dv_model && dv_model == cl.worldmodel;
}

/* Fill a PVS row for org; false if org is in solid or sky. */
static qboolean DV_LightPVS (const vec3_t org, byte *row)
{
	mleaf_t	*leaf = Mod_PointInLeaf ((float *) org, cl.worldmodel);
	int	n;

	if (leaf->contents == CONTENTS_SOLID || leaf->contents == CONTENTS_SKY)
		return false;
	/* Mod_LeafPVS's buffer is MAX_MAP_LEAFS/8 bytes, a byte short of a row
	 * when numleafs is within 7 of the limit: what it cannot hold is visible */
	n = q_min (dv_pvsrow, MAX_MAP_LEAFS / 8);
	memcpy (row, Mod_LeafPVS (leaf, cl.worldmodel), n);
	memset (row + n, 0xff, dv_pvsrow - n);
	return true;
}

/* cl_dlights[lnum]'s state, its cache dropped if it has moved or changed. */
static dvlight_t *DV_Light (int lnum)
{
	const dlight_t	*dl = &cl_dlights[lnum];
	dvlight_t	*L = &dv_lights[lnum];

	if (L->frame == dv_frame)
		return L;
	L->frame = dv_frame;
	if (L->valid && L->key == dl->key &&
	    fabs (dl->origin[0] - L->origin[0]) <= DV_SLOP &&
	    fabs (dl->origin[1] - L->origin[1]) <= DV_SLOP &&
	    fabs (dl->origin[2] - L->origin[2]) <= DV_SLOP)
		return L;
	VectorCopy (dl->origin, L->origin);
	L->key = dl->key;
	L->valid = true;
	L->novis = !DV_LightPVS (L->origin, dv_pvs + lnum * dv_pvsrow);
	memset (dv_tested + lnum * dv_words, 0, dv_words * sizeof(unsigned int));
	memset (dv_visible + lnum * dv_words, 0, dv_words * sizeof(unsigned int));
	dv_stats.rekeys++;
	return L;
}


/*
=============
R_DlightVis_Surface
=============
*/
qboolean R_DlightVis_Surface (int lnum, msurface_t *surf)
{
	dvlight_t	*L;
	unsigned int	*tested, *visible;
	const float	*c;
	vec3_t		d;
	int		idx;
	qboolean	vis;
	double		t0;

	if (!DV_Ready ())
		return true;
	idx = surf - dv_surf0;
	if (idx < 0 || idx >= dv_nsurf || (surf->flags & SURF_DRAWSKY))
		return true;
	L = DV_Light (lnum);
	if (L->novis)
		return true;
	/* out of reach (GL's R_MarkLights has said so already; the software one
	 * marks every surface of a node in reach of its plane): no luxel takes
	 * the light whatever is answered, so do not trace for it */
	c = dv_samp[dv_sampfirst[idx + 1] - 1];
	VectorSubtract (L->origin, c, d);
	if (sqrt (DotProduct (d, d)) - dv_bound[idx] >
	    (fabs (cl_dlights[lnum].radius) + DV_REACH_SLOP) * dv_scale[idx])
		return true;
	dv_stats.surf_queries++;
	tested = dv_tested + lnum * dv_words;
	visible = dv_visible + lnum * dv_words;
	if (DV_BIT_TEST (tested, idx))
		return DV_BIT_TEST (visible, idx) != 0;
	t0 = dv_timing ? Sys_DoubleTime () : 0;
	vis = DV_SeesSurface (L->origin, dv_pvs + lnum * dv_pvsrow, idx, surf);
	if (dv_timing)
		dv_stats.seconds += Sys_DoubleTime () - t0;
	dv_stats.surf_tests++;
	DV_BIT_SET (tested, idx);
	if (vis)
		DV_BIT_SET (visible, idx);
	else
		dv_stats.surf_occluded++;
	return vis;
}

/*
=============
R_DlightVis_Point
=============
*/
qboolean R_DlightVis_Point (int lnum, const vec3_t org)
{
	dvlight_t	*L;
	qboolean	vis;
	double		t0;

	if (!DV_Ready ())
		return true;
	L = DV_Light (lnum);
	if (L->novis)
		return true;
	t0 = dv_timing ? Sys_DoubleTime () : 0;
	dv_stats.traces++;
	vis = !DV_Blocked (cl.worldmodel->nodes, L->origin, org);
	if (dv_timing)
		dv_stats.seconds += Sys_DoubleTime () - t0;
	return vis;
}

/*
=============
R_DlightVis_Entity

The middle of the model's box and a point three eighths of its height above
and below: a model seen at its head or its feet over a ledge is lit.
=============
*/
qboolean R_DlightVis_Entity (int lnum, const entity_t *e)
{
	vec3_t	c, p;
	float	h;
	int	k;

	if (!e->model)
		return true;
	dv_stats.point_queries++;
	c[0] = e->origin[0] + 0.5f * (e->model->mins[0] + e->model->maxs[0]);
	c[1] = e->origin[1] + 0.5f * (e->model->mins[1] + e->model->maxs[1]);
	c[2] = e->origin[2] + 0.5f * (e->model->mins[2] + e->model->maxs[2]);
	h = 0.375f * (e->model->maxs[2] - e->model->mins[2]);
	for (k = -1; k <= 1; k++)
	{
		VectorCopy (c, p);
		p[2] += k * h;
		if (R_DlightVis_Point (lnum, p))
			return true;
	}
	dv_stats.point_occluded++;
	return false;
}


/*
=============
R_DlightVis_BeginFrame

Lights are revalidated on their first query in a frame, not here, so a light
the frustum culls costs nothing.
=============
*/
void R_DlightVis_BeginFrame (void)
{
	dv_frame++;
	dv_stats.frames++;
}


/*
=============
R_DlightVis_NewMap
=============
*/
static void DV_Free (void)
{
	free (dv_samp);		dv_samp = NULL;
	free (dv_sampfirst);	dv_sampfirst = NULL;
	free (dv_bound);	dv_bound = NULL;
	free (dv_scale);	dv_scale = NULL;
	free (dv_leaffirst);	dv_leaffirst = NULL;
	free (dv_leaflist);	dv_leaflist = NULL;
	free (dv_pvs);		dv_pvs = NULL;
	free (dv_tested);	dv_tested = NULL;
	free (dv_visible);	dv_visible = NULL;
	dv_model = NULL;
	dv_nsurf = 0;
}

void R_DlightVis_NewMap (void)
{
	qmodel_t	*m = cl.worldmodel;
	msurface_t	*surf;
	mleaf_t		*leaf;
	const float	*v;
	vec3_t		cen, d;
	float		len, r;
	int		i, j, k, n, e, total, nmark;
	int		*fill;

	DV_Free ();
	memset (dv_lights, 0, sizeof(dv_lights));
	if (!m || !m->nodes || m->nummodelsurfaces <= 0)
		return;

	dv_surf0 = m->surfaces + m->firstmodelsurface;
	dv_nsurf = m->nummodelsurfaces;
	dv_pvsrow = (m->numleafs + 7) >> 3;
	dv_words = (dv_nsurf + 31) >> 5;

	total = 0;
	for (i = 0; i < dv_nsurf; i++)
		total += 2 * dv_surf0[i].numedges + 1;
	nmark = 0;
	for (i = 1; i <= m->numleafs; i++)
		nmark += m->leafs[i].nummarksurfaces;

	dv_samp = (vec3_t *) malloc (total * sizeof(vec3_t));
	dv_sampfirst = (int *) malloc ((dv_nsurf + 1) * sizeof(int));
	dv_bound = (float *) malloc (dv_nsurf * sizeof(float));
	dv_scale = (float *) malloc (dv_nsurf * sizeof(float));
	dv_leaffirst = (int *) calloc (dv_nsurf + 1, sizeof(int));
	dv_leaflist = (int *) malloc ((nmark + 1) * sizeof(int));
	dv_pvs = (byte *) malloc (MAX_DLIGHTS * dv_pvsrow + 1);
	dv_tested = (unsigned int *) calloc (MAX_DLIGHTS * dv_words + 1, sizeof(unsigned int));
	dv_visible = (unsigned int *) calloc (MAX_DLIGHTS * dv_words + 1, sizeof(unsigned int));
	fill = (int *) malloc ((dv_nsurf + 1) * sizeof(int));
	if (!dv_samp || !dv_sampfirst || !dv_bound || !dv_scale || !dv_leaffirst || !dv_leaflist ||
	    !dv_pvs || !dv_tested || !dv_visible || !fill)
	{
		Con_Printf ("r_dlightvis: out of memory, dynamic-light visibility off for %s\n", m->name);
		free (fill);
		DV_Free ();
		return;
	}

	/* the samples: vertices, edge midpoints, the vertex average last */
	k = 0;
	for (i = 0; i < dv_nsurf; i++)
	{
		surf = &dv_surf0[i];
		n = surf->numedges;
		dv_sampfirst[i] = k;
		for (j = 0; j < n; j++)
		{
			e = m->surfedges[surf->firstedge + j];
			v = (e >= 0) ? m->vertexes[m->edges[e].v[0]].position
				     : m->vertexes[m->edges[-e].v[1]].position;
			VectorCopy (v, dv_samp[k + j]);
		}
		VectorClear (cen);
		for (j = 0; j < n; j++)
			VectorAdd (cen, dv_samp[k + j], cen);
		if (n)
			VectorScale (cen, 1.0f / n, cen);
		for (j = 0; j < n; j++)
		{
			const float *a = dv_samp[k + j], *b = dv_samp[k + (j + 1) % n];
			dv_samp[k + n + j][0] = 0.5f * (a[0] + b[0]);
			dv_samp[k + n + j][1] = 0.5f * (a[1] + b[1]);
			dv_samp[k + n + j][2] = 0.5f * (a[2] + b[2]);
		}
		VectorCopy (cen, dv_samp[k + 2 * n]);
		r = 0;
		for (j = 0; j < 2 * n; j++)
		{
			VectorSubtract (cen, dv_samp[k + j], d);
			len = sqrt (DotProduct (d, d));
			if (len > r)
				r = len;
			if (len > 1.0f)
				VectorMA (dv_samp[k + j], 1.0f / len, d, dv_samp[k + j]);
		}
		dv_bound[i] = r;
		dv_scale[i] = 1.0f;
		for (j = 0; j < 2; j++)
		{
			len = sqrt (DotProduct (surf->texinfo->vecs[j], surf->texinfo->vecs[j]));
			if (len > 0 && 1.0f / len > dv_scale[i])
				dv_scale[i] = 1.0f / len;
		}
		k += 2 * n + 1;
	}
	dv_sampfirst[dv_nsurf] = k;

	/* surface -> the leaves that hold it, inverting marksurfaces */
	for (i = 1; i <= m->numleafs; i++)
	{
		leaf = &m->leafs[i];
		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			k = leaf->firstmarksurface[j] - dv_surf0;
			if (k >= 0 && k < dv_nsurf)
				dv_leaffirst[k + 1]++;
		}
	}
	for (i = 0; i < dv_nsurf; i++)
		dv_leaffirst[i + 1] += dv_leaffirst[i];
	memcpy (fill, dv_leaffirst, (dv_nsurf + 1) * sizeof(int));
	for (i = 1; i <= m->numleafs; i++)
	{
		leaf = &m->leafs[i];
		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			k = leaf->firstmarksurface[j] - dv_surf0;
			if (k >= 0 && k < dv_nsurf)
				dv_leaflist[fill[k]++] = i;
		}
	}
	free (fill);

	dv_model = m;
}


/*
=============
r_dlightvis_dump [x y z radius] [faces]

Per light -- every live dlight, or a probe at x y z with that radius -- how
the world surfaces it reaches (R_MarkLights' test) fare in the cascade:
behind (one-sided), outside the light's PVS, occluded (every sample blocked),
visible.  Sky and liquids are left out, as tools/bsp_dlight_leak.py leaves
them.  "front blocked" counts the faces in front whose samples are ALL blocked
whatever the PVS says, which is the oracle's "fully occluded"; "pvs-seen"
counts faces outside the PVS a sample of which a segment still reaches (vis's
conservatism says it should be none).  With "faces", a line per face:
index, class, blocked samples / samples.
=============
*/
static qboolean DV_Reaches (const vec3_t org, float radius, const msurface_t *surf, float *distp)
{
	float	dist, l, s, t, sc, tc;
	vec3_t	impact;
	int	j;

	dist = DotProduct (org, surf->plane->normal) - surf->plane->dist;
	*distp = dist;
	if (dist > radius || dist < -radius)
		return false;
	for (j = 0; j < 3; j++)
		impact[j] = org[j] - surf->plane->normal[j] * dist;
	l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
	sc = (int) (l + 0.5);
	if (sc < 0)
		sc = 0;
	else if (sc > surf->extents[0])
		sc = surf->extents[0];
	s = l - sc;
	l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
	tc = (int) (l + 0.5);
	if (tc < 0)
		tc = 0;
	else if (tc > surf->extents[1])
		tc = surf->extents[1];
	t = l - tc;
	return s*s + t*t + dist*dist < radius*radius;
}

static void DV_DumpLight (const char *what, const vec3_t org, float radius, qboolean faces, byte *row)
{
	const msurface_t	*surf;
	const char	*cls;
	float		dist;
	int		i, k, n, nblock;
	int		reached = 0, behind = 0, nopvs = 0, occluded = 0, visible = 0, fblocked = 0, pvsseen = 0;
	qboolean	inpvs, novis;
	mleaf_t		*leaf = Mod_PointInLeaf ((float *) org, cl.worldmodel);
	vec3_t		p;

	novis = !DV_LightPVS (org, row);
	radius = fabs (radius);
	for (i = 0; i < dv_nsurf; i++)
	{
		surf = &dv_surf0[i];
		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;
		if (!DV_Reaches (org, radius, surf, &dist))
			continue;
		reached++;
		n = dv_sampfirst[i + 1] - dv_sampfirst[i];
		if (((surf->flags & SURF_PLANEBACK) ? -dist : dist) < -DLIGHT_PLANE_EPSILON)
		{
			behind++;
			if (faces)
				Con_Printf ("  face %d behind 0/%d\n", (int)(surf - cl.worldmodel->surfaces), n);
			continue;
		}
		nblock = 0;
		for (k = 0; k < n; k++)
		{
			DV_Sample (i, surf, org, k, p);
			nblock += DV_Blocked (cl.worldmodel->nodes, org, p);
		}
		fblocked += (nblock == n);
		inpvs = novis || DV_InPVS (row, i);
		if (novis)
			cls = "visible", visible++;		/* not judged */
		else if (!inpvs)
		{
			cls = "nopvs", nopvs++;
			pvsseen += (nblock < n);
		}
		else if (nblock == n)
			cls = "occluded", occluded++;
		else
			cls = "visible", visible++;
		if (faces)
			Con_Printf ("  face %d %s %d/%d\n", (int)(surf - cl.worldmodel->surfaces), cls, nblock, n);
	}
	Con_Printf ("r_dlightvis %s org (%.1f %.1f %.1f) radius %.0f leaf %d%s: reached %d  behind %d  "
		    "outside PVS %d  occluded %d  visible %d  front blocked %d  pvs-seen %d\n",
		    what, org[0], org[1], org[2], radius, (int)(leaf - cl.worldmodel->leafs),
		    novis ? " (in solid: not judged)" : "", reached, behind, nopvs, occluded, visible,
		    fblocked, pvsseen);
}

static void R_DlightVis_Dump_f (void)
{
	int		i, argc = Cmd_Argc ();
	qboolean	faces = false;
	vec3_t		org;
	byte		*row;
	char		what[32];

	if (!DV_Ready ())
	{
		Con_Printf ("r_dlightvis_dump: no map\n");
		return;
	}
	if (argc > 1 && !strcmp (Cmd_Argv (argc - 1), "faces"))
	{
		faces = true;
		argc--;
	}
	if (argc != 1 && argc != 5)
	{
		Con_Printf ("usage: r_dlightvis_dump [x y z radius] [faces]\n");
		return;
	}
	row = (byte *) malloc (dv_pvsrow + 1);
	if (!row)
		return;
	Con_Printf ("--- r_dlightvis_dump (r_dlight_occlusion %d) ---\n", r_dlight_occlusion.integer);
	if (argc == 5)
	{
		for (i = 0; i < 3; i++)
			org[i] = atof (Cmd_Argv (i + 1));
		DV_DumpLight ("probe", org, atof (Cmd_Argv (4)), faces, row);
	}
	else
	{
		for (i = 0; i < MAX_DLIGHTS; i++)
		{
			if (cl_dlights[i].die < cl.time || !cl_dlights[i].radius)
				continue;
			q_snprintf (what, sizeof(what), "light %d key %d", i, cl_dlights[i].key);
			DV_DumpLight (what, cl_dlights[i].origin, cl_dlights[i].radius, faces, row);
		}
	}
	Con_Printf ("--- end r_dlightvis_dump ---\n");
	free (row);
}

/* r_dlightvis_stats: what the cascade has cost since the last call.  The
 * first call starts the clock: until somebody asks, the traces are not timed,
 * which on WebAssembly is two calls out to JavaScript per trace. */
static void R_DlightVis_Stats_f (void)
{
	int	f = dv_stats.frames ? dv_stats.frames : 1;

	if (!dv_timing)
		Con_Printf ("r_dlightvis_stats: timing from now on\n");
	dv_timing = true;

	Con_Printf ("r_dlightvis_stats: %d frames  %.4f ms/frame  rekeys %d  surface queries %d "
		    "(traced %d, occluded %d)  traces %d  model queries %d (occluded %d)\n",
		    dv_stats.frames, 1000.0 * dv_stats.seconds / f, dv_stats.rekeys,
		    dv_stats.surf_queries, dv_stats.surf_tests, dv_stats.surf_occluded,
		    dv_stats.traces, dv_stats.point_queries, dv_stats.point_occluded);
	memset (&dv_stats, 0, sizeof(dv_stats));
}

void R_DlightVis_Init (void)
{
	Cmd_AddCommand ("r_dlightvis_dump", R_DlightVis_Dump_f);
	Cmd_AddCommand ("r_dlightvis_stats", R_DlightVis_Stats_f);
}
