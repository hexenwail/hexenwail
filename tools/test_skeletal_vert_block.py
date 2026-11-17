#!/usr/bin/env python3
"""sskeletal_vert reads its uniforms from VertParams and AliasVert (#450, alias S4).

Checks the pieces that have to move together:
  * AliasVert in engine/shaders/alias.inc and r_aliasvert_t in gl_uniforms.h,
    and VertParams and r_vertparams_t, agree member for member by name, size
    and int/float kind (96 and 368 bytes), with COMPILE_TIME_ASSERT pinning
    the AliasVert mirror and the driver check sized from it;
  * every old loose name the stage reads aliases the member its setter writes,
    the stage (the whole file, not just main) reads exactly that set, and no
    lane is written by two setters (u_vposei.w and u_valias.y are spare lanes
    of members other setters own) -- in any form: assignment, compound
    assignment, ++/--, &lane, or the bare member or struct as a pointer for
    memcpy/memset/RU_Set*;
  * each setter writes its set with no early exit and dirties only its own
    block;
  * engine/shaders/sskeletal_vert.glsl has no #version, declares no loose
    non-opaque uniform, puts its bones on VERT_SSBO(4, 0) -- GL binding 4,
    which GL_DrawAliasSkeletal binds, and Vulkan set 0 index 0, since the
    stage samples no texture -- and gives every input an explicit VERT_IN
    equal to the slot, width and integer-ness of the IQM VAO in gl_mesh.c,
    which is NOT the ATTR_* contract the other programs share, and its
    BonePose and main match a pinned token stream (GOLDEN);
  * the literal, GL_SpliceDefines and the GLSL_ALIAS_DLIGHT_VS_* splices are
    gone, all four skeletal programs compile the file through
    GLSL_VERT_HEADER (NOPERSP appended for the two affine ones) inside a
    desktop-only region, glprogram_t carries none of the six skeletal-only
    locations, and GL_InitProgramUniforms binds AliasVert;
  * GL_DrawAliasSkeletal makes no loose glUniform write, writes every member
    the stage reads (each through its setter, from the values the loose
    writes used, each matrix filled by its own getter) after the pose clamp
    and before one R_FlushUniforms at the draw's own brace depth, then binds
    the bones and draws.  Setters, getters, flush and draw are all
    unconditional: no braceless or braced if/else/for/while/do/switch/#if
    over them, no enclosing block that is a conditional's body (a plain
    scoping block is fine), and no exit between the clamp and the draw -- a
    block member is global, so a value left by an earlier draw is a bug,
    where the loose uniform was this program's own;
  * R_SetSkelPose and R_SetSkelLight write their whole set in one call and
    have no caller but GL_DrawAliasSkeletal.
The fragment stage's writes on the same path are test_alias_frag_block.py's,
and the varying interface tools/test_alias_vert_block.py's.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
read = lambda p: (ROOT / p).read_text()


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


def body_of(text, signature, start=0):
    """The brace-balanced body of the first function whose header matches."""
    m = re.compile(signature).search(text, start)
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
shader_h = read("engine/h2shared/gl_shader.h")
uh = read("engine/h2shared/gl_uniforms.h")
uc = read("engine/h2shared/gl_uniforms.c")
rmain = read("engine/hexen2/gl_rmain.c")
mesh = read("engine/h2shared/gl_mesh.c")
alias_inc = read("engine/shaders/alias.inc")
uniforms_inc = read("engine/shaders/uniforms.inc")
layout_inc = read("engine/shaders/layout.inc")
vpath = ROOT / "engine/shaders/sskeletal_vert.glsl"
vert = vpath.read_text() if vpath.exists() else ""
vcode = strip(vert)
if not vert:
    errors.append("engine/shaders/sskeletal_vert.glsl is missing")

# ------------------------------------------------------------ the mirrors
SIZE = {"mat4": 64, "vec4": 16, "ivec4": 16}
KIND = {"mat4": "float", "vec4": "float", "ivec4": "int"}
NAMES = {
    "r_aliasvert_t": {"u_avlightview": "lightview", "u_avshade": "shade", "u_avlight": "light"},
    "r_vertparams_t": {"u_mvpm": "mvp", "u_modelviewm": "modelview", "u_modelm": "model",
                       "u_viewprojm": "viewproj", "u_vscene": "scene", "u_vpup": "pup",
                       "u_vpright": "pright", "u_vvpn": "vpn", "u_vporigin": "porigin",
                       "u_vposei": "posei", "u_valias": "alias"},
}


def check_mirror(glsl_text, block, cstruct, total):
    vb = re.search(rf"uniform {block} \{{(.*?)\}};", strip(glsl_text), re.S)
    if not vb:
        errors.append(f"no uniform block {block}")
        return
    if re.search(r"\b(float|int|uint|bool|vec2|vec3|ivec2|ivec3|mat3)\s+\w+\s*;", vb.group(1)):
        errors.append(f"{block} gained a non-vec4/mat4 member; std140 offsets are no longer sums of sizes")
    g = [(n, SIZE[t], KIND[t]) for t, n in re.findall(r"\b(mat4|ivec4|vec4)\s+(\w+)\s*;", vb.group(1))]
    cm = re.search(rf"typedef struct \{{([^{{}}]*)\}} {cstruct};", uh)
    if not cm:
        errors.append(f"no {cstruct} in gl_uniforms.h")
        return
    c = [(n, 4 * int(k), t) for t, n, k in re.findall(r"\b(float|int)\s+(\w+)\[(\d+)\];", cm.group(1))]
    if [(s, k) for _, s, k in g] != [(s, k) for _, s, k in c]:
        errors.append(f"{block} {g} and {cstruct} {c} differ in size/kind/order")
    if sum(s for _, s, _ in g) != total:
        errors.append(f"{block} is {sum(s for _, s, _ in g)} bytes, expected {total}")
    want = [NAMES[cstruct].get(n, f"?{n}") for n, _, _ in g]
    if want != [n for n, _, _ in c]:
        errors.append(f"{cstruct} fields {[n for n, _, _ in c]} are not the C names of {block}'s members, in order, {want}")


check_mirror(alias_inc, "AliasVert", "r_aliasvert_t", 96)
check_mirror(uniforms_inc, "VertParams", "r_vertparams_t", 368)
if "COMPILE_TIME_ASSERT(aliasvert_size, sizeof(r_aliasvert_t) == 64 + 2 * 16);" not in uc:
    errors.append("COMPILE_TIME_ASSERT must pin r_aliasvert_t at 64 + 2*16")
if "RU_CheckBlockSize (program, RU_NAME_AVERT, sizeof(r_aliasvert_t));" not in uc:
    errors.append("R_BindAliasVertBlock must check the driver's AliasVert size against sizeof(r_aliasvert_t)")

# ------------------------------------------------------------ names, lanes, setters
uv = re.search(r"#ifdef UNIFORMS_VERT\n(.*?)#endif", alias_inc, re.S)
uvt = uv.group(1) if uv else ""
if not re.search(r"VERT_UBO\(1\) uniform AliasVert", uvt):
    errors.append("AliasVert must be VERT_UBO(1) under #ifdef UNIFORMS_VERT")
# old loose name -> (include text, member expression, C slot, setter, draw-site write)
READS = {
    "u_mvp":          (uniforms_inc, "u_mvpm",        "mvp",       "R_SetMVP",         r"R_SetMVP \(mvp\);"),
    "u_modelview":    (uniforms_inc, "u_modelviewm",  "modelview", "R_SetModelView",   r"R_SetModelView \(mv\);"),
    "u_alias_model":  (uniforms_inc, "u_modelm",      "model",     "R_SetModelMatrix", r"R_SetModelMatrix \(model\);"),
    "u_alias_dlight": (uniforms_inc, "u_valias.x",    "alias[0]",  "R_SetAliasDlight", r"R_SetAliasDlight \(GL_GetAliasDlight \(\)\);"),
    "u_pose_base":    (uniforms_inc, "u_vposei.x",    "posei[0]",  "R_SetSkelPose",
                       r"R_SetSkelPose \(posenum \* gm->numbones, prevposenum \* gm->numbones, lerpfrac\);"),
    "u_pose_base2":   (uniforms_inc, "u_vposei.w",    "posei[3]",  "R_SetSkelPose", None),
    "u_skel_blend":   (uniforms_inc, "u_valias.y",    "alias[1]",  "R_SetSkelPose", None),
    "u_shadevector":  (uvt,          "u_avshade.xyz", "shade",     "R_SetSkelLight",
                       r"R_SetSkelLight \(shadevector, light, model_fullbright_pass \? 1\.0f : 0\.0f\);"),
    "u_fullbright":   (uvt,          "u_avshade.w",   "shade",     "R_SetSkelLight", None),
    "u_lightcolor":   (uvt,          "u_avlight",     "light",     "R_SetSkelLight", None),
}
for old, (text, new, _, _, _) in READS.items():
    if not re.search(rf"#define\s+{old}\s+{re.escape(new)}\s*\n", text):
        errors.append(f"{old} must alias {new}")
# The whole file, not just from BonePose down: a helper above it reading a
# member nobody on this path writes is the same leak.  vcode holds no block
# declaration (those are the includes'), so every u_ name in it is a read.
used = set(re.findall(r"\bu_\w+", vcode))
if used != set(READS):
    errors.append(f"sskeletal_vert.glsl reads {sorted(used)}; this check (and the draw it pins) "
                  f"covers exactly {sorted(READS)}")

SETTERS = {
    "R_SetSkelPose": (r"\(int base, int base2, float blend\)",
                      [r"\bvertparams\.posei\[0\] = base;", r"\bvertparams\.posei\[3\] = base2;",
                       r"\bvertparams\.alias\[1\] = blend;", r"\bvert_dirty = true;"], "vert_dirty"),
    "R_SetSkelLight": (r"\(const float \*shadevec3, const float \*light4, float fullbright\)",
                       [r"\bRU_Set4\(aliasvert\.shade, shadevec3\[0\], shadevec3\[1\], shadevec3\[2\], fullbright\)",
                        r"\bRU_Set4\(aliasvert\.light, light4\[0\], light4\[1\], light4\[2\], light4\[3\]\)",
                        r"\bavert_dirty = true;"], "avert_dirty"),
    "R_SetMVP": (r"\(const float \*m16\)", [r"\bRU_SetMat\(vertparams\.mvp, m16\)"], "vert_dirty"),
    "R_SetModelView": (r"\(const float \*m16\)", [r"\bRU_SetMat\(vertparams\.modelview, m16\)"], "vert_dirty"),
    "R_SetModelMatrix": (r"\(const float \*m16\)", [r"\bRU_SetMat\(vertparams\.model, m16\)"], "vert_dirty"),
    "R_SetAliasDlight": (r"\(float scale\)", [r"\bvertparams\.alias\[0\] = scale;"], "vert_dirty"),
}
for fn, (sig, wants, flag) in SETTERS.items():
    b = body_of(uc, rf"\nvoid {fn} {sig}\s*\{{")
    if b is None:
        errors.append(f"gl_uniforms.c: no void {fn} {sig.replace(chr(92), '')}")
        continue
    sb = strip(b)
    for w in wants:
        if not re.search(w, sb):
            errors.append(f"{fn} must write {w}")
    # A setter that can leave early writes part of its set and reports the
    # call done: the draw's unconditional call is then only half a write.
    if re.search(r"\b(?:return|goto|longjmp)\b", sb):
        errors.append(f"{fn} has an early exit; every call must reach its writes")
    flags = set(re.findall(r"\b\w+_dirty\b", sb))
    if flags != {flag}:
        errors.append(f"{fn} names dirty flags {sorted(flags)}; it owns one block, so exactly {{{flag!r}}}")
    if fn == "R_SetSkelPose" and not re.search(
            r"\bif \(vertparams\.posei\[0\] != base \|\| vertparams\.posei\[3\] != base2\s*\|\| vertparams\.alias\[1\] != blend\)",
            sb):
        errors.append("R_SetSkelPose must compare all three lanes before skipping the write")
    if fn == "R_SetSkelLight" and not re.search(r"\bif \(changed\)\s*\bavert_dirty = true;", sb):
        errors.append("R_SetSkelLight must mark AliasVert dirty when either vec4 changed")
if re.search(r"\bR_SetPoseBase\b", strip(uc) + strip(uh)):
    errors.append("R_SetPoseBase is back: it writes the pose base without base2 and blend")


def functions(code):
    """(name, open, close) of every depth-0 brace block whose header ends in a
    parameter list -- the function definitions of a comment-stripped C file."""
    out, depth, start, last = [], 0, 0, 0
    for i, ch in enumerate(code):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                hdr = code[last:start]
                m = re.search(r"\b(\w+)\s*\([^;{}]*\)\s*$", hdr)
                out.append((m.group(1) if m else None, start, i))
                last = i + 1
        elif ch == ";" and depth == 0:
            last = i + 1
    return out


def enclosing_fn(fns, pos):
    for name, a, z in fns:
        if a < pos < z:
            return name or "?"
    return None


# One owner per lane: a second writer of posei[3], alias[1] or the skeletal
# AliasVert members would leak into the next skeletal draw's unwritten half.
# A reference counts as a READ only when it is one literal lane, not assigned,
# incremented or address-taken; anything else -- an assignment, a compound
# assignment, the bare member decaying to a pointer (memcpy/memset/RU_Set*
# targets), &lane -- is a write, and only the owner (or R_Uniforms_Init's
# zeroing) may make it.
ucode = strip(uc)
ufns = functions(ucode)
LANES = {("vertparams", "posei", 0): "R_SetSkelPose", ("vertparams", "posei", 3): "R_SetSkelPose",
         ("vertparams", "alias", 0): "R_SetAliasDlight", ("vertparams", "alias", 1): "R_SetSkelPose",
         ("aliasvert", "shade", None): "R_SetSkelLight", ("aliasvert", "light", None): "R_SetSkelLight"}
ASSIGN = r"\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)"
for struct, member in sorted({(s, m) for s, m, _ in LANES}):
    for m in re.finditer(rf"\b{struct}\s*\.\s*{member}\b(?:\s*\[\s*([^\]]*?)\s*\])?", ucode):
        idx = m.group(1)
        before = ucode[:m.start()].rstrip()
        write = (idx is None or not idx.isdigit()
                 or re.match(ASSIGN, ucode[m.end():]) is not None
                 or re.search(r"(?<!&)&$|\+\+$|--$", before) is not None)
        if not write:
            continue
        lanes = [k for k in LANES if k[:2] == (struct, member)
                 and (k[2] is None or idx is None or not idx.isdigit() or int(idx) == k[2])]
        fn = enclosing_fn(ufns, m.start())
        for lane in lanes:
            if fn != LANES[lane] and fn != "R_Uniforms_Init":
                errors.append(f"{struct}.{member}{'' if lane[2] is None else f'[{lane[2]}]'} is written "
                              f"({m.group(0)!r}) in {fn or 'file scope'}, not only {LANES[lane]}")
# The whole struct as a pointer is a write of every lane at once.  Its
# declaration, sizeof, the flush's upload and R_Uniforms_Init's zeroing are
# the only legitimate whole-struct references.
for struct in ("vertparams", "aliasvert"):
    t = re.sub(rf"\bstatic r_\w+_t\s+{struct}\s*;", " ", ucode)
    t = re.sub(rf"\bglBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\({struct}\), &{struct},", "@", t)
    t = re.sub(rf"\bsizeof\s*\(\s*{struct}\s*\)", "@", t)
    tf = functions(t)
    for m in re.finditer(rf"\b{struct}\b(?!\s*\.)", t):
        fn = enclosing_fn(tf, m.start())
        if fn != "R_Uniforms_Init":
            errors.append(f"{struct} is referenced whole in {fn or 'file scope'}; a pointer to it can write any lane")
for fn in ("R_SetSkelPose", "R_SetSkelLight"):
    for f in sorted((ROOT / "engine").rglob("*.c")):
        if f.name == "gl_uniforms.c":
            continue
        t = strip(f.read_text(errors="replace"))
        if not re.search(rf"\b{fn}\b", t):
            continue
        # Brace structure, not "the last void name ( above": a static helper
        # or a non-void function between them would otherwise be misattributed.
        tfns = functions(t)
        for m in re.finditer(rf"\b{fn}\b", t):
            enc = enclosing_fn(tfns, m.start())
            if enc != "GL_DrawAliasSkeletal":
                errors.append(f"{fn} is referenced from {f.relative_to(ROOT)}:{enc or 'file scope'}; "
                              "only GL_DrawAliasSkeletal writes the skeletal lanes")

# ------------------------------------------------------------ the file
if re.search(r"^\s*#\s*version\b", vert, re.M):
    errors.append("sskeletal_vert.glsl carries a #version; the header chunk supplies it per tier")
if '#include "layout.inc"\n#define UNIFORMS_VERT\n#include "uniforms.inc"\n#include "alias.inc"' not in vert:
    errors.append("sskeletal_vert.glsl must include layout.inc, then uniforms.inc with UNIFORMS_VERT, then alias.inc")
if re.search(r"^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?!sampler|usampler|isampler)\w+\s+\w+\s*(?:\[\w*\])?\s*;", vcode, re.M):
    errors.append("sskeletal_vert.glsl still declares a loose non-opaque uniform")
if re.search(r"\b[iu]?sampler\w*\s+\w+\s*;", vcode):
    errors.append("sskeletal_vert.glsl samples a texture: its bones are no longer Vulkan set 0 index 0")
if not re.search(r"VERT_SSBO\(4, 0\) restrict readonly buffer BoneBuffer \{\s*mat3x4 bones\[\];\s*\}", vcode):
    errors.append("the bones must be VERT_SSBO(4, 0) restrict readonly buffer BoneBuffer { mat3x4 bones[]; }")
if re.search(r"layout\s*\(\s*std430", vcode):
    errors.append("sskeletal_vert.glsl spells a raw layout(std430...); use VERT_SSBO so Vulkan gets its set")
graphics = layout_inc[layout_inc.index("#define VERT_IN(loc)"):] if "#define VERT_IN(loc)" in layout_inc else ""
ssbo_arms = re.findall(r"#define VERT_SSBO\(gl, vk\) (.*)", graphics)
if ssbo_arms != ["layout(std430, set = 0, binding = vk)", "layout(std430, binding = gl)", "layout(std430, binding = gl)"]:
    errors.append(f"layout.inc VERT_SSBO arms (Vulkan, GL_SPIRV, plain) changed: {ssbo_arms}")
# The plain arm must keep the location: without it GL_LinkProgram's
# glBindAttribLocation would put a_texcoord on ATTR_TEXCOORD, the VAO's normal.
in_arms = re.findall(r"#define VERT_IN\(loc\)\s+(.*)", layout_inc)
if len(in_arms) != 3 or any(a.strip() != "layout(location = loc)" for a in in_arms):
    errors.append(f"every arm of layout.inc's VERT_IN must be layout(location = loc), is {in_arms}")
if "invariant gl_Position;" not in vcode:
    errors.append("sskeletal_vert.glsl lost `invariant gl_Position` (uhexen2-iir3)")
if not re.search(r"#ifdef NOPERSP\s*#define TCQUAL noperspective\s*#else\s*#define TCQUAL\s*#endif", vcode):
    errors.append("sskeletal_vert.glsl must define TCQUAL from NOPERSP")

# The body, pinned as a token stream (comments and whitespace do not count).
# Derived from the file at S4 and token-identical to the inline literal it
# replaced, GLSL_ALIAS_DLIGHT_VS_CALC's desktop arm expanded; the u_ names
# resolve through the aliases checked above, of the loose uniforms' types.
# A DELIBERATE shader change updates this golden in the same commit.
GOLDEN = {
    r"\bmat3x4\s+BonePose\s*\(":
        "mat3x4 BonePose ( uint i ) { return bones [ u_pose_base + int ( i ) ] * u_skel_blend + "
        "bones [ u_pose_base2 + int ( i ) ] * ( 1.0 - u_skel_blend ) ; }",
    r"\bvoid\s+main\s*\(":
        "void main ( ) { mat3x4 blend = BonePose ( a_indices . x ) * a_weights . x ; "
        "if ( a_weights . y > 0.0 ) blend += BonePose ( a_indices . y ) * a_weights . y ; "
        "if ( a_weights . z > 0.0 ) blend += BonePose ( a_indices . z ) * a_weights . z ; "
        "if ( a_weights . w > 0.0 ) blend += BonePose ( a_indices . w ) * a_weights . w ; "
        "mat4x3 mat_3x4 = transpose ( blend ) ; "
        "vec3 skinned_pos = mat_3x4 * vec4 ( a_position , 1.0 ) ; "
        "vec3 skinned_normal = normalize ( mat_3x4 * vec4 ( a_normal . xyz , 0.0 ) ) ; "
        "float d = dot ( skinned_normal , u_shadevector ) ; "
        "if ( d < 0.0 ) d *= 0.3 ; "
        "vec3 lit = u_lightcolor . rgb * ( 1.0 + d ) ; "
        "v_texcoord = a_texcoord ; "
        "v_color = vec4 ( mix ( lit , vec3 ( 1.0 ) , u_fullbright ) , u_lightcolor . a ) ; "
        "v_worldxy = ( u_alias_model * vec4 ( skinned_pos , 1.0 ) ) . xy ; "
        "vec4 eyepos = u_modelview * vec4 ( skinned_pos , 1.0 ) ; "
        "v_fogdist = length ( eyepos . xyz ) ; "
        "v_matpos = eyepos . xyz ; "
        "v_eyepos = eyepos . xyz ; "
        "v_dlightscale = u_alias_dlight ; "
        "gl_Position = u_mvp * vec4 ( skinned_pos , 1.0 ) ; }",
}
TOK = r"\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+|\w+|[-+*/%<>=!&|^]=|&&|\|\||\+\+|--|<<|>>|\S"
for head, want in GOLDEN.items():
    hm = list(re.finditer(head, vcode))
    body = body_of(vcode, head) if len(hm) == 1 else None
    got = ""
    if body:
        start = hm[0].start()
        got = " ".join(re.findall(TOK, vcode[start:vcode.index("{", start) + len(body)]))
        # layout.inc's VK_FLIP_Y() is empty outside the Vulkan arm, so GL sees
        # no token; tools/test_vk_flip_y.py owns that call (#340).
        if got.endswith(" VK_FLIP_Y ( ) }"):
            got = got[:-len(" VK_FLIP_Y ( ) }")] + " }"
    if got != want:
        errors.append(f"sskeletal_vert.glsl's {want.split(' ( ')[0]} differs from the pinned body "
                      f"(found {len(hm)} definitions); a deliberate change updates GOLDEN with it.\n"
                      f"    got:  {got}\n    want: {want}")

# Every input against the IQM VAO GL_MakeAliasGPUMesh builds: same slot, same
# width, and an integer attribute exactly where the VAO uses the I pointer.
WIDTH = {"float": 1, "vec2": 2, "vec3": 3, "vec4": 4, "uvec4": 4, "ivec4": 4}
decl_in = re.findall(r"^\s*(?:VERT_IN\((\d+)\)\s+)?in\s+(\w+)\s+(\w+)\s*;", vcode, re.M)
if not decl_in:
    errors.append("sskeletal_vert.glsl: no vertex inputs found")
iqm = mesh[mesh.find("iqm_gpu_setup:"):] if "iqm_gpu_setup:" in mesh else ""
iqm = iqm[:iqm.find("glBindVertexArray_fp(0);")] if "glBindVertexArray_fp(0);" in iqm else ""
vao = {int(i): (int(n), bool(isint)) for isint, i, n in
       re.findall(r"glVertexAttrib(I?)Pointer_fp\((\d+), (\d+),", strip(iqm))}
enabled = {int(i) for i in re.findall(r"glEnableVertexAttribArray_fp\((\d+)\)", strip(iqm))}
if len(vao) != 5 or enabled != set(vao):
    errors.append(f"cannot read the IQM VAO's five attributes from gl_mesh.c (pointers {vao}, enabled {enabled})")
seen = set()
for loc, typ, name in decl_in:
    if not loc:
        errors.append(f"input {name} has no VERT_IN: glBindAttribLocation would place it by the ATTR_* contract")
        continue
    loc = int(loc)
    seen.add(loc)
    if loc not in vao:
        errors.append(f"input {name} is VERT_IN({loc}); the IQM VAO has no attribute {loc}")
        continue
    n, isint = vao[loc]
    if WIDTH.get(typ) != n:
        errors.append(f"input {name} is {typ}, the VAO's attribute {loc} has {n} components")
    if isint != typ.startswith(("u", "i")):
        errors.append(f"input {name} is {typ} but the VAO feeds attribute {loc} with "
                      f"glVertexAttrib{'I' if isint else ''}Pointer -- the bones would be int-to-float converted")
if vao and seen != set(vao):
    errors.append(f"the VAO feeds attributes {sorted(vao)}, the stage reads {sorted(seen)}")
attr = dict(re.findall(r"#define\s+(ATTR_\w+)\s+(\d+)", shader_h))
if attr.get("ATTR_TEXCOORD") == "2":
    errors.append("ATTR_TEXCOORD became 2; the comment in sskeletal_vert.glsl on why its slots differ is stale")

# ------------------------------------------------------------ the C side's builds
if re.search(r"static const char sskeletal_vert\[\]", gl_shader):
    errors.append("the inline sskeletal_vert literal is back in gl_shader.c")
for gone in ("GL_SpliceDefines", "GLSL_ALIAS_DLIGHT_VS_OUT", "GLSL_ALIAS_DLIGHT_VS_CALC"):
    if re.search(rf"\b{gone}\b", strip(gl_shader)):
        errors.append(f"{gone} is back in gl_shader.c; its last user was the skeletal literal")
NP = r'GLSL_VERT_HEADER "#define NOPERSP 1\\n", sskeletal_vert,\s*GLSL_ALIAS_FRAG_HEADER "#define NOPERSP 1\\n", salias_frag\);'
BUILDS = {
    "skeletal": r'GL_InitProgramSplit\(&gl_shader_skeletal, "skeletal", GLSL_VERT_HEADER, sskeletal_vert,\s*GLSL_ALIAS_FRAG_HEADER, salias_frag\);',
    "skeletal_np": r'GL_InitProgramSplit\(&gl_shader_skeletal_np, "skeletal_np",\s*' + NP,
    "skeletal_oit": r'GL_InitOITProgramSplit\(&gl_shader_skeletal_oit, "skeletal", GLSL_VERT_HEADER, sskeletal_vert,\s*GLSL_ALIAS_FRAG_HEADER, salias_frag\);',
    "skeletal_np_oit": r'GL_InitOITProgramSplit\(&gl_shader_skeletal_np_oit, "skeletal_np",\s*' + NP,
}
gsc = strip(gl_shader)


def desktop_only(pos):
    """True when `pos` sits inside an #ifndef USE_GLES region (no #else between)."""
    stack = []
    for m in re.finditer(r"^\s*#\s*(ifndef|ifdef|if|else|elif|endif)\b(.*)$", gsc[:pos], re.M):
        d, rest = m.group(1), m.group(2).strip()
        if d in ("ifndef", "ifdef", "if"):
            stack.append([d == "ifndef" and rest == "USE_GLES"])
        elif d in ("else", "elif") and stack:
            stack[-1][0] = False
        elif d == "endif" and stack:
            stack.pop()
    return any(s[0] for s in stack)


