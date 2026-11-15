#!/usr/bin/env python3
"""sworld_frag and sworld_opaque_frag read FragParams and WorldFrag (#450,
world slice W2).

Checks the pieces that have to move together:
  * WorldFrag in engine/shaders/world.inc and r_worldfrag_t in gl_uniforms.h
    agree member for member by name, size and int/float kind (160 bytes), each
    old loose name aliases the member its value now lives in (world.inc for
    WorldFrag, uniforms.inc for the FragParams slots the world shares), and
    the block has a binding of its own, bound, uploaded with glBufferData into
    its own buffer, invalidated, seeded and deleted like the other four;
  * the two files: no #version, the block includes in order and no other
    block, identical block, sampler and varying declarations in both (the two
    programs are switched between with no rewrite, so their layout must not
    drift), no loose non-opaque uniform, FRAG_TEX 0..n-1 with the light
    buffer's slot after them, the colour output's line first (the OIT splice
    drops it), early_fragment_tests in the opaque twin only and only outside
    GL_ES, discard in the cutout one only, and each preprocessed function
    body pinned by hash on both tiers (GOLDEN), so the bodies are still the
    literals' -- tools/test_world_frag_block.py --golden prints them;
  * the ES arm reads none of the desktop-only members (dlight scale, froxel
    lookup, lightstyle mode) and names no SSBO, findLSB, early-Z or desktop
    sampler; the desktop arm reads them all;
  * R_SetWorldFrag writes every FragParams member the stages read through its
    own setter, and all of WorldFrag from the frame record; the frame setters
    write only that record, are called from where each value is decided, with
    the values the loose uploads used, and nothing else writes the record;
  * every path's R_SetWorldFrag call passes the values that path uploaded by
    hand before (the table on r_worldfrag_t), and nothing else calls it;
  * gl_shader.c: GLSL_WORLD_FRAG_HEADER is the version line plus the grid
    sizes, the macros the literals spliced are gone, and GL_InitProgramUniforms
    binds WorldFrag;
That every path writes and flushes ALL of it before each draw, on every path
through the code, is tools/test_world_vert_block.py's member pass.
"""
from pathlib import Path
import hashlib
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []
read = lambda p: (ROOT / p).read_text()


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), t, flags=re.S))


def body_of(text, signature):
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


uh, uc = read("engine/h2shared/gl_uniforms.h"), read("engine/h2shared/gl_uniforms.c")
ucs = strip(uc)
gl_shader, shader_h = read("engine/h2shared/gl_shader.c"), read("engine/h2shared/gl_shader.h")
world_inc, uniforms_inc = (SH / "world.inc").read_text(), (SH / "uniforms.inc").read_text()
FRAGS = ("sworld_frag", "sworld_opaque_frag")
src = {f: (SH / f"{f}.glsl").read_text() if (SH / f"{f}.glsl").exists() else "" for f in FRAGS}
for f in FRAGS:
    if not src[f]:
        errors.append(f"engine/shaders/{f}.glsl is missing")

# ------------------------------------------------------------ the mirror
SIZE = {"mat4": 64, "vec4": 16, "ivec4": 16}
KIND = {"mat4": "float", "vec4": "float", "ivec4": "int"}
CNAME = {"u_wflightview": "lightview", "u_wfgrid": "grid", "u_wfgridz": "gridz", "u_wfwarp": "warp",
         "u_wfmaterial": "material", "u_wfsoftemu": "softemu", "u_wflmstyles": "lmstyles"}
wb = re.search(r"uniform WorldFrag \{(.*?)\};", strip(world_inc), re.S)
if not wb:
    errors.append("no uniform WorldFrag block in world.inc")
else:
    if re.search(r"\b(float|int|uint|bool|vec2|vec3|ivec2|ivec3|mat3)\s+\w+\s*;", wb.group(1)):
        errors.append("WorldFrag gained a non-vec4/mat4 member; std140 offsets are no longer sums of sizes")
    g = [(n, SIZE[t], KIND[t]) for t, n in re.findall(r"\b(mat4|ivec4|vec4)\s+(\w+)\s*;", wb.group(1))]
    cm = re.search(r"typedef struct \{([^{}]*)\} r_worldfrag_t;", strip(uh))
    c = [(n, 4 * int(k), t) for t, n, k in re.findall(r"\b(float|int)\s+(\w+)\[(\d+)\];", cm.group(1))] if cm else []
    if [(s, k) for _, s, k in g] != [(s, k) for _, s, k in c]:
        errors.append(f"WorldFrag {g} and r_worldfrag_t {c} differ in size/kind/order")
    if sum(s for _, s, _ in g) != 160:
        errors.append(f"WorldFrag is {sum(s for _, s, _ in g)} bytes, expected 160")
    if [CNAME.get(n, f"?{n}") for n, _, _ in g] != [n for n, _, _ in c]:
        errors.append(f"r_worldfrag_t fields {[n for n, _, _ in c]} are not the C names of WorldFrag's members")
