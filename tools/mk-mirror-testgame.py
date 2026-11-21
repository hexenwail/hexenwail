#!/usr/bin/env python3
"""Write a game directory whose demo1 has a mirror, for
tools/sweep-mirror-gpu.steps (#340 P3d).

The renderer makes a mirror of the world texture named window02_1
(gl_rmisc.c, R_NewMap: strncmp over its first 10 characters) whenever
r_mirroralpha is below 1: R_MirrorChain takes the plane of the FIRST surface
on that texture's chain, R_Mirror renders the scene reflected in it into the
far half of the depth range, and the texture's surfaces are then blended over
it at r_mirroralpha.  No Hexen II map has a window02_1, so this copies the
retail demo1.bsp and renames one of its miptex to it -- the 16-byte name field
of the miptex header, nothing else: the pixels, the texinfo, the faces, the
lighting and the entities are demo1's own, byte for byte.

The texture renamed has to have every one of its faces on one plane in the
world model: R_MirrorChain reflects the whole chain in the first surface's
plane, so a texture spread over two walls would draw one of them reflected in
the other's plane, and a face on a brush entity would put the mirror on a
model that moves.  Every condition is checked, and anything else fails loudly
rather than writing a map whose mirror is wrong in a way the sweep would only
see as a difference.  Two maps, one for each way R_Mirror flips the
projection (GL_Scalef (-1, 1, 1) for a wall, (1, -1, 1) for a floor):

  maps/demo1.bsp    rtex013 -- one face: the horseman panel in the wall at
                    x = -216, facing -x (y -2200 .. -2056, z 208 .. 360); its
                    reflection holds the room behind the viewer.
  maps/demo1f.bsp   rtex429 -- five faces on the plane z = 0: the cobbles of
                    the graveyard (x -1320 .. -680, y -1960 .. -1736); the
                    reflection holds the tombstones, the tree and the sky.

Nothing here is committed: the maps are copied out of the retail pak each
time.

Usage:  mk-mirror-testgame.py <basedir> [game]
reads the demo1 the engine loads from <basedir>/data1 -- in quakefs.c
FS_AddGameDirectory's order: a loose maps/demo1.bsp, then the .pk3s
(alphabetically last first), then pak9.pak down to pak0.pak -- and writes
both maps to <basedir>/<game>/maps/ (game defaults to "mirror").  The
engine's user directory, searched above the basedir, is not looked at:
run it with -userdir on an empty one, as tools/headless-drive.sh does.  The basedir has to hold
data1 as a directory or a link that resolves inside
tools/headless-drive.sh's sandbox (anything outside $HOME, as
tools/mk-alias-testgame.py says); then run the engine with -game <game>.
--texture NAME --out NAME writes one map instead, NAME renamed, as
maps/<out>.bsp.

  mk-mirror-testgame.py --self-test
builds a small synthetic BSP and checks the rename and every refusal on it
(no retail data needed).
"""
import argparse
import os
import re
import struct
import sys
import tempfile
import zipfile

MIRROR = b"window02_1"     # gl_rmisc.c R_NewMap: strncmp (name, "window02_1", 10)
BSPVERSION = 29
NUM_LUMPS = 15
LUMP_PLANES, LUMP_TEXTURES, LUMP_TEXINFO, LUMP_FACES, LUMP_MODELS = 1, 2, 6, 7, 14
FACE_SIZE = 20             # dface_t: planenum, side, firstedge, numedges, texinfo, styles[4], lightofs
TEXINFO_SIZE = 40          # texinfo_t: vecs[2][4], miptex, flags
MODEL_SIZE = 80            # Hexen II dmodel_t: mins, maxs, origin, headnode[8], visleafs, firstface, numfaces


class MirrorError(Exception):
    pass


def lumps(d):
    if len(d) < 4 + 8 * NUM_LUMPS:
        raise MirrorError("not a BSP: %d bytes" % len(d))
    ver = struct.unpack_from("<i", d, 0)[0]
    if ver != BSPVERSION:
        raise MirrorError("BSP version %d, want %d" % (ver, BSPVERSION))
    out = [struct.unpack_from("<ii", d, 4 + 8 * i) for i in range(NUM_LUMPS)]
    for i, (o, n) in enumerate(out):
        if o < 0 or n < 0 or o + n > len(d):
            raise MirrorError("lump %d (%d bytes at %d) runs past the file's %d" % (i, n, o, len(d)))
    return out


