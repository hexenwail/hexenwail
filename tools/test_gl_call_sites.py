#!/usr/bin/env python3
"""No GL calls outside the renderer modules (GitHub #453, uhexen2-ibnq.8).

Every draw-affecting GL call lives in engine/h2shared/gl_*.c or
engine/hexen2/gl_*.c, through the gl_func.h loader table (_fp names).
That is what a second backend (SDL_GPU, #447) needs: one place that
knows the whole draw state (gl_pipeline.h:20).  Renderer code calls
glFoo_fp and never glFoo -- commit "fix(gl): route the last raw GL
calls through the loader table" established that contract; this gate
enforces it, plus the location rule.

Scope: engine/**/*.c only, NOT .h -- gl_func.h defines every _fp name
(the loader table itself) and glquake.h holds the renderer's own
GL_Bind macro, so scanning headers would flag the contract rather
than its violations.  Comments are stripped before matching, so a
/* glClear() ... */ note in a non-renderer file is not a violation.

One allowlist entry: engine/h2shared/web_canvas_gl2.c, the software
renderer's WebGL2 presentation canvas.  It calls raw GLES directly
because it never includes gl_func.h and the WEBSOFT build has no
loader table; routing it through _fp would drag the GL loader into a
software-renderer translation unit for no benefit.

Limitations, both accepted: string literals are not stripped (shader
strings in .c use gl_ builtins, not gl[A-Z], so nothing matches today),
and #if 0 blocks are not stripped (the known ones use _fp names but
sit in allowed files, so they are exempt anyway).
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

# *.c files under these prefixes are the renderer modules.
ALLOWED_DIRS = ("engine/h2shared/gl_", "engine/hexen2/gl_")
# The software renderer's WebGL2 presentation canvas: raw GLES, no
# loader table; platform glue, not renderer code.  See the docstring.
ALLOWLIST = {"engine/h2shared/web_canvas_gl2.c"}

FP_RE = re.compile(r"\bgl[A-Z][A-Za-z0-9]*_fp\b")
RAW_RE = re.compile(r"\bgl[A-Z][A-Za-z0-9]*\s*\(")
COMMENT_RE = re.compile(r"//.*?$|/\*.*?\*/", re.MULTILINE | re.DOTALL)


def strip_comments(text):
    """Remove // and /* */ comments, keeping newlines so line numbers hold."""
    return COMMENT_RE.sub(
        lambda m: "".join("\n" if ch == "\n" else " " for ch in m.group(0)),
        text,
    )


def main():
    errors = []
    for path in sorted((ROOT / "engine").rglob("*.c")):
        rel = str(path.relative_to(ROOT))
        if rel in ALLOWLIST:
            continue
        if rel.startswith(ALLOWED_DIRS):
            continue
        text = strip_comments(path.read_text())
        for lineno, line in enumerate(text.split("\n"), 1):
            for m in FP_RE.finditer(line):
                errors.append(f"{rel}:{lineno}: {m.group(0)}")
            for m in RAW_RE.finditer(line):
                errors.append(f"{rel}:{lineno}: {m.group(0)}")

    if errors:
        sys.exit("FAIL:\n  " + "\n  ".join(errors))
    print("PASS no GL calls outside the renderer modules (#453)")


if __name__ == "__main__":
    main()
