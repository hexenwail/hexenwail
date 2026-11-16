#!/usr/bin/env python3
"""No GL program takes a loose value uniform any more (#19).

Every value a shader reads arrives through a std140 block (VertParams,
FragParams, the per-family blocks, CullParams and the rest; RU_BINDING_* in
gl_uniforms.h).  Loose uniforms are per-program state, block members are
global, and Vulkan forbids default-block uniforms outright, so one loose
glUniform4f slipped back in is both the per-program drift that got the first
block conversion reverted and a shader SDL_GPU cannot take.  Checked at source
level:
  - no engine C file calls a glUniform* entry point other than glUniform1i
    (binding a sampler to its texture unit) and glUniformBlockBinding;
  - gl_func.h declares and loads no other glUniform* entry point, so a new
    call site fails to compile instead of drawing with a stale value;
  - gl_vidsdl.c's capability gate does not demand an entry point nothing uses.
The WEBSOFT canvas blit (web_canvas_gl2.c) is outside the renderer: it calls
GLES directly through <GLES3/gl3.h>, never through gl_func.h, and keeps its
own loose uniforms.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ENGINE_DIRS = ["engine/hexen2", "engine/h2shared", "engine/hexenworld"]
EXEMPT = {"engine/h2shared/web_canvas_gl2.c"}
ALLOWED = {"glUniform1i", "glUniformBlockBinding"}
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


CALL = re.compile(r"\b(glUniform(?:[1-4][fiu]v?|Matrix[234](?:x[234])?fv|BlockBinding))(_fp)?\s*\(")

files = []
for d in ENGINE_DIRS:
    for p in sorted((ROOT / d).rglob("*")):
        if p.suffix in (".c", ".h"):
            files.append(p)
if not files:
    errors.append("no engine sources found")

for p in files:
    rel = p.relative_to(ROOT).as_posix()
    if rel in EXEMPT:
        continue
    text = strip(p.read_text(errors="replace"))
    for m in CALL.finditer(text):
        name = m.group(1)
        if name in ALLOWED:
            continue
        line = text.count("\n", 0, m.start()) + 1
        errors.append(f"{rel}:{line}: loose uniform call {m.group(0).rstrip('( ')} "
                      "(put the value in a std140 block, #19)")

func_h = strip((ROOT / "engine/h2shared/gl_func.h").read_text())
for m in re.finditer(r"GL_FUNCTION(?:_OPT)?\s*\([^,]+,\s*(glUniform\w*)\s*,", func_h):
    if m.group(1) not in ALLOWED:
        errors.append(f"gl_func.h declares {m.group(1)}; nothing may call it (#19)")
for m in re.finditer(r"#\s*define\s+(glUniform\w*)_fp\b", func_h):
    if m.group(1) not in ALLOWED:
        errors.append(f"gl_func.h defines {m.group(1)}_fp for the GLES tier; nothing may call it (#19)")

vidsdl = strip((ROOT / "engine/h2shared/gl_vidsdl.c").read_text())
for m in re.finditer(r"(glUniform\w*)_fp\s*=\s*\(", vidsdl):
    if m.group(1) not in ALLOWED:
        errors.append(f"gl_vidsdl.c loads {m.group(1)}; nothing may call it (#19)")
gate = re.search(r"if\s*\(([^{}]*glCreateShader_fp[^{}]*)\)\s*\{\s*Sys_Error", vidsdl)
if not gate:
    errors.append("gl_vidsdl.c: shader capability gate not found")
else:
    for name in re.findall(r"!\s*(glUniform\w*)_fp", gate.group(1)):
        if name not in ALLOWED:
            errors.append(f"gl_vidsdl.c's capability gate demands {name}, which nothing calls (#19)")

if errors:
    print("FAIL test_no_loose_uniforms:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print(f"ok: {len(files)} engine sources, no loose value uniforms outside the WEBSOFT blit")
