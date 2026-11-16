#!/usr/bin/env python3
"""A dynamic light does not light what it cannot see, on the CPU path (GitHub #97, P2).

P1 stopped a light lighting a face it is behind.  A face that does face the
light can still be hidden from it by a wall, and an alias model can be hidden
from it by anything.  engine/h2shared/r_dlightvis.c answers "can light lnum see
this?" for both renderers' CPU dynamic-light path (GLES/WebGL2, WEBSOFT, and
desktop with r_lightclusters 0):

  * R_DlightVis_Surface -- for a world surface R_MarkLights reaches and the
    light is not behind: PVS, then segments from the light to 2n+1 samples on
    the face (n vertices, n edge midpoints, the vertex average, each pulled a
    unit toward that average and lifted a unit off the face -- toward the light
    for a liquid), first clear sample wins.  The samples are
    tools/bsp_dlight_leak.py's, so the engine's r_dlightvis_dump can be checked
    against that oracle face by face.
  * R_DlightVis_Point / _Entity -- a segment to a point, and to three points
    up the middle of an alias model's box.
  * a per-light cache, kept while the light's key is the same and its origin
    within DV_SLOP of where the answers were computed.

The whole of r_dlightvis.c is compiled here, verbatim, against a stub
quakedef.h, and run on a synthetic BSP: two rooms either side of a wall slab
with a doorway in it, under a lintel, and a slanted (non-axial) corner.

  segments   DV_Blocked against the oracle's own Bsp.blocked, on 600 random
             segments and on endpoints that sit exactly on a plane, with the
             doorway empty and with it sky
  samples    every face's samples against the oracle's samples(), to 1e-4
             (one face's edges are stored reversed, so negative surfedges are
             walked too)
  cascade    a face behind the wall is occluded; one a sample of which the
             doorway shows is lit; a doorway of sky blocks; a PVS row without
             the face's leaf drops it untraced; a liquid face is lifted toward
             the light and a SURF_PLANEBACK face off its front; a light in
             solid, a sky face, a brush-entity face and a world that is not
             the one the tables were built for are not judged; a light out of
             reach is not traced, reach counted in texels (a 2x-scaled
             texture is judged twice as far out)
  cache      answers are reused (no new trace) while the light stays within
             DV_SLOP, recomputed past it, on a new key, and after
             R_DlightVis_NewMap; a moved light is revalidated on the next
             R_DlightVis_BeginFrame, not mid-frame
  models     R_DlightVis_Point; R_DlightVis_Entity lit by its lowest point
             under the lintel, dark when all three are behind it
  dump       r_dlightvis_dump's probe line and per-face classes on the fixture

Then the hooks are lifted out of the engine and run: R_MarkLights' surface
loop in gl_rlight.c and r_light.c (asks only for a world surface, after the
one-sided test, after GL's reach test, under r_dlight_occlusion >= 1; an
unseen surface is not marked), and the alias dlight loops -- both of
gl_rmain.c's and r_main.c's R_PrepareAlias (asked only for a light in range,
under the cvar, and on GL not while the GPU grid lights the world; GL's dark
lights and the software renderer's negative radius too).  And pinned: R_DlightVis_BeginFrame first thing in both R_PushDlights,
_NewMap in both R_NewMap, _Init in both R_Init, r_dlightvis.c in both
renderer source lists, and the view model's loops (which feed cl.light_level
to the game) left alone.

    python3 tools/test_dlight_vis.py
"""
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import bsp_dlight_leak as oracle  # noqa: E402

errors = []
VIS_C, VIS_H = "engine/h2shared/r_dlightvis.c", "engine/h2shared/r_dlightvis.h"
GL_RLIGHT, R_LIGHT = "engine/h2shared/gl_rlight.c", "engine/h2shared/r_light.c"
GL_RMAIN, SW_MAIN = "engine/hexen2/gl_rmain.c", "engine/hexen2/r_main.c"
GL_RMISC, CMAKE = "engine/hexen2/gl_rmisc.c", "engine/CMakeLists.txt"
MARK_SIG = "void R_MarkLights (dlight_t *light, int lnum, mnode_t *node, const entity_t *ent)"


def read(rel):
    p = ROOT / rel
    return p.read_text(errors="replace") if p.exists() else ""


def strip_comments(t):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", t, flags=re.S)


def braced(text, at):
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    return None


def body_of(text, head):
    m = re.search(head, text)
    if not m:
        return None
    return braced(text, m.end() - 1)


def define(rel, name):
    m = re.search(r"#define\s+%s\s+(\S+)" % name, read(rel))
    if not m:
        errors.append("%s: no #define %s" % (rel, name))
        return "0"
    return m.group(1)


# ------------------------------------------------------------------ fixture
# Planes (nx, ny, nz, dist, type); nodes (plane, child0, child1), a child < 0
# being leaf -child-1, as the oracle encodes them; leaves (contents, ...).
SOLID, EMPTY, SKY = -2, -1, -6
PLANES = [
    (1.0, 0.0, 0.0, 8.0, 0),        # 0: x = 8, the wall's room-A face
    (1.0, 0.0, 0.0, -8.0, 0),       # 1: x = -8, its room-B face
    (0.0, 1.0, 0.0, 32.0, 1),       # 2: y = 32, the doorway's edge
    (0.6, 0.8, 0.0, -100.0, 3),     # 3: non-axial: room B's slanted back corner
    (0.0, 0.0, 1.0, 50.0, 2),       # 4: z = 50, the lintel
    (1.0, 0.0, 0.0, -100.0, 0),     # 5: x = -100, room B's far wall (faces only)
]
NODES = [
    (0, -2, 1),      # 0: x >= 8 is room A (leaf 1)
    (1, 2, 3),       # 1: x >= -8 is the wall, below it room B
    (2, 4, -1),      # 2: in the wall, y >= 32 is the doorway column, else solid
    (3, -3, -1),     # 3: room B in front of the slant, solid behind it
    (4, -1, -4),     # 4: the doorway column: z >= 50 lintel (solid), below it the doorway (leaf 3)
]
LEAFS = [SOLID, EMPTY, EMPTY, EMPTY]       # 0 solid, 1 room A, 2 room B, 3 the doorway
NUMLEAFS = 3

