#!/usr/bin/env python3
"""No alias-family program regains a loose non-opaque uniform (#450, alias S5).

After slices S1-S4 all four alias stages -- salias_vert, salias_frag,
salias_inst_vert, sskeletal_vert -- read every non-opaque value from a std140
block (VertParams / FragParams in uniforms.inc, AliasVert / AliasFrag in
alias.inc), reaching the old loose names through `#define u_x <member>`
macros.  A glGetUniformLocation on such a name returns -1 on every program, so
the C plumbing that looked it up, cached it and glUniform*'d it was dead, and
S5 removed it.  This keeps it removed.

The alias names are DERIVED, not listed: every block macro an alias stage (or
a helper .inc it pulls in) actually reads, minus every name that some live
shader -- any engine/shaders file, or any string literal in gl_shader.c, both
tiers -- still declares as a loose `uniform`.  Those shared names (u_turb,
u_spans, u_mvp, ...) are the world programs' and legitimately keep their
loose path.  For each derived alias name:
  * no glprogram_t / gl_alias_inst_prog_t / gl_particle_gpu_prog_t location
    field carries it;
  * no C file looks it up (glGetUniformLocation*) or glUniform*s a location
    expression that names it;
  * GL_ImmEnd keeps no per-program "last value" cache for it.
And for the alias-family programs themselves:
  * the stage files (with every #include expanded) declare no loose
    non-opaque uniform;
  * GL_InitAliasInstProgram looks up only samplers the stages declare;
  * no glUniform* call anywhere targets a field of gl_shader_alias*,
    gl_shader_skeletal* or gl_shader_alias_inst* other than a sampler.
Every glprogram_t location field must name a uniform some live shader still
declares loose, unless it is in NON_ALIAS_DEAD -- dead plumbing the migrated
2d / sky programs left behind, outside the alias family's scope; that list
fails when an entry goes stale, so it can only shrink.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "engine/shaders"
errors = []

ALIAS_STAGES = ("salias_vert.glsl", "salias_frag.glsl",
                "salias_inst_vert.glsl", "sskeletal_vert.glsl")
# The .inc files that DECLARE blocks (or the per-backend layout macros).  An
# alias stage includes them, but the macro list inside is not a read.
BLOCK_INCS = ("uniforms.inc", "alias.inc", "layout.inc")
# The six an S5 sanity floor expects to derive; if parsing ever breaks and the
# derived set collapses, the guard must fail rather than pass vacuously.
FLOOR = {"u_alias_dlight", "u_alias_caustics", "u_alias_material",
         "u_alias_lightdir", "u_alias_model", "u_soft_params", "u_eyepos"}
# Loose-lookup fields with no live declarer that are NOT alias-family names:
# s2d's u_color and the sky programs' u_time / u_skyfog / u_wind / u_wind3 /
# u_skyrot, all block members since their files moved.  Out of scope for #450's
# alias slices; listed so their deadness is explicit, not silent.
NON_ALIAS_DEAD = {"u_color", "u_time", "u_skyfog", "u_wind", "u_wind3", "u_skyrot"}
OPAQUE = re.compile(r"^(?:[iu]?sampler\w*|[iu]?image\w*)$")
ALIAS_PROGS = re.compile(r"\bgl_shader_(?:alias|skeletal)\w*\s*(?:\.|->)\s*(u_\w+)")


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


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


def loose_decls(text):
    """(type, name) of every loose `uniform` declaration -- not block members,
    which carry no `uniform` keyword of their own."""
    return re.findall(r"\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;",
                      text)


gl_shader_c = (ROOT / "engine/h2shared/gl_shader.c").read_text()
gl_shader_h = strip((ROOT / "engine/h2shared/gl_shader.h").read_text())
gsc = strip(gl_shader_c)

# ---------------------------------------------------------------- live loose names
literals = "".join(m.group(1) for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', gsc))
literals = literals.replace("\\n", "\n").replace('\\"', '"')
live_loose = {}
for typ, name in loose_decls(literals):
    live_loose.setdefault(name, set()).add(typ)
for f in sorted(SH.iterdir()):
    if f.suffix in (".glsl", ".inc"):
        for typ, name in loose_decls(strip(f.read_text())):
            live_loose.setdefault(name, set()).add(typ)
if "u_turb" not in live_loose or "u_mvp" not in live_loose:
    errors.append("cannot see sworld_*'s loose uniforms in gl_shader.c's literals; the scan is broken")

# ---------------------------------------------------------------- alias-family names
macros = set()
for inc in ("uniforms.inc", "alias.inc"):
    macros |= set(re.findall(r"^\s*#\s*define\s+(u_\w+)\s", (SH / inc).read_text(), re.M))
reads = set()
stage_decls = []
for st in ALIAS_STAGES:
    text = strip((SH / st).read_text())
    for inc in re.findall(r'#\s*include\s+"([^"]+)"', text):
        if inc not in BLOCK_INCS:
            text += "\n" + strip(expand(inc))
    reads |= set(re.findall(r"\bu_\w+", text))
    stage_decls += [(st, t, n) for t, n in loose_decls(strip(expand(st)))]
alias_names = (macros & reads) - set(live_loose)
if not FLOOR <= alias_names:
    errors.append(f"derived alias-name set lost {sorted(FLOOR - alias_names)}; the derivation is broken "
                  "(or one of them is loose again somewhere -- that is the bug this guards)")

for st, typ, name in stage_decls:
    if not OPAQUE.match(typ):
        errors.append(f"{st} (includes expanded) declares a loose non-opaque uniform: {typ} {name}")
stage_samplers = {n for _, t, n in stage_decls if OPAQUE.match(t)}

# ---------------------------------------------------------------- struct fields
for label, rx in (("glprogram_t", r"typedef struct glprogram_s \{(.*?)\} glprogram_t;"),
                  ("gl_alias_inst_prog_t", r"typedef struct \{((?:(?!typedef).)*?)\} gl_alias_inst_prog_t;"),
                  ("gl_particle_gpu_prog_t", r"typedef struct \{((?:(?!typedef).)*?)\} gl_particle_gpu_prog_t;")):
    m = re.search(rx, gl_shader_h, re.S)
    if not m:
        errors.append(f"cannot find {label} in gl_shader.h")
        continue
    fields = set(re.findall(r"\b(u_\w+)\b", m.group(1)))
    bad = fields & alias_names
    if bad:
        errors.append(f"{label} carries alias-family locations that are -1 on every program: {sorted(bad)}")
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

# ---------------------------------------------------------------- C lookups and writes
cfiles = sorted(p for d in ("engine/h2shared", "engine/hexen2", "engine/hexenworld")
                for p in (ROOT / d).rglob("*") if p.suffix in (".c", ".h"))
if len(cfiles) < 20:
    errors.append(f"only {len(cfiles)} C files found; the scan is broken")
lookup = re.compile(r'glGetUniformLocation\w*\s*\([^;]*?"(u_\w+)"')
write = re.compile(r"\bglUniform(?!BlockBinding)\w*\s*\(\s*([^,;]+),")
for p in cfiles:
    rel = p.relative_to(ROOT)
    code = strip(p.read_text(errors="replace"))
    for m in lookup.finditer(code):
        if m.group(1) in alias_names:
            errors.append(f"{rel}: looks up the loose {m.group(1)}, a block member on every alias stage")
    for m in write.finditer(code):
        loc = m.group(1)
        named = set(re.findall(r"\bu_\w+", loc))
        if named & alias_names:
            errors.append(f"{rel}: glUniform* writes {sorted(named & alias_names)}, which no program declares loose")
        for f in ALIAS_PROGS.findall(loc):
            if f not in stage_samplers:
                errors.append(f"{rel}: glUniform* on an alias-family program's {f}; its stages read blocks")
    for m in ALIAS_PROGS.finditer(code):
        if m.group(1) in alias_names:
            errors.append(f"{rel}: uses {m.group(0)}, a location the program cannot have")

vbo = strip((ROOT / "engine/h2shared/gl_vbo.c").read_text())
for cache in sorted(set(re.findall(r"\bimm_cache_(\w+)\b", vbo))):
    hit = sorted(n for n in alias_names if n[2:] in cache)
    if hit or re.match(r"(?:alias|soft)", cache):
        errors.append(f"gl_vbo.c keeps imm_cache_{cache}, a per-program cache for "
                      f"{hit[0] if hit else 'an alias-family value'}, which has no loose copy")

# ---------------------------------------------------------------- GL_InitProgramUniforms / instanced
gipu = body_of(gsc, r"static void GL_InitProgramUniforms \(glprogram_t \*p\)\s*\{") or ""
if not gipu:
    errors.append("cannot find GL_InitProgramUniforms")
inst = body_of(gsc, r"static qboolean GL_InitAliasInstProgram \([^)]*\)\s*\{") or ""
if not inst:
    errors.append("cannot find GL_InitAliasInstProgram")
for name in lookup.findall(inst):
    if name not in stage_samplers:
        errors.append(f"GL_InitAliasInstProgram looks up {name}, which no alias stage declares as a sampler")

if errors:
    print("test_alias_no_loose: FAIL")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print(f"test_alias_no_loose: OK ({len(alias_names)} alias block names, {len(live_loose)} live loose names, "
      f"{len(cfiles)} C files)")
if "-v" in sys.argv:
    print("  alias block names:", " ".join(sorted(alias_names)))
