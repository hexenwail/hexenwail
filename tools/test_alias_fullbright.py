#!/usr/bin/env python3
"""r_fullbright flattens alias model lighting on every alias path (#490).

Under r_fullbright the world draws its unlit texture at 1x (#489).  Alias
models have to do the same: Ironwail sets lightcolor to 0.5 under
r_fullbright and its vertex shader doubles it (Quake/r_alias.c:640-641,
Quake/gl_shaders.h:1158-1160).  No alias shader here doubles its light, so the
flat value is 1.0 after the 0..255 -> shader-range scale.

The light reaches the GPU along exactly two setups and four consumers:

  setups     R_DrawAliasModel        every non-instanced draw: MDL, MD3,
                                     skeletal (MD5/IQM) and the viewmodel
             R_CollectAliasInstance  r_alias_gpu 1, one InstanceData each
  consumers  GL_DrawAliasFrame       MDL vertex colours  (via R_SetupAliasFrame)
             GL_DrawAliasFrameMD3    MD3 vertex colours  (via GL_DrawAliasFrame)
             GL_DrawAliasSkeletal    AliasVert u_avlight (via GL_DrawAliasFrame
                                     or R_SetupAliasFrame)
             inst->light_color       in R_CollectAliasInstance itself

Checked:
  * R_AliasFinishLight is defined once, in gl_rmain.c, and under
    r_fullbright.integer sets lightcolor to 1.0f on all three channels,
    clears model_dlight_gpu (no froxel dynamic light on that model) and
    returns before R_AliasLightScale's scale; otherwise it scales.
  * R_AliasLightScale is called only by R_AliasFinishLight and
    R_AliasDlightScale, so nothing rescales lightcolor around the gate.
  * Each setup calls R_AliasFinishLight exactly once, and after that call
    nothing in the setup writes lightcolor (assignment, compound assignment,
    VectorScale/Copy/MA/Add/Subtract into it, or R_LightPoint* /
    AliasModelGetLightInfo, which overwrite it) or sets model_dlight_gpu;
    the consumers' inputs (R_SetupAliasFrame / inst->light_color and the
    dlight bit / GL_SetAliasDlight) all come after it.
  * Each consumer reads lightcolor[0], [1] and [2] and calls neither
    R_AliasLightScale nor R_LightPoint*: its light is the setup's.
  * The consumers are reached only from the setups: GL_DrawAliasFrame and
    GL_DrawAliasFrameMD3 / GL_DrawAliasSkeletal only from R_SetupAliasFrame /
    GL_DrawAliasFrame, R_SetupAliasFrame only from R_DrawAliasModel, and
    R_CollectAliasInstance's InstanceData is the only light_color writer
    other than the gl_fullbrights glow redraw's constant 1.0f, which itself
    stands down under r_fullbright.
  * Every client function that reads lightcolor is one of the above or a
    listed non-alias reader, so a new alias path with its own light fails
    until it is classified here.
  * R_LightCluster_AliasActive stays gated on !r_fullbright.integer, the
    second lock on froxel dynamic light for the instanced draw's scale.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "engine"
errors = []


def strip(t):
    t = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), t, flags=re.S)
    t = re.sub(r"//[^\n]*", " ", t)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', t)


def functions(text):
    """{name: [body, ...]} for every top-level definition in stripped C text.
    A list, because a function may be defined once per #if arm."""
    out = {}
    for m in re.finditer(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*\{", text, re.M):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        i = m.end() - 1
        depth = 0
        for j in range(i, len(text)):
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            if depth == 0:
                out.setdefault(name, []).append(text[i:j + 1])
                break
    return out


def block_after(body, pattern):
    m = re.search(pattern, body)
    if not m:
        return None
    i = body.index("{", m.end())
    depth = 0
    for j in range(i, len(body)):
        depth += {"{": 1, "}": -1}.get(body[j], 0)
        if depth == 0:
            return body[i:j + 1]
    return None


SOURCES = sorted(p for d in ("hexen2", "h2shared") for p in (ENGINE / d).glob("*.c"))
stripped = {p: strip(p.read_text(errors="replace")) for p in SOURCES}
RMAIN = ENGINE / "hexen2/gl_rmain.c"
rmain = stripped[RMAIN]
fns = functions(rmain)
allfns = {}
for p, t in stripped.items():
    for n, bodies in functions(t).items():
        allfns.setdefault(n, []).extend((p, b) for b in bodies)


def one(name):
    b = fns.get(name)
    if not b:
        errors.append(f"gl_rmain.c: no {name}")
        return ""
    if len(b) != 1:
        errors.append(f"gl_rmain.c: {name} is defined {len(b)} times")
    return b[0]


def callers(callee):
    """Names of every client function whose body calls `callee`."""
    pat = re.compile(rf"\b{callee}\s*\(")
    return sorted({n for n, lst in allfns.items() for _, b in lst if n != callee and pat.search(b)})


LC_WRITE = re.compile(
    r"\blightcolor\s*\[[^\]]*\]\s*(?:[-+*/]?=)(?!=)"
    r"|\b(?:VectorScale|VectorCopy|VectorMA|VectorAdd|VectorSubtract)\s*\([^;]*,\s*lightcolor\s*\)"
    r"|\b(?:R_LightPoint\w*|AliasModelGetLightInfo)\s*\(")

# ------------------------------------------------------------ the helper
defs = [(p, n) for n, lst in allfns.items() if n == "R_AliasFinishLight" for p, _ in lst]
if [p for p, _ in defs] != [RMAIN]:
    errors.append(f"R_AliasFinishLight must be defined exactly once, in gl_rmain.c (found {[str(p.relative_to(ROOT)) for p, _ in defs]})")
if not re.search(r"^static void R_AliasFinishLight \(void\)\s*\{", rmain, re.M):
    errors.append("R_AliasFinishLight must be `static void R_AliasFinishLight (void)`")
helper = one("R_AliasFinishLight")
fb = block_after(helper, r"\bif\s*\(\s*r_fullbright\.integer\s*\)") if helper else None
if fb is None:
    errors.append("R_AliasFinishLight: no `if (r_fullbright.integer)` block")
else:
    flat = re.search(r"lightcolor\[0\]\s*=\s*lightcolor\[1\]\s*=\s*lightcolor\[2\]\s*=\s*([0-9.]+)f?\s*;", fb)
    if not flat:
        errors.append("R_AliasFinishLight's r_fullbright arm must set lightcolor[0] = lightcolor[1] = lightcolor[2]")
    elif float(flat.group(1)) != 1.0:
        errors.append(f"R_AliasFinishLight's r_fullbright light is {flat.group(1)}, not 1.0: no alias shader doubles "
                      "its light, so 1.0 is the unlit skin at 1x (Ironwail's 0.5 is pre-doubling)")
    if not re.search(r"\bmodel_dlight_gpu\s*=\s*false\s*;", fb):
        errors.append("R_AliasFinishLight's r_fullbright arm must clear model_dlight_gpu (no froxel dlight under r_fullbright)")
    if re.search(r"\bR_AliasLightScale\s*\(", fb) or re.search(r"\bVectorScale\s*\(", fb):
        errors.append("R_AliasFinishLight's r_fullbright arm must not scale: the flat light is final")
    if not re.search(r"\breturn\s*;", fb):
        errors.append("R_AliasFinishLight's r_fullbright arm must return before the scale")
    rest = helper[helper.index(fb) + len(fb):]
    if not re.search(r"\bR_AliasLightScale\s*\(\s*\)", rest) or \
            not re.search(r"\bVectorScale\s*\(\s*lightcolor\s*,\s*lightscale\s*,\s*lightcolor\s*\)", rest):
        errors.append("R_AliasFinishLight must scale lightcolor by R_AliasLightScale () after the r_fullbright arm")
    if helper.index(fb) > (helper.find("R_AliasLightScale") if "R_AliasLightScale" in helper else len(helper)):
        errors.append("R_AliasFinishLight must test r_fullbright before it scales")

scale_callers = callers("R_AliasLightScale")
if scale_callers != ["R_AliasDlightScale", "R_AliasFinishLight"]:
    errors.append(f"R_AliasLightScale is called by {scale_callers}; only R_AliasFinishLight (lightcolor) and "
                  "R_AliasDlightScale (the froxel scale) may, or a path scales its light around the r_fullbright gate")

# ------------------------------------------------------------ the two setups
SETUPS = {
    "R_DrawAliasModel": [r"\bR_SetupAliasFrame\s*\(", r"\bGL_SetAliasDlight\s*\(\s*R_AliasDlightScale\s*\(\s*model_dlight_gpu\s*\)\s*\)"],
    "R_CollectAliasInstance": [r"\binst->light_color\b", r"\bVectorCopy\s*\(\s*lightcolor\s*,\s*inst->light_color\s*\)",
                               r"\bif\s*\(\s*model_dlight_gpu\s*\)\s*inst->shadedot_row\s*\|=\s*ALIAS_INST_DLIGHT_BIT"],
}
for name, after in SETUPS.items():
    body = one(name)
    calls = [m.start() for m in re.finditer(r"\bR_AliasFinishLight\s*\(\s*\)\s*;", body)]
    if len(calls) != 1:
        errors.append(f"{name} must call R_AliasFinishLight () exactly once (found {len(calls)})")
        continue
    head, tail = body[:calls[0]], body[calls[0]:]
    w = LC_WRITE.search(tail)
    if w:
        errors.append(f"{name} writes lightcolor after R_AliasFinishLight ({w.group(0).strip()!r}): "
                      "that light would survive r_fullbright")
    if re.search(r"\bmodel_dlight_gpu\s*=", tail):
        errors.append(f"{name} sets model_dlight_gpu after R_AliasFinishLight: r_fullbright must leave it clear")
    if not LC_WRITE.search(head):
        errors.append(f"{name}: no lightcolor write before R_AliasFinishLight -- has the lighting chain moved?")
    for pat in after:
        if re.search(pat, head):
            errors.append(f"{name}: /{pat}/ runs before R_AliasFinishLight, so it reads the unflattened light")
        if not re.search(pat, tail):
            errors.append(f"{name}: /{pat}/ is missing after R_AliasFinishLight")

# ------------------------------------------------------------ the consumers
CONSUMERS = ("GL_DrawAliasFrame", "GL_DrawAliasFrameMD3", "GL_DrawAliasSkeletal")
for name in CONSUMERS:
    bodies = fns.get(name, [])
    reading = [b for b in bodies if re.search(r"\blightcolor\b", b)]
    if not reading:
        errors.append(f"gl_rmain.c: no {name} that reads lightcolor")
    for b in reading:
        for k in range(3):
            if not re.search(rf"\blightcolor\s*\[\s*{k}\s*\]\s*\*", b):
                errors.append(f"{name} must take channel {k} of its light from lightcolor[{k}]")
        if re.search(r"\b(?:R_AliasLightScale|R_LightPoint\w*|AliasModelGetLightInfo)\s*\(", b):
            errors.append(f"{name} computes light of its own; it must use the setup's lightcolor")
        w = LC_WRITE.search(b)
        if w:
            errors.append(f"{name} writes lightcolor ({w.group(0).strip()!r})")

# The instanced path's consumer is the InstanceData fill in the setup itself:
# every light_color write there is lightcolor, channel for channel.
INST_OK = re.compile(r"inst->light_color\[(\d)\]\s*=\s*lightcolor\[\1\]\s*\*\s*[RGB]Tint\[cs\]"
                     r"|VectorCopy\s*\(\s*lightcolor\s*,\s*inst->light_color\s*\)")
cai = one("R_CollectAliasInstance")
fills = [cai[m.start():cai.index(";", m.start())].strip()
         for m in re.finditer(r"\binst->light_color\s*\[[^\]]*\]\s*=(?!=)|\bVectorCopy\s*\([^;]*\binst->light_color\b", cai)]
bad = [f for f in fills if not INST_OK.fullmatch(f)]
if bad or len(fills) != 4:
    errors.append(f"R_CollectAliasInstance's light_color fill must be lightcolor[k] * Tint (x3) or VectorCopy(lightcolor, ...); got {bad or fills}")
for k, t in enumerate("RGB"):
    if not re.search(rf"inst->light_color\[{k}\]\s*=\s*lightcolor\[{k}\]\s*\*\s*{t}Tint\[cs\]", cai):
        errors.append(f"R_CollectAliasInstance must fill light_color[{k}] from lightcolor[{k}] * {t}Tint[cs]")

REACH = {
    "R_SetupAliasFrame": ["R_DrawAliasModel"],
    "GL_DrawAliasFrame": ["R_SetupAliasFrame"],
    "GL_DrawAliasFrameMD3": ["GL_DrawAliasFrame"],
    "GL_DrawAliasSkeletal": ["GL_DrawAliasFrame", "R_SetupAliasFrame"],
}
for callee, allowed in REACH.items():
    got = callers(callee)
    if got != sorted(allowed):
        errors.append(f"{callee} is called from {got}, expected only {sorted(allowed)}: a draw reached any other way "
                      "skips R_DrawAliasModel's R_AliasFinishLight")

LW =re.compile(r"\blight_color\s*\[[^\]]*\]\s*=(?!=)|\bVectorCopy\s*\([^;]*,\s*[\w>.\[\]-]*light_color\s*\)")
for n, lst in allfns.items():
    for p, b in lst:
        for m in LW.finditer(b):
            stmt = b[m.start():b.index(";", m.start())]
            if n == "R_CollectAliasInstance":
                continue
            if n == "R_DrawAliasInstanced" and re.fullmatch(r"light_color\[\d\]\s*=\s*1\.0f", stmt.strip()):
                continue
            errors.append(f"{p.relative_to(ROOT)}:{n} writes an instance's light_color ({stmt.strip()!r}); "
                          "only R_CollectAliasInstance, after R_AliasFinishLight, may")
glow = one("R_DrawAliasInstanced")
if "light_color" in glow and not re.search(r"\bgl_fullbrights\.integer\s*&&\s*!\s*r_fullbright\.integer", glow):
    errors.append("R_DrawAliasInstanced's flat-white glow redraw must stand down under r_fullbright")

# ------------------------------------------------------------ every lightcolor reader is classified
READERS = {
    "R_AliasFinishLight", "R_DrawAliasModel", "R_CollectAliasInstance", *CONSUMERS,
    "R_DrawViewModel",       # the viewmodel's 0..255 sample and cl.light_level; draws through R_DrawAliasModel
    "R_DumpAliasInfo",       # r_aliasinfo: samples, prints, restores
    "R_LightPointColor",     # gl_rlight.c: the producer
    "R_RenderBrushPoly",     # gl_rsurf.c: a translucent brush poly's point tint, saved and restored
}
for n, lst in allfns.items():
    for p, b in lst:
        if re.search(r"\blightcolor\b", b) and n not in READERS:
            errors.append(f"{p.relative_to(ROOT)}:{n} reads lightcolor and is not classified in {Path(__file__).name}: "
                          "if it lights an alias model it must take its light from R_AliasFinishLight")
vm = one("R_DrawViewModel")
if not re.search(r"\bR_DrawAliasModel\s*\(\s*e\s*\)", vm):
    errors.append("R_DrawViewModel must draw through R_DrawAliasModel, whose R_AliasFinishLight flattens the viewmodel")

# ------------------------------------------------------------ froxel dynamic light
lcfns = functions(stripped[ENGINE / "h2shared/gl_lightcluster.c"])
aa = (lcfns.get("R_LightCluster_AliasActive") or [""])[0]
if not re.search(r"!\s*r_fullbright\.integer", aa):
    errors.append("R_LightCluster_AliasActive must return false under r_fullbright")

if errors:
    print("test_alias_fullbright: FAIL")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("test_alias_fullbright: OK (R_AliasFinishLight flattens both setups; 3 consumers, 4 call edges, "
      f"{len(READERS)} lightcolor readers classified)")