# Faces: plane, side (SURF_PLANEBACK), flags, verts, the leaf that holds it,
# reversed (edges stored the other way round, walked by negative surfedges).
FACES = [
    dict(plane=5, back=0, flags=[], verts=[(-100, -40, 0), (-100, -20, 0), (-100, -20, 20), (-100, -40, 20)],
         leaf=2, rev=False),                                     # 0: behind the wall from room A
    dict(plane=5, back=0, flags=[], verts=[(-100, -20, 0), (-100, 200, 0), (-100, 200, 20), (-100, -20, 20)],
         leaf=2, rev=True),                                      # 1: its far end seen through the doorway
    dict(plane=0, back=1, flags=["SURF_DRAWTURB"], verts=[(8, -10, 0), (8, -10, 20), (8, 10, 20), (8, 10, 0)],
         leaf=1, rev=False),                                     # 2: a liquid facing into the wall
    dict(plane=1, back=1, flags=[], verts=[(-8, -10, 0), (-8, -10, 20), (-8, 10, 20), (-8, 10, 0)],
         leaf=2, rev=False),                                     # 3: the wall's room-B face (PLANEBACK)
    dict(plane=5, back=0, flags=["SURF_DRAWSKY"], verts=[(-100, -40, 0), (-100, -20, 0), (-100, -20, 20),
                                                        (-100, -40, 20)], leaf=2, rev=False),  # 4: sky
    dict(plane=5, back=0, flags=[], verts=[(-100, 150, 0), (-100, 200, 0), (-100, 200, 20), (-100, 150, 20)],
         leaf=None, rev=False),                                  # 5: no leaf holds it; seen through the doorway
    dict(plane=5, back=0, flags=[], verts=[(-100, -40, 0), (-100, -20, 0), (-100, -20, 20), (-100, -40, 20)],
         leaf=None, rev=False),                                  # 6: a brush entity's (past the world's)
]
NWORLD = 6
LIGHT_A, LIGHT_B = (50.0, 0.0, 10.0), (-50.0, 0.0, 10.0)


def oracle_bsp(doorway):
    """tools/bsp_dlight_leak.py's Bsp over the fixture, for its blocked()."""
    b = oracle.Bsp.__new__(oracle.Bsp)
    b.planes = [(p[0], p[1], p[2], p[3], p[4]) for p in PLANES]
    b.nodes = list(NODES)
    leafs = list(LEAFS)
    leafs[3] = doorway
    b.leafs = [(c, -1, 0, 0) for c in leafs]
    b.headnode = 0
    return b


def front(f):
    p = PLANES[f["plane"]]
    n = (p[0], p[1], p[2])
    return (-n[0], -n[1], -n[2]) if f["back"] else n


def segments():
    rnd = random.Random(97)
    segs = []
    for _ in range(600):
        a = (rnd.uniform(-150, 150), rnd.uniform(-60, 260), rnd.uniform(-20, 80))
        b = (rnd.uniform(-150, 150), rnd.uniform(-60, 260), rnd.uniform(-20, 80))
        segs.append((a, b))
    # endpoints exactly on a plane: a point on a plane is on its front
    segs += [((8.0, 0.0, 10.0), (50.0, 0.0, 10.0)), ((50.0, 0.0, 10.0), (8.0, 0.0, 10.0)),
             ((-8.0, 40.0, 10.0), (-50.0, 40.0, 10.0)), ((0.0, 32.0, 10.0), (0.0, 32.0, 10.0)),
             ((0.0, 40.0, 50.0), (0.0, 40.0, 50.0)), ((50.0, 40.0, 50.0), (-50.0, 40.0, 50.0)),
             ((50.0, 31.0, 10.0), (-50.0, 33.0, 10.0)), ((LIGHT_A), (LIGHT_A))]
    return segs


# ------------------------------------------------------------------ the C
STUB_H = r"""
#ifndef FAKE_QUAKEDEF_H
#define FAKE_QUAKEDEF_H
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef int qboolean;
#define true 1
#define false 0
typedef float vec_t;
typedef vec_t vec3_t[3];
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define VectorSubtract(a,b,c) ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorAdd(a,b,c) ((c)[0]=(a)[0]+(b)[0],(c)[1]=(a)[1]+(b)[1],(c)[2]=(a)[2]+(b)[2])
#define VectorCopy(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define VectorClear(a) ((a)[0]=(a)[1]=(a)[2]=0)
#define VectorScale(a,s,c) ((c)[0]=(a)[0]*(s),(c)[1]=(a)[1]*(s),(c)[2]=(a)[2]*(s))
#define VectorMA(a,s,b,c) ((c)[0]=(a)[0]+(s)*(b)[0],(c)[1]=(a)[1]+(s)*(b)[1],(c)[2]=(a)[2]+(s)*(b)[2])
%(consts)s
#define MAX_DLIGHTS 128
typedef struct mplane_s { vec3_t normal; float dist; byte type; } mplane_t;
typedef struct { float vecs[2][4]; } mtexinfo_t;
typedef struct { vec3_t position; } mvertex_t;
typedef struct { unsigned int v[2]; } medge_t;
typedef struct msurface_s { mplane_t *plane; int flags; int firstedge, numedges; mtexinfo_t *texinfo;
	int texturemins[2], extents[2]; } msurface_t;
typedef struct mnode_s { int contents; mplane_t *plane; struct mnode_s *children[2]; } mnode_t;
typedef struct mleaf_s { int contents; msurface_t **firstmarksurface; int nummarksurfaces; } mleaf_t;
typedef struct qmodel_s { char name[64]; mnode_t *nodes; mleaf_t *leafs; int numleafs;
	msurface_t *surfaces; int firstmodelsurface, nummodelsurfaces; int *surfedges; medge_t *edges;
	mvertex_t *vertexes; vec3_t mins, maxs; } qmodel_t;
typedef struct entity_s { vec3_t origin; qmodel_t *model; } entity_t;
typedef struct { vec3_t origin; float radius, die; int key; } dlight_t;
typedef struct { int integer; } cvar_t;
extern dlight_t cl_dlights[MAX_DLIGHTS];
extern struct client_s { qmodel_t *worldmodel; double time; } cl;
extern cvar_t r_dlight_occlusion;
mleaf_t *Mod_PointInLeaf (vec3_t p, qmodel_t *model);
byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model);
double Sys_DoubleTime (void);
void Con_Printf (const char *fmt, ...);
typedef void (*xcommand_t) (void);
void Cmd_AddCommand (const char *name, xcommand_t f);
int Cmd_Argc (void);
const char *Cmd_Argv (int i);
#define q_snprintf snprintf
#define q_min(a, b) (((a) < (b)) ? (a) : (b))
#define MAX_MAP_LEAFS 65535
#endif
"""

