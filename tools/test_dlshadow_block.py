#!/usr/bin/env python3
"""The dynamic-light shadow maps' two uniform blocks match their C mirrors, are
bound and uploaded, and stay off the ES tier (GitHub #97, P3/P4).

  * ShadowPass (engine/shaders/sshadow_vert.glsl) and r_shadowpass_t, DlShadow
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
NAMES = {"u_shadowvp": "vp", "u_shadowmodel": "model", "u_shadowlight": "light",
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


check_pair("ShadowPass", svert, "r_shadowpass_t", 144)
check_pair("DlShadow", dls, "r_dlshadow_t", 304)
if DEF.get("DLSHADOW_MAX_MAPS") != DEF.get("SHADOW_MAX_MAPS"):
    errors.append(f"DLSHADOW_MAX_MAPS {DEF.get('DLSHADOW_MAX_MAPS')} != SHADOW_MAX_MAPS {DEF.get('SHADOW_MAX_MAPS')}")
if not DEF.get("SHADOW_MAX_MAPS") or DEF["SHADOW_MAX_MAPS"] > 16:
    errors.append("SHADOW_MAX_MAPS must be 1..16: a layer is four bits of u_dlslayers")
if "if (maps > SHADOW_MAX_MAPS)\n\t\tmaps = SHADOW_MAX_MAPS;" not in read("engine/h2shared/gl_shadow.c"):
    errors.append("SH_EnsureTexture must clamp r_shadow_maxlights to SHADOW_MAX_MAPS")
if not re.search(r"VERT_UBO\(0\)\s+uniform\s+ShadowPass\b", svert):
    errors.append("ShadowPass must be the vertex stage's VERT_UBO(0)")
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
if 'glGetUniformBlockIndex_fp (program, "DlShadow")' not in ip \
        or "glUniformBlockBinding_fp (program, blk, UBO_BINDING_DLSHADOW);" not in ip \
        or 'glGetUniformLocation_fp (program, "u_dlshadowmap")' not in ip \
        or "glUniform1i_fp (loc, DLSHADOW_TMU);" not in ip or "sizeof(r_dlshadow_t)" not in ip:
    errors.append("R_Shadow_InitProgram must pin DlShadow, check its size, and set u_dlshadowmap to DLSHADOW_TMU")
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
    for m in re.finditer(r"GL_TEXTURE0\s*\+\s*(\d+)\b|R_Bind\w*\s*\(\s*(\d+)\s*,|glUniform1i_fp\s*\([^,]+,\s*(\d+)\s*\)", t):
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
        or "hash = SH_EntityHash (sl->origin, sl->radius);" not in sel:
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
print("ok: ShadowPass and DlShadow match their C mirrors, are bound and uploaded, decode as packed, "
      "and stay off the ES tier")
