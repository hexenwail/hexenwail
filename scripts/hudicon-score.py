#!/usr/bin/env python3
"""How strongly a HUD icon appears in one region of a screenshot.

    hudicon-score.py SHOT.png REF.ppm X Y W H

Crops W x H at (X,Y) from SHOT (via ImageMagick's `magick`, the only
dependency), scales REF -- a pic written by wadpic2ppm.py -- to the same size
with nearest-neighbour sampling, and prints the Pearson correlation of the
two in grayscale over the icon's OPAQUE pixels only: near 1 when the icon is
drawn there, well below that when the region shows the world.

Masking matters: Hexen II pics use palette 255 as transparent (wadpic2ppm
writes it as magenta), and the rotating artifact icons are mostly
transparent, so an unmasked comparison mostly scores the world behind them.
Correlation, rather than a pixel distance, shrugs off the brightness and
contrast changes gamma, overbright and 2D canvas scaling apply.

Used by scripts/hw-hud-check.sh.  Exit 2 on bad usage.
"""

import math
import subprocess
import sys


def read_ppm(data):
    """Parse a binary P6 PPM (no comments) into (w, h, bytes)."""
    parts = []
    pos = 0
    while len(parts) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if pos >= len(data):
            raise ValueError("truncated PPM header")
        parts.append(data[start:pos])
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise ValueError("not an 8-bit P6 PPM")
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1: pos + 1 + w * h * 3]


def gray(rgb, i):
    return 0.299 * rgb[i] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2]


def score(shot, ref_path, x, y, w, h):
    crop = subprocess.run(
        ["magick", shot, "-crop", "%dx%d+%d+%d" % (w, h, x, y), "+repage",
         "-depth", "8", "ppm:-"],
        check=True, stdout=subprocess.PIPE).stdout
    cw, ch, cpx = read_ppm(crop)
    if (cw, ch) != (w, h):
        raise ValueError("crop is %dx%d, region runs off the image" % (cw, ch))
    with open(ref_path, "rb") as f:
        rw, rh, rpx = read_ppm(f.read())

    a, b = [], []
    for j in range(h):
        sy = j * rh // h
        for i in range(w):
            ri = (sy * rw + i * rw // w) * 3
            if rpx[ri: ri + 3] == b"\xff\x00\xff":
                continue            # transparent in the pic
            a.append(gray(cpx, (j * w + i) * 3))
            b.append(gray(rpx, ri))
    n = len(a)
    if n < 16:
        raise ValueError("reference has only %d opaque pixels" % n)
    ma, mb = sum(a) / n, sum(b) / n
    cov = sum((p - ma) * (q - mb) for p, q in zip(a, b))
    va = sum((p - ma) ** 2 for p in a)
    vb = sum((q - mb) ** 2 for q in b)
    if va == 0 or vb == 0:
        return 0.0                  # a flat region matches nothing
    return cov / math.sqrt(va * vb)


def main():
    if len(sys.argv) != 7:
        sys.stderr.write("usage: hudicon-score.py SHOT.png REF.ppm X Y W H\n")
        return 2
    shot, ref = sys.argv[1], sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7])
    print("%.4f" % score(shot, ref, x, y, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
