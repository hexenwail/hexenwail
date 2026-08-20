#!/usr/bin/env python3
"""bsp_subdiv.py -- what a given gl_subdivide_size costs, computed offline.

Why this exists (uhexen2-9o7u): the bead's remaining measurement is "load the
map at gl_subdivide_size 64, 24 and 16 with developer 1 and read the vert counts
off the console".  GL_ReportSubdivision prints exactly that, but getting it needs
a GPU, a map load and a person.

GL_SubdivideSurface is deterministic geometry over BSP lumps -- it does not touch
the GPU, the renderer or any cvar other than the size.  So the cost half of that
sweep can be computed straight from the .bsp, for every map at once, with no
engine run.  This reimplements SubdividePolygon (engine/h2shared/gl_warp.c:106)
exactly, including the axial midpoint snap, the 8-unit skip guards and the
wrap-around clip, and reports the poly/vert totals the console line would show.

The other half of the bead -- whether the warp READS correctly at a given size --
is a visual judgement and this tool says nothing about it.

Subdivided surfaces are turb (texture name starting '*') and sky (name starting
'sky'), matching Mod_LoadFaces at engine/h2shared/gl_model.c:1499-1517.

Samples per period: the warp's spatial period is 16*pi ~= 50.265 texture units,
so samples/period = 50.265 / size.  Below 2.0 is under Nyquist and the
piecewise-linear interpolation between tile corners aliases the sine rather than
reconstructing it.  64 gives 0.79; 24 gives 2.09; 16 gives 3.14.

Usage:  tools/bsp_subdiv.py <gamedir> [...] [--sizes 64,24,16] [--map NAME]
  e.g.  tools/bsp_subdiv.py ~/hexen2/data1 ~/hexen2/portals --sizes 64,32,24,16

Caveat: the engine computes in C float, this in Python double.  Clip points can
differ in the last bits, so a count may differ by a vertex or two from what the
console prints.  The ratios between sizes -- which is what the decision turns on
-- are unaffected.
"""
import struct, sys, os, glob, math

sys.setrecursionlimit(20000)