for name, b in BUILDS.items():
    found = list(re.finditer(b, gsc))
    if len(found) != 1:
        errors.append(f"{name}: expected one build {b[:70]}..., found {len(found)}")
    elif not desktop_only(found[0].start()):
        errors.append(f"{name} is built outside #ifndef USE_GLES; shader storage blocks are not ES 3.00")
if len(re.findall(r"\bsskeletal_vert\b", gsc)) != 4:
    errors.append(f"sskeletal_vert is referenced {len(re.findall(r'sskeletal_vert', gsc))} times in gl_shader.c code, expected the 4 builds")

gp = re.search(r"typedef struct glprogram_s \{(.*?)\} glprogram_t;", shader_h, re.S)
gpf = set(re.findall(r"GLint\s+(\w+)", strip(gp.group(1)))) if gp else set()
dead = gpf & {"u_pose_base", "u_pose_base2", "u_skel_blend", "u_shadevector", "u_lightcolor", "u_fullbright"}
if dead:
    errors.append(f"glprogram_t still carries skeletal locations that are -1 now: {sorted(dead)}")
for n in ("u_pose_base", "u_pose_base2", "u_skel_blend", "u_shadevector", "u_lightcolor", "u_fullbright"):
    if re.search(rf'glGetUniformLocation_fp\([^)]*"{n}"\)', gsc):
        errors.append(f"gl_shader.c still looks up the loose {n}")
