#!/usr/bin/env python3
"""The OIT twins' output stage is a file and reads OitFrag (#450 W3).

Until W3 the OIT twin of a fragment stage was a splice: gl_shader.c's
oit_preamble literal, with two loose ints (u_oit_mode, u_mboit_ms), pasted in
place of the stage's colour-output line.  Now it is engine/shaders/oit.inc,
included under #ifdef OIT in place of that line, and the twin is the same file
compiled with "#define OIT 1" after its header.  The two ints are OitFrag's
one ivec4.  Checks the pieces that have to move together:

  * the block: OitFrag in oit.inc and r_oitfrag_t in gl_uniforms.h agree (one
    ivec4, int[4], 16 bytes), the old names alias its x and y, its binding is
    its own (unique among every GL uniform-buffer binding the engine defines,
    and under ES 3.00's 24), and it is bound, uploaded with glBufferData into
    its own buffer, invalidated, seeded and deleted like the other blocks;
  * ONE writer.  OitFrag is per-pass state: R_SetOITMode, and nothing else,
    writes it, and every lane oit.inc reads is a lane R_SetOITMode writes.
    It is called from exactly the four pass transitions in gl_postprocess.c,
    with the arguments the loose GL_SetOITMode got, before the pass's draws;
    every draw with an OIT program goes through GL_ImmEnd (whose block push
    ends in R_FlushUniforms) or the skeletal draw's flush, so each one uploads
    the transition's write before it draws;
  * the shader files: salias_frag, sworld_frag and spart_frag -- and only they
    -- take oit.inc, each as the #ifdef OIT arm whose #else is the colour
    output, with OIT_TEX0 / OIT_UBO equal to the stage's own sampler and block
    counts (so SDL_GPU's slots stay gapless); oit.inc declares no loose
    non-opaque uniform, its four samplers at OIT_TEX0 + 0..3 = OIT_TEX_COUNT,
    its two outputs at 0 and 1, and renames main last; lightgrid.inc moves the
    light buffer past those samplers, and the stages that read it include
    oit.inc first;
  * the C side: no oit_preamble, splice or GL_SetOITMode is left; the six
    twins are rows of gl_shader.c's program table (#340 P1b), desktop-only,
    whose fragment header ends in GLSL_OIT_TWIN -- exactly "#define OIT 1\\n",
    spelled once -- on exactly the three stages that carry the arm, and every
    such row is built by GL_InitOITProgramSplit, which refuses a twin without
    OitFrag and points the moment samplers at MBOIT_TMU_*;
    GL_InitProgramUniforms binds the block on every program; nothing else
    spells "#define OIT".
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
H2 = ROOT / "engine/h2shared"
errors = []

CSKIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"', re.S)


def blank_c(t):
    """Comments and string literals blanked, offsets kept."""
    return CSKIP.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), t)


def nocomment_c(t):
    """Comments blanked, string literals kept."""
    return re.sub(r'//[^\n]*|/\*.*?\*/|("(?:[^"\\\n]|\\.)*")',
                  lambda m: m.group(1) or re.sub(r"[^\n]", " ", m.group(0)), t, flags=re.S)


def strip_glsl(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


def match_close(text, i, open_="{", close="}"):
    depth = 0
    for m in re.finditer("[%s%s]" % (re.escape(open_), re.escape(close)), text[i:]):
        depth += 1 if m.group(0) == open_ else -1
        if depth == 0:
            return i + m.end()
    return len(text)


def body(code, signature):
    """Body of the function whose definition matches `signature` (searched in
    the literal-blanked view so a literal never matches)."""
    b = blank_c(code)
    m = re.search(signature, b)
    if not m:
        return None
    i = b.index("{", m.end() - 1)
    return code[i:match_close(b, i)]


def read(p):
    return Path(p).read_text()


oit = read(SH / "oit.inc")
oitc = strip_glsl(oit)
uh = nocomment_c(read(H2 / "gl_uniforms.h"))
uc = nocomment_c(read(H2 / "gl_uniforms.c"))
sh_h = nocomment_c(read(H2 / "gl_shader.h"))
sc = nocomment_c(read(H2 / "gl_shader.c"))
pp = nocomment_c(read(H2 / "gl_postprocess.c"))
lightgrid = strip_glsl(read(SH / "lightgrid.inc"))

# ------------------------------------------------------------ the block and its mirror
blocks = re.findall(r"\buniform\s+(\w+)\s*\{([^}]*)\}", oitc)
if [b for b, _ in blocks] != ["OitFrag"]:
    errors.append(f"oit.inc declares blocks {[b for b, _ in blocks]}; it is exactly OitFrag")
else:
    members = re.findall(r"(?:highp\s+|mediump\s+|lowp\s+)?(\w+)\s+(\w+)\s*;", blocks[0][1])
    if members != [("ivec4", "u_foit")]:
        errors.append(f"OitFrag is {members}; it is one ivec4 u_foit (16 bytes, std140)")
if not re.search(r"\bFRAG_UBO\(OIT_UBO\)\s*uniform\s+OitFrag\b", oitc):
    errors.append("OitFrag must take its SDL_GPU slot from OIT_UBO: FRAG_UBO(OIT_UBO) uniform OitFrag")
ALIAS = {"u_oit_mode": "x", "u_mboit_ms": "y"}
for name, lane in ALIAS.items():
    if not re.search(rf"^\s*#\s*define\s+{name}\s+u_foit\.{lane}\s*$", oitc, re.M):
        errors.append(f"oit.inc must alias {name} to u_foit.{lane}")
mirror = re.search(r"typedef struct \{([^}]*)\} r_oitfrag_t;", uh)
if not mirror or re.findall(r"\b(\w+)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;", mirror.group(1)) != [("int", "mode", "4")]:
    errors.append("r_oitfrag_t must be exactly `int mode[4];`, OitFrag's ivec4")
if not re.search(r"COMPILE_TIME_ASSERT\(oitfrag_size, sizeof\(r_oitfrag_t\) == 16\);", uc):
    errors.append("gl_uniforms.c must pin sizeof(r_oitfrag_t) at 16")

bindings = {}
for p in sorted(H2.glob("*.[ch]")):
    for m in re.finditer(r"^\s*#\s*define\s+(\w*UBO_BINDING\w*)\s+(\d+)\b", nocomment_c(p.read_text()), re.M):
        bindings.setdefault(m.group(1), set()).add(int(m.group(2)))
ob = bindings.get("UBO_BINDING_OIT_FRAG")
if not ob or len(ob) != 1:
    errors.append("UBO_BINDING_OIT_FRAG must be defined once (gl_shader.h)")
else:
    ob = next(iter(ob))
    clash = sorted(n for n, v in bindings.items() if n != "UBO_BINDING_OIT_FRAG" and ob in v)
    if clash:
        errors.append(f"UBO_BINDING_OIT_FRAG {ob} collides with {clash}")
    if ob >= 24:
        errors.append(f"UBO_BINDING_OIT_FRAG {ob} is past ES 3.00 / WebGL2's 24 binding points")
if len(bindings) < 10:
    errors.append(f"only {len(bindings)} uniform-buffer binding defines found; the scan is broken")

if '#define RU_NAME_OFRAG	"OitFrag"' not in uc:
    errors.append('gl_uniforms.c must name the block "OitFrag" (RU_NAME_OFRAG)')
bind_fn = body(uc, r"GLint R_BindOITFragBlock \(GLuint program\)\s*\{") or ""
if "RU_BindOne (program, RU_NAME_OFRAG, UBO_BINDING_OIT_FRAG)" not in bind_fn \
        or "RU_CheckBlockSize (program, RU_NAME_OFRAG, sizeof(r_oitfrag_t))" not in bind_fn:
    errors.append("R_BindOITFragBlock must pin OitFrag to UBO_BINDING_OIT_FRAG and check its size once")
flush = body(uc, r"void R_FlushUniforms \(void\)\s*\{") or ""
bd = re.search(r"if \(bindings_dirty\)\s*\{(.*?)\}", flush, re.S)
if not bd or "GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_OIT_FRAG, ubo_ofrag);" not in bd.group(1):
    errors.append("R_FlushUniforms must bind ubo_ofrag at UBO_BINDING_OIT_FRAG when the bindings are dirty")
od = re.search(r"if \(ofrag_dirty\)\s*\{(.*?)\}", flush, re.S)
if not od or not re.search(r"ofrag_dirty = false;\s*GL_BindBuffer \(GL_UNIFORM_BUFFER, ubo_ofrag\);\s*"
                           r"glBufferData_fp \(GL_UNIFORM_BUFFER, sizeof\(oitfrag\), &oitfrag,", od.group(1)):
    errors.append("R_FlushUniforms must upload the whole of oitfrag into ubo_ofrag when it is dirty")
inv = body(uc, r"void R_InvalidateUniforms \(void\)\s*\{") or ""
if "ofrag_dirty = true;" not in inv:
    errors.append("R_InvalidateUniforms must re-upload OitFrag")
init = body(uc, r"void R_Uniforms_Init \(void\)\s*\{") or ""
for w in ("memset (&oitfrag, 0, sizeof(oitfrag));", "glGenBuffers_fp (1, &ubo_ofrag);", "ofrag_dirty = true;",
          "ofrag_layout_checked = false;"):
    if w not in init:
        errors.append(f"R_Uniforms_Init: missing `{w}`")
if "ofrag_dirty = true;" in init and init.rfind("ofrag_dirty = true;") > init.rfind("R_FlushUniforms ();"):
    errors.append("R_Uniforms_Init must seed OitFrag before its flush")
shut = body(uc, r"void R_Uniforms_Shutdown \(void\)\s*\{") or ""
if not re.search(r"GL_ForgetBuffer \(ubo_ofrag\);\s*glDeleteBuffers_fp \(1, &ubo_ofrag\);\s*ubo_ofrag = 0;", shut):
    errors.append("R_Uniforms_Shutdown must forget and delete ubo_ofrag")

# ------------------------------------------------------------ one writer, every read lane written
setter = body(uc, r"void R_SetOITMode \(int mode, qboolean ms\)\s*\{") or ""
if not setter:
    errors.append("cannot find R_SetOITMode in gl_uniforms.c")
written = {int(i) for i in re.findall(r"\boitfrag\.mode\[(\d)\]\s*=(?!=)", setter)}
if "m = ms ? 1 : 0;" not in setter or "oitfrag.mode[0] = mode;" not in setter or "oitfrag.mode[1] = m;" not in setter:
    errors.append("R_SetOITMode must store the mode in x and the multisample flag, as 0 or 1, in y")
dirty = re.search(r"if \(([^{]*)\)\s*\{[^}]*\bofrag_dirty = true;", setter)
cond = " ".join(dirty.group(1).split()) if dirty else ""
if not dirty or not {"oitfrag.mode[0] != mode", "oitfrag.mode[1] != m"} <= set(re.split(r"\s*\|\|\s*", cond)):
    errors.append("R_SetOITMode must mark OitFrag dirty whenever either lane changes")
ucb = blank_c(read(H2 / "gl_uniforms.c"))
for m in re.finditer(r"\boitfrag\b", ucb):
    fn = [f for f in re.finditer(r"^\w[^\n;]*\(([^;{)]*)\)\s*\{", ucb, re.M) if f.start() < m.start()]
    fname = re.search(r"(\w+)\s*\(", fn[-1].group(0)).group(1) if fn else "file scope"
    if fname not in ("R_SetOITMode", "R_FlushUniforms", "R_Uniforms_Init", "file scope"):
        errors.append(f"gl_uniforms.c: {fname} names oitfrag; R_SetOITMode is its only writer")
LANE = {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3}
code_only = re.sub(r"^\s*#.*$", " ", oitc, flags=re.M)
read_lanes = set()
for name, lane in ALIAS.items():
    if re.search(rf"\b{name}\b", code_only):
        read_lanes.add(LANE[lane])
for sw in re.findall(r"\bu_foit\s*\.\s*([xyzwrgba]+)", code_only):
    read_lanes |= {LANE[c] for c in sw}
if re.search(r"\bu_foit\b(?!\s*\.)", code_only.replace("ivec4 u_foit;", "")):
    read_lanes |= {0, 1, 2, 3}
if read_lanes != written:
    errors.append(f"oit.inc reads OitFrag lanes {sorted(read_lanes)} but R_SetOITMode writes {sorted(written)}; "
                  "a lane read and never written holds the seed forever")
if not {0, 1} <= read_lanes:
    errors.append("oit.inc no longer reads both of u_oit_mode and u_mboit_ms; the lane scan is broken")

ppb = blank_c(read(H2 / "gl_postprocess.c"))
CALLS = {"OIT_BeginTranslucency": ["1, oit_samples > 1", "0, false"], "OIT_NextPass": ["2, oit_samples > 1"],
         "OIT_EndTranslucency": ["0, false"]}
seen = {}
for p in sorted(list(H2.glob("*.c")) + list((ROOT / "engine/hexen2").glob("*.c"))):
    t = nocomment_c(p.read_text())
    b = blank_c(p.read_text())
    for m in re.finditer(r"\bR_SetOITMode\s*\(([^;]*?)\)\s*;", t):
        if p.name == "gl_uniforms.c":
            continue
        fn = [f for f in re.finditer(r"^\w[^\n;]*?\b(\w+)\s*\([^;{)]*\)\s*\{", b, re.M) if f.start() < m.start()]
        fname = fn[-1].group(1) if fn else "?"
        seen.setdefault((p.name, fname), []).append(" ".join(m.group(1).split()))
want = {("gl_postprocess.c", k): v for k, v in CALLS.items()}
if seen != want:
    errors.append(f"R_SetOITMode is called {seen}; the four pass transitions are {want}")
beg = body(pp, r"void OIT_BeginTranslucency \(void\)\s*\{") or ""
mb = re.search(r"if \(r_oit\.integer >= 2 && mboit_available\)\s*\{(.*?)\n\t\}", beg, re.S)
if not mb or not re.search(r"R_SetOITMode \(1, oit_samples > 1\);\s*return;", mb.group(1)):
    errors.append("OIT_BeginTranslucency's moment arm must set mode 1 last, just before it returns to draw")
if beg and beg.find("R_SetOITMode (0, false);") < beg.find("mboit_pass = 0;"):
    errors.append("OIT_BeginTranslucency's WBOIT arm must set mode 0 once it has left the moment arm")
nxt = body(pp, r"qboolean OIT_NextPass \(void\)\s*\{") or ""
if not re.search(r"mboit_pass = 2;.*R_SetOITMode \(2, oit_samples > 1\);\s*return true;", nxt, re.S):
    errors.append("OIT_NextPass must set mode 2 after moving to the composite and before it returns to draw")
end = body(pp, r"void OIT_EndTranslucency \(GLuint scene_fbo\)\s*\{") or ""
if not re.search(r"if \(mboit_pass\)\s*\{.*R_SetOITMode \(0, false\);\s*mboit_pass = 0;", end, re.S):
    errors.append("OIT_EndTranslucency must return the moment passes' mode to 0, as GL_SetOITMode did")
if '#include "gl_uniforms.h"' not in read(H2 / "gl_postprocess.c"):
    errors.append("gl_postprocess.c must include gl_uniforms.h for R_SetOITMode")

# Every draw with an OIT program flushes after the transition: it reaches the
# GPU through GL_ImmEnd -- whose block push ends in R_FlushUniforms for any
# program with FragParams, which every OIT twin has -- or through a path that
# flushes itself.
vbo = nocomment_c(read(H2 / "gl_vbo.c"))
push = body(vbo, r"static void GL_ImmPushBlocks \(const glprogram_t \*shader, const float \*mvp\)\s*\{") or ""
if not re.search(r"R_FlushUniforms \(\);\s*\}\s*$", push):
    errors.append("GL_ImmPushBlocks must end in an unconditional R_FlushUniforms")
imm = body(vbo, r"void GL_ImmEnd \(GLenum mode, const glprogram_t \*shader\)\s*\{") or ""
if not re.search(r"if \(shader->ub_vert >= 0 \|\| shader->ub_frag >= 0\)\s*GL_ImmPushBlocks\(shader, mvp\);", imm) \
        or imm.find("GL_ImmPushBlocks(shader, mvp);") > imm.find("glDraw"):
    errors.append("GL_ImmEnd must push (and so flush) the blocks of any FragParams program before it draws")
OITPROG = re.compile(r"\bgl_shader_\w+_oit\b")


def stmt_start(text, i):
    """Start of the statement holding text[i]: GL_ImmEnd (mode, pick ()) names
    the program inside its own call."""
    return max(text.rfind(c, 0, i) for c in ";{}") + 1
FLUSHES = re.compile(r"\bGL_ImmEnd\s*\(|\bR_FlushUniforms\s*\(")
RAWDRAW = re.compile(r"\bglDraw\w*\s*\(")


def flushes_first(text):
    """A GL_ImmEnd or R_FlushUniforms comes before any raw glDraw* in text:
    the pick must reach the GPU through a flush, not merely be followed by one."""
    f, d = FLUSHES.search(text), RAWDRAW.search(text)
    return bool(f) and (not d or f.start() < d.start())
oit_users = 0
for p in sorted(list(H2.glob("*.c")) + list((ROOT / "engine/hexen2").glob("*.c"))):
    if p.name == "gl_shader.c":
        continue
    raw = p.read_text()
    t, b = nocomment_c(raw), blank_c(raw)
    fns = [(f.group(1), b.index("{", f.end() - 1)) for f in
           re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{)]*\)\s*\{", b, re.M)]
    for name, i in fns:
        j = match_close(b, i)
        fb = t[i:j]
        refs = list(OITPROG.finditer(fb))
        if not refs:
            continue
        oit_users += 1
        if re.search(r"\breturn\b[^;]*\bgl_shader_\w+_oit\b", fb):
            # a selector: each caller must flush after the call
            for q in sorted(list(H2.glob("*.c")) + list((ROOT / "engine/hexen2").glob("*.c"))):
                qb = blank_c(q.read_text())
                for c in re.finditer(r"\b%s\s*\(\s*\)" % name, qb):
                    k = stmt_start(qb, c.start())
                    fn2 = [f for f in re.finditer(r"^\w[^\n;]*?\b(\w+)\s*\([^;{)]*\)\s*\{", qb, re.M)
                           if f.start() < k]
                    if not fn2:
                        continue
                    s2 = qb.index("{", fn2[-1].end() - 1)
                    if not flushes_first(qb[k:match_close(qb, s2)]):
                        errors.append(f"{q.name}: {fn2[-1].group(1)} takes an OIT program from {name}() "
                                      "and never flushes after it")
        elif not flushes_first(fb[stmt_start(fb, refs[0].start()):]):
            errors.append(f"{p.name}: {name} picks an OIT program and never reaches GL_ImmEnd or "
                          "R_FlushUniforms after it")
if oit_users < 5:
    errors.append(f"only {oit_users} functions pick an OIT program; the scan is broken")

# ------------------------------------------------------------ the shader files
ARM = re.compile(r'#ifdef OIT\s*\n\s*#define OIT_TEX0 (\d+)\s*\n\s*#define OIT_UBO (\d+)\s*\n\s*'
                 r'#include "oit\.inc"\s*\n\s*#else\s*\n\s*FRAG_OUT\(0\) out vec4 fragColor;\s*\n\s*#endif\s*\n')


def expand(name, seen=None):
    seen = set() if seen is None else seen
    out = []
    for line in (SH / name).read_text().splitlines():
        m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
        if m:
            if m.group(1) not in seen:
                seen.add(m.group(1))
                out.append(expand(m.group(1), seen))
        else:
            out.append(line)
    return "\n".join(out)


includers = sorted(f.name for f in SH.iterdir() if f.suffix in (".glsl", ".inc")
                   and re.search(r'#\s*include\s+"oit\.inc"', f.read_text()))
STAGES = ["salias_frag.glsl", "spart_frag.glsl", "sworld_frag.glsl"]
if includers != STAGES:
    errors.append(f"oit.inc is included by {includers}; exactly the three OIT-twinned stages {STAGES} take it")
for st in STAGES:
    text = (SH / st).read_text()
    m = ARM.search(text)
    if not m:
        errors.append(f"{st}: the colour output must be the #else arm of `#ifdef OIT / OIT_TEX0 / OIT_UBO / "
                      "#include \"oit.inc\"`")
        continue
    code = strip_glsl(text)
    tex = [int(x) for x in re.findall(r"\bFRAG_TEX\((\d+)\)", code)]
    if tex != list(range(len(tex))):
        errors.append(f"{st}: FRAG_TEX must be 0..n-1 in order, is {tex}")
    if int(m.group(1)) != len(tex):
        errors.append(f"{st}: OIT_TEX0 is {m.group(1)}, the stage declares {len(tex)} samplers")
    # The stage's own fragment blocks; oit.inc's is FRAG_UBO(OIT_UBO), which
    # this does not match.
    ubo = [int(x) for x in re.findall(r"\bFRAG_UBO\((\d+)\)", strip_glsl(expand(st)))]
    if ubo != list(range(len(ubo))):
        errors.append(f"{st}: its own FRAG_UBO slots must be 0..n-1, are {ubo}")
    if int(m.group(2)) != len(ubo):
        errors.append(f"{st}: OIT_UBO is {m.group(2)}, the stage declares {len(ubo)} fragment blocks")
    if '#define UNIFORMS_FRAG\n#include "uniforms.inc"' not in text:
        errors.append(f"{st}: must read FragParams (GL_ImmEnd's flush is keyed on it)")
    if "lightgrid.inc" in text and text.index('#include "oit.inc"') > text.index('#include "lightgrid.inc"'):
        errors.append(f"{st}: oit.inc must come before lightgrid.inc, or the OIT twin's light buffer "
                      "lands on a moment sampler's slot")
    if "main_body" in code:
        errors.append(f"{st}: names main_body; oit.inc renames the stage's main, the stage must not")

samp = re.findall(r"\bFRAG_TEX\(OIT_TEX0(?:\s*\+\s*(\d+))?\)\s*uniform\s+(\w+)\s+(\w+)\s*;", oitc)
want_s = [(0, "sampler2D", "u_mboit_b0"), (1, "sampler2D", "u_mboit_mom"),
          (2, "sampler2DMS", "u_mboit_b0_ms"), (3, "sampler2DMS", "u_mboit_mom_ms")]
if [(int(k or 0), ty, n) for k, ty, n in samp] != want_s:
    errors.append(f"oit.inc's samplers are {samp}; they are the four moment targets at OIT_TEX0 + 0..3")
cnt = re.search(r"^\s*#\s*define\s+OIT_TEX_COUNT\s+(\d+)\s*$", oitc, re.M)
if not cnt or int(cnt.group(1)) != len(samp):
    errors.append("oit.inc's OIT_TEX_COUNT must equal its sampler count")
loose = [n for ty, n in re.findall(r"^\s*(?:FRAG_TEX\([^)]*\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)\s*;",
                                   oitc, re.M) if not re.match(r"[iu]?sampler", ty)]
if loose:
    errors.append(f"oit.inc declares loose non-opaque uniforms {loose}")
for w in ("vec4 fragColor = vec4(0.0);", "FRAG_OUT(0) out vec4 out_accum;", "FRAG_OUT(1) out vec4 out_reveal;",
          "void main_body();"):
    if w not in oitc:
        errors.append(f"oit.inc lost `{w}`")
tail = [l.strip() for l in oitc.splitlines() if l.strip()]
if tail[-2:] != ["#define main main_body", "#endif"]:
    errors.append("oit.inc must end by renaming main (#define main main_body), after its own main()")
if not re.search(r"#if !defined\(OIT_TEX0\) \|\| !defined\(OIT_UBO\)\n#error", oitc):
    errors.append("oit.inc must refuse to build without OIT_TEX0 and OIT_UBO")
if not re.search(r"#ifdef OIT_TEX_COUNT\s*#define LIGHTGRID_SET2_SLOT\s*\(LIGHTGRID_SSBO_SLOT \+ OIT_TEX_COUNT\)\s*"
                 r"#else\s*#define LIGHTGRID_SET2_SLOT\s*LIGHTGRID_SSBO_SLOT\s*#endif", lightgrid) \
        or "FRAG_SSBO(LIGHT_SSBO_BINDING, LIGHTGRID_SET2_SLOT)" not in lightgrid:
    errors.append("lightgrid.inc must put the light buffer past oit.inc's samplers in an OIT twin")

# ------------------------------------------------------------ the C side
for p in sorted(list(H2.glob("*.[ch]")) + list((ROOT / "engine/hexen2").glob("*.[ch]"))):
    b = blank_c(p.read_text())
    for gone in ("oit_preamble", "GL_CompileOITFragShaderSplit", "GL_CompileOITFragShader",
                 "GL_LoadOITProgramSplit", "GL_SetOITMode"):
        if re.search(rf"\b{gone}\b", b):
            errors.append(f"{p.relative_to(ROOT)} names {gone}, which #450 W3 removed")
gst = re.search(r"typedef struct glprogram_s \{(.*?)\} glprogram_t;", sh_h, re.S)
gfields = set(re.findall(r"\b(u_\w+|ub_\w+)\b", gst.group(1))) if gst else set()
if "ub_ofrag" not in gfields or {"u_oit_mode", "u_mboit_ms"} & gfields:
    errors.append("glprogram_t must carry ub_ofrag and no u_oit_mode / u_mboit_ms location")
gipu = body(sc, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or ""
if "p->ub_ofrag = R_BindOITFragBlock(p->program);" not in gipu:
    errors.append("GL_InitProgramUniforms must bind OitFrag on every program (p->ub_ofrag)")
ip = body(sc, r"static void GL_InitOITProgramSplit \([^)]*\)\s*\{") or ""
for w, why in ((r"p->program = R_Program_Create \(id\);", "build the twin by its id"),
               (r"GL_InitProgramUniforms \(p\);\s*if \(p->ub_ofrag < 0\)\s*\{[^}]*glDeleteProgram_fp \(p->program\);"
                r"\s*p->program = 0;", "refuse a twin that declares no OitFrag")):
    if not re.search(w, ip):
        errors.append(f"GL_InitOITProgramSplit must {why}")
for s_, u in (("u_mboit_b0", "MBOIT_TMU_B0"), ("u_mboit_mom", "MBOIT_TMU_MOM"),
              ("u_mboit_b0_ms", "MBOIT_TMU_B0_MS"), ("u_mboit_mom_ms", "MBOIT_TMU_MOM_MS")):
    if not re.search(rf"if \(p->{s_} >= 0\)\s*glUniform1i_fp\(p->{s_}, {u}\);", ip):
        errors.append(f"GL_InitOITProgramSplit must point {s_} at {u}")
if not re.search(r'#define GLSL_OIT_TWIN\t"#define OIT 1\\n"\n', sc):
    errors.append('GLSL_OIT_TWIN must be exactly "#define OIT 1\\n"')


def conds(text):
    """The #if stack in force at each line of text."""
    stack, out = [], []
    for line in text.splitlines():
        t = line.strip()
        if re.match(r"#\s*if", t):
            stack.append(t)
        elif re.match(r"#\s*else", t) and stack:
            stack[-1] = "else:" + stack[-1]
        elif re.match(r"#\s*endif", t) and stack:
            stack.pop()
        out.append(list(stack))
    return out


