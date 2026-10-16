#!/usr/bin/env python3
"""Mods need the full game, at runtime as at startup, and the menu says so (#171).

FS_Init refuses -game without GAME_REGISTERED / GAME_REGISTERED_OLD ("You must
have the full version of Hexen II to play modified games").  The runtime
"game" command had no such check, and the main menu offered MODS on the demo
regardless.  Checked at source level, in the C reference and the shipped Rust
port:

  - Host_Game_f refuses a non-data1 gamedir without the full game, except in
    hwsv (H2W), which the startup check exempts too;
  - the main menu dims MODS, says why, and ignores Enter on it, keyed on the
    same flags;
  - the GL glyph batcher can fade at all: 2D alpha-tests at 0.666 with
    blending off, so Draw_SetCharacterAlpha has to blend and scale the
    cut-off, or a dimmed row vanishes.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def body(text, signature):
    start = text.index(signature)
    while ";" in text[start:text.index("{", start)]:
        start = text.index(signature, start + 1)
    i = text.index("{", start)
    depth, end = 1, i + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


MSG = "You must have the full version of Hexen II to play modified games"

c = strip((ROOT / "engine/h2shared/quakefs.c").read_text())
game_c = body(c, "static void Host_Game_f (void)")
if not re.search(r"#if\s*!\s*\(\s*defined\s*\(\s*H2W\s*\)\s*&&\s*defined\s*\(\s*SERVERONLY\s*\)\s*\)\s*"
                 r"if\s*\(\s*q_strcasecmp\s*\(\s*dir\s*,\s*\"data1\"\s*\)\s*&&\s*!\s*\(\s*gameflags\s*&\s*"
                 r"\(\s*GAME_REGISTERED\s*\|\s*GAME_REGISTERED_OLD\s*\)\s*\)\s*\)", game_c) or MSG not in game_c:
    errors.append("quakefs.c: Host_Game_f must refuse a mod without the full game (outside hwsv)")

rs = (ROOT / "engine/rust/src/quakefs.rs").read_text()
i = rs.index("unsafe extern \"C\" fn Host_Game_f()")
game_rs = rs[i:rs.index("\n}\n", i)]
if not re.search(r"QuakeFS_TargetIsH2W\(\)\s*==\s*0\s*&&\s*q_strcasecmp\(dir,\s*c\"data1\"\.as_ptr\(\)\)\s*!=\s*0\s*"
                 r"&&\s*\(gameflags\s*&\s*\(GAME_REGISTERED\s*\|\s*GAME_REGISTERED_OLD\)\)\s*==\s*0", game_rs) \
        or MSG not in game_rs:
    errors.append("quakefs.rs: Host_Game_f must carry the same gate as the C")

menu = strip((ROOT / "engine/hexen2/menu.c").read_text())
avail = body(menu, "static qboolean M_ModsAvailable (void)")
if "GAME_REGISTERED|GAME_REGISTERED_OLD" not in avail.replace(" ", ""):
    errors.append("menu.c: M_ModsAvailable must key on GAME_REGISTERED / GAME_REGISTERED_OLD")
draw = body(menu, "static void M_Main_Draw (void)")
if not re.search(r"if\s*\(\s*!\s*M_ModsAvailable\s*\(\s*\)\s*\)\s*Draw_SetCharacterAlpha", draw) \
        or "needs full game" not in draw:
    errors.append("menu.c: M_Main_Draw must dim MODS and say why without the full game")
key = body(menu, "static void M_Main_Key (int key)")
if not re.search(r"case\s+3\s*:\s*if\s*\(\s*!\s*M_ModsAvailable\s*\(\s*\)\s*\)", key):
    errors.append("menu.c: Enter on MODS must do nothing without the full game")

gl = strip((ROOT / "engine/h2shared/gl_draw.c").read_text())
fade = body(gl, "void Draw_SetCharacterAlpha (float a)")
if "Draw_FlushCharBatch" not in fade or not re.search(r"GL_SetAlphaThreshold\s*\(\s*0\.666f\s*\*\s*a\s*\)", fade) \
        or not re.search(r"R_SetBlend\s*\(\s*true\s*\)", fade):
    errors.append("gl_draw.c: Draw_SetCharacterAlpha must flush, blend, and scale the 2D alpha cut-off")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS mods need the full game, and the menu says so (#171)")
