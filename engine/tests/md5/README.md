# Synthetic MD5 skeletal test models

There is no MD5 asset in the Hexen II data, so these hand-built pairs exist to
exercise `engine/h2shared/md5mesh.c` (uhexen2-7ok0) without one.  Each is small
enough that every expected value can be worked out on paper.

| Pair | What it covers |
|---|---|
| `test.md5mesh` / `test.md5anim` | Two bones, a vertex weighted 50/50 across both, three frames rotating the second bone 0°/45°/90° about X. |
| `chain.md5mesh` / `chain.md5anim` | Three-bone chain where only the middle joint animates — the tip must follow it, which only happens if parent transforms propagate before children compose. |

`gen.py` and `gen3.py` regenerate them.  Both encode quaternions the way the
loader reads them back: id's MD5 stores only X/Y/Z and reconstructs W as the
**negative** root, so the generators flip the sign of all four components
whenever W would come out positive.  Get that wrong and every bone rotates the
right amount the wrong way.

## Expected results

Frame 0 of both animations is the rest pose, so every frame-0 bone matrix must
come back as **exact identity** — that is the sharpest available check on the
`anim_world × inv_rest_world` composition, since the two factors have to cancel
to the bit.

For `chain.md5anim` frame 1 (middle joint rotated 90° about X), the three
vertices land at `(0,0,0)`, `(0,0,10)` and `(0,-10,10)`.

## Running them

The parser has no engine dependencies beyond the console, hunk, filesystem and
texture entry points, so `md5mesh.c` links against a handful of stubs and runs
standalone.  There is no committed harness — see the uhexen2-7ok0.2 bead notes
for the stub set that was used, or load the models in the engine with
`developer 1` and read the `MD5_LoadMesh:` line.
