#!/usr/bin/env python3
"""Write a game directory whose models replace demo1's tombstones with a
skeletal (MD5) and an MD3 model, for tools/sweep-alias-gpu.steps (#340 P2e).

The Hexen II data has no skeletal or MD3 model, so the alias GPU paths that
read them -- GL_DrawAliasSkeletal's bone storage buffer and IQM vertex
layout, the instanced path's MD3 pose buffer -- have nothing to draw in a
retail sweep.  gl_model.c's r_enhancedmodels takes models/foo.md5mesh or
models/foo.md3 in place of models/foo.mdl from the .mdl's game directory or a
later one, so a game directory holding these two files turns every tombstone
in demo1's graveyard into one or the other:

  models/tombstn1.md5mesh + .md5anim  a 24 x 24 x 48 box, two joints: the
      bottom four corners on the root alone, the top four half on the root
      and half on the top joint.  The animation's frame 0 -- the frame the
      tombstones stand at -- bends the top joint 40 degrees about X and lifts
      it 6 units, so the bone matrices are NOT identity: a vertex skinned
      with the wrong bone index, the wrong weights, or no bones at all (the
      zeroed storage stand-in) lands somewhere else.  Frame 1 is the rest.
  models/tombstn2.md3  the same box, one surface, a flat normal per face (24
      vertices), two frames (frame 1 lifted 8 units), drawn by the instanced
      path from its MD3 pose buffer.

Nothing here is committed binary: the .md3 is written each time.  Both
models are text-checkable: every number is on this page.

Usage:  mk-alias-testgame.py <basedir> [game]     (game defaults to "skel")
writes <basedir>/<game>/models/.  The basedir also needs data1: copy it whole
(cp -a ~/hexen2/data1 <basedir>/), since tools/headless-drive.sh's sandbox
binds the basedir alone and a symlink out of it does not resolve there.
"""
import math
import os
import struct
import sys

W, H = 12.0, 48.0          # half width, height
MID = 24.0                 # the top joint's rest height
BEND = 40.0                # frame 0's bend of the top joint about X, degrees
LIFT = 6.0                 # and its lift, units

CORNERS = [(-W, -W, 0), (W, -W, 0), (W, W, 0), (-W, W, 0),
           (-W, -W, H), (W, -W, H), (W, W, H), (-W, W, H)]
# outward faces as corner quads, counter-clockwise seen from outside
FACES = [((0, 3, 2, 1), (0, 0, -1)), ((4, 5, 6, 7), (0, 0, 1)),
         ((0, 1, 5, 4), (0, -1, 0)), ((2, 3, 7, 6), (0, 1, 0)),
         ((1, 2, 6, 5), (1, 0, 0)), ((3, 0, 4, 7), (-1, 0, 0))]


def md5mesh():
    out = ["MD5Version 10", 'commandline "hexenwail tools/mk-alias-testgame.py (#340 P2e)"', "",
           "numJoints 2", "numMeshes 1", "", "joints {",
           '\t"root"\t-1 ( 0.000000 0.000000 0.000000 ) ( 0.000000 0.000000 0.000000 )',
           '\t"top"\t0 ( 0.000000 0.000000 %f ) ( 0.000000 0.000000 0.000000 )' % MID, "}", "",
           "mesh {", '\tshader "models/test/skelbox"', "", "\tnumverts 8"]
    weights, w = [], 0
    for i, (x, y, z) in enumerate(CORNERS):
        s, t = (0.0 if x < 0 else 1.0), (1.0 if z == 0 else 0.0)
        if z == 0:
            out.append("\tvert %d ( %f %f ) %d 1" % (i, s, t, w))
            weights.append("\tweight %d 0 1.000000 ( %f %f %f )" % (w, x, y, z))
            w += 1
        else:
            out.append("\tvert %d ( %f %f ) %d 2" % (i, s, t, w))
            weights.append("\tweight %d 0 0.500000 ( %f %f %f )" % (w, x, y, z))
            weights.append("\tweight %d 1 0.500000 ( %f %f %f )" % (w + 1, x, y, z - MID))
            w += 2
    tris = []
    for (a, b, c, d), _n in FACES:
        tris += [(a, b, c), (a, c, d)]
    out += ["", "\tnumtris %d" % len(tris)]
    out += ["\ttri %d %d %d %d" % (i, a, b, c) for i, (a, b, c) in enumerate(tris)]
    out += ["", "\tnumweights %d" % len(weights)] + weights + ["}", ""]
    return "\n".join(out)