def read_pak(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'PACK':
        return
    dirofs, dirlen = struct.unpack_from('<ii', data, 4)
    for i in range(dirlen // 64):
        off = dirofs + i * 64
        name = data[off:off+56].split(b'\0')[0].decode('latin-1')
        filepos, filelen = struct.unpack_from('<ii', data, off+56)
        yield name, data[filepos:filepos+filelen]

def bound_poly(verts):
    mins = [9999999.0] * 3
    maxs = [-9999999.0] * 3
    for v in verts:
        for j in range(3):
            if v[j] < mins[j]: mins[j] = v[j]
            if v[j] > maxs[j]: maxs[j] = v[j]
    return mins, maxs

def subdivide(verts, size, acc):
    """Faithful port of SubdividePolygon (gl_warp.c).  acc = [polys, verts]."""
    mins, maxs = bound_poly(verts)
    for i in range(3):
        m = (mins[i] + maxs[i]) * 0.5
        m = size * math.floor(m / size + 0.5)
        if maxs[i] - m < 8:
            continue
        if m - mins[i] < 8:
            continue
        n = len(verts)
        dist = [verts[j][i] - m for j in range(n)]
        dist.append(dist[0])		# the C code's explicit wrap entry
        front, back = [], []
        for j in range(n):
            v = verts[j]
            vnext = verts[(j + 1) % n]	# matches the wrapped v[3+k] read
            if dist[j] >= 0: front.append(v)
            if dist[j] <= 0: back.append(v)
            if dist[j] == 0 or dist[j+1] == 0:
                continue
            if (dist[j] > 0) != (dist[j+1] > 0):
                frac = dist[j] / (dist[j] - dist[j+1])
                p = tuple(v[k] + frac * (vnext[k] - v[k]) for k in range(3))
                front.append(p)
                back.append(p)
        subdivide(front, size, acc)
        subdivide(back, size, acc)
        return
    acc[0] += 1
    acc[1] += len(verts)

def scan_bsp(name, b, sizes):
    if len(b) < 4 + 15*8 or struct.unpack_from('<i', b, 0)[0] != 29:
        return None
    L = [struct.unpack_from('<ii', b, 4 + i*8) for i in range(15)]

    tofs, tlen = L[2]				# LUMP_TEXTURES
    if tlen < 4:
        return None
    nummiptex = struct.unpack_from('<i', b, tofs)[0]
    texnames = []
    for i in range(nummiptex):
        d = struct.unpack_from('<i', b, tofs + 4 + i*4)[0]
        texnames.append(None if d < 0 else
                        b[tofs+d:tofs+d+16].split(b'\0')[0].decode('latin-1'))

    xofs, xlen = L[6]				# LUMP_TEXINFO, 40 bytes
    ti_miptex = [struct.unpack_from('<i', b, xofs + i*40 + 32)[0]
                 for i in range(xlen // 40)]

    vofs, vlen = L[3]				# LUMP_VERTEXES, 12 bytes
    verts = [struct.unpack_from('<fff', b, vofs + i*12) for i in range(vlen // 12)]

    eofs, elen = L[12]				# LUMP_EDGES, 4 bytes
    edges = [struct.unpack_from('<HH', b, eofs + i*4) for i in range(elen // 4)]

    sofs, slen = L[13]				# LUMP_SURFEDGES, 4 bytes
    surfedges = [struct.unpack_from('<i', b, sofs + i*4)[0] for i in range(slen // 4)]

    fofs, flen = L[7]				# LUMP_FACES, 20 bytes
    out = {sz: [0, 0] for sz in sizes}		# size -> [polys, verts], turb+sky
    turbout = {sz: [0, 0] for sz in sizes}	# size -> [polys, verts], turb only
    n_turb = n_sky = 0
    for i in range(flen // 20):
        firstedge = struct.unpack_from('<i', b, fofs + i*20 + 4)[0]
        numedges  = struct.unpack_from('<h', b, fofs + i*20 + 8)[0]
        ti        = struct.unpack_from('<h', b, fofs + i*20 + 10)[0]
        if not (0 <= ti < len(ti_miptex)):
            continue
        mt = ti_miptex[ti]
        nm = texnames[mt] if 0 <= mt < len(texnames) else None
        if not nm:
            continue
        is_turb = nm.startswith('*')
        is_sky  = nm.lower().startswith('sky')
        if not (is_turb or is_sky):
            continue
        n_turb += is_turb
        n_sky  += is_sky

        poly = []
        for k in range(numedges):
            li = surfedges[firstedge + k]
            poly.append(verts[edges[li][0]] if li > 0 else verts[edges[-li][1]])
        for sz in sizes:
            before = out[sz][1]
            subdivide(poly, float(sz), out[sz])
            if is_turb:
                turbout[sz][0] += 1
                turbout[sz][1] += out[sz][1] - before

    if not (n_turb or n_sky):
        return None
    return dict(name=name, turb=n_turb, sky=n_sky, out=out, turbout=turbout)

def main(argv):
    sizes = [64, 24, 16]
    only = None
    roots = []
    i = 0
    while i < len(argv):
        if argv[i] == '--sizes':
            i += 1; sizes = [int(x) for x in argv[i].split(',')]
        elif argv[i] == '--map':
            i += 1; only = argv[i]
        else:
            roots.append(argv[i])
        i += 1

    rows = []
    for root in roots:
        for pak in sorted(glob.glob(os.path.join(root, '*.pak'))):
            for nm, blob in read_pak(pak):
                if not nm.lower().endswith('.bsp'):
                    continue
                if only and only not in nm:
                    continue
                r = scan_bsp(nm, blob, sizes)
                if r:
                    r['pak'] = os.path.join(os.path.basename(root), os.path.basename(pak))
                    rows.append(r)
    if not rows:
        print("no maps with turb or sky surfaces found", file=sys.stderr)
        return 1

    print("gl_subdivide_size -> samples per period (period = 16*pi = 50.265 units):")
    for sz in sizes:
        print(f"    {sz:4d} -> {50.265/sz:5.2f}{'   (under Nyquist)' if 50.265/sz < 2 else ''}")
    print()

    print("verts emitted per size.  'all' = turb+sky (what GL_ReportSubdivision")
    print("counts); 'turb' = liquid only, which is what the warp decision is about.")
    print()
    hdr = f"{'map':22} {'turbF':>6} {'skyF':>5}"
    for sz in sizes:
        hdr += f" |{('all'+str(sz)):>9} {('turb'+str(sz)):>9}"
    print(hdr)
    rows.sort(key=lambda r: -r['turbout'][sizes[0]][1])
    for r in rows:
        if not r['turb']:
            continue
        line = f"{r['name'][5:]:22} {r['turb']:6d} {r['sky']:5d}"
        for sz in sizes:
            line += f" |{r['out'][sz][1]:9d} {r['turbout'][sz][1]:9d}"
        print(line)

    print()
    base = sizes[0]
    tot  = {sz: sum(r['out'][sz][1] for r in rows) for sz in sizes}
    tott = {sz: sum(r['turbout'][sz][1] for r in rows) for sz in sizes}
    print(f"totals over {len(rows)} maps:")
    for sz in sizes:
        print(f"    size {sz:4d}: {tot[sz]:9d} verts all "
              f"({tot[sz]/tot[base]:5.2f}x)   {tott[sz]:8d} verts turb-only "
              f"({tott[sz]/max(1,tott[base]):5.2f}x)")
    worst = max(rows, key=lambda r: r['turbout'][sizes[-1]][1])
    wb = max(1, worst['turbout'][base][1])
    print(f"worst single map by turb verts at size {sizes[-1]}: {worst['name']} -- "
          f"{worst['turbout'][sizes[-1]][1]} turb verts ({worst['turbout'][sizes[-1]][1]/wb:.2f}x "
          f"its size-{base} count), on {worst['turb']} turb faces")
    print()
    print("A vertex here is 7 floats in the turb VBO path; multiply by 28 bytes for")
    print("the buffer cost, and note this is a load-time cost paid once per map,")
    print("not per frame -- the per-frame cost is the CPU sin loop over these verts.")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
