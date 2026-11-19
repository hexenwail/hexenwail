#!/usr/bin/env python3
"""The light-cluster pass takes its parameters through one LcParams block (#450).

When lc_compute_body moved into engine/shaders/lightcluster_comp.glsl, its
five loose uniforms (illegal in Vulkan SPIR-V) became an std140 block.  This
checks the block and its C mirror lc_params_t agree member for member: an
offset mismatch compiles clean and clusters wrong, which only
`lightcluster_verify` on a live engine would otherwise show.  Also: no loose
uniform location queries or uploads are left for the compute program, the
binding point is 5 on the C side and in the registry comment, and the GLSL
names the old loose uniforms only as aliases into the block.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "engine/shaders/lightcluster_comp.glsl"
CSRC = ROOT / "engine/h2shared/gl_lightcluster.c"
errors = []

glsl = SHADER.read_text() if SHADER.exists() else ""
csrc = CSRC.read_text()


def strip_comments(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


# --- 1. the GLSL block -----------------------------------------------------
blocks = re.findall(r"uniform\s+LcParams\s*\{(.*?)\};", strip_comments(glsl), re.S)
if len(blocks) != 1:
    errors.append(f"LcParams declared {len(blocks)} times in {SHADER.name}, expected 1")
SIZES = {"mat4": 64, "vec4": 16, "ivec4": 16}
glsl_members = []
for line in (blocks[0] if blocks else "").splitlines():
    m = re.match(r"\s*(mat4|vec4|ivec4)\s+(\w+)\s*;", line)
    if m:
        glsl_members.append((m.group(2), SIZES[m.group(1)]))
glsl_off, off = {}, 0
for name, size in glsl_members:
    glsl_off[name] = off
    off += size
if off != 160:
    errors.append(f"LcParams GLSL size is {off} bytes, expected 160")

# --- 2. the C mirror --------------------------------------------------------
m = re.search(r"typedef struct \{([^}]*)\} lc_params_t;", csrc, re.S)
c_off, off = {}, 0
if not m:
    errors.append("no lc_params_t in gl_lightcluster.c")
else:
    for line in strip_comments(m.group(1)).splitlines():
        mm = re.match(r"\s*(float|GLint)\s+(\w+)\[(\d+)\]\s*;", line)
        if mm:
            c_off[mm.group(2)] = off
            off += 4 * int(mm.group(3))
    if off != 160:
        errors.append(f"lc_params_t is {off} bytes, expected 160")
if "sizeof(lc_params_t) == 160" not in csrc:
    errors.append("no COMPILE_TIME_ASSERT pinning sizeof(lc_params_t) == 160")

PAIRS = {"u_view": "view", "u_transposed_proj": "tproj",
         "u_lczlog": "zlog", "u_lccount": "count"}
for g, c in PAIRS.items():
    if g not in glsl_off:
        errors.append(f"GLSL LcParams lacks {g}")
    elif c_off.get(c) != glsl_off[g]:
        errors.append(f"{g}: GLSL offset {glsl_off[g]}, C {c} offset {c_off.get(c)}")
extra = set(glsl_off) - set(PAIRS)
if extra:
    errors.append(f"GLSL LcParams members with no C mirror: {sorted(extra)}")

# --- 3. the aliases keep the body's old names pointing into the block -------
for name, target in (("u_zlogscale", "u_lczlog.x"), ("u_zlogbias", "u_lczlog.y"),
                     ("u_numlights", "u_lccount.x")):
    if not re.search(rf"#define\s+{name}\s+{re.escape(target)}\b", glsl):
        errors.append(f"{name} is not aliased to {target}")
    if re.search(rf"uniform\s+\w+\s+{name}\s*;", strip_comments(glsl)):
        errors.append(f"{name} is still a loose uniform")

# --- 4. C side: binding 5, no loose plumbing left for this program ----------
code = strip_comments(csrc)
code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)
if not re.search(r"#define LC_PARAMS_UBO_BINDING\s+5\b", csrc):
    errors.append("LC_PARAMS_UBO_BINDING is not 5")
if "5 is\n * gl_lightcluster.c's LcParams" not in (ROOT / "engine/h2shared/gl_shader.h").read_text():
    errors.append("gl_shader.h's binding registry does not list 5 as LcParams")
if "glGetUniformLocation_fp" in code:
    errors.append("gl_lightcluster.c still queries a uniform location")
if "lc_compute_body" in code:
    errors.append("the inline lc_compute_body is back in gl_lightcluster.c")
# The program is gl_shader.c's RPROG_LIGHTCLUSTER row (#340 P1b), built by id.
if "lc_prog = R_Program_Create (RPROG_LIGHTCLUSTER);" not in code:
    errors.append("gl_lightcluster.c does not build RPROG_LIGHTCLUSTER")
if not re.search(r'\[RPROG_LIGHTCLUSTER\] = \{ "lightcluster", RPROG_LINK_COMPUTE, \{\s*'
                 r'\{ GLSL_LIGHTCLUSTER_HEADER, lightcluster_comp, "lightcluster.comp" \} \} \},',
                 (ROOT / "engine/h2shared/gl_shader.c").read_text()):
    errors.append("gl_shader.c's RPROG_LIGHTCLUSTER row does not compile lightcluster_comp")
disp = re.search(r"static void LC_UploadAndDispatch \(void\)\s*\{(.*?)\n\}", code, re.S)
if not disp:
    errors.append("no LC_UploadAndDispatch body")
elif re.search(r"glUniform\w*_fp\s*\(", disp.group(1)):
    errors.append("LC_UploadAndDispatch still uploads a loose uniform")
elif "LC_PARAMS_UBO_BINDING" not in disp.group(1):
    errors.append("LC_UploadAndDispatch does not bind LcParams")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS LcParams carries the light-cluster pass's parameters, mirrored in C (#450)")
