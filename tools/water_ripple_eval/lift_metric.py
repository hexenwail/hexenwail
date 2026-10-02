#!/usr/bin/env python3
"""Eval for GitHub #312.  Count pixels the rippled liquid covers that the flat liquid did not.

A surface that only moves at or below its rest plane can only be hidden
further by the floor, never cover more of it, so for #312's fix this count
should be ~0 (edge antialiasing noise only).  The two-sided ripple covers
ramp/wall pixels wherever it rises.

usage: lift_metric.py OUTDIR VIEW...   (expects VIEW-flat.png, VIEW-NN.png)

Two controls keep it from passing vacuously: every flat frame must show at
least MIN_WATER water pixels (the camera is looking at the pool), and every
view must LOSE cover in some frame (a downward ripple uncovers ramp and wall
near the rim, so this proves the ripple actually ran).

Exits 1 if a control fails, or if any frame of any view covers more than LIMIT new pixels (default
48, 0.01% of 800x600: room for antialiasing on another GPU).  Measured on
2026-10-02 at 800x600 under llvmpipe: the two-sided ripple scored up to
69717 exaggerated and 582 at shipping defaults; the fix scored 0 everywhere.
"""
import os
import subprocess, sys, pathlib

def load(p):
    raw = subprocess.run(["magick", str(p), "-depth", "8", "ppm:-"], check=True,
                         capture_output=True).stdout
    # P6\nW H\n255\n
    parts = raw.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    water = bytearray(w * h)
    for i in range(w * h):
        r, g, b = px[3*i], px[3*i+1], px[3*i+2]
        water[i] = b > r + 25          # *water is blue/cyan; ramp is tan, walls grey
    return water

LIMIT = int(os.environ.get("LIMIT", "48"))
MIN_WATER = 20000
out = pathlib.Path(sys.argv[1])
worst = 0
for view in sys.argv[2:]:
    flat = load(out / f"{view}-flat.png")
    counts, lost = [], []
    for f in sorted(out.glob(f"{view}-[0-9]*.png")):
        rip = load(f)
        counts.append(sum(1 for a, b in zip(rip, flat) if a and not b))
        lost.append(sum(1 for a, b in zip(rip, flat) if b and not a))
    print(f"{view:4s} flat-water={sum(flat):6d}  new-cover per frame={counts}  max={max(counts)}"
          f"  max-uncovered={max(lost)}")
    if sum(flat) < MIN_WATER:
        sys.exit(f"FAIL: {view} flat frame shows {sum(flat)} water pixels; the pool is not in view")
    if max(lost) == 0:
        sys.exit(f"FAIL: {view} rippled frames never differ from flat; the ripple did not run")
    worst = max(worst, max(counts))
print(("PASS" if worst <= LIMIT else "FAIL") + f": worst frame covers {worst} new pixels (limit {LIMIT})")
sys.exit(worst > LIMIT)
