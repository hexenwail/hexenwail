#!/usr/bin/env python3
"""Under -gpu, gl_max_size is the SDL_GPU device's limit, never the null GL
table's placeholder (GitHub #513).

gl_max_size caps every texture upload (gl_draw.c, img_load.c, gl_rmisc.c)
and decides whether the lightmap atlas fits (gl_rsurf.c).  Under -gpu every
GL entry point is a counting no-op from rg_null.c, whose GL_MAX_TEXTURE_SIZE
answer is a fixed 16384, so reading it there handed a 4096 Vulkan part
uploads it cannot create.  SDL 3.4 has no query for the limit, so
RG_MaxTextureSize (rg_device.c) returns the floor each backend's
requirements guarantee, and on Vulkan the device's own maxImageDimension2D,
asked through an instance of the engine's (rg_vk_max2d, #340 P5f: at the
4096 floor Storm over Thyrion's courtyard lost the lightmap atlas, and with
it GPU lightstyles and the world cull).  Checked, stock python3 plus a C
compiler (lint.yml):

  * GL_Init dispatches to GL_Init_NullGL under R_GPU () and returns, so
    the GL arm's GL_MAX_TEXTURE_SIZE query never runs under -gpu;
  * GL_Init_NullGL sets gl_max_size from RG_MaxTextureSize () and makes
    no GL_MAX_TEXTURE_SIZE query of its own;
  * GL_Init_NullGL writes gl_max_size once, that assignment, and nothing
    else in the engine's .c, .h or .m files writes it (a plain or compound
    assignment, an increment, or its address handed out, even read-only:
    the check cannot tell), so no later query can put the null table's
    value back;
  * RG_MaxTextureSize, lifted and run against each SDL_GPU driver name and
    Vulkan answer: direct3d12 and metal 16384; vulkan the device's limit,
    4096 when it was not asked or answered below 4096, never above
    SDL_CreateGPUTexture's own 16384; anything else 4096; never below
    GL_Init's 256 floor.

    test_rg_max_size.py            check, exit 1 on any failure
    test_rg_max_size.py --root D   check the tree at D
"""

from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
errors = []

# What each SDL_GPU driver must get.  SDL_GetGPUDeviceDriver's names
# (SDL_gpu_vulkan.c, SDL_gpu_d3d12.c, SDL_gpu_metal.m), plus RG_DriverName's
# "none" and "webgpu", standing in for any driver name the function does not
# know.
# (driver, rg_vk_max2d, what RG_MaxTextureSize must answer)
EXPECT = (
    ("direct3d12", 0, 16384),
    ("metal", 0, 16384),
    ("metal", 8192, 16384),
    ("vulkan", 0, 4096),		# not asked, or the asking failed
    ("vulkan", 2048, 4096),		# below the spec's floor: not believed
    ("vulkan", 4096, 4096),
    ("vulkan", 8192, 8192),
    ("vulkan", 16384, 16384),
    ("vulkan", 32768, 16384),		# SDL's own cap
    ("none", 32768, 4096),
    ("webgpu", 32768, 4096),
)
SDL_MAX_2D = 16384	# SDL_gpu.c SDL_CreateGPUTexture, MAX_2D_DIMENSION
GL_FLOOR = 256		# gl_vidsdl.c GL_Init refuses less


def fail(msg):
    errors.append(msg)