if not re.search(r"FRAG_UBO\(1\) uniform WorldFrag", world_inc):
    errors.append("WorldFrag must be FRAG_UBO(1), after FragParams' 0")
if "COMPILE_TIME_ASSERT(worldfrag_size, sizeof(r_worldfrag_t) == 64 + 6 * 16);" not in uc:
    errors.append("COMPILE_TIME_ASSERT must pin r_worldfrag_t at 64 + 6*16")
# Old loose name -> where its value lives now.
WORLD_ALIASES = {"u_lightview": "u_wflightview", "u_lightgrid_xy": "u_wfgrid", "u_lightgrid_z": "u_wfgridz.xy",
                 "u_dlight_scale": "u_wfgridz.z", "u_turb": "u_wfwarp.xy", "u_spans": "u_wfwarp.z",
                 "u_material": "u_wfmaterial.xyz", "u_softemu": "u_wfsoftemu", "u_lmstyles": "u_wflmstyles.xy"}
FRAG_ALIASES = {"u_fog_color": "u_ffogv.xyz", "u_fog_density": "u_ffogv.w", "u_alpha_threshold": "u_fmask.x",
                "u_force_opaque_alpha": "u_fmask.y", "u_overbright": "u_fmask.z", "u_lightmap_bicubic": "u_fmask.w",
                "u_caustics": "u_fcaustics.xy", "u_lightdebug": "u_fdebug.xy"}
wsec = re.search(r"#ifdef UNIFORMS_FRAG\n(.*?)#endif", world_inc, re.S)
wdefs = dict(re.findall(r"^#define\s+(u_\w+)\s+(\S+)\s*$", wsec.group(1), re.M)) if wsec else {}
if wdefs != WORLD_ALIASES:
    errors.append(f"world.inc's aliases {wdefs} are not {WORLD_ALIASES}")
fsec = re.search(r"#ifdef UNIFORMS_FRAG\n(.*?)#endif", uniforms_inc, re.S)
fdefs = dict(re.findall(r"^#define\s+(u_\w+)\s+(\S+)\s*$", fsec.group(1), re.M)) if fsec else {}
for old, new in FRAG_ALIASES.items():
    if fdefs.get(old) != new:
        errors.append(f"uniforms.inc must map {old} to {new}")

# ------------------------------------------------------------ binding, upload, life cycle
allbind = {}
for f in ("engine/h2shared/gl_shader.h", "engine/h2shared/gl_postprocess.c", "engine/h2shared/gl_ssao.c",
          "engine/h2shared/gl_worldcull.c", "engine/h2shared/gl_lightcluster.c"):
    for name, num in re.findall(r"#define\s+(\w*UBO_BINDING\w*)\s+(\d+)", read(f)):
        allbind.setdefault(int(num), []).append(name)
bind = re.search(r"#define\s+UBO_BINDING_WORLD_FRAG\s+(\d+)", shader_h)
if not bind:
    errors.append("gl_shader.h: no UBO_BINDING_WORLD_FRAG")
elif allbind.get(int(bind.group(1))) != ["UBO_BINDING_WORLD_FRAG"]:
    errors.append(f"UBO binding {bind.group(1)} is shared: {allbind.get(int(bind.group(1)))}")
flush = body_of(ucs, r"\nvoid R_FlushUniforms \(void\)\s*\{") or ""
if "GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_WORLD_FRAG, ubo_wfrag);" not in flush:
    errors.append("R_FlushUniforms must bind ubo_wfrag at UBO_BINDING_WORLD_FRAG")
fl = re.search(r"if \(wfrag_dirty\)\s*\{(.*?)\}", flush, re.S)
if not fl or not re.search(r"GL_BindBuffer \(GL_UNIFORM_BUFFER, ubo_wfrag\);\s*glBufferData_fp \(GL_UNIFORM_BUFFER, "
                           r"sizeof\(worldfrag\), &worldfrag,", fl.group(1)) \
        or re.search(r"\bubo_(?!wfrag)\w+", fl.group(1)) or "wfrag_dirty = false;" not in fl.group(1):
    errors.append("R_FlushUniforms' wfrag branch must clear the flag and fill ubo_wfrag, and only ubo_wfrag, with glBufferData")
