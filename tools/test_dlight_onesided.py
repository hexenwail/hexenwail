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

The CPU half is tested by running it: each R_AddDynamicLights prologue (from
`rad = ...` to `minlight = rad - minlight;`) and each R_MarkLights surface
loop is lifted out of the engine source verbatim, compiled against stubs, and
run with one light and one surface:

  light 24 units behind the face        -> no light, not marked
  light 1 unit behind (inside the 2)    -> lit, marked, radius 199
  light 24 in front                     -> lit, marked
  SURF_PLANEBACK, light on the plane's
    + side (so behind the face)         -> no light; on the - side -> lit
  SURF_DRAWTURB, light 24 behind        -> lit (liquids keep both sides)
  r_dlight_occlusion 0, light 24 behind -> lit (vanilla)
  a brush entity's surface, 24 behind   -> lit: the CPU path measures a
                                           world-space light against a
                                           model-space plane, so it keeps
                                           both sides there (client.h)

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


def prologue(rel, first):
    """R_AddDynamicLights' per-light prologue, `first` .. minlight = rad - minlight;"""
    fn = body_of(read(rel), r"static void R_AddDynamicLights \([^)]*\)\s*\{")
    if not fn:
        errors.append("%s: no R_AddDynamicLights" % rel)
        return None
    a = fn.find(first)
    b = fn.find("minlight = rad - minlight;", a)
    if a < 0 or b < 0:
        errors.append("%s: R_AddDynamicLights' prologue (%r .. minlight = rad - minlight;) is gone" % (rel, first))
        return None
    return fn[a:b + len("minlight = rad - minlight;")]


def mark_loop(rel):
    """R_MarkLights' surface loop, in the live (not #if 0) version."""
    text = read(rel)
    live = text
    m = re.search(r"#if 0\s*/\* the original version from ID \*/(.*?)#else", text, re.S)
    if m:
        live = text[:m.start()] + text[m.end():]
    fn = body_of(live, r"\nvoid R_MarkLights \(dlight_t \*light, int lnum, mnode_t \*node\)\s*\{")
    if not fn:
        errors.append("%s: no R_MarkLights" % rel)
        return None
    a = fn.find("for (i = 0; i < node->numsurfaces; i++, surf++)")
    if a < 0:
        errors.append("%s: R_MarkLights has no surface loop" % rel)
        return None
    depth, i = 0, fn.index("{", a)
    for j in range(i, len(fn)):
        depth += {"{": 1, "}": -1}.get(fn[j], 0)
        if depth == 0:
            return fn[a:j + 1]
    return None


PRELUDE = r'''
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef float vec3_t[3];
typedef int qboolean;
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define MAX_DLIGHTS 128
#define DLIGHTBITS_WORDS ((MAX_DLIGHTS + 31) / 32)
#define DLIGHTBIT_TEST(b,i) ((b)[(i) >> 5] & (1u << ((i) & 31)))
#define DLIGHTBIT_SET(b,i) ((b)[(i) >> 5] |= (1u << ((i) & 31)))
#define DLIGHTBITS_CLEAR(b) memset ((b), 0, sizeof(unsigned int) * DLIGHTBITS_WORDS)
typedef struct { vec3_t normal; float dist; int type; } mplane_t;
typedef struct { float vecs[2][4]; } mtexinfo_t;
typedef struct { mplane_t *plane; int flags; mtexinfo_t *texinfo; short texturemins[2], extents[2];
                 int dlightframe; unsigned int dlightbits[4]; } msurface_t;
typedef struct { int numsurfaces; } mnode_t;
typedef struct { vec3_t origin; float radius, minlight; } dlight_t;
typedef struct { int integer; } cvar_t;
static cvar_t r_dlight_occlusion;
static dlight_t cl_dlights[1];
static int r_dlightframecount = 7;
static mplane_t plane = {{0, 0, 1}, 0, 2};
static mtexinfo_t texinfo = {{{1, 0, 0, 0}, {0, 1, 0, 0}}};
/* surfs[0] is the world's one surface, surfs[1] a brush entity's */
static msurface_t surfs[2];
typedef struct { msurface_t *surfaces; int firstmodelsurface, nummodelsurfaces; } qmodel_t;
static qmodel_t world = {surfs, 0, 1};
static struct { qmodel_t *worldmodel; } cl = {&world};
'''

# Each harness returns 1 and the remaining radius if the light got through,
# 0 if it was dropped (the `continue` leaves the one-pass loop).
HARNESS = r'''
static int add_%(name)s (msurface_t *surf, float *out_rad)
{
	int lnum; float dist, rad, minlight;
	for (lnum = 0; lnum < 1; lnum++)
	{
%(body)s
		*out_rad = rad; (void)minlight; return 1;
	}
	return 0;
}
'''
MARK = r'''
static int mark_%(name)s (dlight_t *light, msurface_t *surf, float dist)
{
	mnode_t real_node = {1}, *node = &real_node;
	msurface_t *const tested = surf;	/* the loop's surf++ walks past it */
	float l, maxdist; int i, j, s, t; vec3_t impact; int lnum = 0;
	(void)l; (void)maxdist; (void)j; (void)s; (void)t; (void)impact;
	maxdist = light->radius * light->radius;
	surf->dlightframe = 0;
	memset (surf->dlightbits, 0, sizeof(surf->dlightbits));
%(body)s
	return DLIGHTBIT_TEST (tested->dlightbits, 0) ? 1 : 0;
}
'''