gipu = strip(body_of(gl_shader, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or "")
if "R_BindAliasVertBlock(p->program);" not in gipu:
    errors.append("GL_InitProgramUniforms must bind AliasVert, or the skeletal programs read block 0")

# ------------------------------------------------------------ the draw
sig = r"static void GL_DrawAliasSkeletal \(entity_t \*e, aliashdr_t \*paliashdr,\s*int posenum, int prevposenum, float lerpfrac\)\s*\{"
raw = body_of(rmain, sig) or ""
draw = strip(raw)
if "glDrawElements_fp" not in draw:
    errors.append("cannot find the desktop GL_DrawAliasSkeletal (the first definition, with the draw)")
else:
    if re.search(r"\bglUniform\w*_fp\b", draw):
        errors.append("GL_DrawAliasSkeletal still makes a loose glUniform write; both stages read blocks now")
    if re.search(r"prog->u_\w+", draw):
        errors.append("GL_DrawAliasSkeletal still reads a per-program uniform location")
    flushes = [m.start() for m in re.finditer(r"\bR_FlushUniforms \(\);", draw)]
    dm = re.search(r"\bglDrawElements_fp \(GL_TRIANGLES, gm->num_indices, GL_UNSIGNED_SHORT, NULL\);", draw)
    if len(flushes) != 1 or not dm:
        errors.append(f"GL_DrawAliasSkeletal must flush exactly once ({len(flushes)}) and draw gm->num_indices")
    else:
        fpos, dpos = flushes[0], dm.start()

        def depth_at(i):
            return draw[:i].count("{") - draw[:i].count("}")

        def stmt_prefix(i):
            """The text between the statement delimiter (; { } outside parens)
            before `i` and `i`, preprocessor lines dropped."""
            parens, j = 0, i - 1
            while j > 0:
                c = draw[j]
                if c == ")":
                    parens += 1
                elif c == "(":
                    parens -= 1
                elif c in ";{}" and parens == 0:
                    break
                j -= 1
            return re.sub(r"(?m)^\s*#.*$", " ", draw[j + 1:i]).strip()

        def conditional(i):
            """Why the statement at `i` can be skipped, or None.  Its own
            statement must start with it (no braceless if/else/for/while/
            switch/case, no ?: or && around it), every enclosing block must be
            a plain scoping block standing as a statement on its own, and no
            #if may be open around it."""
            pre = stmt_prefix(i)
            if pre:
                return f"it is governed by {pre!r}"
            d, j = 0, i - 1
            while j > 0:                       # draw[0] is the function's own brace
                if draw[j] == "}":
                    d += 1
                elif draw[j] == "{":
                    if d == 0:
                        pre = stmt_prefix(j)
                        if pre:
                            return f"its enclosing block is the body of {pre!r}"
                    else:
                        d -= 1
                j -= 1
            pp = 0
            for m in re.finditer(r"(?m)^\s*#\s*(if|ifdef|ifndef|endif)\b", draw[:i]):
                pp += -1 if m.group(1) == "endif" else 1
            if pp:
                return "it sits inside a preprocessor conditional"
            return None
        if fpos > dpos:
            errors.append("GL_DrawAliasSkeletal flushes after its draw")
        for what, at in (("R_FlushUniforms", fpos), ("glDrawElements_fp", dpos)):
            why = conditional(at)
            if why:
                errors.append(f"GL_DrawAliasSkeletal's {what} must be unconditional: {why}")
        if depth_at(fpos) != depth_at(dpos):
            errors.append("GL_DrawAliasSkeletal's R_FlushUniforms must be at the draw's brace depth")
        clamp = re.search(r"\bif \(posenum\s+< 0 \|\| posenum\s+>= gm->numposes\) posenum\s+= 0;\s*"
                          r"if \(prevposenum < 0 \|\| prevposenum >= gm->numposes\) prevposenum = posenum;", draw)
        if not clamp:
            errors.append("GL_DrawAliasSkeletal lost its pose clamp (an out-of-range pose reads past the bone SSBO)")
        # The two early returns (no mesh, no program) precede the clamp; past
        # it nothing may leave, or a conditional exit skips setters and flush.
        if clamp and re.search(r"\b(?:return|goto|break|continue|longjmp)\b", draw[clamp.end():dpos]):
            errors.append("GL_DrawAliasSkeletal can leave between its pose clamp and its draw")
        for old, (_, _, _, setter, w) in READS.items():
            if w is None:
                continue
            ms = list(re.finditer(r"\b" + w, draw))
            if len(ms) != 1:
                errors.append(f"GL_DrawAliasSkeletal must write {old} once through {w} (found {len(ms)})")
                continue
            p = ms[0].start()
            if p > fpos:
                errors.append(f"GL_DrawAliasSkeletal writes {old} after its flush")
            why = conditional(p)
            if why:
                errors.append(f"GL_DrawAliasSkeletal's {setter} write is conditional: {why}")
            if clamp and setter == "R_SetSkelPose" and p < clamp.end():
                errors.append("GL_DrawAliasSkeletal writes the pose bases before clamping them")
        # Which getter fills each matrix: the variable is declared, filled by
        # its own getter, unconditionally, and handed to its setter, with no
        # other mention -- so a swapped fetch or a second writer fails.
        for var, getter, setter in (("mvp", "GL_GetMVP", "R_SetMVP"), ("mv", "GL_GetModelview", "R_SetModelView"),
                                    ("model", "GL_GetAliasModelMatrix", "R_SetModelMatrix")):
            occ = list(re.finditer(rf"\b{var}\b", draw))
            want = (rf"{var}\[16\]", rf"(?<=\b{getter} \(){var}\);", rf"(?<=\b{setter} \(){var}\);")
            if len(occ) != 3 or not all(re.compile(w).match(draw, o.start()) for w, o in zip(want, occ)):
                errors.append(f"GL_DrawAliasSkeletal must declare {var}[16], fill it with {getter} ({var}) and pass "
                              f"it to {setter} ({var}), mentioning it nowhere else (found {len(occ)} mentions)")
                continue
            why = conditional(draw.rindex(getter, 0, occ[1].start()))
            if why:
                errors.append(f"GL_DrawAliasSkeletal's {getter} is conditional: {why}")
        # The light vector the loose u_lightcolor write built, slot for slot.
        lw = re.search(r"\{\s*float\s+light\[4\];\s*light\[0\] = lightcolor\[0\] \* r;\s*light\[1\] = lightcolor\[1\] \* g;\s*"
                       r"light\[2\] = lightcolor\[2\] \* b;\s*light\[3\] = model_constant_alpha;\s*R_SetSkelLight \(", draw)
        if not lw:
            errors.append("GL_DrawAliasSkeletal must build light[] as (lightcolor * tint, model_constant_alpha) "
                          "immediately before R_SetSkelLight")
        bind = re.search(r"\bGL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 4, gm->ssbo_bones\);", draw)
        unbind = re.search(r"\bGL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 4, 0\);", draw)
        if not bind or not unbind or not (bind.start() < dpos < unbind.start()):
            errors.append("GL_DrawAliasSkeletal must bind gm->ssbo_bones at 4 (the file's VERT_SSBO gl slot) "
                          "before the draw and unbind it after")
        use = re.search(r"\bR_UseProgram \(prog->program\);", draw)
        if not use or use.start() > fpos:
            errors.append("GL_DrawAliasSkeletal must bind its program before the flush and draw")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS sskeletal_vert reads VertParams + AliasVert; mirrors, lanes, file, VAO, builds and the draw's writes agree (#450 S4)")
