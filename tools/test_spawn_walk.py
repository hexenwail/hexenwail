#!/usr/bin/env python3
"""An edict created during the physics walk waits for the next frame (#138).

SV_Physics used to treat a new edict by its slot: one appended past
entity_cap waited a frame, one that reused a freed slot after its spawner's
was moved a full tick at once, so a projectile first appeared up to a tick's
travel ahead of where it was spawned (the Paladin's purifier: 14 units at
72 Hz, 50 at 20, measured on #138).  Decided: every new edict waits.

Checked at source level:
  - SV_Physics sets sv_physics_walk to a fresh nonzero value before the walk,
    skips a non-client edict whose spawnwalk equals it, and zeroes it after;
  - ED_Alloc stamps spawnwalk on both of its paths (reused and appended), and
    ED_Alloc_Temp on both of its, after ED_ClearEdict.
The live check -- every purifier missile 6.0 units out at sv_physfps 20 and
72, reused slot or appended -- is in the commit that made the change.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def body(text, signature):
    start = text.index(signature)
    i = text.index("{", start)
    depth, end = 1, i + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


phys = body(strip((ROOT / "engine/hexen2/sv_phys.c").read_text()), "void SV_Physics (void)")
walk_at = phys.find("for (i = 0; i < entity_cap;")
if walk_at < 0:
    errors.append("sv_phys.c: the entity walk is not where this test looks for it")
else:
    before, after = phys[:walk_at], phys[walk_at:]
    if not re.search(r"sv_physics_walk\s*=\s*\(\s*int\s*\)\s*walks\s*;", before) or not re.search(r"unsigned\s+int\s+walks", before):
        errors.append("sv_phys.c: sv_physics_walk must get a fresh nonzero value before the walk")
    if not re.search(r"if\s*\(\s*ent->spawnwalk\s*==\s*sv_physics_walk\s*&&\s*i\s*>\s*svs\.maxclients\s*\)\s*continue\s*;", after):
        errors.append("sv_phys.c: the walk must skip a non-client edict created during it")
    if not re.search(r"sv_physics_walk\s*=\s*0\s*;", after):
        errors.append("sv_phys.c: sv_physics_walk must go back to 0 after the walk")

edict = strip((ROOT / "engine/h2shared/pr_edict.c").read_text())
for fn, want in (("edict_t *ED_Alloc (void)", 2), ("edict_t *ED_Alloc_Temp (void)", 2)):
    b = body(edict, fn)
    n = len(re.findall(r"->spawnwalk\s*=\s*sv_physics_walk\s*;", b))
    if n != want:
        errors.append(f"pr_edict.c: {fn.split('*')[1].split('(')[0].strip()} stamps spawnwalk {n} time(s), wanted {want} (every path)")
    for m in re.finditer(r"ED_ClearEdict\s*\(\s*(\w+)\s*\)\s*;", b):
        tail = b[m.end():m.end() + 120]
        if f"{m.group(1)}->spawnwalk" not in tail and "Least->spawnwalk" not in b:
            errors.append(f"pr_edict.c: {fn}: spawnwalk must be set after ED_ClearEdict")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS an edict created during the physics walk waits for the next frame (#138)")
