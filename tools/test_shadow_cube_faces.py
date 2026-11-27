#!/usr/bin/env python3
"""The cube shadow maps' six face matrices are GL's cube-map faces, and the
pass keeps the faces turned AWAY from the light (GitHub #97, P3).

gl_shadow.c draws each map as six depth-only passes, one per layer-face of a
GL_TEXTURE_CUBE_MAP_ARRAY, with clip x = sc.d, y = tc.d, w = ma.d for the
light-relative position d, taking sc / tc / ma from its sh_faceaxes table.  The
receivers (dlshadow.inc) then sample with the raw direction and let the hardware
pick the face.  Two things have to be exactly right and neither shows up as a
compile error, only as shadows in the wrong place:

  1. THE TABLE IS GL's.  Checked row by row against the major-axis table of the
     GL 4.6 core spec (8.13, table 8.19), in GL_TEXTURE_CUBE_MAP_POSITIVE_X + i
     order, and by projecting directions: for every direction a face owns, the
     texel the pass draws it into is the texel the spec's s/t formula reads.
  2. THE WINDING.  Every face of that table has determinant -1, and so does the
     main view's (x, y, w) mapping (a GL camera looks down -z), so a cube face
     is an ordinary camera, NOT a mirror: a triangle facing the light lands on
     screen with the winding the main view gives a triangle facing the camera.
     The main view keeps those by culling GL_FRONT (R_SetupGL, default CCW
     front face); the shadow pass wants the faces turned AWAY from the light --
     the whole design: the map stores the far side of the wall -- so it must
     cull GL_BACK.  Checked by projecting a real triangle through a model of
     R_SetupGL's own view matrix and frustum, so "which winding is a front" is
     the engine's answer, not an assumption.  (The first draft of gl_shadow.c
     argued the faces were mirrors and culled GL_FRONT; this test is what
     caught it.)

Also checked: SH_FaceMask, the per-face caster cull, never keeps a box from a
face that a point of it is read from (random boxes, point samples).

Also pinned: the matrix layout SH_BuildFaceMatrices writes (rows sc, tc, ma and
ma, clip z = w - SH_ZNEAR_W, inside both clip conventions for w >= 1), that the
pass culls with SH_CULL_FACE and SH_CULL_FACE is GL_BACK, that no engine source
calls glFrontFace, that face f of slot s goes to layer-face s * 6 + f with
sh_facevp[f] (sh_fbos[i] being layer-face i's framebuffer), and that the stored and the looked-up direction are both
light -> point (sshadow_vert's u_shadowmodel is T(-L) * M; dlshadow.inc's
d = wp - origin).
"""
from pathlib import Path
import math
import random
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(rel):
    p = ROOT / rel
    if not p.exists():
        errors.append(f"{rel} is missing")
        return ""
    return p.read_text()


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


shadow = read("engine/h2shared/gl_shadow.c")
code = strip(shadow)
rmain = strip(read("engine/hexen2/gl_rmain.c"))
dls = strip(read("engine/shaders/dlshadow.inc"))
svert = strip(read("engine/shaders/sshadow_vert.glsl"))

# GL 4.6 core, table 8.19: face -> (sc, tc, ma) as signed axes of the direction.
SPEC = {
    "+X": ((0, 0, -1), (0, -1, 0), (1, 0, 0)),
    "-X": ((0, 0, 1), (0, -1, 0), (-1, 0, 0)),
    "+Y": ((1, 0, 0), (0, 0, 1), (0, 1, 0)),
    "-Y": ((1, 0, 0), (0, 0, -1), (0, -1, 0)),
    "+Z": ((1, 0, 0), (0, -1, 0), (0, 0, 1)),
    "-Z": ((-1, 0, 0), (0, -1, 0), (0, 0, -1)),
}
ORDER = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]   # GL_TEXTURE_CUBE_MAP_POSITIVE_X + i

m = re.search(r"static const signed char sh_faceaxes\[6\]\[3\]\[3\]\s*=\s*\{(.*?)\};", code, re.S)
table = []
if not m:
    errors.append("gl_shadow.c: no `static const signed char sh_faceaxes[6][3][3]` table")
else:
    nums = [int(x) for x in re.findall(r"-?\d+", m.group(1))]
    if len(nums) != 54:
        errors.append(f"sh_faceaxes holds {len(nums)} numbers, not 54")
    else:
        table = [tuple(tuple(nums[f * 9 + r * 3:f * 9 + r * 3 + 3]) for r in range(3)) for f in range(6)]
        for f, name in enumerate(ORDER):
            if table[f] != SPEC[name]:
                errors.append(f"sh_faceaxes[{f}] ({name}) is {table[f]}, GL's table says {SPEC[name]}")


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def det3(r):
    (a, b, c), (d, e, f), (g, h, i) = r
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def spec_face(d):
    """The face and (s, t) the GL sampler takes direction d to."""
    ax = max(range(3), key=lambda k: abs(d[k]))
    name = ("+" if d[ax] > 0 else "-") + "XYZ"[ax]
    sc, tc, ma = (dot(d, v) for v in SPEC[name])
    return name, ((sc / abs(ma) + 1) / 2, (tc / abs(ma) + 1) / 2)


