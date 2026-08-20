import math

def quat_axis_angle(ax, ay, az, deg):
    """Return the (x,y,z) triple id's MD5 format stores, given that the
    loader always reconstructs w as NEGATIVE.  A quaternion and its
    negation are the same rotation, so we pick whichever sign puts w<=0."""
    th = math.radians(deg) / 2.0
    s = math.sin(th)
    x, y, z, w = ax*s, ay*s, az*s, math.cos(th)
    if w > 0:
        x, y, z, w = -x, -y, -z, -w
    return x, y, z

JOINTS = [
    ('root', -1, (0.0, 0.0, 0.0),  (0.0, 0.0, 0.0)),
    ('arm',   0, (0.0, 0.0, 16.0), (0.0, 0.0, 0.0)),
]

# (u, v, [(bone, bias, localpos)])
VERTS = [
    (0.0, 0.0, [(0, 1.0, (0.0, -4.0, 0.0))]),
    (1.0, 0.0, [(0, 1.0, (0.0,  4.0, 0.0))]),
    (0.0, 1.0, [(1, 1.0, (0.0, -4.0, 0.0))]),
    (1.0, 1.0, [(1, 1.0, (0.0,  4.0, 0.0))]),
    # a vertex split 50/50 across both bones, to exercise real blending
    (0.5, 0.5, [(0, 0.5, (0.0, 0.0, 8.0)), (1, 0.5, (0.0, 0.0, -8.0))]),
]
TRIS = [(0, 1, 2), (2, 1, 3), (0, 4, 2)]

def write_mesh(path):
    L = []
    L.append('MD5Version 10')
    L.append('commandline "hexenwail synthetic test"')
    L.append('')
    L.append('numJoints %d' % len(JOINTS))
    L.append('numMeshes 1')
    L.append('')
    L.append('joints {')
    for name, parent, t, q in JOINTS:
        L.append('\t"%s"\t%d ( %f %f %f ) ( %f %f %f )\t\t// ' % (name, parent, t[0], t[1], t[2], q[0], q[1], q[2]))
    L.append('}')
    L.append('')
    L.append('mesh {')
    L.append('\tshader "models/test/testskin"')
    L.append('')
    L.append('\tnumverts %d' % len(VERTS))
    wi = 0
    for i, (u, v, ws) in enumerate(VERTS):
        L.append('\tvert %d ( %f %f ) %d %d' % (i, u, v, wi, len(ws)))
        wi += len(ws)
    L.append('')
    L.append('\tnumtris %d' % len(TRIS))
    for i, t in enumerate(TRIS):
        L.append('\ttri %d %d %d %d' % (i, t[0], t[1], t[2]))
    L.append('')
    L.append('\tnumweights %d' % wi)
    wi = 0
    for (u, v, ws) in VERTS:
        for (bone, bias, p) in ws:
            L.append('\tweight %d %d %f ( %f %f %f )' % (wi, bone, bias, p[0], p[1], p[2]))
            wi += 1
    L.append('}')
    L.append('')
    open(path, 'w').write('\n'.join(L))

FRAMES = [0.0, 45.0, 90.0]

def write_anim(path):
    L = []
    L.append('MD5Version 10')
    L.append('commandline "hexenwail synthetic test"')
    L.append('')
    L.append('numFrames %d' % len(FRAMES))
    L.append('numJoints %d' % len(JOINTS))
    L.append('frameRate 10')
    L.append('numAnimatedComponents 6')
    L.append('')
    L.append('hierarchy {')
    L.append('\t"root"\t-1 0 0\t//')
    L.append('\t"arm"\t0 63 0\t// root')
    L.append('}')
    L.append('')
    L.append('bounds {')
    for _ in FRAMES:
        L.append('\t( -20 -20 -4 ) ( 20 20 24 )')
    L.append('}')
    L.append('')
    L.append('baseframe {')
    L.append('\t( 0 0 0 ) ( 0 0 0 )')
    L.append('\t( 0 0 16 ) ( 0 0 0 )')
    L.append('}')
    L.append('')
    for fi, deg in enumerate(FRAMES):
        qx, qy, qz = quat_axis_angle(1, 0, 0, deg)
        L.append('frame %d {' % fi)
        L.append('\t0 0 16 %f %f %f' % (qx, qy, qz))
        L.append('}')
        L.append('')
    open(path, 'w').write('\n'.join(L))

write_mesh('test.md5mesh')
write_anim('test.md5anim')
print('generated test.md5mesh / test.md5anim')
