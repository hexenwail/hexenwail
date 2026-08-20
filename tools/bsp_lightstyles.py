#!/usr/bin/env python3
"""Answer "can this spot test animated lighting?" from map content, offline.

Two modes:

  bsp_lightstyles.py under <pak> <map.bsp> <x> <y> <z>
      Which world surface does a downtrace from (x,y,z) land on, and does it
      carry an animated lightstyle?  R_LightPointColor traces straight down
      2048 units from an alias model's sample point and lights the model from
      whatever face it hits, so this is exactly the surface that decides
      whether a standing model's lighting can change at all.

  bsp_lightstyles.py scan <dir> [<dir>...]
      Rank every map in those dirs' .pak files by how well it can exercise
      animated lighting: the largest floor face carrying a style in 1..11,
      preferring the biggest-amplitude styles.

WHY THIS EXISTS.  Verifying uhexen2-ayrn (Ironwail's alias light-trace cache)
needs a model standing still on a floor whose lighting actually animates -- on
a steady floor a cache that correctly re-interpolates every frame and one
frozen at its first trace render identically, so the test passes vacuously.
demo1/Blackmarsh, the obvious place to try, has ZERO such floor faces; three
runs there proved nothing before that was noticed.

WHICH STYLES ANIMATE.  Only 1..11, the engine's built-in animated set.  0 is
steady, 255 marks an unused slot, and 32+ are switchable lights that sit at a
fixed value until something triggers them -- useless for this even though they
look interesting in a dump.  Amplitude, best first:

    2, 11   full a..z sweep, ~5s          largest swing, easy to sample
    9       a/z slow strobe, 1.6s         full swing
    4       "mamamama" strobe, 10Hz       full swing but aliases badly unless
                                          you sample far faster than 2s/frame
    1, 6, 10  flicker                     moderate, irregular
    3, 7, 8   candle                      small
    5       gentle pulse                  small

THE BYTE-OFFSET TRAP.  dface_t is

    short planenum; short side; int firstedge; short numedges; short texinfo;
    byte styles[4]; int lightofs;

so styles[] is at byte 12 and lightofs at byte 16, and the struct is 20 bytes.
Reading styles at 14 (i.e. forgetting that texinfo ends at 12, not 14) yields
styles[2], styles[3] and the low half of lightofs, which decodes as plausible
but entirely fictional style numbers -- it reported 905 animated floor faces in
a map that has none.  If you adapt this parser, keep those offsets.
"""
import glob
import os
import struct
import sys

# Rank of each animated style by how useful its amplitude is for a visual test.
STYLE_PREF = {2: 0, 11: 0, 9: 1, 4: 2, 1: 3, 6: 3, 10: 3, 3: 4, 7: 4, 8: 4, 5: 5}


