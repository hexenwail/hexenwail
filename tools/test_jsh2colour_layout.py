#!/usr/bin/env python3
"""jsh2colour lays each face's colour out the way the engine reads it (#531).

The engine finds a face's colour at 3 * lightofs in the .lit, and reads it
by its own lightmap size (CalcSurfaceExtents in engine/h2shared/gl_model.c),
one lightmap per style the BSP gives the face, in the BSP's style order.
jsh2colour used to write what it had worked out for itself instead:

  - its extents came from doubles where the engine's come from floats, so a
    vertex at s = 32.000001 gave it one lightmap column or row more than the
    engine (1786 faces over the retail maps).  The lightmap had the wrong
    width, and ran into the next face's colour;
  - it wrote its own styles, in its own order;
  - it wrote all of it even where the BSP had given the face less room.

Three checks, none needing game data:

  layout   lifts CalcFaceLayout out of utils/jsh2color/ltface.c and
           CalcSurfaceExtents out of the engine, compiles both against
           small stubs, and runs them on the same faces: crafted ones at
           the float/double edge, and random ones.  They must agree on
           every face, and move a rotate_ face's sample grid by its offset.
  space    compiles the generated fixture (tools/test_jsh2colour_threads.py)
           and then moves half its faces' lightofs so that each one's space
           ends one sample short of its lightmap and runs into a
           neighbour's.  Every face's own space must still hold that face's
           colour, exactly as the fixture laid out unchanged has it.
  styles   compiles the fixture with a light on style 5, so some faces get
           styles 0 and 5, then swaps the two in the BSP.  The .lit must
           swap with them.
  rotate   colours a rotate_object at origin 32 48 0 and the same brush at
           0 0 0.  The .lit must be the same.

  test_jsh2colour_layout.py UTILS_BIN [--ltface FILE] [--layout CALL]

--ltface and --layout run the layout check against another copy of the
source, to show it failing on one from before the change:
  --ltface old/ltface.c --layout 'CalcFaceExtents(l, off, false)'
"""
import argparse
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_jsh2colour_threads as fixture  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
LTFACE = ROOT / "utils/jsh2color/ltface.c"
GL_MODEL = ROOT / "engine/h2shared/gl_model.c"
f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]


def c_function(src, header):
    """The text of the function whose definition starts with header."""
    i = src.find(header)
    if i < 0:
        sys.exit("FAIL: no %r in the source" % header)
    j = src.find("\n}\n", i)
    return src[i:j + 3]


HARNESS = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef double vec_t;
typedef vec_t vec3_t[3];
typedef unsigned char byte;
typedef int qboolean;
#define false 0
#define true 1
#define MAXV 64
/* the tool's side */
typedef struct { float vecs[2][4]; int miptex, flags; } texinfo_t;
typedef struct { float point[3]; } dvertex_t;
typedef struct { unsigned short v[2]; } dedge_t;
typedef struct { unsigned int v[2]; } dedge2_t;
typedef struct { short planenum, side; int firstedge; short numedges, texinfo; } dface_t;
typedef struct { int planenum, side, firstedge, numedges, texinfo; } dface2_t;
typedef struct {
	void *face; int texmins[2], texsize[2]; vec_t texshift[2];
	vec_t exactmins[2], exactmaxs[2];
} lightinfo_t;
static texinfo_t texinfo[1];
static dvertex_t dvertexes[MAXV];
static dedge_t dedges[MAXV + 1];
static dedge2_t dedges2[MAXV + 1];
static int dsurfedges[MAXV];
static int is_bsp2;
static void COM_Error (const char *m, ...) { (void)m; }
/* the engine's side */
#define TEX_SPECIAL 1
#define MAX_SURFACE_LIGHTMAP 256
typedef struct { float vecs[2][4]; int flags; } mtexinfo_t;
typedef struct { float position[3]; } mvertex_t;
typedef struct { unsigned int v[2]; } medge_t;
typedef struct {
	mtexinfo_t *texinfo; int firstedge, numedges;
	short texturemins[2], extents[2];
} msurface_t;
static struct { int *surfedges; mvertex_t *vertexes; medge_t *edges; } model, *loadmodel = &model;
static mvertex_t mvertexes[MAXV];
static medge_t medges[MAXV + 1];
static mtexinfo_t mtex;
static void Sys_Error (const char *m, ...) { (void)m; }

