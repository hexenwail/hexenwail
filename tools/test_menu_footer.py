#!/usr/bin/env python3
"""The main menu's footer never overprints (GitHub #441).

The renderer tag (bottom-left, M_Main_Draw) and the version watermark
(bottom-right, Draw_ConsoleVersionInfo) share one baseline.  On the software
renderer's 320-wide framebuffer the watermark is wider than the screen, ran
off the left edge, and printed over the tag.  Held: the watermark's x is
clamped to the left margin in both draw paths, and the tag is drawn only
when it ends before the watermark begins.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
for path in ("engine/h2shared/draw.c", "engine/h2shared/gl_draw.c"):
    src = (ROOT / path).read_text()
    fn = src[src.index("static void Draw_ConsoleVersionInfo (int lines)\n{"):]
    fn = fn[:fn.index("\n}\n")]
    if "if (x < 11)\n\t\tx = 11;" not in fn:
        errors.append(f"{path}: the watermark must not start left of the margin")
menu = (ROOT / "engine/hexen2/menu.c").read_text()
if "11 + (int)strlen (r) * 8 + 8 <= wm_x" not in menu:
    errors.append("menu.c: the renderer tag must yield when it would reach the watermark")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS main-menu footer: tag and watermark never overprint (#441)")
