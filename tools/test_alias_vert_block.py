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
  * the inline literal is gone, and all four alias builds (alias, alias_np,
    and both OIT twins) compile the file through GLSL_VERT_HEADER, with
    NOPERSP in the header for the two affine ones;
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

# The varying interface: salias_vert.glsl's outputs must match salias_frag's
# inputs (still a C literal) by name, type and interpolation qualifier, on the
# desktop tier, where a mismatch is a link failure at startup that leaves
# every model undrawn with lint green.  The fragment side is the literal with
# the desktop arm of GLSL_ALIAS_DLIGHT_FS_IN expanded.
def c_literal_text(symbol):
    m = re.search(rf"static const char {symbol}\[\] =\n(.*?)\";\n", src, re.S)
    if not m:
        return None
    # One ordered scan: a string literal, a C comment (consumed whole, so text
    # inside it is never mistaken for a literal or a macro), or a macro name.
    out = []
    for t in re.finditer(r'"((?:[^"\\]|\\.)*)"|/\*.*?\*/|//[^\n]*|\b([A-Z][A-Z0-9_]{3,})\b',
                         m.group(1) + '"', re.S):
        lit, macro = t.group(1), t.group(2)
        if lit is not None:
            out.append(lit.replace("\\n", "\n"))
        elif macro:
            out.append(f"@{macro}@")
    return "".join(out)
def desktop_macro(name):
    m = re.search(rf"#define {name} \\\n((?:\t\".*\" \\\n)*\t\".*\")", src)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).replace("\\n", "\n") if m else ""
def varyings(text, kw):
    found = {}
    for line in text.splitlines():
        line = re.sub(r"\b(VERT_OUT|FRAG_IN)\(\d+\)", "", line).strip()
        m = re.match(rf"((?:(?:TCQUAL|flat|highp|mediump|noperspective)\s+)*){kw}\s+(?:highp\s+)?(\w+)\s+(\w+)\s*;", line)
        if m:
            quals = set(m.group(1).split()) & {"TCQUAL", "flat", "noperspective"}
            found[m.group(3)] = (m.group(2), frozenset(quals))
    return found
frag = c_literal_text("salias_frag")
if frag is None:
    errors.append("cannot find the salias_frag literal to compare varyings against")
else:
    frag = frag.replace("@GLSL_ALIAS_DLIGHT_FS_IN@", desktop_macro("GLSL_ALIAS_DLIGHT_FS_IN"))
    vo, fi = varyings(strip(body), "out"), varyings(frag, "in")
    vo.pop("fragColor", None)
    if not fi or not vo:
        errors.append("could not parse the varying sets")
    elif vo != fi:
        errors.append(f"salias_vert outputs {sorted(vo.items())} != salias_frag inputs {sorted(fi.items())} (desktop link would fail)")

if re.search(r"static const char salias_vert\[\]", src):
    errors.append("the inline salias_vert literal is back in gl_shader.c")
builds = [
    r'GL_InitProgramSplit\(&gl_shader_alias, "alias", GLSL_VERT_HEADER, salias_vert,\s*NULL, salias_frag\)',
    r'GL_InitProgramSplit\(&gl_shader_alias_np, "alias_np",\s*GLSL_VERT_HEADER "#define NOPERSP 1\\n", salias_vert,',
    r'GL_InitOITProgramSplit\(&gl_shader_alias_oit, "alias", GLSL_VERT_HEADER, salias_vert,\s*NULL, salias_frag\)',
    r'GL_InitOITProgramSplit\(&gl_shader_alias_np_oit, "alias_np",\s*GLSL_VERT_HEADER "#define NOPERSP 1\\n", salias_vert,',
]
for b in builds:
    if not re.search(b, src):
        errors.append(f"alias build not found: {b[:60]}...")
if not re.search(r"fs = frag_header \? GL_CompileOITFragShaderSplit \(frag_header, frag_src\)\s*:\s*GL_CompileOITFragShader \(frag_src\);", src):
    errors.append("GL_LoadOITProgramSplit must fall back to the literal splice when frag_header is NULL")

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
