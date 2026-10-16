#!/usr/bin/env python3
"""The two SCR_DrawDemoBar copies must stay the same code (GitHub #123).

The demo playback bar is drawn by engine/h2shared/gl_screen.c in the GL and
WebGL2 builds and by engine/h2shared/screen.c in the 8bpp software web build,
which swaps gl_screen.c out.  The software copy is the GL one line for line;
the 2D API it calls is republished by draw_soft_web.c.  Two copies of one
function drift, and the software config is the one nobody looks at, so this
compares their code with comments and blank lines removed and fails on any
difference except the ones listed in ALLOWED:

  - the software copy declares gw/gh at the top of the block (the GL copy
    declares them after a statement);
  - the software copy restores CANVAS_DEFAULT on the way out, because a
    software canvas is a translation of the drawing origin and screen.c draws
    everything after the bar in the default canvas.

Pure text, no compiler.  An optional pair of arguments compares other copies,
for showing the test fails on a drifted one.
"""
import difflib
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
GL = ROOT / "engine/h2shared/gl_screen.c"
SOFT = ROOT / "engine/h2shared/screen.c"

# (side, line) pairs the diff may contain: "-" only in gl_screen.c, "+" only
# in screen.c.  The gw/gh pair is one moved line, so it appears on both sides.
ALLOWED = {
    ("-", "int\tgw, gh;"),
    ("+", "int\tgw, gh;"),
    ("+", "GL_SetCanvas (CANVAS_DEFAULT);"),
}


def body(path):
    src = path.read_text()
    start = src.find("static void SCR_DrawDemoBar (void)")
    if start < 0:
        sys.exit("FAIL: no SCR_DrawDemoBar in %s" % path)
    end = src.find("\n}\n", start)
    if end < 0:
        sys.exit("FAIL: SCR_DrawDemoBar in %s has no closing brace" % path)
    code = src[start:end + 2]
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    return [l.strip() for l in code.split("\n") if l.strip()]


def main():
    gl = Path(sys.argv[1]) if len(sys.argv) > 2 else GL
    soft = Path(sys.argv[2]) if len(sys.argv) > 2 else SOFT
    a, b = body(gl), body(soft)
    bad = []
    # The layout constants live just above the function in both files.
    for name in ("DEMOBAR_CHARS", "DEMOBAR_FADE"):
        pat = re.compile(r"^#define\s+%s\s+(\S+)" % name, re.M)
        va, vb = pat.findall(gl.read_text()), pat.findall(soft.read_text())
        if len(va) != 1 or va != vb:
            bad.append("%s: gl_screen.c %s, screen.c %s" % (name, va, vb))
    for line in difflib.unified_diff(a, b, n=0, lineterm=""):
        if line.startswith(("---", "+++", "@@")):
            continue
        if (line[0], line[1:]) not in ALLOWED:
            bad.append(line)
    print("gl_screen.c %d lines, screen.c %d lines" % (len(a), len(b)))
    if bad:
        print("FAIL: the SCR_DrawDemoBar copies differ beyond the allowed lines:")
        for line in bad:
            print("  " + line)
        print("Change both copies together (GitHub #123).")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
