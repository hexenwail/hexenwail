#!/usr/bin/env python3
"""Every pass that changes the blend func puts the resting pair back (#547).

The engine's resting blend func is GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
(gl_vidsdl.c sets it at context init), and blended draws that set no func of
their own inherit it: the sky's front layer at r_skyalpha < 1 (R_DrawSkyChain)
is one.  Every pass that switches to another pair -- the additive glows,
gl_rlight.c's flashblends, the dlight passes -- switches back before it
returns.  The bloom upsample in GL_PostProcess_EndFrame did not: it left
GL_ONE, GL_ONE behind, so one frame of r_bloom 1 made demo1's sky additive
(mean 0.26 -> 0.49) until restart, r_bloom 0 or not.

This holds the invariant across engine/: in each top-level function body, any
R_SetBlendFunc to a pair other than the resting one must be followed, later
in the same body, by R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
R_SetBlendFuncIndexed (OIT's per-target funcs) is a separate shadow and not
checked here; gl_pipeline.c is the implementation, not a caller.

  python3 tools/test_blend_func_restore.py            # the tree
  python3 tools/test_blend_func_restore.py FILE...    # just these files
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
RESTING = ("GL_SRC_ALPHA", "GL_ONE_MINUS_SRC_ALPHA")
EXEMPT = {"engine/h2shared/gl_pipeline.c"}
CALL = re.compile(r"\bR_SetBlendFunc\s*\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)")


def strip(text):
    """Comments, string/char literals and #if 0 blocks out; newlines kept so
    line numbers survive."""
    keep_nl = lambda m: "\n" * m.group(0).count("\n")
    text = re.sub(r"/\*.*?\*/", keep_nl, text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", text)
    out, depth = [], 0	# depth > 0: inside an #if 0, counting nested #ifs
    for line in text.split("\n"):
        d = line.strip()
        if depth:
            if re.match(r"#\s*if", d):
                depth += 1
            elif re.match(r"#\s*endif", d):
                depth -= 1
            elif depth == 1 and re.match(r"#\s*(else|elif)", d):
                depth = 0
            out.append("")
            continue
        if re.match(r"#\s*if\s+0\b", d):
            depth = 1
            out.append("")
            continue
        out.append(line)
    return "\n".join(out)


def bodies(code):
    """(start offset, text) of every top-level { ... }."""
    depth, start = 0, None
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}" and depth:
            depth -= 1
            if depth == 0:
                yield start, code[start:i + 1]


def check(path, rel):
    errors = []
    code = strip(path.read_text(errors="replace"))
    for off, body in bodies(code):
        calls = list(CALL.finditer(body))
        for n, m in enumerate(calls):
            if (m.group(1), m.group(2)) == RESTING:
                continue
            if any((r.group(1), r.group(2)) == RESTING for r in calls[n + 1:]):
                continue
            line = code.count("\n", 0, off + m.start()) + 1
            errors.append(f"{rel}:{line}: R_SetBlendFunc ({m.group(1)}, {m.group(2)}) "
                          f"is never put back to ({', '.join(RESTING)}) in this function")
    return errors


def main(argv):
    if argv:
        files = [(Path(a), a) for a in argv]
    else:
        files = [(p, str(p.relative_to(ROOT))) for p in sorted((ROOT / "engine").rglob("*.c"))]
        files = [(p, r) for p, r in files if r not in EXEMPT]
    errors, scanned = [], 0
    for path, rel in files:
        scanned += 1
        errors += check(path, rel)
    # The pin must see the passes it is about, or it proves nothing.
    if not argv:
        pp = strip((ROOT / "engine/h2shared/gl_postprocess.c").read_text())
        if not CALL.search(pp):
            errors.append("gl_postprocess.c has no R_SetBlendFunc call: the bloom pass moved, re-aim this pin")
    for e in errors:
        print(f"FAIL: {e}")
    if errors:
        return 1
    print(f"ok: every non-resting R_SetBlendFunc is restored ({scanned} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
