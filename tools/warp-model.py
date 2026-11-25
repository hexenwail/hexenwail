#!/usr/bin/env python3
"""warp-model.py -- predict the underwater screen warp from the frame without
it, and count how much of the real warped frame disagrees (#340 P4c).

    warp-model.py <base.png> <warped.png> --warp W --time T [--scale S]
                  [--mask WxH+X+Y ...] [--pad N] [--tol N]
    warp-model.py --selftest

The warp is spp_frag.glsl's, with the uniforms PP_BlitWith3DEffects
(gl_postprocess.c) gives it:

    uv.x += sin(uv.y * 10.0 + time * 1.5) * 0.015 * waterwarp;
    uv.y += sin(uv.x * 10.0 + time * 2.0) * 0.015 * waterwarp;
    color = texture(scene, uv);          NEAREST, CLAMP_TO_EDGE

uv is GL's: (0, 0) at the bottom left of the screen, so a screenshot row r
is uv.y = (H - 1 - r + 0.5) / H.  <base.png> is the same view with
r_waterwarp 0 and everything else equal: the unwarped scene, as the warp pass
would have read it (at r_scale S, the half-size scene upscaled NEAREST; the
model reads the base at the pixel that shows the texel the warp asks for).

Prints one line, "<mismatched> <counted> <fraction>": pixels where any channel
of the warped frame differs from the prediction by more than --tol (default
2), over the pixels counted: a pixel is skipped when it or the base pixel it
samples lies in a --mask rectangle (2D drawn after the warp, which must
cover all of it), grown by --pad (default 0) on every side.  NEAREST at a rounding boundary can pick the
neighbouring texel on one rasterizer and not the other, so an honest frame
is not 0; the steps file records how far from 0.  ImageMagick's `magick`
decodes the PNGs.  Stdlib only otherwise.
"""

import argparse
import math
import subprocess
import sys


def read_ppm(path):
    """(w, h, bytes) of an image, RGB8, rows top-down."""
    data = subprocess.run(["magick", path, "-alpha", "off", "-depth", "8", "ppm:-"],
                          check=True, stdout=subprocess.PIPE).stdout
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise SystemExit("warp-model: %s: not 8-bit RGB" % path)
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def src_index(w, h, warp, t, scale):
    """For every output pixel (row-major, top-down), the base pixel the warp
    pass samples: a list of w*h flat pixel indices."""
    ws, hs = int(w * scale), int(h * scale)
    k = 0.015 * warp
    out = [0] * (w * h)
    # the base's pixel showing scene texel (tx, ty): the NEAREST upscale's
    # pre-image, exact for an integer ratio
    colx = [min(w - 1, int(tx * w / ws + 0.5)) for tx in range(ws)]
    rowy = [h - 1 - min(h - 1, int(ty * h / hs + 0.5)) for ty in range(hs)]
    for r in range(h):
        v = (h - 1 - r + 0.5) / h
        dx = math.sin(v * 10.0 + t * 1.5) * k
        base = r * w
        for x in range(w):
            ux = (x + 0.5) / w + dx
            vy = v + math.sin(ux * 10.0 + t * 2.0) * k
            tx = min(ws - 1, max(0, math.floor(ux * ws)))
            ty = min(hs - 1, max(0, math.floor(vy * hs)))
            out[base + x] = rowy[ty] * w + colx[tx]
    return out


def parse_rect(s):
    wh, x, y = s.split("+")
    rw, rh = wh.split("x")
    return int(rw), int(rh), int(x), int(y)


def count(base, warped, w, h, idx, masks, pad, tol):
    excluded = bytearray(w * h)
    for rw, rh, rx, ry in masks:
        for r in range(max(0, ry - pad), min(h, ry + rh + pad)):
            for x in range(max(0, rx - pad), min(w, rx + rw + pad)):
                excluded[r * w + x] = 1
    bad = counted = 0
    for i in range(w * h):
        if excluded[i] or excluded[idx[i]]:
            continue
        counted += 1
        j, s = i * 3, idx[i] * 3
        if (abs(warped[j] - base[s]) > tol or abs(warped[j + 1] - base[s + 1]) > tol
                or abs(warped[j + 2] - base[s + 2]) > tol):
            bad += 1
    return bad, counted


