import math
# 3-bone chain: root -> mid(z=10) -> tip(z=20).  Only `mid` animates, so the
# tip must follow it -- that only happens if parent transforms propagate
# before children are composed.
def q3(ax, ay, az, deg):
    th = math.radians(deg)/2.0; s = math.sin(th)
    x,y,z,w = ax*s, ay*s, az*s, math.cos(th)
    if w > 0: x,y,z,w = -x,-y,-z,-w
    return x,y,z

mesh = """MD5Version 10
commandline ""

numJoints 3
numMeshes 1

joints {
\t"root"\t-1 ( 0 0 0 ) ( 0 0 0 )\t\t//
\t"mid"\t0 ( 0 0 10 ) ( 0 0 0 )\t\t// root
\t"tip"\t1 ( 0 0 20 ) ( 0 0 0 )\t\t// mid
}

mesh {
\tshader "models/test/chain"

\tnumverts 3
\tvert 0 ( 0 0 ) 0 1
\tvert 1 ( 0 1 ) 1 1
\tvert 2 ( 1 1 ) 2 1

\tnumtris 1
\ttri 0 0 1 2

\tnumweights 3
\tweight 0 0 1.0 ( 0 0 0 )
\tweight 1 1 1.0 ( 0 0 0 )
\tweight 2 2 1.0 ( 0 0 0 )
}
"""
open('chain.md5mesh','w').write(mesh)

# mid rotates 90 deg about X; root and tip stay at their baseframe locals.
qx,qy,qz = q3(1,0,0,90)
anim = """MD5Version 10
commandline ""

numFrames 2
numJoints 3
frameRate 20
numAnimatedComponents 6

hierarchy {
\t"root"\t-1 0 0\t//
\t"mid"\t0 63 0\t// root
\t"tip"\t1 0 0\t// mid
}

bounds {
\t( -30 -30 -30 ) ( 30 30 30 )
\t( -30 -30 -30 ) ( 30 30 30 )
}

baseframe {
\t( 0 0 0 ) ( 0 0 0 )
\t( 0 0 10 ) ( 0 0 0 )
\t( 0 0 10 ) ( 0 0 0 )
}

frame 0 {
\t0 0 10 0 0 0
}

frame 1 {
\t0 0 10 %f %f %f
}
""" % (qx,qy,qz)
open('chain.md5anim','w').write(anim)
print("wrote chain.md5mesh / chain.md5anim")
