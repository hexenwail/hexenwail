#!/usr/bin/env python3
"""No alias- or world-family program, nor any OIT twin, regains a loose
non-opaque uniform (#450, alias S5, world W2, OIT W3).

After slices S1-S4 all four alias stages -- salias_vert, salias_frag,
salias_inst_vert, sskeletal_vert -- and after W1-W2 all three world stages --
sworld_vert, sworld_frag, sworld_opaque_frag -- read every non-opaque value
from a std140 block (VertParams / FragParams in uniforms.inc, AliasVert /
AliasFrag in alias.inc, WorldFrag in world.inc), reaching the old loose names
through `#define u_x <member>` macros.  Since W3 so do the OIT twins of
salias_frag and sworld_frag (and spart_frag): their output stage, oit.inc,
reads its mode switches from OitFrag.  A glGetUniformLocation on such a name
returns -1 on every program, so the C plumbing that looked it up, cached it
and glUniform*'d it was dead, and S5 and W2 removed it.  This keeps it
removed.  ("Alias names" below is the historical spelling: the set covers both
families.)

The alias names are DERIVED: every block macro a stage of either family (or a
helper .inc it pulls in) actually reads, minus every name that some live
shader -- any engine/shaders file, or any string literal in gl_shader.c, both
tiers -- still declares as a loose `uniform`.  Since W3 no live shader
declares ANY non-opaque loose uniform (W2 left only the OIT preamble's two
mode switches, and W3 moved them onto OitFrag), and that is checked outright.
FLOOR pins today's full derived set, so a name cannot leave it silently --
which is exactly what re-declaring it loose somewhere would do.

What the stages compile WITH counts as theirs.  The header chunks
passed alongside a stage (GLSL_VERT_HEADER, GLSL_ALIAS_FRAG_HEADER,
GLSL_WORLD_FRAG_HEADER, GLSL_ALIAS_INST_VERT_HEADER, the NOPERSP literal, ...
-- resolved from the
compile calls, through macros, q_snprintf buffers and parameters) are scanned
like the stage files: a non-opaque loose uniform in one fails, and nothing
declared there makes a name "live elsewhere".  The OIT twins' fragment header
is that chunk plus the "#define OIT 1" GL_InitOITProgramSplit appends with
q_snprintf, which is resolved too.

Checks, for each derived alias name, over every engine C file:
  * no C identifier spells it -- not a struct field of any program struct,
    not a local, not a macro argument (which is also what a stringizing
    `#n` lookup needs);
  * no string literal, adjacent literals joined, equals it -- a lookup needs
    the name as a string, whatever function or table it goes through.
And for the alias-family programs themselves:
  * every `uniform` in the stages (includes expanded) and their header chunks
    is a block, an opaque declaration, or a failure -- multi-declarator and
    initialised forms included, and a #define that builds one counts;
  * a glUniform* through gl_shader_alias* / gl_shader_skeletal* /
    gl_shader_world*, through a local that was ever assigned one, or through
    R_AliasProgram() and the like, may target only a stage sampler (the OIT
    twins' moment samplers included);
  * GL_InitAliasInstProgram looks up only samplers the stages declare, and
    gl_alias_inst_prog_t carries no other location;
  * GL_InitProgramUniforms looks every field up under its own name, as one
    plain literal, and only names some live shader declares loose.
Every glprogram_t location must name a uniform some live shader still
declares loose, unless it is in NON_ALIAS_DEAD -- a known-dead leftover
outside the alias family; that list fails when an entry goes stale, so it can
only shrink, and it is empty now.  Every imm_cache_* in gl_vbo.c must sit
under an `if (shader->u_X ...)` whose u_X is such a location: a cache tied to
nothing is a cache for a value no program has loose.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []

ALIAS_STAGES = ("salias_vert.glsl", "salias_frag.glsl",
                "salias_inst_vert.glsl", "sskeletal_vert.glsl",
                # the world family since #450 W2 (its vertex stage since W1)
                "sworld_vert.glsl", "sworld_frag.glsl", "sworld_opaque_frag.glsl")
STAGE_SYMS = {s[:-5] for s in ALIAS_STAGES}
# The .inc files that DECLARE blocks (or the per-backend layout macros).  A
# stage includes them, but the macro list inside is not a read.
BLOCK_INCS = ("uniforms.inc", "alias.inc", "world.inc", "layout.inc")
# Every name the derivation yields today.  If parsing breaks, or a name is
# re-declared loose somewhere and so drops out of the set, the guard fails
# rather than passing over a name it no longer protects.  Grows only when an
# alias stage starts reading a new block macro.
FLOOR = {"u_alias_caustics", "u_alias_dlight", "u_alias_lightdir", "u_alias_material",
         "u_alias_model", "u_eyepos", "u_fullbright", "u_inst_base", "u_lightcolor",
         "u_pose_base", "u_pose_base2", "u_poseverttype", "u_shadevector",
         "u_skel_blend", "u_soft_params", "u_viewproj",
         # since #450 W1: no live shader declares the matrices loose
         "u_modelview", "u_mvp",
         # since #450 W2: the world fragment stages' names, every one of them
         # a FragParams or WorldFrag member (some shared with the alias stage)
         "u_material", "u_fog_density", "u_fog_color", "u_alpha_threshold",
         "u_force_opaque_alpha", "u_caustics", "u_turb", "u_overbright",
         "u_lightmap_bicubic", "u_lightdebug", "u_softemu", "u_spans", "u_lightview",
         "u_lightgrid_xy", "u_lightgrid_z", "u_dlight_scale", "u_lmstyles",
         # since #450 W3: the OIT twins' mode switches, OitFrag members
         "u_oit_mode", "u_mboit_ms"}
# Loose-lookup fields with no live declarer that are NOT alias-family names,
# listed so their deadness is explicit rather than silent.  It held s2d's
# u_color and the sky programs' u_time / u_skyfog / u_wind / u_wind3 /
# u_skyrot until their plumbing went too (#450); keep it empty.
NON_ALIAS_DEAD = set()
OPAQUE = re.compile(r"^(?:[iu]?sampler\w*|[iu]?image\w*)$")
ALIAS_PROG = r"gl_shader_(?:alias|skeletal|world)\w*"
QUALS = r"(?:(?:lowp|mediump|highp|readonly|writeonly|coherent|volatile|restrict)\s+)*"


def strip(t):
    """GLSL comment strip (shader files, and GLSL text pulled out of C literals)."""
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


# ---------------------------------------------------------------- C lexing
# Every C scan works on two same-length views of a file, so offsets carry over:
# `code` has comments blanked (string literals intact, for reading them) and
# `blank` has literals blanked too (for brace, paren, comma and identifier
# scans that must not see the GLSL inside gl_shader.c's strings).
CSKIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'')
SPACE = re.compile(r"[^\n]")


def c_views(text):
    code, blank, last = [], [], 0
    for m in CSKIP.finditer(text):
        s = m.group(0)
        code.append(text[last:m.start()])
        blank.append(text[last:m.start()])
        if s[0] in "\"'":
            code.append(s)
            blank.append(s[0] + " " * (len(s) - 2) + s[0])
        else:
            gap = SPACE.sub(" ", s)
            code.append(gap)
            blank.append(gap)
        last = m.end()
    code.append(text[last:])
    blank.append(text[last:])
    return "".join(code), "".join(blank)


def match_close(text, i, open_="{", close="}"):
    """Index just past the bracket that closes the one at text[i]."""
    depth = 0
    rx = re.compile("[%s%s]" % (re.escape(open_), re.escape(close)))
    for m in rx.finditer(text, i):
        depth += 1 if m.group(0) == open_ else -1
        if depth == 0:
            return m.end()
    return len(text)


def top_blocks(blank):
    """Start of every file-scope `{ ... }` opened right after a `)` --
    function bodies, not the struct or array initialisers between them."""
    depth, out = 0, []
    for m in re.finditer(r"[{}]", blank):
        if m.group(0) == "{":
            if depth == 0 and blank[:m.start()].rstrip().endswith(")"):
                out.append(m.start())
            depth += 1
        else:
            depth = max(depth - 1, 0)
    return out


def body_of(code, blank, signature):
    m = re.compile(signature).search(blank)
    if not m:
        return None
    i = blank.index("{", m.end() - 1)
    return code[i:match_close(blank, i)]


def split_args(code, blank=None):
    """Top-level comma split of an argument list's inside; commas and
    brackets are read from `blank` so a literal's never count."""
    blank = code if blank is None else blank
    out, depth, last = [], 0, 0
    for m in re.finditer(r"[(\[{)\]},]", blank):
        ch = m.group(0)
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0:
            out.append(code[last:m.start()])
            last = m.end()
    out.append(code[last:])
    return [a.strip() for a in out]


