#!/usr/bin/env python3
"""The bloom passes take their parameters through one BloomParams block (#450).

When the bloom family moved into engine/shaders/sbloom_*.glsl, its loose
u_rcpframe / u_threshold uniforms (illegal in Vulkan SPIR-V) became a
16-byte std140 block in bloom.inc.  This checks:

  * the block and its C mirror bloom_params_t agree;
  * the old names are only aliases into the block, in the right components;
  * every bloom stage includes bloom.inc and the inline literals are gone;
  * binding point 6 in gl_postprocess.c and in gl_shader.h's registry;
  * PP_PushBloomParams does NOT use the GL_Upload frame ring.  Bloom runs on
    the ES tier too, where GL_Upload is a stub returning buffer 0: the first
    cut of this slice did exactly that, the ES build read the block as zeros
    and bloomed every pixel, and the desktop build stayed pixel-identical, so
    only an ES run could see it.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
PP = ROOT / "engine/h2shared/gl_postprocess.c"
errors = []


def strip_comments(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


inc = (SH / "bloom.inc").read_text() if (SH / "bloom.inc").exists() else ""
blocks = re.findall(r"uniform\s+BloomParams\s*\{(.*?)\};", strip_comments(inc), re.S)
if len(blocks) != 1:
    errors.append(f"bloom.inc declares BloomParams {len(blocks)} times, expected 1")
members = re.findall(r"\b(vec4|ivec4|mat4)\s+(\w+)\s*;", blocks[0] if blocks else "")
if members != [("vec4", "u_bloomp")]:
    errors.append(f"BloomParams members are {members}, expected one vec4 u_bloomp")
for name, comp in (("u_rcpframe", "u_bloomp.xy"), ("u_threshold", "u_bloomp.z")):
    if not re.search(rf"#define\s+{name}\s+{re.escape(comp)}\b", inc):
        errors.append(f"{name} is not aliased to {comp}")

for stage in ("sbloom_bright_frag.glsl", "sbloom_down_frag.glsl", "sbloom_up_frag.glsl"):
    t = (SH / stage).read_text() if (SH / stage).exists() else ""
    if not re.search(r'^#include "bloom\.inc"\s*$', strip_comments(t), re.M):
        errors.append(f"{stage} does not #include \"bloom.inc\"")
    if re.search(r"uniform\s+(float|vec2)\s+u_", strip_comments(t)):
        errors.append(f"{stage} declares a loose uniform")
if not (SH / "sbloom_vert.glsl").exists():
    errors.append("engine/shaders/sbloom_vert.glsl is missing")

src = PP.read_text()
code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', strip_comments(src))
m = re.search(r"typedef struct \{\s*float\s+p\[4\];[^}]*\} bloom_params_t;", src, re.S)
if not m:
    errors.append("bloom_params_t is not { float p[4]; }")
if "sizeof(bloom_params_t) == 16" not in src:
    errors.append("no COMPILE_TIME_ASSERT pinning sizeof(bloom_params_t) == 16")
if not re.search(r"#define BLOOM_PARAMS_UBO_BINDING\s+6\b", src):
    errors.append("BLOOM_PARAMS_UBO_BINDING is not 6")
if "6 is gl_postprocess.c's BloomParams" not in (ROOT / "engine/h2shared/gl_shader.h").read_text():
    errors.append("gl_shader.h's binding registry does not list 6 as BloomParams")
for gone in ("bloom_vert_src", "bloom_bright_frag_src", "bloom_down_frag_src",
             "bloom_up_frag_src", "_loc_threshold", "bloom_down_loc_rcpframe",
             "bloom_up_loc_rcpframe", "bloom_bright_loc_rcpframe"):
    if gone in code:
        errors.append(f"{gone} is still in gl_postprocess.c")
for sym in ("sbloom_vert", "sbloom_bright_frag", "sbloom_down_frag", "sbloom_up_frag"):
    if sym not in code:
        errors.append(f"gl_postprocess.c never compiles {sym}")
push = re.search(r"static void PP_PushBloomParams \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
if not push:
    errors.append("no PP_PushBloomParams")
else:
    body = push.group(1)
    if re.search(r"\bGL_Upload\s*\(|\bGL_BindBufferRange\s*\(", body):
        errors.append("PP_PushBloomParams uses the GL_Upload ring, a stub on the ES tier")
    if "BLOOM_PARAMS_UBO_BINDING" not in body or "bloom_ubo" not in body:
        errors.append("PP_PushBloomParams does not bind bloom_ubo at BLOOM_PARAMS_UBO_BINDING")
calls = len(re.findall(r"\bPP_PushBloomParams\s*\(", code)) - 1  # minus the definition
if calls != 3:
    errors.append(f"{calls} PP_PushBloomParams calls, expected 3 (bright, down, up)")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS BloomParams carries the bloom passes' parameters on both tiers (#450)")
