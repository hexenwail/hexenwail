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
  - both world fragment shaders apply the sum after sampling and before the
    clustered dlight add, scale by u_lmstyles.y, clamp at 1; the ES tier has
    empty arms, and the WebGL smoke script resolves them;
  - the mode is settled once per frame after the cluster update;
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
sh = (ROOT / "engine/h2shared/gl_shader.c").read_text()
rm = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
smoke = (ROOT / "scripts/webgl-engine-shader-smoke.mjs").read_text()
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

for frag in ("static const char sworld_frag[] =", "static const char sworld_frag_opaque[] ="):
    body = sh[sh.index(frag):]
    body = body[:body.index("\n\t\"}\\n\";")]
    for m in ("GLSL_LMSTYLES_DECL", "GLSL_LMSTYLES_FN"):
        if m not in body:
            errors.append(f"{frag.split()[3]} lacks {m}")
    if "GLSL_LMSTYLES_APPLY\n\tGLSL_DLIGHT_APPLY" not in body:
        errors.append(f"{frag.split()[3]}: the style sum must come right before the clustered dlight add")
if '"    return vec4(min(l * u_lmstyles.y, vec3(1.0)), 1.0);\\n"' not in sh:
    errors.append("LMStyles must apply the CPU shift factor and clamp at 1")
for m in ("GLSL_LMSTYLES_DECL", "GLSL_LMSTYLES_FN", "GLSL_LMSTYLES_APPLY"):
    if not re.search(rf'#define {m}\t""', sh):
        errors.append(f"{m} needs an empty ES arm")
    if f"'{m}'" not in smoke:
        errors.append(f"the WebGL smoke script must resolve {m} on the ES tier")

if not re.search(r"R_LightCluster_BindForAlias \(\);[^\n]*\n(?:\s*/\*.*?\*/\s*\n)?\s*R_LightStyles_FrameSetup \(\);", rm, re.S):
    errors.append("R_LightStyles_FrameSetup must run right after the cluster update and binds")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS GPU lightstyles: gating, layer encoding, shaders, per-frame setup (#439)")