def calls(code, blank, fname, lo=0, hi=None):
    """(start, [args]) of every call of fname in code[lo:hi] -- not its definition."""
    hi = len(blank) if hi is None else hi
    for m in re.compile(r"\b%s\s*\(" % re.escape(fname)).finditer(blank, lo, hi):
        i = m.end() - 1
        j = match_close(blank, i, "(", ")")
        if not blank[j:].lstrip().startswith("{"):
            yield m.start(), split_args(code[i + 1:j - 1], blank[i + 1:j - 1])


def unescape(lit):
    return lit[1:-1].replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


def literal_runs(code):
    """(joined value, pos) of every run of adjacent string literals in
    comment-blanked C -- what the compiler concatenates, so
    `"u_alias_" "model"` is the one string "u_alias_model"."""
    runs, cur, pos, end = [], None, 0, 0
    for m in LIT.finditer(code):
        s = m.group(0)
        if cur is not None and (s[0] == "'" or code[end:m.start()].strip()):
            runs.append((cur, pos))
            cur = None
        if s[0] == '"':
            if cur is None:
                cur, pos = "", m.start()
            cur += unescape(s)
        end = m.end()
    if cur is not None:
        runs.append((cur, pos))
    return runs


# ---------------------------------------------------------------- GLSL uniforms
def uniform_decls(text):
    """Every `uniform` in comment-stripped GLSL, as (kind, type, name):
    kind 'block' (an interface block -- not a loose uniform), 'decl' (one per
    declarator, so `uniform vec3 a, b;` and `uniform float x = 0.0;` both
    count) or 'unparsed' (anything else, e.g. a #define that builds one; only
    a failure where the text is an alias stage's)."""
    out = []
    for m in re.finditer(r"\buniform\b", text):
        rest = text[m.end():]
        b = re.match(r"\s*" + QUALS + r"(\w+)\s*\{", rest)
        if b:
            out.append(("block", "", b.group(1)))
            continue
        d = re.match(r"\s*" + QUALS + r"(\w+)\s+([^;{}#]*?);", rest, re.S)
        names = []
        if d:
            for decl in split_args(d.group(2)):
                n = re.fullmatch(r"(\w+)\s*(?:\[[^\]]*\])?\s*(?:=.*)?", decl, re.S)
                if not n:
                    names = None
                    break
                names.append(n.group(1))
        if names:
            out += [("decl", d.group(1), n) for n in names]
        else:
            out.append(("unparsed", "", " ".join(rest[:60].split())))
    return out


