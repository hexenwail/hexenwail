#!/usr/bin/env python3
"""Entities sharing a brush submodel are each lit by their own light (GitHub #496).

On the CPU dynamic-light path (GLES/WebGL2, WEBSOFT, desktop r_lightclusters 0)
a light on a brush entity's face is measured in that entity's model space
(#495).  Two entities that share a `*N` submodel -- a progs doing
setmodel (e2, e1.model) -- share its surfaces, and msurface_t.dlightent kept
only the last entity that marked one: the other entity's light was measured in
the wrong space, misplaced, and with the one-sided test (#97) dropped.  No
shipped Hexen II progs shares a brush submodel (every setmodel (a, b.model) in
gamecode/hc copies a player's alias model), so the repro is this lint, not a
map.

engine/h2shared/r_dlightmark.c records which entity brought which light to a
shared surface.  It is compiled here whole, twice -- against the GL model
layer's SURF_* values and the software one's -- with each tier's R_MarkLights
surface loop (gl_rlight.c / r_light.c, the live version) and its
R_AddDynamicLights prologue (gl_rsurf.c / r_surf.c, `rad = ...` through the
impact loop) lifted verbatim, and R_DlightModelOrigin (client.h) with the
AngleVectors it calls.  The marking sites hand R_MarkLights the light in the
entity's model space; tools/test_dlight_onesided.py runs those three sites, so
here the harness does what they do: R_DlightModelOrigin, then the one-node
descent's plane reject, then the surface loop.  One face, z = 0 in model
space, 128 x 128 texels; lights of radius 200.

  apart     A at the origin, B 1000 away and turned 90: each light beside its
            own copy.  The software loop marks both lights through both
            copies (it has no reach test), the GL one each through its own.
  stacked   B under A, rolled over: each light is behind the other copy's
            face, so measuring it there drops it (the one-sided test).
  overlap   one light within reach of both copies: measured in the copy whose
            face it is nearest (R_MarkLights' reach measure), A here, in
            either marking order.
  interleave  A, B, then A again with a light it had not brought: every
            (entity, light) pair that marked is in the records, exactly.
  behind    B 18 under A, rolled over: A's light is 6 units from B's face but
            behind it, so B never marks it; only the copies that brought a
            light are candidates, however near another copy is.
  single    one entity: no records at all (the common case costs nothing).

Every case runs in both marking orders.  For each light: it lights the face,
with radius left 200 - |h| and its foot on the plane at its own copy's
model-space (x, y).  The records are compared with the (entity, light) pairs
that a fresh surface says marked, in first-marked order.

  frames    with the pool at 2 records, a conflict in frame 7 and another in
            frame 8 both fit (the pool restarts with r_dlightframecount), a
            frame-9 surface marked by one entity has no records left over, and
            after R_DlightMark_NewMap a conflict in a frame 8 again fits (a
            new map starts r_framecount over and can meet the old stamp).
  overflow  with the pool at 1, the surface is ambiguous: R_DlightSurfaceOrigin
            returns false, the light is measured in dlightent's space and the
            one-sided test is skipped, so the stacked case's lights are
            misplaced but not dropped.

Pinned too: the reset path clears dlightmarks, R_DlightMark_Shared runs before
the bit is set, R_AddDynamicLights asks R_DlightSurfaceOrigin and gates the
one-sided test on its answer, and r_dlightmark.c is in both renderers' source
lists.

    python3 tools/test_dlight_shared_submodel.py
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

GL_RSURF, R_SURF = "engine/hexen2/gl_rsurf.c", "engine/h2shared/r_surf.c"
GL_RLIGHT, R_LIGHT = "engine/h2shared/gl_rlight.c", "engine/h2shared/r_light.c"
MARKC, MARKH = "engine/h2shared/r_dlightmark.c", "engine/h2shared/r_dlightmark.h"
CLIENT_H, MATHLIB = "engine/hexen2/client.h", "engine/h2shared/mathlib.c"
MARK_SIG = "void R_MarkLights (dlight_t *light, int lnum, mnode_t *node, const entity_t *ent)"


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


def braced(text, at):
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    return None


def define(rel, name):
    m = re.search(r"#define\s+%s\s+(\S+)" % name, read(rel))
    if not m:
        errors.append("%s: no #define %s" % (rel, name))
        return "0"
    return m.group(1)


def live(text):
    m = re.search(r"#if 0\s*/\* the original version from ID \*/(.*?)#else", text, re.S)
    return text[:m.start()] + text[m.end():] if m else text


def mark_loop(rel):
    fn = body_of(live(read(rel)), r"\n%s\s*\{" % re.escape(MARK_SIG))
    a = fn.find("for (i = 0; i < node->numsurfaces; i++, surf++)") if fn else -1
    if a < 0:
        errors.append("%s: no R_MarkLights surface loop" % rel)
        return None
    return braced(fn, a)


def prologue(rel, first):
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


def dlightmark_c(pool):
    """r_dlightmark.c whole, its includes dropped and its header's body put in
    their place; `pool` overrides DM_POOL when given."""
    src, hdr = read(MARKC), read(MARKH)
    if not src or not hdr:
        errors.append("r_dlightmark.c / r_dlightmark.h missing")
        return None
    if not re.search(r"^#define\s+DM_POOL\s+\d+\s*$", src, re.M):
        errors.append("r_dlightmark.c: no #define DM_POOL <n>")
        return None
    hdr = re.sub(r"^#(ifndef|define)\s+R_DLIGHTMARK_H.*$|^#endif\s+/\* R_DLIGHTMARK_H \*/$", "", hdr, flags=re.M)
    src = re.sub(r"^#if defined\(WEBSOFT\)\n#include \"r_local.h\"\n#endif\n", "", src, flags=re.M)
    src = re.sub(r"^#include .*$", "", src, flags=re.M)
    if pool is not None:
        src = re.sub(r"^#define\s+DM_POOL\s+\d+", "#define DM_POOL %d" % pool, src, flags=re.M)
    return hdr + "\n" + src


PRELUDE = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef float vec_t;
typedef vec_t vec3_t[3];
typedef int qboolean;
#define true 1
#define false 0
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
static int con_dprints;
#define Con_DPrintf(...) (con_dprints++)
typedef struct entity_s { vec3_t origin, angles; } entity_t;
typedef struct { vec3_t normal; float dist; int type; } mplane_t;
typedef struct { float vecs[2][4]; } mtexinfo_t;
typedef struct msurface_s { mplane_t *plane; int flags; mtexinfo_t *texinfo; int texturemins[2], extents[2];
                 int dlightframe; unsigned int dlightbits[4]; const struct entity_s *dlightent;
                 struct dlightmark_s *dlightmarks; } msurface_t;
typedef struct mnode_s { int numsurfaces; } mnode_t;
typedef struct { vec3_t origin; float radius, minlight, die; } dlight_t;
typedef struct { int integer; } cvar_t;
static cvar_t r_dlight_occlusion = {1};
static dlight_t cl_dlights[4];
static int r_dlightframecount = 7;
static mplane_t plane = {{0, 0, 1}, 0, 2};
static mtexinfo_t texinfo = {{{1, 0, 0, 0}, {0, 1, 0, 0}}};
static int R_DlightVis_Surface (int lnum, msurface_t *surf) { (void)lnum; (void)surf; return 1; }
"""

