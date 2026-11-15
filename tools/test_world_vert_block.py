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
  * the flow pass (end of file) checks the same three things by following
    values and calls through every function the h2 client builds, rather
    than by spelling: no function draws while a world program may be bound
    -- handed in through a parameter, a local, a pointer or a return value,
    or left bound by a caller -- unless it is a pinned path or GL_ImmEnd;
    no block writer is reached through any chain of calls between a path's
    flush and its draw; and DrawTextureChains reaches FlushBatch only with
    world_state_set true on every path, FlushBatch refuses to draw without
    it, and nothing in between can refill the pair it draws with.
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
    for mm in re.finditer(r"[{};]", code):
        ch, i = mm.group(0), mm.start()
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

# ==================================================================== the flow pass
# The checks above read each pinned path by its spelling.  This pass follows
# the values and the calls instead, over every function the h2 client builds
# (engine/h2shared + engine/hexen2, their own and their headers' macros
# expanded), so a world program handed through a parameter, a local, a
# pointer or a return value, or left bound for a callee, still counts:
#   * a function that draws while a world program may be bound must be a
#     pinned path (PATHS, or GL_ImmEnd, which writes the block itself);
#   * a block writer reached through any chain of engine calls between a
#     path's flush and its draw counts as a writer;
#   * DrawTextureChains reaches FlushBatch only with world_state_set true,
#     which only its own BindWorldState sets, so the saved pair is this call's.
# What it cannot model -- a goto it cannot follow, a value stored where it
# cannot follow it, a body it cannot parse -- is an error, never a pass.
# Three choices are modelled rather than proven: a call through a pointer may
# reach any address-taken function with as many parameters; a FUNC_NORETURN
# call ends the path; and a nested frame (a console print repainting the
# screen) is checked from its own entry instead of being charged to the
# print's caller (FRAME_ROOTS).
SCRUB = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'", re.S)


def scrub(t):
    """Comments blanked, string and char literals emptied.  A newline inside a
    comment becomes \\v\\n: a comment does not end a #define, and \\v is
    whitespace to everything else here."""
    def rep(m):
        s = m.group(0)
        if s[0] == '"':
            return '""'
        if s[0] == "'":
            return "' '"
        return "\v\n".join(" " * len(x) for x in s.split("\n"))
    return SCRUB.sub(rep, t)


class Unsupported(Exception):
    pass


CKEYS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else", "case", "default", "goto",
         "break", "continue", "defined", "__attribute__", "typeof", "__typeof__", "_Alignof", "void",
         "char", "short", "int", "long", "float", "double", "signed", "unsigned", "const", "volatile",
         "struct", "union", "enum", "static", "extern", "register", "inline", "__inline", "__inline__"}
BINDS = {"R_UseProgram", "glUseProgram_fp"}
BINDS_RE = r"\b(?:R_UseProgram|glUseProgram_fp)\b"
GLDRAW = r"\b(?:glDraw(?!Buffers?_)\w*_fp|glMultiDraw\w*_fp)\b"
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
PROTO = re.compile(r"\s*(?:(?:extern|static|const|struct|unsigned|signed)\s+)*\w+(?:\s+|\s*\*+\s*)\**\s*\w+\s*\([^=]*\)\s*")
EMPTY = frozenset()


def close_of(s, i):
    """Index just past the bracket matching s[i]."""
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return j + 1
    raise Unsupported("unbalanced brackets")


def split_top(s, sep=","):
    out, depth, last = [], 0, 0
    for j, c in enumerate(s):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(s[last:j])
            last = j + 1
    out.append(s[last:])
    return out


def calls_in(text):
    """(name, args, start, through_member) per call in text, innermost first."""
    out = []
    for m in CALL.finditer(text):
        name = m.group(1)
        if name in CKEYS:
            continue
        b = m.start() - 1
        while b >= 0 and text[b] in " \t\n\v":
            b -= 1
        mem = b >= 0 and (text[b] == "." or (text[b] == ">" and b > 0 and text[b - 1] == "-"))
        op = m.end() - 1
        try:
            z = close_of(text, op)
        except Unsupported:
            continue
        inner = text[op + 1:z - 1]
        out.append((z, name, split_top(inner) if inner.strip() else [], m.start(), mem))
    out.sort()
    return [(n, a, s, mem) for _, n, a, s, mem in out]


