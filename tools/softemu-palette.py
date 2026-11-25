#!/usr/bin/env python3
"""The game palette, and which pixels of a frame are not in it (#340 P4d).

r_softemu snaps every pixel of the final pass to a palette entry: the LUTs
gl_postprocess.c builds hold palette *indices*, which PP_UploadLUT resolves
through d_8to24table -- the 256 colours of gfx/palette.lmp, read as is
(VID_InitPalette).  Both LUT builders search entries 0-254 only: 255 is the
transparency key, and the colormap LUT (r_softemu 3) never resolves to it in
the retail colormap.  So every pixel of an r_softemu frame is one of entries
0-254, on any backend; a pixel that is not means the final pass did not snap
it (softemu off in the shader, a LUT never uploaded and sampled as a
placeholder, a filter between the LUT and the frame).

What it cannot see: a LUT that emits entry 255 where 255's colour is also
an entry below it.  Retail Hexen II's is: 255 is fcfcfc, which is entry 31
too (and 0/1 and 127/236 are pairs as well, 253 distinct colours), so on
retail data an emitted 255 passes.  `dump` says so on stderr when it is.

  softemu-palette.py dump <game dir>
      Prints gfx/palette.lmp as 256 lines of RRGGBB, index order.  <game dir>
      is a data1-style directory, searched in the engine's own order
      (FS_AddGameDirectory): a loose gfx/palette.lmp, then the .pk3s with the
      alphabetically last first, then pak9.pak down to pak0.pak (the engine
      loads no others, and opens those names as written).  The userdir's
      data1, which the engine searches above the basedir's, is not read:
      the sweeps run on a fresh -userdir.

  softemu-palette.py outside <palette file> <width> [<x>,<y>,<w>,<h>]
      Reads packed 8-bit RGB rows of <width> pixels on stdin (ImageMagick's
      `rgb:-`) and prints the number of pixels in the region (default: all of
      it) whose colour is not among entries 0-254 of <palette file> (dump's
      output), then up to five of them as x,y RRGGBB.  Exit status 0 if that
      number is 0, else 1.

  softemu-palette.py reports <qconsole.log> <SECTION>...
      Checks the -gpu run's REPORT-<SECTION>-BEGIN / -END sections: each
      named one must be there, opened and closed, with both gpu_unrouted
      counts ("N calls into", "N routed calls") inside it and both 0.  Prints
      one line per problem; exit status 0 if there are none.

tools/softemu-diff.sh drives all three; tools/test_softemu_palette.py is its gate.
"""
from pathlib import Path
import re
import struct
import sys
import zipfile

LMP = "gfx/palette.lmp"
# The entries the LUTs may emit: 255 is skipped by both builders.
EMITTED = 255


def pak_entry(pak, want):
    """The bytes of `want` in a PACK file, or None."""
    with open(pak, "rb") as f:
        head = f.read(12)
        if len(head) != 12 or head[:4] != b"PACK":
            return None
        dirofs, dirlen = struct.unpack("<ii", head[4:])
        f.seek(dirofs)
        d = f.read(dirlen)
        for i in range(0, len(d) - 63, 64):
            name = d[i:i + 56].split(b"\0", 1)[0].decode("latin-1").lower()
            if name == want:
                pos, size = struct.unpack("<ii", d[i + 56:i + 64])
                f.seek(pos)
                return f.read(size)
    return None


def palette(game):
    game = Path(game)
    loose = game / LMP
    if loose.is_file():
        data = loose.read_bytes()
    else:
        data = None
        for z in sorted((p for p in game.iterdir() if p.name.lower().endswith(".pk3")),
                        key=lambda p: p.name.lower(), reverse=True):
            with zipfile.ZipFile(z) as zf:
                names = [n for n in zf.namelist() if n.lower() == LMP]
                if names:
                    data = zf.read(names[0])
                    break
        paks = [] if data is not None else sorted((p for p in game.iterdir() if re.fullmatch(r"pak\d\.pak", p.name)),
                      key=lambda p: int(re.sub(r"\D", "", p.name)), reverse=True)
        for p in paks:
            data = pak_entry(p, LMP)
            if data is not None:
                break
        if data is None:
            raise SystemExit("softemu-palette: no %s in %s or its paks" % (LMP, game))
    if len(data) < 768:
        raise SystemExit("softemu-palette: %s is %d bytes, want 768" % (LMP, len(data)))
    return [tuple(data[i * 3:i * 3 + 3]) for i in range(256)]


