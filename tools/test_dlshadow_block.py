#!/usr/bin/env python3
"""The dynamic-light shadow maps' two uniform blocks match their C mirrors, are
bound and uploaded, and stay off the ES tier (GitHub #97, P3/P4); the alias
models cast as their draw poses them, and never into their own light (P6).

  * ShadowPass (engine/shaders/sshadow_vert.glsl and sshadow_alias_vert.glsl,
    one block declared twice) and r_shadowpass_t, DlShadow
    (engine/shaders/dlshadow.inc) and r_dlshadow_t (engine/h2shared/gl_shadow.c):
    same members in the same order, every one vec4 / ivec4 / mat4 (arrays of
    them included), std140 offsets equal to the C offsets, sizes pinned by
    COMPILE_TIME_ASSERT; DLSHADOW_MAX_MAPS == SHADOW_MAX_MAPS, and at most 16
    because a layer is four bits.
  * Bindings: UBO_BINDING_SHADOW_PASS and UBO_BINDING_DLSHADOW exist in
    gl_shader.h and share their number with no other block; gl_shadow.c pins
    ShadowPass with glUniformBlockBinding and uploads it per pass with
    GL_Upload + GL_BindBufferRange; R_Shadow_Prepare fills DlShadow and R_Shadow_Draw uploads it with
    glBufferData and binds it every frame; R_Shadow_InitProgram pins DlShadow
    and points u_dlshadowmap at DLSHADOW_TMU, and every program-link path that
    can link salias_frag or sworld_frag calls it while the program is current.
    DLSHADOW_TMU is a unit nothing else in those programs uses.
  * The packing: the mask bit and 4-bit layer list R_Shadow_Prepare writes
    decode, through DlShadow's own expressions, to the layer each light was
    given -- checked by running both sides' arithmetic on random light sets.
  * The stages: sworld_frag, sworld_opaque_frag and salias_frag declare
    u_dlshadowmap as their last plain sampler under #ifndef GL_ES, define
    DLSHADOW_UBO_SLOT as their own block count and include dlshadow.inc after
    lightgrid.inc (and after oit.inc) and before their dlight include; the
    lights are scaled by DlShadowFactor after the radius rejects and only when
    u_dlsparams.x says a map exists; and the ES arm of every stage names none of
    it -- no sampler, no block, no bitCount.
  * The frame: R_SetupGL runs R_Shadow_Prepare then R_Shadow_Draw after the
    cluster binds and not in the mirror pass, the Draw between the r_speeds 2
    RPROF_SHADOW pair.
  * The alias casters (r_shadow_casters 2): sshadow_alias_vert reads the pose
    buffers at the GL bindings and with the vertex slots the stages it mirrors
    use (salias_inst_vert 1 and 3, sskeletal_vert 4 and attributes 0, 3, 4),
    blends pose 0 by the weight the C side gives the current pose, and pushes
    every vertex along its ray from the light by the per-pass amount the C side
    uploads (zero again for the world's pass); its program pins ShadowPass and
    checks its size, and failing to build turns off the casters, not the maps.
    R_AliasShadowCaster poses a COPY of the entity (the lerp advances state the
    draw owns) with R_DrawAliasModel's transform and lerp.  The exclusions --
    SH_AliasCasts compiled and run: not into the light keyed to the model's
    entity, not into a light inside the model's box, nothing beyond reach --
    and SH_AliasEligible's: never the viewmodel, never a translucent model, the
    view's r_entdist.  The cache hash and the collection ask the same question
    with the same key, and a capped list leaves the map incomplete.
"""
from pathlib import Path
import random
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []


def read(rel):
    p = ROOT / rel
    if not p.exists():
        errors.append(f"{rel} is missing")
        return ""
    return p.read_text()


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def body_of(text, head):
    m = re.search(head, text)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    while i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.end():i]
        i += 1
    return None


shadow_c = strip(read("engine/h2shared/gl_shadow.c"))
shadow_h = strip(read("engine/h2shared/gl_shadow.h"))
shader_h = strip(read("engine/h2shared/gl_shader.h"))
shader_c = strip(read("engine/h2shared/gl_shader.c"))
rmain = strip(read("engine/hexen2/gl_rmain.c"))
dls = strip(read("engine/shaders/dlshadow.inc"))
svert = strip(read("engine/shaders/sshadow_vert.glsl"))
avert = strip(read("engine/shaders/sshadow_alias_vert.glsl"))

SIZES = {"vec4": 16, "ivec4": 16, "mat4": 64}
CSIZES = {"float": 4, "int": 4}


def defines(*texts):
    out = {}
    for t in texts:
        for n, v in re.findall(r"#define\s+(\w+)\s+\(?(\d+)\)?\s*$", t, re.M):
            out.setdefault(n, int(v))
    return out


DEF = defines(dls, shadow_h, shadow_c, shader_h)


def glsl_block(text, name):
    m = re.search(r"\buniform\s+" + name + r"\s*\{(.*?)\};", text, re.S)
    if not m:
        errors.append(f"no `uniform {name} {{...}}` block")
        return []
    out = []
    for ty, n, arr in re.findall(r"(?:highp\s+|mediump\s+|lowp\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\w+)\s*\])?\s*;", m.group(1)):
        count = 1 if not arr else (int(arr) if arr.isdigit() else DEF.get(arr))
        if count is None:
            errors.append(f"{name}.{n}: array size {arr} is not a known #define")
            count = 1
        if ty not in SIZES:
            errors.append(f"{name}.{n} is a {ty}; every member must be vec4, ivec4 or mat4 (uniforms.inc says why)")
        out.append((n, ty, count))
    return out


def c_struct(text, name):
    m = re.search(r"typedef struct\s*\{([^{}]*)\}\s*" + name + r"\s*;", text)
    if not m:
        errors.append(f"gl_shadow.c: no `typedef struct {{...}} {name};`")
        return []
    out = []
    for ty, n, dims in re.findall(r"\b(float|int)\s+(\w+)((?:\s*\[\s*\w+\s*\])+)\s*;", m.group(1)):
        size = CSIZES[ty]
        for d in re.findall(r"\[\s*(\w+)\s*\]", dims):
            size *= int(d) if d.isdigit() else DEF.get(d, 0)
        out.append((n, size))
    return out