class Parser:
    """Statement-level C parser over one scrubbed, macro-expanded body."""

    def __init__(self, s):
        self.s = s

    def ws(self, i):
        s = self.s
        while i < len(s) and s[i].isspace():
            i += 1
        return i

    def word(self, i):
        m = re.compile(r"[A-Za-z_]\w*").match(self.s, i)
        return m.group(0) if m else None

    def directive(self, i):
        e = self.s.find("\n", i)
        e = len(self.s) if e < 0 else e
        m = re.match(r"#\s*(\w+)", self.s[i:e])
        return (m.group(1) if m else ""), e

    def block(self, i):
        if self.s[i] != "{":
            raise Unsupported("expected a block")
        items, i = self.stmts(i + 1, "}")
        return ("seq", items), i + 1

    def stmts(self, i, end):
        items = []
        while True:
            i = self.ws(i)
            if i >= len(self.s):
                raise Unsupported("ran off the end")
            if end == "}" and self.s[i] == "}":
                return items, i
            if self.s[i] == "#" and self.directive(i)[0] in ("elif", "else", "endif"):
                if end == "#":
                    return items, i
                raise Unsupported("#if splits a block")
            node, i = self.stmt(i)
            items.append(node)

    def choice(self, i):
        alts, seen_else = [], False
        i = self.directive(i)[1]
        while True:
            items, i = self.stmts(i, "#")
            alts.append(("seq", items))
            kind, i = self.directive(i)
            seen_else = seen_else or kind == "else"
            if kind == "endif":
                break
        if not seen_else:
            alts.append(("seq", []))
        return ("choice", alts), i

    def paren(self, i, what):
        i = self.ws(i)
        if self.s[i] != "(":
            raise Unsupported(what + " without (")
        z = close_of(self.s, i)
        return self.s[i + 1:z - 1], i + 1, z

    def stmt(self, i):
        s = self.s
        i = self.ws(i)
        if s[i] == "{":
            return self.block(i)
        if s[i] == ";":
            return ("seq", []), i + 1
        if s[i] == "#":
            kind, e = self.directive(i)
            if kind in ("if", "ifdef", "ifndef"):
                return self.choice(i)
            if kind in ("elif", "else", "endif"):
                raise Unsupported("stray #" + kind)
            return ("seq", []), e
        w = self.word(i)
        if w == "if":
            cond, cpos, k = self.paren(i + 2, "if")
            then, k = self.stmt(k)
            k2 = self.ws(k)
            els = None
            if self.word(k2) == "else":
                els, k = self.stmt(k2 + 4)
            return ("if", cond, cpos, then, els), k
        if w == "for":
            hdr, hpos, k = self.paren(i + 3, "for")
            parts = split_top(hdr, ";")
            if len(parts) != 3:
                raise Unsupported("for header")
            body, k = self.stmt(k)
            offs = [hpos, hpos + len(parts[0]) + 1, hpos + len(parts[0]) + len(parts[1]) + 2]
            return ("for", parts, offs, body), k
        if w == "while":
            cond, cpos, k = self.paren(i + 5, "while")
            body, k = self.stmt(k)
            return ("while", cond, cpos, body), k
        if w == "do":
            body, k = self.stmt(i + 2)
            k = self.ws(k)
            if self.word(k) != "while":
                raise Unsupported("do without while")
            cond, cpos, k = self.paren(k + 5, "do-while")
            k = self.ws(k)
            if s[k] != ";":
                raise Unsupported("do-while without ;")
            return ("do", body, cond, cpos), k + 1
        if w == "switch":
            cond, cpos, k = self.paren(i + 6, "switch")
            body, k = self.stmt(k)
            return ("switch", cond, cpos, body), k
        if w == "case" or (w == "default" and s[self.ws(i + 7)] == ":"):
            depth, k = 0, i
            while s[k] != ":" or depth:
                depth += {"(": 1, "[": 1, ")": -1, "]": -1}.get(s[k], 0)
                k += 1
            return ("case",), k + 1
        if w in ("break", "continue"):
            k = self.ws(i + len(w))
            if s[k] != ";":
                raise Unsupported(w)
            return (w,), k + 1
        if w == "goto":
            m = re.compile(r"goto\s+(\w+)\s*;").match(s, i)
            if not m:
                raise Unsupported("computed goto")
            return ("goto", m.group(1)), m.end()
        if w == "else":
            raise Unsupported("stray else")
        if w and w not in CKEYS:
            k = self.ws(i + len(w))
            if k < len(s) and s[k] == ":" and s[k + 1:k + 2] != ":":
                return ("label", w), k + 1
        return self.simple(i, w == "return")

    def simple(self, i, is_return):
        s, k = self.s, i
        while True:
            if k >= len(s):
                raise Unsupported("statement runs off the end")
            ch = s[k]
            if ch in "([":
                k = close_of(s, k)
                continue
            if ch == "{":
                before = s[i:k].rstrip()
                if not before or before.endswith(("=", ",", "(", "return")) or \
                        re.search(r"\b(?:struct|union|enum)(?:\s+\w+)?$", before):
                    k = close_of(s, k)
                    continue
                # An unknown macro head with a body: a loop that may run 0..n times.
                body, z = self.block(k)
                return ("macro", s[i:k], i, body), z
            if ch == "}":
                raise Unsupported("statement without ;")
            if ch == "#" and s[:k].rstrip(" \t\v").endswith("\n"):
                kind, e = self.directive(k)
                if kind in ("if", "ifdef", "ifndef", "elif", "else", "endif"):
                    raise Unsupported("#" + kind + " inside a statement")
                k = e
                continue
            if ch == ";":
                if is_return:
                    return ("return", s[i + 6:k], i + 6), k + 1
                return ("simple", s[i:k], i), k + 1
            k += 1