def miptex_names(d, L):
    """[(offset of the 16-byte name field, name)] in miptex order; None for a
    missing (-1) entry."""
    o, n = L[LUMP_TEXTURES]
    cnt = struct.unpack_from("<i", d, o)[0]
    names = []
    for t in struct.unpack_from("<%di" % cnt, d, o + 4):
        if t < 0:
            names.append(None)
            continue
        at = o + t
        if at + 16 > o + n:
            raise MirrorError("a miptex header runs past the texture lump")
        names.append((at, bytes(d[at:at + 16]).split(b"\0")[0]))
    return names


def rename(data, texture):
    """demo1 with `texture` renamed to window02_1, and a summary of what the
    mirror is.  Raises MirrorError if the rename would not make one mirror."""
    d = bytearray(data)
    L = lumps(d)
    names = miptex_names(d, L)
    tex = texture.encode("latin1")
    if not 0 < len(tex) < 16:
        raise MirrorError("texture name %r does not fit the 16-byte name field" % texture)
    already = [n.decode("latin1") for e in names if e for n in [e[1]] if n[:len(MIRROR)] == MIRROR]
    if already:
        raise MirrorError("the map already has a mirror texture (%s)" % ", ".join(already))
    hits = [i for i, e in enumerate(names) if e and e[1] == tex]
    if len(hits) != 1:
        raise MirrorError("%d miptex named %r, want exactly 1" % (len(hits), texture))
    mi = hits[0]

    o, n = L[LUMP_TEXINFO]
    texinfos = {i for i in range(n // TEXINFO_SIZE)
                if struct.unpack_from("<8fii", d, o + TEXINFO_SIZE * i)[8] == mi}
    o, n = L[LUMP_FACES]
    faces = [(i, struct.unpack_from("<HhiHH4Bi", d, o + FACE_SIZE * i)) for i in range(n // FACE_SIZE)]
    faces = [(i, f) for i, f in faces if f[4] in texinfos]
    if not faces:
        raise MirrorError("no face is drawn with %r" % texture)

    o, n = L[LUMP_MODELS]
    if n < MODEL_SIZE:
        raise MirrorError("the map has no world model")
    first, num = struct.unpack_from("<9f8iiii", d, o)[18:20]
    outside = [i for i, _f in faces if not first <= i < first + num]
    if outside:
        raise MirrorError("%r is on %d face(s) of a brush entity (face %s): a mirror there would move"
                          % (texture, len(outside), outside[0]))
    planes = sorted({f[0] for _i, f in faces})
    if len(planes) != 1:
        raise MirrorError("%r's %d faces are on %d planes (%s): R_MirrorChain reflects them all in the "
                          "first one's" % (texture, len(faces), len(planes), planes))
    po, pn = L[LUMP_PLANES]
    if planes[0] * 20 + 20 > pn:
        raise MirrorError("plane %d is past the plane lump" % planes[0])
    plane = struct.unpack_from("<4fi", d, po + 20 * planes[0])

    at = names[mi][0]
    d[at:at + 16] = MIRROR.ljust(16, b"\0")
    return bytes(d), {"faces": len(faces), "plane": plane[:4], "miptex": mi}


def pak_entries(path):
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) != 12 or head[:4] != b"PACK":
            raise MirrorError("%s: not a pak file" % path)
        o, n = struct.unpack_from("<ii", head, 4)
        f.seek(o)
        table = f.read(n)
    for i in range(len(table) // 64):
        name, fo, fl = struct.unpack_from("<56sii", table, 64 * i)
        yield name.split(b"\0")[0].decode("latin1").lower(), fo, fl


def read_from_paks(data1, want):
    """`want` as the engine finds it in data1 (quakefs.c FS_AddGameDirectory
    pushes pak0..pak9, then the .pk3s sorted by name, then the directory, each
    onto the head of the search path): a loose file first, then the
    alphabetically last .pk3 that has it, then the highest-numbered pak.
    Names match case-blind, as FS_OpenFile does: inside a pak or a pk3 the
    last table entry of a name (Hash_Add puts each at the head of its chain),
    and a loose file the exact path or else FS_ResolveCasePath's, the first
    directory entry per component that matches.  The archive names are
    found as fnmatch finds them, case and all.  Returns (bytes, where)."""
    loose = os.path.join(data1, *want.split("/"))
    if not os.path.isfile(loose):
        loose = data1
        for comp in want.split("/"):
            hit = [n for n in (os.listdir(loose) if os.path.isdir(loose) else []) if n.lower() == comp.lower()]
            loose = os.path.join(loose, hit[0]) if hit else None
            if loose is None:
                break
    if loose and os.path.isfile(loose):
        with open(loose, "rb") as f:
            return f.read(), loose
    pk3s = sorted((n for n in os.listdir(data1) if n.endswith(".pk3")), key=str.casefold, reverse=True)
    for name in pk3s:
        path = os.path.join(data1, name)
        try:
            with zipfile.ZipFile(path) as z:
                hits = [i for i in z.infolist() if i.filename.lower() == want]
                if hits:
                    return z.read(hits[-1]), "%s:%s" % (path, hits[-1].filename)
        except zipfile.BadZipFile as e:
            raise MirrorError("%s: %s" % (path, e))
    for i in range(9, -1, -1):
        path = os.path.join(data1, "pak%d.pak" % i)
        if not os.path.isfile(path):
            continue
        hits = [(fo, fl) for name, fo, fl in pak_entries(path) if name == want]
        if hits:
            fo, fl = hits[-1]
            with open(path, "rb") as f:
                f.seek(fo)
                data = f.read(fl)
            if len(data) != fl:
                raise MirrorError("%s: %s is cut short" % (path, want))
            return data, path
    raise MirrorError("nothing in %s holds %s" % (data1, want))


def write_pak(path, files):
    """A pak of `files` [(name, bytes)], for the self-test."""
    body, table, at = b"", b"", 12
    for name, data in files:
        table += name.encode("latin1").ljust(56, b"\0") + struct.pack("<ii", at + len(body), len(data))
        body += data
    with open(path, "wb") as f:
        f.write(b"PACK" + struct.pack("<ii", 12 + len(body), len(table)) + body + table)


def synthetic_bsp(textures, faces, world_faces):
    """A BSP with just the lumps rename() reads: `textures` names, `faces` as
    (plane, miptex) pairs (one texinfo per face), the first `world_faces` of
    them in the world model and the rest in a second model."""
    planes = b"".join(struct.pack("<4fi", 1.0, 0.0, 0.0, 64.0 * i, 0) for i in range(4))
    mip = b""
    offs = []
    base = 4 + 4 * len(textures)
    for t in textures:
        offs.append(base + len(mip))
        mip += t.encode("latin1").ljust(16, b"\0") + struct.pack("<6I", 16, 16, 0, 0, 0, 0)
    texlump = struct.pack("<i%di" % len(textures), len(textures), *offs) + mip
    texinfo = b"".join(struct.pack("<8fii", *([0.0] * 8), m, 0) for _p, m in faces)
    facelump = b"".join(struct.pack("<HhiHH4Bi", p, 0, 0, 0, i, 0, 0, 0, 0, -1)
                        for i, (p, _m) in enumerate(faces))
    models = struct.pack("<9f8iiii", *([0.0] * 9), *([0] * 8), 0, 0, world_faces)
    models += struct.pack("<9f8iiii", *([0.0] * 9), *([0] * 8), 0, world_faces, len(faces) - world_faces)
    lumpdata = {LUMP_PLANES: planes, LUMP_TEXTURES: texlump, LUMP_TEXINFO: texinfo,
                LUMP_FACES: facelump, LUMP_MODELS: models}
    out = b""
    table = []
    at = 4 + 8 * NUM_LUMPS
    for i in range(NUM_LUMPS):
        blob = lumpdata.get(i, b"")
        table.append((at + len(out), len(blob)))
        out += blob
    return struct.pack("<i", BSPVERSION) + b"".join(struct.pack("<ii", o, n) for o, n in table) + out


def self_test():
    failures = []

    def expect_error(what, data, tex, needle):
        try:
            rename(data, tex)
        except MirrorError as e:
            if needle not in str(e):
                failures.append("%s: refused for the wrong reason: %s" % (what, e))
            return
        failures.append("%s: renamed, should have refused" % what)

    # wall: 2 faces on plane 1; spread: plane 1 and plane 2; door: one face
    # in the brush entity; ok: one face
    tex = ["wall", "spread", "door", "other"]
    faces = [(1, 0), (1, 0), (1, 1), (2, 1), (3, 3), (0, 2)]
    bsp = synthetic_bsp(tex, faces, world_faces=5)
    out, info = rename(bsp, "wall")
    if len(out) != len(bsp):
        failures.append("the rename changed the file's size")
    diff = [i for i in range(len(bsp)) if bsp[i] != out[i]]
    names = [e[1] for e in miptex_names(out, lumps(out))]
    if names != [MIRROR, b"spread", b"door", b"other"]:
        failures.append("names after the rename: %s" % names)
    if not diff or max(diff) - min(diff) >= 16:
        failures.append("the rename touched bytes outside one 16-byte name field: %s" % diff[:4])
    if info["faces"] != 2 or info["plane"][3] != 64.0:
        failures.append("summary %s, want 2 faces on the plane at 64" % info)
    expect_error("case", bsp, "WALL", "0 miptex")
    expect_error("two planes", bsp, "spread", "planes")
    expect_error("brush entity", bsp, "door", "brush entity")
    expect_error("missing", bsp, "nosuch", "0 miptex")
    expect_error("unused", synthetic_bsp(["wall", "unused"], [(1, 0)], 1), "unused", "no face")
    expect_error("already mirrored", out, "other", "already has a mirror")
    expect_error("version", b"\x1e" + bsp[1:], "wall", "version 30")
    expect_error("too long", bsp, "a" * 16, "16-byte")
    # Which copy the engine loads: one per source, each a different byte.
    copies = {k: b"BSP-" + k.encode() for k in ("pak0", "pak2", "pak10", "a.pk3", "b.pk3", "loose")}
    want = "maps/demo1.bsp"

    def pick(make, expect, what):
        with tempfile.TemporaryDirectory() as t:
            make(t)
            try:
                got, _src = read_from_paks(t, want)
            except MirrorError as e:
                got = ("error", str(e))
            if got != expect:
                failures.append("%s: read %r, want %r" % (what, got, expect))

    def paks(t, *names):
        for n in names:
            # the copy under a name in another case, as pak tables often hold
            write_pak(os.path.join(t, n + ".pak"), [("maps/other.bsp", b"x"),
                                                    (want.upper() if n == "pak2" else want, copies[n])])

    def pk3(t, n):
        with zipfile.ZipFile(os.path.join(t, n), "w") as z:
            z.writestr("maps/DEMO1.bsp" if n == "b.pk3" else want, copies[n])

    def loose(t, name=("maps", "demo1.bsp")):
        os.makedirs(os.path.join(t, name[0]))
        with open(os.path.join(t, *name), "wb") as f:
            f.write(copies["loose"])

    def dup_pak(t):
        write_pak(os.path.join(t, "pak0.pak"), [(want, b"first"), (want, copies["pak0"])])

    pick(lambda t: paks(t, "pak0"), copies["pak0"], "one pak")
    pick(lambda t: paks(t, "pak0", "pak2"), copies["pak2"], "pak2 over pak0")
    pick(lambda t: paks(t, "pak0", "pak10"), copies["pak0"], "pak10 is past the engine's pak0-pak9")
    pick(lambda t: (paks(t, "pak2"), pk3(t, "a.pk3")), copies["a.pk3"], "a pk3 over the paks")
    pick(lambda t: (pk3(t, "b.pk3"), pk3(t, "a.pk3")), copies["b.pk3"], "the alphabetically last pk3")
    pick(lambda t: (paks(t, "pak2"), pk3(t, "b.pk3"), loose(t)), copies["loose"], "a loose file over everything")
    pick(lambda t: (paks(t, "pak2"), loose(t, ("Maps", "DEMO1.BSP"))), copies["loose"],
         "a loose file in another case (FS_ResolveCasePath)")
    pick(dup_pak, copies["pak0"], "the last of two entries in one pak")
    with tempfile.TemporaryDirectory() as t:
        paks(t, "pak10")
        try:
            read_from_paks(t, want)
            failures.append("only a pak10: read something, should have refused")
        except MirrorError as e:
            if "holds" not in str(e):
                failures.append("only a pak10: refused for the wrong reason: %s" % e)
    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("mk-mirror-testgame self-test: %d failure(s)" % len(failures))
        return 1
    print("mk-mirror-testgame self-test: OK, the rename, 8 refusals and the engine's pick of 8 demo1 copies")
    return 0


MIRRORS = (("rtex013", "demo1"), ("rtex429", "demo1f"))   # (texture, map written)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("basedir", nargs="?")
    ap.add_argument("game", nargs="?", default="mirror")
    ap.add_argument("--texture")
    ap.add_argument("--out")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.basedir:
        ap.error("a basedir is required")
    if bool(a.texture) != bool(a.out):
        ap.error("--texture and --out go together")
    jobs = [(a.texture, a.out)] if a.texture else MIRRORS
    try:
        data, src = read_from_paks(os.path.join(a.basedir, "data1"), "maps/demo1.bsp")
        outs = [(name, rename(data, tex)) for tex, name in jobs]
    except (MirrorError, OSError) as e:
        print("mk-mirror-testgame: %s" % e, file=sys.stderr)
        return 1
    dst = os.path.join(a.basedir, a.game, "maps")
    os.makedirs(dst, exist_ok=True)
    for (tex, _n), (name, (out, info)) in zip(jobs, outs):
        path = os.path.join(dst, name + ".bsp")
        with open(path, "wb") as f:
            f.write(out)
        print("%s: demo1 from %s, miptex %d %s -> %s, %d face(s) on the plane (%g %g %g) . p = %g"
              % (path, src, info["miptex"], tex, MIRROR.decode(), info["faces"],
                 *[v + 0.0 for v in info["plane"]]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
