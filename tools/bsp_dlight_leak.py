#!/usr/bin/env python3
"""Count the world faces a dynamic light lights through a wall (GitHub #97).

Every renderer tier lights both sides of a face's plane: R_AddDynamicLights
reduces the radius by fabs(dist) (gl_rsurf.c, r_surf.c), and the GPU path's
ClusteredDlight by abs(pdist) (world_dlight.inc).  Nothing asks whether the
light can SEE the face, so a torch on one side of a wall lights the other.

This is the oracle for how often that happens on real maps, and for how much of
it each candidate fix removes.  It needs no engine: it reads the BSPs straight
out of the paks.

For every map it puts a light at each light_torch* / light_flame* origin, 8
units up (where cl_main.c puts the XF_TORCH_GLOW dlight), and for every world
face within the light's reach -- the face polygon's closest point is under the
radius -- it traces hull-0 segments from the light to 2n+1 samples on the face
(n vertices, n edge midpoints, the centroid; each pulled a unit toward the
centroid and lifted a unit off the face's FRONT side).  Sky and liquid faces are
skipped: sky takes no lightmap, and liquids are lit from both sides on purpose.

  reachable  the face is within the light's radius: vanilla would light it
  leaking    reachable, and EVERY sample is blocked by solid or sky
  one-sided  the light is behind the face's front plane by more than --eps
             (the test P1 adds on every tier)
  PVS        no leaf holding the face is in the light leaf's PVS

The report gives, per map, leaking / reachable and the share of the leaking
faces each fix removes.  Front-facing faces (not removed by one-sided) are
split into fully occluded (every sample blocked: what one-sided misses) and
partly occluded (some blocked, some not).

    python3 tools/bsp_dlight_leak.py [--basedir ~/hexen2] [--map demo2] \\
        [--radius 200] [--eps 2] [--scenes N] [--out report.json] [paks...]

With no paks it reads <basedir>/data1/pak0.pak and pak1.pak.  --scenes N lists
up to N candidate repro scenes per map and category.  Nothing is written except
the JSON report --out names.  Standard library only; BSP29 and BSP2.
"""
import argparse
import json
import math
import os
import re
import struct
import sys

CONTENTS_SOLID = -2
CONTENTS_SKY = -6
BSP2_MAGIC = 0x32505342     # 'BSP2'
(L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES,
 L_TEXINFO, L_FACES, L_LIGHTING, L_CLIPNODES, L_LEAFS, L_MARKSURFACES,
 L_EDGES, L_SURFEDGES, L_MODELS) = range(15)


