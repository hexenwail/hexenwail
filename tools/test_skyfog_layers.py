#!/usr/bin/env python3
"""Keep r_skyfog / worldspawn skyfog reaching the scrolling sky (GitHub #486).

In a fogged map the classic scrolling sky ignored r_skyfog while skyboxes
honoured it:

  - The single-pass layers (r_skyalpha >= 1, gl_shader_sky_layers) blend with
    FragParams' skyfog in ssky_layers_frag.glsl, but R_SetSkyFog had no
    caller, so the blend was always mix(color, 0, 0).
  - The two-pass layers (r_skyalpha < 1) draw with gl_shader_sky_boxside,
    which has no fog term, and nothing drew a fog pass over them.

Ironwail fills skyfogdata in Fog_SetupFrame every frame: the fog colour, and
alpha CLAMP(0, skyfog, 1) while fog is on, else 0.  Here Fog_SetupFrame
(gl_fog.c) does the same through R_SetSkyFog, and the two-pass layers get the
flat fog pass Sky_DrawSkyBox gives skybox faces, under the same condition.

This source check fails if:
  - Fog_SetupFrame stops calling R_SetSkyFog unconditionally with the fog
    colour and the density-gated, clamped skyfog,
  - R_SetSkyFog gains another caller, or R_RenderScene stops running
    Fog_SetupFrame before the world (which draws the scrolling sky) and
    Sky_DrawSky,
  - Sky_LayerFogAlpha drifts from Sky_DrawSkyBox's condition and alpha,
  - either two-pass path (R_DrawSkyChain, EmitSkyPolysMulti) loses its flat
    fog pass after the front layer, inside the blend, or
  - a shader other than ssky_layers_frag.glsl reads u_skyfog (the skybox
    faces would then be fogged twice: their shader and their flat pass).

Optional argument: --root DIR, a tree to check instead of this one (the
mutation runs use it).
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def strip_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)


def squash(text):
    return re.sub(r"\s+", "", text)


def function(text, signature):
    """Body of the first definition matching signature, "" if there is none
    (every check on an empty body then fails with a readable message)."""
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[brace:end]


def block_after(text, pos):
    """The statement or {block} starting at pos (after skipping spaces)."""
    while text[pos].isspace():
        pos += 1
    if text[pos] == "{":
        depth, i = 1, pos + 1
        while depth:
            depth += (text[i] == "{") - (text[i] == "}")
            i += 1
        return text[pos:i]
    return text[pos:text.index(";", pos) + 1]


# The alpha the sky fog must carry: Ironwail's
#   density > 0.f ? CLAMP (0.f, skyfog, 1.f) : 0.f
# Matched on whitespace-free text, any float spelling of 0 and 1.
ZERO = r"0(?:\.0*)?f?"
ONE = r"1(?:\.0*)?f?"
GATED_ALPHA = (rf"Fog_GetDensity\(\)>{ZERO}\?CLAMP\({ZERO},skyfog,{ONE}\):{ZERO}")


def check(root):
    failures = []
    fog_c = strip_comments((root / "engine/h2shared/gl_fog.c").read_text())
    warp_c = strip_comments((root / "engine/h2shared/gl_warp.c").read_text())
    sky_c = strip_comments((root / "engine/h2shared/gl_sky.c").read_text())
    rmain_c = strip_comments((root / "engine/hexen2/gl_rmain.c").read_text())

    # 1. The per-frame write, Ironwail's values, not under any condition.
    setup = function(fog_c, "void Fog_SetupFrame (void)")
    m = re.search(r"\bR_SetSkyFog\s*\(", setup)
    if not m:
        failures.append("Fog_SetupFrame: no R_SetSkyFog call")
    else:
        call = setup[m.start():setup.index(";", m.start()) + 1]
        args = squash(call)
        if not re.fullmatch(rf"R_SetSkyFog\(c\[0\],c\[1\],c\[2\],{GATED_ALPHA}\);", args):
            failures.append("Fog_SetupFrame: R_SetSkyFog must take the fog colour "
                            "(c = Fog_GetColor()) and 'Fog_GetDensity() > 0 ? "
                            "CLAMP(0, skyfog, 1) : 0', got: " + call)
        if not re.search(r"\bc\s*=\s*Fog_GetColor\s*\(\s*\)", setup):
            failures.append("Fog_SetupFrame: c is not Fog_GetColor()")
        before = setup[1:m.start()]
        depth = before.count("{") - before.count("}")
        prev = before.rstrip()[-1:]
        if depth != 0 or prev not in (";", "}", ""):
            failures.append("Fog_SetupFrame: R_SetSkyFog is conditional; it must "
                            "run every frame, fog off included (alpha 0 zeroes it)")

    # 2. One writer, and it runs before anything draws sky.
    writers = []
    for path in (root / "engine").rglob("*.c"):
        if "rust" in path.parts:
            continue
        text = strip_comments(path.read_text(errors="replace"))
        for hit in re.finditer(r"\bR_SetSkyFog\s*\(", text):
            # the definition itself
            if re.search(r"void\s*$", text[:hit.start()]):
                continue
            writers.append(path.relative_to(root).as_posix())
    if writers != ["engine/h2shared/gl_fog.c"]:
        failures.append("R_SetSkyFog callers must be exactly Fog_SetupFrame, got: "
                        + ", ".join(writers or ["none"]))

    scene = function(rmain_c, "static void R_RenderScene (void)")
    order = [scene.find(s) for s in ("Fog_SetupFrame ()", "R_DrawWorld ()",
                                     "Sky_DrawSky ()")]
    if -1 in order or order != sorted(order):
        failures.append("R_RenderScene: Fog_SetupFrame must run before R_DrawWorld "
                        "(the scrolling sky) and Sky_DrawSky")

    # 3. The two-pass fog alpha is the skybox pass's condition and alpha.
    box = squash(function(sky_c, "void Sky_DrawSkyBox (void)"))
    if not (re.search(rf"if\(Fog_GetDensity\(\)>{ZERO}&&skyfog>{ZERO}\)", box)
            and re.search(rf"GL_ImmColor4f\(c\[0\],c\[1\],c\[2\],CLAMP\({ZERO},skyfog,{ONE}\)\)", box)):
        failures.append("Sky_DrawSkyBox: its fog pass changed shape; re-check "
                        "Sky_LayerFogAlpha against it")
    alpha_fn = squash(function(warp_c, "static float Sky_LayerFogAlpha (void)"))
    if not re.fullmatch(rf"\{{if\(Fog_GetDensity\(\)>{ZERO}&&skyfog>{ZERO}\)"
                        rf"returnCLAMP\({ZERO},skyfog,{ONE}\);return{ZERO};\}}", alpha_fn):
        failures.append("Sky_LayerFogAlpha: must be 'Fog_GetDensity() > 0 && "
                        "skyfog > 0 ? CLAMP(0, skyfog, 1) : 0', got: " + alpha_fn)
    color_fn = squash(function(warp_c, "static void Sky_LayerFogColor (float a)"))
    if not ("Fog_GetColor()" in color_fn and "GL_ImmColor4f(c[0],c[1],c[2],a)" in color_fn):
        failures.append("Sky_LayerFogColor: must set the fog colour at alpha a")

    # 4. Both two-pass paths: a flat fog pass after the front layer, blend on.
    def fog_pass_after_front(body, name, front_marker):
        two = body.find("R_SetBlend (true)")
        front = body.find(front_marker, two)
        off = body.find("R_SetBlend (false)", front)
        if min(two, front, off) < 0:
            failures.append(f"{name}: two-pass front layer not found")
            return
        tail = body[front:off]
        g = re.search(r"\bif\s*\(\s*\(\s*fogalpha\s*=\s*Sky_LayerFogAlpha\s*\(\s*\)\s*\)\s*>\s*0\s*\)", tail)
        if not g:
            failures.append(f"{name}: no 'if ((fogalpha = Sky_LayerFogAlpha ()) > 0)' "
                            "fog pass between the front layer and R_SetBlend (false)")
            return
        guarded = block_after(tail, g.end())
        if not ("gl_shader_flat" in guarded and "Sky_LayerFogColor" in guarded
                and "fogalpha" in guarded.split("Sky_LayerFogColor", 1)[1]):
            failures.append(f"{name}: fog pass must draw gl_shader_flat in "
                            "Sky_LayerFogColor (fogalpha), got: " + " ".join(guarded.split()))
        if re.search(r"\b(GL_Bind|R_BindTextures?|glBindTexture_fp)\b|gl_shader_sky_", guarded):
            failures.append(f"{name}: fog pass binds a sky texture or sky program")

    fog_pass_after_front(function(warp_c, "void R_DrawSkyChain (msurface_t *s)"),
                         "R_DrawSkyChain", "SKYWARP_VERT_FRONT")
    multi = function(warp_c, "static void EmitSkyPolysMulti (msurface_t *fa)")
    fog_pass_after_front(multi, "EmitSkyPolysMulti", "gl_shader_sky_boxside")
    # In EmitSkyPolysMulti the marker finds the front layer's GL_ImmEnd only if
    # the back layer's sits before R_SetBlend (true); check that ordering too.
    if multi:
        two = multi.find("R_SetBlend (true)")
        if multi.find("GL_Bind (alphaskytexture)") > two >= 0 or \
                multi.count("gl_shader_sky_boxside", two) != 1:
            failures.append("EmitSkyPolysMulti: expected one boxside draw after "
                            "R_SetBlend (true) (the front layer)")

    # 5. Only the layers shader reads the sky fog uniform.
    readers = []
    for path in sorted((root / "engine/shaders").iterdir()):
        if path.name == "uniforms.inc" or not path.is_file():
            continue
        text = strip_comments(path.read_text(errors="replace"))
        if re.search(r"\bu_f?skyfog\b", text):
            readers.append(path.name)
    shader_c = strip_comments((root / "engine/h2shared/gl_shader.c").read_text())
    if re.search(r"\bu_f?skyfog\b", shader_c):
        readers.append("gl_shader.c")
    if readers != ["ssky_layers_frag.glsl"]:
        failures.append("u_skyfog readers must be exactly ssky_layers_frag.glsl "
                        "(skybox faces fog in a flat pass), got: "
                        + ", ".join(readers or ["none"]))
    layers = squash(strip_comments((root / "engine/shaders/ssky_layers_frag.glsl").read_text()))
    if "color=mix(color,u_skyfog.rgb,u_skyfog.a);" not in layers:
        failures.append("ssky_layers_frag.glsl: lost 'color = mix(color, "
                        "u_skyfog.rgb, u_skyfog.a)'")

    return failures


def main():
    root = ROOT
    if len(sys.argv) == 3 and sys.argv[1] == "--root":
        root = Path(sys.argv[2]).resolve()
    elif len(sys.argv) != 1:
        print("usage: test_skyfog_layers.py [--root DIR]")
        return 2
    failures = check(root)
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print("PASS: sky fog is set every frame and reaches both scrolling-sky paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
