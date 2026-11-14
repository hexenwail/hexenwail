#!/usr/bin/env python3
"""The shader include expander and its WebGL-smoke mirror agree (#450).

engine/cmake/EmbedShaders.cmake expands `#include "x.inc"` textually, one
distinct include name per pass, for a bounded number of passes.
scripts/webgl-engine-shader-smoke.mjs re-implements the same expansion so it
can compile what the build compiles; its own comment says matching the build
"matters more than doing includes properly".  If the two bounds drift, a stage
with more distinct includes than the smaller bound builds in one and fails (or
silently differs) in the other.  Both were raised together from 9 to 32 passes
for the alias family's split (#450).

Also checked, against every engine/shaders file:
  * no stage needs more distinct includes (transitively) than the bound;
  * no .inc or .glsl comment contains `out vec4 fragColor;`.
    GL_CompileOITFragShaderSplit (gl_shader.c) finds that declaration with
    strstr and splices the OIT outputs in its place; a comment carrying the
    same text earlier in the expanded body would be cut instead.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []

cmake = (ROOT / "engine/cmake/EmbedShaders.cmake").read_text()
mjs = (ROOT / "scripts/webgl-engine-shader-smoke.mjs").read_text()
m1 = re.search(r"foreach\(pass RANGE (\d+)\)", cmake)
m2 = re.search(r"for \(let pass = 0; pass <= (\d+); pass \+= 1\)", mjs)
if not m1 or not m2:
    errors.append("could not find the include bound in EmbedShaders.cmake or the smoke script")
else:
    bound = int(m1.group(1)) + 1
    if int(m1.group(1)) != int(m2.group(1)):
        errors.append(f"EmbedShaders.cmake allows {bound} include passes, "
                      f"the WebGL smoke {int(m2.group(1)) + 1}")

    def includes_of(text, seen):
        for name in re.findall(r'#include "([^"]+)"', text):
            if name not in seen:
                seen.add(name)
                path = SH / name
                if path.exists():
                    includes_of(path.read_text(), seen)
        return seen

    for stage in sorted(SH.glob("*.glsl")):
        n = len(includes_of(stage.read_text(), set()))
        if n > bound:
            errors.append(f"{stage.name} pulls in {n} distinct includes, over the bound of {bound}")

for f in sorted(list(SH.glob("*.inc")) + list(SH.glob("*.glsl"))):
    comments = re.findall(r"/\*.*?\*/|//[^\n]*", f.read_text(), re.S)
    if any("out vec4 fragColor;" in c for c in comments):
        errors.append(f"{f.name}: a comment contains `out vec4 fragColor;`, "
                      "which the OIT splice would cut instead of the declaration")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS shader include bound {bound} matches in the build and the WebGL smoke (#450)")
