#!/usr/bin/env python3
"""Compile every engine/shaders/*.glsl with glslangValidator.

Three frontends: the two GL dialects the engine ships (desktop 4.30 core and
GLSL ES 3.00), and a Vulkan 1.0 SPIR-V target that nothing consumes yet but
that the SDL_GPU backend will (uhexen2-p4ln.5).  The Vulkan pass is what proves
the descriptor annotations in uniforms.inc are complete: Vulkan rejects a
uniform block or sampler with no explicit binding, so a shader that grows a
resource and forgets to qualify it fails here rather than in the backend.

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
    # Not a tier the engine builds -- this is the SPIR-V frontend.  450 core
    # because SDL_GPU's Vulkan backend needs a version that knows descriptor
    # sets, and 450 is what its own examples are authored against.
    "vulkan": {
        "vert": "#version 450 core\n",
        "frag": "#version 450 core\n",
        "early_z_opaque": "layout(early_fragment_tests) in;\n",
    },
}

# Programs the ES tier never compiles.  gl_shader.c skips them behind
# #ifndef USE_GLES; both read storage buffers, which GLSL ES 3.00 has not got.
ES_SKIP = ("sskeletal_vert", "spart_gpu_vert", "salias_inst_vert")

# gl_shader.c passes this prefix only to the opaque world fragment program.
EARLY_Z_OPAQUE = ("sworld_frag_opaque",)

# GL_CompileOITFragShader wraps a fragment body in this to write the WBOIT
# accumulation and revealage targets instead of a colour.  Kept in step with the
# copy in gl_shader.c by hand -- it is eleven lines and it changes about never,
# and the alternative is parsing C from a checker.  Desktop only: OIT needs
# glBlendFunci, which WebGL2 has not got.
OIT_PREAMBLE = """#define OIT 1
vec4 fragColor = vec4(0.0);
layout(location=0) out vec4 out_accum;
layout(location=1) out vec4 out_reveal;
void main_body();
void main() {
    main_body();
    fragColor = clamp(fragColor, 0.0, 1.0);
    float z = 1.0 / gl_FragCoord.w;
    float w = clamp(fragColor.a * fragColor.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
    out_accum = vec4(fragColor.rgb * fragColor.a * w, fragColor.a * w);
    out_reveal = vec4(fragColor.a, 0.0, 0.0, 0.0);
}
#define main main_body
"""

OIT_VARIANTS = ("sworld_frag", "salias_frag", "spart_frag")

# The programs GL_Shaders_Init actually links.  Compiling stages one at a time
# cannot catch a varying whose location disagrees between them, and three of
# these fragment stages are shared by more than one vertex stage, so the pairs
# are checked as pairs.
PROGRAMS = (
    ("s2d_vert", "s2d_frag"),
    ("sflat_vert", "sflat_frag"),
    ("sworld_vert", "sworld_frag"),
    ("sworld_vert", "sworld_frag_opaque"),
    ("salias_vert", "salias_frag"),
    ("sskeletal_vert", "salias_frag"),
    ("spart_vert", "spart_frag"),
    ("ssky_vert", "ssky_frag"),
    ("spart_gpu_vert", "spart_frag"),
    ("salias_inst_vert", "salias_frag"),
)


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
            cmd = ["glslangValidator"]
            if dialect == "vulkan":
                # No -DVULKAN: glslang predefines it as 100 whenever the
                # target is Vulkan, and defining it again is a redefinition
                # error rather than a no-op.
                cmd += ["-V", "--target-env", "vulkan1.0", "-o", out + ".spv"]
            cmd.append(out)
            r = subprocess.run(cmd, capture_output=True, text=True)
            checked += 1
            if r.returncode != 0:
                failures += 1
                print("FAIL %s %s" % (dialect, name))
                print(r.stdout.strip())
    # The OIT fragment variants.  These are built in C, not from a file of
    # their own, so nothing above covers them -- and the body has to keep its
    # `#ifndef OIT` guard around fragColor for them to compile at all.
    for name in OIT_VARIANTS:
        text = (DIALECTS["core"]["frag"] + OIT_PREAMBLE
                + expand(os.path.join(SHADERS, name + ".glsl")))
        out = os.path.join(workdir, "oit_%s.frag" % name)
        open(out, "w").write(text)
        r = subprocess.run(["glslangValidator", out],
                           capture_output=True, text=True)
        checked += 1
        if r.returncode != 0:
            failures += 1
            print("FAIL oit %s" % name)
            print(r.stdout.strip())

    # Link the Vulkan stages in pairs.  Location mismatches only show up here.
    for vert, frag in PROGRAMS:
        v = os.path.join(workdir, "vulkan_%s.vert" % vert)
        f = os.path.join(workdir, "vulkan_%s.frag" % frag)
        r = subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0",
                            "-o", os.path.join(workdir, "prog.spv"), v, f],
                           capture_output=True, text=True)
        checked += 1
        if r.returncode != 0:
            failures += 1
            print("FAIL link %s + %s" % (vert, frag))
            print(r.stdout.strip())

    if not keep:
        shutil.rmtree(workdir, ignore_errors=True)

    print("%d shader compiles, %d failed" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