def selftest():
    """The model against itself on a synthetic image: its own prediction
    scores 0, and each broken warp the sweep's proxies stand for scores high."""
    w, h = 160, 120
    base = bytearray()
    for r in range(h):
        for x in range(w):
            base += bytes(((x * 7) & 255, (r * 11) & 255, ((x ^ r) * 5) & 255))

    def render(idx):
        out = bytearray(w * h * 3)
        for i, s in enumerate(idx):
            out[i * 3:i * 3 + 3] = base[s * 3:s * 3 + 3]
        return out

    good = src_index(w, h, 1.0, 10.0, 1.0)
    cases = {
        "honest": (render(good), 0.0, 0.0),
        "no warp": (bytes(base), 0.5, 1.0),
        "warp twice": (render([good[i] for i in good]), 0.5, 1.0),
        "other time": (render(src_index(w, h, 1.0, 11.0, 1.0)), 0.5, 1.0),
        "warp 2": (render(src_index(w, h, 2.0, 10.0, 1.0)), 0.5, 1.0),
    }
    flipped = []
    for r in range(h):
        flipped += good[(h - 1 - r) * w:(h - r) * w]
    flipped = [(h - 1 - s // w) * w + s % w for s in flipped]
    cases["uv y-flipped"] = (render(flipped), 0.5, 1.0)
    ok = True
    # The model's own conventions (GL's bottom-up uv.y, the second term
    # reading the displaced uv.x, NEAREST's floor, the upscale's pre-image)
    # against values worked by hand from spp_frag.glsl at 800x600, e.g. at
    # warp 1, time 10, row 100 x 400: uv = (0.500625, 0.8325), uv.x +=
    # sin(23.325) * 0.015 -> 0.48605 (texel 388), uv.y += sin(24.8605) *
    # 0.015 -> 0.82848 (texel 497 from the bottom, row 102).
    golden = {
        (1.0, 10.0, 1.0): [(0, 0, 0, 0), (100, 400, 102, 388), (300, 799, 309, 799), (599, 123, 596, 131)],
        (2.0, 3.7, 1.0): [(0, 0, 0, 4), (100, 400, 98, 423), (300, 799, 318, 777), (599, 123, 588, 107)],
        (1.0, 10.0, 0.5): [(0, 0, 1, 0), (100, 400, 103, 388), (300, 799, 309, 798), (599, 123, 597, 130)],
    }
    for (gw, gt, gs), pts in golden.items():
        idx = src_index(800, 600, gw, gt, gs)
        got = [(r, x) + divmod(idx[r * 800 + x], 800) for r, x, _, _ in pts]
        res = got == pts
        ok &= res
        print("selftest golden %-9s %s" % ("%g/%g/%g" % (gw, gt, gs), "ok" if res else "FAIL %r" % got))
    for name, (img, lo, hi) in cases.items():
        bad, n = count(base, img, w, h, good, [], 0, 2)
        f = bad / n
        res = lo <= f <= hi
        ok &= res
        print("selftest %-14s %.4f %s" % (name, f, "ok" if res else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("base", nargs="?")
    ap.add_argument("warped", nargs="?")
    ap.add_argument("--warp", type=float, default=1.0)
    ap.add_argument("--time", type=float, default=0.0)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--mask", action="append", default=[])
    ap.add_argument("--pad", type=int, default=0)
    ap.add_argument("--tol", type=int, default=2)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.base or not a.warped:
        ap.error("need <base.png> <warped.png>")
    w, h, base = read_ppm(a.base)
    w2, h2, warped = read_ppm(a.warped)
    if (w, h) != (w2, h2):
        raise SystemExit("warp-model: %dx%d against %dx%d" % (w, h, w2, h2))
    idx = src_index(w, h, a.warp, a.time, a.scale)
    bad, n = count(base, warped, w, h, idx, [parse_rect(m) for m in a.mask], a.pad, a.tol)
    print("%d %d %.5f" % (bad, n, bad / n if n else 1.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
