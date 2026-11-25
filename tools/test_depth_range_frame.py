#!/usr/bin/env python3
"""Every depth range the frame draws with comes from the frame's range (#552).

At r_mirroralpha < 1 R_Clear (engine/hexen2/gl_rmain.c) splits window Z: the
view draws in one half ([0.5, 1] under reversed-Z, [0, 0.5] forward) and
R_Mirror's reflection in the other.  The split lives in gldepthmin /
gldepthmax and in the pipeline shadow R_GetDepthRange reads back.  A range
spelled in window-Z constants ignores it: Sky_DrawSkyBox pinned the box to
(0, 0) / (1, 1) and then "restored" (0, 1), so every entity drawn after the
sky wrote depth across the whole window range, into the reflection's half.

This holds the invariant across engine/: every argument of every
R_SetDepthRange call names the frame's range -- gldepthmin, gldepthmax, a
variable R_GetDepthRange filled (in that file), or a local assigned from one
of those in the same function.  An argument of constants and other names only
is a window-Z constant and fails; each branch of a ?: is checked on its own,
so "gl_clipcontrol_able ? znear : 1.0" fails too.  Outside gl_pipeline.c and
rg_null.c nothing calls glDepthRange directly.

Exempt: gl_pipeline.c (the implementation) and SH_PassState in gl_shadow.c,
which draws into the shadow map's own framebuffer, where window Z is the
shadow map's, between SH_SaveState and SH_RestoreState.

  python3 tools/test_depth_range_frame.py            # the tree
  python3 tools/test_depth_range_frame.py FILE...    # just these files
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
EXEMPT_FILES = {"gl_pipeline.c"}
EXEMPT_FUNCS = {("gl_shadow.c", "SH_PassState")}
SOURCES = {"gldepthmin", "gldepthmax"}
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\s*(?:->|\.)\s*[A-Za-z_][A-Za-z0-9_]*)*")
GETTER = re.compile(r"\bR_GetDepthRange\s*\(\s*&\s*([^,]+?)\s*,\s*&\s*([^)]+?)\s*\)")


def strip(text):
    """Comments, string/char literals and #if 0 blocks out; newlines kept so
    line numbers survive."""
    keep_nl = lambda m: "\n" * m.group(0).count("\n")
    text = re.sub(r"/\*.*?\*/", keep_nl, text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", text)
    out, depth = [], 0
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
    """(name, start offset, text) of every top-level { ... }."""
    depth, start = 0, None
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}" and depth:
            depth -= 1
            if depth == 0:
                head = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*$", code[:start])
                yield (head[-1] if head else "?"), start, code[start:i + 1]


def calls(body, name):
    """(offset, [args]) of every name ( ... ) call, nested parens allowed."""
    for m in re.finditer(r"\b" + name + r"\s*\(", body):
        i, depth, args, cur = m.end(), 1, [], ""
        while i < len(body) and depth:
            c = body[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if not depth:
                    break
            if c == "," and depth == 1:
                args.append(cur)
                cur = ""
            else:
                cur += c
            i += 1
        args.append(cur)
        yield m.start(), [a.strip() for a in args]


def norm(s):
    return re.sub(r"\s+", "", s)


def names(expr):
    return {norm(n) for n in IDENT.findall(expr)}


def branches(expr):
    """The values a top-level ?: can take; the condition is not one."""
    depth, q = 0, None
    for i, c in enumerate(expr):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "?" and depth == 0 and q is None:
            q = i
        elif c == ":" and depth == 0 and q is not None:
            return branches(expr[q + 1:i]) + branches(expr[i + 1:])
    return [expr.strip()]


def check(path, rel):
    errors = []
    code = strip(path.read_text(errors="replace"))
    # Struct members R_GetDepthRange fills (s->znear) carry across functions:
    # SH_SaveState fills what SH_RestoreState puts back.  Plain names do not.
    members = set()
    for m in GETTER.finditer(code):
        members |= {n for n in (norm(m.group(1)), norm(m.group(2))) if "->" in n or "." in n}
    if path.name not in ("gl_pipeline.c", "rg_null.c"):
        for m in re.finditer(r"\bglDepthRange(?:_fp|f)?\s*\(", code):
            line = code.count("\n", 0, m.start()) + 1
            errors.append(f"{rel}:{line}: glDepthRange called directly; go through "
                          "R_SetDepthRange so the pipeline shadow (and -gpu) follow it")
    for func, off, body in bodies(code):
        if (path.name, func) in EXEMPT_FUNCS:
            continue
        ok = SOURCES | members
        for m in GETTER.finditer(body):
            ok |= {norm(m.group(1)), norm(m.group(2))}
        # locals derived from the range: x = <expr naming a source>;
        for _ in range(3):
            for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=)\s*([^;]+);", body):
                if all(names(b) & ok for b in branches(m.group(2))):
                    ok.add(m.group(1))
        for at, args in calls(body, "R_SetDepthRange"):
            line = code.count("\n", 0, off + at) + 1
            bad = [b for a in args for b in branches(a) if not names(b) & ok]
            if bad:
                errors.append(f"{rel}:{line}: {func}: R_SetDepthRange ({', '.join(args)}): "
                              f"{', '.join(repr(b) for b in bad)} is a window-Z constant, "
                              "not the frame's range (gldepthmin/gldepthmax or R_GetDepthRange)")
    return errors


def main(argv):
    if argv:
        files = [(Path(a), a) for a in argv]
    else:
        files = [(p, str(p.relative_to(ROOT))) for p in sorted((ROOT / "engine").rglob("*.c"))]
    files = [(p, r) for p, r in files if p.name not in EXEMPT_FILES]
    errors, seen = [], 0
    for path, rel in files:
        text = strip(path.read_text(errors="replace"))
        seen += len(list(calls(text, "R_SetDepthRange")))
        errors += check(path, rel)
    # The pin must see the calls it is about, or it proves nothing.
    if not argv:
        sky = strip((ROOT / "engine/h2shared/gl_sky.c").read_text())
        if len(list(calls(sky, "R_SetDepthRange"))) < 2:
            errors.append("gl_sky.c lost its R_SetDepthRange pair: the skybox moved, re-aim this pin")
    for e in errors:
        print(f"FAIL: {e}")
    if errors:
        return 1
    print(f"ok: every R_SetDepthRange names the frame's range ({seen} calls)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