@TOOL@
@ENGINE@

int main (void)
{
	int n, i, j;
	while (scanf ("%d", &n) == 1)
	{
		dface_t f; lightinfo_t L, *l = &L; msurface_t s;
		vec3_t off;
		scanf ("%lf %lf %lf", &off[0], &off[1], &off[2]);
		for (i = 0; i < 2; i++)
			for (j = 0; j < 4; j++)
			{
				scanf ("%f", &texinfo[0].vecs[i][j]);
				mtex.vecs[i][j] = texinfo[0].vecs[i][j];
			}
		for (i = 0; i < n; i++)
		{
			scanf ("%f %f %f", &dvertexes[i].point[0], &dvertexes[i].point[1], &dvertexes[i].point[2]);
			for (j = 0; j < 3; j++)
				mvertexes[i].position[j] = dvertexes[i].point[j];
			/* edge i+1 runs from vertex i to i+1 */
			dedges[i + 1].v[0] = medges[i + 1].v[0] = i;
			dedges[i + 1].v[1] = medges[i + 1].v[1] = (i + 1) % n;
			dsurfedges[i] = i + 1;
		}
		memset (&f, 0, sizeof(f)); f.numedges = n;
		memset (&L, 0, sizeof(L)); L.face = &f;
		@CALL@;
		memset (&s, 0, sizeof(s));
		s.texinfo = &mtex; s.numedges = n;
		model.surfedges = dsurfedges; model.vertexes = mvertexes; model.edges = medges;
		CalcSurfaceExtents (&s);
		printf ("%d %d %d %d %d %d %d %d %.9g %.9g\n", L.texmins[0], L.texmins[1], L.texsize[0], L.texsize[1],
			s.texturemins[0] / 16, s.texturemins[1] / 16, s.extents[0] / 16, s.extents[1] / 16,
			L.texshift[0], L.texshift[1]);
	}
	return 0;
}
'''


def layout_faces():
    """(vecs, verts, offset) faces: the float/double edge first, then random ones."""
    faces = []
    third = f32(1 / 3)
    # 96 * f32(1/3) is 32.000001 in doubles and 32 in floats
    for x0 in (0, 48, 96, -96):
        for y0 in (0, 96):
            vecs = (third, 0, 0, 0, 0, third, 0, 0)
            faces.append((vecs, [(x0, y0, 0), (x0 + 96, y0, 0), (x0 + 96, y0 + 96, 0), (x0, y0 + 96, 0)],
                          (0, 0, 0)))
    rnd = random.Random(531)
    for _ in range(4000):
        scale = rnd.choice((0.25, 0.5, 1, 1.5, 2, 3, 0.3, 0.7, 1.7, 2.5))
        ang = math.radians(rnd.choice((0, 15, 30, 45, 60, 90, 22.5, rnd.uniform(0, 360))))
        sv = [f32(math.cos(ang) / scale), f32(math.sin(ang) / scale), 0.0, float(rnd.randrange(-64, 64))]
        tv = [f32(-math.sin(ang) / scale), f32(math.cos(ang) / scale), 0.0, float(rnd.randrange(-64, 64))]
        x, y, z = (rnd.randrange(-2048, 2048, rnd.choice((1, 8, 16))) for _ in range(3))
        w, h = rnd.randrange(8, 200), rnd.randrange(8, 200)
        verts = [(x, y, z), (x + w, y, z), (x + w, y + h, z), (x, y + h, z)]
        # a rotate_ face: the layout must not move, the sample grid must
        off = tuple(rnd.randrange(-300, 300) for _ in range(3)) if rnd.random() < 0.2 else (0, 0, 0)
        if all(abs(v) <= 17 * 16 for v in (w / scale, h / scale)):
            faces.append((tuple(sv + tv), verts, off))
    return faces


def check_layout(ltface, call, tmp):
    tool = c_function(Path(ltface).read_text(), call.split("(")[0].strip().join(
        ("static void ", " (")))
    engine = c_function(GL_MODEL.read_text(), "static void CalcSurfaceExtents (msurface_t *s)")
    src = HARNESS.replace("@TOOL@", tool).replace("@ENGINE@", engine).replace("@CALL@", call)
    (tmp / "layout.c").write_text(src)
    exe = tmp / "layout"
    # -ffast-math as utils/CMakeLists.txt builds jsh2colour (FASTMATH)
    cc = subprocess.run(["cc", "-O2", "-ffast-math", "-o", str(exe), str(tmp / "layout.c"), "-lm"],
                        capture_output=True, text=True)
    if cc.returncode:
        sys.exit("FAIL: layout harness does not compile\n" + cc.stderr[-3000:])
    faces = layout_faces()
    stdin = "".join("%d %d %d %d %s %s\n" % ((len(v),) + off + (" ".join(repr(x) for x in vecs),
                                                          " ".join("%d %d %d" % p for p in v)))
                    for vecs, v, off in faces)
    r = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True)
    rows = [line.split() for line in r.stdout.splitlines()]
    if len(rows) != len(faces):
        sys.exit("FAIL: layout harness answered %d of %d faces" % (len(rows), len(faces)))
    shift = lambda vecs, off, j: sum(off[k] * vecs[4 * j + k] for k in range(3))
    bad = [(i, row) for i, row in enumerate(rows) if row[:4] != row[4:8]]
    moved = [i for i, (vecs, v, off) in enumerate(faces) if any(off)]
    badshift = [i for i in moved
                if any(abs(float(rows[i][8 + j]) - shift(faces[i][0], faces[i][2], j)) > 1e-3
                       for j in range(2))]
    for i, row in bad[:5]:
        print("  face %d: tool texmins %s size %s, engine %s size %s"
              % (i, row[0:2], row[2:4], row[4:6], row[6:8]))
    for i in badshift[:5]:
        print("  face %d: offset %s moves the grid by %s, not %s" % (
            i, faces[i][2], rows[i][8:10],
            ["%.4f" % shift(faces[i][0], faces[i][2], j) for j in range(2)]))
    if bad or badshift or len(moved) < 100:
        print("FAIL: layout: %d of %d faces laid out differently from the engine, "
              "%d of %d rotate_ faces sampled off their offset"
              % (len(bad), len(faces), len(badshift), len(moved)))
        return False
    print("ok: layout: %d faces laid out as the engine does, %d rotate_ faces sampled "
          "at their offset" % (len(faces), len(moved)))
    return True


# ---- the BSP side ----------------------------------------------------------

FACE = 20  # dface_t


def faces_of(data):
    ofs, length = struct.unpack_from("<ii", data, 4 + 8 * 7)
    return ofs, [list(struct.unpack_from("<hhihh4Bi", data, ofs + i))
                 for i in range(0, length, FACE)]


def put_faces(data, ofs, faces):
    data = bytearray(data)
    for i, f in enumerate(faces):
        struct.pack_into("<hhihh4Bi", data, ofs + i * FACE, *f)
    return bytes(data)


def engine_samples(data, fi, faces):
    """Samples per style, by the engine's float sums (fixture faces only)."""
    L = [struct.unpack_from("<ii", data, 4 + 8 * i) for i in range(15)]
    get = lambda lump, fmt, i: struct.unpack_from(fmt, data, L[lump][0] + i)
    _, _, fe, ne, ti = faces[fi][:5]
    t = get(6, "<8f", ti * 40)
    mins, maxs = [9999999.0] * 2, [-9999999.0] * 2
    for k in range(ne):
        e = get(13, "<i", (fe + k) * 4)[0]
        v = get(3, "<3f", get(12, "<2H", abs(e) * 4)[0 if e >= 0 else 1] * 12)
        for j in range(2):
            val = f32(v[0] * t[4 * j] + v[1] * t[4 * j + 1] + v[2] * t[4 * j + 2] + t[4 * j + 3])
            mins[j], maxs[j] = min(mins[j], val), max(maxs[j], val)
    w, h = (math.ceil(maxs[j] / 16) - math.floor(mins[j] / 16) + 1 for j in range(2))
    return w * h


def nstyles(f):
    return sum(1 for s in f[5:9] if s != 255)


def spaces(faces, litsize):
    starts = sorted({f[9] for f in faces if 0 <= f[9] < litsize})
    nxt = {s: (starts[i + 1] if i + 1 < len(starts) else litsize) for i, s in enumerate(starts)}
    return {i: nxt[f[9]] - f[9] for i, f in enumerate(faces) if 0 <= f[9] < litsize}


def colour(jsh, bsp, ext):
    lit = bsp.with_suffix(".lit")
    lit.unlink(missing_ok=True)
    fixture.run([jsh, "-threads", "1"] + ext + [bsp.name], bsp.parent)
    return lit.read_bytes()[8:]


def check_space(tools, tmp, ext):
    ref = (tmp / "fixture.bsp").read_bytes()
    litsize = struct.unpack_from("<ii", ref, 4 + 8 * 8)[1]
    fofs, faces = faces_of(ref)
    samples = {i: engine_samples(ref, i, faces) for i, f in enumerate(faces) if f[9] >= 0}
    want = colour(tools["jsh2colour"], tmp / "fixture.bsp", ext)

    moved = [list(f) for f in faces]
    taken = {f[9] for f in faces if f[9] >= 0}
    n = 0
    for i in range(1, len(faces), 2):
        a, b = faces[i - 1], faces[i]
        if a[9] < 0 or b[9] < 0 or nstyles(a) != 1 or nstyles(b) != 1:
            continue
        at = a[9] - samples[i] + 1     # b now ends one sample into a
        # two faces at one lightofs would share their space: not this test
        if at >= 0 and at not in taken:
            taken.discard(b[9])
            taken.add(at)
            moved[i][9] = at
            n += 1
    if n < 100:
        sys.exit("FAIL: space: only %d faces to move" % n)
    (tmp / "space.bsp").write_bytes(put_faces(ref, fofs, moved))
    got = colour(tools["jsh2colour"], tmp / "space.bsp", ext)

    # A face jsh2colour does not light keeps the grey copy of whatever mono
    # lightmap sits at its lightofs, which the move changes: leave those out.
    lump = struct.unpack_from("<ii", ref, 4 + 8 * 8)[0]
    mono = ref[lump:lump + litsize]
    grey = lambda o, k: bytes(b for b in mono[o:o + k] for _ in range(3))
    room = spaces(moved, litsize)
    bad = unlit = 0
    for i, f in enumerate(moved):
        if f[9] < 0 or nstyles(f) != 1:
            continue
        k = min(samples[i], room[i]) * 3
        if want[faces[i][9] * 3:faces[i][9] * 3 + k] == grey(faces[i][9], k // 3):
            unlit += 1
            continue
        if got[f[9] * 3:f[9] * 3 + k] != want[faces[i][9] * 3:faces[i][9] * 3 + k]:
            bad += 1
            if bad <= 5:
                print("  face %d at %d: its own %d samples hold another face's colour"
                      % (i, f[9], k // 3))
    if bad:
        print("FAIL: space: %d faces lost colour to a neighbour (%d moved)" % (bad, n))
        return False
    if unlit > n // 4:
        sys.exit("FAIL: space: %d faces left unlit, too few to check" % unlit)
    print("ok: space: %d faces moved into their neighbours, every lit face keeps its colour "
          "(%d unlit)" % (n, unlit))
    return True


def check_styles(tools, tmp, ext):
    fixture.write_map(tmp / "styled.map", "fixture.wad", styled=True)
    fixture.run([tools["qbsp"], "styled.map"], tmp)
    fixture.run([tools["light"], "-threads", "1", "styled.bsp"], tmp)
    ref = (tmp / "styled.bsp").read_bytes()
    fofs, faces = faces_of(ref)
    two = [i for i, f in enumerate(faces) if f[9] >= 0 and f[5:9] == [0, 5, 255, 255]]
    if len(two) < 20:
        sys.exit("FAIL: styles: only %d faces lit on styles 0 and 5" % len(two))
    want = colour(tools["jsh2colour"], tmp / "styled.bsp", ext)
    swapped = [list(f) for f in faces]
    for i in two:
        swapped[i][5:7] = [5, 0]
    (tmp / "swapped.bsp").write_bytes(put_faces(ref, fofs, swapped))
    got = colour(tools["jsh2colour"], tmp / "swapped.bsp", ext)
    bad = 0
    for i in two:
        o = faces[i][9] * 3
        n = engine_samples(ref, i, faces) * 3
        if got[o:o + n] != want[o + n:o + 2 * n] or got[o + n:o + 2 * n] != want[o:o + n]:
            bad += 1
    if bad:
        print("FAIL: styles: %d of %d faces keep their colour in jsh2colour's style order, "
              "not the BSP's" % (bad, len(two)))
        return False
    print("ok: styles: %d two-style faces follow the BSP's style order" % len(two))
    return True


def check_rotate(tools, tmp, ext):
    """A rotate_object with origin 32 48 0: qbsp takes the origin off its
    vertices and jsh2colour adds it back, so its .lit must be byte for byte
    the one of the same brush with origin 0 0 0."""
    lits = []
    for tag, origin in (("rotA", "32 48 0"), ("rotB", "0 0 0")):
        fixture.write_map(tmp / (tag + ".map"), "fixture.wad")
        text = (tmp / (tag + ".map")).read_text()
        text += '{\n"classname" "rotate_object"\n"origin" "%s"\n%s\n}\n' % (
            origin, fixture.brush((-216, -216, 100), (-152, -152, 164), "tred"))
        (tmp / (tag + ".map")).write_text(text)
        fixture.run([tools["qbsp"], tag + ".map"], tmp)
        fixture.run([tools["light"], "-threads", "1", tag + ".bsp"], tmp)
        lits.append(colour(tools["jsh2colour"], tmp / (tag + ".bsp"), ext))
    a, b = (tmp / "rotA.bsp").read_bytes(), (tmp / "rotB.bsp").read_bytes()
    if a == b:
        sys.exit("FAIL: rotate: the origin did not reach the BSP")
    if lits[0] != lits[1]:
        n = sum(1 for x, y in zip(lits[0], lits[1]) if x != y)
        print("FAIL: rotate: a rotate_object at origin 32 48 0 is coloured differently "
              "from the same brush at 0 0 0 (%d bytes)" % n)
        return False
    print("ok: rotate: a rotate_object is coloured where it stands, origin or not")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bindir", type=Path)
    ap.add_argument("--ltface", type=Path, default=LTFACE)
    ap.add_argument("--layout", default="CalcFaceLayout(l, off)")
    a = ap.parse_args()

    tools = {t: a.bindir / t for t in ("qbsp", "light", "jsh2colour")}
    for t, p in tools.items():
        if not p.is_file():
            sys.exit("FAIL: no %s at %s" % (t, p))

    ok = True
    with tempfile.TemporaryDirectory(prefix="jsh2colour-layout-") as tmp:
        tmp = Path(tmp)
        ok &= check_layout(a.ltface, a.layout, tmp)
        fixture.write_wad(tmp / "fixture.wad", sorted(fixture.COLOURS) + [fixture.WALL])
        fixture.write_map(tmp / "fixture.map", "fixture.wad")
        (tmp / "fixture.def").write_text(
            "".join("%s %d %d %d\n" % ((n,) + c) for n, c in fixture.COLOURS.items()))
        fixture.run([tools["qbsp"], "fixture.map"], tmp)
        fixture.run([tools["light"], "-threads", "1", "fixture.bsp"], tmp)
        ext = ["-external", "fixture.def"]
        ok &= check_space(tools, tmp, ext)
        ok &= check_styles(tools, tmp, ext)
        ok &= check_rotate(tools, tmp, ext)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