def join(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return a | b


class Interp:
    """Abstract interpretation of a parsed body over a set-valued domain:
    dom.simple(text, pos, st) -> st, dom.cond(text, pos, st) -> (true, false).
    None is unreachable.  Loops run to a fixed point; break, continue,
    return, case labels and goto go where C sends them."""

    def __init__(self, dom):
        self.dom = dom

    def run(self, node, st):
        self.labels_in = {}
        for _ in range(20):
            self.loops, self.breaks, self.switch_in, self.ret, self.labels = [], [], [], None, {}
            out = self.ex(node, st)
            if self.labels == self.labels_in:
                return join(out, self.ret)
            self.labels_in = self.labels
        raise Unsupported("goto did not converge")

    def ex(self, n, st):
        k, d = n[0], self.dom
        if k == "seq":
            for c in n[1]:
                if c[0] == "case":
                    st = join(st, self.switch_in[-1] if self.switch_in else None)
                elif c[0] == "label":
                    st = join(st, self.labels_in.get(c[1]))
                else:
                    st = self.ex(c, st)
            return st
        if k == "choice":
            out = None
            for alt in n[1]:
                out = join(out, self.ex(alt, st))
            return out
        if k in ("case", "label"):
            return self.ex(("seq", [n]), st)
        if st is None:
            return None
        if k == "simple":
            return d.simple(n[1], n[2], st)
        if k == "return":
            self.ret = join(self.ret, d.simple(n[1], n[2], st))
            return None
        if k == "goto":
            self.labels[n[1]] = join(self.labels.get(n[1]), st)
            return None
        if k == "if":
            t, f = d.cond(n[1], n[2], st)
            a = self.ex(n[3], t) if t is not None else None
            if n[4] is None:
                return join(a, f)
            return join(a, self.ex(n[4], f) if f is not None else None)
        if k in ("break", "continue"):
            if not self.breaks or (k == "continue" and not self.loops):
                raise Unsupported(k + " outside a loop")
            self.breaks[-1 if k == "break" else self.loops[-1]][k].append(st)
            return None
        if k == "switch":
            st = d.simple(n[1], n[2], st)
            frame = {"break": [], "continue": []}
            self.breaks.append(frame)
            self.switch_in.append(st)
            out = self.ex(n[3], None)
            self.switch_in.pop()
            self.breaks.pop()
            out = join(out, st)    # no case taken; harmless next to a default
            for b in frame["break"]:
                out = join(out, b)
            return out
        return self.loop(n, st)

    def loop(self, n, st):
        d, k, incr = self.dom, n[0], None
        if k == "for":
            (init, cond, incr), (pi, pc, pn), body = n[1], n[2], n[3]
            st = d.simple(init, pi, st)
        elif k in ("while", "macro"):
            cond, pc, body = n[1], n[2], n[3]
        else:
            body, cond, pc = n[1], n[2], n[3]
        head = st
        for _ in range(50):
            frame = {"break": [], "continue": []}
            self.breaks.append(frame)
            self.loops.append(len(self.breaks) - 1)
            if k == "do":
                out = self.ex(body, head)
                for c in frame["continue"]:
                    out = join(out, c)
                back, f = d.cond(cond, pc, out) if out is not None else (None, None)
            else:
                t, f = d.cond(cond, pc, head) if cond.strip() else (head, None)
                if k == "macro":
                    f = t
                out = self.ex(body, t) if t is not None else None
                for c in frame["continue"]:
                    out = join(out, c)
                if incr is not None and out is not None:
                    out = d.simple(incr, pn, out)
                back = out
            self.loops.pop()
            self.breaks.pop()
            nh = join(st, back)
            if nh == head:
                for b in frame["break"]:
                    f = join(f, b)
                return f
            head = nh
        raise Unsupported("loop did not converge")


class Func:
    def __init__(self, name, params, file, text, a, z):
        self.name, self.file, self.text, self.a = name, file, text, a
        self.body, self.params = text[a:z], params
        self.ast = self.err = None

    def where(self):
        return f"{self.file}:{self.text.count(chr(10), 0, self.a) + 1} {self.name}"


def param_names(ptext):
    if ptext.strip() in ("", "void"):
        return []
    out = []
    for p in split_top(ptext):
        p = p.strip()
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", p) or re.search(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", p)
        out.append(m.group(1) if m and p != "..." else None)
    return out


IDENT = re.compile(r"[A-Za-z_]\w*")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*[\\\v]\n)*[^\n]*", re.M)


MACRO_CALL = re.compile(r"\b(?!sizeof\b|defined\b)[A-Za-z_]\w*\s*\(")


def macro_defs(text, where, into):
    """A text's #defines that can call or run statements into `into` (the
    first wins: an #if's alternatives share one call surface).  A macro that
    only computes cannot bind, draw or call, and is left as written; any
    other macro that binds, draws or names a world program is an error."""
    for m in DEFINE.finditer(text):
        d = re.sub(r"[\\\v]\n", " ", m.group(0))
        fm = re.match(r"\s*#\s*define\s+(\w+)\(([^)]*)\)(.*)", d, re.S)
        om = re.match(r"\s*#\s*define\s+(\w+)\s(.*)", d, re.S)
        if fm and not re.fullmatch(r"gl\w+_fp", fm.group(1)) and \
                (";" in fm.group(3) or MACRO_CALL.search(fm.group(3))):
            ps = [p.strip() for p in fm.group(2).split(",") if p.strip()]
            if ps and ps[-1].endswith("..."):
                ps[-1] = ps[-1][:-3].strip() or "__VA_ARGS__"
                ps.append("...")
            into.setdefault(fm.group(1), (ps, fm.group(3)))
        elif not fm and om and ";" in om.group(2):
            into.setdefault(om.group(1), (None, om.group(2)))
        elif re.search(BINDS_RE + "|" + GLDRAW + "|" + WORLD_PROG, d) and \
                not re.match(r"\s*#\s*define\s+gl\w+_fp\b", d):
            errors.append(f"{where}: macro {d.split()[1]} binds, draws or names a world program "
                          "in a form the flow pass does not expand")