MAIN = r"""
#include "r_dlightvis.c"

dlight_t cl_dlights[MAX_DLIGHTS];
struct client_s cl;
cvar_t r_dlight_occlusion = {1};
static qmodel_t world, other;
static mplane_t planes[%(np)d];
static mnode_t nodes[%(nn)d];
static mleaf_t leafs[%(nl)d];
static msurface_t surfs[%(ns)d];
static msurface_t *marks[%(nm)d];
static mvertex_t verts[%(nv)d];
static medge_t edges[%(ne)d];
static int surfedges[%(nse)d];
static mtexinfo_t texinfo = {{{0, 1, 0, 0}, {0, 0, 1, 0}}};
static mtexinfo_t texinfo2 = {{{0, 0.5f, 0, 0}, {0, 0, 1, 0}}};	/* s scaled 2x: reaches twice as far along it */
static byte pvsrows[%(nl)d][1];
static byte novis[1] = {0xff};
static int ncmds;
static xcommand_t cmd_dump;

mleaf_t *Mod_PointInLeaf (vec3_t p, qmodel_t *model)
{
	mnode_t *node = model->nodes;
	while (node->contents >= 0)
		node = node->children[DotProduct (p, node->plane->normal) - node->plane->dist > 0 ? 0 : 1];
	return (mleaf_t *) node;
}
byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model)
{
	return leaf == model->leafs ? novis : pvsrows[leaf - model->leafs];
}
double Sys_DoubleTime (void) { return 0; }
void Con_Printf (const char *fmt, ...) { va_list a; va_start (a, fmt); vprintf (fmt, a); va_end (a); }
void Cmd_AddCommand (const char *name, xcommand_t f) { ncmds++; if (!strcmp (name, "r_dlightvis_dump")) cmd_dump = f; }
static int argc; static const char *argv[8];
int Cmd_Argc (void) { return argc; }
const char *Cmd_Argv (int i) { return i < argc ? argv[i] : ""; }

static mnode_t *child (int c) { return c >= 0 ? &nodes[c] : (mnode_t *) &leafs[-c - 1]; }

static void build (void)
{
	int i, k;
%(build)s
	world.nodes = nodes; world.leafs = leafs; world.numleafs = %(numleafs)d;
	world.surfaces = surfs; world.firstmodelsurface = 0; world.nummodelsurfaces = %(nworld)d;
	world.surfedges = surfedges; world.edges = edges; world.vertexes = verts;
	strcpy (world.name, "fixture");
	surfs[0].texinfo = &texinfo2;
	other = world;
	for (i = 0; i < %(nl)d; i++) pvsrows[i][0] = 0x07;
	(void)k;
}

static void light (int l, const float *o, float radius, int key)
{
	VectorCopy (o, cl_dlights[l].origin); cl_dlights[l].radius = radius;
	cl_dlights[l].die = 100; cl_dlights[l].key = key;
}

#define OUT(name, val) printf ("%%s %%d\n", name, (int)(val))

int main (void)
{
	static const float A[3] = {%(la)s}, B[3] = {%(lb)s};
	int i, k, n, t0;
	vec3_t p;

	build ();
	cl.worldmodel = &world; cl.time = 1;
	R_DlightVis_Init ();
	OUT ("cmds", ncmds);
	R_DlightVis_NewMap ();

	/* the segment walk, against the oracle */
%(segs)s
	leafs[3].contents = CONTENTS_SKY;
%(segs_sky)s
	leafs[3].contents = CONTENTS_EMPTY;

	/* the samples, lifted off the front (an origin well in front) */
	for (i = 0; i < %(ns)d && i < dv_nsurf; i++)
	{
		const msurface_t *s = &surfs[i];
		float sgn = (s->flags & SURF_PLANEBACK) ? -1 : 1;
		vec3_t far;
		for (k = 0; k < 3; k++) far[k] = dv_samp[dv_sampfirst[i + 1] - 1][k] + 1000 * sgn * s->plane->normal[k];
		n = dv_sampfirst[i + 1] - dv_sampfirst[i];
		for (k = 0; k < n; k++)
		{
			DV_Sample (i, s, far, k, p);
			printf ("sample %%d %%d %%.5f %%.5f %%.5f\n", i, k, p[0], p[1], p[2]);
		}
		for (k = dv_leaffirst[i]; k < dv_leaffirst[i + 1]; k++)
			printf ("leafof %%d %%d\n", i, dv_leaflist[k]);
	}

	/* the cascade */
	R_DlightVis_BeginFrame ();
	light (0, A, 400, 1);
	OUT ("A_f0", R_DlightVis_Surface (0, &surfs[0]));
	OUT ("A_f1", R_DlightVis_Surface (0, &surfs[1]));
	OUT ("A_f2_liquid", R_DlightVis_Surface (0, &surfs[2]));
	OUT ("A_f4_sky", R_DlightVis_Surface (0, &surfs[4]));
	OUT ("A_f5_noleaf", R_DlightVis_Surface (0, &surfs[5]));
	OUT ("A_f6_brushent", R_DlightVis_Surface (0, &surfs[6]));
	OUT ("A_tests", dv_stats.surf_tests);
	t0 = dv_stats.traces;
	OUT ("A_f0_again", R_DlightVis_Surface (0, &surfs[0]));
	OUT ("A_f1_again", R_DlightVis_Surface (0, &surfs[1]));
	OUT ("A_cached_traces", dv_stats.traces - t0);
	OUT ("A_tests_again", dv_stats.surf_tests);

	/* the doorway is bricked up: the cache keeps the old answer until the light moves past DV_SLOP */
	leafs[3].contents = CONTENTS_SOLID;
	OUT ("brick_samefame", R_DlightVis_Surface (0, &surfs[1]));
	cl_dlights[0].origin[0] += 0.8f;
	OUT ("brick_moved_midframe", R_DlightVis_Surface (0, &surfs[1]));
	cl_dlights[0].origin[0] -= 0.8f;
	R_DlightVis_BeginFrame ();
	OUT ("brick_back_home", R_DlightVis_Surface (0, &surfs[1]));
	cl_dlights[0].origin[0] += 0.4f;
	R_DlightVis_BeginFrame ();
	OUT ("brick_moved_0.4", R_DlightVis_Surface (0, &surfs[1]));
	cl_dlights[0].origin[0] += 0.4f;
	R_DlightVis_BeginFrame ();
	OUT ("brick_moved_0.8", R_DlightVis_Surface (0, &surfs[1]));
	leafs[3].contents = CONTENTS_EMPTY;
	R_DlightVis_BeginFrame ();
	OUT ("open_same_place", R_DlightVis_Surface (0, &surfs[1]));
	cl_dlights[0].key = 2;
	R_DlightVis_BeginFrame ();
	OUT ("open_new_key", R_DlightVis_Surface (0, &surfs[1]));
	leafs[3].contents = CONTENTS_SOLID;
	R_DlightVis_NewMap ();
	OUT ("newmap_lights_reset", dv_lights[0].valid);
	R_DlightVis_BeginFrame ();
	OUT ("brick_after_newmap", R_DlightVis_Surface (0, &surfs[1]));
	leafs[3].contents = CONTENTS_SKY;
	cl_dlights[0].key = 3;
	R_DlightVis_BeginFrame ();
	OUT ("sky_doorway", R_DlightVis_Surface (0, &surfs[1]));
	leafs[3].contents = CONTENTS_EMPTY;

	/* PVS: room A's row without room B drops f1 untraced */
	pvsrows[1][0] = 0x05;
	cl_dlights[0].key = 4;
	R_DlightVis_BeginFrame ();
	t0 = dv_stats.traces;
	OUT ("pvs_f1", R_DlightVis_Surface (0, &surfs[1]));
	OUT ("pvs_traces", dv_stats.traces - t0);
	pvsrows[1][0] = 0x07;

	/* light B, in room B: the PLANEBACK face of the wall faces it */
	R_DlightVis_BeginFrame ();
	light (1, B, 400, 9);
	OUT ("B_f3_planeback", R_DlightVis_Surface (1, &surfs[3]));
	OUT ("B_f0", R_DlightVis_Surface (1, &surfs[0]));

	/* a light inside the wall is not judged */
	{ static const float W[3] = {0, 0, 10}; light (2, W, 400, 5); }
	OUT ("solid_f0", R_DlightVis_Surface (2, &surfs[0]));
	OUT ("solid_point", R_DlightVis_Point (2, B));

	/* a light inside sky is not judged either */
	leafs[3].contents = CONTENTS_SKY;
	{ static const float S[3] = {0, 40, 10}; light (6, S, 400, 10); }
	OUT ("skylight_f0", R_DlightVis_Surface (6, &surfs[0]));
	leafs[3].contents = CONTENTS_EMPTY;

	/* out of reach: not traced */
	{ static const float F[3] = {50, -1000, 10}; light (3, F, 100, 6); }
	t0 = dv_stats.traces;
	OUT ("far_f0", R_DlightVis_Surface (3, &surfs[0]));
	OUT ("far_traces", dv_stats.traces - t0);
	/* ...but on a texture scaled 2x the light reaches twice as far in the
	 * plane, so 230 units away a radius of 100 still lights face 0 and it
	 * must be traced (and found behind the wall) */
	{ static const float K[3] = {50, -200, 10}; light (7, K, 100, 11); }
	t0 = dv_stats.traces;
	OUT ("scaled_f0", R_DlightVis_Surface (7, &surfs[0]));
	OUT ("scaled_traced", dv_stats.traces - t0 > 0);
	/* ...and a dark light's reach is its |radius| */
	{ static const float G[3] = {50, -60, 10}; light (4, G, -400, 7); }
	OUT ("dark_f0", R_DlightVis_Surface (4, &surfs[0]));

	/* a world the tables were not built for */
	cl.worldmodel = &other;
	OUT ("otherworld_f0", R_DlightVis_Surface (0, &surfs[0]));
	OUT ("otherworld_point", R_DlightVis_Point (0, B));
	cl.worldmodel = &world;

	/* points and models */
	{ static const float P1[3] = {-50, 200, 10}; vec3_t q; VectorCopy (P1, q);
	  OUT ("point_doorway", R_DlightVis_Point (0, q)); }
	OUT ("point_wall", R_DlightVis_Point (0, B));
	{
		static qmodel_t mdl; entity_t e;
		static const float L[3] = {50, 0, 60};
		R_DlightVis_BeginFrame ();
		light (5, L, 400, 8);
		mdl.mins[0] = mdl.mins[1] = -8; mdl.maxs[0] = mdl.maxs[1] = 8;
		mdl.mins[2] = -48; mdl.maxs[2] = 48;		/* points at z 24, 60, 96 */
		e.model = &mdl; e.origin[0] = -30; e.origin[1] = 100; e.origin[2] = 60;
		OUT ("model_tall", R_DlightVis_Entity (5, &e));
		mdl.mins[2] = -16; mdl.maxs[2] = 16;		/* points at 68, 80, 92 */
		e.origin[2] = 80;
		OUT ("model_high", R_DlightVis_Entity (5, &e));
		mdl.mins[2] = -16; mdl.maxs[2] = 16;		/* the same box from room A's side of the doorway */
		e.origin[0] = 30; e.origin[1] = 100;
		OUT ("model_sameroom", R_DlightVis_Entity (5, &e));
		e.model = NULL;
		OUT ("model_none", R_DlightVis_Entity (5, &e));
		OUT ("model_queries", dv_stats.point_queries);
		OUT ("model_occluded", dv_stats.point_occluded);
	}

	/* the dump: a probe from light A, faces */
	printf ("DUMP\n");
	argc = 6; argv[0] = "r_dlightvis_dump"; argv[1] = "50"; argv[2] = "0"; argv[3] = "10";
	argv[4] = "400"; argv[5] = "faces";
	if (cmd_dump) cmd_dump ();
	argc = 2; argv[1] = "1";
	if (cmd_dump) cmd_dump ();
	printf ("ENDDUMP\n");
	return 0;
}
"""