# R_MarkLights on a one-node tree whose plane is the face's: what a site hands
# it (the light in the entity's model space), the descent's reject, the loop.
MARK = r"""
static void mark (msurface_t *surf, const entity_t *ent, int lnum)
{
	dlight_t local = cl_dlights[lnum], *light = &local;
	mnode_t real_node = {1}, *node = &real_node;
	float l, maxdist, dist; int i, j, s, t; vec3_t impact;
	(void)l; (void)j; (void)s; (void)t; (void)impact;
	R_DlightModelOrigin (ent, cl_dlights[lnum].origin, local.origin);
	maxdist = light->radius * light->radius;
	dist = DotProduct (light->origin, plane.normal) - plane.dist;
	if (dist > light->radius || dist < -light->radius)
		return;
%(body)s
}
"""

ADD = r"""
static int add (msurface_t *surf, int L, float *out_rad, float *out_impact, int *out_known)
{
	int lnum, i; float dist, rad, minlight; vec3_t impact, lorg; qboolean known = -1;
	for (lnum = L; lnum < L + 1; lnum++)
	{
%(body)s
		*out_rad = rad; VectorCopy (impact, out_impact); *out_known = known; (void)minlight; return 1;
	}
	*out_known = known;
	return 0;
}
"""

MAIN_HEAD = r"""
static entity_t ents[3];
static msurface_t S;
static void fresh (msurface_t *s)
{
	memset (s, 0, sizeof(*s));
	s->plane = &plane; s->flags = 0; s->texinfo = &texinfo;
	s->texturemins[0] = s->texturemins[1] = -64; s->extents[0] = s->extents[1] = 128;
}
/* would (ent, lnum) mark a surface on its own? */
static int marks (int e, int lnum)
{
	msurface_t t; fresh (&t); t.dlightframe = -1;
	mark (&t, &ents[e], lnum);
	return DLIGHTBIT_TEST (t.dlightbits, lnum) ? 1 : 0;
}
static void dump (const char *tag, int nlights)
{
	const dlightmark_t *m; int k, e;
	for (k = 0; k < nlights; k++)
	{
		float rad = 0, imp[3] = {0, 0, 0}; int known = -1, a = 0;
		if (DLIGHTBIT_TEST (S.dlightbits, k))
			a = add (&S, k, &rad, imp, &known);
		printf ("L %s %d %d %d %d %.4f %.4f %.4f %.4f\n", tag, k, DLIGHTBIT_TEST (S.dlightbits, k) ? 1 : 0,
			a, known, a ? rad : 0.0, imp[0], imp[1], imp[2]);
	}
	for (e = 0; e < 3; e++)
		if (S.dlightent == &ents[e])
			printf ("O %s %d\n", tag, e);
	if (S.dlightmarks == &dm_unknown)
		printf ("U %s\n", tag);
	else
		for (m = S.dlightmarks; m; m = m->next)
		{
			for (e = 0; e < 3 && m->ent != &ents[e]; e++)
				;
			printf ("R %s %d %u\n", tag, e, m->bits[0]);
		}
	for (e = 0; e < 3; e++)
		for (k = 0; k < nlights; k++)
			printf ("M %s %d %d %d\n", tag, e, k, marks (e, k));
}
"""

