#!/usr/bin/env python3
"""A brush entity's abslight is lit like a lightmap sample of the same value (#326).

The 1997 software renderer fills blocklights with (255 - abslight) <<
VID_CBITS (r_surf.c), the expression a lightmap sample goes through, so
abslight A shows at about 2A/255 of the texture.  The GL path drew A/255,
skipping the overbright factor every lightmapped surface gets: romeric6's
breakable pillars (abslight 0.3) came out at half brightness, "dark pillars".
Measured on romeric6's start view: the centre pillar's mean luminance 18.7 ->
37.4 (x2.00), the floor unchanged.

Checked here: the software reference is still what it is; R_BrushAbsLight
returns 2A/255, capped at 1 without gl_overbright, 1 under r_fullbright; both
brush-poly paths use it and none divides abslight by 255 any more.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


soft = strip(read("engine/h2shared/r_surf.c"))
if "light = (255-currententity->abslight)<<VID_CBITS;" not in soft:
    errors.append("r_surf.c: the software abslight reference changed; re-derive R_BrushAbsLight's scale")

gl = strip(read("engine/hexen2/gl_rsurf.c"))
fn = re.search(r"static float R_BrushAbsLight \(const entity_t \*e\)\s*\{(.*?)\n\}", gl, re.S)
if not fn:
    errors.append("gl_rsurf.c: no R_BrushAbsLight")
else:
    b = fn.group(1)
    for want in (r"2\.0f \* \(float\)e->abslight / 255\.0f", r"if \(r_fullbright\.integer\)\s*return 1\.0f;",
                 r"if \(!gl_overbright\.integer && v > 1\.0f\)\s*v = 1\.0f;"):
        if not re.search(want, b):
            errors.append(f"R_BrushAbsLight: missing {want}")
if gl.count("R_BrushAbsLight (e)") != 2:
    errors.append("both brush-poly paths (R_RenderBrushPoly, R_RenderBrushPolyMTex) must use R_BrushAbsLight")
if re.search(r"e->abslight\s*/\s*255", gl.replace("2.0f * (float)e->abslight / 255.0f", "")):
    errors.append("gl_rsurf.c: a brush path still lights abslight at abslight/255")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS brush abslight is lit as a lightmap sample of it, 2A/255 (#326)")