def read_pak(path):
    """{name: bytes} for a Quake/Hexen II .pak."""
    with open(path, 'rb') as f:
        d = f.read()
    magic, ofs, ln = struct.unpack_from('<4sii', d, 0)
    if magic != b'PACK':
        raise ValueError('%s: not a pak file' % path)
    out = {}
    for i in range(ln // 64):
        name, o, s = struct.unpack_from('<56sii', d, ofs + i * 64)
        out[name.split(b'\0')[0].decode('latin1')] = d[o:o + s]
    return out


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


class Bsp:
    def __init__(self, b):
        ver = struct.unpack_from('<i', b, 0)[0]
        if ver == 29:
            bsp2 = False
        elif ver == BSP2_MAGIC:
            bsp2 = True
        else:
            raise ValueError('unrecognised BSP version %r' % ver)
        lumps = [struct.unpack_from('<ii', b, 4 + 8 * i) for i in range(15)]

        def lump(i):
            o, n = lumps[i]
            return o, n

        o, n = lump(L_ENTITIES)
        self.entities = b[o:o + n].split(b'\0')[0].decode('latin1')

        o, n = lump(L_PLANES)
        self.planes = [struct.unpack_from('<4fi', b, o + i * 20) for i in range(n // 20)]

        o, n = lump(L_VERTEXES)
        self.verts = [struct.unpack_from('<3f', b, o + i * 12) for i in range(n // 12)]

        o, n = lump(L_VISIBILITY)
        self.vis = b[o:o + n]

        if bsp2:
            nfmt, nsz, lfmt, lsz = '<i2i6fII', 44, '<ii6fII4B', 44
            ffmt, fsz, efmt, esz, mfmt, msz = '<iiiii4Bi', 28, '<II', 8, '<I', 4
        else:
            nfmt, nsz, lfmt, lsz = '<i2h6hHH', 24, '<ii6hHH4B', 28
            ffmt, fsz, efmt, esz, mfmt, msz = '<hhihh4Bi', 20, '<HH', 4, '<H', 2

        o, n = lump(L_NODES)
        self.nodes = []
        for i in range(n // nsz):
            v = struct.unpack_from(nfmt, b, o + i * nsz)
            self.nodes.append((v[0], v[1], v[2]))       # planenum, child0, child1

        o, n = lump(L_LEAFS)
        self.leafs = []
        for i in range(n // lsz):
            v = struct.unpack_from(lfmt, b, o + i * lsz)
            self.leafs.append((v[0], v[1], v[8], v[9]))  # contents, visofs, firstmark, nummark

        o, n = lump(L_TEXINFO)
        self.texinfo = [struct.unpack_from('<8fii', b, o + i * 40) for i in range(n // 40)]

        o, n = lump(L_TEXTURES)
        self.texnames = []
        if n:
            for i in range(struct.unpack_from('<i', b, o)[0]):
                off = struct.unpack_from('<i', b, o + 4 + 4 * i)[0]
                self.texnames.append('' if off < 0 else struct.unpack_from(
                    '<16s', b, o + off)[0].split(b'\0')[0].decode('latin1').lower())

        o, n = lump(L_EDGES)
        self.edges = [struct.unpack_from(efmt, b, o + i * esz) for i in range(n // esz)]

        o, n = lump(L_SURFEDGES)
        self.surfedges = struct.unpack_from('<%di' % (n // 4), b, o)

        o, n = lump(L_MARKSURFACES)
        self.marksurfaces = struct.unpack_from('<%d%s' % (n // msz, mfmt[1]), b, o)

        o, n = lump(L_FACES)
        self.faces = [struct.unpack_from(ffmt, b, o + i * fsz) for i in range(n // fsz)]

        # Hexen II's dmodel_t carries 8 hull heads (80 bytes); Quake's 4 (64).
        o, n = lump(L_MODELS)
        msize = 80 if n % 80 == 0 else 64
        nh = (msize - 48) // 4
        m0 = struct.unpack_from('<9f%dii ii' % nh, b, o)
        self.headnode = m0[9]
        self.visleafs = m0[9 + nh]
        self.world_firstface = m0[10 + nh]
        self.world_numfaces = m0[11 + nh]

        # face -> the leaves that hold it, for the PVS test
        self.face_leafs = {}
        for li, lf in enumerate(self.leafs):
            for k in range(lf[3]):
                self.face_leafs.setdefault(self.marksurfaces[lf[2] + k], []).append(li)

    # -- geometry --------------------------------------------------------
    def face_points(self, fi):
        f = self.faces[fi]
        pts = []
        for k in range(f[3]):
            se = self.surfedges[f[2] + k]
            e = self.edges[abs(se)]
            pts.append(self.verts[e[0] if se >= 0 else e[1]])
        return pts

    def face_front(self, fi):
        """The face's front normal and plane dist (side 1 flips the plane)."""
        f = self.faces[fi]
        p = self.planes[f[0]]
        n, d = (p[0], p[1], p[2]), p[3]
        if f[1]:
            n, d = (-n[0], -n[1], -n[2]), -d
        return n, d

    def face_texname(self, fi):
        ti = self.faces[fi][4]
        if not (0 <= ti < len(self.texinfo)):
            return ''
        mt = self.texinfo[ti][8]
        return self.texnames[mt] if 0 <= mt < len(self.texnames) else ''

    # -- traversal -------------------------------------------------------
    def point_leaf(self, p):
        node = self.headnode
        while node >= 0:
            pl = self.planes[self.nodes[node][0]]
            node = self.nodes[node][1 if dot(p, pl) - pl[3] >= 0 else 2]
        return -node - 1

    def blocked(self, p1, p2):
        """Does the segment p1 -> p2 cross solid or sky in hull 0?"""
        stack = [(self.headnode, p1, p2)]
        planes, nodes, leafs = self.planes, self.nodes, self.leafs
        while stack:
            node, a, b = stack.pop()
            if node < 0:
                c = leafs[-node - 1][0]
                if c == CONTENTS_SOLID or c == CONTENTS_SKY:
                    return True
                continue
            pn, c0, c1 = nodes[node]
            pl = planes[pn]
            t1 = a[0] * pl[0] + a[1] * pl[1] + a[2] * pl[2] - pl[3]
            t2 = b[0] * pl[0] + b[1] * pl[1] + b[2] * pl[2] - pl[3]
            if t1 >= 0 and t2 >= 0:
                stack.append((c0, a, b))
            elif t1 < 0 and t2 < 0:
                stack.append((c1, a, b))
            else:
                m = lerp(a, b, t1 / (t1 - t2))
                near, far = (c0, c1) if t1 >= 0 else (c1, c0)
                stack.append((far, m, b))
                stack.append((near, a, m))
        return False

    def pvs(self, leaf):
        """The set of leaf indices visible from `leaf` (all, if no vis data)."""
        nleafs = len(self.leafs)
        ofs = self.leafs[leaf][1]
        if leaf == 0 or ofs < 0 or not self.vis:
            return set(range(nleafs))
        out = set()
        i, bit = ofs, 0
        nbits = self.visleafs
        while bit < nbits and i < len(self.vis):
            c = self.vis[i]
            i += 1
            if c == 0:
                bit += 8 * self.vis[i]
                i += 1
                continue
            for k in range(8):
                if c & (1 << k) and bit + k < nbits:
                    out.add(bit + k + 1)
            bit += 8
        return out


def poly_distance(p, pts, n, d):
    """Distance from p to a convex planar polygon with plane (n, d)."""
    pd = dot(p, n) - d
    q = (p[0] - n[0] * pd, p[1] - n[1] * pd, p[2] - n[2] * pd)
    inside = True
    best = float('inf')
    m = len(pts)
    for i in range(m):
        a, b = pts[i], pts[(i + 1) % m]
        e = sub(b, a)
        # inward test against the edge, for either winding
        if dot(cross(e, sub(q, a)), n) < 0:
            inside = False
        ee = dot(e, e)
        t = 0.0 if ee == 0 else max(0.0, min(1.0, dot(sub(q, a), e) / ee))
        c = lerp(a, b, t)
        best = min(best, math.sqrt(dot(sub(q, c), sub(q, c))))
    if inside:
        return abs(pd)
    return math.sqrt(pd * pd + best * best)


def samples(pts, n):
    """2n+1 points on the face, a unit inside it and a unit off its front."""
    m = len(pts)
    cen = (sum(p[0] for p in pts) / m, sum(p[1] for p in pts) / m, sum(p[2] for p in pts) / m)
    raw = list(pts) + [lerp(pts[i], pts[(i + 1) % m], 0.5) for i in range(m)] + [cen]
    out = []
    for s in raw:
        v = sub(cen, s)
        ln = math.sqrt(dot(v, v))
        if ln > 1.0:
            s = lerp(s, cen, 1.0 / ln)
        out.append((s[0] + n[0], s[1] + n[1], s[2] + n[2]))
    return out, cen


def parse_entities(text):
    ents = []
    for block in re.findall(r'\{([^{}]*)\}', text):
        ents.append(dict(re.findall(r'"([^"]*)"\s*"([^"]*)"', block)))
    return ents


def lights_of(bsp):
    out = []
    for e in parse_entities(bsp.entities):
        cn = e.get('classname', '')
        if not (cn.startswith('light_torch') or cn.startswith('light_flame')):
            continue
        try:
            o = tuple(float(x) for x in e.get('origin', '').split())
        except ValueError:
            continue
        if len(o) == 3:
            out.append((cn, o))
    return out


def analyse(bsp, radius, eps, nscenes):
    lights = lights_of(bsp)
    faces = []
    for fi in range(bsp.world_firstface, bsp.world_firstface + bsp.world_numfaces):
        name = bsp.face_texname(fi)
        if name.startswith('sky') or name.startswith('*'):
            continue
        pts = bsp.face_points(fi)
        if len(pts) < 3:
            continue
        n, d = bsp.face_front(fi)
        smp, cen = samples(pts, n)
        faces.append((fi, pts, n, d, smp, cen))

    st = dict(lights=len(lights), reachable=0, leaking=0, by_pvs=0, by_onesided=0,
              by_either=0, front_reachable=0, front_full=0, front_partial=0,
              eps_band=0, eps_band_seen=0)
    scenes = {'behind_wall': [], 'front_occluded': [], 'backface_in_pvs': []}
    for cn, org in lights:
        lo = (org[0], org[1], org[2] + 8.0)
        leaf = bsp.point_leaf(lo)
        vis = bsp.pvs(leaf)
        for fi, pts, n, d, smp, cen in faces:
            if poly_distance(lo, pts, n, d) >= radius:
                continue
            st['reachable'] += 1
            sd = dot(lo, n) - d
            behind = sd < -eps
            nblock = sum(1 for s in smp if bsp.blocked(lo, s))
            full = nblock == len(smp)
            if -eps <= sd < 0:
                # behind the plane, but by no more than the tolerance: a
                # light mounted a unit or two inside its own wall
                st['eps_band'] += 1
                st['eps_band_seen'] += not full
            if not behind:
                st['front_reachable'] += 1
                if full:
                    st['front_full'] += 1
                elif nblock:
                    st['front_partial'] += 1
            if not full:
                continue
            st['leaking'] += 1
            in_pvs = any(l in vis for l in bsp.face_leafs.get(fi, ()))
            st['by_pvs'] += not in_pvs
            st['by_onesided'] += behind
            st['by_either'] += behind or not in_pvs
            cat = ('behind_wall' if behind and not in_pvs else
                   'backface_in_pvs' if behind else
                   'front_occluded' if in_pvs else None)
            if cat and len(scenes[cat]) < nscenes:
                scenes[cat].append(dict(face=fi, light=cn, origin=list(org),
                                        centroid=[round(c, 1) for c in cen],
                                        normal=[round(c, 3) for c in n],
                                        texture=bsp.face_texname(fi)))
    return st, scenes


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paks', nargs='*', help='paks to read (default: basedir data1 pak0/pak1)')
    ap.add_argument('--basedir', default=os.path.expanduser('~/hexen2'))
    ap.add_argument('--map', action='append', help='only this map (repeatable), e.g. demo2')
    ap.add_argument('--radius', type=float, default=200.0)
    ap.add_argument('--eps', type=float, default=2.0, help='one-sided tolerance, units')
    ap.add_argument('--scenes', type=int, default=0, help='list N candidate scenes per category')
    ap.add_argument('--out', help='write the full report as JSON here')
    a = ap.parse_args(argv)

    paks = a.paks or [os.path.join(a.basedir, 'data1', p) for p in ('pak0.pak', 'pak1.pak')]
    maps = {}
    for p in paks:
        if not os.path.isfile(p):
            print('%s: not found' % p, file=sys.stderr)
            return 2
        for name, data in read_pak(p).items():
            m = re.match(r'maps/([^/]+)\.bsp$', name.lower())
            if m:
                maps[m.group(1)] = data        # a later pak overrides, as the engine does
    if a.map:
        missing = [m for m in a.map if m.lower() not in maps]
        if missing:
            print('not in the paks: %s' % ', '.join(missing), file=sys.stderr)
            return 2
        maps = {m.lower(): maps[m.lower()] for m in a.map}

    report = {'radius': a.radius, 'eps': a.eps, 'maps': {}}
    tot = dict.fromkeys(('reachable', 'leaking', 'by_pvs', 'by_onesided', 'by_either',
                         'front_reachable', 'front_full', 'front_partial',
                         'eps_band', 'eps_band_seen'), 0)
    print('%-12s %6s %15s %6s %9s %7s %11s %11s' % (
        'map', 'lights', 'leak/reachable', 'PVS', 'one-sided', 'either',
        'front full', 'front part'))
    for name in sorted(maps):
        try:
            bsp = Bsp(maps[name])
        except Exception as e:                    # a foreign or truncated file
            print('%-12s ERROR %s' % (name, e))
            continue
        st, scenes = analyse(bsp, a.radius, a.eps, a.scenes)
        report['maps'][name] = dict(st, scenes=scenes)
        for k in tot:
            tot[k] += st[k]
        if not st['lights']:
            continue
        print('%-12s %6d %7d/%-7d %5.0f%% %8.0f%% %6.0f%% %10.0f%% %10.0f%%' % (
            name, st['lights'], st['leaking'], st['reachable'],
            pct(st['by_pvs'], st['leaking']), pct(st['by_onesided'], st['leaking']),
            pct(st['by_either'], st['leaking']),
            pct(st['front_full'], st['front_reachable']),
            pct(st['front_partial'], st['front_reachable'])))
        for cat, lst in scenes.items():
            for s in lst:
                print('    %-15s face %5d  %-18s light %s  centroid %s  normal %s  %s' % (
                    cat, s['face'], s['light'], ' '.join('%g' % c for c in s['origin']),
                    ' '.join('%g' % c for c in s['centroid']),
                    ' '.join('%g' % c for c in s['normal']), s['texture']))
    print('%-12s %6s %7d/%-7d %5.0f%% %8.0f%% %6.0f%% %10.0f%% %10.0f%%' % (
        'TOTAL', '', tot['leaking'], tot['reachable'], pct(tot['by_pvs'], tot['leaking']),
        pct(tot['by_onesided'], tot['leaking']), pct(tot['by_either'], tot['leaking']),
        pct(tot['front_full'], tot['front_reachable']),
        pct(tot['front_partial'], tot['front_reachable'])))
    print('\nlight behind a face by <= %g units (kept by the tolerance): %d reachable, '
          '%d of them with a sample the light sees' % (a.eps, tot['eps_band'], tot['eps_band_seen']))
    report['total'] = tot
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(report, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