def read_pak(path):
    """Yield (name, bytes) for every member of a Quake/Hexen II .pak."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'PACK':
        return
    dirofs, dirlen = struct.unpack_from('<ii', data, 4)
    for i in range(dirlen // 64):
        off = dirofs + i * 64
        name = data[off:off + 56].split(b'\0')[0].decode('latin-1')
        filepos, filelen = struct.unpack_from('<ii', data, off + 56)
        yield name, data[filepos:filepos + filelen]


def load_faces(b):
    """Parse a BSP29's faces into dicts with plane, winding, styles, lightofs."""
    if len(b) < 4 + 15 * 8 or struct.unpack_from('<i', b, 0)[0] != 29:
        return None
    lumps = [struct.unpack_from('<ii', b, 4 + i * 8) for i in range(15)]

    po, pl = lumps[1]
    planes = [struct.unpack_from('<4fi', b, po + i * 20) for i in range(pl // 20)]
    vo, vl = lumps[3]
    verts = [struct.unpack_from('<3f', b, vo + i * 12) for i in range(vl // 12)]
    eo, el = lumps[12]
    edges = [struct.unpack_from('<2H', b, eo + i * 4) for i in range(el // 4)]
    so, sl = lumps[13]
    surfedges = [struct.unpack_from('<i', b, so + i * 4)[0] for i in range(sl // 4)]

    fo, fl = lumps[7]
    faces = []
    for i in range(fl // 20):
        o = fo + i * 20
        planenum, side, firstedge, numedges, _texinfo = struct.unpack_from('<hhihh', b, o)
        styles = list(b[o + 12:o + 16])                     # NOT o+14 -- see module docstring
        lightofs = struct.unpack_from('<i', b, o + 16)[0]   # NOT o+18
        n = list(planes[planenum][:3])
        d = planes[planenum][3]
        if side:
            n = [-c for c in n]
            d = -d
        pts = []
        for k in range(numedges):
            se = surfedges[firstedge + k]
            e = edges[abs(se)]
            pts.append(verts[e[0] if se >= 0 else e[1]])
        faces.append(dict(i=i, n=n, d=d, pts=pts, styles=styles, lightofs=lightofs))
    return faces


def animated(styles):
    return sorted({s for s in styles if 1 <= s <= 11})


def inside_xy(pts, x, y):
    """Even-odd point-in-polygon on the XY projection."""
    c, n = False, len(pts)
    for i in range(n):
        x0, y0 = pts[i][0], pts[i][1]
        x1, y1 = pts[(i + 1) % n][0], pts[(i + 1) % n][1]
        if (y0 > y) != (y1 > y):
            if x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                c = not c
    return c


def find_map(pak, mapname):
    want = mapname.lower()
    if not want.endswith('.bsp'):
        want += '.bsp'
    for nm, blob in read_pak(pak):
        if nm.lower().endswith(want):
            return load_faces(blob)
    return None


def cmd_under(pak, mapname, x, y, z):
    faces = find_map(pak, mapname)
    if faces is None:
        print(f"{mapname} not found in {pak} (or not BSP29)", file=sys.stderr)
        return 1

    hits = []
    for f in faces:
        if f['n'][2] < 0.7:                 # not floor-ish; a downtrace ignores it
            continue
        fz = (f['d'] - f['n'][0] * x - f['n'][1] * y) / f['n'][2]
        if fz > z + 1.0 or not inside_xy(f['pts'], x, y):
            continue
        hits.append((fz, f))

    if not hits:
        print(f"no floor face under ({x:g}, {y:g}, {z:g})")
        return 1
    hits.sort(key=lambda h: -h[0])          # highest floor below the point is what we hit
    fz, f = hits[0]
    anim = animated(f['styles'])
    switch = [s for s in f['styles'] if 32 <= s < 255]

    print(f"downtrace from ({x:g}, {y:g}, {z:g}) hits face #{f['i']} at z={fz:.1f}")
    print(f"  drop            : {z - fz:.1f} units")
    print(f"  styles[]        : {f['styles']}")
    print(f"  has lightdata   : {f['lightofs'] >= 0}")
    if anim:
        print(f"  ANIMATED (1-11) : {anim}")
    else:
        print("  ANIMATED (1-11) : NONE -- a model here cannot demonstrate lightstyle liveness")
    if switch:
        print(f"  switchable (32+): {switch}  (static until triggered)")
    return 0


def cmd_scan(roots):
    rows = []
    for root in roots:
        for pak in sorted(glob.glob(os.path.join(root, '*.pak'))):
            for nm, blob in read_pak(pak):
                if not nm.lower().endswith('.bsp'):
                    continue
                faces = load_faces(blob)
                if not faces:
                    continue
                cands = []
                for f in faces:
                    if f['n'][2] < 0.85 or f['lightofs'] < 0 or not f['pts']:
                        continue
                    anim = animated(f['styles'])
                    if not anim:
                        continue
                    pts = f['pts']
                    area = ((max(p[0] for p in pts) - min(p[0] for p in pts)) *
                            (max(p[1] for p in pts) - min(p[1] for p in pts)))
                    c = tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
                    cands.append((area, c, anim))
                if not cands:
                    continue
                cands.sort(key=lambda r: (min(STYLE_PREF.get(s, 9) for s in r[2]), -r[0]))
                area, c, anim = cands[0]
                rows.append((min(STYLE_PREF.get(s, 9) for s in anim), -area,
                             nm, len(cands), area, c, anim))

    if not rows:
        print("no map in those dirs has a floor face with an animated style")
        return 1
    rows.sort()
    print(f"{'map':28} {'#animFloors':>11} {'bestArea':>9}  {'centroid':^28}  styles")
    for _, _, nm, n, area, c, anim in rows:
        print(f"{nm:28} {n:11d} {area:9.0f}  ({c[0]:8.1f} {c[1]:8.1f} {c[2]:7.1f})  {anim}")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == 'under' and len(argv) == 7:
        return cmd_under(argv[2], argv[3], float(argv[4]), float(argv[5]), float(argv[6]))
    if len(argv) >= 3 and argv[1] == 'scan':
        return cmd_scan(argv[2:])
    print(__doc__.split('WHY THIS EXISTS')[0].strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
