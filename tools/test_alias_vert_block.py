#!/usr/bin/env python3
"""salias_vert reads its uniforms from VertParams (#450, alias slice S1).

Checks the pieces that have to move together:
  * VertParams in uniforms.inc and r_vertparams_t in gl_uniforms.h agree
    member for member (std140 offsets, total size; nothing else pins this
    mirror in lint -- only RU_CheckBlockSize at run time);
  * u_alias_dlight is VertParams' u_valias.x, and R_SetAliasDlight writes
    alias[0];
  * engine/shaders/salias_vert.glsl includes uniforms.inc, declares no loose
    non-opaque uniform, and keeps the dlight varyings desktop-only (#ifndef
    GL_ES), as the GLSL_ALIAS_DLIGHT_VS_* splices did;
  * the inline literal is gone, and all four alias programs (alias, alias_np,
    and both OIT twins) are program-table rows (#340 P1b) that compile the
    file through GLSL_VERT_HEADER, with GLSL_NOPERSP after it for the two
    affine ones;
  * the varyings of all three vertex stages that link salias_frag (this file,
    engine/shaders/salias_inst_vert.glsl (#450 S3) and
    engine/shaders/sskeletal_vert.glsl (S4)) match the inputs of
    engine/shaders/salias_frag.glsl (#450 S2) by name, type, qualifier and
    location;
  * GL_ImmPushBlocks feeds u_alias_model and u_alias_dlight from imm_*;
  * the MSL cross-compile targets 2.1.0, which `invariant gl_Position` needs.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


inc = (ROOT / "engine/shaders/uniforms.inc").read_text()
vb = re.search(r"uniform VertParams \{(.*?)\};", strip(inc), re.S)
SIZE = {"mat4": 64, "vec4": 16, "ivec4": 16}
KIND = {"mat4": "float", "vec4": "float", "ivec4": "int"}
glsl, gkinds = [], []
for typ, name in re.findall(r"\b(mat4|ivec4|vec4)\s+(\w+)\s*;", vb.group(1) if vb else ""):
    glsl.append((name, SIZE[typ]))
    gkinds.append(KIND[typ])
hdr = (ROOT / "engine/h2shared/gl_uniforms.h").read_text()
cm = re.search(r"typedef struct \{([^{}]*)\} r_vertparams_t;", hdr)
craw = re.findall(r"\b(float|int)\s+(\w+)\[(\d+)\];", cm.group(1)) if cm else []
cfields = [(n, 4 * int(k)) for _, n, k in craw]
if gkinds != [t for t, _, _ in craw]:
    errors.append(f"VertParams member types {gkinds} and r_vertparams_t {[t for t, _, _ in craw]} differ "
                  "(an int read as float, or the reverse, is the wrong bits at the right offset)")
if [s for _, s in glsl] != [s for _, s in cfields]:
    errors.append(f"VertParams {glsl} and r_vertparams_t {cfields} differ in member sizes/order")
if sum(s for _, s in glsl) != 368:
    errors.append(f"VertParams is {sum(s for _, s in glsl)} bytes, expected 368")
if "sizeof(r_vertparams_t) == 4 * 64 + 7 * 16" not in (ROOT / "engine/h2shared/gl_uniforms.c").read_text():
    errors.append("COMPILE_TIME_ASSERT must pin r_vertparams_t at 4*64 + 7*16")
if glsl and glsl[-1][0] != "u_valias" or (cfields and cfields[-1][0] != "alias"):
    errors.append("the last VertParams member must be u_valias, mirrored as alias[4]")
if not re.search(r"#define\s+u_alias_dlight\s+u_valias\.x\b", inc):
    errors.append("u_alias_dlight must alias VertParams' u_valias.x")
uc = (ROOT / "engine/h2shared/gl_uniforms.c").read_text()
if not re.search(r"void R_SetAliasDlight \(float scale\)\s*\{[^}]*vertparams\.alias\[0\]", uc, re.S):
    errors.append("R_SetAliasDlight must write vertparams.alias[0]")

sv = ROOT / "engine/shaders/salias_vert.glsl"
body = sv.read_text() if sv.exists() else ""
code = strip(body)
if '#define UNIFORMS_VERT\n#include "uniforms.inc"' not in body:
    errors.append("salias_vert.glsl must #define UNIFORMS_VERT and include uniforms.inc")
if re.search(r"^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?!sampler|usampler|isampler)\w+\s+\w+\s*(?:\[\w*\])?\s*;", code, re.M):
    errors.append("salias_vert.glsl still declares a loose non-opaque uniform")
# Every VertParams member is vec4 / ivec4 / mat4, so summing sizes equals the
# std140 offsets.  A scalar or vec2/vec3 member would break that silently.
if vb and re.search(r"\b(float|int|uint|bool|vec2|vec3|ivec2|ivec3|mat3)\s+\w+\s*;", vb.group(1)):
    errors.append("VertParams gained a non-vec4/mat4 member; this check's offset arithmetic no longer holds")
m = re.search(r"#ifndef GL_ES\n(.*?)#endif", code, re.S)
if not m or "v_dlightscale" not in m.group(1) or "v_eyepos" not in m.group(1):
    errors.append("the dlight varyings must be declared under #ifndef GL_ES")

src = (ROOT / "engine/h2shared/gl_shader.c").read_text()

# The varying interface: every vertex stage linked with salias_frag must
# output exactly its inputs, by name, type and interpolation qualifier, on the
# desktop tier, where a mismatch is a link failure at startup that leaves every
# model undrawn with lint green.  Since #450 S2 the fragment side is
# engine/shaders/salias_frag.glsl; both arms of its #ifndef GL_ES are read,
# which is the desktop set (the ES arm declares no input of its own).  All
# three vertex stages are files since S4.
def locations(text, macro):
    """name -> SPIR-V location, from VERT_OUT(n) / FRAG_IN(n) declarations."""
    return {m.group(2): int(m.group(1)) for m in re.finditer(
        rf"\b{macro}\((\d+)\)[^;]*?\b(\w+)\s*;", text)}
def varyings(text, kw):
    found = {}
    for line in text.splitlines():
        line = re.sub(r"\b(VERT_OUT|FRAG_IN)\(\d+\)", "", line).strip()
        m = re.match(rf"((?:(?:TCQUAL|flat|highp|mediump|noperspective)\s+)*){kw}\s+(?:highp\s+)?(\w+)\s+(\w+)\s*;", line)
        if m:
            quals = set(m.group(1).split()) & {"TCQUAL", "flat", "noperspective"}
            found[m.group(3)] = (m.group(2), frozenset(quals))
    return found
sf = ROOT / "engine/shaders/salias_frag.glsl"
fi = varyings(strip(sf.read_text()), "in") if sf.exists() else {}
files = {"salias_vert.glsl": strip(body)}
for fname in ("salias_inst_vert.glsl", "sskeletal_vert.glsl"):
    fp = ROOT / "engine/shaders" / fname
    files[fname] = strip(fp.read_text()) if fp.exists() else ""
    if not files[fname]:
        errors.append(f"engine/shaders/{fname} is missing")
stages = {name: varyings(text, "out") for name, text in files.items()}
# Vulkan links varyings by location, not name: every file must agree slot for
# slot with salias_frag.glsl too, or a -V build swaps two of them silently.
floc = locations(strip(sf.read_text()), "FRAG_IN") if sf.exists() else {}
if len(floc) != 7:
    errors.append(f"could not parse salias_frag.glsl's seven FRAG_IN locations: {floc}")
for name, text in files.items():
    vloc = locations(text, "VERT_OUT")
    if vloc != floc:
        errors.append(f"{name} VERT_OUT locations {sorted(vloc.items())} != salias_frag FRAG_IN {sorted(floc.items())}")
if not fi:
    errors.append("could not parse salias_frag.glsl's inputs")
for name, vo in stages.items():
    vo.pop("fragColor", None)
    if not vo:
        errors.append(f"could not parse {name}'s outputs")
    elif vo != fi:
        errors.append(f"{name} outputs {sorted(vo.items())} != salias_frag inputs {sorted(fi.items())} (desktop link would fail)")

if re.search(r"static const char salias_vert\[\]", src):
    errors.append("the inline salias_vert literal is back in gl_shader.c")
# The four programs are rows of gl_shader.c's program table (#340 P1b),
# built by id; the row's first stage is this file with its header chunk.
builds = [
    (r'\[RPROG_ALIAS\] = \{ "alias", RPROG_LINK_ATTRIBS, \{\s*\{ GLSL_VERT_HEADER, salias_vert, "salias.vert" \},',
     r"GL_InitProgramSplit\(&gl_shader_alias, RPROG_ALIAS\);"),
    (r'\[RPROG_ALIAS_NP\] = \{ "alias_np", RPROG_LINK_ATTRIBS, \{\s*\{ GLSL_VERT_HEADER GLSL_NOPERSP, salias_vert, "salias_np.vert" \},',
     r"GL_InitProgramSplit\(&gl_shader_alias_np, RPROG_ALIAS_NP\);"),
    (r'\[RPROG_ALIAS_OIT\] = \{ "alias_oit", RPROG_LINK_ATTRIBS, \{\s*\{ GLSL_VERT_HEADER, salias_vert, "salias.vert" \},',
     r"GL_InitOITProgramSplit\(&gl_shader_alias_oit, RPROG_ALIAS_OIT\);"),
    (r'\[RPROG_ALIAS_NP_OIT\] = \{ "alias_np_oit", RPROG_LINK_ATTRIBS, \{\s*\{ GLSL_VERT_HEADER GLSL_NOPERSP, salias_vert, "salias_np.vert" \},',
     r"GL_InitOITProgramSplit\(&gl_shader_alias_np_oit, RPROG_ALIAS_NP_OIT\);"),
]
for row, call in builds:
    if not re.search(row, src):
        errors.append(f"alias program row not found: {row[:70]}...")
    if not re.search(call, src):
        errors.append(f"alias build not found: {call[:60]}...")
if len(re.findall(r"\bsalias_vert\b", strip(src))) != len(builds):
    errors.append("salias_vert must be the vertex stage of exactly the four alias rows")
# The fragment headers are test_alias_frag_block.py's to pin (#450 S2).

vbo = (ROOT / "engine/h2shared/gl_vbo.c").read_text()
push = re.search(r"static void GL_ImmPushBlocks \([^)]*\)\s*\{(.*?)\n\}", vbo, re.S)
pushcode = strip(push.group(1)) if push else ""
if "R_SetModelMatrix (imm_alias_model);" not in pushcode \
        or "R_SetAliasDlight (imm_alias_dlight);" not in pushcode:
    errors.append("GL_ImmPushBlocks must feed u_alias_model and u_alias_dlight from imm_*")

cross = (ROOT / "engine/cmake/CrossCompileShaders.cmake").read_text()
if "--msl-version 2.1.0" not in cross:
    errors.append("MSL must target 2.1.0 (invariant gl_Position)")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS salias_vert reads VertParams; mirror, builds and feeds agree (#450 S1)")