# The matrix layout SH_BuildFaceMatrices writes, as the test models it.
for want in (r"m\[c \* 4 \+ 0\] = sh_faceaxes\[f\]\[0\]\[c\];", r"m\[c \* 4 \+ 1\] = sh_faceaxes\[f\]\[1\]\[c\];",
             r"m\[c \* 4 \+ 2\] = sh_faceaxes\[f\]\[2\]\[c\];", r"m\[c \* 4 \+ 3\] = sh_faceaxes\[f\]\[2\]\[c\];",
             r"m\[14\] = -SH_ZNEAR_W;"):
    if not re.search(want, code):
        errors.append(f"SH_BuildFaceMatrices no longer writes `{want}` (rows sc, tc, ma, ma; z = w - SH_ZNEAR_W)")
zn = re.search(r"#define\s+SH_ZNEAR_W\s+([\d.]+)f?", code)
znear = float(zn.group(1)) if zn else None
if znear is None or not 0 < znear <= 1.0:
    errors.append("SH_ZNEAR_W must be a positive constant of at most 1 unit")
    znear = 1.0


def face_clip(f, d):
    sc, tc, ma = table[f]
    w = dot(ma, d)
    return (dot(sc, d), dot(tc, d), w - znear, w)


rng = random.Random(97)
if table:
    for f, name in enumerate(ORDER):
        if det3(table[f]) != -1:
            errors.append(f"face {name} has determinant {det3(table[f])}, not -1, the main view's sign")
        # 1. every direction this face owns lands where the sampler reads it
        bad = 0
        for _ in range(400):
            d = [rng.uniform(-1, 1) for _ in range(3)]
            fname, (s, t) = spec_face(d)
            if fname != name:
                continue
            x, y, z, w = face_clip(f, d)
            ps, pt = (x / w + 1) / 2, (y / w + 1) / 2
            if abs(ps - s) > 1e-9 or abs(pt - t) > 1e-9 or w <= 0:
                bad += 1
            # z inside both clip conventions (-w..w and 0..w) once w >= 1 unit
            for scale in (1.0, 37.0, 900.0):
                dd = [c * scale / max(abs(c) for c in d) for c in d]
                x, y, z, w = face_clip(f, dd)
                if w >= 1.0 and not (0 <= z <= w and -w <= z <= w):
                    bad += 1
        if bad:
            errors.append(f"face {name}: {bad} directions drawn somewhere the sampler does not read them")


# ------------------------------------------------------------ 2. the winding
def rot(angle, x, y, z):
    """GL_Rotatef's matrix, column-major."""
    s, c = math.sin(math.radians(angle)), math.cos(math.radians(angle))
    ic = 1 - c
    return [x * x * ic + c, y * x * ic + z * s, z * x * ic - y * s, 0,
            x * y * ic - z * s, y * y * ic + c, z * y * ic + x * s, 0,
            x * z * ic + y * s, y * z * ic - x * s, z * z * ic + c, 0, 0, 0, 0, 1]


def mul(a, b):
    return [sum(a[k * 4 + j] * b[i * 4 + k] for k in range(4)) for i in range(4) for j in range(4)]


def xform(mat, v):
    return [sum(mat[k * 4 + j] * v[k] for k in range(4)) for j in range(4)]


def main_view(org, pitch, yaw):
    """R_SetupGL's modelview: Rx(-90) Rz(90) Rx(-roll) Ry(-pitch) Rz(-yaw) T(-org)."""
    mv = rot(-90, 1, 0, 0)
    for r in (rot(90, 0, 0, 1), rot(0, 1, 0, 0), rot(-pitch, 0, 1, 0), rot(-yaw, 0, 0, 1)):
        mv = mul(mv, r)
    t = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -org[0], -org[1], -org[2], 1]
    mv = mul(mv, t)
    n, fz, xm = 4.0, 4096.0, 4.0
    fr = [n / xm, 0, 0, 0, 0, n / xm, 0, 0, 0, 0, -(fz + n) / (fz - n), -1, 0, 0, -2 * fz * n / (fz - n), 0]
    return mul(fr, mv)


def area(clips):
    p = [(c[0] / c[3], c[1] / c[3]) for c in clips]
    return (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1])