# name, cvar, surf flags (by name), light height above the plane (the plane
# is z = 0, normal +z), want lit; a trailing True puts the surface on a brush
# entity, where the CPU path keeps both sides (DLIGHT_WORLD_SURFACE, client.h:
# its light origin is world space, its plane model space)
CASES = [
    ("behind24", 1, "0", -24.0, False),
    ("behind1", 1, "0", -1.0, True),
    ("edge2", 1, "0", -2.0, True),
    ("behind2.5", 1, "0", -2.5, False),
    ("front24", 1, "0", 24.0, True),
    ("planeback_plus", 1, "SURF_PLANEBACK", 24.0, False),
    ("planeback_minus", 1, "SURF_PLANEBACK", -24.0, True),
    ("turb_behind", 1, "SURF_DRAWTURB", -24.0, True),
    ("turb_planeback", 1, "SURF_DRAWTURB|SURF_PLANEBACK", 24.0, True),
    ("cvar0_behind", 0, "0", -24.0, True),
    ("cvar2_behind", 2, "0", -24.0, False),
    ("brushent_behind", 1, "0", -24.0, True, True),
    ("brushent_front", 1, "0", 24.0, True, True),
]


def run_c():
    eps = define("engine/hexen2/client.h", "DLIGHT_PLANE_EPSILON")
    consts = ["#define DLIGHT_PLANE_EPSILON %s" % eps]
    ws = re.search(r"^#define\s+DLIGHT_WORLD_SURFACE\(s\)(?:[^\n]*\\\n)*[^\n]*$",
                   read("engine/hexen2/client.h"), re.M)
    if not ws:
        errors.append("engine/hexen2/client.h: no #define DLIGHT_WORLD_SURFACE(s)")
        return None
    consts.append(ws.group(0))
    for hdr, tag in (("engine/h2shared/gl_model.h", "GL"), ("engine/h2shared/model.h", "SW")):
        for n in ("SURF_PLANEBACK", "SURF_DRAWTURB"):
            consts.append("#define %s_%s %s" % (tag, n, define(hdr, n)))
    sites = {
        "gl_add": prologue(GL_RSURF, "rad = cl_dlights[lnum].radius;"),
        "sw_add": prologue(R_SURF, "rad = fabs(cl_dlights[lnum].radius);"),
        "gl_mark": mark_loop(GL_RLIGHT),
        "sw_mark": mark_loop(R_LIGHT),
    }
    if any(v is None for v in sites.values()):
        return None
    # the GL path builds against gl_model.h, the software one against model.h
    tagged = {}
    for k, v in sites.items():
        tag = "GL" if k.startswith("gl") else "SW"
        tagged[k] = re.sub(r"\b(SURF_PLANEBACK|SURF_DRAWTURB)\b", tag + r"_\1", v)
    prog = [PRELUDE, "\n".join(consts),
            ]
    for k in ("gl_add", "sw_add"):
        prog.append(HARNESS % {"name": k, "body": tagged[k]})
    for k in ("gl_mark", "sw_mark"):
        prog.append(MARK % {"name": k, "body": tagged[k]})
    calls = []
    for name, cvar, flags, h, _, *ent in CASES:
        for tag in ("GL", "SW"):
            f = re.sub(r"\b(SURF_\w+)\b", tag + r"_\1", flags)
            low = tag.lower()
            calls.append(r'''
	{
		msurface_t *sp = &surfs[%(ent)d]; float rad = -1; int a, m;
#define s (*sp)
		memset (&s, 0, sizeof(s));
		s.plane = &plane; s.flags = %(f)s; s.texinfo = &texinfo;
		s.texturemins[0] = s.texturemins[1] = -64; s.extents[0] = s.extents[1] = 128;
		r_dlight_occlusion.integer = %(cvar)d;
		cl_dlights[0].origin[0] = cl_dlights[0].origin[1] = 0; cl_dlights[0].origin[2] = %(h)ff;
		cl_dlights[0].radius = 200; cl_dlights[0].minlight = 0;
		s.dlightbits[0] = 1;
		a = add_%(low)s_add (&s, &rad);
		m = mark_%(low)s_mark (&cl_dlights[0], &s, %(h)ff);
		printf ("%(tag)s %(name)s %%d %%d %%g\n", a, m, a ? rad : 0.0);
#undef s
	}''' % dict(f=f, cvar=cvar, h=h, low=low, tag=tag, name=name, ent=1 if ent else 0))
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
            tag, name, a, m, rad = line.split()
            got[(tag, name)] = (int(a), int(m), float(rad))
    for name, cvar, flags, h, want, *_ in CASES:
        for tag, what in (("GL", "gl_rsurf.c / gl_rlight.c"), ("SW", "r_surf.c / r_light.c")):
            a, m, rad = got.get((tag, name), (-1, -1, 0))
            print("%s %-16s cvar %d %-28s light %+6.1f: adds %s, marks %s%s" % (
                tag, name, cvar, flags, h, "yes" if a else "no ", "yes" if m else "no ",
                "  (radius left %g)" % rad if a else ""))
            if a != want:
                errors.append("%s R_AddDynamicLights, %s: %s, want %s" % (
                    what, name, "lights" if a else "drops the light", "lit" if want else "dropped"))
            if m != want:
                errors.append("%s R_MarkLights, %s: %s, want %s" % (
                    what, name, "marks" if m else "does not mark", "marked" if want else "not marked"))
            if a and want and abs(rad - (200 - abs(h))) > 1e-3:
                errors.append("%s R_AddDynamicLights, %s: radius left %g, want %g (the lit side keeps fabs(dist))"
                              % (what, name, rad, 200 - abs(h)))


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
check_gpu()
check_cvar()
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(dict.fromkeys(errors)))
print("PASS a light behind a face does not light it: CPU twins (add + mark) run, GPU test, lane, writer, "
      "liquid bracket and cvar pinned (GitHub #97)")