# Entities as (origin, angles); lights as (owner, its model-space position).
ENTS = {
    "apart": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((1000.0, 0.0, 0.0), (0.0, 90.0, 0.0))],
    "stacked": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((0.0, 0.0, -48.0), (0.0, 0.0, 180.0))],
    "overlap": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((100.0, 0.0, 0.0), (0.0, 0.0, 0.0))],
    "interleave": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((0.0, 0.0, -48.0), (0.0, 0.0, 180.0))],
    "behind": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((0.0, 0.0, 18.0), (0.0, 0.0, 180.0))],
    "single": [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((0.0, 0.0, -48.0), (0.0, 0.0, 180.0))],
}
LIGHTS = {
    "apart": [(0, (10.0, 20.0, 24.0)), (1, (-30.0, 5.0, 40.0))],
    "stacked": [(0, (10.0, 20.0, 24.0)), (1, (-30.0, 5.0, 40.0))],
    "overlap": [(0, (30.0, 20.0, 24.0))],                 # B sees it at (-70, 20, 24): off its face
    "interleave": [(0, (10.0, 20.0, 24.0)), (1, (-30.0, 5.0, 40.0)), (0, (-40.0, -10.0, 16.0))],
    "behind": [(0, (10.0, 20.0, 24.0)), (1, (-30.0, 5.0, 40.0))],   # light 0 is B-local (10, -20, -6)
    "single": [(0, (10.0, 20.0, 24.0)), (0, (-50.0, 30.0, 60.0))],
}
# marking sequences: (entity, [lights]) in the order the sites would run
SEQS = {
    "apart": {"AB": [(0, [0, 1]), (1, [0, 1])], "BA": [(1, [0, 1]), (0, [0, 1])]},
    "stacked": {"AB": [(0, [0, 1]), (1, [0, 1])], "BA": [(1, [0, 1]), (0, [0, 1])]},
    "overlap": {"AB": [(0, [0]), (1, [0])], "BA": [(1, [0]), (0, [0])]},
    "interleave": {"ABA": [(0, [0]), (1, [1]), (0, [2])], "BAB": [(1, [1]), (0, [0, 2]), (1, [1])]},
    "behind": {"AB": [(0, [0, 1]), (1, [0, 1])], "BA": [(1, [0, 1]), (0, [0, 1])]},
    "single": {"A": [(0, [0, 1])], "AA": [(0, [0, 1]), (0, [0, 1])]},
}