# Which C field mirrors which block member: equal sizes in a different order
# would pass every offset check and still swap two values.
NAMES = {"u_shadowvp": "vp", "u_shadowmodel": "model", "u_shadowlight": "light", "u_shadowpose": "pose",
         "u_dlsmask": "mask", "u_dlslayers": "layers", "u_dlsparams": "params", "u_dlsorigin": "origin"}


def check_pair(gname, gtext, cname, csize):
    g = glsl_block(gtext, gname)
    c = c_struct(shadow_c, cname)
    goff, coff = 0, 0
    if len(g) != len(c):
        errors.append(f"{gname} has {len(g)} members, {cname} {len(c)}")
    for (gn, ty, cnt), (cn, cs) in zip(g, c):
        if NAMES.get(gn) != cn:
            errors.append(f"{gname}.{gn} sits where {cname}.{cn} is; its mirror is {NAMES.get(gn)}")
        gs = SIZES.get(ty, 16) * cnt
        if gs != cs:
            errors.append(f"{gname}.{gn} is {gs} bytes, {cname}.{cn} {cs}")
        if goff != coff:
            errors.append(f"{gname}.{gn} at std140 offset {goff}, {cname}.{cn} at {coff}")
        goff += gs
        coff += cs
    if goff != csize:
        errors.append(f"{gname} is {goff} bytes, the pinned size is {csize}")
    if not re.search(r"COMPILE_TIME_ASSERT\(\w+,\s*sizeof\(" + cname + r"\)\s*==\s*" + str(csize) + r"\)", shadow_c):
        errors.append(f"gl_shadow.c must COMPILE_TIME_ASSERT sizeof({cname}) == {csize}")


check_pair("ShadowPass", svert, "r_shadowpass_t", 160)
check_pair("ShadowPass", avert, "r_shadowpass_t", 160)
check_pair("DlShadow", dls, "r_dlshadow_t", 304)
if DEF.get("DLSHADOW_MAX_MAPS") != DEF.get("SHADOW_MAX_MAPS"):
    errors.append(f"DLSHADOW_MAX_MAPS {DEF.get('DLSHADOW_MAX_MAPS')} != SHADOW_MAX_MAPS {DEF.get('SHADOW_MAX_MAPS')}")
if not DEF.get("SHADOW_MAX_MAPS") or DEF["SHADOW_MAX_MAPS"] > 16:
    errors.append("SHADOW_MAX_MAPS must be 1..16: a layer is four bits of u_dlslayers")
if "if (maps > SHADOW_MAX_MAPS)\n\t\tmaps = SHADOW_MAX_MAPS;" not in read("engine/h2shared/gl_shadow.c"):
    errors.append("SH_EnsureTexture must clamp r_shadow_maxlights to SHADOW_MAX_MAPS")
for name, text in (("sshadow_vert", svert), ("sshadow_alias_vert", avert)):
    if not re.search(r"VERT_UBO\(0\)\s+uniform\s+ShadowPass\b", text):
        errors.append(f"ShadowPass must be {name}'s VERT_UBO(0)")
if re.search(r"\buniform\s+ShadowPass\b", strip(read("engine/shaders/sshadow_frag.glsl"))):
    errors.append("sshadow_frag declares ShadowPass; the fragment stage reads only the varying")
if not re.search(r"FRAG_UBO\(DLSHADOW_SET3_SLOT\)\s+uniform\s+DlShadow\b", dls) \
        or not re.search(r"#ifdef OIT_TEX_COUNT\s*#define DLSHADOW_SET3_SLOT\s*\(DLSHADOW_UBO_SLOT \+ 1\)\s*#else\s*"
                         r"#define DLSHADOW_SET3_SLOT\s*DLSHADOW_UBO_SLOT\s*#endif", dls):
    errors.append("DlShadow's SDL_GPU slot must be DLSHADOW_UBO_SLOT, one later in an OIT twin (after OitFrag)")

# ------------------------------------------------------------ bindings
allbind = {}
for f in ("engine/h2shared/gl_shader.h", "engine/h2shared/gl_postprocess.c", "engine/h2shared/gl_ssao.c",
          "engine/h2shared/gl_worldcull.c", "engine/h2shared/gl_lightcluster.c", "engine/h2shared/gl_shadow.c"):
    for name, num in re.findall(r"#define\s+(\w*UBO_BINDING\w*)\s+(\d+)", read(f)):
        allbind.setdefault(int(num), []).append(name)
for name in ("UBO_BINDING_SHADOW_PASS", "UBO_BINDING_DLSHADOW"):
    b = re.search(r"#define\s+" + name + r"\s+(\d+)", shader_h)
    if not b:
        errors.append(f"gl_shader.h: no {name}")
    elif allbind.get(int(b.group(1))) != [name]:
        errors.append(f"UBO binding {b.group(1)} is shared: {allbind.get(int(b.group(1)))}")
bp = body_of(shadow_c, r"static qboolean SH_BuildPrograms \(void\)\s*\{") or ""
if 'glGetUniformBlockIndex_fp (sh_prog, "ShadowPass")' not in bp \
        or "glUniformBlockBinding_fp (sh_prog, blk, UBO_BINDING_SHADOW_PASS);" not in bp \
        or "GL_UNIFORM_BLOCK_DATA_SIZE" not in bp or "sizeof(r_shadowpass_t)" not in bp:
    errors.append("SH_BuildPrograms must pin ShadowPass to UBO_BINDING_SHADOW_PASS and check its size")