def md5anim():
    # MD5 keeps a unit quaternion's x, y, z and rebuilds w as the NEGATIVE
    # root (engine/tests/md5/README.md), so a rotation is stored with w <= 0.
    half = math.radians(BEND) / 2
    qx = -math.sin(half)
    out = ["MD5Version 10", 'commandline "hexenwail tools/mk-alias-testgame.py (#340 P2e)"', "",
           "numFrames 2", "numJoints 2", "frameRate 10", "numAnimatedComponents 6", "",
           "hierarchy {", '\t"root"\t-1 0 0\t//', '\t"top"\t0 63 0\t// root', "}", "",
           "bounds {"] + ["\t( %f %f -4 ) ( %f %f %f )" % (-H, -H, H, H, H + LIFT)] * 2 + ["}", "",
           "baseframe {", "\t( 0 0 0 ) ( 0 0 0 )", "\t( 0 0 %f ) ( 0 0 0 )" % MID, "}", "",
           "frame 0 {", "\t0 0 %f %f -0.000000 -0.000000" % (MID + LIFT, qx), "}", "",
           "frame 1 {", "\t0 0 %f -0.000000 -0.000000 -0.000000" % MID, "}", ""]
    return "\n".join(out)


def md3():
    def latlng(n):
        """MD3's normal bytes: longitude from +Z, latitude about Z, 256ths of a turn."""
        x, y, z = n
        lng = int(round(math.degrees(math.acos(z)) * 256 / 360)) & 255
        lat = int(round(math.degrees(math.atan2(y, x)) * 256 / 360)) & 255
        return lng, lat

    verts0, sts, tris = [], [], []
    for (a, b, c, d), n in FACES:
        base = len(verts0)
        lng, lat = latlng(n)
        for k, ci in enumerate((a, b, c, d)):
            verts0.append((CORNERS[ci], lng, lat))
            sts.append(((0.0, 1.0, 1.0, 0.0)[k], (1.0, 1.0, 0.0, 0.0)[k]))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]

    def framev(dz):
        return b"".join(struct.pack("<3hBB", int(x * 64), int(y * 64), int((z + dz) * 64), lng, lat)
                        for (x, y, z), lng, lat in verts0)

    nverts, ntris, nframes = len(verts0), len(tris), 2
    shead = 4 + 64 + 4 * 10
    shaders = struct.pack("<64si", b"models/test/md3box", 0)
    tridata = b"".join(struct.pack("<3i", *t) for t in tris)
    stdata = b"".join(struct.pack("<2f", *s) for s in sts)
    vdata = framev(0) + framev(8)
    o_sh = shead
    o_tri = o_sh + len(shaders)
    o_st = o_tri + len(tridata)
    o_v = o_st + len(stdata)
    o_end = o_v + len(vdata)
    surface = (b"IDP3" + b"box".ljust(64, b"\0") +
               struct.pack("<10i", 0, nframes, 1, nverts, ntris, o_tri, o_sh, o_st, o_v, o_end) +
               shaders + tridata + stdata + vdata)

    def frame(dz, name):
        return struct.pack("<3f3f3ff16s", -W, -W, dz, W, W, H + dz, 0, 0, 0,
                           math.sqrt(2 * W * W + H * H), name)
    frames = frame(0, b"rest") + frame(8, b"lifted")
    head_size = 4 + 4 + 64 + 4 * 9
    o_frames = head_size
    o_surf = o_frames + len(frames)
    end = o_surf + len(surface)
    head = (b"IDP3" + struct.pack("<i", 15) + b"md3box".ljust(64, b"\0") +
            struct.pack("<9i", 0, nframes, 0, 1, 0, o_frames, o_surf, o_surf, end))
    assert len(head) == head_size
    return head + frames + surface


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = os.path.join(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "skel", "models")
    os.makedirs(d, exist_ok=True)
    for name, data in (("tombstn1.md5mesh", md5mesh()), ("tombstn1.md5anim", md5anim())):
        with open(os.path.join(d, name), "w") as f:
            f.write(data)
    with open(os.path.join(d, "tombstn2.md3"), "wb") as f:
        f.write(md3())
    print("wrote %s: tombstn1.md5mesh / .md5anim (skeletal), tombstn2.md3" % d)


if __name__ == "__main__":
    main()
