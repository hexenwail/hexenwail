#!/usr/bin/env python3
"""GPU lightstyles: animated light without a lightmap rebuild (GitHub #439).

Ironwail's design with same-UV layer textures: where clustered lighting
shades the world (its SSBO carries LightStyles[]) and the atlas is on, the
atlas holds each surface's first style RAW, three layer atlases at the same
UVs hold styles 2-4, every layer's alpha is that layer's style index (255:
none), and the world shader sums them weighted by LightStyles[], then applies
the CPU path's shift and clamp.

Held here:
  - every per-frame style-change check is gated off in that mode (a missed
    one would rebuild layer 0 from combined light and double-count styles);
  - R_BuildLightMap's layer 0: first style only, at scale 256 and shift 8, no
    baked dlights, the style index in alpha;
  - the layer atlases are cleared to alpha 255 ("no style");
  - both world fragment shaders (engine/shaders, #450 W2) apply the sum
    after sampling and right before the clustered dlight add, desktop only
    (#ifndef GL_ES, where the ES tier has no SSBO), scale by u_lmstyles.y
    (WorldFrag) and clamp at 1 (lmstyles.inc);
  - the mode is settled once per frame after the cluster update, into the
    world frame record every world draw copies into WorldFrag;
  - the rebuild skips SURF_DRAWTILED only, so lit liquids are rebuilt too.

Measured on demo2 (NVIDIA, offscreen): at fixed style values (r_flatlightstyles
1 and 2) the GPU path matches the CPU one to RMSE 0.002-0.004 in the textured
view, also with gl_overbright 0, gl_coloredlight 0 and r_lightmap_bicubic 1;
with the lights animating, lightmap pages uploaded per frame drop from 2.00
to 0.00 at the same view.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
rs = (ROOT / "engine/hexen2/gl_rsurf.c").read_text()
SH = ROOT / "engine/shaders"
rm = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
errors = []

checks = re.findall(r"for \(maps = 0;([^;]*);[^)]*\)\s*\{\s*if \(d_lightstylevalue", rs)
if len(checks) != 6:
    errors.append(f"expected 6 per-frame style-change checks, found {len(checks)}")
for cond in checks:
    if "!lm_gpu_styles" not in cond:
        errors.append("a style-change check is not gated on !lm_gpu_styles")

blm = rs[rs.index("static void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)"):]
blm = blm[:blm.index("\n}\n")]
for need in ("const qboolean	layered = lm_gpu_styles;", "if (layered)\n				scale = 256;",
             "if (layered)\n				break;", "if (!layered && r_dynamic.integer",
             "const int lmshift = layered ? 8 : 7 + (gl_overbright.integer ? 1 : 0);"):
    if need not in blm:
        errors.append(f"R_BuildLightMap lacks: {need!r}")
if blm.count("dest[3] = lm_alpha;") != 2:
    errors.append("both lightmap writers must store the style index in alpha")

if "clear[n + 3] = 255;" not in rs:
    errors.append("the layer atlases must be cleared to alpha 255 (no style)")
reb = rs[rs.find("static void LM_RebuildSurfaces (void)\n{"):] if "static void LM_RebuildSurfaces (void)\n{" in rs else ""
reb = reb[:reb.find("\n}\n")]
if "if (surf->flags & SURF_DRAWTILED)" not in reb or "SURF_DRAWTURB" in reb:
    errors.append("LM_RebuildSurfaces must skip SURF_DRAWTILED only (lit liquids have lightmaps)")

for frag in ("sworld_frag", "sworld_opaque_frag"):
    body = (SH / f"{frag}.glsl").read_text()
    desk = "".join(re.findall(r"#ifndef GL_ES\n(.*?)#endif", body, re.S))
    for need in ('#include "lmstyles.inc"', "uniform sampler2D u_lmlayer1;", "uniform sampler2D u_lmlayer2;",
                 "uniform sampler2D u_lmlayer3;"):
        if need not in desk:
            errors.append(f"{frag}.glsl: {need} must sit under #ifndef GL_ES")
    if not re.search(r"#ifndef GL_ES\n(?:\s*/\*.*?\*/\n)?\s*if \(u_lmstyles\.x > 0\.5\) lm = LMStyles\(lm, lmuv\);\n"
                     r"\s*if \(u_dlight_scale > 0\.0\) lm\.rgb = ClusteredDlight\(lm\.rgb, v_eyepos\);\n#endif", body, re.S):
        errors.append(f"{frag}.glsl: the style sum must come right before the clustered dlight add, desktop only")
if not re.search(r"#define\s+u_lmstyles\s+u_wflmstyles\.xy\n", (SH / "world.inc").read_text()):
    errors.append("world.inc: u_lmstyles must be WorldFrag's u_wflmstyles.xy")
if "    return vec4(min(l * u_lmstyles.y, vec3(1.0)), 1.0);\n" not in (SH / "lmstyles.inc").read_text():
    errors.append("LMStyles must apply the CPU shift factor and clamp at 1")
fs = rs[rs.index("void R_LightStyles_FrameSetup (void)"):]
fs = fs[:fs.index("\n}\n")]
if "R_SetWorldLMStyles (lm_gpu_styles ? 1.0f : 0.0f, gl_overbright.integer ? 1.0f : 2.0f);" not in fs:
    errors.append("R_LightStyles_FrameSetup must record the mode and the CPU shift factor for the world draws")

if not re.search(r"R_LightCluster_BindForAlias \(\);[^\n]*\n(?:\s*/\*.*?\*/\s*\n)?\s*R_LightStyles_FrameSetup \(\);", rm, re.S):
    errors.append("R_LightStyles_FrameSetup must run right after the cluster update and binds")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS GPU lightstyles: gating, layer encoding, shaders, per-frame setup (#439)")