src_frontface = [p for p in (ROOT / "engine").rglob("*.c") if re.search(r"\bglFrontFace", strip(p.read_text()))]
if src_frontface:
    errors.append(f"glFrontFace is called in {src_frontface}; the default CCW front face this relies on is gone")
setup = re.search(r"static void R_SetupGL \(void\)\s*\{(.*?)\n\}", rmain, re.S)
if not setup or not re.search(r"\}\s*else\s*R_SetCullFace \(GL_FRONT\);", setup.group(1)):
    errors.append("R_SetupGL no longer culls GL_FRONT outside the mirror pass; re-derive the shadow pass's cull face")
if not re.search(r"#define\s+SH_CULL_FACE\s+GL_BACK\b", code):
    errors.append("SH_CULL_FACE must be GL_BACK, the opposite of the main view's GL_FRONT (see the docstring)")
ps = re.search(r"static void SH_PassState \(void\)\s*\{(.*?)\n\}", code, re.S)
if not ps or "R_SetCull (true);" not in ps.group(1) or "R_SetCullFace (SH_CULL_FACE);" not in ps.group(1):
    errors.append("SH_PassState must enable culling with SH_CULL_FACE")

AXES = {"+X": (1, 0, 0), "-X": (-1, 0, 0), "+Y": (0, 1, 0), "-Y": (0, -1, 0), "+Z": (0, 0, 1), "-Z": (0, 0, -1)}
if table:
    L = (37.0, -120.0, 64.0)
    for f, name in enumerate(ORDER):
        a = AXES[name]
        # a triangle 100 units out along the face's axis, slightly off axis so
        # the main-view camera aimed at it is not looking straight up or down
        u = (0, 0, 1) if a[2] == 0 else (1, 0, 0)
        v = (a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0])
        c = [L[k] + 100 * a[k] + 3 * u[k] + 2 * v[k] for k in range(3)]
        tri = [[c[k] + 20 * u[k] for k in range(3)], [c[k] + 20 * v[k] for k in range(3)],
               [c[k] - 20 * u[k] - 20 * v[k] for k in range(3)]]
        dvec = [c[k] - L[k] for k in range(3)]
        yaw = math.degrees(math.atan2(dvec[1], dvec[0]))
        pitch = -math.degrees(math.atan2(dvec[2], math.hypot(dvec[0], dvec[1])))
        mvp = main_view(L, pitch, yaw)
        # the model of R_SetupGL must put the triangle in front of the camera
        if any(xform(mvp, p + [1.0])[3] <= 0 for p in tri):
            errors.append(f"face {name}: the R_SetupGL model does not look where AngleVectors says; fix the test")
            continue
        main = area([xform(mvp, p + [1.0]) for p in tri])
        # GL_FRONT with a CCW front face keeps what is clockwise on screen, so
        # the main view calls a clockwise triangle a front: this one faces the
        # light, which stands where the camera does.
        facing = tri if main < 0 else tri[::-1]
        # GL_BACK with a CCW front face keeps what is counter-clockwise
        sh = area([face_clip(f, [p[k] - L[k] for k in range(3)]) for p in facing])
        if not sh < 0:
            errors.append(f"face {name}: a triangle facing the light survives GL_BACK in the shadow pass -- "
                          "the map would store the near side of the wall")
        sh = area([face_clip(f, [p[k] - L[k] for k in range(3)]) for p in facing[::-1]])
        if not sh > 0:
            errors.append(f"face {name}: a triangle facing away from the light would be culled")
        mdet = det3([[mvp[c * 4 + r] for c in range(3)] for r in (0, 1, 3)])
        if (mdet < 0) != (det3(table[f]) < 0):
            errors.append(f"face {name}: the main view's handedness differs from the face's")

