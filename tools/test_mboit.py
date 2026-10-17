#!/usr/bin/env python3
"""r_oit 2: moment-based OIT, from vkQuake's mboit.inc (GitHub #87).

Held:
  - the OIT preamble branches on u_oit_mode: 0 WBOIT (unchanged), 1 MBOIT
    moments (b0, then z..z^4 weighted by absorbance), 2 MBOIT composite
    (colour times the transmittance reconstructed at this depth, moments
    read with texelFetch -- not Vulkan subpass inputs), both MBOIT passes
    dropping alpha < 1/255;
  - the reconstruction keeps vkQuake's constants (overestimation 0.25, bias
    6e-5 toward (0, .375, 0, .375));
  - the translucency block in R_RenderView is drawn again while
    OIT_NextPass() says so, and R_DrawWaterSurfaces keeps its chains on the
    moment pass (OIT_KeepChains), or the composite would draw no water;
  - the moment targets are 32-bit float; the resolve turns b0 into
    revealage as exp(-b0);
  - the MBOIT samplers sit on their own units, 2D and multisample apart.

Measured: a single translucent layer (demo1 water) resolves the same under
r_oit 1 and 2 (RMSE 0.0005); four stacked translucent shells render in both
with and without 4x MSAA, 0 GL errors; the reconstruction, run on the CPU
for those depths, gives transmittance 0.81 / 0.54 / 0.28 / 0.14 against a
true 1 / .5 / .25 / .125 -- the 0.25 overestimation's known front-layer
under-weighting.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []
sh, pp, rm, rs = (read("engine/h2shared/gl_shader.c"), read("engine/h2shared/gl_postprocess.c"),
                  read("engine/hexen2/gl_rmain.c"), read("engine/hexen2/gl_rsurf.c"))

for need in ('"uniform int u_oit_mode;\\n"', '"    if (u_oit_mode == 0) {\\n"', '"    if (u_oit_mode == 1) {\\n"',
             '"        out_reveal = vec4(mz, z2, z2 * mz, z2 * z2) * absorbance;\\n"',
             '"    if (fragColor.a < (1.0 / 255.0))\\n"', '"#define MBOIT_OVERESTIMATION 0.25\\n"',
             'z, 6e-5, vec4(0.0, 0.375, 0.0, 0.375)', '"        b0 = texelFetch(u_mboit_b0_ms, pix, 0).r;\\n"',
             '"    out_accum = vec4(fragColor.rgb * fragColor.a * t, fragColor.a * t);\\n"'):
    if need not in sh:
        errors.append(f"OIT preamble lacks {need}")
if "subpassInput" in sh or "subpassLoad" in sh:
    errors.append("no Vulkan subpass inputs in the GL port")
if not re.search(r"\} while \(OIT_NextPass \(\)\);\s*OIT_EndTranslucency\(GL_GetSceneFBO\(\)\);", rm):
    errors.append("R_RenderView must redraw the translucency block while OIT_NextPass() is true")
if "if (!OIT_KeepChains ())\n\t\t\tt->texturechain = NULL;" not in rs:
    errors.append("R_DrawWaterSurfaces must keep its chains on the MBOIT moment pass")
if "GLenum	fmts[2] = { GL_R32F, GL_RGBA32F };" not in pp:
    errors.append("the moment targets must be 32-bit float")
if pp.count('"    if (u_mboit != 0) revealage = exp(-max(revealage, 0.0));\\n"') != 2:
    errors.append("both resolve programs must turn b0 into revealage as exp(-b0)")
units = [int(x) for x in re.findall(r"#define MBOIT_TMU_\w+\s+(\d+)", read("engine/h2shared/gl_shader.h"))]
if len(units) != 4 or len(set(units)) != 4 or min(units) <= 5:
    errors.append("the four MBOIT sampler units must be distinct and above the units draws bind (0-5)")
kc = re.search(r"qboolean OIT_KeepChains \(void\)\s*\{(.*?)\n\}", pp, re.S)
if not kc or "mboit_pass == 1" not in kc.group(1):
    errors.append("OIT_KeepChains must be true only on the moment pass")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS r_oit 2 moment-based OIT: preamble, two passes, chains kept, resolve (#87)")
