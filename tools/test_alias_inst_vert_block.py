#!/usr/bin/env python3
"""salias_inst_vert reads its uniforms from VertParams and AliasVert (#450, alias S3).

Checks the pieces that have to move together:
  * AliasVert in engine/shaders/alias.inc and r_aliasvert_t in gl_uniforms.h
    agree member for member, by NAME as well as size and kind (96 bytes since
    S4 added the skeletal lighting), and
    VertParams and r_vertparams_t likewise -- test_alias_vert_block.py pins
    VertParams' sizes and kinds only, which let two same-shaped mat4s (model
    and viewproj) swap places and still pass;
  * each old loose name the stage reads aliases the member its setter writes:
    u_viewproj, u_eyepos, u_inst_base, u_poseverttype, u_alias_dlight in
    VertParams, u_lightview in AliasVert;
  * AliasVert has a binding point no other uniform block uses, is bound and
    uploaded by R_FlushUniforms through its own permanent buffer, re-uploaded
    by R_InvalidateUniforms, seeded with an identity, and written by
    R_SetAliasLightGrid from the same view as AliasFrag's copy;
  * engine/shaders/salias_inst_vert.glsl declares no loose non-opaque uniform,
    puts its four storage buffers on VERT_SSBO(n, n) (layout.inc: set 0 on
    Vulkan, the GL binding elsewhere), aliases gl_VertexID / gl_InstanceID on
    Vulkan, and takes the ShadedotRow constants from macros whose #ifndef
    defaults equal gl_shader.h's, which do not overlap;
  * the glow is unshaded (GitHub #491): main() drops the shade term (sdot = 1)
    for a record carrying ALIAS_INST_FULLBRIGHT_BIT, after both format arms
    compute it and before v_color takes it; R_DrawAliasInstanced's fullbright
    pass sets the bit on every record, with the flat light, before it
    re-streams them; and nothing else in gl_rmain.c sets it, so the main pass
    stays shaded;
  * the literal and GL_CompileShaderDefines are gone, both instanced programs
    compile the file with GLSL_ALIAS_INST_VERT_HEADER (the tier's version line
    plus the three constants; NOPERSP appended for the affine twin), and the
    program binds AliasVert after the memset that would clear it;
  * R_DrawAliasInstanced writes every VertParams member the stage reads before
    each pass's first flush, sets the instance base PER BATCH and flushes it
    before that batch's draw in BOTH passes (hazard 4), and makes no loose
    glUniform write at all;
  * both passes draw an MD3 batch (GitHub #485): the pose type is the batch's
    gm->poseverttype, set before the flush; a PV_MD3 batch draws from
    ssbo_pose_md3 at binding 3; no batch is skipped on ssbo_pose alone, which
    an MD3 mesh never has; and the fullbright pass unbinds binding 3 after;
  * R_LightCluster_BindForAlias no longer writes a per-program u_lightview.
The varying interface between this stage and salias_frag.glsl is
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
shader_h = read("engine/h2shared/gl_shader.h")
uh = read("engine/h2shared/gl_uniforms.h")
uc = read("engine/h2shared/gl_uniforms.c")
rmain = read("engine/hexen2/gl_rmain.c")
lc = read("engine/h2shared/gl_lightcluster.c")
alias_inc = read("engine/shaders/alias.inc")
uniforms_inc = read("engine/shaders/uniforms.inc")
layout_inc = read("engine/shaders/layout.inc")
vpath = SH / "salias_inst_vert.glsl"
vert = vpath.read_text() if vpath.exists() else ""
vcode = strip(vert)
if not vert:
    errors.append("engine/shaders/salias_inst_vert.glsl is missing")

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
        errors.append(f"{block} gained a non-vec4/mat4 member; the offset arithmetic here no longer holds")
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
uv = re.search(r"#ifdef UNIFORMS_VERT\n(.*?)#endif", alias_inc, re.S)
if not uv or not re.search(r"VERT_UBO\(1\) uniform AliasVert", uv.group(1)):
    errors.append("AliasVert must be VERT_UBO(1), after VertParams' 0, under #ifdef UNIFORMS_VERT")
uf = re.search(r"#ifdef UNIFORMS_FRAG\n(.*?)#endif", alias_inc, re.S)
if uf and "AliasVert" in strip(uf.group(1)):
    errors.append("AliasVert must not be declared to the fragment stage")

# Old loose name -> where its value lives now, and the C slot the setter fills.
ALIASES = [(uniforms_inc, "u_viewproj", "u_viewprojm"), (uniforms_inc, "u_eyepos", "u_vscene.xyz"),
           (uniforms_inc, "u_inst_base", "u_vposei.y"), (uniforms_inc, "u_poseverttype", "u_vposei.z"),
           (uniforms_inc, "u_alias_dlight", "u_valias.x"), (uv.group(1) if uv else "", "u_lightview", "u_avlightview")]
for text, old, new in ALIASES:
    if not re.search(rf"#define\s+{old}\s+{re.escape(new)}\s*\n", text):
        errors.append(f"{old} must alias {new}")
SETTERS = {
    "R_SetViewProj": [r"RU_SetMat\(vertparams\.viewproj, m16\)"],
    "R_SetInstBase": [r"vertparams\.posei\[1\] = v"],
    "R_SetPoseVertType": [r"vertparams\.posei\[2\] = v"],
    "R_SetEyePos": [r"RU_Set4\(vertparams\.scene, xyz\[0\], xyz\[1\], xyz\[2\]"],
    "R_SetAliasDlight": [r"vertparams\.alias\[0\] = scale"],
    "R_SetAliasLightGrid": [r"RU_SetMat\(aliasvert\.lightview, view16\)\)\s*avert_dirty = true;",
                            r"RU_SetMat\(aliasfrag\.lightview, view16\)"],
}
for fn, wants in SETTERS.items():
    b = body_of(uc, rf"\nvoid {fn} \([^)]*\)\s*\{{")
    if b is None:
        errors.append(f"gl_uniforms.c: no {fn}")
        continue
    for w in wants:
        if not re.search(w, b):
            errors.append(f"{fn} must write {w}")
    if fn != "R_SetAliasLightGrid" and "vert_dirty = true" not in b:
        errors.append(f"{fn} must mark VertParams dirty")

# ------------------------------------------------------------ binding and upload
bind = re.search(r"#define\s+UBO_BINDING_ALIAS_VERT\s+(\d+)", shader_h)
allbind = {}
for f in ("engine/h2shared/gl_shader.h", "engine/h2shared/gl_postprocess.c", "engine/h2shared/gl_ssao.c",
          "engine/h2shared/gl_worldcull.c", "engine/h2shared/gl_lightcluster.c"):
    for name, num in re.findall(r"#define\s+(\w*UBO_BINDING\w*)\s+(\d+)", read(f)):
        allbind.setdefault(int(num), []).append(name)
if not bind:
    errors.append("gl_shader.h: no UBO_BINDING_ALIAS_VERT")
elif bind.group(1) != "10":
    errors.append(f"UBO_BINDING_ALIAS_VERT is {bind.group(1)}; the plan on #450 holds 10 for it")
elif len(allbind.get(10, [])) != 1:
    errors.append(f"UBO binding 10 is shared: {allbind[10]}")
flush = body_of(uc, r"\nvoid R_FlushUniforms \(void\)\s*\{") or ""
if "GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_ALIAS_VERT, ubo_avert);" not in flush:
    errors.append("R_FlushUniforms must bind ubo_avert at UBO_BINDING_ALIAS_VERT")
fl = re.search(r"if \(avert_dirty\)\s*\{(.*?)\n\t\}", flush, re.S)
if not fl or not re.search(r"GL_BindBuffer \(GL_UNIFORM_BUFFER, ubo_avert\);\s*glBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\(aliasvert\), &aliasvert,", fl.group(1)) \
        or re.search(r"\bubo_(?:vert|frag|afrag)\b", fl.group(1)):
    errors.append("R_FlushUniforms' avert branch must bind and fill ubo_avert, and only ubo_avert")
if "avert_dirty = true" not in (body_of(uc, r"\nvoid R_InvalidateUniforms \(void\)\s*\{") or ""):
    errors.append("R_InvalidateUniforms must re-upload AliasVert too")
init = strip(body_of(uc, r"\nvoid R_Uniforms_Init \(void\)\s*\{") or "")
init_flush = init.find("R_FlushUniforms ();")
for w in ("memcpy (aliasvert.lightview, ident, sizeof(ident));", "glGenBuffers_fp (1, &ubo_avert);", "avert_dirty = true;"):
    at = init.find(w)
    if at < 0:
        errors.append(f"R_Uniforms_Init: missing {w}")
    elif init_flush < 0 or at > init_flush:
        # A gen after the seed flush binds buffer 0 at the AliasVert point and
        # clears bindings_dirty, so the block stays unbound until the next
        # invalidate.
        errors.append(f"R_Uniforms_Init: {w} must come before the seed R_FlushUniforms")
shut = strip(body_of(uc, r"\nvoid R_Uniforms_Shutdown \(void\)\s*\{") or "")
if not re.search(r"GL_ForgetBuffer \(ubo_avert\);\s*glDeleteBuffers_fp \(1, &ubo_avert\);\s*ubo_avert = 0;", shut):
    errors.append("R_Uniforms_Shutdown must GL_ForgetBuffer, delete and zero ubo_avert, in that order")
if not re.search(r"GLint R_BindAliasVertBlock \(GLuint program\)\s*\{.*?RU_BindOne \(program, RU_NAME_AVERT, UBO_BINDING_ALIAS_VERT\)", uc, re.S) \
        or '#define RU_NAME_AVERT	"AliasVert"' not in uc:
    errors.append("R_BindAliasVertBlock must pin the \"AliasVert\" block to UBO_BINDING_ALIAS_VERT")

# ------------------------------------------------------------ the file
if '#include "layout.inc"\n#define UNIFORMS_VERT\n#include "uniforms.inc"\n#include "alias.inc"' not in vert:
    errors.append("salias_inst_vert.glsl must include layout.inc, then uniforms.inc with UNIFORMS_VERT, then alias.inc")
if re.search(r"^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?!sampler|usampler|isampler)\w+\s+\w+\s*(?:\[\w*\])?\s*;", vcode, re.M):
    errors.append("salias_inst_vert.glsl still declares a loose non-opaque uniform")
SSBOS = [("0", "InstanceBuffer", "instances"), ("1", "PoseBuffer", "pose_data"),
         ("2", "ShadeDots", "shadedots"), ("3", "MD3PoseBuffer", "md3_pose_data")]
for n, blk, member in SSBOS:
    if not re.search(rf"VERT_SSBO\({n}, {n}\) restrict readonly buffer {blk} \{{\s*\w+ {member}\[", vcode):
        errors.append(f"storage buffer {blk} must be VERT_SSBO({n}, {n}) restrict readonly, member {member}")
if re.search(r"layout\s*\(\s*std430", vcode):
    errors.append("salias_inst_vert.glsl spells a raw layout(std430...); use VERT_SSBO so Vulkan gets its set")
ARMS = {"VULKAN": "layout(std430, set = 0, binding = vk)", "GL_SPIRV": "layout(std430, binding = gl)",
        "plain": "layout(std430, binding = gl)"}
graphics = layout_inc[layout_inc.index("#define VERT_IN(loc)"):] if "#define VERT_IN(loc)" in layout_inc else ""
arms = re.findall(r"#define VERT_SSBO\(gl, vk\) (.*)", graphics)
if arms != list(ARMS.values()):
    errors.append(f"layout.inc VERT_SSBO arms (Vulkan, GL_SPIRV, plain) must be {list(ARMS.values())}, are {arms}")
vk = re.search(r"#ifdef VULKAN\n(.*?)#endif", vcode, re.S)
if not vk or "#define gl_VertexID gl_VertexIndex" not in vk.group(1) \
        or "#define gl_InstanceID gl_InstanceIndex" not in vk.group(1):
    errors.append("salias_inst_vert.glsl must alias gl_VertexID / gl_InstanceID for Vulkan")
if "invariant gl_Position;" not in vcode:
    errors.append("salias_inst_vert.glsl lost `invariant gl_Position` (uhexen2-iir3)")
# The layout qualifier beats glBindAttribLocation, so the file's locations
# must be the C attribute numbers the VAOs are built with.
attr = dict(re.findall(r"#define\s+(ATTR_\w+)\s+(\d+)", shader_h))
INPUTS = {"a_position": "ATTR_POSITION", "a_texcoord": "ATTR_TEXCOORD", "a_color": "ATTR_COLOR"}
for fname, code in (("salias_inst_vert.glsl", vcode), ("salias_vert.glsl", strip(read("engine/shaders/salias_vert.glsl")))):
    ins = re.findall(r"VERT_IN\((\d+)\)\s+in\s+\w+\s+(\w+)\s*;", code)
    if not ins:
        errors.append(f"{fname}: no VERT_IN inputs found")
    for loc, name in ins:
        want = attr.get(INPUTS.get(name, ""))
        if want is None:
            errors.append(f"{fname}: input {name} has no ATTR_* mapping in this test")
        elif loc != want:
            errors.append(f"{fname}: {name} is VERT_IN({loc}), gl_shader.h's {INPUTS[name]} is {want}")
if "instances[u_inst_base + gl_InstanceID]" not in vcode:
    errors.append("the instance fetch must offset gl_InstanceID by u_inst_base")


def hnum(name):
    m = re.search(rf"#define\s+{name}\s+(\d+)", shader_h)
    return m.group(1) if m else None


fb = re.search(r"#ifndef ALIAS_INST_SHADEDOT_ROW_MASK\n(.*?)#endif", vcode, re.S)
BITS = ("ALIAS_INST_SHADEDOT_ROW_MASK", "ALIAS_INST_DLIGHT_BIT", "ALIAS_INST_FULLBRIGHT_BIT")
for k in BITS:
    if hnum(k) is None:
        errors.append(f"gl_shader.h: no {k}")
    elif not fb or not re.search(rf"#define\s+{k}\s+{hnum(k)}\s*\n", fb.group(1)):
        errors.append(f"salias_inst_vert.glsl's #ifndef default for {k} must be gl_shader.h's {hnum(k)}")
main = vcode[vcode.find("void main()"):]
if "inst.ShadedotRow & ALIAS_INST_SHADEDOT_ROW_MASK" not in main or "inst.ShadedotRow & ALIAS_INST_DLIGHT_BIT" not in main:
    errors.append("main() must mask ShadedotRow with the macros, not literals")
# The three ShadedotRow fields own separate bits.
vals = [int(hnum(k)) for k in BITS if hnum(k)]
if len(vals) == 3 and (vals[0] & vals[1] or vals[0] & vals[2] or vals[1] & vals[2]):
    errors.append(f"ShadedotRow fields overlap: {dict(zip(BITS, vals))}")
# GitHub #491: a glow record is drawn unshaded.  Both format arms compute the
# shade term (shadedots table, MD3 1 + d), so the override comes after both,
# outside either, and before v_color reads it.
fbo = re.search(r"if \(\(inst\.ShadedotRow & ALIAS_INST_FULLBRIGHT_BIT\) != 0\)\s*sdot = 1\.0;", main)
vcol = main.find("v_color = vec4(inst.LightAlpha.rgb * sdot, inst.LightAlpha.a);")
last_arm = max(main.find("sdot = max(1.0 + d, 0.0);"), main.find("sdot = max(sdot, 0.0);"))
if not fbo:
    errors.append("main() (#491): a record with ALIAS_INST_FULLBRIGHT_BIT must set sdot = 1.0 -- the glow is "
                  "emissive, drawn unshaded as the CPU redraw draws it")
elif vcol < 0 or not (last_arm < fbo.start() < vcol):
    errors.append("main() (#491): the glow's sdot = 1.0 must come after both shade arms and before v_color")
else:
    dep = lambda i: main[:i].count("{") - main[:i].count("}")
    if dep(fbo.start()) != dep(vcol):
        errors.append("main() (#491): the glow's sdot override must sit at v_color's depth, outside either format arm")

# ------------------------------------------------------------ the C side's builds
if re.search(r"static const char salias_inst_vert\[\]", gl_shader):
    errors.append("the inline salias_inst_vert literal is back in gl_shader.c")
if re.search(r"\bGL_CompileShaderDefines\b", strip(gl_shader)):
    errors.append("GL_CompileShaderDefines is back; its only caller was the instanced literal")
hdr = re.search(r"#define GLSL_ALIAS_INST_VERT_HEADER \\\n((?:\t.*\\\n)*\t.*\n)", gl_shader)
want_hdr = ['GLSL_VERT_HEADER',
            '"#define ALIAS_INST_SHADEDOT_ROW_MASK " GLSL_STR(ALIAS_INST_SHADEDOT_ROW_MASK) "\\n"',
            '"#define ALIAS_INST_DLIGHT_BIT " GLSL_STR(ALIAS_INST_DLIGHT_BIT) "\\n"',
            '"#define ALIAS_INST_FULLBRIGHT_BIT " GLSL_STR(ALIAS_INST_FULLBRIGHT_BIT) "\\n"']
if not hdr or [l.strip().rstrip("\\").strip() for l in hdr.group(1).splitlines()] != want_hdr:
    errors.append("GLSL_ALIAS_INST_VERT_HEADER must be GLSL_VERT_HEADER plus the three ShadedotRow constants")
inst = body_of(gl_shader, r"static qboolean GL_InitAliasInstProgram \([^)]*\)\s*\{") or ""
for w in (r'q_snprintf \(vert_header, sizeof\(vert_header\), "%s%s",\s*GLSL_ALIAS_INST_VERT_HEADER, defines \? defines : ""\);',
          r"vs = GL_CompileShaderSplit\(GL_VERTEX_SHADER, vert_header, salias_inst_vert\);",
          r"R_BindAliasVertBlock\(prog\);"):
    if not re.search(w, inst):
        errors.append(f"GL_InitAliasInstProgram: missing {w[:70]}")
if "memset(p, 0" not in inst or "R_BindAliasVertBlock" in inst[:inst.find("memset(p, 0")]:
    errors.append("GL_InitAliasInstProgram must bind AliasVert after the memset that clears p")
st = re.search(r"typedef struct \{(.*?)\} gl_alias_inst_prog_t;", shader_h, re.S)
stf = set(re.findall(r"GLint\s+(\w+);", strip(st.group(1)))) if st else set()
dead = stf & {"u_viewproj", "u_inst_base", "u_eyepos", "u_poseverttype", "u_alias_dlight", "u_lightview"}
if dead:
    errors.append(f"gl_alias_inst_prog_t still carries vertex-stage locations that are -1 now: {sorted(dead)}")

# ------------------------------------------------------------ the draw
draw = strip(body_of(rmain, r"static void R_DrawAliasInstanced \(void\)\s*\{") or "")
if not draw:
    errors.append("gl_rmain.c: no R_DrawAliasInstanced")
if re.search(r"\bglUniform\w*_fp\b", draw):
    errors.append("R_DrawAliasInstanced still makes a loose glUniform write; both stages read blocks now")
if "last_poseverttype" in draw:
    errors.append("R_DrawAliasInstanced's per-function poseverttype cache is back; it cannot see the global block")
LOOP = "for (b = 0; b < num_alias_batches; b++)"
fbi = draw.find("R_SetFog (r_fog_density, vec3_origin);")
fbloop = draw.find(LOOP, fbi) if fbi >= 0 else -1
mainloop = draw.find(LOOP)
if fbi < 0 or fbloop < 0 or mainloop < 0 or mainloop > fbi:
    errors.append("cannot find R_DrawAliasInstanced's two batch loops")
else:
    def loop_body(start):
        """The brace-balanced body of the batch loop starting at `start`."""
        return body_of(draw[start:], re.escape(LOOP) + r"\s*\{") or ""
    fbuse = draw.rfind("R_UseProgram (prog->program);", 0, fbloop)
    passes = {"main pass": (draw[:mainloop], loop_body(mainloop),
                            [r"R_SetViewProj \(alias_inst_view_proj\);", r"R_SetEyePos \(r_origin\);",
                             r"R_SetAliasDlight \(R_AliasDlightScale \(true\)\);"],
                            [r"R_SetPoseVertType \(gm->poseverttype\);"]),
              "fullbright pass": (draw[fbuse:fbloop] if fbuse >= 0 else "", loop_body(fbloop),
                                  [r"R_SetViewProj \(alias_inst_view_proj\);", r"R_SetEyePos \(r_origin\);",
                                   r"R_SetAliasDlight \(0\.0f\);"],
                                  [r"R_SetPoseVertType \(gm->poseverttype\);"])}
    for label, (pre, loop, pre_wants, loop_wants) in passes.items():
        if not loop:
            errors.append(f"R_DrawAliasInstanced {label}: cannot parse the batch loop body")
            continue
        # Before the loop is enough: every batch flushes before it draws.
        for w in pre_wants:
            if not re.search(w, pre):
                errors.append(f"R_DrawAliasInstanced {label}: must write {w} before its batch loop")
        # Hazard 4, per batch: the instance base is set, then flushed, then
        # drawn, inside the loop -- in that order, every iteration.
        seq = [m.start() for m in (re.search(r"R_SetInstBase \(batch->first\);", loop),
                                   re.search(r"R_FlushUniforms \(\);", loop),
                                   re.search(r"glDrawElementsInstanced_fp", loop)) if m]
        if len(seq) != 3 or seq != sorted(seq):
            errors.append(f"R_DrawAliasInstanced {label} (hazard 4): each batch must R_SetInstBase (batch->first), "
                          "then R_FlushUniforms, then draw")
        else:
            # ...and the flush is unconditional: at the draw's own brace depth
            # and not the body of a braceless if/else/for/while.
            def depth_at(i):
                return loop[:i].count("{") - loop[:i].count("}")
            stmt_start = max(loop.rfind(";", 0, seq[1]), loop.rfind("{", 0, seq[1]), loop.rfind("}", 0, seq[1]))
            lead = loop[stmt_start + 1:seq[1]]
            if depth_at(seq[1]) != depth_at(seq[2]) or re.search(r"\b(?:if|else|for|while)\b", lead):
                errors.append(f"R_DrawAliasInstanced {label} (hazard 4): the per-batch R_FlushUniforms must be "
                              "unconditional, at the same depth as the draw")
        for w in loop_wants:
            m = re.search(w, loop)
            if not m or (len(seq) == 3 and m.start() > seq[1]):
                errors.append(f"R_DrawAliasInstanced {label}: {w} must be set per batch before the flush")
        if re.search(r"glDrawElementsInstanced_fp", loop) and \
                loop.count("glDrawElementsInstanced_fp") != 1:
            errors.append(f"R_DrawAliasInstanced {label}: expected one instanced draw per batch")
        # GitHub #485: GL_MakeAliasModelGPU builds ssbo_pose OR ssbo_pose_md3,
        # never both, so a batch is drawn from whichever its format has, at
        # that format's binding (1 for PV_QUAKE1, 3 for PV_MD3), under that
        # format's pose type -- in both passes.  A skip on ssbo_pose alone
        # silently dropped every MD3 glow.
        if re.search(r"!\s*gm->ssbo_pose\b(?!_md3)", loop):
            errors.append(f"R_DrawAliasInstanced {label} (#485): skips a batch on !gm->ssbo_pose, "
                          "which an MD3 mesh never has; test the pose buffer of the batch's format")
        if not re.search(r"gm->poseverttype == PV_MD3[^;]*?\bgm->ssbo_pose_md3\b", loop, re.S):
            errors.append(f"R_DrawAliasInstanced {label} (#485): a PV_MD3 batch must draw from gm->ssbo_pose_md3")
        if not re.search(r"GL_BindBufferBase\s*\(\s*GL_SHADER_STORAGE_BUFFER\s*,\s*"
                         r"(?:3\b|gm->poseverttype == PV_MD3 \? 3 : 1\b)", loop):
            errors.append(f"R_DrawAliasInstanced {label} (#485): the MD3 pose buffer must be bound at binding 3, "
                          "where salias_inst_vert's MD3PoseBuffer reads it")
        if re.search(r"R_SetPoseVertType \(PV_", loop):
            errors.append(f"R_DrawAliasInstanced {label} (#485): the pose type is the batch's "
                          "(gm->poseverttype), never a fixed PV_* constant")
    # GitHub #491: the glow pass marks every record, alongside the flat light,
    # in one loop before it re-streams them.  The main pass's never carry it.
    has_fb = draw.rfind("if (has_fb)", 0, fbi)
    up = draw.find("GL_Upload", has_fb) if has_fb >= 0 else -1
    seg = draw[has_fb:up] if 0 <= has_fb < up else ""
    loop_m = re.search(r"for \(i = 0; i < num_alias_instances; i\+\+\)\s*\{([^{}]*)\}", seg)
    if not loop_m or not re.search(r"alias_instances\[i\]\.shadedot_row \|= ALIAS_INST_FULLBRIGHT_BIT;",
                                   loop_m.group(1)) \
            or not all(f"alias_instances[i].light_color[{k}] = 1.0f;" in loop_m.group(1) for k in range(3)):
        errors.append("R_DrawAliasInstanced fullbright pass (#491): one loop over num_alias_instances must give "
                      "every record the flat light 1.0f and ALIAS_INST_FULLBRIGHT_BIT before the pass re-streams them")
    if has_fb < 0 or "ALIAS_INST_FULLBRIGHT_BIT" in draw[:has_fb]:
        errors.append("R_DrawAliasInstanced (#491): ALIAS_INST_FULLBRIGHT_BIT is set before the main pass draws")
    # Binding 3 goes back to 0 after each pass that may have bound it.
    tail = draw[fbloop:]
    if not re.search(r"GL_BindBufferBase\s*\(\s*GL_SHADER_STORAGE_BUFFER\s*,\s*3\s*,\s*0\s*\)", tail):
        errors.append("R_DrawAliasInstanced fullbright pass (#485): must unbind binding 3 once its batches are drawn")

if len(re.findall(r"\bALIAS_INST_FULLBRIGHT_BIT\b", strip(rmain))) != 1:
    errors.append("gl_rmain.c (#491): ALIAS_INST_FULLBRIGHT_BIT must be written exactly once, in the glow pass; "
                  "a main-pass record carrying it would draw unshaded")

bfa = strip(body_of(lc, r"void R_LightCluster_BindForAlias \(void\)\s*\{") or "")
if re.search(r"glUniform\w*_fp|LC_SetAliasInstUniforms|u_lightview", bfa) or "LC_SetAliasInstUniforms" in strip(lc):
    errors.append("R_LightCluster_BindForAlias still writes a per-program u_lightview; AliasVert carries it")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS salias_inst_vert reads VertParams + AliasVert; mirrors, file, builds and both passes' per-batch writes agree (#450 S3)")