def c_fixture():
    consts = ["#define %s %s" % (n, define("engine/h2shared/gl_model.h", n))
              for n in ("SURF_PLANEBACK", "SURF_DRAWSKY", "SURF_DRAWTURB")]
    consts += ["#define %s %s" % (n, define("common/bspfile.h", n))
               for n in ("CONTENTS_EMPTY", "CONTENTS_SOLID", "CONTENTS_SKY")]
    consts.append("#define DLIGHT_PLANE_EPSILON %s" % define("engine/hexen2/client.h", "DLIGHT_PLANE_EPSILON"))
    b = []
    for i, p in enumerate(PLANES):
        b.append("\tplanes[%d].normal[0] = %r; planes[%d].normal[1] = %r; planes[%d].normal[2] = %r; "
                 "planes[%d].dist = %r; planes[%d].type = %d;" % (i, p[0], i, p[1], i, p[2], i, p[3], i, p[4]))
    for i, (pl, c0, c1) in enumerate(NODES):
        b.append("\tnodes[%d].contents = 0; nodes[%d].plane = &planes[%d]; nodes[%d].children[0] = child (%d); "
                 "nodes[%d].children[1] = child (%d);" % (i, i, pl, i, c0, i, c1))
    names = {SOLID: "CONTENTS_SOLID", EMPTY: "CONTENTS_EMPTY", SKY: "CONTENTS_SKY"}
    for i, c in enumerate(LEAFS):
        b.append("\tleafs[%d].contents = %s;" % (i, names[c]))
    verts, edges, surfedges = [], [(0, 0)], []
    nmark = 0
    marks_by_leaf = {}
    for fi, f in enumerate(FACES):
        first = len(surfedges)
        base = len(verts)
        verts += f["verts"]
        n = len(f["verts"])
        for k in range(n):
            a, c = base + k, base + (k + 1) % n
            if f["rev"]:
                edges.append((c, a))
                surfedges.append(-(len(edges) - 1))
            else:
                edges.append((a, c))
                surfedges.append(len(edges) - 1)
        flags = " | ".join(f["flags"] + (["SURF_PLANEBACK"] if f["back"] else [])) or "0"
        b.append("\tsurfs[%d].plane = &planes[%d]; surfs[%d].flags = %s; surfs[%d].firstedge = %d; "
                 "surfs[%d].numedges = %d; surfs[%d].texinfo = &texinfo; surfs[%d].texturemins[0] = "
                 "surfs[%d].texturemins[1] = -1000; surfs[%d].extents[0] = surfs[%d].extents[1] = 2000;"
                 % (fi, f["plane"], fi, flags, fi, first, fi, n, fi, fi, fi, fi, fi))
        if f["leaf"] is not None:
            marks_by_leaf.setdefault(f["leaf"], []).append(fi)
    for li in range(len(LEAFS)):
        ms = marks_by_leaf.get(li, [])
        b.append("\tleafs[%d].firstmarksurface = &marks[%d]; leafs[%d].nummarksurfaces = %d;"
                 % (li, nmark, li, len(ms)))
        for fi in ms:
            b.append("\tmarks[%d] = &surfs[%d];" % (nmark, fi))
            nmark += 1
    for i, v in enumerate(verts):
        b.append("\tverts[%d].position[0] = %r; verts[%d].position[1] = %r; verts[%d].position[2] = %r;"
                 % (i, float(v[0]), i, float(v[1]), i, float(v[2])))
    for i, e in enumerate(edges):
        b.append("\tedges[%d].v[0] = %d; edges[%d].v[1] = %d;" % (i, e[0], i, e[1]))
    for i, s in enumerate(surfedges):
        b.append("\tsurfedges[%d] = %d;" % (i, s))
    segs = segments()

    def segcode(tag):
        out = []
        for i, (a, c) in enumerate(segs):
            out.append("\t{ static const float a[3] = {%rf, %rf, %rf}, c[3] = {%rf, %rf, %rf}; "
                       "printf (\"%s %d %%d\\n\", (int) DV_Blocked (nodes, a, c)); }"
                       % (a[0], a[1], a[2], c[0], c[1], c[2], tag, i))
        return "\n".join(out)
    main = MAIN % dict(np=len(PLANES), nn=len(NODES), nl=len(LEAFS), ns=len(FACES), nm=max(nmark, 1),
                       nv=len(verts), ne=len(edges), nse=len(surfedges), build="\n".join(b),
                       numleafs=NUMLEAFS, nworld=NWORLD, la="%rf, %rf, %rf" % LIGHT_A,
                       lb="%rf, %rf, %rf" % LIGHT_B, segs=segcode("seg"), segs_sky=segcode("segsky"))
    return STUB_H % dict(consts="\n".join(consts)), main


