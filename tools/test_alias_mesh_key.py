#!/usr/bin/env python3
"""An alias model finds its GPU mesh through its header, not its address (#134).

GL_MakeAliasGPUMesh runs on the loader's working header in the hunk; the
loader then copies that header into the cache and frees the hunk.  The mesh
used to be found by comparing header ADDRESSES with the one it was made for
-- a dead address before the first draw.  Every drawn model missed, built a
second mesh from the draw path (7 on demo1 + demo2), and a later header that
landed on a dead address was handed another model's mesh.

Now the slot and the table generation ride in the header (the memcpy into
the cache carries them).  Checked at source level:
  - aliashdr_t carries gpu_mesh / gpu_mesh_gen;
  - GL_GetAliasGPUMesh trusts the slot only for the current generation, and
    compares no header pointers;
  - every registration writes both fields, every refusal marks the header
    (-1, current generation) so the draw path does not rebuild it per frame;
  - GL_FreeAliasGPUMeshes bumps the generation, retiring every slot at once;
  - the generation starts at 1, so a zero-filled header is never current.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


hdr = strip((ROOT / "engine/h2shared/gl_model.h").read_text())
struct = hdr[:hdr.index("} aliashdr_t;")]
struct = struct[struct.rindex("typedef struct"):]
for field in ("int\t\tgpu_mesh;", "int\t\tgpu_mesh_gen;"):
    if field not in struct:
        errors.append(f"aliashdr_t: missing {field.split()[-1]}")

mesh = strip((ROOT / "engine/h2shared/gl_mesh.c").read_text())
if "alias_gpu_owners" in mesh:
    errors.append("gl_mesh.c still keeps a header-address table (alias_gpu_owners)")
if not re.search(r"static int alias_gpu_generation = 1;", mesh):
    errors.append("the generation must start at 1 (a zero-filled header must never be current)")

get = re.search(r"alias_gpu_mesh_t \*GL_GetAliasGPUMesh \(aliashdr_t \*hdr\)\s*\{(.*?)\n\}", mesh, re.S)
if not get:
    errors.append("no GL_GetAliasGPUMesh")
else:
    body = get.group(1)
    if re.search(r"==\s*hdr\b|\bhdr\s*==", body):
        errors.append("GL_GetAliasGPUMesh compares header pointers")
    if "hdr->gpu_mesh_gen == alias_gpu_generation" not in body or "alias_gpu_meshes[hdr->gpu_mesh]" not in body:
        errors.append("GL_GetAliasGPUMesh must look the slot up through the header, for the current generation")

free = re.search(r"void GL_FreeAliasGPUMeshes \(void\)\s*\{(.*?)\n\}", mesh, re.S)
if not free or "alias_gpu_generation++" not in free.group(1):
    errors.append("GL_FreeAliasGPUMeshes must start a new generation")

make = mesh[mesh.index("void GL_MakeAliasGPUMesh (aliashdr_t *hdr)\n"):]
registrations = len(re.findall(r"hdr->gpu_mesh = num_alias_gpu_meshes;\s*hdr->gpu_mesh_gen = alias_gpu_generation;\s*num_alias_gpu_meshes\+\+;", make))
if registrations != 2 or make.count("num_alias_gpu_meshes++") != 2:
    errors.append(f"every registration must stamp the header (found {registrations} of 2)")
# Every early return in the builder marks the header refused.
for m in re.finditer(r"\breturn;", make):
    before = make[max(0, m.start() - 260):m.start()]
    if not ("hdr->gpu_mesh = -1;" in before or "GL_RefuseAliasGPUMesh(hdr, gm);" in before
            or "num_alias_gpu_meshes++;" in before):
        line = make[:m.start()].count("\n")
        errors.append(f"GL_MakeAliasGPUMesh: a return (~{line} lines in) leaves the header unmarked; "
                      "the draw path would rebuild it every frame")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS alias models find their GPU mesh through the header, per generation (#134)")
