# Synthetic MD5 skeletal test models

Hand-built models for testing `engine/h2shared/md5mesh.c`, since the Hexen II
data has no MD5 asset. Every expected value can be worked out on paper.

| Pair | What it covers |
|---|---|
| `test.md5mesh` / `test.md5anim` | Two bones, a vertex weighted 50/50 across both, three frames rotating the second bone 0°/45°/90° about X. |
| `chain.md5mesh` / `chain.md5anim` | Three-bone chain where only the middle joint animates; the tip must follow it. |

`gen.py` and `gen3.py` regenerate them. MD5 stores only quaternion X/Y/Z and
reconstructs W as the **negative** root, so the generators flip all four signs
whenever W would come out positive.

## Expected results

Frame 0 of both animations is the rest pose, so every frame-0 bone matrix must
be **exact identity**.

For `chain.md5anim` frame 1 (middle joint rotated 90° about X), the three
vertices land at `(0,0,0)`, `(0,0,10)` and `(0,-10,10)`.

## Running them

`md5check.c` links `md5mesh.c` against a handful of stubs and runs standalone:

```bash
nix develop --command gcc -o /tmp/md5check \
    engine/tests/md5/md5check.c engine/h2shared/md5mesh.c \
    common/strlcpy.c common/qsnprint.c \
    -Iengine/hexen2 -Iengine/h2shared -Icommon \
    -DGLQUAKE -DSDLQUAKE -DGL_DLSYM -D_GNU_SOURCE=1 -D_REENTRANT \
    $(pkg-config --cflags sdl3) -w -lm
/tmp/md5check engine/tests/md5      # exits 1 on any mismatch
```

It loads `chain` and checks every joint's world position in both frames. That
covers the `bindpose` block `md5mesh.c` keeps for `r_showskel`, which the GPU
never reads, so nothing else would catch a bad value there.

Alternatively, load the models in the engine with `developer 1` and read the
`MD5_LoadMesh:` line.