at = conds(sc)
ROW = re.compile(r'\[(RPROG_\w+)\] = \{ "\w+", \w+, \{\s*\{ [^{}]*?, \w+, "[\w.]+" \},\s*'
                 r'\{ ([^{}]*?), (\w+), "[\w.]+" \} \} \},')
twins = {}
for m in ROW.finditer(sc):
    if re.search(r"\bGLSL_OIT_TWIN\s*$", m.group(2)):
        twins[m.group(1)] = (m.group(3), at[sc.count("\n", 0, m.start())])
builds = {}
for m in re.finditer(r"GL_InitOITProgramSplit\s*\(&(\w+), (RPROG_\w+)\);", sc):
    builds[m.group(2)] = (m.group(1), at[sc.count("\n", 0, m.start())])
if len(twins) != 6:
    errors.append(f"{len(twins)} program rows carry GLSL_OIT_TWIN; the six OIT twins are a row each")
for rid, (src, st) in twins.items():
    if "#ifndef USE_GLES" not in st:
        errors.append(f"{rid}'s row is outside #ifndef USE_GLES; OIT is desktop-only")
    if src + ".glsl" not in STAGES:
        errors.append(f"{rid} is built from {src}, which carries no OIT arm")
    if rid not in builds:
        errors.append(f"{rid} carries GLSL_OIT_TWIN but GL_InitOITProgramSplit does not build it, so "
                      "nothing refuses it without OitFrag")
