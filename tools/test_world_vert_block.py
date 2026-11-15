#!/usr/bin/env python3
"""sworld_vert reads its uniforms from VertParams (#450, world slice W1).

Checks the pieces that have to move together:
  * VertParams in engine/shaders/uniforms.inc and r_vertparams_t in
    gl_uniforms.h agree member for member by name, size and int/float kind
    (368 bytes);
  * the stage reads exactly u_mvp and u_modelview, which alias u_mvpm and
    u_modelviewm, and R_SetMVP / R_SetModelView write exactly those members,
    dirty VertParams and cannot leave early;
  * engine/shaders/sworld_vert.glsl has no #version, includes only layout.inc
    and uniforms.inc (UNIFORMS_VERT), declares no loose uniform, keeps
    `invariant gl_Position`, and its main matches a pinned token stream
    (GOLDEN, token-identical to the literal it replaced);
  * every input has VERT_IN equal to its ATTR_* slot in gl_shader.h, with the
    width the imm and world VAOs feed it;
  * its outputs match sworld_frag's and sworld_frag_opaque's inputs (still C
    literals) by name, type, interpolation and precision, and their VERT_OUT
    locations are distinct and 0..n-1;
  * the literal is gone, the three world programs compile the file through
    GLSL_VERT_HEADER with a NULL fragment header (world_oit desktop-only),
    GL_LoadOITProgramSplit takes the literal splice only for a NULL header,
    the WebGL smoke still builds both ES world programs, and no glprogram_t
    field, lookup or C identifier for the old loose u_mvp / u_modelview is
    left;
  * every path that draws with a world program writes both members and
    flushes before it draws, because a block member is global where the loose
    uniform was this program's own:
      GL_ImmEnd / GL_ImmPushBlocks       the GL_ImmEnd batches (water, lit
                                         water, brush polys, the imm world)
      DrawTextureChains_FlushBatch       the world fast path, from the pair
                                         DrawTextureChains_BindWorldState saved
      DrawTextureChains_DrawDeferred     fence + underwater world batches
      R_DrawBrushModel                   the brush-entity fast path
      R_DispatchBrushInstancedPass       instanced brush entities, per instance
      R_DrawWorldCulled                  the GPU-culled indirect world draw
    Each setter call and the flush is unconditional within its path (no
    braceless or extra enclosing if/else/loop beyond the ones pinned per
    path, no #if), comes after the matrix fill, and no block writer runs
    between the flush and any draw it serves.  Every function anywhere in
    the engine that names a world program and issues a glDraw* is one of
    these, so a new path has to be added here, and two of them (FlushBatch,
    DispatchBrushInstancedPass) are reached only from their pinned callers.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
read = lambda p: (ROOT / p).read_text()


def strip(t):
    """Comments out, line structure kept (offsets shift, line count does not)."""
    t = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), t, flags=re.S)
    return re.sub(r"//[^\n]*", " ", t)


def strip_c(t):
    """strip() plus string literals blanked, for C code scans."""
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', strip(t))


def body_of(text, signature):
    m = re.compile(signature).search(text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    return None


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
                m = re.search(r"\b(\w+)\s*\([^;{}]*\)\s*$", code[last:start])
                out.append((m.group(1) if m else None, start, i + 1))
                last = i + 1
        elif ch == ";" and depth == 0:
            last = i + 1
    return out


gl_shader = read("engine/h2shared/gl_shader.c")
gsc = strip_c(gl_shader)
shader_h = read("engine/h2shared/gl_shader.h")
uh = read("engine/h2shared/gl_uniforms.h")
uc = read("engine/h2shared/gl_uniforms.c")
uniforms_inc = read("engine/shaders/uniforms.inc")
vpath = ROOT / "engine/shaders/sworld_vert.glsl"
vert = vpath.read_text() if vpath.exists() else ""
vcode = strip(vert)
if not vert:
    errors.append("engine/shaders/sworld_vert.glsl is missing")

# ------------------------------------------------------------ the mirror
SIZE = {"mat4": 64, "vec4": 16, "ivec4": 16}
KIND = {"mat4": "float", "vec4": "float", "ivec4": "int"}
CNAME = {"u_mvpm": "mvp", "u_modelviewm": "modelview", "u_modelm": "model",
         "u_viewprojm": "viewproj", "u_vscene": "scene", "u_vpup": "pup",
         "u_vpright": "pright", "u_vvpn": "vpn", "u_vporigin": "porigin",
         "u_vposei": "posei", "u_valias": "alias"}
vb = re.search(r"uniform VertParams \{(.*?)\};", strip(uniforms_inc), re.S)
if not vb:
    errors.append("no uniform VertParams block in uniforms.inc")
else:
    if re.search(r"\b(float|int|uint|bool|vec2|vec3|ivec2|ivec3|mat3)\s+\w+\s*;", vb.group(1)):
        errors.append("VertParams gained a non-vec4/mat4 member; std140 offsets are no longer sums of sizes")
    g = [(n, SIZE[t], KIND[t]) for t, n in re.findall(r"\b(mat4|ivec4|vec4)\s+(\w+)\s*;", vb.group(1))]
    cm = re.search(r"typedef struct \{([^{}]*)\} r_vertparams_t;", uh)
    c = [(n, 4 * int(k), t) for t, n, k in re.findall(r"\b(float|int)\s+(\w+)\[(\d+)\];", cm.group(1))] if cm else []
    if [(s, k) for _, s, k in g] != [(s, k) for _, s, k in c]:
        errors.append(f"VertParams {g} and r_vertparams_t {c} differ in size/kind/order")
    if sum(s for _, s, _ in g) != 368:
        errors.append(f"VertParams is {sum(s for _, s, _ in g)} bytes, expected 368")
    if [CNAME.get(n, f"?{n}") for n, _, _ in g] != [n for n, _, _ in c]:
        errors.append(f"r_vertparams_t fields {[n for n, _, _ in c]} are not the C names of VertParams' members")
    if not re.search(r"VERT_UBO\(0\) uniform VertParams", uniforms_inc):
        errors.append("VertParams must be VERT_UBO(0)")

# ------------------------------------------------------------ names and setters
READS = {"u_mvp": ("u_mvpm", "R_SetMVP", "mvp"), "u_modelview": ("u_modelviewm", "R_SetModelView", "modelview")}
uv = re.search(r"#ifdef UNIFORMS_VERT\n(.*?)#endif", uniforms_inc, re.S)
for old, (member, _, _) in READS.items():
    if not uv or not re.search(rf"^#define\s+{old}\s+{member}\s*$", uv.group(1), re.M):
        errors.append(f"{old} must alias VertParams' {member} under UNIFORMS_VERT")
used = set(re.findall(r"\bu_\w+", vcode))
if used != set(READS):
    errors.append(f"sworld_vert.glsl reads {sorted(used)}; the draw paths below write exactly {sorted(READS)}")
ucode = strip(uc)
for old, (_, setter, slot) in READS.items():
    b = body_of(ucode, rf"\nvoid {setter} \(const float \*m16\)\s*\{{")
    if b is None:
        errors.append(f"gl_uniforms.c: no void {setter} (const float *m16)")
        continue
    if not re.search(rf"\bif \(RU_SetMat\(vertparams\.{slot}, m16\)\)\s*vert_dirty = true;", b):
        errors.append(f"{setter} must be `if (RU_SetMat(vertparams.{slot}, m16)) vert_dirty = true;`")
    if re.search(r"\b(?:return|goto|longjmp)\b", b) or set(re.findall(r"\b\w+_dirty\b", b)) != {"vert_dirty"}:
        errors.append(f"{setter} must reach its write and dirty VertParams only")
if not re.search(r"\nvoid\s+R_SetMVP \(const float \*m16\);", uh) or \
        not re.search(r"\nvoid\s+R_SetModelView \(const float \*m16\);", uh):
    errors.append("gl_uniforms.h must declare R_SetMVP and R_SetModelView")

# ------------------------------------------------------------ the file
if re.search(r"^\s*#\s*version\b", vert, re.M):
    errors.append("sworld_vert.glsl carries a #version; GLSL_VERT_HEADER supplies it per tier")
incs = re.findall(r'^\s*#\s*include\s+"([^"]+)"', vcode, re.M)
if incs != ["layout.inc", "uniforms.inc"] or '#include "layout.inc"\n#define UNIFORMS_VERT\n#include "uniforms.inc"' not in vert:
    errors.append(f"sworld_vert.glsl must include layout.inc, then uniforms.inc with UNIFORMS_VERT, and nothing else ({incs})")
if re.search(r"#\s*define\s+UNIFORMS_FRAG\b", vcode):
    errors.append("sworld_vert.glsl must not declare FragParams: a vertex stage reads only its own set")
if re.search(r"\buniform\b", vcode):
    errors.append("sworld_vert.glsl declares a uniform of its own; it reads VertParams only")
if re.search(r"\bbuffer\b|\bsampler\w*\b", vcode):
    errors.append("sworld_vert.glsl grew a buffer or sampler; the SPIR-V resource numbering assumes none")
if not re.search(r"^invariant gl_Position;\s*$", vcode, re.M):
    errors.append("sworld_vert.glsl lost `invariant gl_Position` (uhexen2-mf45: brush ents and world share depth)")
GOLDEN = ("void main ( ) { v_texcoord = a_texcoord ; v_lmcoord = a_lmcoord ; v_color = a_color ; "
          "v_worldxy = a_position . xy ; vec4 eyepos = u_modelview * vec4 ( a_position , 1.0 ) ; "
          "v_eyepos = eyepos . xyz ; v_fogdist = length ( eyepos . xyz ) ; "
          "gl_Position = u_mvp * vec4 ( a_position , 1.0 ) ; }")
TOK = r"\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+|\w+|[-+*/%<>=!&|^]=|&&|\|\||\+\+|--|<<|>>|\S"
mains = list(re.finditer(r"\bvoid\s+main\s*\(", vcode))
got = ""
if len(mains) == 1:
    mb = body_of(vcode, r"\bvoid\s+main\s*\(")
    got = " ".join(re.findall(TOK, vcode[mains[0].start():vcode.index("{", mains[0].start()) + len(mb)]))
if got != GOLDEN:
    errors.append(f"sworld_vert.glsl's main differs from the pinned body ({len(mains)} definitions); "
                  f"a deliberate change updates GOLDEN with it.\n    got:  {got}\n    want: {GOLDEN}")
# Everything outside main is declarations: no helper function a later edit
# could hide a block read in (the u_ scan above covers it, this keeps it simple).
if len(re.findall(r"\b\w+\s+\w+\s*\([^;]*\)\s*\{", vcode)) != 1:
    errors.append("sworld_vert.glsl defines a function besides main")

# ------------------------------------------------------------ inputs
attr = {k: int(v) for k, v in re.findall(r"#define\s+(ATTR_\w+)\s+(\d+)", shader_h)}
WANT_IN = {"a_position": ("ATTR_POSITION", "vec3"), "a_texcoord": ("ATTR_TEXCOORD", "vec2"),
           "a_lmcoord": ("ATTR_LMCOORD", "vec2"), "a_color": ("ATTR_COLOR", "vec4")}
WIDTH = {"float": 1, "vec2": 2, "vec3": 3, "vec4": 4}
decl_in = re.findall(r"^\s*(?:VERT_IN\((\d+)\)\s+)?in\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)\s*;", vcode, re.M)
if {n for _, _, n in decl_in} != set(WANT_IN) or len(decl_in) != len(WANT_IN):
    errors.append(f"sworld_vert.glsl inputs {[n for _, _, n in decl_in]}, expected {sorted(WANT_IN)}")
vbo = strip(read("engine/h2shared/gl_vbo.c"))
rsurf_raw = read("engine/hexen2/gl_rsurf.c")
rsurf = strip(rsurf_raw)
imm = {a: int(n) for a, n in re.findall(r"glVertexAttribFormat_fp\((ATTR_\w+), (\d+),", vbo)}
wvao = rsurf[rsurf.find("glGenVertexArrays_fp(1, &world_vao);"):]
wvao = wvao[:wvao.find("glGenBuffers_fp(1, &world_ibo);")]
world = {a: int(n) for a, n in re.findall(r"glVertexAttribPointer_fp\((ATTR_\w+), (\d+),", wvao)}
if len(imm) != 4 or len(world) != 3:
    errors.append(f"cannot read the imm ({imm}) and world ({world}) VAO formats")
for loc, typ, name in decl_in:
    if name not in WANT_IN:
        continue
    slot, want_t = WANT_IN[name]
    if not loc or int(loc) != attr.get(slot):
        errors.append(f"{name} must be VERT_IN({attr.get(slot)}) ({slot}); a layout location overrides "
                      "GL_LinkProgram's glBindAttribLocation, so a wrong one reads another attribute")
    if typ != want_t or imm.get(slot) != WIDTH.get(typ) or (slot in world and world[slot] != WIDTH.get(typ)):
        errors.append(f"{name} is {typ}; the imm VAO feeds {slot} with {imm.get(slot)} and the world VAO "
                      f"with {world.get(slot, 'a generic value')} components")

# ------------------------------------------------------------ varyings
def c_literal_text(symbol):
    m = re.search(rf"static const char {symbol}\[\] =\n(.*?)\";\n", gl_shader, re.S)
    if not m:
        return None
    out = []
    for t in re.finditer(r'"((?:[^"\\]|\\.)*)"|/\*.*?\*/|//[^\n]*|\b([A-Z][A-Z0-9_]{3,})\b',
                         m.group(1) + '"', re.S):
        lit = t.group(1)
        if lit is not None:
            out.append(lit.replace("\\n", "\n"))
    return "".join(out)


INTERP = {"flat", "smooth", "noperspective", "centroid", "invariant"}


def varyings(text, kw):
    found = {}
    for line in text.splitlines():
        line = re.sub(r"\b(VERT_OUT|FRAG_IN)\((\d+)\)", "", line).strip()
        m = re.match(rf"((?:\w+\s+)*?){kw}\s+((?:(?:highp|mediump|lowp)\s+)?)(\w+)\s+(\w+)\s*;", line)
        if m:
            quals = set(m.group(1).split())
            found[m.group(4)] = (m.group(3), frozenset(quals & INTERP), m.group(2).strip() or "default",
                                 frozenset(quals - INTERP))
    return found


vo = varyings(vcode, "out")
if not vo:
    errors.append("could not parse sworld_vert.glsl's outputs")
for q in {x for v in vo.values() for x in v[3]}:
    errors.append(f"sworld_vert.glsl output qualifier {q!r} is not understood by this check")
for frag in ("sworld_frag", "sworld_frag_opaque"):
    ft = c_literal_text(frag)
    if ft is None:
        if (ROOT / f"engine/shaders/{frag}.glsl").exists():
            errors.append(f"{frag} became a file; this check must read its FRAG_IN declarations there")
        else:
            errors.append(f"cannot find the {frag} literal")
        continue
    fi = varyings(ft, "in")
    fi.pop("a_position", None)
    if not fi:
        errors.append(f"could not parse {frag}'s inputs")
    elif {k: v[:3] for k, v in vo.items()} != {k: v[:3] for k, v in fi.items()}:
        errors.append(f"sworld_vert outputs {sorted((k, v[:3]) for k, v in vo.items())} != {frag} inputs "
                      f"{sorted((k, v[:3]) for k, v in fi.items())} (name, type, interpolation, precision)")
locs = [int(x) for x in re.findall(r"VERT_OUT\((\d+)\)", vcode)]
if sorted(locs) != list(range(len(vo))) or len(re.findall(r"\bout\b", vcode)) != len(locs):
    errors.append(f"every output needs its own VERT_OUT location, 0..{len(vo) - 1}; found {locs}")

# ------------------------------------------------------------ the C side's builds
if re.search(r"static const char sworld_vert\[\]", gl_shader):
    errors.append("the inline sworld_vert literal is back in gl_shader.c")
BUILDS = {
    "world": r'GL_InitProgramSplit\(&gl_shader_world, "world", GLSL_VERT_HEADER, sworld_vert,\s*NULL, sworld_frag\);',
    "world_opaque": r'GL_InitProgramSplit\(&gl_shader_world_opaque, "world_opaque", GLSL_VERT_HEADER, sworld_vert,\s*NULL, sworld_frag_opaque\);',
    "world_oit": r'GL_InitOITProgramSplit\(&gl_shader_world_oit, "world", GLSL_VERT_HEADER, sworld_vert,\s*NULL, sworld_frag\);',
}


def desktop_only(code, pos):
    stack = []
    for m in re.finditer(r"^\s*#\s*(ifndef|ifdef|if|else|elif|endif)\b(.*)$", code[:pos], re.M):
        d, rest = m.group(1), m.group(2).strip()
        if d in ("ifndef", "ifdef", "if"):
            stack.append([d == "ifndef" and rest == "USE_GLES"])
        elif d in ("else", "elif") and stack:
            stack[-1][0] = False
        elif d == "endif" and stack:
            stack.pop()
    return any(s[0] for s in stack)


gs_lit = strip(gl_shader)   # literals kept: the build calls quote names
init = body_of(gs_lit, r"\nvoid GL_Shaders_Init \(void\)\s*\{") or ""
init_at = gs_lit.find("{", gs_lit.find("\nvoid GL_Shaders_Init (void)"))
for name, b in BUILDS.items():
    found = list(re.finditer(b, init))
    if len(found) != 1:
        errors.append(f"{name}: expected one build in GL_Shaders_Init {b[:70]}..., found {len(found)}")
    elif name == "world_oit" and not desktop_only(gs_lit, init_at + found[0].start()):
        errors.append("world_oit is built outside #ifndef USE_GLES; the OIT programs are desktop-only")
    elif name != "world_oit" and desktop_only(gs_lit, init_at + found[0].start()):
        errors.append(f"{name} is built inside #ifndef USE_GLES; the ES tier needs it")
if len(re.findall(r"\bsworld_vert\b", gsc)) != 3:
    errors.append(f"sworld_vert is named {len(re.findall(r'sworld_vert', gsc))} times in gl_shader.c code, expected the 3 builds")
if not re.search(r"fs = frag_header \? GL_CompileOITFragShaderSplit \(frag_header, frag_src\)\s*"
                 r": GL_CompileOITFragShader \(frag_src\);", gsc):
    errors.append("GL_LoadOITProgramSplit must splice a NULL-header (literal) fragment the original way")
smoke = read("scripts/webgl-engine-shader-smoke.mjs")
for spec in ("['world', 'shader', 'sworld_vert', 'sworld_frag']",
             "['world_opaque', 'shader', 'sworld_vert', 'sworld_frag_opaque']"):
    if spec not in smoke:
        errors.append(f"the WebGL smoke no longer builds {spec}; the ES tier's only compile of this file")

gp = re.search(r"typedef struct glprogram_s \{(.*?)\} glprogram_t;", strip(shader_h), re.S)
gpf = set(re.findall(r"GLint\s+(\w+)", gp.group(1))) if gp else set()
if not gpf or {"u_mvp", "u_modelview"} & gpf:
    errors.append(f"glprogram_t still carries {sorted({'u_mvp', 'u_modelview'} & gpf)}; nothing declares them loose")
if "ub_vert" not in gpf:
    errors.append("glprogram_t lost ub_vert, which GL_ImmEnd keys the block path on")
gipu = body_of(gs_lit, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or ""
if "R_BindProgramBlocks(p->program, &p->ub_vert, &p->ub_frag);" not in gipu:
    errors.append("GL_InitProgramUniforms must bind the shared blocks (ub_vert), or the world programs read block 0")
C_FILES = [f for f in sorted((ROOT / "engine").rglob("*.c")) + sorted((ROOT / "engine").rglob("*.h"))
           if "rust" not in f.relative_to(ROOT).parts]
for f in C_FILES:
    raw = f.read_text(errors="replace")
    if re.search(r'"u_(?:mvp|modelview)"', strip(raw)):
        errors.append(f"{f.relative_to(ROOT)} looks up a loose u_mvp / u_modelview by name")
    if re.search(r"\bu_(?:mvp|modelview)\b", strip_c(raw)):
        errors.append(f"{f.relative_to(ROOT)} still spells u_mvp / u_modelview in C code")

# ------------------------------------------------------------ the draw paths
BLOCK_WRITERS = set(re.findall(r"\bvoid\s+(R_Set\w+)\s*\(", uh)) | {
    "R_FlushUniforms", "R_InvalidateUniforms", "GL_ImmEnd", "GL_ImmPushBlocks", "GL_ImmInvalidateState"}
if not {"R_SetMVP", "R_SetModelView", "R_SetFog", "R_SetAliasDlight"} <= BLOCK_WRITERS:
    errors.append("cannot read the block setters out of gl_uniforms.h")
DRAW = r"\bglDraw\w*_fp\b|^\s*EMIT_BATCH\("
WORLD_PROG = r"\bgl_shader_world(?:_opaque|_oit)?\b"

# function -> (file, mvp expr, mv expr, where the pair comes from, allowed enclosing heads)
PATHS = {
    "DrawTextureChains_FlushBatch": ("engine/hexen2/gl_rsurf.c", "world_state_mvp", "world_state_mv", "saved", []),
    "DrawTextureChains_DrawDeferred": ("engine/hexen2/gl_rsurf.c", "mvp", "mv", "local", [
        r"if \(e == &r_worldentity && lm_atlas_enabled && lm_atlas_texture && world_vao && world_ibo\)",
        r"if \(fence_n > 0 \|\| water_n > 0\)"]),
    "R_DrawBrushModel": ("engine/hexen2/gl_rsurf.c", "mvp", "mv", "local", [
        r"if \(world_vao && lm_atlas_enabled && lm_atlas_texture && world_ibo &&"]),
    "R_DispatchBrushInstancedPass": ("engine/hexen2/gl_rmain.c", r"world_instances\[inst\]\.mvp",
                                     r"world_instances\[inst\]\.mv", "instance", [r"while \(i < num_keys\)"]),
    "R_DrawWorldCulled": ("engine/h2shared/gl_worldcull.c", "mvp", "mv", "local", [r""]),
}


class Fn:
    def __init__(self, path, name):
        self.code = ""
        text = strip_c(read(path))
        for n, a, z in functions(text):
            if n == name:
                self.code = text[a:z]
                break

    def stmt_prefix(self, i):
        """Text between the statement delimiter before `i` and `i`, preprocessor lines dropped."""
        d, parens, j = self.code, 0, i - 1
        while j > 0:
            ch = d[j]
            if ch == ")":
                parens += 1
            elif ch == "(":
                parens -= 1
            elif ch in ";{}" and parens == 0:
                break
            j -= 1
        return " ".join(re.sub(r"(?m)^\s*#.*$", " ", d[j + 1:i]).split())

    def heads(self, i):
        """The statement heads of every block enclosing `i`, outermost first.
        A plain scoping block has an empty head and is left out."""
        out, depth, j = [], 0, i - 1
        while j > 0:
            if self.code[j] == "}":
                depth += 1
            elif self.code[j] == "{":
                if depth == 0:
                    h = self.stmt_prefix(j)
                    if h:
                        out.append(h)
                else:
                    depth -= 1
            j -= 1
        return out[::-1]

    def in_pp_conditional(self, i):
        depth = 0
        for m in re.finditer(r"(?m)^\s*#\s*(if|ifdef|ifndef|endif)\b", self.code[:i]):
            depth += -1 if m.group(1) == "endif" else 1
        return depth != 0


def check_unconditional(fn, label, at, allowed):
    pre = fn.stmt_prefix(at)
    if pre:
        errors.append(f"{label} is governed by {pre!r}")
    hs = fn.heads(at)
    if len(hs) != len([a for a in allowed if a]) or not all(re.match(a, h) for a, h in zip([a for a in allowed if a], hs)):
        errors.append(f"{label} sits under {hs}; only {[a for a in allowed if a]} may enclose it")
    if fn.in_pp_conditional(at):
        errors.append(f"{label} sits inside a preprocessor conditional")


for name, (path, mvp_e, mv_e, source, allowed) in PATHS.items():
    fn = Fn(path, name)
    d = fn.code
    if not d:
        errors.append(f"{path}: no function {name}")
        continue
    draws = [m.start() for m in re.finditer(DRAW, d, re.M)]
    sets = {w: [m.start() for m in re.finditer(rf"\b{w} \({e}\);", d)]
            for w, e in (("R_SetMVP", mvp_e), ("R_SetModelView", mv_e))}
    flushes = [m.start() for m in re.finditer(r"\bR_FlushUniforms \(\);", d)]
    if not draws:
        errors.append(f"{name}: cannot find its draws")
        continue
    bad = [w for w, p in sets.items() if len(p) != 1] + (["R_FlushUniforms"] if len(flushes) != 1 else [])
    if bad:
        errors.append(f"{name} must call R_SetMVP ({mvp_e}), R_SetModelView ({mv_e}) and R_FlushUniforms () "
                      f"exactly once each before its draws (missing or repeated: {bad})")
        continue
    fpos = flushes[0]
    for w, (p,) in sets.items():
        if p > fpos:
            errors.append(f"{name} calls {w} after its flush")
        check_unconditional(fn, f"{name}'s {w}", p, allowed)
    check_unconditional(fn, f"{name}'s R_FlushUniforms", fpos, allowed)
    if draws[0] < fpos:
        errors.append(f"{name} draws before it flushes VertParams")
    # From the flush to each draw nothing may write a block: the draw would
    # read whatever that writer left.  For a draw in a loop the stretch is
    # the text between them, which is every path through the loop body.
    for dp in draws:
        if dp < fpos:
            continue
        for w in sorted(BLOCK_WRITERS - {"R_FlushUniforms"}):
            if re.search(rf"\b{w}\b", d[fpos:dp]):
                errors.append(f"{name} calls {w} between its VertParams flush and a draw")
    for w in sorted(BLOCK_WRITERS - {"R_SetMVP", "R_SetModelView", "R_FlushUniforms"}):
        hit = re.search(rf"\b{w}\b", d[min(sets["R_SetMVP"][0], sets["R_SetModelView"][0]):fpos])
        if hit:
            errors.append(f"{name} calls {w} between its matrix setters and its flush")
    if re.search(r"\bglUniformMatrix4fv_fp\b", d):
        errors.append(f"{name} still uploads a loose matrix")
    if source == "local":
        for var, getter in ((mvp_e, "GL_GetMVP"), (mv_e, "GL_GetModelview")):
            occ = list(re.finditer(rf"\b{var}\b", d))
            shapes = [lambda o: d.startswith(f"{var}[16]", o),
                      lambda o: d.endswith(f"{getter}(", 0, o) and d.startswith(f"{var})", o),
                      lambda o: (d.endswith("R_SetMVP (", 0, o) or d.endswith("R_SetModelView (", 0, o))
                      and d.startswith(f"{var})", o)]
            # R_DrawWorldCulled also saves the frame's mvp for the next
            # frame's Hi-Z reprojection; that read is not a second writer.
            rest = [o for o in occ[3:] if not (name == "R_DrawWorldCulled" and var == "mvp"
                                                and re.match(r"mvp, sizeof\(hiz_prev_mvp\)", d[o.start():]))]
            if len(occ) < 3 or not all(f(o.start()) for f, o in zip(shapes, occ)) or rest:
                errors.append(f"{name} must declare {var}[16], fill it with {getter}({var}) and hand it to its "
                              f"setter, and do nothing else with it ({len(occ)} mentions)")
                continue
            check_unconditional(fn, f"{name}'s {getter}", d.rfind(getter, 0, occ[1].start()), allowed)
            if occ[1].start() > sets["R_SetMVP"][0] or occ[1].start() > sets["R_SetModelView"][0]:
                errors.append(f"{name} fills {var} after handing it to its setter")

# The saved pair: filled in one place, by the bind the fast path runs first.
bws = Fn("engine/hexen2/gl_rsurf.c", "DrawTextureChains_BindWorldState")
for var, getter in (("world_state_mvp", "GL_GetMVP"), ("world_state_mv", "GL_GetModelview")):
    m = re.search(rf"\b{getter}\({var}\);", bws.code)
    if not m:
        errors.append(f"DrawTextureChains_BindWorldState must fill {var} with {getter}")
    else:
        check_unconditional(bws, f"DrawTextureChains_BindWorldState's {getter}", m.start(), [])
    rcode = strip_c(rsurf_raw)
    if len(re.findall(rf"\b{var}\b", rcode)) != 3 or not re.search(rf"\bstatic float\s+{var}\[16\];", rcode):
        errors.append(f"{var} must be a file-static float[16] filled only by BindWorldState and read only by FlushBatch")
chains = Fn("engine/hexen2/gl_rsurf.c", "DrawTextureChains")
fb_calls = [m.start() for m in re.finditer(r"\bDrawTextureChains_FlushBatch\b", chains.code)]
bind_calls = [m.start() for m in re.finditer(r"\bDrawTextureChains_BindWorldState\s*\(\);", chains.code)]
if len(re.findall(r"\bDrawTextureChains_FlushBatch\b", strip_c(rsurf_raw))) != len(fb_calls) + 1 or not fb_calls:
    errors.append("DrawTextureChains_FlushBatch must be called only from DrawTextureChains")
elif not bind_calls or min(bind_calls) > min(fb_calls):
    errors.append("DrawTextureChains must run DrawTextureChains_BindWorldState before its first FlushBatch")

# The instanced pass draws with whatever its caller bound, which must be a
# world program, and nobody else calls it: each of the two calls sits in a
# block that binds prog_opaque / prog_cutout, which are the world programs.
rmain = strip_c(read("engine/hexen2/gl_rmain.c"))
inst = Fn("engine/hexen2/gl_rmain.c", "R_DrawBrushInstanced").code
blocks = re.findall(r"\{\s*R_UseProgram \((prog_opaque|prog_cutout)->program\);([^{}]*)\}", inst)
bound = sorted(p for p, b in blocks if re.search(r"\bR_DispatchBrushInstancedPass\s*\(", b))
if len(re.findall(r"\bR_DispatchBrushInstancedPass\s*\(", rmain)) != 3 or bound != ["prog_cutout", "prog_opaque"] \
        or not re.search(r"glprogram_t \*prog_opaque = &gl_shader_world_opaque;", inst) \
        or not re.search(r"glprogram_t \*prog_cutout = &gl_shader_world;", inst):
    errors.append("R_DispatchBrushInstancedPass must be called twice, from R_DrawBrushInstanced, each in the "
                  "block that binds its world_opaque / world program")

# GL_ImmEnd: the block half is keyed on the program having a block, which the
# world programs do now, and writes both matrices before its one flush.
push = Fn("engine/h2shared/gl_vbo.c", "GL_ImmPushBlocks")
pc = push.code
for stmt, allowed in ((r"R_SetMVP \(mvp\);", [r"if \(shader->ub_vert >= 0\)"]),
                      (r"GL_GetModelview\(mv\);", [r"if \(shader->ub_vert >= 0\)"]),
                      (r"R_SetModelView \(mv\);", [r"if \(shader->ub_vert >= 0\)"]),
                      (r"R_FlushUniforms \(\);", [])):
    ms = list(re.finditer(rf"\b{stmt}", pc))
    if len(ms) != 1:
        errors.append(f"GL_ImmPushBlocks must run `{stmt}` once ({len(ms)})")
        continue
    check_unconditional(push, f"GL_ImmPushBlocks's {stmt}", ms[0].start(), allowed)
    if stmt != r"R_FlushUniforms \(\);" and re.search(r"\bR_FlushUniforms \(\);", pc) \
            and ms[0].start() > re.search(r"\bR_FlushUniforms \(\);", pc).start():
        errors.append(f"GL_ImmPushBlocks runs `{stmt}` after its flush")
if not re.search(r"\bR_FlushUniforms \(\);\s*\}$", pc):
    errors.append("GL_ImmPushBlocks must end with its flush")
ie = Fn("engine/h2shared/gl_vbo.c", "GL_ImmEnd")
ic = ie.code
g = re.search(r"\bGL_GetMVP\(mvp\);", ic)
p = re.search(r"\bif \(shader->ub_vert >= 0 \|\| shader->ub_frag >= 0\)\s*GL_ImmPushBlocks\(shader, mvp\);", ic)
dr = [m.start() for m in re.finditer(DRAW, ic, re.M)]
if not g or not p or not dr or not (g.start() < p.start() < min(dr)):
    errors.append("GL_ImmEnd must fill mvp, then push the blocks for any program that has one, then draw")
else:
    check_unconditional(ie, "GL_ImmEnd's GL_GetMVP", g.start(), [])
    check_unconditional(ie, "GL_ImmEnd's block push", p.start(), [])
    for w in sorted(BLOCK_WRITERS - {"GL_ImmPushBlocks"}):
        if re.search(rf"\b{w}\b", ic[p.end():max(dr)]):
            errors.append(f"GL_ImmEnd calls {w} between its block push and its draw")

# Completeness: every function that names a world program and draws is a
# pinned path, so a new one cannot draw with whatever VertParams last held.
for f in C_FILES:
    if f.suffix != ".c":
        continue
    text = strip_c(f.read_text(errors="replace"))
    for n, a, z in functions(text):
        body = text[a:z]
        if re.search(WORLD_PROG, body) and re.search(DRAW, body, re.M) and n not in PATHS:
            errors.append(f"{f.relative_to(ROOT)}:{n} names a world program and draws, but is not a pinned "
                          "path; write VertParams there and add it to PATHS")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS sworld_vert reads VertParams; mirror, file, inputs, varyings, builds and {len(PATHS) + 1} "
      "draw paths agree (#450 W1)")
