#!/usr/bin/env python3
"""A self-only misc_modelpimp cast light (spawnflags 8 + 16) is lit once, at its
controller (GitHub #522).

Inky's controllers have effects exactly EF_NODRAW, which the server never
sends, so CL_RelinkEntities never sees them and never allocated the light.
PF_pimpmodel now marks the override (self_light) and registers it on the same
self-only list the #518 orb uses (R_AddPimpSelfOnly); after the entity loop,
CL_PimpSelfLights lights every registered controller the loop did not already
light, through CL_PimpCastLight -- the same function the loop's EF_ILLUMINATE
branch calls.

This lifts the production code verbatim and runs it against stubs:

  - from engine/h2shared/pr_cmds.c, the self_orb / self_light assignments and
    the registration test, for every spawnflags 0..127;
  - from engine/h2shared/gl_model.h and engine/hexen2/client.h, the
    pimp_override_t and dlight_t layouts and the glow-settings indices;
  - from engine/hexen2/gl_rmain.c AND engine/hexen2/r_soft_web.c (each in
    turn), the override table, the self-only list, R_ClearPimpOverrides,
    R_AddPimpSelfOnly, R_GetPimpSelfOnly, R_GetPimpOverride and
    R_GetPimpFlags;
  - from engine/hexen2/cl_main.c, CL_AllocDlightAt, CL_AllocDlight,
    CL_PimpCastLight, CL_PimpSelfLights, and the per-entity dlight block of
    CL_RelinkEntities (the visible path), so the dedupe and the "same light as
    a visible entity" checks run the real thing on both sides.

Nothing on the dlight path is stubbed: the stubs are the client's globals
(cl, cl_entities, cl_dlights, r_origin, cvars) and Con_DPrintf.

The live check (needs Storm over Thyrion) is tools/pimp-orb-check.sh.  Any
source can be swapped for a mutant: --cl-main, --pr-cmds, --gl-rmain, --soft.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

CONSTANTS = ["EF_ILLUMINATE", "XF_NO_DLIGHT", "XF_TORCH_GLOW", "EF_MUZZLEFLASH",
             "EF_BRIGHTLIGHT", "EF_DIMLIGHT", "EF_DARKLIGHT", "EF_LIGHT",
             "COLOR_R", "COLOR_G", "COLOR_B", "COLOR_A", "ORB_OFFSET_X",
             "ORB_OFFSET_Y", "ORB_OFFSET_Z", "ORB_RADIUS", "LIGHT_STYLE",
             "LIGHT_RADIUS", "GLOW_SETTINGS_COUNT"]


def fail(msg):
    sys.exit("FAIL: " + msg)


def function(text, signature, what):
    a = text.find(signature)
    if a < 0:
        fail("%s: cannot find %r" % (what, signature))
    b = text.find("\n}\n", a)
    if b < 0:
        fail("%s: no end for %r" % (what, signature))
    return text[a:b + 3]


def line(text, pattern, what):
    m = re.search(pattern, text, re.M)
    if not m:
        fail("%s: cannot find /%s/" % (what, pattern))
    return m.group(0)


def compile_and_run(program, work, name):
    c = Path(work) / (name + ".c")
    exe = Path(work) / name
    c.write_text(program)
    r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-o", str(exe), str(c), "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        fail("the lifted code no longer compiles against the stubs (%s):\n%s"
             % (name, r.stderr[-3000:]))
    return subprocess.run([str(exe)], check=True, capture_output=True,
                          text=True).stdout


def constants(gl_model):
    out = []
    for name in CONSTANTS:
        m = re.search(r"^#define\s+%s\s+(.+?)\s*(?:/\*.*|//.*)?$" % name, gl_model, re.M)
        if not m:
            fail("%s not defined in gl_model.h" % name)
        out.append("#define %s %s" % (name, m.group(1).strip()))
    return "\n".join(out)


# ---------------------------------------------------------------- spawnflags

def check_spawnflags(pr_cmds, work, errors):
    fn = function(pr_cmds, "static void PF_pimpmodel (void)", "PF_pimpmodel")
    stmts = []
    for field in ("self_orb", "self_light"):
        m = re.search(r"pimp->%s\s*=\s*\(\s*spawnflags.*?;" % field, fn, re.S)
        if not m:
            fail("PF_pimpmodel no longer assigns pimp->%s" % field)
        stmts.append(m.group(0))
    m = re.search(r"if\s*\([^;{]*\)\s*R_AddPimpSelfOnly\s*\(\s*entnum\s*\)\s*;", fn)
    if not m:
        fail("PF_pimpmodel no longer registers with R_AddPimpSelfOnly (entnum)")
    stmts.append(m.group(0))
    defines = "\n".join(re.findall(r"^#define\s+PIMP_SF_\w+\s+\d+", pr_cmds, re.M))
    program = "\n".join([
        "#include <stdio.h>", defines,
        "typedef struct { int self_orb, self_light; } pimp_override_t;",
        "static int registered;",
        "static void R_AddPimpSelfOnly (int n) { (void)n; registered++; }",
        "int main (void) {",
        "\tpimp_override_t p, *pimp = &p; int spawnflags, entnum = 7;",
        "\tfor (spawnflags = 0; spawnflags < 128; spawnflags++) {",
        "\t\tregistered = 0;",
        "\t\t" + "\n\t\t".join(stmts),
        "\t\tprintf (\"%d %d %d %d\\n\", spawnflags, pimp->self_orb ? 1 : 0,",
        "\t\t\tpimp->self_light ? 1 : 0, registered);",
        "\t}",
        "\treturn 0;", "}"])
    bad_light, bad_reg = [], []
    for ln in compile_and_run(program, work, "spawnflags").split("\n"):
        if not ln:
            continue
        sf, orb, light, reg = map(int, ln.split())
        want_light = 1 if (sf & 8 and sf & 16) else 0
        want_reg = 1 if (sf & 16 and sf & (4 | 8)) else 0
        if light != want_light:
            bad_light.append(sf)
        if reg != want_reg:
            bad_reg.append(sf)
    if bad_light:
        errors.append("self_light wrong for spawnflags %s (want: exactly when 8 "
                      "and 16 are both set)" % bad_light[:12])
    if bad_reg:
        errors.append("registration wrong for spawnflags %s (want: once, exactly "
                      "when 16 is set with 4 or 8)" % bad_reg[:12])
    if not bad_light and not bad_reg:
        print("spawnflags 0..127: self_light exactly when 8 and 16 are set; "
              "registered exactly when 16 is set with 4 or 8")


# ---------------------------------------------------------------- client pass

PRELUDE = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef float vec_t;
typedef vec_t vec3_t[3];
typedef int qboolean;
#define true 1
#define false 0
#define MAX_EDICTS 64
#define VectorCopy(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define VectorSubtract(a,b,c) ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorMA(a,s,v,o) ((o)[0]=(a)[0]+(s)*(v)[0],(o)[1]=(a)[1]+(s)*(v)[1],(o)[2]=(a)[2]+(s)*(v)[2])
static vec_t VectorLength (const vec3_t v) { return sqrtf (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
static void AngleVectors (vec3_t a, vec3_t f, vec3_t r, vec3_t u)
{ (void)a; memset(f,0,12); memset(r,0,12); memset(u,0,12); }
typedef struct qmodel_s { int type; int ex_flags; float glow_settings[16]; } qmodel_t;
typedef struct { qmodel_t *model; vec3_t origin, angles; int effects; double msgtime; } entity_t;
typedef struct { int integer; float value; } cvar_t;
typedef struct { int length; char map[64]; } lightstyle_t;
static struct { double time; int num_entities; } cl;
static entity_t cl_entities[MAX_EDICTS];
/* Past MAX_LIGHTSTYLES the table is padded with a style that is always
 * 'a' (black), so a light that reads off the end comes out dark. */
static lightstyle_t cl_lightstyle_storage[MAX_LIGHTSTYLES * 2];
#define cl_lightstyle cl_lightstyle_storage
static vec3_t r_origin;
static cvar_t gl_missile_glows = {1, 1}, gl_flashintensity = {1, 1}, gl_torch_dlight = {1, 1};
static int ndprint;
static void Con_DPrintf (const char *fmt, ...) { (void)fmt; ndprint++; }
'''

DRIVER = r'''
static qmodel_t alias;

/* What PF_pimpmodel leaves behind: an orange light of radius 150, 8 above
 * org.  No zero component: CL_PimpCastLight reads 0 as 1. */
static void pimp (int n, int orb, int light, float x, float y, float z)
{
	pimp_override_t *p = &pimp_overrides[n];
	memset (p->glow_settings, 0, sizeof(p->glow_settings));
	p->active = true;
	p->ex_flags = light ? EF_ILLUMINATE : 0;
	p->self_orb = orb;
	p->self_light = light;
	p->origin[0] = x; p->origin[1] = y; p->origin[2] = z;
	p->model = &alias;
	p->glow_settings[COLOR_R] = 1;
	p->glow_settings[COLOR_G] = 0.5f;
	p->glow_settings[COLOR_B] = 0.25f;
	p->glow_settings[COLOR_A] = 0.75f;
	p->glow_settings[ORB_OFFSET_Z] = 8;
	p->glow_settings[LIGHT_RADIUS] = 150;
	if (orb || light)
		R_AddPimpSelfOnly (n);
}

static int vis[MAX_EDICTS], nvis;

/* CL_RelinkEntities for this frame: the entity loop's dlight block for each
 * entity the client has, then the pass after the loop. */
static void relink (void)
{
	int v;
	cl.time += 0.02;	/* last frame's 0.001s lights are dead */
	for (v = 0; v < nvis; v++)
		visible (vis[v], &cl_entities[vis[v]]);
	CL_PimpSelfLights ();
}

static void dump (const char *name)
{
	int i, n = 0;
	printf ("%s", name);
	for (i = 0; i < MAX_DLIGHTS; i++)
	{
		dlight_t *dl = &cl_dlights[i];
		if (dl->die < cl.time || dl->radius == 0 || dl->key >= 1000)
			continue;
		n++;
		printf (" %d@%.0f,%.0f,%.0f/r%.0f/c%.2f,%.2f,%.2f,%.2f", dl->key,
			dl->origin[0], dl->origin[1], dl->origin[2], dl->radius,
			dl->color[0], dl->color[1], dl->color[2], dl->color[3]);
	}
	printf ("\n");
}

static void frame (const char *name) { relink (); dump (name); }

static dlight_t *find (int key)
{
	int i;
	for (i = 0; i < MAX_DLIGHTS; i++)
		if (cl_dlights[i].key == key && cl_dlights[i].die >= cl.time && cl_dlights[i].radius)
			return &cl_dlights[i];
	return NULL;
}

/* Every slot live, keys 1000+, at dist from the eye. */
static void fill (float dist, float radius)
{
	int i;
	for (i = 0; i < MAX_DLIGHTS; i++)
	{
		memset (&cl_dlights[i], 0, sizeof(cl_dlights[i]));
		cl_dlights[i].key = 1000 + i;
		cl_dlights[i].origin[0] = dist;
		cl_dlights[i].radius = radius;
		cl_dlights[i].die = 1e9;
	}
}

int main (void)
{
	int n;

	alias.type = 2;
	for (n = MAX_LIGHTSTYLES; n < MAX_LIGHTSTYLES * 2; n++)
	{
		cl_lightstyle[n].length = 1;
		cl_lightstyle[n].map[0] = 'a';
	}
	cl.num_entities = 8;	/* the controllers are beyond it: never sent */
	R_ClearPimpOverrides ();
	R_AddPimpSelfOnly (-1);
	R_AddPimpSelfOnly (MAX_EDICTS);
	frame ("empty");

	pimp (10, false, true, 100, 200, 300);		/* spawnflags 24 */
	frame ("controller");
	pimp (10, false, true, 100, 200, 300);		/* re-pimped every wait */
	frame ("inplace");

	pimp_overrides[10].ex_flags |= XF_NO_DLIGHT;	/* 24 + 64 */
	frame ("nodlight");
	pimp (10, false, true, 100, 200, 300);
	alias.ex_flags = XF_NO_DLIGHT;			/* 64 shared by another controller */
	frame ("modelnodlight");
	alias.ex_flags = 0;

	pimp (11, true, false, 5, 5, 5);		/* spawnflags 20: an orb, no light */
	frame ("orbonly");
	pimp_overrides[11].active = false;

	/* An entity the client does have (a visible prop pimped 24): the loop
	 * lights it at its own origin, and the pass after the loop adds nothing. */
	cl.num_entities = 16;
	pimp (12, false, true, 7, 8, 9);
	cl_entities[12].model = &alias;
	cl_entities[12].origin[0] = 70;
	cl_entities[12].msgtime = 1;
	vis[nvis++] = 12;
	frame ("visible");

	/* The controller's light is the light a visible entity carrying the same
	 * override at the same spot gets. */
	pimp (13, false, true, 100, 200, 300);
	pimp_overrides[13].glow_settings[COLOR_G] = 0.3f;
	pimp_overrides[13].glow_settings[ORB_OFFSET_X] = 4;
	memcpy (pimp_overrides[10].glow_settings, pimp_overrides[13].glow_settings,
		sizeof(pimp_overrides[10].glow_settings));
	cl_entities[13].model = &alias;
	cl_entities[13].msgtime = 1;
	VectorCopy (pimp_overrides[13].origin, cl_entities[13].origin);
	vis[nvis++] = 13;
	relink ();
	{
		dlight_t *a = find (10), *b = find (13);
		int same = a && b && !memcmp (a->origin, b->origin, sizeof(a->origin))
			&& a->radius == b->radius && a->die == b->die && a->dark == b->dark
			&& !memcmp (a->color, b->color, sizeof(a->color));
		printf ("match %d\n", same);
	}
	nvis = 0;
	cl_entities[13].model = NULL;
	pimp_overrides[13].active = false;

	/* The visible prop leaves the PVS (or QC hides it with EF_NODRAW): the
	 * relink drops it, and its light goes with it, as before #522.  Still
	 * pimped, still on the list, still re-pimped every wait. */
	pimp (10, false, true, 100, 200, 300);
	cl_entities[12].model = NULL;
	pimp (12, false, true, 7, 8, 9);
	frame ("gone");
	pimp_overrides[12].active = false;
	cl_entities[12].msgtime = 0;
	cl.num_entities = 8;
	pimp (10, false, true, 100, 200, 300);

	pimp_overrides[10].active = false;		/* ED_Free */
	frame ("freed");
	pimp_overrides[10].active = true;		/* slot back, not re-pimped yet */
	frame ("stale");
	pimp (10, false, true, 100, 200, 300);
	frame ("again");
	pimp (10, false, false, 100, 200, 300);		/* re-pimped as 8 alone */
	frame ("unflagged");
	pimp (10, false, true, 100, 200, 300);

	/* R_NewMap: an override set up by hand, not registered, so only a list
	 * left over from the last map could light it. */
	R_ClearPimpOverrides ();
	pimp_overrides[10].active = true;
	pimp_overrides[10].self_light = true;
	pimp_overrides[10].ex_flags = EF_ILLUMINATE;
	pimp_overrides[10].model = &alias;
	frame ("newmap");

	/* Pool full of lights near the eye: a far controller is refused, as a
	 * far entity would be, rather than evicting one of them. */
	R_ClearPimpOverrides ();
	pimp (10, false, true, 5000, 0, 0);
	cl.time += 0.02;
	fill (100, 300);
	CL_PimpSelfLights ();
	printf ("poolfar %d\n", find (10) != NULL);
	/* Pool full of small far lights: a controller at the eye takes a slot. */
	pimp (10, false, true, 100, 0, 0);
	fill (3000, 10);
	CL_PimpSelfLights ();
	printf ("poolnear %d\n", find (10) != NULL);

	/* The same contest for every other light, through CL_AllocDlight: an
	 * EF_DIMLIGHT entity far from the eye is refused, one at the eye gets a
	 * slot.  Driven through CL_RelinkEntities' own block. */
	R_ClearPimpOverrides ();
	cl.num_entities = 16;
	memset (&cl_entities[5], 0, sizeof(cl_entities[5]));
	cl_entities[5].model = &alias;
	cl_entities[5].effects = EF_DIMLIGHT;
	cl_entities[5].msgtime = 1;
	cl_entities[5].origin[0] = 5000;
	fill (100, 300);
	visible (5, &cl_entities[5]);
	printf ("entfar %d\n", find (5) != NULL);
	cl_entities[5].origin[0] = 100;
	fill (3000, 10);
	visible (5, &cl_entities[5]);
	printf ("entnear %d\n", find (5) != NULL);
	memset (&cl_entities[5], 0, sizeof(cl_entities[5]));
	memset (cl_dlights, 0, sizeof(cl_dlights));
	cl.num_entities = 8;

	/* A style past MAX_LIGHTSTYLES is no style, not a read off the table. */
	pimp (10, false, true, 100, 200, 300);
	pimp_overrides[10].glow_settings[LIGHT_STYLE] = MAX_LIGHTSTYLES + 44;
	frame ("bigstyle");
	pimp (10, false, true, 100, 200, 300);

	/* A pimpmodel call that fails (no model, or one that will not load)
	 * grants nothing self-only: the entry drops, and the light with it. */
	parm0 = &edicts[10];
	edicts[10].v.model = 0;
	PF_pimpmodel ();
	frame ("failnomodel");
	pimp (10, false, true, 100, 200, 300);
	edicts[10].v.model = 2;			/* a name Mod_FindName cannot find */
	PF_pimpmodel ();
	frame ("failmissing");
	pimp (10, false, true, 100, 200, 300);
	edicts[10].v.model = 1;			/* and one it can: the call gets through */
	PF_pimpmodel ();
	printf ("callok %d\n", g_return == 1 && pimp_overrides[10].active);
	return 0;
}
'''

RED = "/c1.00,0.50,0.25,0.75"
WANT = {
    "empty": [],
    "controller": ["10@100,200,308/r150" + RED],
    "inplace": ["10@100,200,308/r150" + RED],
    "nodlight": [],
    "modelnodlight": [],
    "orbonly": ["10@100,200,308/r150" + RED],
    "visible": ["10@100,200,308/r150" + RED, "12@70,0,8/r150" + RED],
    "freed": [],
    "stale": [],
    "again": ["10@100,200,308/r150" + RED],
    "unflagged": [],
    "newmap": [],
    "gone": ["10@100,200,308/r150" + RED],
    "bigstyle": ["10@100,200,308/r150" + RED],
    "failnomodel": [],
    "failmissing": [],
}
SCALARS = {"match": "1", "poolfar": "0", "poolnear": "1", "entfar": "0",
           "entnear": "1", "callok": "1"}

# The head of PF_pimpmodel, up to the point a call has succeeded, runs against
# these: edict 10's .model names a string; 1 loads, 2 does not.
PF_STUBS = r"""
typedef struct { struct { int model; } v; } edict_t;
static edict_t edicts[MAX_EDICTS], *parm0;
static float parm1[3];
static int g_return;
#define OFS_PARM0 0
#define OFS_PARM1 1
#define OFS_RETURN 2
#define G_EDICT(o) parm0
#define G_VECTOR(o) parm1
#define G_INT(o) g_return
#define NUM_FOR_EDICT(e) ((int)((e) - edicts))
#define __thisfunc__ "PF_pimpmodel"
static qmodel_t loadable;
static const char *PR_GetString (int s) { return s == 1 ? "models/ok.mdl" : "models/missing.mdl"; }
static qmodel_t *Mod_FindName (const char *n) { return strcmp (n, "models/ok.mdl") ? NULL : &loadable; }
static void Con_Printf (const char *fmt, ...) { (void)fmt; }
"""


def pf_head(pr_cmds):
    sig = "static void PF_pimpmodel (void)"
    a = pr_cmds.find(sig)
    if a < 0:
        fail("pr_cmds.c: cannot find %r" % sig)
    end = "pimp->active = true;"
    b = pr_cmds.find(end, a)
    if b < 0:
        fail("pr_cmds.c: PF_pimpmodel no longer sets pimp->active = true")
    return pr_cmds[a:b + len(end)] + "\n\tG_INT(OFS_RETURN) = 1;\n}\n"


def client_program(cl_main, list_src, list_name, gl_model, client_h, pr_cmds, quakedef):
    dl = client_h[:client_h.index("} dlight_t;") + len("} dlight_t;")]
    dl = dl[dl.rindex("typedef struct"):]
    pimp_t = gl_model[gl_model.index("// Per-entity PimpModel overrides"):]
    pimp_t = pimp_t[:pimp_t.index("} pimp_override_t;") + len("} pimp_override_t;")]
    lst = "\n".join([
        line(list_src, r"^static pimp_override_t\s+pimp_overrides\[MAX_EDICTS\];", list_name),
        line(list_src, r"^#define\s+MAX_PIMP_SELF_ONLY\s+\d+", list_name),
        line(list_src, r"^static int\s+pimp_self_only\[MAX_PIMP_SELF_ONLY\];", list_name),
        line(list_src, r"^static int\s+num_pimp_self_only;", list_name),
        line(list_src, r"^static float\s+null_glow_settings\[GLOW_SETTINGS_COUNT\];", list_name),
        function(list_src, "void R_ClearPimpOverrides (void)", list_name),
        function(list_src, "void R_AddPimpSelfOnly (int entnum)", list_name),
        function(list_src, "int R_GetPimpSelfOnly (const int **list)", list_name),
        function(list_src, "pimp_override_t *R_GetPimpOverride (int entnum)", list_name),
        function(list_src, "int R_GetPimpFlags (entity_t *e, float **gsettings_out)", list_name),
    ])
    try:
        a = cl_main.index("pflags = R_GetPimpFlags (ent, &pimp_gs);")
        b = cl_main.index("/* Use per-entity trail flags", a)
    except ValueError:
        fail("cl_main.c: cannot find CL_RelinkEntities' dlight block")
    visible = "\n".join([
        "static void visible (int i, entity_t *ent)", "{",
        "\tdlight_t *dl; float *pimp_gs; int pflags; qboolean nodlight;",
        "\t" + cl_main[a:b],
        "\t(void)dl;", "}"])
    client = "\n".join([
        line(cl_main, r"^#define\s+DLIGHT_NEAR\s+\S+", "cl_main.c"),
        line(cl_main, r"^#define\s+DLIGHT_FAR\s+\S+", "cl_main.c"),
        line(cl_main, r"^static dlight_t\s+cl_dlight_discard;", "cl_main.c"),
        function(cl_main, "static dlight_t *CL_AllocDlightAt (int key, const float *org)", "cl_main.c"),
        function(cl_main, "dlight_t *CL_AllocDlight (int key)", "cl_main.c"),
        function(cl_main, "static void CL_PimpCastLight (int key, const float *org, const float *gs)", "cl_main.c"),
        function(cl_main, "static void CL_PimpSelfLights (void)", "cl_main.c"),
    ])
    return "\n".join([
        line(quakedef, r"^#define\s+MAX_LIGHTSTYLES\s+\d+", "quakedef.h"),
        PRELUDE, constants(gl_model),
        line(client_h, r"^#define\s+MAX_DLIGHTS\s+\d+", "client.h"),
        dl, "static dlight_t cl_dlights[MAX_DLIGHTS];",
        pimp_t, lst, client, visible, PF_STUBS, pf_head(pr_cmds), DRIVER])


def check_client(cl_main, list_src, list_name, gl_model, client_h, pr_cmds, quakedef,
                 work, errors):
    out = compile_and_run(client_program(cl_main, list_src, list_name, gl_model, client_h,
                                         pr_cmds, quakedef),
                          work, "selflight_" + re.sub(r"\W", "_", list_name))
    seen = {}
    for ln in out.split("\n"):
        if ln:
            parts = ln.split()
            seen[parts[0]] = parts[1:]
    tag = "[%s]" % list_name
    for name, want in WANT.items():
        got = sorted(seen.get(name, ["<missing>"]))
        ok = got == sorted(want)
        print("%s %-13s lit %s, want %s  %s" % (tag, name, got or "nothing",
                                                 want or "nothing", "ok" if ok else "WRONG"))
        if not ok:
            errors.append("%s %s: lit %s, want %s" % (tag, name, got or "nothing",
                                                      want or "nothing"))
    for name, want in SCALARS.items():
        got = " ".join(seen.get(name, ["<missing>"]))
        ok = got == want
        print("%s %-13s %s, want %s  %s" % (tag, name, got, want, "ok" if ok else "WRONG"))
        if not ok:
            errors.append("%s %s: %s, want %s" % (tag, name, got, want))


def strip_c(text):
    """Comments and literals blanked to spaces, so braces inside them do not
    count; offsets are kept."""
    out = re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
                 lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return out


def check_relink_calls(cl_main, errors):
    """CL_PimpSelfLights runs once per frame: in CL_RelinkEntities' own body
    (brace depth 1), after the entity loop has closed, and only there."""
    fn = strip_c(function(cl_main, "static void CL_RelinkEntities (void)", "CL_RelinkEntities"))
    loop = re.search(r"for\s*\(\s*i\s*=\s*1\s*,\s*ent\s*=\s*cl_entities\s*\+\s*1\s*;", fn)
    calls = [m.start() for m in re.finditer(r"\bCL_PimpSelfLights\s*\(\s*\)\s*;", fn)]
    if not loop:
        errors.append("CL_RelinkEntities: cannot find the entity loop")
        return
    depth, loop_end, depth_at = 0, None, {}
    open_ = fn.index("{", loop.end())
    for i, ch in enumerate(fn):
        if i in calls:
            depth_at[i] = depth
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if loop_end is None and i > open_ and depth == 1:
                loop_end = i
    ok = (len(calls) == 1 and loop_end is not None and calls[0] > loop_end
          and depth_at.get(calls[0]) == 1)
    if ok:
        print("CL_RelinkEntities calls CL_PimpSelfLights once, at depth 1, after the entity loop")
    else:
        errors.append("CL_RelinkEntities must call CL_PimpSelfLights () exactly once, in "
                      "its own body after the entity loop closes (calls at %s, depths %s, "
                      "loop ends at %s)" % (calls, [depth_at.get(c) for c in calls], loop_end))


def check_headers(errors):
    for h in (ROOT / "engine/h2shared/gl_model.h", ROOT / "engine/h2shared/model.h"):
        t = h.read_text()
        body = t[t.find("// Per-entity PimpModel overrides"):t.find("} pimp_override_t;")]
        if not re.search(r"qboolean\s+self_light;", body):
            errors.append("%s: pimp_override_t lacks self_light" % h.name)
        if "int R_GetPimpSelfOnly (const int **list);" not in t:
            errors.append("%s does not declare R_GetPimpSelfOnly" % h.name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cl-main", default=ROOT / "engine/hexen2/cl_main.c", type=Path)
    ap.add_argument("--pr-cmds", default=ROOT / "engine/h2shared/pr_cmds.c", type=Path)
    ap.add_argument("--gl-rmain", default=ROOT / "engine/hexen2/gl_rmain.c", type=Path)
    ap.add_argument("--soft", default=ROOT / "engine/hexen2/r_soft_web.c", type=Path)
    args = ap.parse_args()
    cl_main = args.cl_main.read_text()
    gl_model = (ROOT / "engine/h2shared/gl_model.h").read_text()
    client_h = (ROOT / "engine/hexen2/client.h").read_text()
    quakedef = (ROOT / "engine/hexen2/quakedef.h").read_text()
    errors = []
    with tempfile.TemporaryDirectory() as work:
        check_spawnflags(args.pr_cmds.read_text(), work, errors)
        check_relink_calls(cl_main, errors)
        for src, name in ((args.gl_rmain, "gl_rmain.c"), (args.soft, "r_soft_web.c")):
            check_client(cl_main, src.read_text(), name, gl_model, client_h,
                         args.pr_cmds.read_text(), quakedef, work, errors)
    check_headers(errors)
    for e in errors:
        print("FAIL: " + e)
    if errors:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
