#!/usr/bin/env python3
"""Draw every sv_muzzlespawn muzzle point on its view model (GitHub #567).

Reads each `MuzzleFix (missile, self, 'F R U')  // <model>.mdl <frame>` call in
gamecode/hc/h2 and gamecode/hc/portals, loads that view model from the retail
paks, projects the named frame the way the engine draws a view model (camera
at the model origin, +X forward, fov 90 on 4:3) and marks the muzzle point in
red.  Writes one SVG contact sheet.  Standard library only; open the SVG in a
browser, or `rsvg-convert` it.

Use it whenever a muzzle value changes.  The first cut of the spellbook values
came from the model's forward-most vertex, which on spllbook.mdl is the corner
of the book, not the casting hand; a glance at this sheet shows that at once.

  python3 tools/muzzle_sheet.py ~/hexen2 /tmp/muzzles.svg

The basedir must hold data1/pak0.pak and data1/pak1.pak (retail) and, for the
Portal of Praevus sites, portals/pak3.pak.  Nothing from the paks is written
out except the projected outlines in the SVG.
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
CALL = re.compile(r"MuzzleFix\s*\(\s*\w+\s*,\s*self\s*,\s*'(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)'"
                  r"[^;]*;\s*//\s*(\w+)\.mdl\s+(\w+)")
CELL_W, CELL_H, FOCAL, COLS = 400, 300, 200.0, 4


def read_paks(basedir):
    """{lowercased path inside the pak: bytes}, later paks overriding earlier."""
    files = {}
    for rel in ("data1/pak0.pak", "data1/pak1.pak", "portals/pak3.pak"):
        p = Path(basedir) / rel
        if not p.exists():
            continue
        d = p.read_bytes()
        ident, off, ln = struct.unpack("<4sii", d[:12])
        if ident != b"PACK":
            continue
        for i in range(ln // 64):
            name, o, l = struct.unpack("<56sii", d[off + 64 * i:off + 64 * (i + 1)])
            files[name.split(b"\0")[0].decode("latin-1").lower()] = d[o:o + l]
    return files


def load_mdl(d):
    """(triangles, {frame name: [(x, y, z)]}) for an IDPO or RAPO model."""
    ident = d[:4]
    scale = struct.unpack("<3f", d[8:20])
    trans = struct.unpack("<3f", d[20:32])
    p = 48
    nskins, sw, sh, nverts, ntris, nframes, _sync, _flags, _size = struct.unpack("<9i", d[p:p + 36])
    p += 36
    nst = nverts
    if ident == b"RAPO":
        nst = struct.unpack("<i", d[p:p + 4])[0]
        p += 4
    for _ in range(nskins):
        kind = struct.unpack("<i", d[p:p + 4])[0]
        p += 4
        if kind == 0:
            p += sw * sh
        else:
            n = struct.unpack("<i", d[p:p + 4])[0]
            p += 4 + 4 * n + n * sw * sh
    p += nst * 12
    tris = []
    for _ in range(ntris):
        if ident == b"RAPO":
            _f, a, b, c = struct.unpack("<i3H", d[p:p + 10])
        else:
            _f, a, b, c = struct.unpack("<4i", d[p:p + 16])
        tris.append((a, b, c))
        p += 16
    frames = {}
    order = []
    for _ in range(nframes):
        kind = struct.unpack("<i", d[p:p + 4])[0]
        p += 4
        if kind != 0:
            break  # frame groups: no view model uses one
        p += 8
        name = d[p:p + 16].split(b"\0")[0].decode("latin-1")
        p += 16
        order.append(name.lower())
        frames[name.lower()] = [(d[p + 4 * i] * scale[0] + trans[0],
                                 d[p + 4 * i + 1] * scale[1] + trans[1],
                                 d[p + 4 * i + 2] * scale[2] + trans[2]) for i in range(nverts)]
        p += 4 * nverts
    # By position too: a .hc's $frame names need not match the names stored
    # in the model (icestaff.mdl stores fire1a..fire5c as "1".."15").
    return tris, frames, order


def project(v):
    x, y, z = v
    if x <= 0.5:
        return None
    return (CELL_W / 2 - y / x * FOCAL, CELL_H / 2 - z / x * FOCAL, x)


def sites():
    out = []
    for tree in ("h2", "portals"):
        for f in sorted((ROOT / "gamecode/hc" / tree).glob("*.hc")):
            text = f.read_text(encoding="latin-1")
            declared = [w.lower() for line in re.findall(r"^\$frame\b(.*)$", text, re.M)
                        for w in line.split("//")[0].split()]
            for n, line in enumerate(text.splitlines(), 1):
                m = CALL.search(line)
                if m:
                    fwd, right, up, model, frame = m.groups()
                    index = declared.index(frame.lower()) if frame.lower() in declared else None
                    out.append((f"{tree}/{f.name}:{n}", (float(fwd), float(right), float(up)),
                                model.lower(), frame.lower(), index))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    paks = read_paks(sys.argv[1])
    if not paks:
        sys.exit(f"no retail paks under {sys.argv[1]}")
    cache, cells = {}, []
    for where, (fwd, right, up), model, frame, index in sites():
        key = f"models/{model}.mdl"
        if key not in cache:
            cache[key] = load_mdl(paks[key]) if key in paks else None
        body = []
        mdl = cache[key]
        verts = mdl[1].get(frame) if mdl else None
        if verts is None and mdl and index is not None and index < len(mdl[2]):
            verts = mdl[1][mdl[2][index]]
        if verts is None:
            body.append(f'<text x="8" y="40" fill="#f88" font-size="13">no {model}.mdl frame {frame}</text>')
        else:
            pts = [project(v) for v in verts]
            tris = sorted(((pts[a][2] + pts[b][2] + pts[c][2]) / 3, a, b, c)
                          for a, b, c in mdl[0] if None not in (pts[a], pts[b], pts[c]))
            for depth, a, b, c in reversed(tris):
                g = int(max(60, min(230, 255 - depth * 4)))
                body.append(f'<polygon points="{pts[a][0]:.1f},{pts[a][1]:.1f} {pts[b][0]:.1f},{pts[b][1]:.1f} '
                            f'{pts[c][0]:.1f},{pts[c][1]:.1f}" fill="rgb({g},{g},{g})"/>')
            mz = project((fwd, -right, up))  # muzzle _y is right; the model's +y is left
            if mz:
                body.append(f'<circle cx="{mz[0]:.1f}" cy="{mz[1]:.1f}" r="5" fill="#ff2020"/>')
            else:
                body.append('<text x="8" y="40" fill="#f88" font-size="13">muzzle behind the camera</text>')
        body.append(f'<text x="8" y="16" fill="#fff" font-size="12">{where}</text>'
                    f'<text x="8" y="32" fill="#ccc" font-size="12">{model} {frame}  '
                    f"'{fwd:g} {right:g} {up:g}'</text>")
        cells.append(body)
    rows = (len(cells) + COLS - 1) // COLS
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{CELL_W * COLS}" height="{CELL_H * rows}">',
           f'<defs><clipPath id="cell"><rect width="{CELL_W}" height="{CELL_H}"/></clipPath></defs>',
           f'<rect width="100%" height="100%" fill="#203040"/>']
    for i, body in enumerate(cells):
        x, y = CELL_W * (i % COLS), CELL_H * (i // COLS)
        # A view model reaches past the near edge of the frame; clip each
        # cell or its outline paints over the neighbours.
        svg.append(f'<g transform="translate({x},{y})"><g clip-path="url(#cell)">{"".join(body)}</g>'
                   f'<rect width="{CELL_W}" height="{CELL_H}" fill="none" stroke="#405060"/></g>')
    svg.append("</svg>")
    Path(sys.argv[2]).write_text("\n".join(svg))
    print(f"{len(cells)} muzzle sites -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
