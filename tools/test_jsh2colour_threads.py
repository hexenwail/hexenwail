#!/usr/bin/env python3
"""jsh2colour writes the same .lit with any number of threads (#515).

Two races made -threads 8 write a different .lit from one run to the next,
and never reliably the -threads 1 one:

  - TestLightFace runs on every worker thread and adds the colour of each
    face a light reaches into that light's entities[].lightcolor.  The add
    was an unlocked read-modify-write, so one face's colour could be lost,
    which changes the light's normalised colour.
  - LightFaceLIT writes each face's lightmap at the face's old lightofs, and
    on retail maps a face's lightmap can run into the next face's bytes.
    With one thread the higher face is written last and wins; with more,
    whichever thread finished last won.

This test builds a map for both: a room of many small cubes, in three
colours, under many lights, so that every thread adds into the same few
lights all the time; then a copy of it whose odd faces share their even
neighbour's lightofs, so that pairs of faces lit at the same moment write the
same bytes.  The WAD and the .map are generated here, compiled with the qbsp
and light that sit next to the jsh2colour under test, and coloured through an
-external definition file, so no game data is needed.  Each multi-threaded
.lit must be byte-identical to the -threads 1 one.

With retail Hexen II data present (--retail, default ~/hexen2), it repeats the
check on keep2.bsp from portals/pak3.pak, the map #515 was reported on, and on
demo1.bsp from data1/pak0.pak, which has overlapping faces.  With no data the
retail part prints SKIP and the test still runs on the fixture.

  test_jsh2colour_threads.py UTILS_BIN [--runs N] [--threads N] [--retail DIR]

UTILS_BIN is the directory holding qbsp, light and jsh2colour
(e.g. "$(nix build .#utils --no-link --print-out-paths)/bin").
"""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

# texture -> colour given to jsh2colour through -external.  Three unequal
# colours, so a lost add shifts the light's colour ratio and shows in the .lit.
COLOURS = {"tred": (255, 40, 10), "tgreen": (20, 255, 30), "tblue": (10, 60, 255)}
WALL = "twhite"


def miptex_lump(name, size=16):
    """A WAD2 miptex: header, then the four mip levels of a flat texture."""
    header = struct.pack("<16sII", name.encode(), size, size)
    offsets, pixels, ofs = [], b"", 40
    for level in range(4):
        n = (size >> level) ** 2
        offsets.append(ofs)
        pixels += bytes([15 + 16 * (len(name) % 8)]) * n
        ofs += n
    return header + struct.pack("<4I", *offsets) + pixels


def write_wad(path, names):
    lumps = [miptex_lump(n) for n in names]
    data, info, ofs = b"", b"", 12
    for name, lump in zip(names, lumps):
        info += struct.pack("<iiibbbb16s", ofs, len(lump), len(lump),
                            ord("D"), 0, 0, 0, name.encode())
        data += lump
        ofs += len(lump)
    path.write_bytes(struct.pack("<4sii", b"WAD2", len(names), ofs) + data + info)


def brush(mins, maxs, tex):
    """An axial box as six Quake .map planes."""
    (x0, y0, z0), (x1, y1, z1) = mins, maxs
    faces = [
        ((x0, y0, z0), (x0, y1, z0), (x0, y0, z1)),  # -x
        ((x1, y0, z0), (x1, y0, z1), (x1, y1, z0)),  # +x
        ((x0, y0, z0), (x0, y0, z1), (x1, y0, z0)),  # -y
        ((x0, y1, z0), (x1, y1, z0), (x0, y1, z1)),  # +y
        ((x0, y0, z0), (x1, y0, z0), (x0, y1, z0)),  # -z
        ((x0, y0, z1), (x0, y1, z1), (x1, y0, z1)),  # +z
    ]
    lines = ["{"]
    for a, b, c in faces:
        pts = " ".join("( %d %d %d )" % p for p in (a, b, c))
        lines.append("%s %s 0 0 0 1 1 0" % (pts, tex))
    lines.append("}")
    return "\n".join(lines)


def write_map(path, wad):
    H, Z = 640, 384           # half width of the room, its height
    T = 16                    # wall thickness
    out = ['{', '"classname" "worldspawn"', '"wad" "%s"' % wad]
    walls = [
        ((-H - T, -H - T, -T), (H + T, H + T, 0)),
        ((-H - T, -H - T, Z), (H + T, H + T, Z + T)),
        ((-H - T, -H - T, 0), (-H, H + T, Z)),
        ((H, -H - T, 0), (H + T, H + T, Z)),
        ((-H, -H - T, 0), (H, -H, Z)),
        ((-H, H, 0), (H, H + T, Z)),
    ]
    out += [brush(a, b, WALL) for a, b in walls]
    names = sorted(COLOURS)
    k = 0
    for z in (64, 192):
        for x in range(-H + 32, H - 32, 48):
            for y in range(-H + 32, H - 32, 48):
                out.append(brush((x, y, z), (x + 16, y + 16, z + 16), names[k % 3]))
                k += 1
    out.append("}")
    for x in range(-H + 80, H, 160):
        for y in range(-H + 80, H, 160):
            out += ['{', '"classname" "light"', '"light" "300"',
                    '"origin" "%d %d %d"' % (x, y, Z - 32), '}']
    out += ['{', '"classname" "info_player_start"', '"origin" "0 0 32"', '}']
    path.write_text("\n".join(out) + "\n")


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.exit("FAIL: %s exited %d\n%s" % (" ".join(map(str, cmd)),
                                              r.returncode, r.stdout[-2000:]))
    return r.stdout


