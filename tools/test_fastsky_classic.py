#!/usr/bin/env python3
"""Keep r_fastsky wired to the classic scrolling sky (GitHub #323).

r_fastsky used to reach only the skybox path (Sky_ProcessPoly / Sky_DrawSky in
gl_sky.c).  Maps without a skybox draw their sky through R_DrawSkyChain (world)
and EmitBothSkyLayers (brush models) in gl_warp.c, and neither looked at the
cvar.  Its fill colour, skyflatcolor, was computed only in gl_sky.c's
Sky_LoadTexture, which nothing calls; the live sky loader is R_InitSky in
gl_warp.c, so even a wired-up fill came out black.

On a skybox map the world chain went to Sky_ProcessPoly, which returns early
under r_fastsky, while Sky_DrawSky skipped the box: the sky was left at the
glClear colour.  Under r_fastsky the world chain now takes R_DrawSkyChain on
every map.

This source check fails if:
  - R_DrawSkyChain or EmitBothSkyLayers stops branching on r_fastsky, or the
    fast branch is empty, binds a sky texture/program, or skips Sky_FastColor,
  - Sky_FastColor stops reading the fog colour or skyflatcolor,
  - gl_rsurf.c sends skybox maps past R_DrawSkyChain under r_fastsky,
  - R_InitSky stops assigning all three skyflatcolor channels, or
  - R_InitSky loses its guard against a fully transparent front layer (0/0).
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
WARP = ROOT / "engine/h2shared/gl_warp.c"
RSURF = ROOT / "engine/hexen2/gl_rsurf.c"


def strip_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)


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


def main():
    # Optional argument: another copy of gl_warp.c, e.g. to prove the check
    # fails on the pre-fix source.
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else WARP
    src = strip_comments(path.read_text())
    failures = []

    def fast_branch(body):
        """Text of the statement or block guarded by if (r_fastsky.value)."""
        m = re.search(r"\bif\s*\(\s*r_fastsky\.value\s*\)\s*", body)
        if not m:
            return None
        rest = body[m.end():]
        if rest.startswith("{"):
            depth, i = 1, 1
            while depth:
                depth += (rest[i] == "{") - (rest[i] == "}")
                i += 1
            return rest[:i]
        return rest[:rest.index(";") + 1]

    chain = fast_branch(function(src, "void R_DrawSkyChain (msurface_t *s)"))
    if chain is None:
        failures.append("R_DrawSkyChain: no 'if (r_fastsky.value)' branch")
    elif not ("gl_shader_flat" in chain and "Sky_FastColor" in chain):
        failures.append("R_DrawSkyChain: fast branch must draw gl_shader_flat "
                        "in Sky_FastColor, got: " + " ".join(chain.split()))

    both = fast_branch(function(src, "void EmitBothSkyLayers (msurface_t *fa)"))
    if both is None or "EmitSkyPolysFast" not in both:
        failures.append("EmitBothSkyLayers: r_fastsky must route to EmitSkyPolysFast")

    fast = function(src, "static void EmitSkyPolysFast (msurface_t *fa)")
    if "gl_shader_flat" not in fast or "Sky_FastColor" not in fast:
        failures.append("EmitSkyPolysFast: must draw gl_shader_flat in Sky_FastColor")
    if re.search(r"\b(GL_Bind|glBindTexture_fp)\b|gl_shader_sky_", fast + (chain or "")):
        failures.append("fast sky path binds a sky texture or sky program")

    color = function(src, "static void Sky_FastColor (void)")
    for name in ("Fog_GetDensity", "Fog_GetColor", "skyflatcolor"):
        if name not in color:
            failures.append(f"Sky_FastColor: does not read {name}")

    # On a skybox map Sky_DrawSky skips the box under r_fastsky and
    # Sky_ProcessPoly returns early, so the world chain has to take
    # R_DrawSkyChain's flat fill or the sky is left at the clear colour.
    rsurf = strip_comments(RSURF.read_text())
    if not re.search(r"if\s*\(\s*skybox_name\[0\]\s*&&\s*!\s*r_fastsky\.value\s*\)", rsurf):
        failures.append("gl_rsurf.c: skybox maps bypass R_DrawSkyChain even "
                        "under r_fastsky (want 'skybox_name[0] && !r_fastsky.value')")

    init = function(src, "void R_InitSky (texture_t *mt)")
    for ch in range(3):
        if not re.search(rf"\bskyflatcolor\[{ch}\]\s*=", init):
            failures.append(f"R_InitSky: skyflatcolor[{ch}] is never assigned")
    if not re.search(r"if\s*\(\s*count\s*==\s*0\s*\)", init):
        failures.append("R_InitSky: no count == 0 guard before dividing by count")

    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print("PASS: r_fastsky reaches the classic sky and R_InitSky sets its colour")
    return 0


if __name__ == "__main__":
    sys.exit(main())