def expand(name, seen=None):
    """A stage file with its #includes inlined (each once)."""
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


gl_shader_c = (ROOT / "engine/h2shared/gl_shader.c").read_text()
gl_shader_h = c_views((ROOT / "engine/h2shared/gl_shader.h").read_text())[0]
gsc, gsb = c_views(gl_shader_c)

# ---------------------------------------------------------------- alias header chunks
# Every #define in gl_shader.c, every arm (both tiers): name -> [(body, pos)].
cmacros = {}
for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)(\([^)]*\))?((?:[^\n\\]|\\\n|\\.)*)", gsc, re.M):
    cmacros.setdefault(m.group(1), []).append((m.group(3).replace("\\\n", " "), m.start(3), m.end(3)))
funcs = []
for m in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{)]*)\)\s*\{", gsb, re.M):
    i = gsb.index("{", m.end() - 1)
    params = [re.findall(r"\w+", p)[-1] for p in m.group(2).split(",") if re.findall(r"\w+", p)]
    funcs.append((m.group(1), params, i, match_close(gsb, i)))


def enclosing(pos):
    return next((f for f in funcs if f[2] <= pos < f[3]), None)


hdr_spans, hdr_texts, hdr_macros, unresolved = [], [], set(), []
# The tier preambles are engine/shaders/glonly files, embedded by EmbedShaders
# as upper-case string macros (tier_es.glsl -> TIER_ES), since #450 left no
# #version in C.  They are header text like any other macro body.
GLONLY = {p.stem.upper(): p.read_text()
          for p in (ROOT / "engine/shaders/glonly").glob("*.glsl")}


def resolve(expr, pos, depth=0):
    """Collect the GLSL an expression passed as a header chunk can carry:
    its literals, macro bodies (every arm), q_snprintf'd buffers and the
    matching argument at each call of the enclosing function's parameter."""
    if depth > 12:
        unresolved.append(expr)
        return
    for v, _ in literal_runs(expr):
        hdr_texts.append(v)
    fn = enclosing(pos)
    # GLSL_STR(X) stringizes a C constant into a number; X is not GLSL.
    bare = c_views(expr)[1]
    bare = re.sub(r"\bGLSL_STR_?\s*\([^()]*\)", " ", bare)
    for ident in re.findall(r"\b[A-Za-z_]\w*\b", bare):
        if ident in cmacros:
            if ident not in hdr_macros:
                hdr_macros.add(ident)
                for body, s, e in cmacros[ident]:
                    hdr_spans.append((s, e))
                    resolve(body, s, depth + 1)
        elif ident == "NULL":
            continue
        elif ident in GLONLY:
            hdr_macros.add(ident)
            hdr_texts.append(GLONLY[ident])
        elif fn and ident in fn[1]:
            idx = fn[1].index(ident)
            sites = [(p, a) for p, a in calls(gsc, gsb, fn[0]) if not (fn[2] <= p < fn[3])]
            sites = [(p, a) for p, a in sites if len(a) == len(fn[1])]
            if not sites:
                unresolved.append(f"{ident} (parameter of {fn[0]}, no call sites)")
            for p, a in sites:
                resolve(a[idx], p, depth + 1)
        elif fn and re.search(r"\bq_snprintf\s*\(\s*%s\s*," % ident, gsb[fn[2]:fn[3]]):
            for p, a in calls(gsc, gsb, "q_snprintf", fn[2], fn[3]):
                if a[0] == ident:
                    for arg in a[3:]:
                        resolve(arg, p, depth + 1)
        else:
            unresolved.append(f"{ident} in `{' '.join(expr.split())}`")


stage_compiles = 0
for fname, pairs in (("GL_InitProgramSplit", ((2, 3), (4, 5))),
                     ("GL_InitOITProgramSplit", ((2, 3), (4, 5))),
                     ("GL_CompileShaderSplit", ((1, 2),))):
    for p, a in calls(gsc, gsb, fname):
        for h, s in pairs:
            if len(a) > s and a[s] in STAGE_SYMS:
                stage_compiles += 1
                resolve(a[h], p)
# The OIT twins compile with more than their callers' fragment header: the
# builder q_snprintf's "#define OIT 1" onto it (#450 W3).  Everything that
# buffer is built from is header text too.
oit_fn = next((f for f in funcs if f[0] == "GL_InitOITProgramSplit"), None)
if not oit_fn:
    errors.append("cannot find GL_InitOITProgramSplit; the OIT twins' header resolution is broken")
else:
    for p, a in calls(gsc, gsb, "q_snprintf", oit_fn[2], oit_fn[3]):
        for arg in a[3:]:
            resolve(arg, p)
for u in unresolved:
    errors.append(f"cannot resolve an alias stage's header chunk: {u}; teach the guard, "
                  "or it cannot see what the stage compiles with")
# Every mention of an alias stage's source symbol is one of those calls; any
# other path would compile it with a header this guard never read.
mentions = len(re.findall(r"\b(?:%s)\b" % "|".join(sorted(STAGE_SYMS)), gsb))
if mentions != stage_compiles or stage_compiles < 10:
    errors.append(f"gl_shader.c names an alias stage {mentions} times but {stage_compiles} are "
                  "recognised compile calls; a stage is compiled where the header scan cannot see")
for need in ("GLSL_VERT_HEADER", "GLSL_FRAG_HEADER", "GLSL_ALIAS_FRAG_HEADER",
             "GLSL_WORLD_FRAG_HEADER", "GLSL_ALIAS_INST_VERT_HEADER", "GLSL_LIGHTGRID_CONSTS"):
    if need not in hdr_macros:
        errors.append(f"the alias header chunks no longer include {need}; the resolution is broken")
if not any("NOPERSP" in t for t in hdr_texts):
    errors.append("the alias header chunks carry no NOPERSP define; the resolution is broken")
if not any("#define OIT 1" in t for t in hdr_texts):
    errors.append("the alias header chunks carry no OIT define; the OIT twins' resolution is broken")

hdr_decls = []
for t in hdr_texts:
    hdr_decls += uniform_decls(strip(t))

# ---------------------------------------------------------------- live loose names
in_hdr = lambda pos: any(s <= pos < e for s, e in hdr_spans)
live_loose = {}
for v, pos in literal_runs(gsc):
    if in_hdr(pos):
        continue
    for kind, typ, name in uniform_decls(strip(v)):
        if kind == "decl":
            live_loose.setdefault(name, set()).add(typ)
for f in sorted(SH.iterdir()):
    if f.suffix in (".glsl", ".inc"):
        for kind, typ, name in uniform_decls(strip(f.read_text())):
            if kind == "decl":
                live_loose.setdefault(name, set()).add(typ)
# The scan must see the loose declarations that are left: every sampler of
# both families and the OIT twins' moment samplers (oit.inc).  Since #450 W3
# those are ALL of them: no live shader declares a non-opaque loose uniform,
# and one that does is a block member that came back loose.
if not {"u_texture0", "u_lmlayer1", "u_lightgrid", "u_mboit_b0", "u_mboit_mom_ms"} <= set(live_loose):
    errors.append("cannot see the live loose declarations (samplers, oit.inc's); the scan is broken")
nonopaque = {n for n, ts in live_loose.items() if any(not OPAQUE.match(t) for t in ts)}
if nonopaque:
    errors.append(f"live shaders declare non-opaque loose uniforms {sorted(nonopaque)}; none may (#450 W3)")
if re.search(r"\boit_preamble\s*\[", gsb):
    errors.append("gl_shader.c defines an oit_preamble again; the OIT output stage is engine/shaders/oit.inc "
                  "(#450 W3), and a C literal is a place loose uniforms come back")

# ---------------------------------------------------------------- alias-family names
macros = set()
for inc in ("uniforms.inc", "alias.inc", "world.inc", "oit.inc"):
    macros |= set(re.findall(r"^\s*#\s*define\s+(u_\w+)\s", (SH / inc).read_text(), re.M))
reads = set()
stage_decls = []
for st in ALIAS_STAGES:
    text = strip((SH / st).read_text())
    for inc in re.findall(r'#\s*include\s+"([^"]+)"', text):
        if inc not in BLOCK_INCS:
            text += "\n" + strip(expand(inc))
    reads |= set(re.findall(r"\bu_\w+", text))
    stage_decls += [(st + " (includes expanded)", *d) for d in uniform_decls(strip(expand(st)))]
stage_decls += [("an alias stage's header chunk in gl_shader.c", *d) for d in hdr_decls]
alias_names = (macros & reads) - set(live_loose)
if not FLOOR <= alias_names:
    errors.append(f"derived alias-name set lost {sorted(FLOOR - alias_names)}; the derivation is broken "
                  "(or one of them is loose again somewhere -- that is the bug this guards)")

for where, kind, typ, name in stage_decls:
    if kind == "unparsed":
        errors.append(f"{where} has a `uniform` that is neither a block nor a plain declaration "
                      f"(a macro that builds one?): uniform {name}")
    elif kind == "decl" and not OPAQUE.match(typ):
        errors.append(f"{where} declares a loose non-opaque uniform: {typ} {name}")
stage_samplers = {n for _, k, t, n in stage_decls if k == "decl" and OPAQUE.match(t)}

# ---------------------------------------------------------------- struct fields
for label, rx in (("glprogram_t", r"typedef struct glprogram_s \{(.*?)\} glprogram_t;"),
                  ("gl_alias_inst_prog_t", r"typedef struct \{((?:(?!typedef).)*?)\} gl_alias_inst_prog_t;"),
                  ("gl_particle_gpu_prog_t", r"typedef struct \{((?:(?!typedef).)*?)\} gl_particle_gpu_prog_t;")):
    m = re.search(rx, gl_shader_h, re.S)
    if not m:
        errors.append(f"cannot find {label} in gl_shader.h")
        continue
    fields = set(re.findall(r"\b(u_\w+)\b", m.group(1)))
    if label == "glprogram_t":
        unread = fields - set(live_loose) - NON_ALIAS_DEAD - alias_names
        if unread:
            errors.append(f"glprogram_t has locations no live shader declares loose: {sorted(unread)}")
        stale = NON_ALIAS_DEAD - fields
        if stale:
            errors.append(f"NON_ALIAS_DEAD lists names glprogram_t no longer has, drop them: {sorted(stale)}")
        revived = NON_ALIAS_DEAD & set(live_loose)
        if revived:
            errors.append(f"NON_ALIAS_DEAD names are declared loose again, drop them: {sorted(revived)}")
        if NON_ALIAS_DEAD & alias_names:
            errors.append(f"NON_ALIAS_DEAD overlaps the alias names: {sorted(NON_ALIAS_DEAD & alias_names)}")
    if label == "gl_alias_inst_prog_t":
        nonsampler = fields - stage_samplers
        if nonsampler:
            errors.append(f"gl_alias_inst_prog_t keeps non-sampler locations: {sorted(nonsampler)}")

# ---------------------------------------------------------------- C: names, literals, writes
cfiles = sorted(p for d in ("engine/h2shared", "engine/hexen2", "engine/hexenworld")
                for p in (ROOT / d).rglob("*") if p.suffix in (".c", ".h"))
if len(cfiles) < 20:
    errors.append(f"only {len(cfiles)} C files found; the scan is broken")
WRITE = re.compile(r"\bglUniform(?!BlockBinding)\w*\s*\(")
write_ok = stage_samplers
lookup = re.compile(r'glGetUniformLocation\w*\s*\([^;]*?"(u_\w+)"')
ALIAS_RX = re.compile(r"\b(?:%s)\b" % "|".join(sorted(alias_names | STAGE_SYMS)))
for p in cfiles:
    rel = p.relative_to(ROOT)
    raw = p.read_text(errors="replace")
    code, blank = c_views(raw)
    for m in ALIAS_RX.finditer(blank):
        line = raw.count("\n", 0, m.start()) + 1
        if m.group(0) in STAGE_SYMS:
            if p.name != "gl_shader.c":
                errors.append(f"{rel}:{line}: names the alias stage {m.group(0)}; only gl_shader.c's "
                              "compile calls, whose header chunks this guard reads, may")
        else:
            errors.append(f"{rel}:{line}: names {m.group(0)} in C -- a location, field or stringized "
                          "lookup for a block member no program declares loose")
    for v, pos in literal_runs(code):
        if v in alias_names:
            line = raw.count("\n", 0, pos) + 1
            errors.append(f"{rel}:{line}: the string \"{v}\" -- a uniform lookup of a block member "
                          "no program declares loose")
    # Alias-program handles: the globals, any function returning one, and any
    # local or array ever assigned one, per function.
    alias_fns = {m.group(1) for m in re.finditer(r"\*\s*(\w+)\s*\([^)]*\)\s*\{", blank)
                 if re.search(r"\breturn\b[^;]*\b" + ALIAS_PROG,
                              blank[m.end() - 1:match_close(blank, m.end() - 1)])}
    handle = "|".join([ALIAS_PROG] + sorted(alias_fns))
    for start in top_blocks(blank):
        body = blank[start:match_close(blank, start)]
        if not WRITE.search(body):
            continue
        tainted = set(re.findall(r"\b(\w+)\s*(?:\[[^\]]*\])?\s*=\s*(?:\{[^;]*|[^;{]*)\b(?:%s)\b" % handle,
                                 body))
        obj = "|".join([ALIAS_PROG] + [re.escape(x) for x in sorted(tainted | alias_fns)])
        target = re.compile(r"\b(?:%s)\b\s*(?:\([^()]*\))?\s*(?:\[[^\]]*\])?\s*(?:\.|->)\s*(u_\w+)" % obj)
        for w in WRITE.finditer(body):
            i = w.end() - 1
            loc = split_args(body[i + 1:match_close(body, i, "(", ")") - 1])[0]
            for f in target.findall(loc):
                if f not in write_ok:
                    errors.append(f"{rel}: glUniform* on an alias-family program's {f} (`{loc}`); "
                                  "its stages read blocks")
    for m in lookup.finditer(code):
        if m.group(1) in alias_names:
            errors.append(f"{rel}: looks up the loose {m.group(1)}, a block member on every alias stage")

vbo = c_views((ROOT / "engine/h2shared/gl_vbo.c").read_text())[1]
tied = set()
for m in re.finditer(r"\bif\s*\(", vbo):
    i = m.end() - 1
    j = match_close(vbo, i, "(", ")")
    cond = vbo[i:j]
    if not set(re.findall(r"\bshader->(u_\w+)", cond)) & (set(live_loose) | NON_ALIAS_DEAD):
        continue
    k = j + len(vbo[j:]) - len(vbo[j:].lstrip())
    stmt_end = match_close(vbo, k) if vbo[k:k + 1] == "{" else vbo.index(";", k) + 1
    tied |= set(re.findall(r"\bimm_cache_\w+", cond + vbo[k:stmt_end]))
for cache in sorted(set(re.findall(r"\bimm_cache_(\w+)\b", vbo))):
    hit = sorted(n for n in alias_names if n[2:] in cache)
    if hit or re.match(r"(?:alias|soft)", cache):
        errors.append(f"gl_vbo.c keeps imm_cache_{cache}, a per-program cache for "
                      f"{hit[0] if hit else 'an alias-family value'}, which has no loose copy")
    elif cache != "shader" and "imm_cache_" + cache not in tied:
        errors.append(f"gl_vbo.c keeps imm_cache_{cache}, which no `if (shader->u_X ...)` on a "
                      "live loose location reads or writes; a cache for nothing a program has loose")

# ---------------------------------------------------------------- GL_InitProgramUniforms / instanced
gipu = body_of(gsc, gsb, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or ""
if not gipu:
    errors.append("cannot find GL_InitProgramUniforms")
plain = re.compile(r'p->(u_\w+)\s*=\s*glGetUniformLocation_fp\s*\(\s*p->program\s*,\s*"(u_\w+)"\s*\)\s*;')
gipu_names = set()
for stmt in re.findall(r"[^;{}]*\bglGetUniformLocation\w*\s*\([^;]*;", gipu):
    m = plain.fullmatch(stmt.strip())
    if not m or m.group(1) != m.group(2):
        errors.append(f"GL_InitProgramUniforms: `{' '.join(stmt.split())}` is not "
                      "`p->u_x = glGetUniformLocation_fp(p->program, \"u_x\");`")
        continue
    gipu_names.add(m.group(1))
    if m.group(1) not in set(live_loose) | NON_ALIAS_DEAD:
        errors.append(f"GL_InitProgramUniforms looks up {m.group(1)}, which no live shader declares loose")
if gipu and len(gipu_names) < 14:
    errors.append(f"GL_InitProgramUniforms: only {len(gipu_names)} lookups seen; the scan is broken")
inst = body_of(gsc, gsb, r"static qboolean GL_InitAliasInstProgram \([^)]*\)\s*\{") or ""
if not inst:
    errors.append("cannot find GL_InitAliasInstProgram")
for name in lookup.findall(inst):
    if name not in stage_samplers:
        errors.append(f"GL_InitAliasInstProgram looks up {name}, which no alias stage declares as a sampler")

if errors:
    print("test_alias_no_loose: FAIL")
    for e in dict.fromkeys(errors):
        print("  -", e)
    sys.exit(1)
print(f"test_alias_no_loose: OK ({len(alias_names)} alias block names, {len(live_loose)} live loose names, "
      f"{len(hdr_macros)} alias header macros, {len(cfiles)} C files)")
if "-v" in sys.argv:
    print("  alias block names:", " ".join(sorted(alias_names)))
    print("  alias header macros:", " ".join(sorted(hdr_macros)))