def expand(body, macros, rx):
    """Macros expanded in place, nested ones included."""
    if not macros.keys() & set(IDENT.findall(body)):
        return body
    for _ in range(8):
        out, pos, hit = [], 0, False
        for m in rx.finditer(body):
            if m.start() < pos:
                continue
            params, rep = macros[m.group(1)]
            if params is None:
                out += [body[pos:m.start()], " " + rep + " "]
                pos, hit = m.end(), True
                continue
            if not m.group(2):
                continue
            try:
                z = close_of(body, m.end() - 1)
            except Unsupported:
                continue
            inner = body[m.end():z - 1]
            args = [x.strip() for x in split_top(inner)] if inner.strip() else []
            names = params
            if params and params[-1] == "...":
                names = params[:-1]
                args = args[:len(names) - 1] + [", ".join(args[len(names) - 1:])]
            r = re.sub(r"\s*##\s*", "", re.sub(r"(?<!#)#\s*\w+", '""', rep))
            if names:
                tbl = dict(zip(names, args))
                r = re.sub(r"\b(" + "|".join(map(re.escape, names)) + r")\b", lambda w: tbl.get(w.group(1), ""), r)
            out += [body[pos:m.start()], " " + r + " "]
            pos, hit = z, True
        body = "".join(out + [body[pos:]])
        if not hit:
            return body
    raise Unsupported("macro expansion did not terminate")


SRC_DIRS = [ROOT / "engine/h2shared", ROOT / "engine/hexen2"]
HEADER_MACROS, NORETURN = {}, set()
for f in [h for d in SRC_DIRS for h in sorted(d.glob("*.h"))]:
    ht = scrub(f.read_text(errors="replace"))
    macro_defs(ht, str(f.relative_to(ROOT)), HEADER_MACROS)
    NORETURN |= set(re.findall(r"FUNC_NORETURN\s+\w+\s+\**\s*(\w+)\s*\(", ht))
if not {"Sys_Error", "Host_Error"} <= NORETURN:
    errors.append("cannot read FUNC_NORETURN declarations for Sys_Error / Host_Error")
INDEX, ALL, FPVARS, NAMED, MRX = {}, [], set(), set(), {}
for f in [c for d in SRC_DIRS for c in sorted(d.glob("*.c"))]:
    rel = str(f.relative_to(ROOT))
    text = scrub(f.read_text(errors="replace"))
    macros = {}
    macro_defs(text, rel, macros)
    for k, v in HEADER_MACROS.items():
        macros.setdefault(k, v)
    names = frozenset(macros)
    if names not in MRX:
        MRX[names] = re.compile(r"\b(" + "|".join(map(re.escape, sorted(names, key=len, reverse=True))) + r")\b(\s*\()?")
    mrx = MRX[names]
    text = DEFINE.sub(lambda m: "\n".join(" " * len(x) for x in m.group(0).split("\n")), text)
    FPVARS |= set(re.findall(r"\(\s*\*\s*(\w+)\s*\)\s*\(", text))
    NAMED |= set(re.findall(r"\b([A-Za-z_]\w*)\b(?!\s*\()", text))
    depth, start, last = 0, 0, 0
    for m in re.finditer(r"[{};]", text):
        ch, i = m.group(0), m.start()
        if ch == "{":
            start = i if depth == 0 else start
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                h = re.search(r"\b(\w+)\s*\(([^;{}]*)\)\s*$", text[last:start])
                if h and h.group(1) not in CKEYS:
                    fn = Func(h.group(1), param_names(h.group(2)), rel, text, start, i + 1)
                    try:
                        fn.body = expand(fn.body, macros, mrx)
                    except Unsupported as e:
                        fn.err = str(e)
                    INDEX.setdefault(fn.name, []).append(fn)
                    ALL.append(fn)
                elif re.search(WORLD_PROG + "|" + GLDRAW + "|" + BINDS_RE, text[start:i + 1]) or \
                        re.search(WORLD_PROG + "|" + GLDRAW + "|" + BINDS_RE, expand(text[start:i + 1], macros, mrx)):
                    errors.append(f"{rel}:{text.count(chr(10), 0, start) + 1}: a file-scope block names a world "
                                  "program, binds or draws, and is not a function the flow pass can follow")
                last = i + 1
        elif depth == 0:
            last = i + 1
for need in list(PATHS) + ["GL_ImmEnd", "DrawTextureChains", "DrawTextureChains_BindWorldState", "SCR_UpdateScreen"]:
    if need not in INDEX:
        errors.append(f"the flow pass did not index {need}")


def defs_of(name, caller):
    defs = INDEX.get(name, [])
    return [d for d in defs if d.file == caller.file] or defs


# A call through a pointer reaches some function whose address is taken
# somewhere (named other than in a call), with as many parameters.
ADDR = {}
for n in NAMED & set(INDEX):
    for g in INDEX[n]:
        ADDR.setdefault(len(g.params), []).append(g)
VARIADIC = [g for gs in ADDR.values() for g in gs if g.params and g.params[-1] is None]
# A console print may repaint the screen (CON_Printf -> SCR_UpdateScreen while
# loading).  That nested frame is checked like every frame, from an entry that
# may hold anything; what it leaves bound is not charged to the print's caller,
# or every function that can print would inherit the whole renderer.
FRAME_ROOTS = {"SCR_UpdateScreen"}


def targets(name, nargs, mem, fn):
    if mem or (name in FPVARS and name not in INDEX and not re.fullmatch(r"gl\w+_fp", name)):
        return ADDR.get(nargs, []) + VARIADIC
    return defs_of(name, fn)


for fn in ALL:
    fn.calls = calls_in(fn.body)
    fn.binds = any(n in BINDS and not mem for n, _, _, mem in fn.calls)
    fn.draws = any(re.fullmatch(GLDRAW, n) and not mem for n, _, _, mem in fn.calls)
    fn.out = {id(g): g for n, a, _, mem in fn.calls if n not in BINDS for g in targets(n, len(a), mem, fn)}
