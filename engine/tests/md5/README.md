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
standalone.  `md5check.c` is that harness, committed as of uhexen2-a5nn.10 —
it was written by hand twice before that and thrown away both times.

```bash
nix develop --command gcc -o /tmp/md5check \
    engine/tests/md5/md5check.c engine/h2shared/md5mesh.c \
    common/strlcpy.c common/qsnprint.c \
    -Iengine/hexen2 -Iengine/h2shared -Icommon \
    -DGLQUAKE -DSDLQUAKE -DGL_DLSYM -D_GNU_SOURCE=1 -D_REENTRANT \
    $(pkg-config --cflags sdl3) -w -lm
/tmp/md5check engine/tests/md5      # exits 1 on any mismatch
```

It loads `chain` and checks every joint's world position in both frames
against the values the geometry above forces.  What it is really checking is
the `bindpose` block `md5mesh.c` keeps for `r_showskel`: joint position is
`bone_matrix x bindpose`, so a wrong offset or a stale bind pose moves the
joints and nothing else in the engine would notice — the GPU never reads that
block.

Alternatively, load the models in the engine with `developer 1` and read the
`MD5_LoadMesh:` line.