def strip_c(text, strings=True):
    """Comments (and, with `strings`, string literals) blanked, newlines
    kept."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)
    # char literals first, so '"' cannot open a string that swallows code
    text = re.sub(r"'(?:\\.|[^'\\\n])'", "' '", text)
    if not strings:
        return text
    return re.sub(r'"(?:\\.|[^"\\\n])*"', lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', text)


def match_close(text, i):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def function(code, name):
    """(start, body) of the top-level definition of `name`, or (-1, "")."""
    m = re.search(r"^(?:[A-Za-z_][\w \t\*]*?[\s\*])?%s\s*\([^;{}]*\)\s*\{" % re.escape(name), code, re.M)
    if not m:
        return -1, ""
    i = m.end() - 1
    j = match_close(code, i)
    return m.start(), code[m.start():j] if j > 0 else ""


def enclosing(code, pos):
    """Name of the top-level function whose body holds `pos`, or None."""
    best = None
    for m in re.finditer(r"^(?:[A-Za-z_][\w \t\*]*?[\s\*])?(\w+)\s*\([^;{}]*\)\s*\{", code, re.M):
        if m.start() > pos:
            break
        j = match_close(code, m.end() - 1)
        if j > pos:
            best = m.group(1)
    return best


def check_dispatch(vidsdl):
    _, body = function(vidsdl, "GL_Init")
    if not body:
        fail("gl_vidsdl.c: no GL_Init")
        return
    m = re.search(r"if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*\{\s*GL_Init_NullGL\s*\(\s*\)\s*;\s*return\s*;\s*\}", body)
    q = re.search(r"\bGL_MAX_TEXTURE_SIZE\b", body)
    if not m:
        fail("gl_vidsdl.c GL_Init: no `if (R_GPU()) { GL_Init_NullGL(); return; }` ahead of the GL arm")
    elif q and q.start() < m.start():
        fail("gl_vidsdl.c GL_Init: GL_MAX_TEXTURE_SIZE is queried before the R_GPU () dispatch")


def check_nullgl(vidsdl):
    _, body = function(vidsdl, "GL_Init_NullGL")
    if not body:
        fail("gl_vidsdl.c: no GL_Init_NullGL")
        return
    if re.search(r"\bGL_MAX_TEXTURE_SIZE\b|\b0[xX]0*[dD]33\b", body):
        fail("gl_vidsdl.c GL_Init_NullGL queries GL_MAX_TEXTURE_SIZE: under -gpu that is "
             "rg_null.c's placeholder, not the device's limit (#513)")
    writes = list(WRITE_RX.finditer(body))
    ok = re.compile(r"\bgl_max_size\s*=\s*RG_MaxTextureSize\s*\(\s*\)\s*;")
    if len(writes) != 1 or not ok.match(body, writes[0].start()):
        fail("gl_vidsdl.c GL_Init_NullGL: its one write of gl_max_size must be "
             "`gl_max_size = RG_MaxTextureSize ();` (found %d writes)" % len(writes))


WRITE_RX = re.compile(r"&\s*gl_max_size\b|\bgl_max_size\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|"
                      r"(?:\+\+|--)\s*gl_max_size\b|\bgl_max_size\s*(?:\+\+|--)")


def check_writers(root):
    """Every write of gl_max_size is GL_Init's query or GL_Init_NullGL's
    assignment."""
    seen = set()
    paths = [q for ext in ("*.c", "*.h", "*.m") for q in (root / "engine").rglob(ext)]
    for path in sorted(paths):
        # engine/rust is another language; engine/tests are harnesses that
        # stub and set engine globals, never linked into the engine.
        if path.relative_to(root / "engine").parts[0] in ("rust", "tests"):
            continue
        code = strip_c(path.read_text(errors="replace"))
        for m in WRITE_RX.finditer(code):
            fn = enclosing(code, m.start())
            rel = path.relative_to(root)
            line = code.count("\n", 0, m.start()) + 1
            where = "%s:%d (%s)" % (rel, line, fn or "file scope")
            if fn is None and re.match(r"GLint\s+gl_max_size\s*=", code[code.rfind("\n", 0, m.start()) + 1:].lstrip()):
                continue	# the definition's initializer, before any GL_Init
            if path.name == "gl_vidsdl.c" and fn in ("GL_Init", "GL_Init_NullGL"):
                seen.add(fn)
                continue
            fail("%s writes gl_max_size: under -gpu only GL_Init_NullGL may, from "
                 "RG_MaxTextureSize (#513)" % where)
    for fn in ("GL_Init", "GL_Init_NullGL"):
        if fn not in seen:
            fail("gl_vidsdl.c %s no longer writes gl_max_size; this test's writer list is stale" % fn)


def check_limits(root):
    dev = strip_c((root / "engine/h2shared/rg_device.c").read_text(), strings=False)
    _, body = function(dev, "RG_MaxTextureSize")
    if not body:
        fail("rg_device.c: no RG_MaxTextureSize")
        return
    hdr = (root / "engine/h2shared/rg_device.h").read_text()
    if not re.search(r"\bint\s+RG_MaxTextureSize\s*\(\s*void\s*\)\s*;", hdr):
        fail("rg_device.h does not declare int RG_MaxTextureSize (void)")
    cc = os.environ.get("CC", "cc")
    src = ["#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
           "static const char *drv;",
           "static int rg_vk_max2d;",
           "static const char *RG_DriverName (void) { return drv; }",
           body,
           "int main (int argc, char **argv) {",
           "  int i; for (i = 1; i + 1 < argc; i += 2) { drv = argv[i]; rg_vk_max2d = atoi (argv[i + 1]);"
           " printf (\"%s %d %d\\n\", drv, rg_vk_max2d, RG_MaxTextureSize ()); }",
           "  return 0; }"]
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "maxsize.c"
        exe = Path(tmp) / "maxsize"
        c.write_text("\n".join(src) + "\n")
        r = subprocess.run([cc, "-std=c99", "-Wall", "-Werror", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            fail("RG_MaxTextureSize does not compile lifted:\n" + r.stderr)
            return
        args = [x for d, m, _ in EXPECT for x in (d, str(m))]
        out = subprocess.run([str(exe)] + args, capture_output=True, text=True,
                             check=True).stdout.split("\n")
    got = {(d, int(m)): int(v) for d, m, v in (l.split() for l in out if l.strip())}
    for name, max2d, want in EXPECT:
        v = got.get((name, max2d), -1)
        if v != want:
            fail("RG_MaxTextureSize: driver %r with the device at %d gives %d, want %d" % (name, max2d, v, want))
        if not GL_FLOOR <= v <= SDL_MAX_2D:
            fail("RG_MaxTextureSize: driver %r gives %d, outside %d..%d" % (name, v, GL_FLOOR, SDL_MAX_2D))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    args = ap.parse_args()
    vidsdl = strip_c((args.root / "engine/h2shared/gl_vidsdl.c").read_text())
    check_dispatch(vidsdl)
    check_nullgl(vidsdl)
    check_writers(args.root)
    check_limits(args.root)
    if errors:
        for e in errors:
            print("FAIL: " + e)
        sys.exit(1)
    print("test_rg_max_size: OK, -gpu takes gl_max_size from RG_MaxTextureSize "
          "(%s)" % ", ".join("%s@%d %d" % e for e in EXPECT))


if __name__ == "__main__":
    main()
