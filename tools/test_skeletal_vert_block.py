#!/usr/bin/env python3
"""sskeletal_vert reads its uniforms from VertParams and AliasVert (#450, alias S4).

Checks the pieces that have to move together:
  * AliasVert in engine/shaders/alias.inc and r_aliasvert_t in gl_uniforms.h,
    and VertParams and r_vertparams_t, agree member for member by name, size
    and int/float kind (96 and 368 bytes), with COMPILE_TIME_ASSERT pinning
    the AliasVert mirror and the driver check sized from it;
  * every old loose name the stage reads aliases the member its setter writes,
    the stage reads exactly that set, and no lane is written by two setters
    (u_vposei.w and u_valias.y are spare lanes of members other setters own);
  * engine/shaders/sskeletal_vert.glsl has no #version, declares no loose
    non-opaque uniform, puts its bones on VERT_SSBO(4, 0) -- GL binding 4,
    which GL_DrawAliasSkeletal binds, and Vulkan set 0 index 0, since the
    stage samples no texture -- and gives every input an explicit VERT_IN
    equal to the slot, width and integer-ness of the IQM VAO in gl_mesh.c,
    which is NOT the ATTR_* contract the other programs share;
  * the literal, GL_SpliceDefines and the GLSL_ALIAS_DLIGHT_VS_* splices are
    gone, all four skeletal programs compile the file through
    GLSL_VERT_HEADER (NOPERSP appended for the two affine ones) inside a
    desktop-only region, glprogram_t carries ub_avert and none of the six
    skeletal-only locations, and GL_InitProgramUniforms binds AliasVert;
  * GL_DrawAliasSkeletal makes no loose glUniform write, writes every member
    the stage reads (each through its setter, from the values the loose
    writes used) after the pose clamp and before one unconditional
    R_FlushUniforms at the draw's own brace depth, then binds the bones and
    draws -- a block member is global, so a value left by an earlier draw is
    a bug, where the loose uniform was this program's own;
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
main = vcode[vcode.find("mat3x4 BonePose"):] if "mat3x4 BonePose" in vcode else ""
used = set(re.findall(r"\bu_\w+", main))
if used != set(READS):
    errors.append(f"sskeletal_vert.glsl reads {sorted(used)}; this check (and the draw it pins) "
                  f"covers exactly {sorted(READS)}")

SETTERS = {
    "R_SetSkelPose": (r"\(int base, int base2, float blend\)",
                      [r"vertparams\.posei\[0\] = base;", r"vertparams\.posei\[3\] = base2;",
                       r"vertparams\.alias\[1\] = blend;", r"vert_dirty = true;"]),
    "R_SetSkelLight": (r"\(const float \*shadevec3, const float \*light4, float fullbright\)",
                       [r"RU_Set4\(aliasvert\.shade, shadevec3\[0\], shadevec3\[1\], shadevec3\[2\], fullbright\)",
                        r"RU_Set4\(aliasvert\.light, light4\[0\], light4\[1\], light4\[2\], light4\[3\]\)",
                        r"avert_dirty = true;"]),
    "R_SetMVP": (r"\(const float \*m16\)", [r"RU_SetMat\(vertparams\.mvp, m16\)"]),
    "R_SetModelView": (r"\(const float \*m16\)", [r"RU_SetMat\(vertparams\.modelview, m16\)"]),
    "R_SetModelMatrix": (r"\(const float \*m16\)", [r"RU_SetMat\(vertparams\.model, m16\)"]),
    "R_SetAliasDlight": (r"\(float scale\)", [r"vertparams\.alias\[0\] = scale;"]),
}
for fn, (sig, wants) in SETTERS.items():
    b = body_of(uc, rf"\nvoid {fn} {sig}\s*\{{")
    if b is None:
        errors.append(f"gl_uniforms.c: no void {fn} {sig.replace(chr(92), '')}")
        continue
    for w in wants:
        if not re.search(w, strip(b)):
            errors.append(f"{fn} must write {w}")
    if fn == "R_SetSkelPose" and not re.search(
            r"if \(vertparams\.posei\[0\] != base \|\| vertparams\.posei\[3\] != base2\s*\|\| vertparams\.alias\[1\] != blend\)",
            strip(b)):
        errors.append("R_SetSkelPose must compare all three lanes before skipping the write")
    if fn == "R_SetSkelLight" and not re.search(r"if \(changed\)\s*avert_dirty = true;", strip(b)):
        errors.append("R_SetSkelLight must mark AliasVert dirty when either vec4 changed")
if re.search(r"\bR_SetPoseBase\b", strip(uc) + strip(uh)):
    errors.append("R_SetPoseBase is back: it writes the pose base without base2 and blend")
# One owner per lane: a second writer of posei[3], alias[1] or the skeletal
# AliasVert members would leak into the next skeletal draw's unwritten half.
ucode = strip(uc)
for pat, owner in ((r"vertparams\.posei\[3\]\s*=", "R_SetSkelPose"), (r"vertparams\.alias\[1\]\s*=", "R_SetSkelPose"),
                   (r"vertparams\.alias\[0\]\s*=", "R_SetAliasDlight"), (r"vertparams\.posei\[0\]\s*=", "R_SetSkelPose"),
                   (r"aliasvert\.(?:shade|light)\b", "R_SetSkelLight")):
    for m in re.finditer(pat, ucode):
        fn = re.findall(r"\nvoid (\w+) \(", ucode[:m.start()])
        if not fn or fn[-1] != owner:
            if fn and fn[-1] == "R_Uniforms_Init":
                continue
            errors.append(f"{pat} is written in {fn[-1] if fn else '?'}, not only {owner}")
if re.search(r"RU_Set4\(vertparams\.(?:alias|posei)\b", ucode):
    errors.append("a whole-vec4 write of VertParams' alias/posei would clobber a lane another setter owns")
for fn in ("R_SetSkelPose", "R_SetSkelLight"):
    for f in sorted((ROOT / "engine").rglob("*.c")):
        if f.name == "gl_uniforms.c":
            continue
        t = strip(f.read_text(errors="replace"))
        for m in re.finditer(rf"\b{fn} \(", t):
            enc = re.findall(r"\n(?:static )?void (\w+) \(", t[:m.start()])
            if not enc or enc[-1] != "GL_DrawAliasSkeletal":
                errors.append(f"{fn} is called from {f.relative_to(ROOT)}:{enc[-1] if enc else '?'}; "
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
if "ub_avert" not in gpf:
    errors.append("glprogram_t needs ub_avert")
dead = gpf & {"u_pose_base", "u_pose_base2", "u_skel_blend", "u_shadevector", "u_lightcolor", "u_fullbright"}
if dead:
    errors.append(f"glprogram_t still carries skeletal locations that are -1 now: {sorted(dead)}")
for n in ("u_pose_base", "u_pose_base2", "u_skel_blend", "u_shadevector", "u_lightcolor", "u_fullbright"):
    if re.search(rf'glGetUniformLocation_fp\([^)]*"{n}"\)', gsc):
        errors.append(f"gl_shader.c still looks up the loose {n}")
gipu = strip(body_of(gl_shader, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or "")
if "p->ub_avert = R_BindAliasVertBlock(p->program);" not in gipu:
    errors.append("GL_InitProgramUniforms must bind AliasVert (p->ub_avert), or the skeletal programs read block 0")

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
    flushes = [m.start() for m in re.finditer(r"R_FlushUniforms \(\);", draw)]
    dm = re.search(r"glDrawElements_fp \(GL_TRIANGLES, gm->num_indices, GL_UNSIGNED_SHORT, NULL\);", draw)
    if len(flushes) != 1 or not dm:
        errors.append(f"GL_DrawAliasSkeletal must flush exactly once ({len(flushes)}) and draw gm->num_indices")
    else:
        fpos, dpos = flushes[0], dm.start()

        def depth_at(i):
            return draw[:i].count("{") - draw[:i].count("}")
        stmt = max(draw.rfind(";", 0, fpos), draw.rfind("{", 0, fpos), draw.rfind("}", 0, fpos))
        if fpos > dpos:
            errors.append("GL_DrawAliasSkeletal flushes after its draw")
        if depth_at(fpos) != depth_at(dpos) or re.search(r"\b(?:if|else|for|while)\b", draw[stmt + 1:fpos]):
            errors.append("GL_DrawAliasSkeletal's R_FlushUniforms must be unconditional, at the draw's brace depth")
        if re.search(r"\breturn\b", draw[fpos:dpos]):
            errors.append("GL_DrawAliasSkeletal can return between its flush and its draw")
        clamp = re.search(r"if \(posenum\s+< 0 \|\| posenum\s+>= gm->numposes\) posenum\s+= 0;\s*"
                          r"if \(prevposenum < 0 \|\| prevposenum >= gm->numposes\) prevposenum = posenum;", draw)
        if not clamp:
            errors.append("GL_DrawAliasSkeletal lost its pose clamp (an out-of-range pose reads past the bone SSBO)")
        for old, (_, _, _, setter, w) in READS.items():
            if w is None:
                continue
            ms = list(re.finditer(w, draw))
            if len(ms) != 1:
                errors.append(f"GL_DrawAliasSkeletal must write {old} once through {w} (found {len(ms)})")
                continue
            p = ms[0].start()
            if p > fpos:
                errors.append(f"GL_DrawAliasSkeletal writes {old} after its flush")
            if depth_at(p) > depth_at(fpos) + (1 if setter == "R_SetSkelLight" else 0):
                errors.append(f"GL_DrawAliasSkeletal's {setter} write is conditional")
            if clamp and setter == "R_SetSkelPose" and p < clamp.end():
                errors.append("GL_DrawAliasSkeletal writes the pose bases before clamping them")
        # The light vector the loose u_lightcolor write built, slot for slot.
        lw = re.search(r"\{\s*float\s+light\[4\];\s*light\[0\] = lightcolor\[0\] \* r;\s*light\[1\] = lightcolor\[1\] \* g;\s*"
                       r"light\[2\] = lightcolor\[2\] \* b;\s*light\[3\] = model_constant_alpha;\s*R_SetSkelLight \(", draw)
        if not lw:
            errors.append("GL_DrawAliasSkeletal must build light[] as (lightcolor * tint, model_constant_alpha) "
                          "immediately before R_SetSkelLight")
        bind = re.search(r"GL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 4, gm->ssbo_bones\);", draw)
        unbind = re.search(r"GL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 4, 0\);", draw)
        if not bind or not unbind or not (bind.start() < dpos < unbind.start()):
            errors.append("GL_DrawAliasSkeletal must bind gm->ssbo_bones at 4 (the file's VERT_SSBO gl slot) "
                          "before the draw and unbind it after")
        use = draw.find("R_UseProgram (prog->program);")
        if use < 0 or use > fpos:
            errors.append("GL_DrawAliasSkeletal must bind its program before the flush and draw")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS sskeletal_vert reads VertParams + AliasVert; mirrors, lanes, file, VAO, builds and the draw's writes agree (#450 S4)")