def run_fixture():
    if not read(VIS_C):
        errors.append("%s is missing" % VIS_C)
        return None
    hdr, main = c_fixture()
    with tempfile.TemporaryDirectory() as work:
        inc = Path(work) / "inc"
        inc.mkdir()
        (inc / "quakedef.h").write_text(hdr)
        c, exe = Path(work) / "dlightvis_main.c", Path(work) / "dlightvis"
        c.write_text(main)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-I", str(inc),
                            "-I", str(ROOT / "engine/h2shared"), "-o", str(exe), str(c), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("r_dlightvis.c no longer compiles against the stub quakedef.h:\n" + r.stderr[-3000:])
            return None
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if r.returncode:
            errors.append("the fixture program failed (%d):\n%s" % (r.returncode, r.stderr[-2000:]))
            return None
        return r.stdout


WANT = {
    "cmds": 2,
    "A_f0": 0, "A_f1": 1, "A_f2_liquid": 1, "A_f4_sky": 1, "A_f5_noleaf": 1, "A_f6_brushent": 1, "A_tests": 4,
    "A_f0_again": 0, "A_f1_again": 1, "A_cached_traces": 0, "A_tests_again": 4,
    "brick_samefame": 1, "brick_moved_midframe": 1, "brick_back_home": 1, "brick_moved_0.4": 1,
    "brick_moved_0.8": 0, "open_same_place": 0, "open_new_key": 1, "newmap_lights_reset": 0,
    "brick_after_newmap": 0, "sky_doorway": 0, "skylight_f0": 1,
    "pvs_f1": 0, "pvs_traces": 0,
    "B_f3_planeback": 1, "B_f0": 1,
    "solid_f0": 1, "solid_point": 1,
    "far_f0": 1, "far_traces": 0, "scaled_f0": 0, "scaled_traced": 1, "dark_f0": 0,
    "otherworld_f0": 1, "otherworld_point": 1,
    "point_doorway": 1, "point_wall": 0,
    "model_tall": 1, "model_high": 0, "model_sameroom": 1, "model_none": 1,
    "model_queries": 3, "model_occluded": 1,
}
WHY = {
    "A_f0": "a face the wall hides from the light must be occluded",
    "A_f1": "a face one sample of which the doorway shows must be lit",
    "A_f2_liquid": "a liquid's samples are lifted toward the light (off its front they are inside the wall)",
    "A_f4_sky": "a sky face is not judged",
    "A_f5_noleaf": "a face no leaf holds cannot be judged by PVS, so goes on to the segments",
    "A_f6_brushent": "a brush entity's face is not judged",
    "A_cached_traces": "a stationary light's second query must be answered from the cache",
    "brick_samefame": "the cache must hold within a frame",
    "brick_moved_midframe": "a light is revalidated per frame, not per query",
    "brick_back_home": "a light back where its answers were computed keeps them",
    "newmap_lights_reset": "R_DlightVis_NewMap must forget every light",
    "skylight_f0": "a light inside sky is not judged",
    "brick_moved_0.4": "a light within DV_SLOP of its cached origin keeps its answers",
    "brick_moved_0.8": "a light past DV_SLOP must be recomputed",
    "open_same_place": "the cache must hold for a light that has not moved",
    "open_new_key": "a new key must drop the cache",
    "brick_after_newmap": "R_DlightVis_NewMap must drop every cache",
    "sky_doorway": "a sky leaf must block a segment",
    "pvs_f1": "a face no leaf of which is in the light's PVS must be dropped",
    "pvs_traces": "PVS must come before the segments",
    "B_f3_planeback": "a SURF_PLANEBACK face's samples are lifted off its front, which faces the other way",
    "solid_f0": "a light inside solid is not judged",
    "far_traces": "a light that cannot reach the face must not be traced for it",
    "scaled_f0": "reach is in texels: a scaled texture takes the light further, so it must be judged",
    "dark_f0": "a dark light (negative radius) reaches by |radius|",
    "otherworld_f0": "a world the tables were not built for is not judged",
    "point_wall": "a point behind the wall is not seen",
    "model_tall": "a model whose lowest point the doorway shows is lit",
    "model_high": "a model all three of whose points are behind the lintel is not lit",
}


