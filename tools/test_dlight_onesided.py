#!/usr/bin/env python3
"""A dynamic light behind a face does not light it, on every tier (GitHub #97).

Every renderer lit both sides of a face's plane, so a torch lit the far side
of the wall it stood against.  r_dlight_occlusion 1 (the default) adds a
one-sided test with a tolerance of DLIGHT_PLANE_EPSILON (2 units, client.h):

  * the CPU twins, gl_rsurf.c / r_surf.c R_AddDynamicLights, which decide
    what lands in a lightmap, and gl_rlight.c / r_light.c R_MarkLights, which
    decide which surfaces are rebuilt at all;
  * the GPU path, ClusteredDlight in engine/shaders/world_dlight.inc, fed the
    tolerance through WorldFrag's u_wfgridz.w.

The CPU half is tested by running it.  Lifted out of the engine source
verbatim, compiled against stubs and run with one light and one surface: each
R_AddDynamicLights prologue (from `rad = ...` through the impact loop), each
R_MarkLights surface loop, R_DlightModelOrigin (client.h) with the
AngleVectors it calls, and the dlight loop of each of the three brush-entity
marking sites -- R_DrawBrushModel (gl_rsurf.c), R_CollectBrushInstances
(gl_rmain.c) and the software R_DrawBEntitiesOnList (r_main.c) -- with
R_MarkLights caught by a macro, so what a site hands it is what the mark loop
is then run on:

  light 24 units behind the face        -> no light, not marked
  light 1 unit behind (inside the 2)    -> lit, marked, radius 199
  light 24 in front                     -> lit, marked
  SURF_PLANEBACK, light on the plane's
    + side (so behind the face)         -> no light; on the - side -> lit
  SURF_DRAWTURB, light 24 behind        -> lit (liquids keep both sides)
  r_dlight_occlusion 0, light 24 behind -> lit (vanilla)

and the same on brush entities (GitHub #495), which are tested like the world
once the light is in their model space: a plat lowered 128 with a light 20
above it (Quake measured -108), a moved door, an entity turned 90 in yaw, one
turned in all three angles, and one rolled over.  The plane is z = 0 in model
space and the light sits at model-space (10, 20, h); its world-space origin is
worked out in Python from R_RotateBmodel's entity_rotation (r_bsp.c), written
out independently of AngleVectors.  Each case checks lit / dropped, marked /
not, that the site passed its entity once and the mark loop recorded it
(dlightent), the radius left (200 - |h|) and the impact point (10, 20, 0).
Every R_MarkLights call outside gl_rlight.c / r_light.c must be one of the
three sites, and R_PushDlights marks the world with a NULL entity.  The GL
draw is pinned to the same transform as the software reference:
R_RotateForEntity's rotation order and signs, and the pitch / roll negation
both GL brush paths make before calling it.

The GPU half cannot be run here, so it is pinned: the derivative normal is
turned toward the eye before anything reads it, the light is dropped when
pdist < -u_dlight_onesided (gated on the lane being > 0), the falloff keeps
abs(pdist), the lane is u_wfgridz.w, R_SetWorldDlight writes it, the cluster
bind passes the cvar's tolerance, and lit water brackets its batches with
R_SetWorldLiquid so liquids pass 0.  The cvar itself is pinned too: defined in
gl_rmain.c and r_soft_web.c, "1" and CVAR_ARCHIVE in both, registered in
gl_rmisc.c and R_SoftWebInitCvars.

    python3 tools/test_dlight_onesided.py
"""
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(rel):
    p = ROOT / rel
    return p.read_text(errors="replace") if p.exists() else ""


def strip_comments(t):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", t, flags=re.S)


