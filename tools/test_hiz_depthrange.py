#!/usr/bin/env python3
"""The Hi-Z cull compares in the depth range its pyramid was drawn in (#533).

At r_mirroralpha != 1, on any map, R_Clear (engine/hexen2/gl_rmain.c) puts
the view into half the window-Z range -- [0.5, 1] under reversed-Z, [0, 0.5]
forward -- for the mirror's reflection to take the other, and the Hi-Z
pyramid is built from that buffer.  cull_mark_comp compared each surface's
unscaled [0, 1] depth against it and over-rejected: on demo1f's mirrored
graveyard (tools/hiz-mirror-check.sh) 306 of 493 post-frustum surfaces at
r_mirroralpha 0.5 against 164 at 1.

So, pinned here:
  1. CullParams carries the range (u_cdepthrange / depthrange; the layout
     itself is tools/test_cullparams_block.py's);
  2. R_DrawWorldCulled keeps the live glDepthRange beside the MVP it keeps
     for the next frame's reprojection -- the pair describes one frame;
  3. R_DispatchWorldCull uploads that kept range, not the current one;
  4. cull_mark_comp maps the surface depth into it after the corner loop
     and before the pyramid compare, unconditionally (at the Hi-Z block's
     own brace depth), and the mapping is the glDepthRange one:
     n + (f - n) * d, identity at [0, 1];
  5. a view frame drawn without the cull drops the kept MVP and range
     (R_HiZ_ForgetView), so a later culled frame cannot pair an older
     frame's range with a newer pyramid; the reflected pass does not.
A source pin: the live numbers are tools/hiz-mirror-check.sh's.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def uncommented(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


inc = uncommented((ROOT / "engine/shaders/cull.inc").read_text())
mark = uncommented((ROOT / "engine/shaders/cull_mark_comp.glsl").read_text())
src = uncommented((ROOT / "engine/h2shared/gl_worldcull.c").read_text())

# --- 1. the block member and its C mirror ---------------------------------
if not re.search(r"\bvec4\s+u_cdepthrange\s*;", inc):
    errors.append("cull.inc's CullParams has no vec4 u_cdepthrange")
if not re.search(r"\bfloat\s+depthrange\[4\]\s*;", src):
    errors.append("cull_params_t has no float depthrange[4]")


def body(name, text):
    m = re.search(rf"\n\w[^\n]*\b{name} \(void\)\s*\{{(.*?)\n\}}", text, re.S)
    return m.group(1) if m else None


# --- 2. kept with the MVP --------------------------------------------------
draw = body("R_DrawWorldCulled", src)
if draw is None:
    errors.append("no R_DrawWorldCulled body")
else:
    mvp = draw.find("memcpy(hiz_prev_mvp")
    valid = draw.find("hiz_prev_mvp_valid = true")
    get = draw.find("R_GetDepthRange")
    if min(mvp, valid, get) < 0:
        errors.append("R_DrawWorldCulled does not keep R_GetDepthRange with the MVP")
    elif not (mvp < get < valid):
        errors.append("R_DrawWorldCulled reads the depth range outside the MVP capture")
    gm = re.search(r"R_GetDepthRange\s*\(\s*&(\w+)\s*,\s*&(\w+)\s*\)", draw)
    if not gm:
        errors.append("R_DrawWorldCulled's R_GetDepthRange call is not (&near, &far)")
    else:
        for i, var in enumerate(gm.groups()):
            if not re.search(rf"hiz_prev_depthrange\[{i}\]\s*=\s*\(float\)\s*{var}\s*;", draw):
                errors.append(f"hiz_prev_depthrange[{i}] is not set from R_GetDepthRange's {var}")

# --- 3. uploaded from what was kept ---------------------------------------
disp = body("R_DispatchWorldCull", src)
if disp is None:
    errors.append("no R_DispatchWorldCull body")
else:
    for i in (0, 1):
        if not re.search(rf"params\.depthrange\[{i}\]\s*=\s*hiz_prev_depthrange\[{i}\]\s*;", disp):
            errors.append(f"R_DispatchWorldCull does not upload hiz_prev_depthrange[{i}]")
    if "R_GetDepthRange" in disp:
        errors.append("R_DispatchWorldCull reads the live range; the pyramid is last frame's")

# --- 4. the shader maps dnear, between the corners and the compare ---------
loop_end = mark.find("ry1 = max(ry1, ndc.y);")
# the first of the four samples (HIZ_AT since #340 P5f: GL's textureLod, a
# texelFetch in the Vulkan arm), not the macro defined above main
compare = mark.find("HIZ_AT(cuv")
m = re.search(r"dnear\s*=\s*([^;]*u_cdepthrange[^;]*);", mark)
if not m:
    errors.append("cull_mark_comp never maps dnear through u_cdepthrange")
elif not (0 <= loop_end < m.start() < compare):
    errors.append("cull_mark_comp maps dnear outside the corner loop .. pyramid compare span")
elif (mark[:m.start()].count("{") - mark[:m.start()].count("}")
      != mark[:loop_end].count("{") - mark[:loop_end].count("}") - 1
      or not re.search(r"(;|\})\s*$", mark[:m.start()])):
    errors.append("cull_mark_comp's dnear mapping is not an unconditional "
                  "statement of the Hi-Z block (nested, or under an if)")
else:
    expr = m.group(1)
    py = (expr.replace("u_cdepthrange.x", "n").replace("u_cdepthrange.y", "f"))
    names = set(re.findall(r"[A-Za-z_]\w*", py))
    if not re.fullmatch(r"[\w\s.+\-*/()]*", py) or not names <= {"n", "f", "dnear"}:
        errors.append(f"cull_mark_comp's mapping is not a plain expression: {expr}")
    else:
        def mapped(n, f, d):
            return eval(py, {"__builtins__": {}}, {"n": n, "f": f, "dnear": d})
        for d in (0.0, 0.001, 0.25, 0.5, 0.999, 1.0):
            if mapped(0.0, 1.0, d) != d:
                errors.append(f"mapping is not the identity at [0, 1] (d={d})")
            for n, f in ((0.5, 1.0), (0.0, 0.5)):
                want = n + (f - n) * d
                if abs(mapped(n, f, d) - want) > 1e-12:
                    errors.append(f"mapping of {d} into [{n}, {f}] gives "
                                  f"{mapped(n, f, d)}, glDepthRange gives {want}")

# --- 5. a frame drawn without the cull forgets the kept view ---------------
fv = body("R_HiZ_ForgetView", src)
if fv is None or not re.search(r"hiz_prev_mvp_valid\s*=\s*false\s*;", fv):
    errors.append("no R_HiZ_ForgetView clearing hiz_prev_mvp_valid")
rsurf = uncommented((ROOT / "engine/hexen2/gl_rsurf.c").read_text())
if not re.search(r"if\s*\(gpu_cull_active\)\s*\{[^{}]*R_DrawWorldCulled\s*\(\)\s*;\s*\}"
                 r"\s*else\s+if\s*\(\s*!\s*mirror\s*\)\s*R_HiZ_ForgetView\s*\(\s*\)\s*;", rsurf):
    errors.append("R_DrawWorld does not call R_HiZ_ForgetView for a view frame "
                  "drawn without the cull (and only for the view, not the reflection)")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS the Hi-Z cull compares in its pyramid's depth range (#533)")