sp = body_of(shadow_c, r"static void SH_Pass \(const r_shadowpass_t \*pass\)\s*\{") or ""
if "GL_Upload (GL_UNIFORM_BUFFER, pass, sizeof(*pass), &buf, &ofs);" not in sp \
        or "GL_BindBufferRange (GL_UNIFORM_BUFFER, UBO_BINDING_SHADOW_PASS, buf, ofs, sizeof(*pass));" not in sp:
    errors.append("SH_Pass must upload ShadowPass and bind it at UBO_BINDING_SHADOW_PASS")
dm = body_of(shadow_c, r"static void SH_DrawMap \([^)]*\)\s*\{") or ""
if dm.count("SH_Pass (&pass);") != 2:
    errors.append("SH_DrawMap must re-upload ShadowPass before the world's draw and before each entity's")
pr = body_of(shadow_c, r"\nvoid R_Shadow_Prepare \(void\)\s*\{") or ""
up = body_of(shadow_c, r"\nvoid R_Shadow_Draw \(void\)\s*\{") or ""
if not re.search(r"memset \(&sh_block, 0, sizeof\(sh_block\)\);", pr) \
        or not re.search(r"glBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\(sh_block\), &sh_block, GL_DYNAMIC_DRAW\);"
                         r"\s*GL_BindBuffer \(GL_UNIFORM_BUFFER, 0\);\s*"
                         r"GL_BindBufferBase \(GL_UNIFORM_BUFFER, UBO_BINDING_DLSHADOW, sh_ubo\);\s*$", up):
    errors.append("R_Shadow_Prepare must clear the block, and R_Shadow_Draw end by uploading it and binding "
                  "it at UBO_BINDING_DLSHADOW, every frame (the shaders read it whether or not a map exists)")
if pr.find("memset (&sh_block") < 0 or pr.find("memset (&sh_block") > pr.find("SH_Select ()"):
    errors.append("R_Shadow_Prepare must clear the block before SH_Select fills it")
if "SH_DrawMap (&sh_draws[i]);" not in up or up.find("SH_RestoreState (&saved);") > up.find("glBufferData_fp") \
        or "SH_DrawMap" in (body_of(shadow_c, r"static void SH_Select \(void\)\s*\{") or ""):
    errors.append("R_Shadow_Draw draws the collected maps, restores the state, then uploads; SH_Select draws nothing")
ip = body_of(shadow_c, r"\nvoid R_Shadow_InitProgram \(GLuint program\)\s*\{") or ""
# Both arms (#340 P2a): GL pins the block and points the sampler; SDL_GPU finds
# the block by name at draw time and is told the sampler's unit the same way.
if 'glGetUniformBlockIndex_fp (program, "DlShadow")' not in ip \
        or "glUniformBlockBinding_fp (program, blk, UBO_BINDING_DLSHADOW);" not in ip \
        or ip.count('loc = R_ProgramSamplerLoc (program, "u_dlshadowmap");') != 2 \
        or ip.count('R_ProgramSamplerUnit (program, loc, "u_dlshadowmap", DLSHADOW_TMU);') != 2 \
        or "sizeof(r_dlshadow_t)" not in ip:
    errors.append("R_Shadow_InitProgram must pin DlShadow, check its size, and set u_dlshadowmap to DLSHADOW_TMU "
                  "in both the GL and the SDL_GPU arm")
# The SDL_GPU mirror (#340 P2a): registered under -gpu over the very struct
# the GL arm uploads, and flushed where the GL arm uploads it -- after the
# maps are drawn, before the world and alias draws that read it.  Unregistered,
# every world and alias draw leaves the slot unpushed; unflushed, it reads
# the zeros of the registration.
init = body_of(shadow_c, r"\nvoid R_Shadow_Init \(void\)\s*\{") or ""
if not re.search(r"#ifdef USE_SDL_GPU\s*(?:/\*.*?\*/\s*)?if \(R_GPU \(\)\)\s*"
                 r'RG_UniformBlockRegister \("DlShadow", &sh_block, sizeof\(sh_block\)\);\s*#endif', init, re.S):
    errors.append("R_Shadow_Init must register sh_block as the \"DlShadow\" mirror under USE_SDL_GPU, if R_GPU ()")
if not re.search(r"#ifdef USE_SDL_GPU\s*(?:/\*.*?\*/\s*)?if \(R_GPU \(\)\)\s*\{\s*"
                 r'RG_UniformBlockFlushed \("DlShadow"\);\s*return;\s*\}\s*#endif\s*'
                 r"GL_BindBuffer \(GL_UNIFORM_BUFFER, sh_ubo\);\s*"
                 r"glBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\(sh_block\), &sh_block, GL_DYNAMIC_DRAW\);", up, re.S) \
        or up.find("RG_UniformBlockFlushed") < up.find("SH_RestoreState (&saved);"):
    errors.append("R_Shadow_Draw must flush the DlShadow mirror under -gpu right where the GL arm uploads the "
                  "block, after the maps are drawn")
if len(re.findall(r"RG_UniformBlock(?:Register|Flushed) \(\"DlShadow\"", shadow_c)) != 2:
    errors.append("gl_shadow.c must register the DlShadow mirror once and flush it once")
SITES = {
    "GL_InitProgramSplit": r"static qboolean GL_InitProgramSplit \([^)]*\)\s*\{",
    "GL_InitOITProgramSplit": r"static void GL_InitOITProgramSplit \([^)]*\)\s*\{",
    "GL_InitAliasInstProgram": r"static qboolean GL_InitAliasInstProgram \([^)]*\)\s*\{",
}
for site, head in SITES.items():
    b = body_of(shader_c, head)
    if b is None:
        errors.append(f"gl_shader.c: cannot find {site}")
        continue
    call = re.search(r"R_Shadow_InitProgram \((?:p->program|prog)\);", b)
    use = [m.start() for m in re.finditer(r"R_UseProgram \(", b)]
    if not call:
        errors.append(f"{site} links world or alias stages and must call R_Shadow_InitProgram")
    elif not any(u < call.start() for u in use) or not any(u > call.start() for u in use):
        errors.append(f"{site}: R_Shadow_InitProgram must run while the program is current (glUniform1i)")
