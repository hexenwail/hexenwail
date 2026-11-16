#!/usr/bin/env python3
"""Both brush-entity dlight mark loops stand down under the same gates.

R_DrawBrushModel (gl_rsurf.c) and R_CollectBrushInstances (gl_rmain.c) are
twins: each marks dlights into a submodel's surfaces for the CPU lightmap
path.  Each must skip marking when:
  - gl_flashblend is on (dlights are drawn as blobs instead), and
  - the clustered GPU path shades the world (R_LightCluster_ShadesWorld),
    since marking then double-lights the surfaces and re-dirties their
    lightmap pages (uhexen2-26bm).
The instanced twin lacked the gl_flashblend gate, so with gl_flashblend 1
instanced brush entities still took lightmap dlights on top of the blobs.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def function_body(text, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def guard_of_mark_loop(body):
    """Return the condition of the `if` whose block contains R_MarkLights."""
    call = body.find("R_MarkLights")
    if call < 0:
        return None
    best = None
    for m in re.finditer(r"\bif\s*\(", body[:call]):
        depth, i = 1, m.end()
        while depth and i < len(body):
            depth += {"(": 1, ")": -1}.get(body[i], 0)
            i += 1
        cond = body[m.end():i - 1]
        j = i
        while j < len(body) and body[j] in " \t\r\n":
            j += 1
        if j >= len(body) or body[j] != "{":
            continue
        depth, k = 1, j + 1
        while depth and k < len(body):
            depth += {"{": 1, "}": -1}.get(body[k], 0)
            k += 1
        if j < call < k:
            best = cond
    return best


SITES = [
    ("engine/hexen2/gl_rsurf.c", "R_DrawBrushModel"),
    ("engine/hexen2/gl_rmain.c", "R_CollectBrushInstances"),
]
for rel, fn in SITES:
    text = strip((ROOT / rel).read_text(errors="replace"))
    body = function_body(text, fn)
    if body is None:
        errors.append(f"{rel}: {fn} not found")
        continue
    cond = guard_of_mark_loop(body)
    if cond is None:
        errors.append(f"{rel}: {fn}: no if-block around its R_MarkLights loop")
        continue
    norm = re.sub(r"\s+", "", cond)
    if "!gl_flashblend.integer" not in norm:
        errors.append(f"{rel}: {fn} marks dlights without the !gl_flashblend.integer gate")
    if "!R_LightCluster_ShadesWorld()" not in norm:
        errors.append(f"{rel}: {fn} marks dlights without the !R_LightCluster_ShadesWorld() gate")
    if "firstmodelsurface!=0" not in norm:
        errors.append(f"{rel}: {fn} marks dlights without the firstmodelsurface != 0 gate")
    if re.search(r"\|\|", norm):
        errors.append(f"{rel}: {fn}: the mark-loop gate must be a pure && chain")

if errors:
    print("FAIL test_brush_dlight_gates:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: both brush dlight mark loops share the flashblend and cluster gates")
