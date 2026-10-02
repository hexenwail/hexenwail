#!/usr/bin/env python3
"""Data-free regression for the liquid surface Z ripple (#312).

A two-sided ripple lifted the water's edge off any sloped floor it touched:
a rim vertex pushed up leaves the slope and floats, while against a vertical
wall it only slides along the wall.  The fix makes the ripple downward only.

Compiles the production R_WaterRipple out of engine/h2shared/gl_warp.c and
sweeps it over position, time and depth, checking:

  * It never rises above the rest plane, which is the whole of #312.
  * Its trough is the old two-sided trough (-depth), so a shallow puddle
    never shows more of its floor than it did before the fix.
  * It still moves, by the full depth, and along both in-plane axes rather
    than in 1-D stripes (212e3c588).
  * A depth of zero, below zero or NaN is a flat surface, so a sign flip at
    the call site cannot turn the wave upward.

Then checks every assignment to nz in EmitWaterPolys is `nz = v[2];` or
`nz += R_WaterRipple (...);` and nothing else, so no extra term, inline
sine or sinf can bring an upward wave back beside it.

The trade-off the fix accepts, half the old peak-to-peak height, is pinned
analytically by the trough check; tools/water_ripple_eval measures the lift
on screen.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from test_multiplayer_transparency import ROOT, function

WARP = "engine/h2shared/gl_warp.c"

PRELUDE = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
'''

TEST = r'''
#define EPS 1e-4f

int main(void)
{
    static const float depths[] = {0.25f, 1.0f, 2.0f, 40.0f};
    unsigned t;

    for (t = 0; t < sizeof(depths)/sizeof(depths[0]); t++)
    {
        float amt = depths[t], lo = 0.0f, hi = -1e9f;
        int xi, yi, ti;

        for (ti = 0; ti < 64; ti++)
        for (xi = -256; xi <= 256; xi += 4)
        for (yi = -256; yi <= 256; yi += 4)
        {
            float z = R_WaterRipple ((float)xi, (float)yi, ti * 0.37f, amt);
            /* #312: never above the rest plane, never below the old trough. */
            assert(z <= EPS);
            assert(z >= -amt - EPS * amt);
            if (z < lo) lo = z;
            if (z > hi) hi = z;
        }
        /* The full depth is still used: the wave reaches the rest plane and
         * the old trough, so it has not been flattened or shrunk further. */
        assert(fabsf(hi) < 0.01f * amt);
        assert(fabsf(lo + amt) < 0.01f * amt);
    }

    /* 2-D, not stripes: at a fixed time the surface varies along x with y
     * held and along y with x held. */
    {
        float ax = R_WaterRipple (0.0f, 10.0f, 0.3f, 1.0f) - R_WaterRipple (20.0f, 10.0f, 0.3f, 1.0f);
        float ay = R_WaterRipple (10.0f, 0.0f, 0.3f, 1.0f) - R_WaterRipple (10.0f, 20.0f, 0.3f, 1.0f);
        assert(fabsf(ax) > 0.05f && fabsf(ay) > 0.05f);
    }

    /* No depth, no ripple: zero, negative (a sign flip at the call site
     * would otherwise turn the wave upward) and NaN are all flat. */
    {
        static const float flat[] = {0.0f, -1.0f, -40.0f};
        unsigned i;
        for (i = 0; i < sizeof(flat)/sizeof(flat[0]); i++)
        {
            int xi;
            for (xi = -256; xi <= 256; xi += 8)
                assert(R_WaterRipple ((float)xi, 31.0f, 1.3f, flat[i]) == 0.0f);
        }
        assert(R_WaterRipple (1.0f, 2.0f, 3.0f, NAN) == 0.0f);
    }

    puts("PASS: liquid ripple stays at or below the rest plane, keeps the old trough, and is 2-D");
    return 0;
}
'''


def main():
    ripple = function(WARP, "static float R_WaterRipple")
    emit = function(WARP, "void EmitWaterPolys")

    # The vertex Z goes through R_WaterRipple and only through it.  Every
    # write to nz must be one of these two shapes; anything else (an extra
    # "+ R", "nz = nz + sinf(...)", a second ripple) could lift the rim again.
    writes = re.findall(r"\bnz\s*([-+*/]?=)\s*([^;]*);", emit)
    ripples = 0
    for op, rhs in writes:
        rhs = " ".join(rhs.split())
        if op == "=" and rhs == "v[2]":
            continue
        if op == "+=" and re.fullmatch(r"R_WaterRipple\s*\([^()]*\)", rhs):
            ripples += 1
            continue
        raise AssertionError(f"EmitWaterPolys writes nz {op} {rhs}: only "
                             "nz = v[2] and nz += R_WaterRipple (...) may touch it")
    assert ripples == 1, "EmitWaterPolys must ripple nz through R_WaterRipple exactly once"
    # nz must be what reaches the vertex, not v[2] directly.
    assert re.search(r"GL_ImmVertex3f\s*\(\s*v\[0\]\s*,\s*v\[1\]\s*,\s*nz\s*\)", emit), \
        "EmitWaterPolys must emit the rippled nz"

    with tempfile.TemporaryDirectory(prefix="water-ripple-") as tmp:
        cfile = Path(tmp) / "ripple.c"
        binary = Path(tmp) / "ripple"
        cfile.write_text(PRELUDE + ripple + "\n" + TEST)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                        "-Werror", str(cfile), "-o", str(binary), "-lm"], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
