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
# The programs are rows of gl_shader.c's program table (#340 P1b), each
# sbloom_vert with its fragment stage, built by id here.
shader_c = (ROOT / "engine/h2shared/gl_shader.c").read_text()
for pass_ in ("bright", "down", "up"):
    rid = f"RPROG_BLOOM_{pass_.upper()}"
    if not re.search(rf'\[{rid}\] = \{{ "bloom_{pass_}", RPROG_LINK_PLAIN, \{{\s*'
                     rf'\{{ PP_VERT_HEADER, sbloom_vert, "sbloom.vert" \}},\s*'
                     rf'\{{ PP_FRAG_HEADER, sbloom_{pass_}_frag, "sbloom_{pass_}.frag" \}} \}} \}},', shader_c):
        errors.append(f"gl_shader.c has no {rid} row of sbloom_vert + sbloom_{pass_}_frag")
    if not re.search(rf"bloom_{pass_}_prog = R_Program_Create\({rid}\);", code):
        errors.append(f"gl_postprocess.c never builds {rid}")
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

# --- the OIT resolve's OitParams, the same arrangement (#450) --------------
oinc = (SH / "oit_resolve.inc").read_text() if (SH / "oit_resolve.inc").exists() else ""
oblocks = re.findall(r"uniform\s+OitParams\s*\{(.*?)\};", strip_comments(oinc), re.S)
if len(oblocks) != 1 or re.findall(r"\b(ivec4|vec4)\s+(\w+)\s*;", oblocks[0]) != [("ivec4", "u_oitp")]:
    errors.append("oit_resolve.inc must declare exactly OitParams { ivec4 u_oitp; }")
if not re.search(r"#define\s+u_mboit\s+u_oitp\.x\b", oinc):
    errors.append("u_mboit is not aliased to u_oitp.x")
for stage in ("soit_resolve_frag.glsl", "soit_resolve_msaa_frag.glsl"):
    t = (SH / stage).read_text() if (SH / stage).exists() else ""
    if not re.search(r'^#include "oit_resolve\.inc"\s*$', strip_comments(t), re.M):
        errors.append(f"{stage} does not #include \"oit_resolve.inc\"")
    if re.search(r"uniform\s+int\s+u_mboit", t):
        errors.append(f"{stage} still declares u_mboit loose")
if "sizeof(oit_params_t) == 16" not in src:
    errors.append("no COMPILE_TIME_ASSERT pinning sizeof(oit_params_t) == 16")
if not re.search(r"#define OIT_PARAMS_UBO_BINDING\s+7\b", src):
    errors.append("OIT_PARAMS_UBO_BINDING is not 7")
if "7\n * is its OitParams" not in (ROOT / "engine/h2shared/gl_shader.h").read_text():
    errors.append("gl_shader.h's binding registry does not list 7 as OitParams")
for gone in ("oit_resolve_vert", "oit_resolve_frag", "loc_mboit"):
    if re.search(rf"\b{gone}\b", code):
        errors.append(f"{gone} is still in gl_postprocess.c")
# the resolve's upload: its own buffer, never the ES-stubbed ring
oit_use = re.search(r"OIT_PARAMS_UBO_BINDING, oit_ubo\)", code)
if not oit_use:
    errors.append("the OIT resolve does not bind oit_ubo at OIT_PARAMS_UBO_BINDING")
if re.search(r"GL_BindBufferRange\s*\(\s*GL_UNIFORM_BUFFER\s*,\s*OIT_PARAMS_UBO_BINDING", code):
    errors.append("OitParams is bound through the GL_Upload ring, a stub on the ES tier")
for buf in ("oit_ubo", "bloom_ubo", "pp_vert_ubo", "pp_frag_ubo"):
    if not re.search(rf"glDeleteBuffers_fp\s*\(\s*1\s*,\s*&{buf}\s*\)", code):
        errors.append(f"{buf} is never freed")

# --- the main post-process program: PpVert + PpParams (#450) ---------------
ppf = (SH / "spp_frag.glsl").read_text() if (SH / "spp_frag.glsl").exists() else ""
ppv = (SH / "spp_vert.glsl").read_text() if (SH / "spp_vert.glsl").exists() else ""
pblock = re.findall(r"uniform\s+PpParams\s*\{(.*?)\};", strip_comments(ppf), re.S)
want = [("vec4", "pp_p0"), ("vec4", "pp_p1"), ("vec4", "pp_p2"), ("vec4", "pp_p3"), ("ivec4", "pp_i0")]
if len(pblock) != 1 or re.findall(r"\b(ivec4|vec4)\s+(\w+)\s*;", pblock[0]) != want:
    errors.append(f"spp_frag.glsl's PpParams must be exactly {want}")