def check_fixture(out):
    if out is None:
        return
    got, samples, leafof, seg, segsky = {}, {}, {}, {}, {}
    dump = out[out.find("DUMP\n") + 5:out.find("ENDDUMP")] if "DUMP\n" in out else ""
    for line in out[:out.find("DUMP\n")].split("\n"):
        w = line.split()
        if not w:
            continue
        if w[0] == "sample":
            samples[(int(w[1]), int(w[2]))] = tuple(map(float, w[3:6]))
        elif w[0] == "leafof":
            leafof.setdefault(int(w[1]), []).append(int(w[2]))
        elif w[0] == "seg":
            seg[int(w[1])] = int(w[2])
        elif w[0] == "segsky":
            segsky[int(w[1])] = int(w[2])
        else:
            got[w[0]] = int(w[1])
    for k, v in WANT.items():
        if got.get(k) != v:
            errors.append("%s: got %s, want %s%s" % (k, got.get(k), v, (" -- " + WHY[k]) if k in WHY else ""))
    # the segment walk against the oracle's
    segs = segments()
    for tag, res, doorway in (("doorway empty", seg, EMPTY), ("doorway sky", segsky, SKY)):
        bsp = oracle_bsp(doorway)
        bad = [i for i, (a, b) in enumerate(segs) if res.get(i) != int(bsp.blocked(a, b))]
        if bad:
            i = bad[0]
            errors.append("DV_Blocked disagrees with the oracle's Bsp.blocked on %d of %d segments (%s), "
                          "first %s -> %s: C %s, oracle %s" % (len(bad), len(segs), tag, segs[i][0], segs[i][1],
                                                              res.get(i), int(bsp.blocked(*segs[i]))))
        nb = sum(res.values())
        if not (0 < nb < len(segs)):
            errors.append("the segment fixture is degenerate (%s): %d of %d blocked" % (tag, nb, len(segs)))
    # the samples against the oracle's
    for fi, f in enumerate(FACES[:NWORLD]):
        want, _ = oracle.samples([tuple(map(float, v)) for v in f["verts"]], front(f))
        have = [samples.get((fi, k)) for k in range(len(want))]
        if samples.get((fi, len(want))) is not None:
            errors.append("face %d: more than 2n+1 samples" % fi)
        for k, (w, h) in enumerate(zip(want, have)):
            if h is None or max(abs(a - b) for a, b in zip(w, h)) > 1e-4:
                errors.append("face %d sample %d: C %s, oracle %s" % (fi, k, h, tuple(round(c, 4) for c in w)))
                break
        if sorted(leafof.get(fi, [])) != ([f["leaf"]] if f["leaf"] is not None else []):
            errors.append("face %d: held by leaves %s, want %s" % (fi, leafof.get(fi), f["leaf"]))
    if leafof.get(NWORLD) or len(FACES) != NWORLD + 1:
        errors.append("the brush-entity face got leaves: the tables must cover the world's surfaces only")
    # the dump
    m = re.search(r"r_dlightvis probe org \(50\.0 0\.0 10\.0\) radius 400 leaf 1: reached (\d+)  behind (\d+)  "
                  r"outside PVS (\d+)  occluded (\d+)  visible (\d+)  front blocked (\d+)  pvs-seen (\d+)", dump)
    if not m:
        errors.append("r_dlightvis_dump's probe line is missing or changed shape:\n" + dump[:600])
    else:
        # reached: 0, 1, 3, 5 (the liquid 2 and sky 4 are left out, 6 is not the world's);
        # 3 is behind (its front faces room B); 0 occluded; 1 and 5 visible
        want = dict(reached=4, behind=1, nopvs=0, occluded=1, visible=2, fblocked=1, pvsseen=0)
        have = dict(zip(want, map(int, m.groups())))
        if have != want:
            errors.append("r_dlightvis_dump's probe counts: %s, want %s" % (have, want))
    for face, cls in ((0, r"occluded 9/9"), (1, r"visible \d/9"), (3, r"behind 0/9")):
        if not re.search(r"^  face %d %s$" % (face, cls), dump, re.M):
            errors.append("r_dlightvis_dump faces: want 'face %d %s'" % (face, cls.replace("\\d", "N")))
    if re.search(r"^  face [246] ", dump, re.M):
        errors.append("r_dlightvis_dump must leave out liquids, sky and brush entities' faces")
    if "usage: r_dlightvis_dump" not in dump:
        errors.append("r_dlightvis_dump with one argument must print its usage")


# ------------------------------------------------------------------ the hooks, run
def live(text):
    m = re.search(r"#if 0\s*/\* the original version from ID \*/(.*?)#else", text, re.S)
    return text[:m.start()] + text[m.end():] if m else text


def mark_loop(rel):
    fn = body_of(live(read(rel)), r"\n%s\s*\{" % re.escape(MARK_SIG))
    if not fn:
        errors.append("%s: no %s" % (rel, MARK_SIG))
        return None
    a = fn.find("for (i = 0; i < node->numsurfaces; i++, surf++)")
    if a < 0:
        errors.append("%s: R_MarkLights has no surface loop" % rel)
        return None
    return braced(fn, a)


def gl_alias_loops():
    text = read(GL_RMAIN)
    head = "for (lnum = 0; !model_dlight_gpu && lnum < MAX_DLIGHTS; lnum++)"
    loops = [braced(text, m.start()) for m in re.finditer(re.escape(head), text)]
    if len(loops) != 2:
        errors.append("gl_rmain.c: want the two alias dlight loops (`%s`), found %d" % (head, len(loops)))
        return []
    return loops


def sw_alias_loop():
    fn = body_of(read(SW_MAIN), r"static void R_PrepareAlias \(void\)\s*\{")
    if not fn:
        errors.append("r_main.c: no R_PrepareAlias")
        return None
    m = re.search(r"for \(lnum = 0; lnum < MAX_DLIGHTS; lnum\+\+\)", fn)
    if not m:
        errors.append("r_main.c: R_PrepareAlias has no dlight loop")
        return None
    return braced(fn, m.start())


HOOK_PRELUDE = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef float vec_t;
typedef vec_t vec3_t[3];
typedef int qboolean;
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define VectorSubtract(a,b,c) ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorLengthFast(a) sqrt(DotProduct((a),(a)))
#define MAX_DLIGHTS 128
#define DLIGHTBITS_WORDS 4
#define DLIGHTBIT_TEST(b,i) ((b)[(i) >> 5] & (1u << ((i) & 31)))
#define DLIGHTBIT_SET(b,i) ((b)[(i) >> 5] |= (1u << ((i) & 31)))
#define DLIGHTBITS_CLEAR(b) memset ((b), 0, sizeof(unsigned int) * DLIGHTBITS_WORDS)
#define SURF_PLANEBACK 2
#define SURF_DRAWTURB 0x10
#define DLIGHT_PLANE_EPSILON 2.0f
typedef struct entity_s { vec3_t origin; } entity_t;
typedef struct { vec3_t normal; float dist; int type; } mplane_t;
typedef struct { float vecs[2][4]; } mtexinfo_t;
typedef struct { mplane_t *plane; int flags; mtexinfo_t *texinfo; int texturemins[2], extents[2];
                 int dlightframe; unsigned int dlightbits[4]; const entity_t *dlightent; } msurface_t;
