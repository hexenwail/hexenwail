#!/usr/bin/env python3
"""salias_frag reads its uniforms from FragParams and AliasFrag (#450, alias S2).

Checks the pieces that have to move together:
  * AliasFrag in engine/shaders/alias.inc and r_aliasfrag_t in gl_uniforms.h
    agree member for member (std140 sizes, int/float kinds, 144 bytes), and
    FragParams and r_fragparams_t likewise; each alias names the member the
    old loose uniform's value now lives in, and each setter writes that slot;
  * AliasFrag has a binding point of its own that no other uniform block
    uses, is bound and uploaded by R_FlushUniforms through a permanent buffer
    (glBufferData, not the GL_Upload ring, a stub on the ES tier), and is
    seeded with material and light direction (0,0,1);
  * engine/shaders/salias_frag.glsl declares no loose non-opaque uniform, keeps
    the clustered-dlight half under #ifndef GL_ES, and declares its colour
    output on one line, first, as the OIT splice needs;
  * the .inc copies of the GLSL_* helpers sworld_frag still splices are
    token-identical to them, and the froxel-grid sizes agree with the C side;
  * the literal and its alias-only macros are gone, and all ten programs
    compile the file through GLSL_ALIAS_FRAG_HEADER (NOPERSP in the header
    for the four affine twins), the instanced pair binding its blocks by hand;
  * every draw path that uses those programs writes the members it reads
    before it draws (the hazards in the #450 alias plan):
      GL_ImmPushBlocks (8), the skeletal draw (2, 5), the instanced main pass
      (1, 4, 5) and its fullbright pass (3), R_SetSpanCell (6),
      R_LightCluster_BindForAlias (7), the instanced program's hookup (9),
      the shadow's seed (10).
The varying interface between the three vertex stages and this file is
tools/test_alias_vert_block.py's.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []
read = lambda p: (ROOT / p).read_text()


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


def tokens(t):
    return re.findall(r"[A-Za-z_]\w*|\d+\.\d*(?:e[-+]?\d+)?|\.\d+|\d+(?:e[-+]?\d+)?|\S", strip(t))


def body_of(text, signature):
    """The brace-balanced body of the first function whose header matches."""
    m = re.search(signature, text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    return None


gl_shader = read("engine/h2shared/gl_shader.c")
uh = read("engine/h2shared/gl_uniforms.h")
uc = read("engine/h2shared/gl_uniforms.c")
vbo = read("engine/h2shared/gl_vbo.c")
rmain = read("engine/hexen2/gl_rmain.c")
lc = read("engine/h2shared/gl_lightcluster.c")
shader_h = read("engine/h2shared/gl_shader.h")
alias_inc = read("engine/shaders/alias.inc")
uniforms_inc = read("engine/shaders/uniforms.inc")
frag_path = SH / "salias_frag.glsl"
frag = frag_path.read_text() if frag_path.exists() else ""
fcode = strip(frag)

# ------------------------------------------------------------ the mirrors
SIZE = {"mat4": 64, "vec4": 16, "ivec4": 16}
KIND = {"mat4": "float", "vec4": "float", "ivec4": "int"}


def check_mirror(glsl_text, block, cstruct, total):
    vb = re.search(rf"uniform {block} \{{(.*?)\}};", strip(glsl_text), re.S)
    if not vb:
        errors.append(f"no uniform block {block}")
        return []
    if re.search(r"\b(float|int|uint|bool|vec2|vec3|ivec2|ivec3|mat3)\s+\w+\s*;", vb.group(1)):
        errors.append(f"{block} gained a non-vec4/mat4 member; the offset arithmetic here no longer holds")
    g = [(n, SIZE[t], KIND[t]) for t, n in re.findall(r"\b(mat4|ivec4|vec4)\s+(\w+)\s*;", vb.group(1))]
    cm = re.search(rf"typedef struct \{{([^{{}}]*)\}} {cstruct};", uh)
    if not cm:
        errors.append(f"no {cstruct} in gl_uniforms.h")
        return g
    c = [(n, 4 * int(k), t) for t, n, k in re.findall(r"\b(float|int)\s+(\w+)\[(\d+)\];", cm.group(1))]
    if [(s, k) for _, s, k in g] != [(s, k) for _, s, k in c]:
        errors.append(f"{block} {[(n, s, k) for n, s, k in g]} and {cstruct} {c} differ in size/kind/order")
    if sum(s for _, s, _ in g) != total:
        errors.append(f"{block} is {sum(s for _, s, _ in g)} bytes, expected {total}")
    return g


af = check_mirror(alias_inc, "AliasFrag", "r_aliasfrag_t", 144)
check_mirror(uniforms_inc, "FragParams", "r_fragparams_t", 144)
if "COMPILE_TIME_ASSERT(aliasfrag_size, sizeof(r_aliasfrag_t) == 64 + 5 * 16);" not in uc:
    errors.append("COMPILE_TIME_ASSERT must pin r_aliasfrag_t at 64 + 5*16")
if not re.search(r"FRAG_UBO\(1\) uniform AliasFrag", alias_inc):
    errors.append("AliasFrag must be FRAG_UBO(1), after FragParams' 0")

# Old loose name -> (block member, swizzle) and the C slot its setter writes.
ALIASES = {
    "u_lightview": "u_aflightview", "u_lightgrid_xy": "u_afgrid", "u_lightgrid_z": "u_afgridz.xy",
    "u_turb": "u_afwarp.xy", "u_spans": "u_afwarp.z",
    "u_alias_material": "u_afmaterial.xyz", "u_alias_lightdir": "u_aflightdir.xyz",
}
for old, new in ALIASES.items():
    if not re.search(rf"#define\s+{old}\s+{re.escape(new)}\s*\n", alias_inc):
        errors.append(f"alias.inc must map {old} to {new}")
FRAG_ALIASES = {
    "u_fog_color": "u_ffogv.xyz", "u_fog_density": "u_ffogv.w", "u_alpha_threshold": "u_fmask.x",
    "u_force_opaque_alpha": "u_fmask.y", "u_alias_caustics": "u_fcaustics.zw", "u_soft_params": "u_fsoft.xyz",
}
for old, new in FRAG_ALIASES.items():
    if not re.search(rf"#define\s+{old}\s+{re.escape(new)}\s*\n", uniforms_inc):
        errors.append(f"uniforms.inc must map {old} to {new}")
SETTERS = {
    "R_SetAliasLightGrid": [r"aliasfrag\.lightview", r"aliasfrag\.grid,", r"aliasfrag\.gridz"],
    "R_SetAliasTurb": [r"aliasfrag\.warp\[0\] = amplitude", r"aliasfrag\.warp\[1\] = time"],
    "R_SetAliasSpans": [r"aliasfrag\.warp\[2\] = cell"],
    "R_SetAliasMaterialU": [r"aliasfrag\.material,\s*normal, gloss, glossexp"],
    "R_SetAliasLightDirU": [r"aliasfrag\.lightdir,\s*dir3\[0\], dir3\[1\], dir3\[2\]"],
    "R_SetAliasCausticsU": [r"fragparams\.caustics\[2\] = intensity", r"fragparams\.caustics\[3\] = time"],
    "R_SetSoftParams": [r"fragparams\.soft, x, y, z"],
}
for fn, wants in SETTERS.items():
    b = body_of(uc, rf"\nvoid {fn} \([^)]*\)\s*\{{")
    if b is None:
        errors.append(f"gl_uniforms.c: no {fn}")
        continue
    for w in wants:
        if not re.search(w, b):
            errors.append(f"{fn} must write {w}")
    if "afrag_dirty = true" not in b and "frag_dirty = true" not in b:
        errors.append(f"{fn} never marks its block dirty")
for fn in ("R_SetAliasTurb", "R_SetAliasSpans", "R_SetAliasMaterialU", "R_SetAliasLightDirU", "R_SetAliasLightGrid"):
    b = body_of(uc, rf"\nvoid {fn} \([^)]*\)\s*\{{") or ""
    if "afrag_dirty = true" not in b:
        errors.append(f"{fn} must mark AliasFrag dirty, not another block")

# ------------------------------------------------------------ binding and upload
bind = re.search(r"#define\s+UBO_BINDING_ALIAS_FRAG\s+(\d+)", shader_h)
allbind = {}
for f in ("engine/h2shared/gl_shader.h", "engine/h2shared/gl_postprocess.c", "engine/h2shared/gl_ssao.c",
          "engine/h2shared/gl_worldcull.c", "engine/h2shared/gl_lightcluster.c"):
    for name, num in re.findall(r"#define\s+(\w*UBO_BINDING\w*)\s+(\d+)", read(f)):
        allbind.setdefault(int(num), []).append(name)
if not bind:
    errors.append("gl_shader.h: no UBO_BINDING_ALIAS_FRAG")
elif len(allbind.get(int(bind.group(1)), [])) != 1:
    errors.append(f"UBO binding {bind.group(1)} is shared: {allbind[int(bind.group(1))]}")
flush = body_of(uc, r"\nvoid R_FlushUniforms \(void\)\s*\{") or ""
if "GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_ALIAS_FRAG, ubo_afrag);" not in flush:
    errors.append("R_FlushUniforms must bind ubo_afrag at UBO_BINDING_ALIAS_FRAG")
if not re.search(r"if \(afrag_dirty\)\s*\{[^}]*glBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\(aliasfrag\), &aliasfrag", flush):
    errors.append("R_FlushUniforms must upload a dirty AliasFrag with glBufferData")
if "GL_Upload" in strip(uc):
    errors.append("gl_uniforms.c must not use the GL_Upload ring (a stub on the ES tier)")
inv = body_of(uc, r"\nvoid R_InvalidateUniforms \(void\)\s*\{") or ""
if "afrag_dirty = true" not in inv:
    errors.append("R_InvalidateUniforms must re-upload AliasFrag too")
init = body_of(uc, r"\nvoid R_Uniforms_Init \(void\)\s*\{") or ""
for w in ("aliasfrag.material[2] = 1.0f;", "aliasfrag.lightdir[2] = 1.0f;", "glGenBuffers_fp (1, &ubo_afrag);",
          "afrag_dirty = true;"):
    if w not in init:
        errors.append(f"R_Uniforms_Init: missing {w} (hazard 10 / seeding)")
if not re.search(r"GLint R_BindAliasFragBlock \(GLuint program\)\s*\{.*?RU_BindOne \(program, RU_NAME_AFRAG, UBO_BINDING_ALIAS_FRAG\)", uc, re.S) \
        or '#define RU_NAME_AFRAG	"AliasFrag"' not in uc:
    errors.append("R_BindAliasFragBlock must pin the \"AliasFrag\" block to UBO_BINDING_ALIAS_FRAG")

# ------------------------------------------------------------ the file
if '#include "layout.inc"\n#define UNIFORMS_FRAG\n#include "uniforms.inc"\n#include "alias.inc"' not in frag:
    errors.append("salias_frag.glsl must include layout.inc, then uniforms.inc with UNIFORMS_FRAG, then alias.inc")
# Kept loose, on purpose: none.  Samplers are opaque and stay loose (GL
# assigns their units with glUniform1i; Vulkan via FRAG_TEX).
ALLOWED_LOOSE = set()
loose = [m.group(1) for m in re.finditer(
    r"^\s*(?:FRAG_TEX\(\d+\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?!sampler|usampler|isampler)\w+\s+(\w+)\s*(?:\[\w*\])?\s*;",
    fcode, re.M)]
if set(loose) - ALLOWED_LOOSE:
    errors.append(f"salias_frag.glsl declares loose non-opaque uniforms {sorted(set(loose) - ALLOWED_LOOSE)}")
samplers = re.findall(r"FRAG_TEX\((\d+)\) uniform (?:highp )?(\w+) (\w+);", fcode)
if sorted(int(b) for b, _, _ in samplers) != list(range(len(samplers))):
    errors.append(f"FRAG_TEX bindings must be 0..n-1 in set 2: {samplers}")
if not re.search(rf"#define LIGHTGRID_SSBO_SLOT {len(samplers)}\n", fcode):
    errors.append(f"LIGHTGRID_SSBO_SLOT must follow the {len(samplers)} sampled textures in set 2")
desk = re.findall(r"#ifndef GL_ES\n(.*?)#(?:endif|else)", fcode, re.S)
deskall = "".join(desk)
for want in ("v_eyepos", "v_dlightscale", "usampler3D u_lightgrid", '#include "lightgrid.inc"',
             '#include "alias_dlight.inc"', "ClusteredAliasDlight(lit, v_eyepos, v_dlightscale)"):
    if want not in deskall:
        errors.append(f"salias_frag.glsl: {want} must sit under #ifndef GL_ES")
es = re.search(r"#else\n(.*?)#endif", fcode[fcode.index("ClusteredAliasDlight(lit"):] if "ClusteredAliasDlight(lit" in fcode else "", re.S)
if not es or "vec4 color = tex * v_color;" not in es.group(1):
    errors.append("the ES arm must be the plain `vec4 color = tex * v_color;`")
lines = [l for l in fcode.splitlines() if "fragColor;" in l]
if not lines or lines[0].strip() != "FRAG_OUT(0) out vec4 fragColor;":
    errors.append("the first line holding `fragColor;` must be the one-line FRAG_OUT(0) declaration (OIT splice)")
# The OIT splice runs on the EXPANDED body, so included text must not reach
# `fragColor;` first either.
expanded = frag
for _ in range(32):
    m = re.search(r'#include "([^"]+)"\n', expanded)
    if not m:
        break
    expanded = expanded.replace(m.group(0), (SH / m.group(1)).read_text())
first = expanded.find("fragColor;")
if first < 0 or "FRAG_OUT(0) out vec4 fragColor;" not in expanded[expanded.rfind("\n", 0, first):first + 11]:
    errors.append("after include expansion, the first `fragColor;` is not the output declaration")

# ------------------------------------------------------------ the .inc copies


def c_macro(name, nums):
    """GLSL text of a gl_shader.c #define of string literals (desktop arm, the
    first definition), with GLSL_STR(X) replaced from `nums`."""
    i = re.search(rf"^#define {name}\b", gl_shader, re.M)
    if not i:
        return None
    i = i.end()
    out = []
    while i < len(gl_shader):
        if gl_shader.startswith("/*", i):
            i = gl_shader.index("*/", i) + 2
            continue
        if gl_shader.startswith("\\\n", i):
            i += 2
            continue
        ch = gl_shader[i]
        if ch == "\n":
            break
        if ch == '"':
            j = i + 1
            while gl_shader[j] != '"':
                j += 2 if gl_shader[j] == "\\" else 1
            out.append(gl_shader[i + 1:j].encode().decode("unicode_escape"))
            i = j + 1
            continue
        m = re.match(r"GLSL_STR\((\w+)\)|[A-Za-z_]\w*", gl_shader[i:])
        if m:
            out.append(str(nums[m.group(1)]) if m.group(1) else (c_macro(m.group(0), nums) or ""))
            i += m.end()
            continue
        i += 1
    return "".join(out)


def cnum(path, name):
    m = re.search(rf"#define\s+{name}\s+(\d+)", read(path))
    return int(m.group(1)) if m else None


NUMS = {"LIGHT_TILES_X": cnum("engine/h2shared/gl_lightcluster.h", "LIGHT_TILES_X"),
        "LIGHT_TILES_Y": cnum("engine/h2shared/gl_lightcluster.h", "LIGHT_TILES_Y"),
        "LIGHT_TILES_Z": cnum("engine/h2shared/gl_lightcluster.h", "LIGHT_TILES_Z"),
        "LIGHT_SSBO_BINDING": cnum("engine/h2shared/gl_lightcluster.h", "LIGHT_SSBO_BINDING"),
        "MAX_LIGHTSTYLES": cnum("engine/hexen2/quakedef.h", "MAX_LIGHTSTYLES")}
lg = (SH / "lightgrid.inc").read_text()
for k, v in NUMS.items():
    if v is None or not re.search(rf"#define\s+{k}\s+{v}\b", lg):
        errors.append(f"lightgrid.inc's #ifndef default for {k} must be the C value {v}")
consts = c_macro("GLSL_LIGHTGRID_CONSTS", NUMS) or ""
for k, v in NUMS.items():
    if f"#define {k} {v}\n" not in consts:
        errors.append(f"GLSL_LIGHTGRID_CONSTS must pass {k} to the alias fragment header")
if not re.search(r"#define GLSL_ALIAS_FRAG_HEADER\tGLSL_FRAG_HEADER GLSL_LIGHTGRID_CONSTS\n", gl_shader):
    errors.append("GLSL_ALIAS_FRAG_HEADER must be GLSL_FRAG_HEADER followed by GLSL_LIGHTGRID_CONSTS")
if not re.search(r'#else\t/\* ES/WebGL2.*?#define GLSL_LIGHTGRID_CONSTS\t""', gl_shader, re.S):
    errors.append("GLSL_LIGHTGRID_CONSTS needs an empty ES arm")


def inc_code(name):
    t = (SH / name).read_text()
    t = re.sub(r"#ifndef \w+_INC\n#define \w+_INC\n", "", t)
    t = re.sub(r"#endif /\* \w+_INC \*/\n", "", t)
    return t


for inc, macro in (("caustics.inc", "GLSL_CAUSTICS_FN"), ("turb_uv.inc", "GLSL_TURB_UV_FN"),
                   ("span_uv.inc", "GLSL_SPAN_UV_FN"), ("material.inc", "GLSL_MATERIAL_FN")):
    c = c_macro(macro, NUMS)
    if c is None or tokens(c) != tokens(inc_code(inc)):
        errors.append(f"{inc} is not token-identical to {macro}")
# lightgrid.inc: the struct and the buffer of GLSL_LIGHTGRID_DECL, and all of
# GLSL_LIGHTGRID_FETCH_FN, with the sizes substituted.  The DECL's loose
# uniforms are the includer's business.
lgt = tokens(re.sub(r"#ifndef LIGHT_TILES_X.*?#endif\n", "", inc_code("lightgrid.inc"), flags=re.S)
             .replace("FRAG_SSBO(LIGHT_SSBO_BINDING, LIGHTGRID_SSBO_SLOT)",
                      f"layout(std430, binding = {NUMS['LIGHT_SSBO_BINDING']})")
             .replace("MAX_LIGHTSTYLES", str(NUMS["MAX_LIGHTSTYLES"]))
             .replace("LIGHT_TILES_X", str(NUMS["LIGHT_TILES_X"]))
             .replace("LIGHT_TILES_Y", str(NUMS["LIGHT_TILES_Y"]))
             .replace("LIGHT_TILES_Z", str(NUMS["LIGHT_TILES_Z"])))
decl = c_macro("GLSL_LIGHTGRID_DECL", NUMS) or ""
decl = decl[:decl.index("uniform usampler3D")] if "uniform usampler3D" in decl else ""
if lgt != tokens(decl + (c_macro("GLSL_LIGHTGRID_FETCH_FN", NUMS) or "")):
    errors.append("lightgrid.inc is not token-identical to GLSL_LIGHTGRID_DECL's struct+buffer and GLSL_LIGHTGRID_FETCH_FN")
if not re.search(r"#define FRAG_SSBO\(gl, vk\) layout\(std430, set = 2, binding = vk\)", read("engine/shaders/layout.inc")):
    errors.append("layout.inc: FRAG_SSBO's Vulkan arm must be set 2, binding vk")

# ------------------------------------------------------------ the C side's builds
if re.search(r"static const char salias_frag\[\]", gl_shader):
    errors.append("the inline salias_frag literal is back in gl_shader.c")
for gone in ("GLSL_ALIAS_DLIGHT_DECL", "GLSL_ALIAS_DLIGHT_FN", "GLSL_ALIAS_DLIGHT_APPLY", "GLSL_ALIAS_DLIGHT_FS_IN",
             "GL_InitProgramDefines"):
    if re.search(rf"\b{gone}\b", strip(gl_shader)):
        errors.append(f"{gone} is still in gl_shader.c; salias_frag.glsl replaced it")
H, NP = r"GLSL_ALIAS_FRAG_HEADER", r'GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\\n"'
builds = {
    "alias": rf'GL_InitProgramSplit\(&gl_shader_alias, "alias", GLSL_VERT_HEADER, salias_vert,\s*{H}, salias_frag\)',
    "alias_np": rf'GL_InitProgramSplit\(&gl_shader_alias_np, "alias_np",\s*GLSL_VERT_HEADER "#define NOPERSP 1\\n", salias_vert,\s*{NP}, salias_frag\)',
    "alias_oit": rf'GL_InitOITProgramSplit\(&gl_shader_alias_oit, "alias", GLSL_VERT_HEADER, salias_vert,\s*{H}, salias_frag\)',
    "alias_np_oit": rf'GL_InitOITProgramSplit\(&gl_shader_alias_np_oit, "alias_np",\s*GLSL_VERT_HEADER "#define NOPERSP 1\\n", salias_vert,\s*{NP}, salias_frag\)',
    "skeletal": rf'GL_InitProgramSplit\(&gl_shader_skeletal, "skeletal", NULL, sskeletal_vert,\s*{H}, salias_frag\)',
    "skeletal_np": rf'GL_SpliceDefines \(sskeletal_vert, "#define NOPERSP 1\\n"\);\s*if \(v\)\s*GL_InitProgramSplit\(&gl_shader_skeletal_np, "skeletal_np", NULL, v,\s*{NP}, salias_frag\)',
    "skeletal_oit": rf'GL_InitOITProgramSplit\(&gl_shader_skeletal_oit, "skeletal", NULL, sskeletal_vert,\s*{H}, salias_frag\)',
    "skeletal_np_oit": rf'GL_SpliceDefines \(sskeletal_vert, "#define NOPERSP 1\\n"\);\s*if \(v\)\s*GL_InitOITProgramSplit\(&gl_shader_skeletal_np_oit, "skeletal_np", NULL, v,\s*{NP}, salias_frag\)',
}
for name, b in builds.items():
    if not re.search(b, gl_shader):
        errors.append(f"build of {name} not found: {b[:70]}...")
inst = body_of(gl_shader, r"static qboolean GL_InitAliasInstProgram \([^)]*\)\s*\{") or ""
for w in (r'q_snprintf \(frag_header, sizeof\(frag_header\), "%s%s",\s*GLSL_ALIAS_FRAG_HEADER, defines \? defines : ""\);',
          r"fs = GL_CompileShaderSplit\(GL_FRAGMENT_SHADER, frag_header, salias_frag\);",
          r"R_BindProgramBlocks\(prog, &p->ub_vert, &p->ub_frag\);",
          r"p->ub_afrag = R_BindAliasFragBlock\(prog\);"):
    if not re.search(w, inst):
        errors.append(f"GL_InitAliasInstProgram (hazard 9): missing {w[:60]}")
if "memset(p, 0" in inst and "R_BindAliasFragBlock" in inst \
        and inst.index("memset(p, 0") > inst.index("R_BindAliasFragBlock"):
    errors.append("GL_InitAliasInstProgram binds the blocks before memset clears them")
if "GL_AliasInst_Init" not in gl_shader or 'GL_InitAliasInstProgram(&gl_shader_alias_inst_np, "alias_instanced_np", "#define NOPERSP 1\\n");' not in gl_shader:
    errors.append("the instanced affine twin must pass NOPERSP")
st = re.search(r"typedef struct \{(.*?)\} gl_alias_inst_prog_t;", shader_h, re.S)
stf = set(re.findall(r"GLint\s+(\w+);", strip(st.group(1)))) if st else set()
if not {"ub_vert", "ub_frag", "ub_afrag"} <= stf:
    errors.append("gl_alias_inst_prog_t needs ub_vert / ub_frag / ub_afrag (hazard 9)")
dead = stf & {"u_fog_density", "u_fog_color", "u_alpha_threshold", "u_force_opaque_alpha", "u_alias_caustics",
              "u_alias_material", "u_alias_lightdir", "u_lightgrid_xy", "u_lightgrid_z", "u_turb", "u_soft_params", "u_spans"}
if dead:
    errors.append(f"gl_alias_inst_prog_t still carries fragment-stage locations that are -1 now: {sorted(dead)}")
if "p->ub_afrag = R_BindAliasFragBlock(p->program);" not in gl_shader:
    errors.append("GL_InitProgramUniforms must look up ub_afrag")
if "fs = GL_CompileOITFragShaderSplit (frag_header, frag_src);" not in gl_shader:
    errors.append("GL_LoadOITProgramSplit must always take the header-aware splice now")


# ------------------------------------------------------------ per-path writes
def ordered(text, label, wants, before):
    """Every regex in `wants` occurs in `text`, and each one before `before`."""
    t = strip(text or "")
    end = re.search(before, t)
    if not end:
        errors.append(f"{label}: cannot find {before}")
        return
    for w in wants:
        m = re.search(w, t)
        if not m:
            errors.append(f"{label}: must write {w}")
        elif m.start() > end.start():
            errors.append(f"{label}: {w} comes after {before}")


push = body_of(vbo, r"static void GL_ImmPushBlocks \([^)]*\)\s*\{")
gate = re.search(r"if \(shader->ub_afrag >= 0\)\s*\{(.*?)\}", strip(push or ""), re.S)
for w in (r"R_SetAliasCausticsU \(imm_alias_caustics\[0\], imm_alias_caustics\[1\]\);",
          r"R_SetSoftParams \(imm_soft\[0\], imm_soft\[1\], imm_soft\[2\]\);",
          r"R_SetAliasTurb \(imm_turb\[0\], imm_turb\[1\]\);",
          r"R_SetAliasMaterialU \(imm_alias_material\[0\], imm_alias_material\[1\],\s*imm_alias_material\[2\]\);",
          r"R_SetAliasLightDirU \(imm_alias_lightdir\);"):
    if not gate or not re.search(w, gate.group(1)):
        errors.append(f"GL_ImmPushBlocks (hazard 8): the ub_afrag branch must write {w}")
ordered(push, "GL_ImmPushBlocks", [
    r"R_SetAlphaMask \(imm_alpha_threshold >= 0\.0f \? imm_alpha_threshold : 0\.0f,\s*imm_force_opaque_alpha >= 0\.0f \? imm_force_opaque_alpha : 0\.0f\);",
    r"R_SetFog \(r_fog_density, r_fog_color\);", r"if \(shader->ub_afrag >= 0\)"], r"R_FlushUniforms \(\);")
if push and re.search(r"if \([^)]*imm_cache", strip(push)):
    errors.append("GL_ImmPushBlocks must not gate a block write on the per-program loose cache")

skel = body_of(rmain, r"static void GL_DrawAliasSkeletal \(entity_t \*e, aliashdr_t \*paliashdr,\s*int posenum, int prevposenum, float lerpfrac\)\s*\{\s*gl_alias_gpu_mesh")
if skel is None:
    skel = body_of(rmain, r"static void GL_DrawAliasSkeletal \([^)]*\)\s*\{")
ordered(skel, "GL_DrawAliasSkeletal (hazards 2, 5)", [
    r"R_SetFog \(r_fog_density, r_fog_color\);",
    r"R_SetAlphaMask \(GL_GetAlphaThreshold\(\), GL_GetForceOpaqueAlpha\(\)\);",
    r"R_SetAliasCausticsU \(caustics\[0\], caustics\[1\]\);",
    r"R_SetSoftParams \(0\.0f, 0\.0f, 0\.0f\);", r"R_SetAliasTurb \(0\.0f, 0\.0f\);",
    r"R_SetAliasSpans \(0\.0f\);", r"R_SetAliasMaterialU \(mat\[0\], mat\[1\], mat\[2\]\);",
    r"R_SetAliasLightDirU \(dir\);", r"R_FlushUniforms \(\);"], r"glDrawElements_fp \(GL_TRIANGLES")

inst_draw = body_of(rmain, r"static void R_DrawAliasInstanced \(void\)\s*\{") or ""
loop1 = inst_draw.find("for (b = 0; b < num_alias_batches; b++)")
fbi = inst_draw.find("Fullbright pass")
main_pre, main_loop = inst_draw[:loop1], inst_draw[loop1:fbi]
ordered(main_pre + "\nLOOP\n", "R_DrawAliasInstanced main pass (hazards 1, 3)", [
    r"R_SetFog \(r_fog_density, r_fog_color\);", r"R_SetAlphaMask \(0\.01f, 1\.0f\);",
    r"R_SetAliasCausticsU \(R_CausticsIntensity\(\), \(float\)cl\.time\);",
    r"R_SetSoftParams \(0\.0f, 0\.0f, 0\.0f\);", r"R_SetAliasTurb \(0\.0f, 0\.0f\);",
    r"R_SetAliasSpans \(0\.0f\);"], r"\nLOOP\n")
ordered(main_loop, "R_DrawAliasInstanced batch loop (hazards 4, 5)", [
    r"R_SetAliasMaterialU \(nrm, gls,", r"R_AliasMaterialLightDirWorld \(dir\);\s*R_SetAliasLightDirU \(dir\);",
    r"else\s*R_SetAliasMaterialU \(0\.0f, 0\.0f, 1\.0f\);", r"R_FlushUniforms \(\);"],
    r"glDrawElementsInstanced_fp")
fb = inst_draw[fbi:]
loop2 = fb.find("for (b = 0; b < num_alias_batches; b++)")
ordered(fb[:loop2] + "\nLOOP\n", "R_DrawAliasInstanced fullbright pass (hazard 3)", [
    r"R_SetFog \(r_fog_density, vec3_origin\);", r"R_SetAlphaMask \(0\.01f, 1\.0f\);",
    r"R_SetAliasCausticsU \(R_CausticsIntensity\(\), \(float\)cl\.time\);",
    r"R_SetSoftParams \(0\.0f, 0\.0f, 0\.0f\);", r"R_SetAliasTurb \(0\.0f, 0\.0f\);",
    r"R_SetAliasSpans \(0\.0f\);", r"R_SetAliasMaterialU \(0\.0f, 0\.0f, 1\.0f\);",
    r"R_FlushUniforms \(\);"], r"\nLOOP\n")
if not re.search(r"glDrawElementsInstanced_fp.*?R_SetFog \(r_fog_density, r_fog_color\);", strip(fb[loop2:]), re.S):
    errors.append("R_DrawAliasInstanced fullbright pass (hazard 3): must put the scene fog back after its draws")

span = body_of(rmain, r"void R_SetSpanCell \(const glprogram_t \*p, float cell\)\s*\{") or ""
if not re.search(r"if \(p->ub_afrag >= 0\)\s*\{\s*R_SetAliasSpans \(cell\);\s*return;\s*\}", strip(span)):
    errors.append("R_SetSpanCell (hazard 6): alias programs must take the AliasFrag arm")
if span.find("ub_afrag") > span.find("glUniform1f_fp (p->u_spans") >= 0:
    errors.append("R_SetSpanCell: the AliasFrag arm must come before the loose write")

bfa = body_of(lc, r"void R_LightCluster_BindForAlias \(void\)\s*\{") or ""
if "R_SetAliasLightGrid (lc_view, lc_vp, lc_zlogscale, lc_zlogbias);" not in bfa:
    errors.append("R_LightCluster_BindForAlias (hazard 7): must write the froxel lookup into AliasFrag once")
if bfa.find("R_LightCluster_AliasActive") > bfa.find("R_SetAliasLightGrid") or "R_LightCluster_AliasActive" not in bfa:
    errors.append("R_LightCluster_BindForAlias: the AliasFrag write must sit behind R_LightCluster_AliasActive")
if "LC_SetAliasUniforms" in strip(lc):
    errors.append("LC_SetAliasUniforms is back; the alias programs' froxel lookup is AliasFrag's now")
if not re.search(r"static float R_AliasDlightScale \(qboolean want\)\s*\{\s*if \(!want \|\| !R_LightCluster_AliasActive \(\)\)\s*return 0\.0f;", rmain):
    errors.append("R_AliasDlightScale must return 0 whenever the grid is not live, or AliasFrag's stale lookup is read")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS salias_frag reads FragParams + AliasFrag; mirrors, copies, builds and every path's writes agree (#450 S2)")