tmu = DEF.get("DLSHADOW_TMU")
others = {DEF.get("LIGHT_GRID_TMU", 5), 0, 1, 2, 3, 4} | {DEF.get("LMSTYLE_TMU", 6) + k for k in range(3)} \
    | {DEF.get(n) for n in ("MBOIT_TMU_B0", "MBOIT_TMU_MOM", "MBOIT_TMU_B0_MS", "MBOIT_TMU_MOM_MS")}
# ...and nothing else in the engine binds to it: every texture-unit expression
# in the GL sources, a define or a literal, that names DLSHADOW_TMU's number.
for p in sorted((ROOT / "engine").rglob("gl_*.c")):
    if p.name == "gl_shadow.c" or tmu is None:
        continue
    t = strip(p.read_text(errors="replace"))
    for m in re.finditer(r"GL_TEXTURE0\s*\+\s*(\d+)\b|R_Bind\w*\s*\(\s*(\d+)\s*,|glUniform1i_fp\s*\([^,]+,\s*(\d+)\s*\)"
                         r"|R_ProgramSamplerUnit\s*\([^;]*?,\s*(\d+)\s*\)", t):
        if int(next(g for g in m.groups() if g)) == tmu:
            errors.append(f"{p.relative_to(ROOT)} uses texture unit {tmu}, DLSHADOW_TMU's")
if tmu is None or tmu in others or not 0 <= tmu < 16:
    errors.append(f"DLSHADOW_TMU {tmu} must be a unit 0..15 no world or alias sampler uses ({sorted(x for x in others if x is not None)})")

# ------------------------------------------------------------ the cache's honesty
# A failure sticks (no half-built program, no per-frame retry); a stale map is
# kept only near where it was drawn; a map missing a caster is never "fresh".
sel = body_of(shadow_c, r"static void SH_Select \(void\)\s*\{") or ""
bp2 = body_of(shadow_c, r"static qboolean SH_BuildPrograms \(void\)\s*\{") or ""
if not re.search(r"if \(sh_failed\)\s*return false;\s*if \(sh_prog\)\s*return true;", bp2) \
        or bp2.count("return SH_Fail ();") < 3 or "!sh_failed && R_LightCluster_Available ()" not in pr:
    errors.append("SH_BuildPrograms must check sh_failed first and fail through SH_Fail; R_Shadow_Prepare must stop on it")
if "if (budget <= 0 && DotProduct (moved, moved) > SH_STALE * SH_STALE)\n\t\t\t\tcand[i].slot = -1;" not in sel \
        or "hash = SH_EntityHash (sl->origin, sl->radius, sl->key);" not in sel:
    errors.append("SH_Select must drop a stale map drawn more than SH_STALE away, and hash about the drawn origin")
if "sl->enthash = complete ? hash : ~hash;" not in sel:
    errors.append("SH_Select must keep a map that left a caster out from passing for fresh")

# ------------------------------------------------------------ the packing
sel = body_of(shadow_c, r"static void SH_Select \(void\)\s*\{") or ""
PACK = ["qsort (cand, take, sizeof(cand[0]), SH_CompareLight);",
        "sh_block.mask[l >> 5] |= (int)(1u << (l & 31));",
        "sh_block.layers[s >> 3] |= (int)((unsigned int)layer << (4 * (s & 7)));",
        "sh_block.origin[layer][3] = 1.0f / sl->radius;"]
for p in PACK:
    if p not in sel:
        errors.append(f"SH_Select no longer packs the block with `{p}`")
if sel.find(PACK[0]) > sel.find(PACK[1]):
    errors.append("SH_Select must sort the mapped lights by light index before packing them")
UNPACK = ["uint bits = uint(u_dlsmask[i >> 5]);", "uint bit = 1u << uint(i & 31);",
          "int k = bitCount(bits & (bit - 1u));", "k += bitCount(uint(u_dlsmask[w]));",
          "int layer = (u_dlslayers[k >> 3] >> (4 * (k & 7))) & 15;", "highp vec4 o = u_dlsorigin[layer];"]
for p in UNPACK:
    if p not in dls:
        errors.append(f"DlShadow no longer decodes with `{p}`")


def to_i32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


rng = random.Random(9797)
for trial in range(300):
    n = rng.randint(1, 128)
    maps = rng.randint(1, 16)
    lights = sorted(rng.sample(range(n), min(n, maps)))
    layers = rng.sample(range(16), len(lights))
    mask, packed = [0] * 4, [0] * 4
    for s, (l, layer) in enumerate(zip(lights, layers)):      # the C side, ascending light index
        mask[l >> 5] = to_i32(mask[l >> 5] | (1 << (l & 31)))
        packed[s >> 3] = to_i32(packed[s >> 3] | (layer << (4 * (s & 7))))
    want = dict(zip(lights, layers))
    for i in range(n):                                         # the shader side
        bits = mask[i >> 5] & 0xffffffff
        bit = 1 << (i & 31)
        if not bits & bit:
            if i in want:
                errors.append(f"trial {trial}: light {i} lost its map")
            continue
        k = bin(bits & (bit - 1)).count("1") + sum(bin(mask[w] & 0xffffffff).count("1") for w in range(i >> 5))
        layer = (packed[k >> 3] >> (4 * (k & 7))) & 15
        if want.get(i) != layer:
            errors.append(f"trial {trial}: light {i} decodes to layer {layer}, was given {want.get(i)}")
            break
    if len(errors) > 10:
        break

# ------------------------------------------------------------ the stages
WS = {"sworld_frag": "world_dlight.inc", "sworld_opaque_frag": "world_dlight.inc", "salias_frag": "alias_dlight.inc"}
FORBID = ["u_dlshadowmap", "samplerCubeArrayShadow", "DlShadow", "DlShadowFactor", "u_dlsmask", "u_dlslayers", "u_dlsparams",
          "u_dlsorigin", "bitCount", "DlShadowWorldPos"]


