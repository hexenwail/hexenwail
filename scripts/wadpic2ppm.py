#!/usr/bin/env python3
"""Extract one picture from a Hexen II install and write it as a binary PPM.

Used by scripts/hw-hud-check.sh to get the reference HUD icons (the net
"disconnected" icon lives in gfx.wad, the artifact icons are loose gfx/*.lmp
files) straight from the player's own paks, so no game art is committed.

    wadpic2ppm.py BASEDIR NAME OUT.ppm

NAME is either a gfx.wad lump name ("net") or a pak path ("gfx/durshd1.lmp").
Palette index 255 is transparent in Hexen II pics; it is written as magenta
(255,0,255) so the caller can mask it.  Exit 1 if the picture is not found.
"""

import glob
import os
import struct
import sys


def pak_files(basedir):
    """Yield (name, bytes) for every file in data1 paks, later paks winning."""
    files = {}
    for pak in sorted(glob.glob(os.path.join(basedir, "data1", "pak*.pak"))):
        with open(pak, "rb") as f:
            magic, ofs, length = struct.unpack("<4sii", f.read(12))
            if magic != b"PACK":
                continue
            f.seek(ofs)
            entries = [struct.unpack("<56sii", f.read(64))
                       for _ in range(length // 64)]
            for raw, fofs, flen in entries:
                f.seek(fofs)
                files[raw.split(b"\0")[0].decode("latin-1").lower()] = f.read(flen)
    return files


def wad_lump(wad, name):
    magic, count, ofs = struct.unpack("<4sii", wad[:12])
    if magic != b"WAD2":
        return None
    for i in range(count):
        e = wad[ofs + 32 * i: ofs + 32 * (i + 1)]
        fpos, _dsize, size, _typ, _cmp, _pad = struct.unpack("<iiibbh", e[:16])
        lname = e[16:32].split(b"\0")[0].decode("latin-1").lower()
        if lname == name:
            return wad[fpos: fpos + size]
    return None


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    basedir, name, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    files = pak_files(basedir)
    pal = files.get("gfx/palette.lmp")
    if pal is None:
        sys.stderr.write("wadpic2ppm: no gfx/palette.lmp under %s\n" % basedir)
        return 1
    if "/" in name:
        pic = files.get(name)
    else:
        wad = files.get("gfx.wad")
        pic = wad_lump(wad, name) if wad else None
    if pic is None:
        sys.stderr.write("wadpic2ppm: %s not found\n" % name)
        return 1
    w, h = struct.unpack("<ii", pic[:8])
    px = pic[8: 8 + w * h]
    rgb = bytearray()
    for c in px:
        rgb += b"\xff\x00\xff" if c == 255 else pal[c * 3: c * 3 + 3]
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(rgb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
