#!/usr/bin/env python3
"""Write engine/tests/md3/quad.md3 -- the fixture md3check.c reads.

Two surfaces so the concatenation md3mesh.c does is actually exercised, two
frames so pose stride and lerping are, and geometry chosen so every number the
loader should produce is one a human can check by eye:

  surface "front": one triangle, verts at (0,0,0) (64,0,0) (0,64,0),
                   st (0,0) (1,0) (0,1), normal +Z (lat 0, lng 0 -> (0,0,1))
  surface "back" : one triangle, verts at (0,0,64) (-64,0,64) (0,-64,64),
                   st (1,1) (0,1) (1,0), normal +X (lat 0, lng 64 -> (1,0,0))

Frame 1 is frame 0 with every Z raised by 64 units, so a lerp at 0.5 lands on
+32 and nothing else moves.

MD3 stores positions as shorts at 1/64 units, so "64 units" is 4096 on disk and
1.0 in model space.
"""
import struct
import os

MD3_IDENT = b'IDP3'
MD3_VERSION = 15


def frame(mins, maxs, origin, radius, name):
    return struct.pack('<3f3f3ff16s',
                       *mins, *maxs, *origin, radius, name.encode())


def vertex(x, y, z, lng, lat):
    """xyz in model units; lng/lat are the raw byte encodings."""
    return struct.pack('<3hBB', int(x * 64), int(y * 64), int(z * 64), lng, lat)


def surface(name, shader, tris, sts, frames_verts):
    nverts = len(sts)
    ntris = len(tris)
    nframes = len(frames_verts)

    header_size = 4 + 64 + 4 * 10          # ident, name, then 10 ints
    ofs_shaders = header_size
    shaders = struct.pack('<64si', shader.encode(), 0)
    ofs_triangles = ofs_shaders + len(shaders)
    tridata = b''.join(struct.pack('<3i', *t) for t in tris)
    ofs_st = ofs_triangles + len(tridata)
    stdata = b''.join(struct.pack('<2f', *c) for c in sts)
    ofs_verts = ofs_st + len(stdata)
    vdata = b''.join(b''.join(fv) for fv in frames_verts)
    ofs_end = ofs_verts + len(vdata)

    head = (MD3_IDENT + name.encode().ljust(64, b'\0') +
            struct.pack('<10i', 0, nframes, 1, nverts, ntris,
                        ofs_triangles, ofs_shaders, ofs_st, ofs_verts, ofs_end))
    assert len(head) == header_size, len(head)
    return head + shaders + tridata + stdata + vdata


def main():
    front = surface(
        'front', 'models/test/quad_front',
        tris=[(0, 1, 2)],
        sts=[(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)],
        frames_verts=[
            [vertex(0, 0, 0, 0, 0), vertex(1, 0, 0, 0, 0), vertex(0, 1, 0, 0, 0)],
            [vertex(0, 0, 1, 0, 0), vertex(1, 0, 1, 0, 0), vertex(0, 1, 1, 0, 0)],
        ])
    back = surface(
        'back', 'models/test/quad_back',
        tris=[(0, 1, 2)],
        sts=[(1.0, 1.0), (0.0, 1.0), (1.0, 0.0)],
        frames_verts=[
            [vertex(0, 0, 1, 64, 0), vertex(-1, 0, 1, 64, 0), vertex(0, -1, 1, 64, 0)],
            [vertex(0, 0, 2, 64, 0), vertex(-1, 0, 2, 64, 0), vertex(0, -1, 2, 64, 0)],
        ])

    frames = (frame((-1, -1, 0), (1, 1, 1), (0, 0, 0), 2.0, 'rest') +
              frame((-1, -1, 1), (1, 1, 2), (0, 0, 0), 2.0, 'lifted'))

    header_size = 4 + 4 + 64 + 4 * 9
    ofs_frames = header_size
    ofs_tags = ofs_frames + len(frames)
    ofs_surfaces = ofs_tags
    surfaces = front + back
    ofs_end = ofs_surfaces + len(surfaces)

    head = (MD3_IDENT + struct.pack('<i', MD3_VERSION) +
            b'quad'.ljust(64, b'\0') +
            struct.pack('<9i', 0, 2, 0, 2, 0,
                        ofs_frames, ofs_tags, ofs_surfaces, ofs_end))
    assert len(head) == header_size, len(head)

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'quad.md3')
    with open(out, 'wb') as f:
        f.write(head + frames + surfaces)
    print('wrote %s (%d bytes)' % (out, ofs_end))


if __name__ == '__main__':
    main()
