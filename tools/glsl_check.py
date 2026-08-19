#!/usr/bin/env python3
"""Compile every engine/shaders/*.glsl in both dialects with glslangValidator.

The engine builds these with the dialect header supplied from C, so a plain
`glslangValidator engine/shaders/foo.glsl` cannot work -- there is no #version
line in the file and no #include support in core GLSL.  This does what
engine/cmake/EmbedShaders.cmake plus gl_shader.c's GL_CompileStage do, writes
the result to a temp dir, and runs the validator over it.

Usage:  python3 tools/glsl_check.py [--keep DIR]

Needs glslangValidator on PATH; on this project's dev shell:
    nix-shell -p glslang --run 'python3 tools/glsl_check.py'
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

SHADERS = os.path.join("engine", "shaders")

# Mirrors the GLSL_*_HEADER macros in engine/h2shared/gl_shader.c.
DIALECTS = {
    "core": {
        "vert": "#version 430 core\n",
        "frag": "#version 430 core\n",
        "early_z_opaque": "layout(early_fragment_tests) in;\n",
    },
    "es": {
        "vert": "#version 300 es\nprecision highp float;\n",
        "frag": "#version 300 es\nprecision mediump float;\n",
        "early_z_opaque": "",
    },
}

# Programs the ES tier never compiles.  gl_shader.c skips them behind
# #ifndef USE_GLES; both read storage buffers, which GLSL ES 3.00 has not got.
ES_SKIP = ("sskeletal_vert", "spart_gpu_vert", "salias_inst_vert")

# gl_shader.c passes this prefix only to the opaque world fragment program.
EARLY_Z_OPAQUE = ("sworld_frag_opaque",)


def expand(path, depth=0):
    if depth > 8:
        sys.exit("cyclic include in " + path)
    text = open(path).read()
    while True:
        m = re.search(r'#include "([^"]+)"\n', text)
        if not m:
            return text
        inc = os.path.join(SHADERS, m.group(1))
        if not os.path.exists(inc):
            sys.exit("%s includes missing %s" % (path, m.group(1)))
        text = text[:m.start()] + expand(inc, depth + 1) + text[m.end():]


def main():
    keep = None
    if len(sys.argv) > 2 and sys.argv[1] == "--keep":
        keep = sys.argv[2]
        os.makedirs(keep, exist_ok=True)
    workdir = keep or tempfile.mkdtemp(prefix="glslcheck")

    if not shutil.which("glslangValidator"):
        sys.exit("glslangValidator not found on PATH")

    names = sorted(f[:-5] for f in os.listdir(SHADERS) if f.endswith(".glsl"))
    failures = 0
    checked = 0
    for dialect, hdr in DIALECTS.items():
        for name in names:
            if dialect == "es" and name in ES_SKIP:
                continue
            stage = "vert" if name.endswith("_vert") else "frag"
            text = hdr[stage]
            if name in EARLY_Z_OPAQUE:
                text += hdr["early_z_opaque"]
            text += expand(os.path.join(SHADERS, name + ".glsl"))

            out = os.path.join(workdir, "%s_%s.%s" % (dialect, name, stage))
            open(out, "w").write(text)
            r = subprocess.run(["glslangValidator", out],
                               capture_output=True, text=True)
            checked += 1
            if r.returncode != 0:
                failures += 1
                print("FAIL %s %s" % (dialect, name))
                print(r.stdout.strip())
    if not keep:
        shutil.rmtree(workdir, ignore_errors=True)

    print("%d shader compiles, %d failed" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