CALLERS = {}
for fn in ALL:
    for g in fn.out.values():
        CALLERS.setdefault(id(g), []).append(fn)


def reaching(seed, cut=None):
    """Every function that reaches a seed through the call graph, seeds
    included.  Not through a call that never returns (NORETURN), nor through
    a nested frame (FRAME_ROOTS)."""
    cut = NORETURN | FRAME_ROOTS if cut is None else cut
    seen, work = {id(f): f for f in seed}, list(seed)
    seeds = set(seen)
    while work:
        g = work.pop()
        if g.name in cut and id(g) not in seeds:
            continue
        for c in CALLERS.get(id(g), []):
            if id(c) not in seen:
                seen[id(c)] = c
                work.append(c)
    return seen


def reached(fn, cut=None):
    """Every function fn reaches, with reaching()'s cuts."""
    cut = NORETURN | FRAME_ROOTS if cut is None else cut
    seen, work = {}, [fn]
    while work:
        for g in work.pop().out.values():
            if id(g) not in seen and g.name not in cut:
                seen[id(g)] = g
                work.append(g)
    return seen


def pp_variant(body, pick):
    """The body with every #if group resolved to its first or its last branch
    (a group without #else has an empty last), other lines blanked."""
    lines, groups, stack, info = body.split("\n"), [], [], {}
    for n, ln in enumerate(lines):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", ln)
        if m:
            k = info[n] = m.group(1)
            if k.startswith("if"):
                groups.append([1, False])
                stack.append(groups[-1])
            elif stack and k == "endif":
                stack.pop()
            elif stack:
                stack[-1][0] += 1
                stack[-1][1] = stack[-1][1] or k == "else"
    out, stack, gi = [], [], 0
    for n, ln in enumerate(lines):
        k = info.get(n)
        if k:
            if k.startswith("if"):
                stack.append([groups[gi], 0])
                gi += 1
            elif stack and k == "endif":
                stack.pop()
            elif stack:
                stack[-1][1] += 1
            out.append("")
            continue
        live = all(br == (0 if pick == "first" else (g[0] - 1 if g[1] else -1)) for g, br in stack)
        out.append(ln if live else "")
    return "\n".join(out)


REL = reaching([fn for fn in ALL if fn.binds or fn.draws])
for fn in REL.values():
    if fn.err:
        continue
    try:
        fn.ast = Parser(fn.body).block(0)[0]
    except (Unsupported, IndexError) as e:
        if not re.search(r"(?m)^\s*#\s*if", fn.body):
            fn.err = str(e)
            continue
        try:
            fn.ast = ("choice", [Parser(pp_variant(fn.body, p)).block(0)[0] for p in ("first", "last")])
        except (Unsupported, IndexError) as e2:
            fn.err = f"{e}; per #if branch: {e2}"

# ------------------------------------------------------------ world-program values
# A value's atoms: "W" may be a world program (or its ->program handle),
# ("P", i) is whatever parameter i holds.  For the bound program, "E" is
# whatever was bound when the function was entered.
WORLD_NAMES = {"gl_shader_world", "gl_shader_world_opaque", "gl_shader_world_oit"}
ASSIGN = re.compile(r"(?<![=!<>+\-*/%&|^])=(?!=)")
DECL_HEAD = re.compile(r"\s*(?:(?:const|static|volatile|register|struct|unsigned|signed|extern)\s+)*"
                       r"(?!return\b|else\b|case\b|goto\b|sizeof\b)\w+\s*[\s*]\s*")


def chunks(body):
    body = re.sub(r"(?m)^[ \t]*#[^\n]*", " ", body)
    out, depth, last = [], 0, 0
    for m in re.finditer(r"[()\[\]{};]", body):
        c = m.group(0)
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif depth == 0:
            out.append(body[last:m.start()])
            last = m.end()
    return out + [body[last:]]


def lhs_of(c, e):
    j = e - 1
    while j >= 0:
        ch = c[j]
        if ch == "]":
            d = 0
            while j >= 0:
                d += {"]": 1, "[": -1}.get(c[j], 0)
                if d == 0:
                    break
                j -= 1
            j -= 1
        elif ch.isalnum() or ch in "_.* \t\n\v":
            j -= 1
        elif ch == ">" and j > 0 and c[j - 1] == "-":
            j -= 2
        else:
            break
    return c[j + 1:e].strip()


def rhs_of(c, e):
    depth, k = 0, e + 1
    while k < len(c):
        ch = c[k]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif ch in ",;" and depth == 0:
            break
        k += 1
    return c[e + 1:k]


for fn in ALL:
    fn.assigns, fn.locals, fn.rets = [], {p for p in fn.params if p}, []
    for c in chunks(fn.body):
        if c.strip().startswith("return"):
            fn.rets.append(c.strip()[6:])
        dm = DECL_HEAD.match(c)
        if dm and "(" not in c.split("=")[0]:
            for piece in split_top(c[dm.end():]):
                nm = re.match(r"\s*\**\s*(?:const\s+)?(\w+)", piece)
                if nm:
                    fn.locals.add(nm.group(1))
        for m in ASSIGN.finditer(c):
            lhs = lhs_of(c, m.start())
            if lhs:
                fn.assigns.append((lhs, rhs_of(c, m.start())))
    fn.env = {p: frozenset({("P", i)}) for i, p in enumerate(fn.params) if p}
    fn.ret, fn.loose, fn.stores, fn.sinks, fn.flows = EMPTY, {}, {}, {}, []