def entity_rotation(angles):
    """r_bsp.c's R_RotateBmodel: roll * (pitch * yaw); local = M (world - origin)."""
    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    p, y, r = (math.radians(v) for v in angles)
    yaw = [[math.cos(y), math.sin(y), 0], [-math.sin(y), math.cos(y), 0], [0, 0, 1]]
    pitch = [[math.cos(p), 0, -math.sin(p)], [0, 1, 0], [math.sin(p), 0, math.cos(p)]]
    roll = [[1, 0, 0], [0, math.cos(r), math.sin(r)], [0, -math.sin(r), math.cos(r)]]
    return mul(roll, mul(pitch, yaw))


def to_world(ent, local):
    origin, angles = ent
    m = entity_rotation(angles)
    return tuple(origin[j] + sum(m[i][j] * local[i] for i in range(3)) for j in range(3))


def to_local(ent, world):
    origin, angles = ent
    m = entity_rotation(angles)
    d = [world[j] - origin[j] for j in range(3)]
    return tuple(sum(m[i][j] * d[j] for j in range(3)) for i in range(3))


def reach2(local):
    """R_MarkLights' reach measure on the 128 x 128 face from -64."""
    def over(v):
        return v + 64 if v < -64 else v - 64 if v > 64 else 0.0
    return over(local[0]) ** 2 + over(local[1]) ** 2 + local[2] ** 2


def program(tier, pool, body):
    consts = ["#define DLIGHT_PLANE_EPSILON %s" % define(CLIENT_H, "DLIGHT_PLANE_EPSILON")]
    hdr = "engine/h2shared/gl_model.h" if tier == "GL" else "engine/h2shared/model.h"
    for n in ("SURF_PLANEBACK", "SURF_DRAWTURB"):
        consts.append("#define %s %s" % (n, define(hdr, n)))
    loop = mark_loop(GL_RLIGHT if tier == "GL" else R_LIGHT)
    pro = prologue(GL_RSURF, "rad = cl_dlights[lnum].radius;") if tier == "GL" \
        else prologue(R_SURF, "rad = fabs(cl_dlights[lnum].radius);")
    parts = [angle_vectors(), helper(), dlightmark_c(pool), loop, pro]
    if any(p is None for p in parts):
        return None
    return "\n".join([PRELUDE] + consts + parts[:3] + [MARK % {"body": loop}, ADD % {"body": pro}, MAIN_HEAD,
                                                         "int main (void)\n{\n" + body + "\n\treturn 0;\n}\n"])


def run(src, name):
    with tempfile.TemporaryDirectory() as work:
        c, exe = Path(work) / (name + ".c"), Path(work) / name
        c.write_text(src)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(exe), str(c), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("%s: the lifted C no longer compiles against the stubs:\n%s" % (name, r.stderr[-2500:]))
            return None
        # A record list that loops back on itself (a pool handing one record
        # out twice) would spin the harness forever; that is a FAIL, not a hang.
        try:
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            errors.append("%s: the harness did not finish in 20 s (a cycle in a surface's records?)" % name)
            return None
        if r.returncode:
            errors.append("%s: the harness exited %d:\n%s" % (name, r.returncode, r.stderr[-1500:]))
            return None
        return r.stdout


def setup_c(case):
    lines = []
    for i, (o, a) in enumerate(ENTS[case]):
        lines.append("\tents[%d] = (entity_t){{%rf, %rf, %rf}, {%rf, %rf, %rf}};" % ((i,) + o + a))
    for k, (owner, local) in enumerate(LIGHTS[case]):
        w = to_world(ENTS[case][owner], local)
        lines.append("\tcl_dlights[%d].origin[0] = %.9ff; cl_dlights[%d].origin[1] = %.9ff; "
                     "cl_dlights[%d].origin[2] = %.9ff; cl_dlights[%d].radius = 200; cl_dlights[%d].minlight = 0;"
                     % (k, w[0], k, w[1], k, w[2], k, k))
    return "\n".join(lines)