inv = body_of(ucs, r"\nvoid R_InvalidateUniforms \(void\)\s*\{") or ""
if "wfrag_dirty = true;" not in inv:
    errors.append("R_InvalidateUniforms must re-upload WorldFrag too")
init = body_of(ucs, r"\nvoid R_Uniforms_Init \(void\)\s*\{") or ""
for w in ("memset (&worldfrag, 0, sizeof(worldfrag));", "memset (&worldframe, 0, sizeof(worldframe));",
          "memcpy (worldfrag.lightview, ident, sizeof(ident));", "memcpy (worldframe.lightview, ident, sizeof(ident));",
          "glGenBuffers_fp (1, &ubo_wfrag);", "wfrag_dirty = true;"):
    if w not in init:
        errors.append(f"R_Uniforms_Init: missing {w}")
shut = body_of(ucs, r"\nvoid R_Uniforms_Shutdown \(void\)\s*\{") or ""
if "glDeleteBuffers_fp (1, &ubo_wfrag);" not in shut:
    errors.append("R_Uniforms_Shutdown must delete ubo_wfrag")
if not re.search(r"GLint R_BindWorldFragBlock \(GLuint program\)\s*\{.*?RU_BindOne \(program, RU_NAME_WFRAG, "
                 r"UBO_BINDING_WORLD_FRAG\)", ucs, re.S) or '#define RU_NAME_WFRAG	"WorldFrag"' not in uc:
    errors.append("R_BindWorldFragBlock must pin the \"WorldFrag\" block to UBO_BINDING_WORLD_FRAG")
