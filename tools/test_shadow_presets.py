#!/usr/bin/env python3
"""The Display presets' dynamic-light occlusion tiers, and the menu row (GitHub #97, P5).

engine/hexen2/menu.c, M_Display_AdjustSliders, gives each preset a shadow-map
tier through PRESET_SHADOWS (res, lights, filter, updates, casters):

  preset      r_shadow_resolution  maxlights  filter  updates  casters
  Crunchy     64                   8          0       2        1
  Retro       64                   8          0       2        1
  Authentic   64                   8          0       2        1
  Faithful    128                  16         1       4        1
  Clean       128                  16         1       4        1
  Modern      256                  16         1       8        2
  Ultra       512                  16         1       16       2

and every preset sets r_dlight_occlusion to M_DLOCCLUSION_MAX, the most the
tier renders: 2 (the cube shadow maps) on desktop GL, 1 on the ES tier and the
software renderer, where a 2 renders as 1.  Checked here:

  * the table, preset by preset, exactly once each, and the macro's two arms
    (the software build has no r_shadow_* cvars, so its arm sets only the
    occlusion) and M_DLOCCLUSION_MAX's two values;
  * every value inside what gl_shadow.c accepts (the resolution clamp, the
    maxlights clamp to SHADOW_MAX_MAPS, casters 1 or 2), and the cvars it
    names registered there;
  * DETECTION: each preset's own writes -- its block, PRESET_COMMON,
    PRESET_SHADOWS -- run through M_Display_DetectPreset's conditions, parsed
    out of the source and evaluated, return that preset (a player on a preset
    is not shown as "User"), and every cvar the detection reads is written by
    every preset, so the answer cannot hang on whatever came before;
  * the Misc menu's "DLight Occlusion" row: placed after "Wall Torch DLs", a
    soft-ok row, stepping r_dlight_occlusion through 0..M_DlightOcclusionMax ()
    with wrap, drawn through M_DlightOcclusion (which reads a 2 as 1 where 2
    does not render) as Off / Walls / Shadows.  M_DlightOcclusionMax is what
    renders now: M_DLOCCLUSION_MAX, except 1 on desktop when
    R_LightCluster_ShadesWorld () is false (r_lightclusters 0, or no compute),
    because the cube maps light only the clustered path's receivers and
    gl_shadow.c builds none without it.  There the row's note says why
    Shadows is missing, inside M_ROWNOTE_MAX;
  * the defaults: r_dlight_occlusion 2 on desktop GL, 1 on the ES tier and in
    the software build, and r_shadow_casters 1 (alias casters only where a
    preset or the player asks).
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(rel):
    p = ROOT / rel
    if not p.exists():
        errors.append(f"{rel} is missing")
        return ""
    return p.read_text(errors="replace")


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


menu_raw = read("engine/hexen2/menu.c")
menu = strip(menu_raw)
shadow = strip(read("engine/h2shared/gl_shadow.c"))
shadow_h = strip(read("engine/h2shared/gl_shadow.h"))
draw = strip(read("engine/h2shared/gl_draw.c"))

NAMES = ["Crunchy", "Retro", "Authentic", "Faithful", "Clean", "Modern", "Ultra"]
TABLE = {
    1: (64, 8, 0, 2, 1), 2: (64, 8, 0, 2, 1), 3: (64, 8, 0, 2, 1),
    4: (128, 16, 1, 4, 1), 5: (128, 16, 1, 4, 1),
    6: (256, 16, 1, 8, 2), 7: (512, 16, 1, 16, 2),
}
ARGS = ["res", "lights", "filter", "updates", "casters"]
CVARS = ["r_shadow_resolution", "r_shadow_maxlights", "r_shadow_filter", "r_shadow_updates", "r_shadow_casters"]

# ------------------------------------------------------------------ the tier ceiling
m = re.search(r"#if defined\(GLQUAKE\) && !defined\(USE_GLES\)\s*#define\s+M_DLOCCLUSION_MAX\s+(\d+)\s*"
              r"#else\s*#define\s+M_DLOCCLUSION_MAX\s+(\d+)\s*#endif", menu)
if not m or (m.group(1), m.group(2)) != ("2", "1"):
    errors.append("M_DLOCCLUSION_MAX must be 2 under defined(GLQUAKE) && !defined(USE_GLES), else 1")

# ------------------------------------------------------------------ the macro
mac = re.search(r"#ifdef GLQUAKE\s*#define PRESET_SHADOWS\((\w+), (\w+), (\w+), (\w+), (\w+)\)((?:[^\n]*\\\n)*[^\n]*)\n"
                r"#else\s*#define PRESET_SHADOWS\([^)]*\)((?:[^\n]*\\\n)*[^\n]*)\n#endif", menu)
if not mac:
    errors.append("menu.c: no `#ifdef GLQUAKE / #define PRESET_SHADOWS(res, lights, filter, updates, casters) "
                  "... / #else / #define PRESET_SHADOWS(...) ... / #endif`")
    gl_sets, soft_sets, params = [], [], ARGS
else:
    params = list(mac.group(1, 2, 3, 4, 5))
    gl_sets = re.findall(r'Cvar_SetValue \("(\w+)", (\w+)\);', mac.group(6))
    soft_sets = re.findall(r'Cvar_SetValue \("(\w+)", (\w+)\);', mac.group(7))
if params != ARGS:
    errors.append(f"PRESET_SHADOWS takes {params}, not {ARGS}")
want_gl = [("r_dlight_occlusion", "M_DLOCCLUSION_MAX")] + list(zip(CVARS, ARGS))
if gl_sets != want_gl:
    errors.append(f"PRESET_SHADOWS' GL arm writes {gl_sets}; it must write {want_gl}, in that order")
if soft_sets != [("r_dlight_occlusion", "M_DLOCCLUSION_MAX")]:
    errors.append(f"PRESET_SHADOWS' software arm writes {soft_sets}; it must write only "
                  "r_dlight_occlusion = M_DLOCCLUSION_MAX (that build has no r_shadow_* cvars)")

# ------------------------------------------------------------------ the table
adj = body_of(menu, r"static void M_Display_AdjustSliders \(int dir\)\s*\{") or ""
case = re.search(r"case DISP_PRESET:\s*\{(.*?)\n\t\tbreak;\n\t\}", adj, re.S)
case = case.group(1) if case else ""
common = re.search(r"#define PRESET_COMMON((?:[^\n]*\\\n)*[^\n]*)\n", case)
common_sets = re.findall(r'Cvar_Set(?:Value)? \("(\w+)", ([^)]+)\);', common.group(1)) if common else []
if not common:
    errors.append("M_Display_AdjustSliders: no PRESET_COMMON")
blocks = {}
for m in re.finditer(r"if \(preset == (\d)\)\s*\{(.*?)\n\t\t\}", case, re.S):
    n = int(m.group(1))
    if n in blocks:
        errors.append(f"preset {n} has two blocks")
    blocks[n] = m.group(2)
if sorted(blocks) != list(range(1, 8)):
    errors.append(f"presets found {sorted(blocks)}, want 1..7")
raw_case = re.search(r"case DISP_PRESET:(.*?)#undef PRESET_COMMON", menu_raw, re.S)
for n, name in enumerate(NAMES, 1):
    if raw_case and not re.search(r"preset == %d\)\s*/\* %s\b" % (n, name), raw_case.group(1)):
        errors.append(f"preset {n} is not commented as {name}; the table's order is the menu's")

states = {}
for n, blk in sorted(blocks.items()):
    calls = re.findall(r"PRESET_SHADOWS \(([^)]*)\)", blk)
    if len(calls) != 1:
        errors.append(f"{NAMES[n - 1]} must call PRESET_SHADOWS exactly once, has {len(calls)}")
        continue
    vals = tuple(int(v) for v in re.split(r"\s*,\s*", calls[0].strip()))
    if vals != TABLE[n]:
        errors.append(f"{NAMES[n - 1]}: PRESET_SHADOWS {vals}, the table says {TABLE[n]}")
    if "PRESET_COMMON" not in blk:
        errors.append(f"{NAMES[n - 1]} lost PRESET_COMMON")
    st = {}
    for k, v in re.findall(r'Cvar_Set(?:Value)? \("(\w+)", ([^;]+)\);', blk) + common_sets:
        st[k] = v.strip()
    for k, v in zip(CVARS, vals):
        st[k] = str(v)
    st["r_dlight_occlusion"] = "2"     # the desktop arm; detection does not read it
    states[n] = st

# what gl_shadow.c accepts
for c in CVARS + ["r_shadow_bias", "r_shadow_cache", "r_shadow_debug"]:
    if not re.search(r'cvar_t\s+\w+\s*=\s*\{"%s",' % c, shadow) \
            or not re.search(r"Cvar_RegisterVariable \(&%s\);" % c, shadow):
        errors.append(f"gl_shadow.c must define and register {c}")
if not re.search(r'cvar_t\s+r_shadow_casters = \{"r_shadow_casters", "1", CVAR_ARCHIVE\};', shadow):
    errors.append('r_shadow_casters must default to "1", CVAR_ARCHIVE: alias casters are the Modern and Ultra tier')
smax = re.search(r"#define\s+SHADOW_MAX_MAPS\s+(\d+)", shadow_h)
smax = int(smax.group(1)) if smax else 0
ens = body_of(shadow, r"static qboolean SH_EnsureTexture \(void\)\s*\{") or ""
lo = re.search(r"if \(res < (\d+)\)\s*res = \1;", ens)
hi = re.search(r"if \(res > (\d+)\)\s*res = \1;", ens)
if not lo or not hi:
    errors.append("SH_EnsureTexture no longer clamps r_shadow_resolution; update this test's range check")
for n, vals in TABLE.items():
    res, lights, filt, upd, cas = vals
    if lo and hi and not int(lo.group(1)) <= res <= int(hi.group(1)):
        errors.append(f"{NAMES[n - 1]}: resolution {res} is outside SH_EnsureTexture's clamp")
    if res & (res - 1):
        errors.append(f"{NAMES[n - 1]}: resolution {res} is not a power of two")
    if not 1 <= lights <= smax:
        errors.append(f"{NAMES[n - 1]}: {lights} lights, SHADOW_MAX_MAPS is {smax}")
    if filt not in (0, 1) or cas not in (1, 2) or upd < 1:
        errors.append(f"{NAMES[n - 1]}: filter {filt}, casters {cas}, updates {upd} out of range")
if [TABLE[n][4] for n in range(1, 8)] != [1, 1, 1, 1, 1, 2, 2]:
    errors.append("alias casters are Modern and Ultra only (the plan's P6)")

# ------------------------------------------------------------------ detection
modes = re.search(r"glmode_t gl_texmodes\[NUM_GL_FILTERS\] =\s*\{(.*?)\};", draw, re.S)
modes = re.findall(r'\{\s*"(\w+)"', modes.group(1)) if modes else []
if len(modes) != 6:
    errors.append(f"gl_draw.c: gl_texmodes parsed as {modes}")
det = body_of(menu, r"static int M_Display_DetectPreset \(void\)\s*\{") or ""
locs = {}
for ty, var, expr in re.findall(r"\b(int|float)\s+(\w+)\s*=\s*([^;]+);", det):
    m = re.fullmatch(r'(?:\(int\))?\s*(?:Cvar_VariableValue\s*\(\s*"(\w+)"\s*\)|(\w+)\.(?:value|integer))', expr.strip())
    if not m:
        errors.append(f"DetectPreset reads `{expr.strip()}`, which this test cannot model")
        continue
    locs[var] = (ty, m.group(1) or m.group(2))
conds = re.findall(r"if \(([^)]*)\)\s*return (\d);", det)
if [int(r) for _, r in conds] != list(range(1, 8)) or not re.search(r"return 0;\s*$", det.strip()):
    errors.append(f"DetectPreset must test presets 1..7 in order, then return 0 (User); found {[r for _, r in conds]}")
read_cvars = {c for _, c in locs.values()} | ({"gl_texturemode"} if "gl_filter_idx" in det else set())


def num(s):
    s = s.strip().strip('"')
    return float(s.rstrip("f"))


def detect(st):
    env = {}
    for var, (ty, cv) in locs.items():
        v = num(st.get(cv, "nan"))
        env[var] = int(v) if ty == "int" and v == v else v
    env["gl_filter_idx"] = modes.index(st.get("gl_texturemode", '""').strip('"')) \
        if st.get("gl_texturemode", '""').strip('"') in modes else -1
    for expr, ret in conds:
        e = expr.replace("&&", " and ").replace("||", " or ")
        e = re.sub(r"!(?!=)", " not ", e)
        e = re.sub(r"(\d+\.\d+)f\b", r"\1", e)
        if eval(e, {"__builtins__": {}}, env):
            return int(ret)
    return 0


for n, st in sorted(states.items()):
    missing = sorted(c for c in read_cvars if c not in st)
    if missing:
        errors.append(f"{NAMES[n - 1]} does not set {missing}, which DetectPreset reads: "
                      "its detection would depend on the state before it")
        continue
    got = detect(st)
    if got != n:
        errors.append(f"{NAMES[n - 1]} applied is detected as {NAMES[got - 1] if got else 'User'}")

# ------------------------------------------------------------------ the row
gfx_enum = re.search(r"enum\s*\{\s*GFX_CENTERPRINTBG = 0,(.*?)GFX_ITEMS", menu, re.S)
members = re.findall(r"(GFX_\w+)", gfx_enum.group(1)) if gfx_enum else []
if "GFX_DLOCCLUSION" not in members or members[members.index("GFX_DLOCCLUSION") - 1] != "GFX_TORCH_DLIGHT":
    errors.append("GFX_DLOCCLUSION must follow GFX_TORCH_DLIGHT in the Misc menu")
if not re.search(r'"DLight Occlusion:",\s*/\* GFX_DLOCCLUSION \*/', menu_raw):
    errors.append('gfx_labels must carry "DLight Occlusion:" for GFX_DLOCCLUSION')
ga = body_of(menu, r"static void M_Graphics_AdjustSliders \(int dir\)\s*\{") or ""
row = re.search(r"case GFX_DLOCCLUSION:\s*\{(.*?)\}\s*case", ga, re.S)
row = row.group(1) if row else ""
for s in ("int v = M_DlightOcclusion () + dir;", "if (v < 0) v = M_DlightOcclusionMax ();",
          "if (v > M_DlightOcclusionMax ()) v = 0;", 'Cvar_SetValue ("r_dlight_occlusion", v);'):
    if s not in row:
        errors.append(f"the GFX_DLOCCLUSION row must step with `{s}`")
fn = body_of(menu, r"static int M_DlightOcclusion \(void\)\s*\{") or ""
if 'Cvar_VariableValue ("r_dlight_occlusion")' not in fn \
        or "int max = M_DlightOcclusionMax ();" not in fn \
        or "return v < 0 ? 0 : v > max ? max : v;" not in fn:
    errors.append("M_DlightOcclusion must read r_dlight_occlusion clamped to 0..M_DlightOcclusionMax ()")
# The ceiling that renders: the cube maps are gl_shadow.c's, which selects
# lights only while the cluster grid is up (R_LightCluster_Available), so on
# desktop the ceiling follows R_LightCluster_ShadesWorld, its frame-stable twin.
cap = re.search(r"static int M_DlightOcclusionMax \(void\)\s*\{\s*"
                r"#if defined\(GLQUAKE\) && !defined\(USE_GLES\)\s*"
                r"if \(!R_LightCluster_ShadesWorld \(\)\)\s*return 1;\s*#endif\s*"
                r"return M_DLOCCLUSION_MAX;\s*\}", menu)
if not cap:
    errors.append("M_DlightOcclusionMax must return 1 on desktop GL when R_LightCluster_ShadesWorld () is false, "
                  "M_DLOCCLUSION_MAX otherwise")
if not re.search(r"r_dlight_occlusion\.integer >= 2 && !sh_failed && R_LightCluster_Available \(\)", shadow):
    errors.append("gl_shadow.c no longer gates the maps on R_LightCluster_Available (); the row's ceiling "
                  "(M_DlightOcclusionMax) must follow whatever it gates on now")
if not re.search(r'#if defined\(GLQUAKE\)\n(?:#include "[^"]+"\n)*#include "gl_lightcluster.h"\n(?:#include "[^"]+"\n)*#endif',
                 menu_raw):
    errors.append('menu.c must #include "gl_lightcluster.h" under #if defined(GLQUAKE)')
# Capped: the note says why -- the cvar when it is off, the GPU when it is on
# (no compute, or the grid failed to build) -- each inside M_ROWNOTE_MAX.
note = re.search(r'static const char \*M_Graphics_Note \(int row\)\s*\{\s*'
                 r'if \(row == GFX_DLOCCLUSION && M_DlightOcclusionMax \(\) < M_DLOCCLUSION_MAX\)\s*'
                 r'return Cvar_VariableValue \("r_lightclusters"\) \? "([^"]*)"\s*:\s*"([^"]*)";', menu)
rowmax = re.search(r"#define\s+M_ROWNOTE_MAX\s+(\d+)", menu)
if not note or "r_lightclusters 1" not in note.group(2) or "GPU" not in note.group(1):
    errors.append("M_Graphics_Note must say, while the row is capped below the tier, that Shadows need "
                  "r_lightclusters 1 (cvar off) or that the GPU cannot draw them (cvar on)")
else:
    for n in note.groups():
        if not rowmax or len(n) > int(rowmax.group(1)):
            errors.append(f"the capped row's note \"{n}\" runs past M_ROWNOTE_MAX")

gd = body_of(menu, r"static void M_Graphics_Draw \(void\)\s*\{") or ""
if not re.search(r'names\[\] = \{ "Off", "Walls", "Shadows" \};.*?names\[M_DlightOcclusion \(\)\]', gd, re.S):
    errors.append("the row must draw Off / Walls / Shadows through M_DlightOcclusion")
skip = body_of(menu_raw, r"static qboolean M_Graphics_IsSkip \(int i\)\s*\{") or ""
okc = re.search(r"soft-ok:(.*?)\*/", skip, re.S)
if not okc or "GFX_DLOCCLUSION" not in okc.group(1):
    errors.append("GFX_DLOCCLUSION is a soft-ok row (r_surf.c, r_light.c, r_main.c read the cvar)")

# ------------------------------------------------------------------ the defaults
rm = read("engine/hexen2/gl_rmain.c")
if not re.search(r'#ifdef USE_GLES\n(?:[^#\n][^\n]*\n)*?cvar_t\s+r_dlight_occlusion = \{"r_dlight_occlusion", "1", CVAR_ARCHIVE\};\n'
                 r'(?:[^#\n][^\n]*\n)*?#else\n(?:[^#\n][^\n]*\n)*?cvar_t\s+r_dlight_occlusion = \{"r_dlight_occlusion", "2", CVAR_ARCHIVE\};\n(?:[^#\n][^\n]*\n)*?#endif', rm):
    errors.append("gl_rmain.c: r_dlight_occlusion defaults to 2 on desktop, 1 under USE_GLES")
if not re.search(r'cvar_t\s+r_dlight_occlusion = \{"r_dlight_occlusion", "1", CVAR_ARCHIVE\};',
                 read("engine/hexen2/r_soft_web.c")):
    errors.append("r_soft_web.c: r_dlight_occlusion defaults to 1")

if errors:
    print("FAIL test_shadow_presets:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: 7 presets carry their shadow-map tier and are detected as themselves; the occlusion row "
      "and defaults follow the tier (#97 P5)")
