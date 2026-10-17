#!/usr/bin/env python3
"""r_ssao: entity contact occlusion (GitHub #188).

A port of vkQuake's r_ssao (XeGTAO underneath) to GL 4.3 compute.  GTAO is
evaluated against the world's depth alone and against the scene's, and only
the occlusion the entities ADD is applied, only to world pixels: the world's
own corners are already in its lightmaps.

Held here, because each one breaks the effect quietly rather than loudly:
  - hook order in R_RenderScene: the world snapshot right after R_DrawWorld
    and before the sky writes depth; the apply right after the opaque
    entities and before the world fog is switched off (the composite fades by
    it);
  - world and scene depth are rounded through packHalf2x16 in BOTH the
    prepare and the mip pass -- the evaluate pass treats "equal" as "no
    entity here", and one unrounded side makes every pixel a mismatch;
  - the composite never writes the scene's alpha;
  - the vertical ndc->view term is negated (GL keeps view-up as NDC-up,
    the ported shaders assume Vulkan's flip);
  - off by default, desktop only (stubs on the ES tier), and torn down with
    the other GL objects on vid_restart;
  - the mismatch clear covers every texel the target can have.

Measured on demo1 (Tesla P4, offscreen, 640x480): an archer standing on the
floor gains a soft contact shadow (r_ssao_debug 1 shows it under the feet
only; world corners stay white), RMSE 0.032 against r_ssao 0, nothing else in
the frame moves.  Same view under forward-Z/no-MSAA arguments: same mask.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
c = (ROOT / "engine/h2shared/gl_ssao.c").read_text()
rm = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
misc = (ROOT / "engine/hexen2/gl_rmisc.c").read_text()
vid = (ROOT / "engine/h2shared/gl_vidsdl.c").read_text()
cm = (ROOT / "engine/CMakeLists.txt").read_text()
sh = {n: (ROOT / f"engine/shaders/{n}").read_text() for n in (
    "ssao.inc", "ssao_prepare_comp.glsl", "ssao_mip_comp.glsl", "ssao_evaluate_comp.glsl",
    "ssao_filter_comp.glsl", "ssao_composite_vert.glsl", "ssao_composite_frag.glsl")}
errors = []

scene = rm[rm.index("R_DrawWorld ();"):]
order = ["R_DrawWorld ();", "R_SSAO_CaptureWorld ();", "Sky_DrawSky ();",
         "R_DrawEntitiesOnList ();", "R_SSAO_Apply ();", "Fog_DisableGFog ();"]
pos = [scene.find(s) for s in order]
if -1 in pos or pos != sorted(pos):
    errors.append(f"R_RenderScene order must be {' -> '.join(order)}; found offsets {pos}")
if not re.search(r"R_DrawWorld \(\);[^\n]*\n\s*RPROF_CPU_END\(rprof_cpu_drawworld\);\s*(/\*.*?\*/\s*)?R_SSAO_CaptureWorld", rm, re.S):
    errors.append("R_SSAO_CaptureWorld must follow R_DrawWorld directly")

if not re.search(r'cvar_t\s+r_ssao = \{"r_ssao", "0",', c):
    errors.append("r_ssao must default to 0")
if "R_SSAO_Init ();" not in misc:
    errors.append("R_Init must register the r_ssao cvars")
if not re.search(r"R_LightCluster_Shutdown\(\);[^\n]*\n\s*R_SSAO_Shutdown \(\);", vid):
    errors.append("R_SSAO_Shutdown must run beside R_LightCluster_Shutdown (GL reuses names)")
if "${COMMONDIR}/gl_ssao.c" not in cm:
    errors.append("gl_ssao.c is not in the GL client sources")
if not re.search(r"#else\s*/\* USE_GLES.*?\*/\s*void R_SSAO_Shutdown \(void\) \{\}\s*void R_SSAO_CaptureWorld \(void\) \{\}\s*void R_SSAO_Apply \(void\) \{\}", c, re.S):
    errors.append("the ES tier must compile empty R_SSAO_* stubs")

for n in ("ssao_prepare_comp.glsl", "ssao_mip_comp.glsl"):
    if not re.search(r"(?<!un)packHalf2x16 \(", sh[n]):
        errors.append(f"{n} must round depth through packHalf2x16 (world==scene equality)")
for n, s in sh.items():
    if n != "ssao.inc" and '#include "ssao.inc"' not in s and "vert" not in n:
        errors.append(f"{n} does not include ssao.inc")
    if re.search(r"^\s*#version", s, re.M):
        errors.append(f"{n} carries a #version line; gl_ssao.c supplies it")

if "R_SetColorMask (true, true, true, false);" not in c:
    errors.append("the composite must not write the scene's alpha")
for need in ("/ -proj[5];", ") / -proj[5];"):
    if need not in c:
        errors.append(f"the vertical ndc->view term must be negated: missing {need!r}")
if "zeros[SSAO_MISMATCH_MAX * SSAO_MISMATCH_MAX]" not in c or \
   "ss_mismatch_w > SSAO_MISMATCH_MAX || ss_mismatch_h > SSAO_MISMATCH_MAX" not in c or \
   "0, 0, 0, ss_mismatch_w, ss_mismatch_h," not in c:
    errors.append("the mismatch clear must cover the whole tag texture, bounded by SSAO_MISMATCH_MAX")
if "#define SSAO_UBO_BINDING	3" not in c:
    errors.append("SSAO_UBO_BINDING moved: 0-2 are taken (Hi-Z, shared vert/frag)")

if errors:
    print("test_ssao: FAIL")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("test_ssao: ok (hook order, rounding, alpha, projection, stubs, teardown)")