typedef struct mnode_s { int numsurfaces; } mnode_t;
typedef struct { vec3_t origin; float radius, die; int dark; float color[4]; } dlight_t;
typedef struct { int integer; } cvar_t;
static cvar_t r_dlight_occlusion;
static dlight_t cl_dlights[MAX_DLIGHTS];
static struct { double time; } cl = {10.0};
static int r_dlightframecount = 7;
static mplane_t plane = {{0, 0, 1}, 0, 2};
static mtexinfo_t texinfo = {{{1, 0, 0, 0}, {0, 1, 0, 0}}};
static int stub_vis, vis_calls, vis_lnum; static const void *vis_what;
static int R_DlightVis_Surface (int lnum, msurface_t *surf) { vis_calls++; vis_lnum = lnum; vis_what = surf; return stub_vis; }
static int R_DlightVis_Entity (int lnum, const entity_t *e) { vis_calls++; vis_lnum = lnum; vis_what = e; return stub_vis; }
static int stub_gpuworld;
static int R_LightCluster_ShadesWorld (void) { return stub_gpuworld; }
"""

HOOK_MARK = r"""
static int mark_%(name)s (dlight_t *light, int lnum, float h, float lateral, const entity_t *ent, int *right)
{
	static msurface_t thesurf;
	mnode_t real_node = {1}, *node = &real_node;
	msurface_t *surf = &thesurf;
	float l, maxdist, dist; int i, j, s, t; vec3_t impact;
	(void)l; (void)j; (void)s; (void)t; (void)impact;
	memset (&thesurf, 0, sizeof(thesurf));
	thesurf.plane = &plane; thesurf.texinfo = &texinfo;
	thesurf.texturemins[0] = thesurf.texturemins[1] = -64; thesurf.extents[0] = thesurf.extents[1] = 128;
	light->origin[0] = lateral; light->origin[1] = 0; light->origin[2] = h;
	maxdist = light->radius * light->radius;
	dist = h;
%(body)s
	*right = vis_what == &thesurf && vis_lnum == lnum;
	return DLIGHTBIT_TEST (thesurf.dlightbits, lnum) != 0;
}
"""

HOOK_GL_ALIAS = r"""
static float gl_alias_%(n)d (entity_t *e, float *amb_out)
{
	int lnum, i; vec3_t dist; float add, ambientlight = *amb_out, lightcolor[3] = {0, 0, 0};
	qboolean model_dlight_gpu = 0;
	(void)i;
%(body)s
	*amb_out = ambientlight;
	return lightcolor[0];
}
"""
HOOK_SW_ALIAS = r"""
static int sw_alias (entity_t *currententity, int amb)
{
	int lnum; vec3_t dist; float add;
	struct { int ambientlight; } lighting;
	lighting.ambientlight = amb;
%(body)s
	return lighting.ambientlight;
}
"""


def run_hooks():
    marks = {"gl": mark_loop(GL_RLIGHT), "sw": mark_loop(R_LIGHT)}
    gls, sw = gl_alias_loops(), sw_alias_loop()
    if any(v is None for v in marks.values()) or sw is None or len(gls) != 2:
        return None
    prog = [HOOK_PRELUDE]
    for k, v in marks.items():
        # GL's luxel offsets are s and t; the harness's surface is `s` only inside the macro
        prog.append(HOOK_MARK % dict(name=k, body=v))
    for n, body in enumerate(gls):
        prog.append(HOOK_GL_ALIAS % dict(n=n, body=body))
    prog.append(HOOK_SW_ALIAS % dict(body=sw))
    calls = []
    # mark loops: name, cvar, vis, h (light height over the face), lateral, ent?
    for k in ("gl", "sw"):
        for name, cvar, vis, h, lat, ent in (
                ("world_unseen", 1, 0, 24, 0, 0), ("world_seen", 1, 1, 24, 0, 0),
                ("entity_unseen", 1, 0, 24, 0, 1), ("cvar0_unseen", 0, 0, 24, 0, 0),
                ("cvar2_unseen", 2, 0, 24, 0, 0), ("behind_seen", 1, 1, -24, 0, 0),
                ("unreached_unseen", 1, 0, 24, 1000, 0)):
            calls.append(r"""
	{ static entity_t E; int m, right = 0; vis_calls = 0; vis_lnum = -1; vis_what = 0; stub_vis = %d;
	  r_dlight_occlusion.integer = %d; cl_dlights[37].radius = 200; cl_dlights[37].die = 20;
	  m = mark_%s (&cl_dlights[37], 37, %d, %d, %s, &right);
	  printf ("mark %s %s %%d %%d %%d\n", m, vis_calls, right); }"""
                         % (vis, cvar, k, h, lat, "&E" if ent else "NULL", k, name))
    # alias loops: name, cvar, vis, distance, radius, dark, ambient before
    for name, cvar, vis, d, rad, dark, amb in (
            ("seen", 1, 1, 50, 200, 0, 0), ("unseen", 1, 0, 50, 200, 0, 0),
            ("cvar0_unseen", 0, 0, 50, 200, 0, 0), ("outofrange", 1, 1, 300, 200, 0, 0),
            ("dark_seen", 1, 1, 50, 200, 1, 200), ("dark_unseen", 1, 0, 50, 200, 1, 200),
            ("negradius_seen", 1, 1, 50, -200, 0, 0), ("negradius_unseen", 1, 0, 50, -200, 0, 0),
            ("gpuworld_unseen", 1, 0, 50, 200, 0, 0)):
        setup = (r"""vis_calls = 0; vis_what = 0; stub_vis = %d; r_dlight_occlusion.integer = %d;
	  stub_gpuworld = %d;
	  memset (cl_dlights, 0, sizeof(cl_dlights)); for (k = 0; k < MAX_DLIGHTS; k++) cl_dlights[k].die = -1;
	  cl_dlights[5].origin[0] = %d; cl_dlights[5].radius = %d; cl_dlights[5].die = 20; cl_dlights[5].dark = %d;
	  cl_dlights[5].color[0] = 1;""" % (vis, cvar, name.startswith("gpuworld"), d, rad, dark))
        for n in range(2):
            if rad < 0:
                continue        # GL marks dark lights with .dark, not a negative radius
            calls.append(r"""
	{ static entity_t E; float amb = %d; int k; %s
	  gl_alias_%d (&E, &amb); printf ("alias gl%d %s %%.1f %%d %%d\n", amb, vis_calls, vis_what == &E && vis_lnum == 5); }"""
                         % (amb, setup, n, n, name))
        if not dark and not name.startswith("gpuworld"):     # no GPU grid in the software renderer
            calls.append(r"""
	{ static entity_t E; int k, r; %s
	  r = sw_alias (&E, %d); printf ("alias sw %s %%d.0 %%d %%d\n", r, vis_calls, vis_what == &E && vis_lnum == 5); }"""
                         % (setup, amb, name))
    prog.append("int main (void)\n{" + "".join(calls) + "\n\treturn 0;\n}")
    with tempfile.TemporaryDirectory() as work:
        c, exe = Path(work) / "hooks.c", Path(work) / "hooks"
        c.write_text("\n".join(prog))
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(exe), str(c), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("the lifted hooks no longer compile against the stubs:\n" + r.stderr[-2500:])
            return None
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def check_hooks(out):
    if out is None:
        return
    # mark: (marked, asked)
    want_mark = {"world_unseen": (0, 1), "world_seen": (1, 1), "entity_unseen": (1, 0),
                 "cvar0_unseen": (1, 0), "cvar2_unseen": (0, 1), "behind_seen": (0, 0)}
    want_alias = {"seen": (150.0, 1), "unseen": (0.0, 1), "cvar0_unseen": (150.0, 0),
                  "outofrange": (0.0, 0), "dark_seen": (50.0, 1), "dark_unseen": (200.0, 1),
                  "negradius_seen": (-150.0, 1), "negradius_unseen": (0.0, 1),
                  "gpuworld_unseen": (150.0, 0)}
    seen = set()
    for line in out.split("\n"):
        w = line.split()
        if not w:
            continue
        if w[0] == "mark":
            tier, name, marked, asked, right = w[1], w[2], int(w[3]), int(w[4]), int(w[5])
            seen.add(("mark", tier, name))
            if name == "unreached_unseen":
                # GL asks only after its reach test; the software loop has none and asks
                want = (0, 0) if tier == "gl" else (0, 1)
            else:
                want = want_mark[name]
            if (marked, asked) != want or (asked and not right):
                errors.append("%s R_MarkLights, %s: marked/asked %s (with the surface and lnum: %d), want %s" % (
                    "gl_rlight.c" if tier == "gl" else "r_light.c", name, (marked, asked), right, want))
        elif w[0] == "alias":
            site, name, amb, asked, right = w[1], w[2], float(w[3]), int(w[4]), int(w[5])
            seen.add(("alias", site, name))
            wa, wk = want_alias[name]
            if amb != wa or asked != wk or (asked and not right):
                errors.append("%s alias dlight loop, %s: ambient %g asked %d (with the entity and lnum: %d), "
                              "want ambient %g asked %d" % ({"gl0": "gl_rmain.c R_DrawAliasModel",
                                                             "gl1": "gl_rmain.c instanced",
                                                             "sw": "r_main.c R_PrepareAlias"}[site],
                                                            name, amb, asked, right, wa, wk))
    for tier in ("gl", "sw"):
        for name in list(want_mark) + ["unreached_unseen"]:
            if ("mark", tier, name) not in seen:
                errors.append("mark %s %s: no result" % (tier, name))
    for name in want_alias:
        for site in ("gl0", "gl1", "sw"):
            if site == "sw" and (name.startswith("dark") or name.startswith("gpuworld")):
                continue
            if site != "sw" and name.startswith("negradius"):
                continue
            if ("alias", site, name) not in seen:
                errors.append("alias %s %s: no result" % (site, name))


# ------------------------------------------------------------------ pins
def check_pins():
    for rel in (GL_RLIGHT, R_LIGHT):
        fn = body_of(strip_comments(read(rel)), r"void R_PushDlights \(void\)\s*\{") or ""
        a = fn.find("R_DlightVis_BeginFrame ();")
        b = fn.find("return")
        if a < 0 or (b >= 0 and b < a):
            errors.append("%s: R_PushDlights must call R_DlightVis_BeginFrame before it can return "
                          "(the CPU alias loops ask r_dlightvis.c too)" % rel)
    for rel, head in ((GL_RMISC, r"void R_NewMap \(void\)\s*\{"), (SW_MAIN, r"void R_NewMap \(void\)\s*\{")):
        if strip_comments(body_of(read(rel), head) or "").count("R_DlightVis_NewMap ();") != 1:
            errors.append("%s: R_NewMap must call R_DlightVis_NewMap once" % rel)
    for rel, head in ((GL_RMISC, r"void R_Init \(void\)\s*\{"), (SW_MAIN, r"void R_Init \(void\)\s*\{")):
        if strip_comments(body_of(read(rel), head) or "").count("R_DlightVis_Init ();") != 1:
            errors.append("%s: R_Init must call R_DlightVis_Init once" % rel)
    cm = read(CMAKE)
    for lst in ("GL_RENDERER_SOURCES", "SOFT_RENDERER_SOURCES"):
        m = re.search(r"set\(%s\n(.*?)\n\)" % lst, cm, re.S)
        if not m or "${COMMONDIR}/r_dlightvis.c" not in m.group(1):
            errors.append("CMakeLists.txt: r_dlightvis.c must be in %s" % lst)
    for rel in (GL_RMAIN, SW_MAIN):
        fn = body_of(read(rel), r"void R_DrawViewModel \(void\)\s*\{") or ""
        if not fn:
            errors.append("%s: no R_DrawViewModel" % rel)
        elif "R_DlightVis" in fn:
            errors.append("%s: R_DrawViewModel's dlight loop feeds cl.light_level to the game; leave it vanilla"
                          % rel)
    src = strip_comments(read(VIS_C))
    for bad in (r"\bSV_\w+", r"\bsv\.", r"\bhulls?\b"):
        if re.search(bad, src):
            errors.append("r_dlightvis.c must not use server code (%r): WEBSOFT and the client link it alone" % bad)
    if not read(VIS_H):
        errors.append("%s is missing" % VIS_H)
    calls = {}
    for d in ("engine/hexen2", "engine/h2shared"):
        for p in sorted((ROOT / d).glob("*.c")):
            for m in re.finditer(r"\bR_DlightVis_(Surface|Entity|Point)\s*\(", strip_comments(p.read_text(errors="replace"))):
                calls.setdefault(m.group(1), set()).add(p.name)
    want = {"Surface": {"gl_rlight.c", "r_light.c", "r_dlightvis.c"}, "Entity": {"gl_rmain.c", "r_main.c", "r_dlightvis.c"},
            "Point": {"r_dlightvis.c"}}
    for k, v in want.items():
        if calls.get(k, set()) != v:
            errors.append("R_DlightVis_%s is called from %s, want %s (a new caller is untested here)"
                          % (k, sorted(calls.get(k, set())), sorted(v)))


check_fixture(run_fixture())
check_hooks(run_hooks())
check_pins()
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(dict.fromkeys(errors)))
print("PASS a dynamic light does not light what it cannot see (GitHub #97 P2): r_dlightvis.c run on a "
      "synthetic BSP against the oracle's segments and samples, cascade, cache and dump checked; "
      "R_MarkLights (GL, SW) and the three alias loops run; hooks pinned")