CAST = re.compile(r"\(\s*(?:const\s+)?(?:struct\s+)?\w+\s*\**\s*\)\s*")
TRUTH = re.compile(r"==|!=|<=|>=|&&|\|\||<|(?<!-)>")
PAREN = re.compile(r"\((?:[^()]|\([^()]*\))*\)")
BRACK = re.compile(r"\[[^\]]*\]")
NAMEX = re.compile(r"(\w+)\s*((?:\[[^\]]*\]\s*)*)(?:(->|\.)\s*(\w+))?")
CALLX = re.compile(r"(\w+)\s*\(")
MEMBER = re.compile(r"\s*(?:\[[^\]]*\]\s*)*(?:->|\.)\s*(\w+)")
LOOSE = re.compile(r"\b[A-Za-z_]\w*\b(?!\s*\()")
RETS = set()       # names of functions that may return a value of interest


def unparen(e):
    e = e.strip()
    while e:
        m = CAST.match(e)
        if m and m.end() < len(e):
            e = e[m.end():].strip()
        elif e[0] == "(" and close_of(e, 0) == len(e):
            e = e[1:-1].strip()
        elif e[0] in "&*" and e[1:2] != "=":
            e = e[1:].strip()
        else:
            break
    return e


def env_atoms(name, fn):
    return frozenset({"W"}) if name in WORLD_NAMES else fn.env.get(name, EMPTY)


def subst(atoms, args, fn):
    out = set()
    for a in atoms:
        if a == "W":
            out.add("W")
        elif a[1] < len(args):
            out |= taint(args[a[1]], fn)
    return frozenset(out)


def taint(e, fn):
    if not any(t in fn.env or t in WORLD_NAMES or t in RETS for t in IDENT.findall(e)):
        return EMPTY
    try:
        e = unparen(e)
    except Unsupported:
        pass
    if not e:
        return EMPTY
    q = split_top(e, "?")
    if len(q) > 1:
        rest = "?".join(q[1:])
        alts = split_top(rest, ":")
        return taint(alts[0], fn) | taint(":".join(alts[1:]), fn)
    if e[0] == "{" and e[-1] == "}":
        out = EMPTY
        for x in split_top(e[1:-1]):
            out |= taint(x, fn)
        return out
    top = BRACK.sub("", PAREN.sub("", e))
    if e[0] == "!" or TRUTH.search(top):
        return EMPTY        # a truth value, not a program
    m = NAMEX.fullmatch(e)
    if m:
        if m.group(4) and m.group(4) != "program":
            return EMPTY
        return env_atoms(m.group(1), fn)
    m = CALLX.match(e)
    if m and close_of(e, m.end() - 1) == len(e):
        inner = e[m.end():-1]
        args = split_top(inner) if inner.strip() else []
        out = EMPTY
        for g in defs_of(m.group(1), fn):
            out |= subst(g.ret, args, fn)
        return out
    # Any other form that touches a value of interest is one this pass does
    # not model, and an error if that value can be a world program.
    out = EMPTY
    for w in LOOSE.finditer(e):
        fm = MEMBER.match(e, w.end())
        if not (fm and fm.group(1) != "program"):
            out |= env_atoms(w.group(0), fn)
    if out:
        fn.loose[e] = out
    return out


LVAL = re.compile(r"(\w+)\s*(?:\[[^\]]*\]\s*)*")


def values_of(fn):
    for _ in range(20):
        grew = False
        for lhs, rhs in fn.assigns:
            a = taint(rhs, fn)
            if not a:
                continue
            dm = DECL_HEAD.match(lhs)
            nm = LVAL.fullmatch(lhs[dm.end():] if dm else lhs)
            if nm and (dm or nm.group(1) in fn.locals):
                if not a <= fn.env.get(nm.group(1), EMPTY):
                    fn.env[nm.group(1)] = fn.env.get(nm.group(1), EMPTY) | a
                    grew = True
            else:
                fn.stores[lhs] = fn.stores.get(lhs, EMPTY) | a
        if not grew:
            break
    r = EMPTY
    for x in fn.rets:
        r |= taint(x, fn)
    return r


for fn in ALL:
    fn.ret = values_of(fn)
    if fn.ret:
        RETS.add(fn.name)
for _ in range(20):
    moved = False
    for fn in ALL:
        if any(g.ret for g in fn.out.values()):
            r = values_of(fn)
            if r != fn.ret:
                fn.ret, moved = r, True
                RETS.add(fn.name)
    if not moved:
        break
else:
    errors.append("the world-program value walk did not converge")
for fn in ALL:
    for name, args, _, mem in fn.calls:
        if name in BINDS and not mem:
            continue
        gs = targets(name, len(args), mem, fn)
        for i, x in enumerate(args):
            a = taint(x, fn)
            if a and gs:
                fn.flows += [(g, i, a) for g in gs]
            elif a and not re.fullmatch(r"gl\w+_fp", name):
                fn.sinks[(name, " ".join(x.split()))] = a

# ------------------------------------------------------------ the bound program
PINNED = set(PATHS) | {"GL_ImmEnd"}
SUM = {id(fn): (EMPTY, False) for fn in ALL}