def overlap_faces(src, dst):
    """Copy a BSP with every odd face sharing its even neighbour's lightofs.

    Both faces of a pair then write the same bytes of the .lit, as the faces
    of a retail map do wherever jsh2colour's extents outgrow the space light
    gave them (demo1 has 199 such pairs).  -threads 1 lets the higher face
    win; the threaded run must too."""
    data = bytearray(src.read_bytes())
    version, ofs, length = struct.unpack_from("<iii", data, 0)
    if version != 29:
        sys.exit("FAIL: %s is BSP version %d, expected 29" % (src, version))
    ofs, length = struct.unpack_from("<ii", data, 4 + 8 * 7)  # LUMP_FACES
    size = 20                                                # dface_t
    pairs = 0
    for i in range(1, length // size, 2):
        prev = struct.unpack_from("<i", data, ofs + (i - 1) * size + 16)[0]
        mine = struct.unpack_from("<i", data, ofs + i * size + 16)[0]
        if prev != -1 and mine != -1:
            struct.pack_into("<i", data, ofs + i * size + 16, prev)
            pairs += 1
    if pairs < 100:
        sys.exit("FAIL: only %d face pairs to overlap in %s" % (pairs, src))
    dst.write_bytes(data)


def lit_digest(jsh, bsp, threads, extra):
    lit = bsp.with_suffix(".lit")
    if lit.exists():
        lit.unlink()  # MakeLITFile exits 0 when it cannot write
    out = run([jsh, "-threads", str(threads)] + extra + [bsp.name], bsp.parent)
    if "Skipping texture lighting" in out:
        sys.exit("FAIL: %s skipped the texture pass, so the threads were "
                 "never tested on it" % bsp.name)
    if not lit.is_file():
        sys.exit("FAIL: %s wrote no %s\n%s" % (jsh, lit.name, out[-2000:]))
    return hashlib.sha256(lit.read_bytes()).hexdigest()


def check(label, jsh, bsp, threads, runs, extra=()):
    extra = list(extra)
    want = lit_digest(jsh, bsp, 1, extra)
    bad = 0
    for i in range(runs):
        got = lit_digest(jsh, bsp, threads, extra)
        if got != want:
            bad += 1
            print("  %s run %d -threads %d: %s != -threads 1 %s"
                  % (label, i + 1, threads, got[:16], want[:16]))
    if bad:
        print("FAIL: %s: %d of %d -threads %d runs differ from -threads 1"
              % (label, bad, runs, threads))
        return False
    print("ok: %s: %d -threads %d runs match -threads 1 (%s)"
          % (label, runs, threads, want[:16]))
    return True


def pak_member(paks, want):
    for pak in paks:
        with open(pak, "rb") as f:
            ident, ofs, size = struct.unpack("<4sii", f.read(12))
            if ident != b"PACK":
                continue
            f.seek(ofs)
            for _ in range(size // 64):
                name, pos, length = struct.unpack("<56sii", f.read(64))
                if name.split(b"\0")[0].decode("latin-1").lower() == want:
                    f.seek(pos)
                    return f.read(length)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bindir", type=Path)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--retail", type=Path, default=Path.home() / "hexen2")
    ap.add_argument("--retail-runs", type=int, default=3)
    a = ap.parse_args()

    tools = {t: a.bindir / t for t in ("qbsp", "light", "jsh2colour")}
    for t, p in tools.items():
        if not p.is_file():
            sys.exit("FAIL: no %s at %s" % (t, p))

    ok = True
    with tempfile.TemporaryDirectory(prefix="jsh2colour-threads-") as tmp:
        tmp = Path(tmp)
        write_wad(tmp / "fixture.wad", sorted(COLOURS) + [WALL])
        write_map(tmp / "fixture.map", "fixture.wad")
        (tmp / "fixture.def").write_text(
            "".join("%s %d %d %d\n" % ((n,) + c) for n, c in COLOURS.items()))
        run([tools["qbsp"], "fixture.map"], tmp)
        # light is threaded too; one thread keeps the fixture the same.
        run([tools["light"], "-threads", "1", "fixture.bsp"], tmp)
        ext = ["-external", "fixture.def"]
        ok &= check("fixture", tools["jsh2colour"], tmp / "fixture.bsp",
                    a.threads, a.runs, ext)
        overlap_faces(tmp / "fixture.bsp", tmp / "overlap.bsp")
        ok &= check("overlap", tools["jsh2colour"], tmp / "overlap.bsp",
                    a.threads, a.runs, ext)

        # keep2 is the map #515 was reported on; demo1 is a map whose faces
        # overlap in the .lit (see overlap_faces).
        retail = (("keep2", a.retail / "portals" / "pak3.pak"),
                  ("demo1", a.retail / "data1" / "pak0.pak"))
        for name, pak in retail:
            data = pak_member([pak], "maps/%s.bsp" % name) if pak.is_file() else None
            if data is None:
                print("SKIP: %s: no maps/%s.bsp in %s (retail Hexen II)"
                      % (name, name, pak))
                continue
            (tmp / ("%s.bsp" % name)).write_bytes(data)
            ok &= check(name, tools["jsh2colour"], tmp / ("%s.bsp" % name),
                        a.threads, a.retail_runs)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
