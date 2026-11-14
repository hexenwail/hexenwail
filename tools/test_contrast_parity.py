#!/usr/bin/env python3
"""`contrast` is Ironwail's: a multiplier clamped to 1..2 that brightens.

Ironwail (gl_rmain.c / gl_shaders.h), QuakeSpasm and MarkV multiply the frame
by contrast, clamped to [1, 2], before gamma.  Ours stretched about mid-grey,
(c - 0.5) * contrast + 0.5, which a dark scene reads as darkening: contrast
1.5 took demo1's mean from 0.107 to 0.018; now 0.161.  Checked: the shader
multiplies, the uniform and the post-process trigger use the clamped value,
the menu offers 1..2.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []
pp = read("engine/h2shared/gl_postprocess.c")
# The post-process fragment shader is engine/shaders/spp_frag.glsl since #450.
shader = read("engine/shaders/spp_frag.glsl")
if "    color.rgb *= contrast;\n" not in shader:
    errors.append("the post-process shader must multiply by contrast")
if "(color.rgb - 0.5) * contrast + 0.5" in shader + pp:
    errors.append("the mid-grey contrast stretch is still there")
if not re.search(r"#define\s+contrast\s+pp_p0\.y\b", shader):
    errors.append("contrast must alias PpParams' pp_p0.y")
fn = re.search(r"static float PP_Contrast \(void\)\s*\{(.*?)\n\}", pp, re.S)
if not fn or "if (c < 1.0f) c = 1.0f;" not in fn.group(1) or "if (c > 2.0f) c = 2.0f;" not in fn.group(1):
    errors.append("PP_Contrast must clamp to Ironwail's 1..2")
if "pp_params.p0[1] = PP_Contrast ();" not in pp or "PP_Contrast () != 1.0f" not in pp:
    errors.append("the uniform and the post-process trigger must use the clamped contrast")
menu = read("engine/hexen2/menu.c")
if '{ DISP_CONTRAST,	"contrast",	1.0f, 2.0f,	1.0f, 2.0f,' not in menu or "if (f < 1)	f = 1;" not in menu:
    errors.append("the Display menu must offer contrast 1..2")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS contrast multiplies, clamped 1..2, as Ironwail")