gipu = body_of(strip(gl_shader), r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or ""
if "p->ub_wfrag = R_BindWorldFragBlock(p->program);" not in gipu \
        or "R_BindProgramBlocks(p->program, &p->ub_vert, &p->ub_frag);" not in gipu:
    errors.append("GL_InitProgramUniforms must bind FragParams and WorldFrag (ub_frag, ub_wfrag) on every program")
if not re.search(r"\bGLint\s+ub_wfrag;", shader_h):
    errors.append("glprogram_t needs ub_wfrag, which GL_ImmPushBlocks and R_SetSpanCell key the world writes on")

# ------------------------------------------------------------ the files


def expand(text, seen=None):
    """EmbedShaders.cmake's plain-text include expansion."""
    for _ in range(32):
        m = re.search(r'#include "([^"]+)"', text)
        if not m:
            break
        text = text.replace(f'#include "{m.group(1)}"\n', (SH / m.group(1)).read_text())
    return text


def preprocess(text, defined):
    """The arm of `text` a compiler with `defined` predefined keeps: #ifdef,
    #ifndef, #if/#elif defined(X), #else, #endif, #define/#undef X.  Function-
    and object-like macro bodies are not expanded; callers scan for names."""
    out, stack, defs = [], [], set(defined)

    def live():
        return all(s[0] for s in stack)

    def ev(expr):
        e = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", lambda m: "1" if (m.group(1) or m.group(2)) in defs else "0", expr)
        e = re.sub(r"\b[A-Za-z_]\w*\b", "0", e)
        return bool(eval(e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")))
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif|define|undef)\b\s*(.*)", line)
        if not m:
            if live():
                out.append(line)
            continue
        d, rest = m.group(1), m.group(2).strip()
        if d in ("ifdef", "ifndef"):
            name = rest.split()[0]
            v = (name in defs) == (d == "ifdef")
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
        elif d == "define" and live():
            defs.add(rest.split("(")[0].split()[0])
            out.append(line)
        elif d == "undef" and live():
            defs.discard(rest.split()[0])
    return "\n".join(out)


TOK = r"\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|0x[0-9A-Fa-f]+u?|\d+u?|\w+|[-+*/%<>=!&|^]=|&&|\|\||\+\+|--|<<=|>>=|<<|>>|\S"
ALIASES = {**WORLD_ALIASES, **FRAG_ALIASES}
DESKTOP_ONLY = {"u_dlight_scale", "u_lightview", "u_lightgrid_xy", "u_lightgrid_z", "u_lmstyles"}
ES_FORBIDDEN = DESKTOP_ONLY | {"u_lightgrid", "u_lmlayer1", "u_lmlayer2", "u_lmlayer3", "Lights", "LightStyles",
                               "findLSB", "early_fragment_tests", "buffer", "u_wflightview", "u_wfgrid",
                               "u_wflmstyles"}


def functions_of(text):
    """name -> token list of every function definition in preprocessed GLSL."""
    toks = re.findall(TOK, strip(text))
    fns, i = {}, 0
    while i < len(toks):
        if toks[i] == "(" and i > 1 and re.fullmatch(r"\w+", toks[i - 1]) and re.fullmatch(r"\w+", toks[i - 2]):
            j, depth = i, 0
            while j < len(toks):
                depth += {"(": 1, ")": -1}.get(toks[j], 0)
                if depth == 0:
                    break
                j += 1
            if j + 1 < len(toks) and toks[j + 1] == "{":
                k, depth = j + 1, 0
                while k < len(toks):
                    depth += {"{": 1, "}": -1}.get(toks[k], 0)
                    if depth == 0:
                        break
                    k += 1
                fns[toks[i - 1]] = toks[i - 2:k + 1]
                i = k + 1
                continue
        i += 1
    return fns


def digest(fns):
    return hashlib.sha256("\n".join(f"{n}: {' '.join(t)}" for n, t in fns.items()).encode()).hexdigest()[:16]


# Pinned at the move: the function bodies, helpers included, as each tier's
# preprocessor leaves them, token for token.  At #450 W2 these were proven
# token-identical to the literals they replaced (aliases expanded) on both
# tiers and for the OIT splice.  A deliberate shader change updates them:
# run this file with --golden.
GOLDEN = {
    ("sworld_frag", "desk"): "b569cef5ad4db0cb",
    ("sworld_frag", "es"): "20aa06ceead5c04e",
    ("sworld_opaque_frag", "desk"): "41dd74c6e03d48d0",
    ("sworld_opaque_frag", "es"): "63569b0838c0cfaa",
}
ARMS = {"desk": set(), "es": {"GL_ES"}}
expanded = {f: expand(src[f]) for f in FRAGS}
decls, fn_sets = {}, {}
for f in FRAGS:
    if not src[f]:
        continue
    text, code = src[f], strip(src[f])
    if re.search(r"^\s*#\s*version\b", text, re.M):
        errors.append(f"{f}.glsl carries a #version; GLSL_WORLD_FRAG_HEADER supplies it per tier")
    if '#include "layout.inc"\n#define UNIFORMS_FRAG\n#include "uniforms.inc"\n#include "world.inc"\n' not in text:
        errors.append(f"{f}.glsl must include layout.inc, then uniforms.inc with UNIFORMS_FRAG, then world.inc")
    blocks = re.findall(r"\buniform\s+(\w+)\s*\{", strip(preprocess(expanded[f], set())))
    if sorted(blocks) != ["FragParams", "WorldFrag"]:
        errors.append(f"{f}.glsl (includes expanded) declares blocks {blocks}; the world stages read exactly "
                      "FragParams and WorldFrag")
    if re.search(r"#\s*define\s+UNIFORMS_VERT\b|alias\.inc", code):
        errors.append(f"{f}.glsl pulls in a vertex block or AliasFrag")
    loose = [m.group(1) for m in re.finditer(
        r"^\s*(?:FRAG_TEX\(\d+\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?![iu]?sampler)\w+\s+(\w+)\s*;",
        strip(expanded[f]), re.M)]
    if loose:
        errors.append(f"{f}.glsl declares loose non-opaque uniforms {loose}")
    samplers = re.findall(r"FRAG_TEX\((\d+)\) uniform (?:highp )?(\w+) (\w+);", code)
    if [int(b) for b, _, _ in samplers] != list(range(len(samplers))):
        errors.append(f"{f}.glsl: FRAG_TEX bindings must be 0..n-1 in set 2, in order: {samplers}")
    if not re.search(rf"#define LIGHTGRID_SSBO_SLOT {len(samplers)}\n", code):
        errors.append(f"{f}.glsl: LIGHTGRID_SSBO_SLOT must follow the {len(samplers)} sampled textures in set 2")
    decls[f] = (samplers, [" ".join(x.split()) for x in re.findall(r"^FRAG_(?:IN|OUT)\(\d+\).*$", code, re.M)])
    lines = [l for l in expanded[f].splitlines() if "fragColor;" in l]
    if not lines or lines[0].strip() != "FRAG_OUT(0) out vec4 fragColor;":
        errors.append(f"{f}.glsl: the first line holding `fragColor;` after include expansion must be the "
                      "one-line FRAG_OUT(0) declaration (the OIT splice drops it)")
    desk = "".join(re.findall(r"#ifndef GL_ES\n(.*?)#(?:endif|else)", code, re.S))
    for want in ("usampler3D u_lightgrid", "sampler2D u_lmlayer1", "sampler2D u_lmlayer2", "sampler2D u_lmlayer3",
                 '#include "lightgrid.inc"', '#include "world_dlight.inc"', '#include "lmstyles.inc"',
                 "if (u_lmstyles.x > 0.5) lm = LMStyles(lm, lmuv);",
                 "if (u_dlight_scale > 0.0) lm.rgb = ClusteredDlight(lm.rgb, v_eyepos);"):
        if want not in desk:
            errors.append(f"{f}.glsl: `{want}` must sit under #ifndef GL_ES")
    for arm, defined in ARMS.items():
        pp = preprocess(expanded[f], defined)
        # What the arm READS: its code, without the #define lines and the
        # block declarations, which exist on both tiers so the layout never
        # varies.
        body = re.sub(r"\buniform\s+\w+\s*\{.*?\};", " ", re.sub(r"(?m)^\s*#.*$", " ", strip(pp)), flags=re.S)
        names = set(re.findall(r"\b\w+\b", body))
        for n in list(names):
            if n in ALIASES:
                names.add(ALIASES[n].split(".")[0])
        if arm == "es" and names & ES_FORBIDDEN:
            errors.append(f"{f}.glsl's ES arm names desktop-only {sorted(names & ES_FORBIDDEN)}; WebGL2 has no "
                          "SSBO, compute pass or early-Z, and those members are never written for it")
        if arm == "desk" and not DESKTOP_ONLY <= names:
            errors.append(f"{f}.glsl's desktop arm lost {sorted(DESKTOP_ONLY - names)}")
        fns = functions_of(pp)
        fn_sets[(f, arm)] = fns
        if "main" not in fns:
            errors.append(f"{f}.glsl ({arm}): cannot find main")
        elif "--golden" not in sys.argv and digest(fns) != GOLDEN.get((f, arm)):
            errors.append(f"{f}.glsl's {arm} functions ({len(fns)}, {sum(map(len, fns.values()))} tokens) differ from "
                          f"the pinned bodies ({digest(fns)} != {GOLDEN.get((f, arm))}); a deliberate change "
                          "updates GOLDEN (--golden)")
    ez = re.findall(r"layout\s*\(\s*early_fragment_tests\s*\)\s*in\s*;", code)
    if f == "sworld_opaque_frag":
        if len(ez) != 1 or "layout(early_fragment_tests) in;" not in "".join(re.findall(r"#ifndef GL_ES\n(.*?)#endif", code, re.S)):
            errors.append("sworld_opaque_frag.glsl must declare early_fragment_tests once, under #ifndef GL_ES "
                          "(Hi-Z on the opaque world, uhexen2-5c6r)")
        if re.search(r"\bdiscard\b", code):
            errors.append("sworld_opaque_frag.glsl discards: early depth writes would then occlude through holes")
    else:
        if ez:
            errors.append("sworld_frag.glsl forces early_fragment_tests: a discarded fence texel would still write "
                          "depth (uhexen2-238u)")
        if len(re.findall(r"\bdiscard\b", code)) != 1:
            errors.append("sworld_frag.glsl must keep its one alpha-test discard")
        if "#ifdef OIT\n    fragColor = color;\n#else" not in text:
            errors.append("sworld_frag.glsl lost its OIT arm (world_oit compiles it with OIT defined)")
if len(decls) == 2 and decls["sworld_frag"] != decls["sworld_opaque_frag"]:
    errors.append("sworld_frag.glsl and sworld_opaque_frag.glsl declare different samplers, inputs or outputs; "
                  "the two are switched between at the same call sites and must keep one layout")
if "--golden" in sys.argv:
    for k, fns in fn_sets.items():
        print(k, digest(fns), len(fns), sum(map(len, fns.values())))
    sys.exit(0)

# ------------------------------------------------------------ the writers
wf = body_of(ucs, r"\nvoid R_SetWorldFrag \([^)]*\)\s*\{") or ""
for w in (r"r_worldfrag_t\s+w = worldframe;", r"R_SetFog \(fog_density, fog_rgb\);",
          r"R_SetAlphaMask \(alpha_threshold, force_opaque\);", r"R_SetOverbright \(overbright\);",
          r"R_SetLightmapBicubic \(worldframe_bicubic\);",
          r"R_SetWorldCaustics \(worldframe_caustics\[0\], worldframe_caustics\[1\]\);",
          r"R_SetLightDebug \(worldframe_debug\[0\], worldframe_debug\[1\]\);",
          r"w\.warp\[0\] = turb_amplitude;", r"w\.warp\[1\] = turb_time;",
          r"if \(memcmp \(&worldfrag, &w, sizeof\(w\)\)\)\s*\{\s*worldfrag = w;\s*wfrag_dirty = true;\s*\}"):
    if not re.search(w, wf):
        errors.append(f"R_SetWorldFrag must run {w}")
if re.search(r"\b(?:return|goto)\b|\bif\s*\((?!memcmp)", wf):
    errors.append("R_SetWorldFrag must write everything, unconditionally (its one compare is the dirty check)")
if re.search(r"\bw\.warp\[2\]|\bw\.(?!warp\[[01]\])\w+\s*(?:\[\d+\])?\s*=", wf):
    errors.append("R_SetWorldFrag may override only the liquid warp from its arguments; the rest is the frame record's")
FRAME = {
    "R_SetWorldFrame": [r"worldframe_caustics\[0\] = caustics_intensity;", r"worldframe_caustics\[1\] = caustics_time;",
                        r"worldframe_bicubic = bicubic;", r"worldframe_debug\[0\] = fullbright;",
                        r"worldframe_debug\[1\] = lightmapdbg;",
                        r"RU_Set4 \(worldframe\.material, material3\[0\], material3\[1\], material3\[2\], 0\.0f\);",
                        r"RU_Set4 \(worldframe\.softemu, softemu4\[0\], softemu4\[1\], softemu4\[2\], softemu4\[3\]\);"],
    "R_SetWorldSpans": [r"worldframe\.warp\[2\] = cell;"],
    "R_SetWorldDlight": [r"RU_SetMat \(worldframe\.lightview, view16\);",
                         r"RU_Set4 \(worldframe\.grid, grid4\[0\], grid4\[1\], grid4\[2\], grid4\[3\]\);",
                         r"RU_Set4 \(worldframe\.gridz, zlogscale, zlogbias, scale, 0\.0f\);"],
    "R_SetWorldLMStyles": [r"RU_Set4 \(worldframe\.lmstyles, on, shift, 0\.0f, 0\.0f\);"],
}
for fn, wants in FRAME.items():
    b = body_of(ucs, rf"\nvoid {fn} \([^)]*\)\s*\{{") or ""
    if not b:
        errors.append(f"gl_uniforms.c: no {fn}")
        continue
    for w in wants:
        if not re.search(w, b):
            errors.append(f"{fn} must run {w}")
    if re.search(r"\b(?:vertparams|fragparams|worldfrag|aliasfrag|aliasvert)\b|_dirty\b", b):
        errors.append(f"{fn} touches a shadow or a dirty flag; the frame record reaches the GPU only through R_SetWorldFrag")
# Only those setters, R_SetWorldFrag (reads) and the life cycle name the record.
for m in re.finditer(r"\bworldframe(?:_\w+)?\b", ucs):
    owner = None
    for fm in re.finditer(r"\n(?:static\s+)?\w[\w \t\*]*?\b(\w+)\s*\([^;{)]*\)\s*\{", ucs[:m.start()]):
        owner = fm.group(1)
    depth = ucs[:m.start()].count("{") - ucs[:m.start()].count("}")
    if depth and owner not in set(FRAME) | {"R_SetWorldFrag", "R_Uniforms_Init"}:
        errors.append(f"gl_uniforms.c: {owner} names the world frame record; only its setters may")
if len(re.findall(r"\bworldfrag\s*=", ucs)) != 1:
    errors.append("WorldFrag's shadow must be assigned in R_SetWorldFrag alone")

rmain, rsurf = strip(read("engine/hexen2/gl_rmain.c")), strip(read("engine/hexen2/gl_rsurf.c"))
lc, vbo, wcull = strip(read("engine/h2shared/gl_lightcluster.c")), strip(read("engine/h2shared/gl_vbo.c")), \
    strip(read("engine/h2shared/gl_worldcull.c"))
warp = strip(read("engine/h2shared/gl_warp.c"))
sf = body_of(rmain, r"static void R_SetupFrame \(void\)\s*\{") or ""
if not re.search(r"GL_MaterialParams \(mat\);\s*R_SoftEmuParams \(se\);\s*R_SetWorldFrame \(R_CausticsIntensity\(\), "
                 r"\(float\)cl\.time,\s*r_lightmap_bicubic\.integer \? 1\.0f : 0\.0f,\s*r_fullbright\.integer \? 1\.0f : "
                 r"0\.0f,\s*r_lightmap\.integer \? 1\.0f : 0\.0f, mat, se\);\s*R_SetWorldSpans \(R_SpanCell \(false\)\);", sf):
    errors.append("R_SetupFrame must fill the world frame record with the values it pushed to the three programs")
if sf.find("R_SetWorldFrame") < sf.find("r_viewleaf = Mod_PointInLeaf"):
    errors.append("R_SetupFrame must set the caustics after r_viewleaf moves (R_CausticsIntensity reads it)")
bfw = body_of(lc, r"void R_LightCluster_BindForWorld \(void\)\s*\{") or ""
if not re.search(r"\}\s*R_SetWorldDlight \(scale, lc_view, lc_vp, lc_zlogscale, lc_zlogbias\);\s*if \(scale <= 0\.0f\)", bfw):
    errors.append("R_LightCluster_BindForWorld must record the scale and lookup for every frame, after deciding the scale")
lsf = body_of(rsurf, r"void R_LightStyles_FrameSetup \(void\)\s*\{") or ""
if "R_SetWorldLMStyles (lm_gpu_styles ? 1.0f : 0.0f, gl_overbright.integer ? 1.0f : 2.0f);" not in lsf:
    errors.append("R_LightStyles_FrameSetup must record the lightstyle mode and the CPU shift factor")
spc = body_of(rmain, r"void R_SetSpanCell \(const glprogram_t \*p, float cell\)\s*\{") or ""
if not re.search(r"if \(p->ub_wfrag >= 0\)\s*R_SetWorldSpans \(cell\);", spc):
    errors.append("R_SetSpanCell must put a world program's cell in the frame record")
ewp = body_of(warp, r"void EmitWaterPolys \(msurface_t \*fa\)\s*\{") or ""
if not re.search(r"R_SetSpanCell \(prog, R_SpanCell \(true\)\);.*R_SetSpanCell \(prog, lit \? R_SpanCell \(false\) : 0\.0f\);", ewp, re.S):
    errors.append("lit water must bracket its batches' span cell and leave the frame's behind")
CALLS = {"R_SetWorldFrame": 1, "R_SetWorldSpans": 2, "R_SetWorldDlight": 1, "R_SetWorldLMStyles": 1}
engine_c = {p: strip(p.read_text(errors="replace")) for d in ("engine/h2shared", "engine/hexen2")
            for p in sorted((ROOT / d).glob("*.c")) if p.name != "gl_uniforms.c"}
for fn, n in CALLS.items():
    got = sum(len(re.findall(rf"\b{fn}\s*\(", t)) for t in engine_c.values())
    if got != n:
        errors.append(f"{fn} is called {got} times outside gl_uniforms.c, expected {n}")

# Per path: the values it used to upload by hand.  (fog, rgb, alpha, force, overbright, turb amp, turb time)
UNGATED = r"gl_overbright\.integer \? 2\.0f : 1\.0f"
ARGS = {
    "GL_ImmPushBlocks": (vbo, r"if \(shader->ub_wfrag >= 0\)\s*R_SetWorldFrag \(r_fog_density, r_fog_color,\s*"
                              r"imm_alpha_threshold >= 0\.0f \? imm_alpha_threshold : 0\.0f,\s*"
                              r"imm_force_opaque_alpha >= 0\.0f \? imm_force_opaque_alpha : 0\.0f,\s*"
                              r"\(gl_overbright\.integer && !r_fullbright\.integer\) \? 2\.0f : 1\.0f,\s*"
                              r"imm_turb\[0\], imm_turb\[1\]\);"),
    "DrawTextureChains_FlushBatch": (rsurf, rf"R_SetWorldFrag \(world_state_fog_density, world_state_fog_color, 0\.01f, "
                                            rf"1\.0f,\s*{UNGATED}, 0\.0f, 0\.0f\);"),
    "R_DrawBrushModel": (rsurf, rf"R_SetWorldFrag \(r_fog_density, r_fog_color, 0\.01f, 1\.0f,\s*{UNGATED}, 0\.0f, 0\.0f\);"),
    "R_DispatchBrushInstancedPass": (rmain, r"R_SetWorldFrag \(r_fog_density, r_fog_color, alpha_threshold, 1\.0f,\s*"
                                            r"overbright, 0\.0f, 0\.0f\);"),
    "R_DrawWorldCulled": (wcull, rf"R_SetWorldFrag \(r_fog_density, r_fog_color, 0\.01f, 1\.0f,\s*{UNGATED}, 0\.0f, 0\.0f\);"),
}
for fn, (text, want) in ARGS.items():
    b = body_of(text, rf"\b{fn} \([^;{{]*\)\s*\{{") or ""
    if len(re.findall(r"\bR_SetWorldFrag\s*\(", b)) != 1 or not re.search(want, b):
        errors.append(f"{fn} must make exactly one R_SetWorldFrag call, with the values it uploaded by hand: {want[:90]}")
dd = body_of(rsurf, r"static void DrawTextureChains_DrawDeferred \([^;{]*\)\s*\{") or ""
BS = r"(?:\s*\\)*\s*"     # whitespace, and the macro's line continuations
if not re.search(rf"#define EMIT_BATCH\(BUF, N, ALPHA_T, A2C_ON\) do \{{{BS}if \(\(N\) <= 0\) break;{BS}"
                 rf"R_SetWorldFrag \(r_fog_density, r_fog_color, \(ALPHA_T\), 1\.0f,{BS}{UNGATED}, 0\.0f, 0\.0f\);{BS}"
                 r"R_FlushUniforms \(\);", dd) \
        or not re.search(r"EMIT_BATCH\(fence_buf, fence_n, 0\.666f, r_alphatocoverage\.integer\);\s*"
                         r"EMIT_BATCH\(water_buf, water_n, 0\.01f,\s*false\);", dd):
    errors.append("DrawDeferred's EMIT_BATCH must write the fragment set with its own threshold (0.666 fences, "
                  "0.01 underwater) and flush before its draws")
inst = body_of(rmain, r"void R_DrawBrushInstanced \(void\)\s*\{") or ""
if not re.search(rf"R_UseProgram \(prog_opaque->program\);\s*R_DispatchBrushInstancedPass\(world_surf_keys, "
                 rf"num_world_surf_keys, 0\.01f,\s*{UNGATED}\);", inst) \
        or not re.search(r"R_UseProgram \(prog_cutout->program\);.*?R_DispatchBrushInstancedPass\(world_surf_keys_fence, "
                         r"num_world_surf_keys_fence, 0\.666f,\s*\(gl_overbright\.integer && !r_fullbright\.integer\) "
                         r"\? 2\.0f : 1\.0f\);", inst, re.S):
    errors.append("R_DrawBrushInstanced must hand its opaque pass 0.01 / the ungated overbright and its fence pass "
                  "0.666 / the gated one")
bws = body_of(rsurf, r"static void DrawTextureChains_BindWorldState \(void\)\s*\{") or ""
if not re.search(r"world_state_fog_density = r_fog_density;\s*VectorCopy \(r_fog_color, world_state_fog_color\);", bws):
    errors.append("DrawTextureChains_BindWorldState must save the fog its batches draw with")
for v in ("world_state_fog_density", "world_state_fog_color"):
    if len(re.findall(rf"\b{v}\b", rsurf)) != 3:
        errors.append(f"{v} must be a file static filled only by BindWorldState and read only by FlushBatch")
calls = sum(len(re.findall(r"\bR_SetWorldFrag\s*\(", t)) for t in engine_c.values())
if calls != 6:
    errors.append(f"R_SetWorldFrag is called {calls} times; the six world draw paths call it once each "
                  "(DrawDeferred inside EMIT_BATCH)")

# ------------------------------------------------------------ gl_shader.c
if not re.search(r"#define GLSL_WORLD_FRAG_HEADER\tGLSL_FRAG_HEADER GLSL_LIGHTGRID_CONSTS\n", gl_shader):
    errors.append("GLSL_WORLD_FRAG_HEADER must be GLSL_FRAG_HEADER followed by GLSL_LIGHTGRID_CONSTS")
for gone in ("GLSL_EARLY_Z", "GLSL_EARLY_Z_OPAQUE", "GLSL_BITFIELD_REVERSE", "GLSL_BICUBIC_LM_FN",
             "GLSL_SOFTEMU_NOISE_FN", "GLSL_DLIGHT_DECL", "GLSL_DLIGHT_FN", "GLSL_DLIGHT_APPLY",
             "GLSL_LMSTYLES_DECL", "GLSL_LMSTYLES_FN", "GLSL_LMSTYLES_APPLY", "GL_CompileOITFragShader",
             "GL_ClearTurb", "GL_MaterialUniform", "LC_SetWorldUniforms"):
    for p, t in list(engine_c.items()) + [(ROOT / "engine/h2shared/gl_vbo.h", strip(read("engine/h2shared/gl_vbo.h")))]:
        if re.search(rf"\b{gone}\b", t):
            errors.append(f"{p.relative_to(ROOT)} still names {gone}; #450 W2 retired it")
if re.search(r"static const char sworld_frag(?:_opaque)?\[\]", gl_shader):
    errors.append("a world fragment literal is back in gl_shader.c")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(dict.fromkeys(errors)))
print(f"PASS sworld_frag / sworld_opaque_frag read FragParams + WorldFrag (160 bytes); files, both tiers, the frame "
      f"record and all six paths' arguments agree (#450 W2)")
