#!/usr/bin/env python3
"""The worldcull compute family rides one CullParams block (#19).

After the slice, engine/h2shared/gl_worldcull.c must carry no loose uniform
uploads or location queries for the three compute programs: the only
glUniform value uploads left are the four R_DrawWorldCulled world-fragment
writes (the world family's fragment stages, a later slice; its matrices
moved onto VertParams in #450 W1), glGetUniformLocation_fp is gone
entirely, and the C mirror must agree with the GLSL block member for
member -- the std140 array-stride error this catches compiles clean and
renders wrong, so it is checked here, not on screen.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []

WORLDCULL = ROOT / "engine/h2shared/gl_worldcull.c"
src = WORLDCULL.read_text()

# --- 1. The GLSL block: one declaration, in cull.inc, used by all three ----
# Since #450 the three programs are engine/shaders files that #include one
# copy, so "the copies agree" now means "there is only one copy".
SHADERS = ROOT / "engine/shaders"
CULL_INC = SHADERS / "cull.inc"
STAGES = ("cull_clear_comp.glsl", "cull_mark_comp.glsl", "hiz_reduce_comp.glsl")
BLOCK_RE = r"uniform\s+CullParams\s*\{(.*?)\};"
inc = CULL_INC.read_text() if CULL_INC.exists() else ""
blocks = re.findall(BLOCK_RE, inc, re.S)
if len(blocks) != 1:
    errors.append(f"cull.inc declares CullParams {len(blocks)} times, expected 1")
block = blocks[0] if blocks else ""
if not re.search(r"^UBO\(0\)\s+uniform\s+CullParams\b", inc, re.M):
    errors.append("cull.inc's CullParams is not UBO(0) (the SDL_GPU slot; "
                  "GL's point 4 is assigned from C)")
def uncommented(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))

for stage in STAGES:
    text = (SHADERS / stage).read_text() if (SHADERS / stage).exists() else ""
    if not re.search(r'^#include "cull\.inc"\s*$', uncommented(text), re.M):
        errors.append(f"{stage} does not #include \"cull.inc\"")
# Exactly one CullParams declaration across every shader source, so a second
# copy cannot reappear in some other stage or include.
copies = [p.name for p in sorted(SHADERS.glob("*"))
          if p.suffix in (".glsl", ".inc")
          for _ in re.findall(BLOCK_RE, uncommented(p.read_text()), re.S)]
if copies != ["cull.inc"]:
    errors.append(f"CullParams is declared in {copies}, expected only cull.inc")
if re.search(r"uniform CullParams", src):
    errors.append("gl_worldcull.c still carries an inline CullParams copy")

# --- 2. std140 offsets of the GLSL members --------------------------------
# All members are vec4/mat4/ivec4, so offsets are arithmetic: every member
# is 16-byte aligned, mat4 is 4 vec4 columns, vec4[4] is 4 * 16 bytes.
GLSL_TYPE = {"ivec4": (16, 16), "vec4": (16, 16), "mat4": (16, 64)}


def glsl_members(text):
    members = []
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    for line in text.splitlines():
        line = line.replace("\\n", "").strip().rstrip(";").strip()
        m = re.match(r"(ivec4|vec4|mat4)\s+(\w+)(\[(\d+)\])?$", line)
        if m:
            typ, name, _, count = m.groups()
            align, size = GLSL_TYPE[typ]
            members.append((name, typ, int(count) if count else 1, align, size))
    return members


glsl = glsl_members(block)
off = 0
glsl_offsets = {}
for name, typ, count, align, size in glsl:
    if off % align:
        errors.append(f"GLSL member {name} misaligned at {off}")
    glsl_offsets[name] = off
    off += size * count
if off != 224:
    errors.append(f"CullParams GLSL size is {off} bytes, expected 224")

# --- 3. C mirror: same members, same offsets -------------------------------
m = re.search(r"typedef struct \{([^}]*)\} cull_params_t;", src, re.S)
if not m:
    errors.append("no cull_params_t mirror in gl_worldcull.c")
    c_members = []
else:
    c_members = []
    for line in m.group(1).splitlines():
        line = line.split("/*")[0].strip().rstrip(";").strip()
        mm = re.match(r"(GLint|float)\s+(\w+)\[(\d+)\](\[(\d+)\])?$", line)
        if mm:
            typ, name, n1, _, n2 = mm.groups()
            count = int(n1) * (int(n2) if n2 else 1)
            c_members.append((name, typ, count))

C_NAME = {"u_ccounts0": "counts0", "u_ccounts1": "counts1",
          "u_cvieworg": "vieworg", "u_cfrustum": "frustum",
          "u_cprevmvp": "prevmvp", "u_chizsize": "hizsize",
          "u_creduce": "reduce", "u_creduce2": "reduce2"}
off = 0
c_offsets = {}
for name, typ, count in c_members:
    # Every member is 16 or 64 bytes of 4-byte scalars, so natural C
    # layout agrees with std140 exactly; assert the arithmetic anyway.
    size = 4 * count
    if size not in (16, 64):
        errors.append(f"C member {name} is {size} bytes, not a std140 unit")
    c_offsets[name] = off
    off += size
if off != 224:
    errors.append(f"cull_params_t is {off} bytes, expected 224")
if "sizeof(cull_params_t) == 224" not in src:
    errors.append("no COMPILE_TIME_ASSERT pinning sizeof(cull_params_t) == 224")

for gname, goff in glsl_offsets.items():
    cname = C_NAME.get(gname)
    if cname is None:
        errors.append(f"GLSL member {gname} has no C mirror field")
    elif c_offsets.get(cname) != goff:
        errors.append(f"{gname}: GLSL offset {goff}, C offset "
                      f"{c_offsets.get(cname)}")

# --- 4. Binding point 4, and the three comments agree ----------------------
if not re.search(r"#define CULL_PARAMS_UBO_BINDING\s+4\b", src):
    errors.append("CULL_PARAMS_UBO_BINDING is not 4")
for path, want in (
        ("engine/h2shared/gl_shader.h", "4 is its CullParams"),
        ("engine/h2shared/gl_ssao.c", "4 CullParams")):
    if want not in (ROOT / path).read_text():
        errors.append(f"{path}: binding comment does not list 4 as CullParams")

# --- 5. No loose uploads or location queries left --------------------------
# Strip comments and string literals so shader-body text cannot trip the
# gate; the block declaration itself holds no glUniform calls.
code = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
code = re.sub(r"//[^\n]*", " ", code)
code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)

uploads = re.findall(r"glUniform(?:1f|1i|2f|2i|3f|4f|3fv|4fv|Matrix4fv)_fp\s*\(",
                     code)
if len(uploads) != 4:
    errors.append(f"{len(uploads)} loose glUniform uploads in "
                  "gl_worldcull.c, expected the 4 R_DrawWorldCulled sites")
draw = re.search(r"void R_DrawWorldCulled \(void\)\s*\{(.*?)\n\}", code, re.S)
if not draw:
    errors.append("no R_DrawWorldCulled body found")
elif len(re.findall(r"glUniform(?:1f|1i|2f|2i|3f|4f|3fv|4fv|Matrix4fv)_fp\s*\(",
                    draw.group(1))) != 4:
    errors.append("the 4 remaining uploads are not all in R_DrawWorldCulled")
if "glGetUniformLocation_fp" in code:
    errors.append("glGetUniformLocation_fp still used in gl_worldcull.c")
# The block bindings are plumbing, not uploads; both calls must still
# exist (the bare name also appears in the two null guards -- count calls).
if len(re.findall(r"glUniformBlockBinding_fp\s*\(", code)) != 2:
    errors.append("expected 2 glUniformBlockBinding_fp calls "
                  "(CullParams helper + hiz_copy)")
# No loose uniform name from the old bodies may survive anywhere, shader
# bodies included -- so this checks the raw source, not the stripped code.
for stale in ("u_vieworg", "u_frustum", "u_framecount", "u_num_marksurfs",
              "u_num_buckets", "u_max_dst_indices", "u_prev_mvp", "u_hiz_size",
              "u_hiz_mip_count", "u_hiz_enable", "u_reverse_z", "u_stats_enable",
              "u_src_size", "u_dst_size", "u_src_lod"):
    for name, text in [("gl_worldcull.c", src), ("cull.inc", inc)] + [
            (s, (SHADERS / s).read_text()) for s in STAGES if (SHADERS / s).exists()]:
        if re.search(rf"\b{stale}\b", text):
            errors.append(f"stale loose uniform name {stale} in {name}")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS CullParams block covers the worldcull compute family (#19)")