def seq_c(seq):
    out = []
    for e, ks in seq:
        for k in ks:
            out.append("\tmark (&S, &ents[%d], %d);" % (e, k))
    return "\n".join(out)


def parse(out):
    got = {}
    for line in (out or "").split("\n"):
        f = line.split()
        if not f:
            continue
        d = got.setdefault(f[1], {"L": {}, "O": None, "U": False, "R": [], "M": {}})
        if f[0] == "L":
            d["L"][int(f[2])] = (int(f[3]), int(f[4]), int(f[5]), float(f[6]),
                                 (float(f[7]), float(f[8]), float(f[9])))
        elif f[0] == "O":
            d["O"] = int(f[2])
        elif f[0] == "U":
            d["U"] = True
        elif f[0] == "R":
            d["R"].append((int(f[2]), int(f[3])))
        elif f[0] == "M":
            d["M"][(int(f[2]), int(f[3]))] = int(f[4])
    return got


def want_records(case, seq, m):
    """(entity, bits) in first-marked order, from the pairs a fresh surface marks."""
    order, bits = [], {}
    for e, ks in seq:
        for k in ks:
            if m.get((e, k)):
                if e not in bits:
                    order.append(e)
                    bits[e] = 0
                bits[e] |= 1 << k
    return [(e, bits[e]) for e in order]


def owner_of(case, k, m, seq):
    """The entity whose space light k must be measured in: of those that marked
    it, the one whose face it is nearest; ties to the first marked."""
    best = None
    seen = []
    for e, ks in seq:
        if k in ks and m.get((e, k)) and e not in seen:
            seen.append(e)
    w = to_world(ENTS[case][LIGHTS[case][k][0]], LIGHTS[case][k][1])
    for e in seen:
        d = reach2(to_local(ENTS[case][e], w))
        if best is None or d < best[0]:
            best = (d, e)
    return best[1] if best else None


def check_main(tier, got):
    for case in SEQS:
        for oname, seq in SEQS[case].items():
            tag = "%s_%s" % (case, oname)
            d = got.get(tag)
            where = "%s %s, order %s" % (tier, case, oname)
            if not d:
                errors.append("%s: no output" % where)
                continue
            m = d["M"]
            marked = sorted({e for e, ks in seq for k in ks if m.get((e, k))})
            for k in range(len(LIGHTS[case])):
                bit, a, known, rad, imp = d["L"][k]
                own = owner_of(case, k, m, seq)
                if own is None:
                    errors.append("%s: light %d marks the face through no entity; the case is wrong" % (where, k))
                    continue
                if own != LIGHTS[case][k][0]:
                    errors.append("%s: light %d resolves to entity %d, the case says %d" % (
                        where, k, own, LIGHTS[case][k][0]))
                local = LIGHTS[case][k][1]
                print("%s %-10s %-3s light %d (copy %s): %s, known %d, radius left %g, foot %g %g %g" % (
                    tier, case, oname, k, "AB"[own], "lit" if a else "DROPPED", known, rad, *imp))
                if not a:
                    errors.append("%s: light %d is dropped (measured in another copy's space?)" % (where, k))
                    continue
                if known != 1:
                    errors.append("%s: light %d: R_DlightSurfaceOrigin says the space is unknown" % (where, k))
                if abs(rad - (200 - abs(local[2]))) > 1e-2:
                    errors.append("%s: light %d: radius left %g, want %g (its height over its own copy's face)"
                                  % (where, k, rad, 200 - abs(local[2])))
                if max(abs(x - y) for x, y in zip(imp, (local[0], local[1], 0.0))) > 1e-2:
                    errors.append("%s: light %d: foot %s, want %s (in its own copy's model space)"
                                  % (where, k, imp, (local[0], local[1], 0.0)))
            first = next(e for e, ks in seq for k in ks if m.get((e, k)))
            if d["O"] != first:
                errors.append("%s: dlightent is entity %s, want %d (the first to mark)" % (where, d["O"], first))
            if d["U"]:
                errors.append("%s: the surface went ambiguous with the pool at its full size" % where)
            elif len(marked) == 1:
                if d["R"]:
                    errors.append("%s: one entity marked the face, yet it has records %s" % (where, d["R"]))
            elif d["R"] != want_records(case, seq, m):
                errors.append("%s: records %s, want %s (each (entity, light) pair that marked, first-marked "
                              "order)" % (where, d["R"], want_records(case, seq, m)))