class ProgDom:
    def __init__(self, fn):
        self.fn, self.draws, self.sites = fn, {}, {}
        self.cache = fn.__dict__.setdefault("evcache", {})

    def events(self, text, pos):
        key = (pos, text)
        if key not in self.cache:
            evs = []
            if not PROTO.fullmatch(text):
                for name, args, at, mem in calls_in(text):
                    if name in BINDS and not mem:
                        evs.append(("bind", taint(args[0], self.fn) if args else EMPTY))
                    elif re.fullmatch(GLDRAW, name) and not mem:
                        evs.append(("draw", pos + at))
                    else:
                        gs = targets(name, len(args), mem, self.fn)
                        if gs:
                            evs.append(("call", name, gs, [taint(x, self.fn) for x in args]))
                        if name in NORETURN and not mem:
                            evs.append(("stop",))
            self.cache[key] = evs
        return self.cache[key]

    def simple(self, text, pos, st):
        for ev in self.events(text, pos):
            if ev[0] == "bind":
                st = ev[1]
            elif ev[0] == "draw":
                self.draws[ev[1]] = self.draws.get(ev[1], EMPTY) | st
            elif ev[0] == "stop":
                return None
            else:
                atoms, kills = EMPTY, ev[1] not in FRAME_ROOTS
                for g in ev[2]:
                    self.sites[id(g)] = (g, self.sites.get(id(g), (g, EMPTY))[1] | st)
                    if ev[1] in FRAME_ROOTS:
                        continue
                    ga, gk = SUM[id(g)]
                    for a in ga:
                        atoms |= {a} if a == "W" else (ev[3][a[1]] if a[1] < len(ev[3]) else EMPTY)
                    kills = kills and gk
                st = atoms if kills else st | atoms
        return st

    def cond(self, text, pos, st):
        st = self.simple(text, pos, st)
        return st, st


work = list(REL.values())
queued = {id(f) for f in work}
for _ in range(200000):
    if not work:
        break
    fn = work.pop()
    queued.discard(id(fn))
    if fn.err:
        s = (frozenset({"W"}), False)
    else:
        fn.dom = ProgDom(fn)
        try:
            out = Interp(fn.dom).run(fn.ast, frozenset({"E"}))
            s = ((out or EMPTY) - {"E"}, out is not None and "E" not in out)
        except Unsupported as e:
            fn.err, s = str(e), (frozenset({"W"}), False)
    if s != SUM[id(fn)]:
        SUM[id(fn)] = s
        for c in CALLERS.get(id(fn), []):
            if id(c) in REL and id(c) not in queued:
                work.append(c)
                queued.add(id(c))
else:
    errors.append("the bound-program walk did not converge")

PT = {id(fn): set() for fn in ALL}
ENTRY = {id(fn): fn.name in FRAME_ROOTS for fn in ALL}


def world(atoms, fn):
    return any(a == "W" or (a == "E" and ENTRY[id(fn)]) or (isinstance(a, tuple) and a[1] in PT[id(fn)])
               for a in atoms)


moved = True
while moved:
    moved = False
    for fn in ALL:
        for g, i, a in fn.flows:
            if i not in PT[id(g)] and world(a, fn):
                PT[id(g)].add(i)
                moved = True
    for fn in REL.values():
        for g, st in (fn.dom.sites.values() if hasattr(fn, "dom") else ()):
            if not ENTRY[id(g)] and world(st, fn):
                ENTRY[id(g)] = moved = True

# GL_ImmEnd keeps the last shader it saw only to compare the next one with.
CACHE_OK = {("engine/h2shared/gl_vbo.c", "GL_ImmEnd", "imm_cache_shader")}
vbo_c = strip_c(read("engine/h2shared/gl_vbo.c"))
uses = re.findall(r"[^;{}]*\bimm_cache_shader\b[^;{}]*", vbo_c)
if sorted(" ".join(u.split()) for u in uses) != sorted([
        "static const glprogram_t *imm_cache_shader", "imm_cache_shader = NULL",
        "if (shader != imm_cache_shader)", "imm_cache_shader = shader"]):
    errors.append("imm_cache_shader is used for more than GL_ImmEnd's shader-changed compare "
                  f"({[' '.join(u.split()) for u in uses]}); the flow pass would have to follow it")
for fn in REL.values():
    if fn.err:
        errors.append(f"{fn.where()}: the flow pass cannot parse this function ({fn.err})")
    elif fn.name not in PINNED:
        for pos, st in fn.dom.draws.items():
            if world(st, fn):
                errors.append(f"{fn.where()} draws while a world program may be bound; write VertParams "
                              "there and pin it in PATHS, or draw through GL_ImmEnd")
                break
for fn in ALL:
    for what, table in (("stores", fn.stores), ("hands an unindexed callee", fn.sinks),
                        ("computes with", fn.loose)):
        for k, a in table.items():
            if world(a, fn) and (fn.file, fn.name, k) not in CACHE_OK:
                errors.append(f"{fn.where()} {what} a world-program value the flow pass cannot follow: {k}")

# ------------------------------------------------------------ writers, transitively
# A block writer is one of BLOCK_WRITERS or any engine function that reaches
# one; R_FlushUniforms alone writes nothing.
seeds = [g for n in BLOCK_WRITERS - {"R_FlushUniforms"} for g in INDEX.get(n, [])]
WRITERS = reaching(seeds)
if len(seeds) < len(BLOCK_WRITERS) - 3:
    errors.append("the flow pass indexed too few block writers")


def writer_between(fn_name, text):
    """The first call in text that is, or reaches, a block writer."""
    for name, args, _, mem in calls_in(text):
        if name == "R_FlushUniforms" or name in BINDS:
            continue
        if name in BLOCK_WRITERS:
            return name
        for g in targets(name, len(args), mem, INDEX[fn_name][0]):
            if id(g) in WRITERS:
                return f"{name} (reaches a block writer)"
    return None


