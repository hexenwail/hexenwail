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
  - Sky_LayerFogAlpha drifts from Sky_DrawSkyBox's condition and alpha, or
    Sky_LayerFogColor from setting the fog colour at alpha a and nothing else,
  - anything but R_SetSkyFog (gl_uniforms.c) touches fragparams.skyfog,
  - Fog_GetColor stops clamping its colour to 0..1 after the fade and before
    the 24-bit rounding, as Ironwail does (the worldspawn fog key is raw),
  - either two-pass path (R_DrawSkyChain, EmitSkyPolysMulti) loses its flat
    fog pass after the front layer, inside the blend, or
  - a shader other than ssky_layers_frag.glsl reads u_skyfog (the skybox
    faces would then be fogged twice: their shader and their flat pass), or
  - Sky_DrawSky gates the skybox on skyfog again (GitHub #487).  QuakeSpasm
    skips the box at skyfog >= 1 because it had painted the sky surfaces the
    fog colour; here they only write depth and stencil, so the skip left the
    sky black.  Ironwail always draws the box; r_fastsky is the only gate.

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

    # 1b. Fog_GetColor clamps after the fade, before the 24-bit rounding.
    getc = squash(function(fog_c, "float *Fog_GetColor (void)"))
    clamp = re.search(rf"for\(i=0;i<3;i\+\+\)\{{?c\[i\]=CLAMP\({ZERO},c\[i\],{ONE}\);\}}?", getc)
    lerp = getc.rfind("c[2]=fog_blue;")
    rnd = getc.find("Q_rint(")
    if not clamp or lerp < 0 or rnd < 0 or not lerp < clamp.start() < rnd:
        failures.append("Fog_GetColor: must clamp c[0..2] to 0..1 "
                        "('for (i=0;i<3;i++) c[i] = CLAMP(0, c[i], 1);') after "
                        "the fade and before the Q_rint rounding, as Ironwail does")

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

    # The uniform itself: R_SetSkyFog is its only toucher, and it is a plain
    # store, so R_SetSkyFog's single caller decides what the shader sees.
    touchers = []
    for path in sorted((root / "engine").rglob("*.[ch]")):
        if "rust" in path.parts:
            continue
        text = strip_comments(path.read_text(errors="replace"))
        for hit in re.finditer(r"\bfragparams\s*(?:\.|->)\s*skyfog\b", text):
            touchers.append((path.relative_to(root).as_posix(), hit.start()))
    uni_c = strip_comments((root / "engine/h2shared/gl_uniforms.c").read_text())
    setter_at = uni_c.find("void R_SetSkyFog (float r, float g, float b, float a)")
    setter = function(uni_c, "void R_SetSkyFog (float r, float g, float b, float a)")
    inside = False
    if setter_at >= 0 and len(touchers) == 1:
        brace = uni_c.index("{", setter_at)
        f, pos = touchers[0]
        inside = f == "engine/h2shared/gl_uniforms.c" and brace < pos < brace + len(setter)
    if not inside:
        failures.append("fragparams.skyfog must be touched only in R_SetSkyFog "
                        "(gl_uniforms.c), once; got %d: " % len(touchers)
                        + ", ".join(sorted({t[0] for t in touchers}) or ["none"]))
    if squash(setter) != "{if(RU_Set4(fragparams.skyfog,r,g,b,a))frag_dirty=true;}":
        failures.append("R_SetSkyFog: must store (r, g, b, a) as given, got: "
                        + squash(setter))

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
    if color_fn != "{float*c=Fog_GetColor();GL_ImmColor4f(c[0],c[1],c[2],a);}":
        failures.append("Sky_LayerFogColor: must be exactly 'float *c = "
                        "Fog_GetColor(); GL_ImmColor4f (c[0], c[1], c[2], a);', "
                        "got: " + color_fn)

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
        if re.search(r"\b(R_BindTexture\w*|glBindTexture_fp)\b|gl_shader_sky_", guarded):
            failures.append(f"{name}: fog pass binds a sky texture or sky program")

    fog_pass_after_front(function(warp_c, "void R_DrawSkyChain (msurface_t *s)"),
                         "R_DrawSkyChain", "SKYWARP_VERT_FRONT")
    multi = function(warp_c, "static void EmitSkyPolysMulti (msurface_t *fa)")
    fog_pass_after_front(multi, "EmitSkyPolysMulti", "gl_shader_sky_boxside")
    # In EmitSkyPolysMulti the marker finds the front layer's GL_ImmEnd only if
    # the back layer's sits before R_SetBlend (true); check that ordering too.
    if multi:
        two = multi.find("R_SetBlend (true)")
        if multi.find("R_BindTextureSlot (0, alphaskytexture)") > two >= 0 or \
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

    # 6. The skybox is drawn whatever skyfog is; only r_fastsky skips it.
    draw = function(sky_c, "void Sky_DrawSky (void)")
    if re.search(r"\bskyfog\b", draw):
        failures.append("Sky_DrawSky: reads skyfog; the skybox must not be "
                        "skipped on it (#487: the sky surfaces are not painted, "
                        "so a skipped box leaves the sky black)")
    calls = [m.start() for m in re.finditer(r"\bSky_DrawSkyBox\s*\(\s*\)\s*;", draw)]
    gate = re.search(r"\bif\s*\(\s*!\s*r_fastsky\.value\s*\)", draw)
    if len(calls) != 1 or not gate:
        failures.append("Sky_DrawSky: expected one Sky_DrawSkyBox () call inside "
                        "'if (!r_fastsky.value)'")
    else:
        guarded = block_after(draw, gate.end())
        start = gate.end() + draw[gate.end():].index(guarded[0])
        if not start <= calls[0] < start + len(guarded):
            failures.append("Sky_DrawSky: Sky_DrawSkyBox () is not inside "
                            "'if (!r_fastsky.value)'")

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
    print("PASS: sky fog is set every frame, reaches both scrolling-sky paths, "
          "and never skips the skybox")
    return 0


if __name__ == "__main__":
    sys.exit(main())