def main_body():
    body = []
    for case in SEQS:
        for oname, seq in SEQS[case].items():
            body.append("\t{\n%s\n\tfresh (&S); r_dlightframecount++;\n%s\n\tdump (\"%s_%s\", %d);\n\t}" % (
                setup_c(case), seq_c(seq), case, oname, len(LIGHTS[case])))
    return "\n".join(body)


def frames_body():
    """pool 2: a conflict in each of frames 7 and 8 fits; frame 9, one entity;
    then a new map whose frame count meets 8 again: it fits too."""
    st = SEQS["stacked"]["AB"]
    return "\n".join([
        setup_c("stacked"), "\tfresh (&S);",
        "\tr_dlightframecount = 7;", seq_c(st), '\tdump ("f7", 2);',
        "\tr_dlightframecount = 8;", seq_c(st), '\tdump ("f8", 2);',
        "\tr_dlightframecount = 9;", seq_c([(0, [0, 1])]), '\tdump ("f9", 2);',
        "\tR_DlightMark_NewMap (); fresh (&S); r_dlightframecount = 8;", seq_c(st), '\tdump ("n8", 2);',
        '\tprintf ("P %d\\n", con_dprints);'])


def overflow_body():
    return "\n".join([setup_c("stacked"), "\tfresh (&S); r_dlightframecount = 7;",
                      seq_c(SEQS["stacked"]["AB"]), '\tdump ("ov", 2);', '\tprintf ("P %d\\n", con_dprints);'])


def check_frames(tier, out):
    got = parse(out)
    for f in ("f7", "f8", "n8"):
        d = got.get(f)
        if not d:
            errors.append("%s frames: no %s output" % (tier, f))
            continue
        if d["U"] or [r for r in d["R"]] != [(0, 1), (1, 2)]:
            errors.append("%s frames, %s: records %s%s, want [(0, 1), (1, 2)] -- the pool must restart with "
                          "each r_dlightframecount" % (tier, f, d["R"], " (ambiguous)" if d["U"] else ""))
        for k in (0, 1):
            if d["L"].get(k, (0, 0, 0))[2] != 1:
                errors.append("%s frames, %s: light %d not in a known space" % (tier, f, k))
    d = got.get("f9")
    if not d or d["U"] or d["R"] or d["O"] != 0:
        errors.append("%s frames, f9: a surface one entity marked must start the frame with no records and "
                      "dlightent that entity (stale records from frame 8 left over?)" % tier)
    elif d["L"][0][1] != 1 or d["L"][0][2] != 1:
        errors.append("%s frames, f9: light 0 not lit in a known space" % tier)
    if "P 0" not in (out or ""):
        errors.append("%s frames: the overflow warning fired with the pool big enough" % tier)


def check_overflow(tier, out):
    got = parse(out)
    d = got.get("ov")
    if not d:
        errors.append("%s overflow: no output" % tier)
        return
    if not d["U"]:
        errors.append("%s overflow: with the pool at 1 the surface must go ambiguous (dm_unknown)" % tier)
    for k, (owner, local) in enumerate(LIGHTS["stacked"]):
        bit, a, known, rad, imp = d["L"][k]
        print("%s overflow   light %d: %s, known %d" % (tier, k, "lit" if a else "DROPPED", known))
        if known != 0:
            errors.append("%s overflow: light %d: R_DlightSurfaceOrigin must say the space is unknown" % (tier, k))
        if not a:
            errors.append("%s overflow: light %d dropped; an unknown space must skip the one-sided test" % (tier, k))
    # dlightent stays the first marker, so its own light is still right
    if d["L"][0][1] and max(abs(x - y) for x, y in zip(d["L"][0][4], (10.0, 20.0, 0.0))) > 1e-2:
        errors.append("%s overflow: light 0 should still be measured in dlightent's (A's) space" % tier)
    if "P 1" not in out:
        errors.append("%s overflow: Con_DPrintf the overflow once" % tier)


