#!/usr/bin/env python3
"""Test map for GitHub #312: a pool whose rim lies along a sloped floor.

Room x[-320,320] y[-256,256] z[0,320].  A ramp rises along +x on the plane
-x + 2z = 32 (slope 1:2, z=0 at x=-32).  Water fills z[0,64], so its rim
meets the slope along x=96: off the 64-unit subdivision grid, so the rim
vertices are not pinned near a ripple zero.  The side walls at y=+-256 are
the straight-wall tank to compare against.
"""
import struct, sys, pathlib

out = pathlib.Path(sys.argv[1]); pak = pathlib.Path(sys.argv[2])

# --- palette from data1/pak0.pak gfx/palette.lmp
data = pak.read_bytes()
_, dirofs, dirlen = struct.unpack_from("<4sii", data, 0)
pal = None
for i in range(dirlen // 64):
    name, ofs, ln = struct.unpack_from("<56sii", data, dirofs + i * 64)
    if name.split(b"\0")[0] == b"gfx/palette.lmp":
        pal = data[ofs:ofs + 768]
assert pal, "no palette"
def nearest(rgb):
    best = min(range(256), key=lambda i: sum((pal[i*3+k]-rgb[k])**2 for k in range(3)))
    return best

def miptex(name, fn):
    w = h = 64
    mips = []
    for lvl in range(4):
        s = 1 << lvl
        mips.append(bytes(fn(x*s, y*s) for y in range(h>>lvl) for x in range(w>>lvl)))
    hdr = struct.pack("<16sII4I", name.encode(), w, h,
                      40, 40+w*h, 40+w*h+w*h//4, 40+w*h+w*h//4+w*h//16)
    return hdr + b"".join(mips)

blue, cyan = nearest((30, 60, 160)), nearest((80, 150, 220))
tan, brown = nearest((190, 160, 110)), nearest((110, 80, 50))
grey, dgrey = nearest((140, 140, 140)), nearest((80, 80, 80))
lumps = [
    ("*water", miptex("*water", lambda x, y: cyan if ((x//16 + y//16) & 1) else blue)),
    ("ramp",   miptex("ramp",   lambda x, y: brown if (x % 16) < 2 or (y % 16) < 2 else tan)),
    ("wall",   miptex("wall",   lambda x, y: dgrey if ((x//32 + y//32) & 1) else grey)),
]
body = b""; infos = []; ofs = 12
for name, blob in lumps:
    infos.append(struct.pack("<iiiBBBB16s", ofs, len(blob), len(blob), 0x44, 0, 0, 0, name.encode()))
    body += blob; ofs += len(blob)
(out / "ripple312.wad").write_bytes(struct.pack("<4sii", b"WAD2", len(lumps), ofs) + body + b"".join(infos))

# --- brushes, each plane given as (outward normal n, d) for n.p <= d
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def plane(n, d, tex):
    # integer tangents u, v with u x v parallel to n and pointing the same way
    ax = max(range(3), key=lambda k: abs(n[k]))
    seed = (0, 0, 1) if ax != 2 else (1, 0, 0)
    u = cross(seed, n); v = cross(n, u)
    if sum(c*k for c, k in zip(cross(u, v), n)) < 0: u, v = v, u
    nn = sum(c*c for c in n)
    # a point on the plane: n*d/|n|^2 must be integral for the map; scale up
    p = tuple(n[k]*d/nn for k in range(3))
    s = 8
    pts = [tuple(p[k]+u[k]*s for k in range(3)), p, tuple(p[k]+v[k]*s for k in range(3))]
    f = lambda q: "( %s )" % " ".join("%g" % round(c, 4) for c in q)
    return "%s %s %s %s 0 0 0 1 1" % (f(pts[0]), f(pts[1]), f(pts[2]), tex)

def box(lo, hi, tex):
    ps = []
    for k in range(3):
        n = [0, 0, 0]; n[k] = 1;  ps.append(plane(tuple(n), hi[k], tex))
        n = [0, 0, 0]; n[k] = -1; ps.append(plane(tuple(n), -lo[k], tex))
    return ps

brushes = [
    box((-336, -272, -16), (336, 272, 0), "wall"),     # floor
    box((-336, -272, 320), (336, 272, 336), "wall"),   # ceiling
    box((-336, -272, 0), (-320, 272, 320), "wall"),
    box((320, -272, 0), (336, 272, 320), "wall"),
    box((-320, -272, 0), (320, -256, 320), "wall"),
    box((-320, 256, 0), (320, 272, 320), "wall"),
    # ramp wedge: bottom, far x, both y sides, slope -x + 2z <= 32
    [plane((0, 0, -1), 0, "ramp"), plane((1, 0, 0), 320, "ramp"),
     plane((0, 1, 0), 256, "ramp"), plane((0, -1, 0), 256, "ramp"),
     plane((-1, 0, 2), 32, "ramp")],
    box((-320, -256, 0), (320, 256, 64), "*water"),
]
m = ['{\n"classname" "worldspawn"\n"wad" "%s"\n' % (out / "ripple312.wad")]
for b in brushes:
    m.append("{\n" + "\n".join(b) + "\n}\n")
m.append("}\n")
m.append('{\n"classname" "info_player_start"\n"origin" "-200 0 200"\n"angle" "0"\n}\n')
for x in (-200, 0, 200):
    for y in (-150, 150):
        m.append('{\n"classname" "light"\n"origin" "%d %d 280"\n"light" "350"\n}\n' % (x, y))
(out / "ripple312.map").write_text("".join(m))
print("wrote", out / "ripple312.map")