cm = re.search(r"typedef struct \{([^{}]*)\} pp_params_t;", src)
cfields = re.findall(r"\b(float|GLint)\s+(\w+)\[4\];", cm.group(1)) if cm else []
if cfields != [("float", "p0"), ("float", "p1"), ("float", "p2"), ("float", "p3"), ("GLint", "i0")]:
    errors.append(f"pp_params_t does not mirror PpParams member for member: {cfields}")
if "sizeof(pp_params_t) == 80" not in src:
    errors.append("no COMPILE_TIME_ASSERT pinning sizeof(pp_params_t) == 80")
if not re.search(r"uniform\s+PpVert\s*\{\s*mat4\s+u_ppmvp\s*;\s*\}", strip_comments(ppv)):
    errors.append("spp_vert.glsl must declare PpVert { mat4 u_ppmvp; }")
# every old loose name is an alias into the right component, and only that
ALIASES = {"gamma": "pp_p0.x", "contrast": "pp_p0.y", "dither": "pp_p0.z", "lutmax": "pp_p0.w",
           "scale": "pp_p1.x", "waterwarp": "pp_p1.y", "time": "pp_p1.z", "fxaa_on": "pp_p1.w",
           "rcpFrame": "pp_p2.xy", "viewdelta": "pp_p2.zw", "motionblur": "pp_p3.x",
           "hdr_exposure": "pp_p3.y", "u_bloom_strength": "pp_p3.z", "softemu": "pp_i0.x"}
for name, target in ALIASES.items():
    if not re.search(rf"#define\s+{name}\s+{re.escape(target)}\b", ppf):
        errors.append(f"{name} is not aliased to {target}")
    if re.search(rf"uniform\s+\w+\s+{name}\s*;", strip_comments(ppf)):
        errors.append(f"{name} is still a loose uniform")
# The C side must write the field each alias reads -- a swapped index
# compiles and renders wrong.  Every write in gl_postprocess.c carries a
# trailing /* alias */ comment (".y" for the second half of a vec2 alias),
# and each one is checked against the alias table above, in BOTH passes.
# Then, per function: the push follows the last write, and the draw follows
# the push.  (A first cut of this check pinned 8 writes by substring; a
# reviewer's mutation test swapped three other fields and it still passed.)
COMP = {"x": 0, "y": 1, "z": 2, "w": 3, "xy": 0, "zw": 2}
def alias_slot(comment):
    name, _, half = comment.partition(".")
    target = ALIASES.get(name)
    if not target:
        return None
    field, comp = target.split(".")
    idx = COMP[comp] + (1 if half == "y" else 0)
    return field.replace("pp_", "").replace("i0", "i0"), idx
write_re = re.compile(r"pp_params\.(p[0-3]|i0)\[(\d)\]\s*=[^;]*;[ \t]*/\*\s*([\w.]+)\s*\*/")
bare_re = re.compile(r"pp_params\.(p[0-3]|i0)\[(\d)\]\s*=")
funcs_with_writes = 0
for fm in re.finditer(r"\n(?:static\s+)?\w[\w\s\*]*?\b(\w+)\s*\([^;{)]*\)\s*\{(.*?)\n\}", src, re.S):
    fname, body = fm.group(1), fm.group(2)
    if not bare_re.search(body):
        continue
    funcs_with_writes += 1
    bare = len(bare_re.findall(body))
    writes = list(write_re.finditer(body))
    if len(writes) != bare:
        errors.append(f"{fname}: {bare - len(writes)} pp_params write(s) lack an /* alias */ comment")
    for w in writes:
        field, idx, comment = "p" + w.group(1)[1] if w.group(1) != "i0" else "i0", int(w.group(2)), w.group(3)
        slot = alias_slot(comment)
        if slot is None:
            errors.append(f"{fname}: write commented /* {comment} */ names no PpParams alias")
        elif slot != (field, idx):
            errors.append(f"{fname}: /* {comment} */ is written to {field}[{idx}], "
                          f"but the shader reads it from {slot[0]}[{slot[1]}]")
    last = max(w.end() for w in bare_re.finditer(body))
    push = body.find("PP_PushParams();", last)
    draw = body.find("GL_ImmDraw", push if push >= 0 else len(body))
    if push < 0 or draw < 0:
        errors.append(f"{fname}: needs PP_PushParams() after its last pp_params write and before GL_ImmDraw")