def body_of(text, head):
    m = re.search(head, text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    return None


def define(rel, name):
    m = re.search(r"#define\s+%s\s+(\S+)" % name, read(rel))
    if not m:
        errors.append("%s: no #define %s" % (rel, name))
        return "0"
    return m.group(1)


# ------------------------------------------------------------------ the C
GL_RSURF, R_SURF = "engine/hexen2/gl_rsurf.c", "engine/h2shared/r_surf.c"
GL_RLIGHT, R_LIGHT = "engine/h2shared/gl_rlight.c", "engine/h2shared/r_light.c"
CLIENT_H, MATHLIB = "engine/hexen2/client.h", "engine/h2shared/mathlib.c"
MARK_SIG = "void R_MarkLights (dlight_t *light, int lnum, mnode_t *node, const entity_t *ent)"

# The three brush-entity marking loops (#495): name, file, the function they
# are in, and what the entity is called there.  Each is lifted whole and run.
SITES = [
    ("gl_rsurf", "engine/hexen2/gl_rsurf.c", r"void R_DrawBrushModel \(entity_t \*e, qboolean Translucent\)\s*\{", "e"),
    ("gl_rmain", "engine/hexen2/gl_rmain.c", r"void R_CollectBrushInstances \(void\)\s*\{", "e"),
    ("sw_main", "engine/hexen2/r_main.c", r"static void R_DrawBEntitiesOnList \(void\)\s*\{", "currententity"),
]


def braced(text, at):
    """text[at:] through the end of the brace block that follows `at`."""
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    return None


def prologue(rel, first):
    """R_AddDynamicLights' per-light prologue: `first` through the impact loop."""
    fn = body_of(read(rel), r"static void R_AddDynamicLights \([^)]*\)\s*\{")
    if not fn:
        errors.append("%s: no R_AddDynamicLights" % rel)
        return None
    a = fn.find(first)
    b = fn.find("minlight = rad - minlight;", a)
    c = fn.rfind("for (i = 0; i < 3; i++)", b, fn.find("impact[i] = ", b))
    if a < 0 or b < 0 or c < 0:
        errors.append("%s: R_AddDynamicLights' prologue (%r .. the impact loop) is gone" % (rel, first))
        return None
    return fn[a:c] + braced(fn, c)


def live(text):
    m = re.search(r"#if 0\s*/\* the original version from ID \*/(.*?)#else", text, re.S)
    return text[:m.start()] + text[m.end():] if m else text


def mark_loop(rel):
    """R_MarkLights' surface loop, in the live (not #if 0) version."""
    fn = body_of(live(read(rel)), r"\n%s\s*\{" % re.escape(MARK_SIG))
    if not fn:
        errors.append("%s: no %s" % (rel, MARK_SIG))
        return None
    a = fn.find("for (i = 0; i < node->numsurfaces; i++, surf++)")
    if a < 0:
        errors.append("%s: R_MarkLights has no surface loop" % rel)
        return None
    return braced(fn, a)


def site_loop(rel, head):
    """A brush-entity function's dlight loop: the body of its one
    `for (k = 0; k < MAX_DLIGHTS; k++)` that calls R_MarkLights."""
    fn = body_of(read(rel), head)
    if not fn:
        errors.append("%s: no %s" % (rel, head))
        return None
    loops = [braced(fn, m.end()) for m in re.finditer(r"for \(k = 0; k < MAX_DLIGHTS; k\+\+\)", fn)]
    loops = [l for l in loops if l and "R_MarkLights" in l]
    if len(loops) != 1 or len(re.findall(r"\bR_MarkLights\b", strip_comments(fn))) != 1:
        errors.append("%s: want exactly one dlight loop, calling R_MarkLights once, in %s" % (rel, head))
        return None
    return loops[0]


def helper():
    m = re.search(r"^static inline void R_DlightModelOrigin \(const struct entity_s \*e, const vec3_t world, "
                  r"vec3_t out\)\n\{.*?^\}", read(CLIENT_H), re.S | re.M)
    if not m:
        errors.append("client.h: no static inline R_DlightModelOrigin (e, world, out)")
        return None
    return m.group(0)


def angle_vectors():
    m = re.search(r"^void AngleVectors \(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up\)\n\{.*?^\}",
                  read(MATHLIB), re.S | re.M)
    if not m:
        errors.append("mathlib.c: no AngleVectors")
        return None
    return m.group(0)


PRELUDE = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef float vec_t;
typedef vec_t vec3_t[3];
typedef int qboolean;
#define PITCH 0
#define YAW 1
#define ROLL 2
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define VectorSubtract(a,b,c) ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorCopy(a,b) memcpy((b),(a),sizeof(vec3_t))
#define MAX_DLIGHTS 128
#define DLIGHTBITS_WORDS ((MAX_DLIGHTS + 31) / 32)
#define DLIGHTBIT_TEST(b,i) ((b)[(i) >> 5] & (1u << ((i) & 31)))
#define DLIGHTBIT_SET(b,i) ((b)[(i) >> 5] |= (1u << ((i) & 31)))
#define DLIGHTBITS_CLEAR(b) memset ((b), 0, sizeof(unsigned int) * DLIGHTBITS_WORDS)
typedef struct entity_s { vec3_t origin, angles; } entity_t;
typedef struct { vec3_t normal; float dist; int type; } mplane_t;
typedef struct { float vecs[2][4]; } mtexinfo_t;
typedef struct { mplane_t *plane; int flags; mtexinfo_t *texinfo; short texturemins[2], extents[2];
                 int dlightframe; unsigned int dlightbits[4]; const struct entity_s *dlightent; } msurface_t;
typedef struct mnode_s { int numsurfaces; } mnode_t;
typedef struct { vec3_t origin; float radius, minlight, die; } dlight_t;
typedef struct { int integer; } cvar_t;
static cvar_t r_dlight_occlusion;
static dlight_t cl_dlights[1];
static int r_dlightframecount = 7;
static mplane_t plane = {{0, 0, 1}, 0, 2};
static mtexinfo_t texinfo = {{{1, 0, 0, 0}, {0, 1, 0, 0}}};
static msurface_t surfs[1];
static mnode_t nodes[1];
typedef struct { mnode_t *nodes; struct { int firstclipnode; } hulls[1]; msurface_t *surfaces; } qmodel_t;
static qmodel_t world = {nodes, {{0}}, surfs};
static struct { qmodel_t *worldmodel; double time; } cl = {&world, 10.0};
/* what a site handed R_MarkLights */
static dlight_t captured; static const entity_t *captured_ent; static int captured_n;
static const entity_t not_an_entity;
"""

SITE = r"""
static void site_%(name)s (entity_t *ent)
{
	int k;
	entity_t *%(var)s = ent;
	qmodel_t *clmodel = &world;
#define R_MarkLights(L, K, N, E) (captured = *(L), captured_ent = (E), captured_n++, (void)(K), (void)(N))
	for (k = 0; k < 1; k++)
%(body)s
#undef R_MarkLights
}
"""

# Returns 1, the remaining radius and the impact point if the light got
# through, 0 if it was dropped (the `continue` leaves the one-pass loop).
HARNESS = r"""
static int add_%(name)s (msurface_t *surf, float *out_rad, float *out_impact)
{
	int lnum, i; float dist, rad, minlight; vec3_t impact, lorg;
	for (lnum = 0; lnum < 1; lnum++)
	{
%(body)s
		*out_rad = rad; VectorCopy (impact, out_impact); (void)minlight; return 1;
	}
	return 0;
}
"""
# The light it is given is what R_PushDlights (the world) or a site (an
# entity) handed R_MarkLights; dist is to the node's plane, which here is the
# surface's.  Returns 1 marked, 0 not, 2 marked without recording ent.
MARK = r"""
static int mark_%(name)s (dlight_t *light, msurface_t *surf, const entity_t *ent)
{
	mnode_t real_node = {1}, *node = &real_node;
	msurface_t *const tested = surf;	/* the loop's surf++ walks past it */
	float l, maxdist, dist; int i, j, s, t; vec3_t impact; int lnum = 0;
	(void)l; (void)maxdist; (void)j; (void)s; (void)t; (void)impact;
	maxdist = light->radius * light->radius;
	dist = DotProduct (light->origin, plane.normal) - plane.dist;
	surf->dlightframe = 0;
	memset (surf->dlightbits, 0, sizeof(surf->dlightbits));
	surf->dlightent = &not_an_entity;
%(body)s
	if (!DLIGHTBIT_TEST (tested->dlightbits, 0))
		return 0;
	return tested->dlightent == ent ? 1 : 2;
}
"""

# The entities, as (origin, angles).  The surface's plane is z = 0, normal +z,
# in MODEL space; each case puts its light at model-space (10, 20, h), and the
# world-space origin it is given is worked out here, in Python, from
# R_RotateBmodel's entity_rotation (r_bsp.c) -- the software renderer's model
# transform, written out independently of AngleVectors.
ENTS = {
    None: None,
    "plat": ((0.0, 0.0, -128.0), (0.0, 0.0, 0.0)),       # lowered 128: Quake put a light 20 above it at -108
    "door": ((64.0, -32.0, 16.0), (0.0, 0.0, 0.0)),
    "yaw90": ((-200.0, 48.0, 0.0), (0.0, 90.0, 0.0)),
    "turned": ((100.0, -40.0, 64.0), (30.0, 50.0, 20.0)),
    "flipped": ((0.0, 0.0, 96.0), (0.0, 0.0, 180.0)),   # rolled over: its +z is the world's -z
}
LOCAL = (10.0, 20.0)

# name, cvar, surf flags (by name), the light's height above the plane in
# model space, want lit, entity (None: the world).  Brush entities get the
# one-sided test like the world, measured in their own model space (#495).
CASES = [
    ("behind24", 1, "0", -24.0, False, None),
    ("behind1", 1, "0", -1.0, True, None),
    ("edge2", 1, "0", -2.0, True, None),
    ("behind2.5", 1, "0", -2.5, False, None),
    ("front24", 1, "0", 24.0, True, None),
    ("planeback_plus", 1, "SURF_PLANEBACK", 24.0, False, None),
    ("planeback_minus", 1, "SURF_PLANEBACK", -24.0, True, None),
    ("turb_behind", 1, "SURF_DRAWTURB", -24.0, True, None),
    ("turb_planeback", 1, "SURF_DRAWTURB|SURF_PLANEBACK", 24.0, True, None),
    ("cvar0_behind", 0, "0", -24.0, True, None),
    ("cvar2_behind", 2, "0", -24.0, False, None),
    ("plat_front20", 1, "0", 20.0, True, "plat"),
    ("plat_behind24", 1, "0", -24.0, False, "plat"),
    ("plat_cvar0_behind", 0, "0", -24.0, True, "plat"),
    ("door_front24", 1, "0", 24.0, True, "door"),
    ("door_behind24", 1, "0", -24.0, False, "door"),
    ("door_planeback_minus", 1, "SURF_PLANEBACK", -24.0, True, "door"),
    ("yaw90_front24", 1, "0", 24.0, True, "yaw90"),
    ("yaw90_behind24", 1, "0", -24.0, False, "yaw90"),
    ("turned_front30", 1, "0", 30.0, True, "turned"),
    ("turned_behind30", 1, "0", -30.0, False, "turned"),
    ("turned_turb_behind", 1, "SURF_DRAWTURB", -30.0, True, "turned"),
    ("flipped_front24", 1, "0", 24.0, True, "flipped"),
    ("flipped_behind24", 1, "0", -24.0, False, "flipped"),
]


def entity_rotation(angles):
    """r_bsp.c's R_RotateBmodel: roll * (pitch * yaw); local = M (world - origin)."""
    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    p, y, r = (math.radians(v) for v in angles)
    yaw = [[math.cos(y), math.sin(y), 0], [-math.sin(y), math.cos(y), 0], [0, 0, 1]]
    pitch = [[math.cos(p), 0, -math.sin(p)], [0, 1, 0], [math.sin(p), 0, math.cos(p)]]
    roll = [[1, 0, 0], [0, math.cos(r), math.sin(r)], [0, -math.sin(r), math.cos(r)]]
    return mul(roll, mul(pitch, yaw))


def world_of(ent, local):
    """The world-space point the entity's model sees at `local`."""
    if ENTS[ent] is None:
        return local
    origin, angles = ENTS[ent]
    m = entity_rotation(angles)		# orthonormal, so world = origin + M^T local
    return tuple(origin[j] + sum(m[i][j] * local[i] for i in range(3)) for j in range(3))


def run_c():
    eps = define("engine/hexen2/client.h", "DLIGHT_PLANE_EPSILON")
    consts = ["#define DLIGHT_PLANE_EPSILON %s" % eps]
    for hdr, tag in (("engine/h2shared/gl_model.h", "GL"), ("engine/h2shared/model.h", "SW")):
        for n in ("SURF_PLANEBACK", "SURF_DRAWTURB"):
            consts.append("#define %s_%s %s" % (tag, n, define(hdr, n)))
    sites = {
        "gl_add": prologue(GL_RSURF, "rad = cl_dlights[lnum].radius;"),
        "sw_add": prologue(R_SURF, "rad = fabs(cl_dlights[lnum].radius);"),
        "gl_mark": mark_loop(GL_RLIGHT),
        "sw_mark": mark_loop(R_LIGHT),
    }
    loops = {name: site_loop(rel, head) for name, rel, head, _ in SITES}
    fns = [angle_vectors(), helper()]
    if any(v is None for v in list(sites.values()) + list(loops.values()) + fns):
        return None
    # the GL path builds against gl_model.h, the software one against model.h
    tagged = {}
    for k, v in sites.items():
        tag = "GL" if k.startswith("gl") else "SW"
        tagged[k] = re.sub(r"\b(SURF_PLANEBACK|SURF_DRAWTURB)\b", tag + r"_\1", v)
    prog = [PRELUDE, "\n".join(consts)] + fns
    for name, _, _, var in SITES:
        prog.append(SITE % {"name": name, "var": var, "body": loops[name]})
    for k in ("gl_add", "sw_add"):
        prog.append(HARNESS % {"name": k, "body": tagged[k]})
    for k in ("gl_mark", "sw_mark"):
        prog.append(MARK % {"name": k, "body": tagged[k]})
    calls = []
    for name, cvar, flags, h, _, ent in CASES:
        wx, wy, wz = world_of(ent, LOCAL + (h,))
        if ent is None:
            setent = "entity_t *ent = NULL;"
        else:
            (ox, oy, oz), (ap, ay, ar) = ENTS[ent]
            setent = ("static entity_t E = {{%rf, %rf, %rf}, {%rf, %rf, %rf}}; entity_t *ent = &E;"
                      % (ox, oy, oz, ap, ay, ar))
        for tag in ("GL", "SW"):
            f = re.sub(r"\b(SURF_\w+)\b", tag + r"_\1", flags)
            low = tag.lower()
            # the world is marked by R_PushDlights with the light as it is
            # (check_calls pins that); an entity by each of its tier's sites
            ss = [None] if ent is None else [n for n, *_ in SITES if n.startswith(low)]
            for site in ss:
                calls.append(r"""
	{
		%(setent)s
		msurface_t *sp = &surfs[0]; float rad = -1, imp[3] = {0, 0, 0}; int a, m, n;
#define s (*sp)
		memset (&s, 0, sizeof(s));
		s.plane = &plane; s.flags = %(f)s; s.texinfo = &texinfo;
		s.texturemins[0] = s.texturemins[1] = -64; s.extents[0] = s.extents[1] = 128;
		r_dlight_occlusion.integer = %(cvar)d;
		cl_dlights[0].origin[0] = %(wx).9ff; cl_dlights[0].origin[1] = %(wy).9ff; cl_dlights[0].origin[2] = %(wz).9ff;
		cl_dlights[0].radius = 200; cl_dlights[0].minlight = 0; cl_dlights[0].die = 20;
		if (ent)
		{
			captured_n = 0; captured_ent = NULL;
			%(sitecall)s
			n = captured_n == 1 && captured_ent == ent;
		}
		else
		{
			captured = cl_dlights[0]; captured_ent = NULL; n = 1;
		}
		m = mark_%(low)s_mark (&captured, &s, captured_ent);
		/* R_AddDynamicLights reads the space R_MarkLights recorded; where
		 * this light did not mark it, an earlier one of the entity's would
		 * have */
		if (m != 1)
			s.dlightent = ent;
		s.dlightbits[0] = 1;
		a = add_%(low)s_add (&s, &rad, imp);
		printf ("%(tag)s %(name)s %(site)s %%d %%d %%d %%.4f %%.4f %%.4f %%.4f\n", a, m, n,
			a ? rad : 0.0, imp[0], imp[1], imp[2]);
#undef s
	}""" % dict(f=f, cvar=cvar, wx=wx, wy=wy, wz=wz, low=low, tag=tag, name=name,
                            site=site or "world", setent=setent,
                            sitecall="site_%s (ent);" % site if site else ";"))
    prog.append("int main (void)\n{" + "".join(calls) + "\n\treturn 0;\n}")
    src = "\n".join(prog)
    with tempfile.TemporaryDirectory() as work:
        c, exe = Path(work) / "onesided.c", Path(work) / "onesided"
        c.write_text(src)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(exe), str(c), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("the lifted C no longer compiles against the stubs:\n" + r.stderr[-2500:])
            return None
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def check_c(out):
    if out is None:
        return
    got = {}
    for line in out.split("\n"):
        if line.strip():
            tag, name, site, a, m, n, rad, ix, iy, iz = line.split()
            got[(tag, name, site)] = (int(a), int(m), int(n), float(rad), (float(ix), float(iy), float(iz)))
    files = {"GL": "gl_rsurf.c / gl_rlight.c", "SW": "r_surf.c / r_light.c"}
    for name, cvar, flags, h, want, ent in CASES:
        for tag in ("GL", "SW"):
            ss = ["world"] if ent is None else [n for n, *_ in SITES if n.startswith(tag.lower())]
            for site in ss:
                what = files[tag] + ("" if site == "world" else " via %s's loop" % site)
                a, m, n, rad, imp = got.get((tag, name, site), (-1, -1, -1, 0, (0, 0, 0)))
                print("%s %-20s %-8s cvar %d %-28s light %+6.1f: adds %s, marks %s%s" % (
                    tag, name, site, cvar, flags, h, "yes" if a else "no ", "yes" if m else "no ",
                    "  (radius left %g, impact %g %g %g)" % ((rad,) + imp) if a else ""))
                if not n:
                    errors.append("%s, %s: the site must hand R_MarkLights the light once, with its entity"
                                  % (what, name))
                if a != want:
                    errors.append("%s R_AddDynamicLights, %s: %s, want %s" % (
                        what, name, "lights" if a else "drops the light", "lit" if want else "dropped"))
                if m == 2:
                    errors.append("%s R_MarkLights, %s: marks the surface without recording its entity "
                                  "(dlightent), the space R_AddDynamicLights measures in" % (what, name))
                elif m != want:
                    errors.append("%s R_MarkLights, %s: %s, want %s" % (
                        what, name, "marks" if m else "does not mark", "marked" if want else "not marked"))
                if a and want:
                    if abs(rad - (200 - abs(h))) > 1e-2:
                        errors.append("%s R_AddDynamicLights, %s: radius left %g, want %g (the lit side keeps "
                                      "fabs(dist), in the surface's model space)" % (what, name, rad, 200 - abs(h)))
                    if max(abs(x - y) for x, y in zip(imp, LOCAL + (0.0,))) > 1e-2:
                        errors.append("%s R_AddDynamicLights, %s: impact %s, want %s (the light's foot on the "
                                      "plane, in model space)" % (what, name, imp, LOCAL + (0.0,)))


def check_calls():
    """R_MarkLights takes the entity; the world is marked with NULL; and every
    call outside the two R_MarkLights files is one of the sites run above."""
    for rel in (GL_RLIGHT, R_LIGHT):
        fn = body_of(strip_comments(read(rel)), r"\nvoid R_PushDlights \(void\)\s*\{") or ""
        if not re.search(r"R_MarkLights \(l, i, cl\.worldmodel->nodes, NULL\);", fn):
            errors.append("%s: R_PushDlights must mark the world with a NULL entity (world space)" % rel)
    for rel in (GL_RLIGHT, R_LIGHT, "engine/h2shared/glquake.h", "engine/h2shared/r_local.h",
                "engine/hexen2/gl_rmain.c"):
        if MARK_SIG not in re.sub(r"[ \t]+", " ", read(rel)):
            errors.append("%s: R_MarkLights must be %s" % (rel, MARK_SIG))
    want = {GL_RLIGHT, R_LIGHT} | {rel for _, rel, _, _ in SITES}
    stray = set()
    for d in ("engine/hexen2", "engine/h2shared"):
        for p in sorted((ROOT / d).glob("*.c")):
            rel = "%s/%s" % (d, p.name)
            if rel not in want and re.search(r"\bR_MarkLights \(", strip_comments(p.read_text(errors="replace"))):
                stray.add(rel)
    if stray:
        errors.append("R_MarkLights is called from %s, which this test does not run" % ", ".join(sorted(stray)))
    for rel in (GL_RSURF, R_SURF):
        fn = body_of(read(rel), r"static void R_AddDynamicLights \([^)]*\)\s*\{") or ""
        if len(re.findall(r"cl_dlights\[lnum\]\.origin", strip_comments(fn))) != 1:
            errors.append("%s: R_AddDynamicLights must read the light's world origin once, into "
                          "R_DlightModelOrigin, and use the model-space one after" % rel)
    # The run's reference is R_RotateBmodel's matrix.  The GL draw must be the
    # same transform: R_RotateForEntity's T * Rz(yaw) * Ry(-pitch) * Rx(-roll),
    # entered with pitch and roll negated by both brush paths, nets to
    # Rz(yaw) Ry(pitch) Rx(roll), whose axes are AngleVectors' forward, -right, up.
    rmain = strip_comments(read("engine/hexen2/gl_rmain.c"))
    rfe = body_of(rmain, r"\nvoid R_RotateForEntity \(entity_t \*e\)\s*\{") or ""
    if not re.fullmatch(r"\{\s*GL_Translatef \(e->origin\[0\], e->origin\[1\], e->origin\[2\]\);\s*"
                        r"GL_Rotatef \(e->angles\[1\], 0, 0, 1\);\s*GL_Rotatef \(-e->angles\[0\], 0, 1, 0\);\s*"
                        r"GL_Rotatef \(-e->angles\[2\], 1, 0, 0\);\s*\}", rfe):
        errors.append("gl_rmain.c: R_RotateForEntity is no longer T * Rz(yaw) * Ry(-pitch) * Rx(-roll), the "
                      "transform R_DlightModelOrigin inverts (with the brush paths' pitch/roll negation)")
    for rel, head, var in (
            (GL_RSURF, r"void R_DrawBrushModel \(entity_t \*e, qboolean Translucent\)\s*\{", r"e->angles"),
            ("engine/hexen2/gl_rmain.c", r"void R_CollectBrushInstances \(void\)\s*\{", r"eview->angles")):
        fn = strip_comments(body_of(read(rel), head) or "")
        v = re.escape(var)
        if not re.search(v + r"\[0\] = -.*?" + v + r"\[2\] = -.*?R_RotateForEntity \(\w+\);", fn, re.S):
            errors.append("%s: the brush path must negate pitch and roll before R_RotateForEntity, or the "
                          "drawn model and R_DlightModelOrigin's model space disagree" % rel)


# ------------------------------------------------------------------ the GPU
def check_gpu():
    dl = strip_comments(read("engine/shaders/world_dlight.inc"))
    fn = body_of(dl, r"vec3 ClusteredDlight\(vec3 lm, highp vec3 p\)\s*\{") or ""
    if not fn:
        errors.append("world_dlight.inc: no ClusteredDlight")
        return
    if not re.search(r"highp vec3 N = normalize\(cross\(dFdx\(p\), dFdy\(p\)\)\);\s*"
                     r"if \(dot\(N, p\) > 0\.0\) N = -N;\s*uvec4 mask = LightGridMask\(p\);", fn):
        errors.append("ClusteredDlight must turn the derivative normal toward the eye (eye space: dot(N, p) > 0 "
                      "means N faces away) right after computing it, before the mask and before any use")
    if len(re.findall(r"\bN\s*=", fn)) != 2:
        errors.append("ClusteredDlight must assign N exactly twice: computed, then turned toward the eye")
    if not re.search(r"float pdist = dot\(lpos - p, N\);\s*"
                     r"if \(u_dlight_onesided > 0\.0 && pdist < -u_dlight_onesided\) continue;\s*"
                     r"float rad = Lights\[i\]\.radius - abs\(pdist\);", fn):
        errors.append("ClusteredDlight must drop a light more than u_dlight_onesided behind the face "
                      "(gated on the lane > 0) between pdist and the falloff, which keeps abs(pdist)")
    if len(re.findall(r"\bu_dlight_onesided\b", fn)) != 2:
        errors.append("ClusteredDlight must read u_dlight_onesided exactly in the one test")
    winc = read("engine/shaders/world.inc")
    if not re.search(r"^#define u_dlight_onesided u_wfgridz\.w\s*$", winc, re.M):
        errors.append("world.inc must alias u_dlight_onesided to u_wfgridz.w (the spare lane)")
    uc = strip_comments(read("engine/h2shared/gl_uniforms.c"))
    wd = body_of(uc, r"\nvoid R_SetWorldDlight \([^)]*float onesided\)\s*\{") or ""
    if not re.search(r"RU_Set4 \(worldframe\.gridz, zlogscale, zlogbias, scale, onesided\);", wd) \
            or "worldframe_onesided = onesided;" not in wd:
        errors.append("R_SetWorldDlight must take the tolerance and write it to gridz.w and worldframe_onesided")
    wl = body_of(uc, r"\nvoid R_SetWorldLiquid \(qboolean liquid\)\s*\{") or ""
    if not re.fullmatch(r"\{\s*worldframe\.gridz\[3\] = liquid \? 0\.0f : worldframe_onesided;\s*\}", wl):
        errors.append("R_SetWorldLiquid must write 0 (both sides) for a liquid and the frame's tolerance after")
    lc = strip_comments(read("engine/h2shared/gl_lightcluster.c"))
    bfw = body_of(lc, r"void R_LightCluster_BindForWorld \(void\)\s*\{") or ""
    if not re.search(r"R_SetWorldDlight \(scale, lc_view, lc_vp, lc_zlogscale, lc_zlogbias,\s*"
                     r"r_dlight_occlusion\.integer >= 1 \? DLIGHT_PLANE_EPSILON : 0\.0f\);", bfw):
        errors.append("R_LightCluster_BindForWorld must pass DLIGHT_PLANE_EPSILON under r_dlight_occlusion >= 1, "
                      "0 otherwise")
    warp = strip_comments(read("engine/h2shared/gl_warp.c"))
    ewp = body_of(warp, r"void EmitWaterPolys \(msurface_t \*fa\)\s*\{") or ""
    if not re.search(r"R_SetWorldLiquid \(true\);.*?GL_ImmBegin \(\);.*?GL_ImmEnd \(GL_TRIANGLES, prog\);"
                     r".*?R_SetWorldLiquid \(false\);", ewp, re.S) \
            or re.search(r"\breturn\b", ewp):
        errors.append("EmitWaterPolys must bracket its batch with R_SetWorldLiquid (true) / (false), "
                      "with no return in between")


# ------------------------------------------------------------------ the cvar
def check_cvar():
    for rel in ("engine/hexen2/gl_rmain.c", "engine/hexen2/r_soft_web.c"):
        if not re.search(r'^cvar_t\s+r_dlight_occlusion = \{"r_dlight_occlusion", "1", CVAR_ARCHIVE\};',
                         read(rel), re.M):
            errors.append('%s must define r_dlight_occlusion as "1", CVAR_ARCHIVE' % rel)
    if "Cvar_RegisterVariable (&r_dlight_occlusion);" not in read("engine/hexen2/gl_rmisc.c"):
        errors.append("gl_rmisc.c must register r_dlight_occlusion")
    sw = body_of(strip_comments(read("engine/hexen2/r_soft_web.c")), r"void R_SoftWebInitCvars \(void\)\s*\{") or ""
    if "Cvar_RegisterVariable (&r_dlight_occlusion);" not in sw:
        errors.append("R_SoftWebInitCvars must register r_dlight_occlusion")
    if not re.search(r"extern\s+cvar_t\s+r_dlight_occlusion;", read("engine/h2shared/glquake.h")) \
            or not re.search(r"extern\s+cvar_t\s+r_dlight_occlusion;", read("engine/h2shared/r_local.h")):
        errors.append("glquake.h and r_local.h must declare r_dlight_occlusion")
    eps = re.findall(r"#define\s+DLIGHT_PLANE_EPSILON\s+(\S+)",
                     "".join(p.read_text(errors="replace") for d in ("engine/hexen2", "engine/h2shared")
                             for p in sorted((ROOT / d).glob("*.[ch]"))))
    if eps != ["2.0f"]:
        errors.append("DLIGHT_PLANE_EPSILON must be defined once, as 2.0f (found %s)" % eps)


check_c(run_c())
check_calls()
check_gpu()
check_cvar()
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(dict.fromkeys(errors)))
print("PASS a light behind a face does not light it: CPU twins (add + mark) and the three brush-entity sites "
      "run, in model space (#495); GPU test, lane, writer, liquid bracket and cvar pinned (GitHub #97)")
