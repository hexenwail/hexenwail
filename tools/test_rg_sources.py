#!/usr/bin/env python3
"""The SDL_GPU backend stays inside the desktop GL client (GitHub #340 P0).

engine/h2shared/rg_*.c is the SDL_GPU backend.  It must never reach the
GLES tier, the browser builds (WebGL2 and the WEBSOFT rasterizer) or the
dedicated servers: on WEBSOFT a stray reference is a link error, on GLES it
would pull SDL_GPU into a build that has no GL_DLSYM loader table for
rg_null.c to fill, and on h2ded/hwsv it would drag SDL into a server.

Checked, from the sources (stock python3, lint.yml):

  engine/CMakeLists.txt
    * rg_*.c is named only inside `if(USE_SDL_GPU)`, collected into
      RG_SOURCES, and RG_SOURCES feeds only the client's ALL_SOURCES;
    * USE_SDL_GPU is forced OFF for USE_GLES, EMSCRIPTEN, any build
      without GL_DLSYM and SDL older than 3.4, BEFORE anything tests it;
    * USE_SDL_GPU reaches the compiler only through CLIENT_DEFINITIONS --
      never add_definitions() / a -D flag, which would leak it into h2ded
      and hwsv.
  every other engine C source and header
    * each RG_* / rg_* identifier, R_GPU() and #include "rg_device.h" sits
      inside an `#ifdef USE_SDL_GPU` (or `#if defined(USE_SDL_GPU)`) region,
      comments excluded -- so with the option off, no translation unit names
      a symbol only rg_*.c defines.
  every engine C source
    * no SDL_GL_GetProcAddress load can run under -gpu: outside gl_vidsdl.c's
      own loaders each sits in an `if (!R_GPU())` block, GL_Init returns into
      GL_Init_NullGL before its first load, and the loaders are called from
      GL_Init alone.  -gpu loads no GL library, so a load writes NULL over a
      null-GL stub.
  engine/h2shared/rg_*.c / rg_*.h
    * each refuses to compile without USE_SDL_GPU (#error), so a CMake slip
      that compiled one anyway fails loudly instead of linking half a
      backend.

Usage: test_rg_sources.py [--root DIR]
"""
from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
errors = []


def fail(msg):
    errors.append(msg)


def strip_c_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] == '"':
            # keep string literals (an #include "rg_device.h" is one) but do
            # not let a quote inside a comment desync anything
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


GUARD = re.compile(r"#\s*(?:ifdef\s+USE_SDL_GPU\b|if\s+defined\s*\(\s*USE_SDL_GPU\s*\)\s*$)")
SYMBOL = re.compile(r"\b(?:RG_\w+|rg_\w+|R_GPU)\b|\"rg_device\.h\"")


def check_c(path, rel):
    """Every backend reference is under an #ifdef USE_SDL_GPU arm."""
    stack = []  # per open conditional: True if this arm is a USE_SDL_GPU arm
    for ln, line in enumerate(strip_c_comments(path.read_text(errors="replace")).splitlines(), 1):
        s = line.strip()
        d = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b", s)
        if d:
            kw = d.group(1)
            if kw in ("if", "ifdef", "ifndef"):
                stack.append(bool(GUARD.match(s)))
            elif kw in ("elif", "else"):
                if stack:
                    stack[-1] = False   # the other arm of `#ifdef USE_SDL_GPU`
            elif stack:
                stack.pop()
        for m in SYMBOL.finditer(line):
            tok = m.group(0)
            if tok.startswith("R_GPU") and re.match(r"R_GPU\w", line[m.start():m.start() + 6]):
                continue
            if not any(stack):
                fail("%s:%d: `%s` outside #ifdef USE_SDL_GPU -- builds without the SDL_GPU "
                     "backend (GLES, WEBSOFT, dedicated) would not compile or link" % (rel, ln, tok))