# ------------------------------------------------------------ layer-face order and direction
dm = re.search(r"static void SH_DrawMap \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
dm = dm.group(1) if dm else ""
if not re.search(r"for \(f = 0; f < 6; f\+\+\)", dm) \
        or "R_BindFramebuffer (GL_FRAMEBUFFER, sh_fbos[slot * 6 + f]);" not in dm \
        or "memcpy (pass.vp, sh_facevp[f], sizeof(pass.vp));" not in dm:
    errors.append("SH_DrawMap must draw face f of `slot` into layer-face slot * 6 + f with sh_facevp[f]")
et = re.search(r"static qboolean SH_EnsureTexture \(void\)\s*\{(.*?)\n\}", code, re.S)
# through gl_fbo.h's seams since #340 P6a
if not et or not re.search(r"for \(i = 0; i < maps \* 6[^)]*\)\s*\{\s*R_BindFramebuffer \(GL_FRAMEBUFFER, sh_fbos\[i\]\);"
                            r"\s*R_DrawBuffers \(1, &none\);\s*R_FramebufferTextureLayer \(GL_FRAMEBUFFER, "
                            r"GL_DEPTH_ATTACHMENT, sh_tex, 0, i\);", et.group(1)):
    errors.append("SH_EnsureTexture must attach layer-face i, and only it, to sh_fbos[i]")
if dm.count("Mat4_ApplyTranslation") < 2 or "-org[0], -org[1], -org[2]" not in dm \
        or "Mat4_Multiply (t, sh_ents[i].model, pass.model);" not in dm:
    errors.append("SH_DrawMap's model matrices must be T(-light) for the world and T(-light) * M for an entity")
if not re.search(r"highp vec3 rel = \(u_shadowmodel \* vec4\(a_position, 1\.0\)\)\.xyz;", svert) \
        or "gl_Position = u_shadowvp * vec4(rel, 1.0);" not in svert:
    errors.append("sshadow_vert must project the light-relative position u_shadowmodel * a_position")
if "highp vec3 d = wp - o.xyz;" not in dls or "texture(u_dlshadowmap, vec4(d, float(layer)), ref)" not in dls:
    errors.append("dlshadow.inc must look up light -> point (wp - origin), the direction the maps were drawn along")

# ------------------------------------------------------------ per-face culling
# SH_FaceMask may let a box through to a face it misses (a clipped draw), but
# must never keep one from a face it reaches: that face would lose a caster
# and light would show through the wall.  Its planes are pinned here and the
# same arithmetic is run against point samples of random boxes.
fm = re.search(r"static int SH_FaceMask \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
fm = fm.group(1) if fm else ""
for want in ("const signed char *sc = sh_faceaxes[f][0], *tc = sh_faceaxes[f][1], *ma = sh_faceaxes[f][2];",
             "n[c] = ma[c];", "if (p == 1) n[c] -= sc[c];", "if (p == 2) n[c] += sc[c];",
             "if (p == 3) n[c] -= tc[c];", "if (p == 4) n[c] += tc[c];",
             "best += n[c] * (n[c] > 0.0f ? hi[c] : lo[c]);", "if (best < 0.0f)", "if (p == 5)\n\t\t\tmask |= 1 << f;",
             "lo[c] = mins[c] - org[c];", "hi[c] = maxs[c] - org[c];"):
    if want.replace("\\n", "\n").replace("\\t", "\t") not in fm:
        errors.append(f"SH_FaceMask no longer reads `{want}`")
mw = re.search(r"static void SH_MarkWorld \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
cm = re.search(r"static qboolean SH_CollectMap \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
dmap = re.search(r"static void SH_DrawMap \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
if not mw or "faces = SH_FaceMask (org, &sh_sbox[idx * 6], &sh_sbox[idx * 6 + 3]);" not in mw.group(1) \
        or not cm or "if (!(sh_smask[sh_list[i]] & (1 << f)))" not in code \
        or "d->wcount[f] = SH_BuildRuns (base, f);" not in cm.group(1) \
        or "faces = SH_FaceMask (org, emins, emaxs);" not in cm.group(1) \
        or not dmap or "&sh_counts[d->wfirst[f]]" not in dmap.group(1) \
        or "if (!(sh_ents[i].faces & (1 << f)))" not in dmap.group(1):
    errors.append("the world's and the entities' runs must be culled per face with SH_FaceMask and drawn per face")


def face_mask(lo, hi):
    mask = 0
    for f in range(6):
        sc, tc, ma = table[f]
        for p in range(5):
            n = list(ma)
            for c in range(3):
                if p == 1: n[c] -= sc[c]
                if p == 2: n[c] += sc[c]
                if p == 3: n[c] -= tc[c]
                if p == 4: n[c] += tc[c]
            if sum(n[c] * (hi[c] if n[c] > 0 else lo[c]) for c in range(3)) < 0:
                break
        else:
            mask |= 1 << f
    return mask


if table:
    missed = 0
    for _ in range(3000):
        c0 = [rng.uniform(-200, 200) for _ in range(3)]
        ext = [rng.choice((0.0, rng.uniform(0, 40), rng.uniform(0, 300))) for _ in range(3)]
        lo = [c0[k] - ext[k] for k in range(3)]
        hi = [c0[k] + ext[k] for k in range(3)]
        mask = face_mask(lo, hi)
        for _ in range(12):
            p = [rng.uniform(lo[k], hi[k]) for k in range(3)]
            if max(abs(x) for x in p) == 0:
                continue
            name, _st = spec_face(p)
            if not mask & (1 << ORDER.index(name)):
                missed += 1
    if missed:
        errors.append(f"SH_FaceMask kept {missed} sampled box points from the face the sampler reads them from")

if errors:
    print("FAIL test_shadow_cube_faces:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: the six shadow-map faces are GL's, as handed as the main view, and GL_BACK keeps the faces turned from the light")