def check_source():
    for rel in (GL_RLIGHT, R_LIGHT):
        loop = strip_comments(mark_loop(rel) or "")
        reset = re.search(r"if \(surf->dlightframe != r_dlightframecount\)\s*\{(.*?)\}", loop, re.S)
        if not reset or "surf->dlightent = ent;" not in reset.group(1) \
                or "surf->dlightmarks = NULL;" not in reset.group(1):
            errors.append("%s: R_MarkLights' reset must set dlightent and clear dlightmarks" % rel)
        if not re.search(r"\}\s*else if \(ent != surf->dlightent \|\| surf->dlightmarks\)\s*"
                         r"R_DlightMark_Shared \(surf, lnum, ent, r_dlightframecount\);\s*"
                         r"DLIGHTBIT_SET \(surf->dlightbits, lnum\);", loop):
            errors.append("%s: R_MarkLights must call R_DlightMark_Shared for another entity (or once records "
                          "exist), before setting the bit" % rel)
        if len(re.findall(r"surf->dlightent\s*=", loop)) != 1:
            errors.append("%s: R_MarkLights may set dlightent only in the reset" % rel)
        if '#include "r_dlightmark.h"' not in read(rel):
            errors.append("%s must include r_dlightmark.h" % rel)
    for rel in (GL_RSURF, R_SURF):
        fn = strip_comments(body_of(read(rel), r"static void R_AddDynamicLights \([^)]*\)\s*\{") or "")
        if "known = R_DlightSurfaceOrigin (surf, lnum, cl_dlights[lnum].origin, lorg);" not in fn:
            errors.append("%s: R_AddDynamicLights must take lorg from R_DlightSurfaceOrigin" % rel)
        if "dlightent" in fn:
            errors.append("%s: R_AddDynamicLights must not read dlightent itself" % rel)
        if not re.search(r"if \(known && r_dlight_occlusion\.integer >= 1", fn):
            errors.append("%s: the one-sided test must be gated on R_DlightSurfaceOrigin's answer" % rel)
        if '#include "r_dlightmark.h"' not in read(rel):
            errors.append("%s must include r_dlightmark.h" % rel)
    for rel in ("engine/h2shared/gl_model.h", "engine/h2shared/model.h"):
        if not re.search(r"struct dlightmark_s\s+\*dlightmarks;", read(rel)):
            errors.append("%s: msurface_t needs struct dlightmark_s *dlightmarks" % rel)
    for rel in ("engine/hexen2/gl_rmisc.c", "engine/hexen2/r_main.c"):
        if not re.search(r"R_DlightVis_NewMap \(\);[^\n]*\n\s*R_DlightMark_NewMap \(\);", read(rel)):
            errors.append("%s: R_DlightMark_NewMap must run beside R_DlightVis_NewMap (a new map starts "
                          "r_framecount over)" % rel)
    cm = read("engine/CMakeLists.txt")
    for lst in ("GL_RENDERER_SOURCES", "SOFT_RENDERER_SOURCES"):
        blk = re.search(r"set\(%s\n(.*?)\n\)" % lst, cm, re.S)
        if not blk or "${COMMONDIR}/r_dlightmark.c" not in blk.group(1):
            errors.append("engine/CMakeLists.txt: r_dlightmark.c must be in %s" % lst)


for tier in ("GL", "SW"):
    src = program(tier, None, main_body())
    if src:
        check_main(tier, parse(run(src, "shared_" + tier)))
    src = program(tier, 2, frames_body())
    if src:
        check_frames(tier, run(src, "frames_" + tier))
    src = program(tier, 1, overflow_body())
    if src:
        check_overflow(tier, run(src, "overflow_" + tier))
check_source()

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(dict.fromkeys(errors)))
print("PASS entities sharing a brush submodel are each lit by their own light, in their own space, on both CPU "
      "tiers; the shared light goes to the nearest copy; pool restarts per frame; overflow misplaces, never "
      "drops (GitHub #496)")
