#!/usr/bin/env python3
"""320x200 is a valid video mode again, and the main menu fits in it (#135).

MIN_HEIGHT had been raised from 200 to 240 without a recorded reason, so
`-width 320 -height 200` was rejected as "invalid" and silently ran at
640x480.  320x200 is the minimum Quake and WinHexen2 define.  Checked at a real
320x200 before lowering it: HUD, console, view and menus render; the only
collision was the main menu's bottom labels (renderer name, version watermark)
printing over QUIT, which now yield to the menu when its canvas reaches their
line.  Source-level checks:
  - one floor, MIN_HEIGHT 200, and no second spelling of it (MIN_CONHEIGHT);
  - both bottom labels consult Draw_MenuReachesLine, which the GL and the
    software-web draw layers both provide.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []

vid = read("engine/h2shared/gl_vidsdl.c")
if not re.search(r"#define\s+MIN_HEIGHT\s+200\b", vid):
    errors.append("gl_vidsdl.c: MIN_HEIGHT must be 200")
if "MIN_CONHEIGHT" in vid:
    errors.append("gl_vidsdl.c: MIN_CONHEIGHT is a second spelling of the one floor")

menu = read("engine/hexen2/menu.c")
main = menu[menu.index("static void M_Main_Draw (void)\n{"):]
main = main[:main.index("\n}\n")]
if "if (!Draw_MenuReachesLine (y)" not in main:
    errors.append("M_Main_Draw: the renderer label must yield where the menu reaches its line")
gl = read("engine/h2shared/gl_draw.c")
ver = gl[gl.index("static void Draw_ConsoleVersionInfo (int lines)\n{"):]
ver = ver[:ver.index("\n}\n")]
if "Key_GetDest () == key_menu && Draw_MenuReachesLine (y)" not in ver:
    errors.append("Draw_ConsoleVersionInfo: the watermark must yield to an open menu that reaches it")
if "qboolean Draw_MenuReachesLine (int y)" not in gl or "qboolean Draw_MenuReachesLine (int y)" not in read("engine/h2shared/draw_soft_web.c"):
    errors.append("Draw_MenuReachesLine must exist in gl_draw.c and draw_soft_web.c")
if "200.0f * s / (float)gh * (float)vid.height > (float)y" not in gl:
    errors.append("gl_draw.c: Draw_MenuReachesLine no longer maps the menu's 200 units the way GL_SetCanvas does")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS 320x200 is a valid mode and the main menu's labels yield to it (#135)")