def expand(text):
    for _ in range(32):
        m = re.search(r'#include "([^"]+)"', text)
        if not m:
            break
        text = text.replace(f'#include "{m.group(1)}"\n', (SH / m.group(1)).read_text())
    return text


def es_arm(text):
    """What a compiler with GL_ES predefined keeps: #ifdef, #ifndef, #if/#elif
    defined(X), #else, #endif, #define.  Macro bodies are not expanded."""
    out, stack, defs = [], [], {"GL_ES"}

    def ev(expr):
        e = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                   lambda m: "1" if (m.group(1) or m.group(2)) in defs else "0", expr)
        e = re.sub(r"\b[A-Za-z_]\w*\b", "0", e)
        return bool(eval(e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")))
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif|define)\b\s*(.*)", line)
        live = all(s[0] for s in stack)
        if not m:
            if live:
                out.append(line)
            continue
        d, rest = m.group(1), m.group(2).strip()
        if d in ("ifdef", "ifndef"):
            v = (rest.split()[0] in defs) == (d == "ifdef")
            stack.append([v, v])
        elif d == "if":
            v = ev(rest)
            stack.append([v, v])
        elif d == "elif":
            v = not stack[-1][1] and ev(rest)
            stack[-1] = [v, stack[-1][1] or v]
        elif d == "else":
            stack[-1] = [not stack[-1][1], True]
        elif d == "endif":
            stack.pop()
        elif d == "define" and live:
            defs.add(rest.split("(")[0].split()[0])
    return "\n".join(out)


for st, inc in WS.items():
    text = read(f"engine/shaders/{st}.glsl")
    code = strip(text)
    desk = "".join(re.findall(r"#ifndef GL_ES\n(.*?)#(?:endif|else)", code, re.S))
    tex = re.findall(r"FRAG_TEX\((\d+)\) uniform (?:highp )?(\w+) (\w+);", code)
    if not tex or tex[-1][1:] != ("samplerCubeArrayShadow", "u_dlshadowmap"):
        errors.append(f"{st}: u_dlshadowmap must be the last plain sampler (FRAG_TEX before oit.inc's)")
    if "uniform highp samplerCubeArrayShadow u_dlshadowmap;" not in desk:
        errors.append(f"{st}: u_dlshadowmap must sit under #ifndef GL_ES")
    ubo = [int(x) for x in re.findall(r"\bFRAG_UBO\((\d+)\)", strip(expand(text)))]
    if not re.search(rf"#define DLSHADOW_UBO_SLOT {len(ubo)}\n", desk):
        errors.append(f"{st}: DLSHADOW_UBO_SLOT must be {len(ubo)}, the stage's own fragment blocks, under #ifndef GL_ES")
    order = [code.find(s) for s in ('#include "lightgrid.inc"', '#include "dlshadow.inc"', f'#include "{inc}"')]
    if -1 in order or order != sorted(order):
        errors.append(f"{st}: dlshadow.inc must come after lightgrid.inc and before {inc}")
    if '#include "oit.inc"' in code and code.find('#include "oit.inc"') > order[1]:
        errors.append(f"{st}: oit.inc must come before dlshadow.inc (OitFrag takes the slot before DlShadow's)")
    if '#include "dlshadow.inc"' not in desk:
        errors.append(f"{st}: dlshadow.inc must be included under #ifndef GL_ES")
    es = strip(es_arm(expand(text)))
    named = [w for w in FORBID if re.search(r"\b" + w + r"\b", es)]
    if named:
        errors.append(f"{st}'s ES arm names {named}; WebGL2 has no shadow maps")
    full = strip(expand(text))
    if not re.search(r"\buniform\s+DlShadow\b", full):
        errors.append(f"{st}: the desktop arm does not declare DlShadow")

wd = strip(read("engine/shaders/world_dlight.inc"))
fn = body_of(wd, r"vec3 ClusteredDlight\(vec3 lm, highp vec3 p\)\s*\{") or ""
for a, b in (("if (d >= rad - ml) continue;", "if (shadowed) add *= DlShadowFactor(i, wp);"),
             ("if (shadowed) add *= DlShadowFactor(i, wp);", "lm = max(lm + add, vec3(0.0));")):
    if a not in fn or b not in fn or fn.find(a) > fn.find(b):
        errors.append(f"ClusteredDlight: `{b}` must follow `{a}`")
if "vec3 add = (rad - d) * u_dlight_scale * Lights[i].color;" not in fn:
    errors.append("ClusteredDlight's light term must keep its old arithmetic (unshadowed lights render as before)")
ad = strip(read("engine/shaders/alias_dlight.inc"))
fa = body_of(ad, r"vec3 ClusteredAliasDlight\(vec3 lit, highp vec3 p, float scale\)\s*\{") or ""
for a, b in (("if (add <= 0.0) continue;", "if (shadowed) term *= DlShadowFactor(i, wp);"),
             ("if (shadowed) term *= DlShadowFactor(i, wp);", "lit = max(lit + term, vec3(0.0));")):
    if a not in fa or b not in fa or fa.find(a) > fa.find(b):
        errors.append(f"ClusteredAliasDlight: `{b}` must follow `{a}`")
for name, f in (("ClusteredDlight", fn), ("ClusteredAliasDlight", fa)):
    if len(re.findall(r"\bDlShadowFactor\s*\(", f)) != 1:
        errors.append(f"{name} must ask DlShadowFactor exactly once per light, after the rejects")
    if "bool shadowed = u_dlsparams.x > 0.0;" not in f \
            or "highp vec3 wp = shadowed ? DlShadowWorldPos(p) : vec3(0.0);" not in f:
        errors.append(f"{name} must gate the lookups on u_dlsparams.x and take the world position once")
if "return transpose(mat3(u_lightview)) * (p - u_lightview[3].xyz);" not in dls:
    errors.append("DlShadowWorldPos must invert the rigid u_lightview (transpose of its rotation)")

# ------------------------------------------------------------ the alias casters (P6)
inst = strip(read("engine/shaders/salias_inst_vert.glsl"))
skel = strip(read("engine/shaders/sskeletal_vert.glsl"))


def ssbo(text, block):
    m = re.search(r"VERT_SSBO\((\d+),\s*(\d+)\)\s+restrict readonly buffer " + block + r"\s*\{\s*(\w+)\s+(\w+)\[\]", text)
    return (int(m.group(1)), m.group(3)) if m else None


for block, other, oname in (("PoseBuffer", inst, "salias_inst_vert"), ("MD3PoseBuffer", inst, "salias_inst_vert"),
                            ("BoneBuffer", skel, "sskeletal_vert")):
    a, o = ssbo(avert, block), ssbo(other, block)
    if not a or a != o:
        errors.append(f"sshadow_alias_vert's {block} is {a}, {oname}'s {o}: same GL binding and element type")
for name in ("a_position", "a_weights", "a_indices"):
    a = re.search(r"VERT_IN\((\d+)\) in (\w+) " + name + ";", avert)
    o = re.search(r"VERT_IN\((\d+)\) in (\w+) " + name + ";", skel)
    if not a or not o or a.groups() != o.groups():
        errors.append(f"sshadow_alias_vert's {name} must be sskeletal_vert's slot and type (the IQM VAO's)")
for s in ("local = mix(v1, v0, u_shadowlight.w);",
          "MD3_Position(md3_pose_data[u_shadowpose.y + gl_VertexID]),\n                    "
          "MD3_Position(md3_pose_data[u_shadowpose.x + gl_VertexID]), u_shadowlight.w);",
          "return bones[u_shadowpose.x + int(i)] * u_shadowlight.w +\n           "
          "bones[u_shadowpose.y + int(i)] * (1.0 - u_shadowlight.w);",
          "if (u_shadowpose.z == 2) {", "} else if (u_shadowpose.z == 1) {",
          "rel *= 1.0 + (u_shadowlight.y + u_shadowlight.z * len) / len;",
          "v_lrel = rel * u_shadowlight.x;", "gl_Position = u_shadowvp * vec4(rel, 1.0);"):
    if s not in avert:
        errors.append(f"sshadow_alias_vert no longer says `{s.splitlines()[0]}...`")
if avert.find("rel *= 1.0 +") > avert.find("v_lrel = rel"):
    errors.append("sshadow_alias_vert must push the vertex before writing v_lrel and gl_Position")
if not re.search(r"PV_QUAKE1 = 0,.*?PV_MD3 = 1,.*?PV_IQM = 2,", strip(read("engine/h2shared/gl_model.h")), re.S):
    errors.append("u_shadowpose.z is gm->poseverttype: PV_QUAKE1 0, PV_MD3 1, PV_IQM 2")

af = body_of(shadow_c, r"static void SH_DrawAliasFace \([^)]*\)\s*\{") or ""
for s in ("R_UseProgram (sh_aprog);", "Mat4_Multiply (t, c->model, pass->model);",
          "pass->light[1] = SH_ALIAS_PUSH;", "pass->light[2] = r_shadow_bias.value * 2.0f / (float)sh_res;",
          "pass->light[3] = c->blend;", "pass->pose[0] = c->pose0;", "pass->pose[1] = c->pose1;",
          "pass->pose[2] = c->format;",
          "glBindVertexArray_fp (c->gm->vao);",
          "glDrawElements_fp (GL_TRIANGLES, c->gm->num_indices, GL_UNSIGNED_SHORT, NULL);",
          "pass->light[1] = pass->light[2] = pass->light[3] = 0.0f;",
          "pass->pose[0] = pass->pose[1] = pass->pose[2] = 0;",
          "R_UseProgram (sh_prog);", "glBindVertexArray_fp (world_vao);"):
    if s not in af:
        errors.append(f"SH_DrawAliasFace no longer does `{s}`")
if af.find("SH_Pass (pass);") < 0 or af.find("SH_Pass (pass);") > af.find("glDrawElements_fp"):
    errors.append("SH_DrawAliasFace must upload ShadowPass before each alias draw")
if not re.search(r"if \(c->format == PV_IQM\)\s*GL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 4, c->gm->ssbo_bones\);\s*"
                 r"else if \(c->format == PV_MD3\)\s*GL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 3, c->gm->ssbo_pose_md3\);\s*"
                 r"else\s*GL_BindBufferBase \(GL_SHADER_STORAGE_BUFFER, 1, c->gm->ssbo_pose\);", af):
    errors.append("SH_DrawAliasFace binds each format's buffer at the binding its shader block names (1, 3, 4)")
dm2 = body_of(shadow_c, r"static void SH_DrawMap \([^)]*\)\s*\{") or ""
if "if (d->acount > 0)\n\t\t\tSH_DrawAliasFace (d, f, &pass);" not in dm2:
    errors.append("SH_DrawMap must draw each face's alias casters after its world and brush entities")
bp3 = body_of(shadow_c, r"static qboolean SH_BuildPrograms \(void\)\s*\{") or ""
for s in ('sh_aprog = SH_LinkProgram (RPROG_SHADOW_ALIAS);',
          'glGetUniformBlockIndex_fp (sh_aprog, "ShadowPass");',
          "glUniformBlockBinding_fp (sh_aprog, blk, UBO_BINDING_SHADOW_PASS);",
          "size != (GLint)sizeof(r_shadowpass_t)"):
    if s not in bp3:
        errors.append(f"SH_BuildPrograms must build and pin the alias program: `{s}`")
# Its stages are its program-table row (#340 P1b): the alias caster vertex
# stage with the world casters' fragment stage, both on the desktop header.
if not re.search(r'\[RPROG_SHADOW_ALIAS\] = \{ "shadow_alias", RPROG_LINK_ATTRIBS, \{\s*'
                 r'\{ TIER_CORE, sshadow_alias_vert, "sshadow_alias.vert" \},\s*'
                 r'\{ TIER_CORE, sshadow_frag, "sshadow.frag" \} \} \},', shader_c):
    errors.append("RPROG_SHADOW_ALIAS must be sshadow_alias_vert + sshadow_frag on TIER_CORE")
if re.search(r"if \(!sh_aprog[^)]*\)\s*return SH_Fail", bp3) or "!sh_prog || !sh_dbgprog)" not in bp3:
    errors.append("a failed alias program must turn off the alias casters, not the maps")
if "if (sh_aprog) { glDeleteProgram_fp (sh_aprog); sh_aprog = 0; }" not in \
        (body_of(shadow_c, r"\nvoid R_Shadow_Shutdown \(void\)\s*\{") or ""):
    errors.append("R_Shadow_Shutdown must delete the alias program")
if "if (sh_aprog) { glDeleteProgram_fp (sh_aprog); sh_aprog = 0; }" not in \
        (body_of(shadow_c, r"static qboolean SH_Fail \(void\)\s*\{") or ""):
    errors.append("SH_Fail must delete the alias program")
dr = body_of(shadow_c, r"\nvoid R_Shadow_Draw \(void\)\s*\{") or ""
for b in (1, 3, 4):
    if f"GL_BindBufferBase (GL_SHADER_STORAGE_BUFFER, {b}, 0);" not in dr:
        errors.append(f"R_Shadow_Draw must leave storage binding {b} at 0 after alias draws, as the alias paths do")

on = body_of(shadow_c, r"static qboolean SH_AliasOn \(void\)\s*\{") or ""
if "r_shadow_casters.integer >= 2 && sh_aprog && r_drawentities.integer" not in on:
    errors.append("SH_AliasOn: r_shadow_casters >= 2, a built program, and r_drawentities")
el = body_of(shadow_c, r"static qboolean SH_AliasEligible \(const entity_t \*e\)\s*\{") or ""
if not re.search(r"if \(!m \|\| m->type != mod_alias \|\| e == &cl\.viewent\)\s*return false;", el):
    errors.append("SH_AliasEligible must refuse the viewmodel (cl.viewent) and anything not an alias model")
coll = body_of(rmain, r"static qboolean R_CollectAliasInstance \(entity_t \*e\)\s*\{") or ""
for s in ("(e->drawflags & DRF_TRANSLUCENT)", "(EF_TRANSPARENT | EF_HOLEY | EF_SPECIAL_TRANS)",
          "(e->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(e->alpha))"):
    if s not in el or s not in coll:
        errors.append(f"SH_AliasEligible and R_CollectAliasInstance must both refuse `{s}`")
if "r_entdist.value > 0 && e != &cl_entities[cl.viewentity]" not in el:
    errors.append("SH_AliasEligible must apply the view's r_entdist")
eh = body_of(shadow_c, r"static unsigned int SH_EntityHash \(const vec3_t org, float radius, int key\)\s*\{") or ""
cm = body_of(shadow_c, r"static qboolean SH_CollectMap \([^)]*\)\s*\{") or ""
if "if (SH_AliasCasts (a, org, radius, key) && SH_AliasPosed (a))" not in eh or "if (SH_AliasOn ())" not in eh:
    errors.append("SH_EntityHash must hash every alias caster SH_AliasCasts admits, under SH_AliasOn")
if "if (!SH_AliasCasts (a, org, radius, key) || !SH_AliasPosed (a))" not in cm:
    errors.append("SH_CollectMap must draw exactly the alias casters SH_AliasCasts admits")
if "if (sh_alias_full)\n\t\t*complete = false;" not in cm \
        or not re.search(r"if \(sh_nacasts >= SH_MAX_ACASTS\)\s*\{\s*\*complete = false;", cm):
    errors.append("SH_CollectMap: a capped alias list leaves the map incomplete")
sel2 = body_of(shadow_c, r"static void SH_Select \(void\)\s*\{") or ""
for s in ("hash = SH_EntityHash (sl->origin, sl->radius, sl->key);",
          "hash = SH_EntityHash (org, mapr, cl_dlights[cand[i].dlight].key);",
          "SH_CollectMap (cand[i].slot, org, mapr, cl_dlights[cand[i].dlight].key, &complete)",
          "SH_GatherAlias (cand, take);"):
    if s not in sel2:
        errors.append(f"SH_Select must key both questions to the light: `{s}`")
if sel2.find("SH_GatherAlias (cand, take);") > sel2.find("hash = SH_EntityHash (sl->origin"):
    errors.append("SH_Select must gather the alias casters before the first hash")
ps = body_of(shadow_c, r"static qboolean SH_AliasPosed \(shalias_t \*a\)\s*\{") or ""
for f in ("&a->c.gm", "&a->c.pose0", "&a->c.pose1", "&a->c.blend"):
    if f"SH_Fnv (h, {f}," not in ps:
        errors.append(f"SH_AliasPosed's hash must cover {f}: a pose change has to redraw the map")
rc = body_of(rmain, r"qboolean R_AliasShadowCaster \(entity_t \*e, r_aliascaster_t \*out\)\s*\{") or ""
for s in ("tmp = *e;", "R_AliasResolveLerp (&tmp, hdr, &prevpose, &pose, &blend);",
          "pitch = e->angles[0];\n\tif (e == &cl_entities[cl.viewentity])\n\t\te->angles[0] *= 0.3;\n"
          "\tR_RotateForEntity2 (e);\n\tR_AliasModelTransform (e, hdr, tmatrix, mdl_t, mdl_s);\n\te->angles[0] = pitch;",
          "GL_Translatef (mdl_t[0], mdl_t[1], mdl_t[2]);",
          "GL_Scalef (mdl_s[0], mdl_s[1], mdl_s[2]);", "out->pose0 = pose * scale;", "out->pose1 = prevpose * scale;",
          "out->spins = (e->model->flags & (EF_ROTATE | EF_FACE_VIEW)) ||\n\t\t     (R_GetPimpFlags (e, NULL) & (EF_SPIN | EF_FLOAT));"):
    if s not in rc:
        errors.append(f"R_AliasShadowCaster must pose as the draw does: `{s.splitlines()[0]}`")
# the lerp on a copy (it advances state the draw owns); the matrix from the
# entity itself (R_GetPimpFlags finds its misc_modelpimp by its cl_entities index)
if re.search(r"R_AliasResolveLerp \(e\b|R_RotateForEntity2 \(&tmp\b|R_AliasModelTransform \(&tmp\b", rc):
    errors.append("R_AliasShadowCaster: the lerp on the copy, the transform on the entity")
if not re.search(r"if \(R_IsMenuPanel \(\(entity_t \*\)e\)\)\s*return false;", el):
    errors.append("SH_AliasEligible must refuse a menu panel (UI pinned in front of the eye)")
if not re.search(r"if \(a->c\.spins\)\s*h = SH_Fnv \(h, a->ent->origin, sizeof\(vec3_t\)\);\s*else\s*"
                 r"h = SH_Fnv \(h, a->c\.model, sizeof\(a->c\.model\)\);", ps):
    errors.append("SH_AliasPosed must key a clock-driven model by origin, every other by its matrix")
if "Mod_Extradata" in rc or "Cache_Check (&e->model->cache)" not in rc:
    errors.append("R_AliasShadowCaster must take the resident header (Cache_Check), never load a model mid-frame")

# SH_AliasCasts, lifted out of gl_shadow.c, compiled and run
casts_fn = re.search(r"static qboolean SH_AliasCasts \(const shalias_t \*a, const vec3_t org, float radius, int key\)\s*\{.*?\n\}",
                     shadow_c, re.S)
struct = re.search(r"typedef struct \{[^{}]*\} shalias_t;", shadow_c)
if not casts_fn or not struct:
    errors.append("cannot lift SH_AliasCasts and shalias_t out of gl_shadow.c")
else:
    import os
    import subprocess
    import tempfile
    prog = r"""
#include <stdio.h>
#include <string.h>
typedef float vec3_t[3]; typedef int qboolean; enum { false, true };
typedef struct entity_s entity_t; typedef struct { int dummy; } r_aliascaster_t;
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define VectorSubtract(a,b,c) ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
%s
%s
static shalias_t mk (int num, float x, float y, float z) {
	shalias_t a; memset (&a, 0, sizeof(a)); a.num = num;
	a.center[0] = x; a.center[1] = y; a.center[2] = z; a.radius = 20;
	a.mins[0] = x - 13; a.mins[1] = y - 19; a.mins[2] = z + 2;
	a.maxs[0] = x + 13; a.maxs[1] = y + 19; a.maxs[2] = z + 36;
	return a;
}
int main (void) {
	vec3_t L = {0, 0, 100}, in = {5, 5, 20};
	shalias_t near = mk (7, 0, 60, 0), far = mk (8, 0, 400, 0), holds = mk (9, 0, 0, 0);
	printf ("%%d %%d %%d %%d %%d %%d %%d %%d\n",
		SH_AliasCasts (&near, L, 150, 0),	/* in reach, no owner: 1 */
		SH_AliasCasts (&near, L, 150, 7),	/* keyed to it: 0 */
		SH_AliasCasts (&near, L, 150, 8),	/* keyed to another: 1 */
		SH_AliasCasts (&far, L, 150, 0),	/* beyond reach: 0 */
		SH_AliasCasts (&holds, in, 150, 0),	/* light inside its box: 0 */
		SH_AliasCasts (&holds, L, 150, 0),	/* the same model, light above its box: 1 */
		SH_AliasCasts (&near, L, 30, -1),	/* 116.6 apart, 30 + 20 reach: 0 */
		SH_AliasCasts (&far, L, 400, 0));	/* 412 apart, 420 reach: 1 */
	return 0;
}
""" % (struct.group(0), casts_fn.group(0))
    with tempfile.TemporaryDirectory() as td:
        c, exe = Path(td) / "casts.c", Path(td) / "casts"
        c.write_text(prog)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("SH_AliasCasts harness failed to compile: " + r.stderr.strip()[:300])
        else:
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
            want = ["1", "0", "1", "0", "0", "1", "0", "1"]
            if out != want:
                errors.append(f"SH_AliasCasts answers {out}, want {want} (in reach / keyed to it / keyed "
                              "elsewhere / beyond / light in its box / light above it / just out / far, big radius)")

# ------------------------------------------------------------ the frame
setup = body_of(rmain, r"static void R_SetupGL \(void\)\s*\{") or ""
i_alias = setup.find("R_LightCluster_BindForAlias ();")
m = re.search(r"if \(!mirror\)\s*\{[^{}]*?R_Shadow_Prepare \(\);[^{}]*?R_ProfileShadowTimestamp \(false\);\s*"
              r"R_Shadow_Draw \(\);\s*R_ProfileShadowTimestamp \(true\);\s*\}", setup)
for fn in ("R_Shadow_Prepare", "R_Shadow_Draw"):
    if len(re.findall(r"\b" + fn + r" \(\)", rmain)) != 1:
        errors.append(f"gl_rmain.c must call {fn} exactly once, from R_SetupGL")
if not m or i_alias < 0 or m.start() < i_alias:
    errors.append("R_SetupGL must run R_Shadow_Prepare, then R_Shadow_Draw between the two shadow timestamps, "
                  "after R_LightCluster_BindForAlias and outside the mirror pass")
if not re.search(r"#define RPROF_SHADOW\s+\(RPROF_COUNT \+ 1\)", rmain) \
        or not re.search(r"#define RPROF_QUERIES\s+\(RPROF_COUNT \+ 3\)", rmain) \
        or "glGenQueries_fp(RPROF_QUERIES, rprof_queries);" not in rmain:
    errors.append("the profiler must allocate RPROF_QUERIES with the RPROF_SHADOW pair after RPROF_COUNT")

if errors:
    print("FAIL test_dlshadow_block:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: ShadowPass and DlShadow match their C mirrors, are bound and uploaded, decode as packed, the alias "
      "casters pose as drawn and skip their own light, "
      "and stay off the ES tier")