def functions(code):
    """[(name, open, close)] for each top-level function body in comment-
    stripped C: the name is the last identifier before the header's '('."""
    out, depth, i, n = [], 0, 0, len(code)
    start = None
    while i < n:
        c = code[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and code[j] != c and code[j] != "\n":
                j += 2 if code[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            if depth == 0:
                head = code[max(0, i - 400):i]
                m = re.search(r"(\w+)\s*\([^;{}]*\)\s*$", head)
                start = (m.group(1) if m else None, i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start:
                out.append((start[0], start[1], i))
                start = None
        i += 1
    return out


# The loaders SDL_GPU mode never reaches: GL_Init returns into GL_Init_NullGL
# before calling either of the first two, and before its own loads.
VIDSDL_LOADERS = ("GL_Init_Functions", "GL_LoadFunctionPointers", "GL_Init")
GPU_SKIP = re.compile(r"if\s*\(\s*!\s*R_GPU\s*\(\s*\)\s*\)\s*(?:#\s*endif\s*)?$")


def check_proc_loads(root):
    """No SDL_GL_GetProcAddress load runs under -gpu.  rg_null.c's stubs fill
    every gl_func.h pointer; a later load finds no GL library and writes NULL
    over one, which crashes on the first call far from the load (a stray
    reload in GL_PostProcess_Init did exactly that, #340 P0)."""
    vid = root / "engine/h2shared/gl_vidsdl.c"
    for p in sorted((root / "engine").rglob("*.c")):
        rel = p.relative_to(root)
        if re.match(r"rg_\w+\.c$", p.name) or ("rust" in rel.parts and "target" in rel.parts):
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        if "SDL_GL_GetProcAddress" not in code:
            continue
        funcs = functions(code)
        for m in re.finditer(r"\bSDL_GL_GetProcAddress\b", code):
            ln = code.count("\n", 0, m.start()) + 1
            fn = next((f for f in funcs if f[1] < m.start() < f[2]), None)
            if p == vid and fn and fn[0] in VIDSDL_LOADERS:
                continue
            # walk outwards through the enclosing blocks for an `if (!R_GPU())`
            ok, pos, depth = False, m.start(), 0
            while pos > (fn[1] if fn else 0):
                pos -= 1
                if code[pos] == "}":
                    depth += 1
                elif code[pos] == "{":
                    if depth:
                        depth -= 1
                    elif GPU_SKIP.search(code[max(0, pos - 200):pos].rstrip()):
                        ok = True
                        break
            if not ok:
                fail("%s:%d: SDL_GL_GetProcAddress outside an `if (!R_GPU())` block -- under -gpu "
                     "there is no GL library, and the NULL it returns overwrites rg_null.c's stub"
                     % (rel, ln))
    # GL_Init has to leave for GL_Init_NullGL before any load, and the two
    # loader functions have to be called from GL_Init alone.
    code = strip_c_comments(vid.read_text())
    funcs = {f[0]: f for f in functions(code)}
    gi = funcs.get("GL_Init")
    if not gi:
        fail("gl_vidsdl.c: no GL_Init")
        return
    body = code[gi[1]:gi[2]]
    arm = re.search(r"if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*\{\s*GL_Init_NullGL\s*\(\s*\)\s*;\s*return\s*;", body)
    first = min([body.find(s) for s in ("SDL_GL_GetProcAddress", "GL_Init_Functions",
                                          "GL_LoadFunctionPointers") if s in body] or [len(body)])
    if not arm or arm.start() > first:
        fail("gl_vidsdl.c: GL_Init does not return into GL_Init_NullGL under R_GPU() before its "
             "first GL load")
    for name in ("GL_Init_Functions", "GL_LoadFunctionPointers"):
        for m in re.finditer(r"\b%s\s*\(" % name, code):
            fn = next((f for f in funcs.values() if f[1] < m.start() < f[2]), None)
            if fn and fn[0] != "GL_Init":
                fail("gl_vidsdl.c:%d: %s called from %s, not GL_Init -- a path SDL_GPU mode "
                     "may reach" % (code.count("\n", 0, m.start()) + 1, name, fn[0]))


def cmake_blocks(text):
    """(line, kind, condition, depth-stack-of-conditions) for each line."""
    stack, out = [], []
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0]
        m = re.match(r"\s*(if|elseif|else|endif)\s*\((.*)\)\s*$", line, re.I)
        if m:
            kw, cond = m.group(1).lower(), " ".join(m.group(2).split())
            if kw == "if":
                stack.append(cond)
            elif kw in ("elseif", "else"):
                if stack:
                    stack[-1] = "NOT(%s)" % stack[-1] if kw == "else" else cond
            elif stack:
                stack.pop()
            out.append((ln, line, list(stack)))
            continue
        out.append((ln, line, list(stack)))
    return out


def check_cmake(root):
    path = root / "engine/CMakeLists.txt"
    text = path.read_text()
    rows = cmake_blocks(text)
    first_test = None
    force_line = None
    rg_named = 0
    for ln, line, stack in rows:
        if re.search(r"\brg_\w+\.c\b", line):
            rg_named += 1
            if not stack or stack[-1] != "USE_SDL_GPU":
                fail("CMakeLists.txt:%d: rg_*.c named outside `if(USE_SDL_GPU)`" % ln)
        if re.search(r"\bRG_SOURCES\b", line) and not re.search(r"set\s*\(\s*RG_SOURCES\b", line):
            if not re.match(r"\s*\$\{RG_SOURCES\}\s*$", line):
                fail("CMakeLists.txt:%d: RG_SOURCES used other than as a line of ALL_SOURCES" % ln)
        if re.search(r"add_definitions\s*\([^)]*USE_SDL_GPU|-DUSE_SDL_GPU\b|"
                     r"add_compile_definitions\s*\([^)]*USE_SDL_GPU", line):
            fail("CMakeLists.txt:%d: USE_SDL_GPU as a directory-wide definition; it must reach "
                 "only the client, through CLIENT_DEFINITIONS" % ln)
        if re.match(r"\s*if\s*\(\s*USE_SDL_GPU\s+AND\s*\(.*USE_GLES.*EMSCRIPTEN.*GL_DLSYM", line):
            force_line = ln
            # rg_device.c calls SDL_GetGPUDeviceProperties, new in SDL 3.4
            if not re.search(r"SDL3_VERSION\s+VERSION_LESS\s+3\.4", line):
                fail("CMakeLists.txt:%d: USE_SDL_GPU is not forced off below SDL 3.4" % ln)
        elif re.search(r"\bif\s*\(\s*USE_SDL_GPU\b", line) and first_test is None:
            first_test = ln
    if not rg_named:
        fail("CMakeLists.txt: no rg_*.c source listed -- the SDL_GPU backend is never built")
    if force_line is None:
        fail("CMakeLists.txt: no `if(USE_SDL_GPU AND (USE_GLES OR EMSCRIPTEN OR NOT \"GL_DLSYM\" "
             "IN_LIST CLIENT_DEFINITIONS))` forcing the option off")
    elif first_test is not None and first_test < force_line:
        fail("CMakeLists.txt:%d: USE_SDL_GPU tested before it is forced off for GLES/web (line %d)"
             % (first_test, force_line))
    else:
        nxt = [l for n, l, _ in rows if n == force_line + 1]
        if not nxt or not re.match(r"\s*set\s*\(\s*USE_SDL_GPU\s+OFF\s*\)", nxt[0]):
            fail("CMakeLists.txt:%d: the forcing `if` must be followed by set(USE_SDL_GPU OFF)"
                 % force_line)
    if not re.search(r"list\s*\(\s*APPEND\s+CLIENT_DEFINITIONS\s+USE_SDL_GPU\s*\)", text):
        fail("CMakeLists.txt: USE_SDL_GPU never appended to CLIENT_DEFINITIONS")
    # RG_SOURCES must be in the client's ALL_SOURCES and no server list
    m = re.search(r"set\s*\(\s*ALL_SOURCES\b(.*?)\)", text, re.S)
    if not m or "${RG_SOURCES}" not in m.group(1):
        fail("CMakeLists.txt: ${RG_SOURCES} missing from ALL_SOURCES")
    for lst in ("SERVER_SOURCES", "HWSV_SOURCES"):
        for mm in re.finditer(r"(?:set|list)\s*\(\s*(?:APPEND\s+)?%s\b(.*?)\)" % lst, text, re.S):
            if "RG_SOURCES" in mm.group(1) or re.search(r"\brg_\w+\.c", mm.group(1)):
                fail("CMakeLists.txt: %s carries the SDL_GPU backend" % lst)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    a = ap.parse_args(argv)
    root = a.root.resolve()
    eng = root / "engine"
    check_cmake(root)
    check_proc_loads(root)
    nfiles = 0
    rg_files = []
    for p in sorted(eng.rglob("*")):
        if p.suffix not in (".c", ".h") or not p.is_file():
            continue
        rel = p.relative_to(root)
        if "rust" in rel.parts and "target" in rel.parts:
            continue
        if re.match(r"rg_\w+\.[ch]$", p.name):
            rg_files.append(p)
            continue
        nfiles += 1
        check_c(p, rel)
    if not rg_files:
        fail("no engine/**/rg_*.c or rg_*.h found")
    for p in rg_files:
        code = strip_c_comments(p.read_text())
        if not re.search(r"#\s*ifndef\s+USE_SDL_GPU\s*\n\s*#\s*error\b", code):
            fail("%s: no `#ifndef USE_SDL_GPU / #error` guard" % p.relative_to(root))
    for e in errors:
        print("FAIL: " + e)
    if errors:
        print("test_rg_sources: %d failure(s)" % len(errors))
        return 1
    print("test_rg_sources: OK, %d backend files guarded, %d other engine sources reference it only "
          "under USE_SDL_GPU, CMake builds it into the desktop GL client alone" % (len(rg_files), nfiles))
    return 0


if __name__ == "__main__":
    sys.exit(main())
