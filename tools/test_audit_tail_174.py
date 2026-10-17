#!/usr/bin/env python3
"""The 2026-08-18 rendering audit's deferred tail (GitHub #174).

(a) r_entdist culls instanced alias models too: R_CollectAliasInstance hands
    an entity beyond it to the legacy loop, which applies the cull.  Verified:
    r_entdist 1 on demo1's start left the tree and angel statue drawn on
    master, culled with this.
(b) MAX_ALIAS_BATCHES == MAX_ALIAS_INSTANCES, so batching cannot run out.
(e) gl_mesh.c's buffers are sized from MAXALIASTRIS (no overrun for any
    model the loader accepts), and the legacy draw's strip scratch from
    MAX_ALIAS_STRIP_VERTS, with no desyncing count clamp.
(f) both scale switches read the undefined both-bits values as 0 (uniform,
    center), in both draw paths.
(h) the dead GL R_LightPoint is gone.
(c), (d), (g) were already fixed on master when this landed.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []
rm, mesh, hdr = read("engine/hexen2/gl_rmain.c"), read("engine/h2shared/gl_mesh.c"), read("engine/h2shared/gl_model.h")

ci = re.search(r"static qboolean R_CollectAliasInstance \(entity_t \*e\)\s*\{(.*?)\n\}", rm, re.S)
if not ci or "r_entdist.value > 0" not in ci.group(1):
    errors.append("(a) R_CollectAliasInstance must leave entities beyond r_entdist to the legacy cull")
if "#define MAX_ALIAS_BATCHES	MAX_ALIAS_INSTANCES" not in hdr:
    errors.append("(b) MAX_ALIAS_BATCHES must equal MAX_ALIAS_INSTANCES")
if "#define	MAX_ALIAS_STRIP_VERTS	(MAXALIASTRIS + 2)" not in hdr:
    errors.append("(e) MAX_ALIAS_STRIP_VERTS must be MAXALIASTRIS + 2")
for need in ("static int		used[MAXALIASTRIS];", "static int		commands[MAXALIASTRIS * 7 + 1];",
             "static int		vertexorder[MAXALIASTRIS * 3];", "static int		stripverts[MAX_ALIAS_STRIP_VERTS];",
             "static int	bestverts[MAX_ALIAS_STRIP_VERTS];"):
    if need not in mesh:
        errors.append(f"(e) gl_mesh.c lacks {need!r}")
if re.search(r"\[(128|1024|8192)\]", re.sub(r"/\*.*?\*/", "", mesh, flags=re.S)):
    errors.append("(e) gl_mesh.c still has a fixed-size mesh buffer")
if "if (count > 128) count = 128;" in rm or rm.count("if (count > MAX_ALIAS_STRIP_VERTS)\n\t\t\tbreak;") != 2:
    errors.append("(e) the legacy strip walk must stop, not clamp, on an oversized count")
if re.search(r"tmp_(tc|color|pos)\[128\]", rm):
    errors.append("(e) the legacy strip scratch must be MAX_ALIAS_STRIP_VERTS")
if rm.count("default:	/* 24, both type bits") != 2 or rm.count("default:	/* 96, both origin bits") != 2:
    errors.append("(f) both draw paths must read the both-bits scale values as 0")
if "int R_LightPoint (vec3_t p)" in read("engine/h2shared/gl_rlight.c") or "int R_LightPoint (vec3_t p);" in read("engine/h2shared/glquake.h"):
    errors.append("(h) the dead GL R_LightPoint must stay removed")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS rendering audit tail (#174): entdist, batches, mesh buffers, scale bits, dead code")