# The native-resolution 3D-effects pass runs before this frame's bloom exists,
# so it must not composite any: left alone it inherited the previous frame's
# strength and sampled last frame's bloom_tex[0] on unit 2 -- a second, stale
# bloom whenever motion blur or underwater warp was on with r_bloom.
native = re.search(r"static void PP_BlitWith3DEffects \([^)]*\)\s*\{(.*?)\n\}", src, re.S)
if not native or not re.search(r"pp_params\.p3\[2\]\s*=\s*0\.0f;", native.group(1)):
    errors.append("PP_BlitWith3DEffects must zero the bloom strength (pp_params.p3[2] = 0.0f)")
if funcs_with_writes != 2:
    errors.append(f"{funcs_with_writes} functions write pp_params, expected 2 (native and final pass)")
for gone in ("pp_vert_src", "pp_frag_src", "PP_BITFIELD_REVERSE", "pp_loc_gamma", "pp_loc_mvp"):
    if re.search(rf"\b{gone}\b", code):
        errors.append(f"{gone} is still in gl_postprocess.c")
if not re.search(r"#define PP_VERT_UBO_BINDING\s+8\b", src) or not re.search(r"#define PP_FRAG_UBO_BINDING\s+9\b", src):
    errors.append("PpVert/PpParams must be at binding points 8 and 9")
if "9 is its PpParams" not in (ROOT / "engine/h2shared/gl_shader.h").read_text():
    errors.append("gl_shader.h's binding registry does not list 8 and 9")
push = re.search(r"static void PP_PushParams \(void\)\s*\{(.*?)\n\}", code, re.S)
if not push or re.search(r"\bGL_Upload\s*\(|\bGL_BindBufferRange\s*\(", push.group(1)):
    errors.append("PP_PushParams must exist and stay off the GL_Upload ring (ES stub)")
# #340 P4a-1: under -gpu the blocks are mirrors (tools/test_rg_sources.py
# pins the SDL_GPU arm); the GL arm after it is the upload it always was.
if push:
    gl = re.split(r"#\s*endif", push.group(1))[-1]
    for want in (r"glBufferSubData_fp\(GL_UNIFORM_BUFFER, 0, sizeof\(pp_vert_mvp\), pp_vert_mvp\)",
                 r"glBufferSubData_fp\(GL_UNIFORM_BUFFER, 0, sizeof\(pp_params\), &pp_params\)",
                 r"GL_BindBufferBase\(GL_UNIFORM_BUFFER, PP_VERT_UBO_BINDING, pp_vert_ubo\)",
                 r"GL_BindBufferBase\(GL_UNIFORM_BUFFER, PP_FRAG_UBO_BINDING, pp_frag_ubo\)"):
        if not re.search(want, gl):
            errors.append("PP_PushParams' GL arm (after its #endif) lost `%s`" % want.replace("\\", ""))
bpush = re.search(r"static void PP_PushBloomParams \([^)]*\)\s*\{(.*?)\n\}", code, re.S)
if bpush:
    gate = re.search(r"#ifdef USE_SDL_GPU\s*if \(R_GPU \(\)\)\s*\{[^{}]*\breturn;\s*\}\s*#endif", bpush.group(1))
    first = re.search(r"\bGL_BindBuffer\(GL_UNIFORM_BUFFER, bloom_ubo\)", bpush.group(1))
    if not gate or not first or gate.start() > first.start():
        errors.append("PP_PushBloomParams must return under R_GPU () before touching bloom_ubo, "
                      "which -gpu does not create until BloomParams is a mirror (P4e)")
if len(re.findall(r"\bPP_PushParams\s*\(\s*\)\s*;", code)) != 2:
    errors.append("PP_PushParams must run before both pp_program draws (native and final)")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS BloomParams, OitParams and PpVert/PpParams carry the post-process parameters on both tiers (#450)")
