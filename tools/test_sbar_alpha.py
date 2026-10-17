#!/usr/bin/env python3
"""scr_sbaralpha fades the bottom bar on GL, and nothing else (#83).

Decided on #83: Ironwail's scr_sbaralpha, scoped to the bottom bar (the bar,
its top bumps, its numbers, the lower bar); the edge ring and artifact
indicators, the inventory strip, the deathmatch overlays and the minimal
full-screen HUD stay opaque.  Default 1 -- Hexen II's bar has always been
solid -- not upstream's 0.75.  GL only: the software renderer keeps sbtrans,
which GL never read, so the "HUD Transparency" row was a dead knob there; on GL
it is now "Status Bar Alpha", a slider for scr_sbaralpha.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []

sbar = read("engine/hexen2/sbar.c")
if 'cvar_t scr_sbaralpha = {"scr_sbaralpha", "1", CVAR_ARCHIVE};' not in sbar:
    errors.append("scr_sbaralpha must exist, archived, default 1")
draw = sbar[sbar.index("void Sbar_Draw(void)\n{"):]
draw = draw[:draw.index("\n}\n")]
on = draw.find("Sbar_BarAlpha (true);")
bumps = draw.find('Draw_CachePic("gfx/topbumpl.lmp")')
for edge in ("DrawArtifactInventory();", "DrawActiveRings();", "DrawActiveArtifacts();",
             "Sbar_DeathmatchOverlay();", "Sbar_SmallDeathmatchOverlay();"):
    if draw.find(edge) > on >= 0:
        errors.append(f"{edge} must stay opaque: it is drawn after the bar alpha is set")
if on < 0 or bumps < on:
    errors.append("the bar alpha must be set before the top bumps")
blk = re.search(r"if \(BarHeight < 0 && scr_viewsize\.integer <= 120\)\s*\{(.*?)DrawFullScreenInfo\(\);", draw, re.S)
if not blk or "Sbar_BarAlpha (false);" not in blk.group(1):
    errors.append("the minimal full-screen HUD must be drawn opaque (reset inside its own block)")
if not draw.rstrip().endswith("Sbar_BarAlpha (false);"):
    errors.append("Sbar_Draw must put the alpha back to opaque at the end")
helper = sbar[sbar.index("static void Sbar_BarAlpha (qboolean on)"):]
helper = helper[:helper.index("\n}\n")]
if "#if defined(GLQUAKE)" not in helper:
    errors.append("Sbar_BarAlpha must be GL only (software has sbtrans)")

gl = read("engine/h2shared/gl_draw.c")
for sig in ("void Draw_Pic (int x, int y, qpic_t *pic)\n{", "void Draw_PicCropped (int x, int y, qpic_t *pic)\n{",
            "void Draw_SubPicCropped (int x, int y, int h, qpic_t *pic)\n{"):
    b = gl[gl.index(sig):]
    b = b[:b.index("\n}\n")]
    if "GL_ImmColor4f (1, 1, 1, pic_alpha);" not in b or "Draw_PicAlphaBegin ();" not in b or "Draw_PicAlphaEnd ();" not in b:
        errors.append(f"gl_draw.c: {sig.split('(')[0]} must draw at the pic alpha")

menu = read("engine/hexen2/menu.c")
case = menu[menu.index("\tcase GFX_HUDTRANS:\n\t{"):]
case = case[:case.index("\t\tbreak;")]
if 'Cvar_SetValue ("scr_sbaralpha", v);' not in case or 'Cvar_SetValue ("sbtrans", v);' not in case:
    errors.append("the HUD row must drive scr_sbaralpha on GL and sbtrans on the software build")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS scr_sbaralpha fades the bottom bar on GL and nothing else (#83)")
