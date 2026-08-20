#!/usr/bin/env python3
"""bsp_texcount.py -- count the distinct textures each shipped map actually uses.

Why this exists (uhexen2-im9g): the bindless-texture bead's gate 1 is "profile
first and show that per-draw texture binding is actually a dominant bucket".
r_speeds 2 now has a c_texbinds counter for that, but reading it needs a GPU and
a person.  This gets at the same question from the other end and needs neither.

DrawTextureChains (engine/hexen2/gl_rsurf.c) loops `ti < worldmodel->numtextures`
and issues one GL_Bind per chain that has visible surfaces.  So the number of
distinct textures referenced by faces in a .bsp is a hard ceiling on the world
pass's texture binds per frame, decided by the content and knowable offline.

Reports, per map: face count, miptex entries in the lump, distinct textures
actually referenced by faces (the ceiling), and how many faces are turb (`*name`)
or sky -- the last two because uhexen2-9o7u cares about turb surface counts.

Usage:  tools/bsp_texcount.py <gamedir> [<gamedir> ...]
  e.g.  tools/bsp_texcount.py ~/hexen2/data1 ~/hexen2/portals ~/hexen2/sot

Reads maps out of .pak files in each directory.  BSP version 29 only, which is
every Hexen II map; BSP2 maps are skipped rather than misparsed.
"""
import struct, sys, os, glob

def read_pak(path):
    """Yield (name, bytes) for every member of a Quake/Hexen II .pak."""
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

def scan_bsp(name, b):
    """Parse one BSP29 and return its texture/face census, or None."""
    if len(b) < 4 + 15 * 8:
        return None
    if struct.unpack_from('<i', b, 0)[0] != 29:	# BSPVERSION; BSP2 not handled
        return None
    lumps = [struct.unpack_from('<ii', b, 4 + i*8) for i in range(15)]

    tofs, tlen = lumps[2]			# LUMP_TEXTURES
    if tlen < 4:
        return None
    nummiptex = struct.unpack_from('<i', b, tofs)[0]
    texnames = []
    for i in range(nummiptex):
        dofs = struct.unpack_from('<i', b, tofs + 4 + i*4)[0]
        if dofs < 0:				# a hole in the lump is legal
            texnames.append(None)
            continue
        texnames.append(b[tofs+dofs:tofs+dofs+16].split(b'\0')[0].decode('latin-1'))

    xofs, xlen = lumps[6]			# LUMP_TEXINFO, 40 bytes each
    ntexinfo = xlen // 40
    ti_miptex = [struct.unpack_from('<i', b, xofs + i*40 + 32)[0] for i in range(ntexinfo)]

    # LUMP_FACES, 20 bytes each.  texinfo is at byte 10:
    # short planenum, short side, int firstedge, short numedges, short texinfo.
    fofs, flen = lumps[7]
    nfaces = flen // 20
    used, turb_faces, sky_faces = set(), 0, 0
    for i in range(nfaces):
        ti = struct.unpack_from('<h', b, fofs + i*20 + 10)[0]
        if not (0 <= ti < ntexinfo):
            continue
        mt = ti_miptex[ti]
        used.add(mt)
        if 0 <= mt < len(texnames) and texnames[mt]:
            nm = texnames[mt]
            if nm.startswith('*'):
                turb_faces += 1
            elif nm.lower().startswith('sky'):
                sky_faces += 1

    return dict(name=name, nummiptex=nummiptex, used=len(used),
                nfaces=nfaces, turb_faces=turb_faces, sky_faces=sky_faces)

def main(roots):
    results = []
    for root in roots:
        for pak in sorted(glob.glob(os.path.join(root, '*.pak'))):
            for nm, blob in read_pak(pak):
                if nm.lower().endswith('.bsp'):
                    r = scan_bsp(nm, blob)
                    if r:
                        r['pak'] = os.path.join(os.path.basename(root),
                                                os.path.basename(pak))
                        results.append(r)
    if not results:
        print("no BSP29 maps found in:", ", ".join(roots), file=sys.stderr)
        return 1

    results.sort(key=lambda r: -r['used'])
    print(f"{'map':34} {'faces':>7} {'miptex':>7} {'used':>5} {'turbF':>6} {'skyF':>5}  pak")
    for r in results:
        print(f"{r['name']:34} {r['nfaces']:7d} {r['nummiptex']:7d} {r['used']:5d} "
              f"{r['turb_faces']:6d} {r['sky_faces']:5d}  {r['pak']}")

    used = [r['used'] for r in results]
    faces = [r['nfaces'] for r in results]
    print()
    print(f"maps scanned: {len(results)}")
    print(f"distinct textures referenced by faces -- max {max(used)}, mean {sum(used)/len(used):.1f}")
    print(f"    ^ this is the per-frame ceiling on world-pass GL_Bind calls")
    print(f"faces -- max {max(faces)}, mean {sum(faces)/len(faces):.0f}")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