for rid, (prog, st) in builds.items():
    if rid not in twins:
        errors.append(f"{prog} is built from {rid}, whose fragment header carries no GLSL_OIT_TWIN")
    if "#ifndef USE_GLES" not in st:
        errors.append(f"{prog} is built outside #ifndef USE_GLES; OIT is desktop-only")
if len(builds) != 6:
    errors.append(f"{len(builds)} GL_InitOITProgramSplit builds found; the six OIT twins are built once each")
if sorted({src for src, _ in twins.values()}) != sorted(s_[:-5] for s_ in STAGES):
    errors.append(f"the OIT twins cover {sorted({src for src, _ in twins.values()})}; every arm-carrying "
                  "stage needs one")
for p in sorted(list(H2.glob("*.c")) + list((ROOT / "engine/hexen2").glob("*.c"))):
    for v in re.findall(r'"((?:[^"\\\n]|\\.)*)"', nocomment_c(p.read_text())):
        if re.search(r"#\s*define\s+OIT\b", v) and not (p.name == "gl_shader.c" and v == "#define OIT 1\\n"):
            errors.append(f"{p.name}: a literal spells `{v}`; only GLSL_OIT_TWIN defines OIT")
if len(re.findall(r'"#define OIT 1\\n"', sc)) != 1:
    errors.append("gl_shader.c must spell \"#define OIT 1\\n\" exactly once, as GLSL_OIT_TWIN")

if errors:
    print("FAIL:")
    for e in dict.fromkeys(errors):
        print("  -", e)
    sys.exit(1)
print(f"PASS the OIT twins read OitFrag (16 bytes, binding {ob}) from oit.inc; one writer at the four pass "
      f"transitions, {oit_users} OIT-picking functions all flush, {len(STAGES)} stages carry the arm, "
      f"{len(builds)} desktop-only builds (#450 W3)")