for name in list(PATHS) + ["GL_ImmEnd"]:
    for fn in INDEX.get(name, []):
        b = fn.body
        fl = [m.end() for m in re.finditer(r"\bR_FlushUniforms\s*\(\s*\)", b)]
        if name == "GL_ImmEnd":
            fl = [m.end() for m in re.finditer(r"\bGL_ImmPushBlocks\s*\([^;]*\)", b)]
        draws = [m.start() for m in re.finditer(GLDRAW, b)]
        if len(fl) != 1 or not draws:
            errors.append(f"{fn.where()}: the flow pass expects one flush and its draws ({len(fl)}, {len(draws)})")
            continue
        for dp in draws:
            w = writer_between(name, b[fl[0]:dp]) if dp > fl[0] else None
            if w:
                errors.append(f"{fn.where()} calls {w} between its VertParams flush and a draw")
                break

# ------------------------------------------------------------ world_state_set
# DrawTextureChains_FlushBatch draws with the pair the last BindWorldState
# saved in two file statics.  That pair is this DrawTextureChains call's only
# while the local world_state_set is true: false on entry, set only right
# after BindWorldState, cleared wherever the state is torn down.  So: every
# FlushBatch call is reached with it true on every path, FlushBatch refuses
# to draw without it, and nothing DrawTextureChains calls can re-enter it or
# re-bind behind its back.
dtc = INDEX.get("DrawTextureChains", [None])[0]
FLAG = "world_state_set"


class FlagDom:
    def __init__(self):
        self.bad = []

    def simple(self, text, pos, st):
        t = " ".join(text.split())
        for m in re.finditer(r"\bDrawTextureChains_FlushBatch\s*\(", text):
            args = split_top(text[m.end():close_of(text, m.end() - 1) - 1])
            if args[0].strip() != FLAG:
                self.bad.append(f"FlushBatch is not handed {FLAG} ({t})")
            elif st != {"T"}:
                self.bad.append(f"FlushBatch is reached with {FLAG} possibly false ({t})")
        a = re.fullmatch(rf"(?:qboolean )?{FLAG} = (true|false)", t)
        if a:
            return frozenset({"T" if a.group(1) == "true" else "F"})
        rest = re.sub(r"\bDrawTextureChains_FlushBatch\s*\(\s*" + FLAG + r"\b", "", text)
        if re.search(rf"\b{FLAG}\b", rest):
            self.bad.append(f"{FLAG} is used in a form this check does not model ({t})")
        return st

    def cond(self, text, pos, st):
        t = " ".join(text.split())
        if t == FLAG:
            return st & {"T"} or None, st & {"F"} or None
        if t == "!" + FLAG:
            return st & {"F"} or None, st & {"T"} or None
        st = self.simple(text, pos, st)
        return st, st


if not (dtc and dtc.ast):
    errors.append("the flow pass has no parsed DrawTextureChains to follow world_state_set through")
else:
    fd = FlagDom()
    Interp(fd).run(dtc.ast, frozenset({"T", "F"}))
    errors += [f"DrawTextureChains: {b}" for b in dict.fromkeys(fd.bad)]
    db = " ".join(dtc.body.split())
    if len(re.findall(rf"\bqboolean {FLAG} = false;", db)) != 1 or re.search(rf"\bstatic\b[^;]*\b{FLAG}\b", db):
        errors.append(f"DrawTextureChains must declare one local `qboolean {FLAG} = false;`")
    dense = re.sub(r"\s+", "", dtc.body)
    n_set = dense.count(f"{FLAG}=true;")
    if not n_set or dense.count(f"DrawTextureChains_BindWorldState();{FLAG}=true;") != n_set:
        errors.append(f"every `{FLAG} = true;` must directly follow `DrawTextureChains_BindWorldState ();`")
    if not re.search(r"\bDrawTextureChains_FlushBatch\s*\(", db):
        errors.append("DrawTextureChains no longer calls DrawTextureChains_FlushBatch")
    bw_callers = {c.name for c in CALLERS.get(id(INDEX["DrawTextureChains_BindWorldState"][0]), [])}
    if bw_callers != {"DrawTextureChains"}:
        errors.append(f"DrawTextureChains_BindWorldState is called from {sorted(bw_callers)}; only "
                      "DrawTextureChains may refill the pair FlushBatch draws with")
    inner = reached(dtc)
    if id(dtc) in inner:
        errors.append("DrawTextureChains can re-enter itself through its callees, which would refill the "
                      "saved pair under the outer call's world_state_set")
fbf = INDEX.get("DrawTextureChains_FlushBatch", [None])[0]
if fbf:
    if fbf.params[:1] != [FLAG]:
        errors.append(f"DrawTextureChains_FlushBatch must take {FLAG} as its first parameter")
    first = re.sub(r"^\{(?:\s*(?:const\s+)?\w+\s+[^;(){}=]*(?:=[^;(){}]*)?;)*\s*", "", fbf.body)
    if not re.match(rf"if \(!{FLAG}\)\s*Sys_Error\s*\(", first):
        errors.append(f"DrawTextureChains_FlushBatch must open with `if (!{FLAG}) Sys_Error (...)`: "
                      "without the bind it would draw with another call's matrices")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS sworld_vert reads VertParams; mirror, file, inputs, varyings, builds and {len(PATHS) + 1} "
      f"draw paths agree, and the flow pass over {len(ALL)} functions ({len(REL)} that bind or draw) "
      "finds no other (#450 W1)")