def read_palette(path):
    cols = []
    for line in Path(path).read_text().split():
        if not re.fullmatch(r"[0-9a-fA-F]{6}", line):
            raise SystemExit("softemu-palette: %s: %r is not RRGGBB" % (path, line))
        cols.append(bytes.fromhex(line))
    if len(cols) != 256:
        raise SystemExit("softemu-palette: %s holds %d colours, want 256" % (path, len(cols)))
    return cols


def outside(pal, width, data, region=None):
    """(count, examples) of the region's pixels not among entries 0-254."""
    if width <= 0 or len(data) % (width * 3):
        raise SystemExit("softemu-palette: %d bytes is not whole rows of %d RGB pixels" % (len(data), width))
    height = len(data) // (width * 3)
    x0, y0, w, h = region or (0, 0, width, height)
    if x0 < 0 or y0 < 0 or w <= 0 or h <= 0 or x0 + w > width or y0 + h > height:
        raise SystemExit("softemu-palette: region %d,%d,%d,%d outside the %dx%d frame" % (x0, y0, w, h, width, height))
    allowed = set(pal[:EMITTED])
    count, examples = 0, []
    for y in range(y0, y0 + h):
        row = data[(y * width + x0) * 3:(y * width + x0 + w) * 3]
        for x in range(w):
            c = row[x * 3:x * 3 + 3]
            if c not in allowed:
                count += 1
                if len(examples) < 5:
                    examples.append("%d,%d %s" % (x0 + x, y, c.hex()))
    return count, examples


def reports(log, sections):
    """Problems with the named REPORT sections of a qconsole.log's text."""
    found, problems, sec, counts = {}, [], None, {}
    for line in log.splitlines():
        m = re.match(r"REPORT-([A-Z0-9]+)-(BEGIN|END)\b", line)
        if m:
            name, edge = m.groups()
            if edge == "BEGIN":
                if sec is not None:
                    problems.append("REPORT-%s: never closed" % sec)
                sec, counts = name, {}
            elif sec != name:
                problems.append("REPORT-%s: END without its BEGIN" % name)
                sec = None
            else:
                found[name] = counts
                sec = None
            continue
        if sec is None:
            continue
        m = re.match(r"gpu_unrouted: (\d+) (calls into|routed calls)", line)
        if m:
            counts[m.group(2)] = int(m.group(1))
    if sec is not None:
        problems.append("REPORT-%s: never closed" % sec)
    for name in sections:
        if name not in found:
            problems.append("REPORT-%s: missing" % name)
            continue
        for kind in ("calls into", "routed calls"):
            n = found[name].get(kind)
            if n is None:
                problems.append("REPORT-%s: no gpu_unrouted '%s' count" % (name, kind))
            elif n:
                problems.append("REPORT-%s: gpu_unrouted %d %s" % (name, n, kind))
    return problems


def main(argv):
    if len(argv) == 2 and argv[0] == "dump":
        pal = palette(argv[1])
        if pal[255] in pal[:EMITTED]:
            sys.stderr.write("softemu-palette: entry 255 (%02x%02x%02x) is also an entry below it: "
                             "an emitted 255 is invisible to `outside`\n" % pal[255])
        for c in pal:
            print("%02x%02x%02x" % c)
        return 0
    if len(argv) >= 3 and argv[0] == "reports":
        problems = reports(Path(argv[1]).read_text(errors="replace"), argv[2:])
        for p in problems:
            print(p)
        return 1 if problems else 0
    if len(argv) in (3, 4) and argv[0] == "outside":
        region = None
        if len(argv) == 4:
            region = tuple(int(v) for v in argv[3].split(","))
            if len(region) != 4:
                raise SystemExit("softemu-palette: region is x,y,w,h")
        count, examples = outside(read_palette(argv[1]), int(argv[2]), sys.stdin.buffer.read(), region)
        print(count, *examples)
        return 0 if count == 0 else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
