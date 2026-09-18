# MD3 loader tests

Tests for `engine/h2shared/md3mesh.c`, the .md3 parser.

## What is actually being checked

- **Pose vertex order.** `hdr->posedata` must be in GL command-list order and
  `hdr->poseverts` must be that walk's count, because `GL_MakeAliasGPUMesh`
  numbers pose vertices that way. Get it wrong and the model still loads and
  draws, but the wrong shape.
- **Normal byte order.** MD3 stores one little-endian short per vertex, low
  byte longitude, high byte latitude. The fixture's two surfaces carry
  different normals; read the bytes the wrong way and both come out `(0,0,1)`.
- **Truncation.** The last case feeds the loader half the file and expects a
  refusal.

## The fixture

`gen.py` writes `quad.md3`: two surfaces of one triangle each, two frames.
Frame 1 is frame 0 with every Z raised by one unit, and the declared bounding
boxes reach further than the vertices, so the test checks that bounds come from
the file.

```bash
python3 engine/tests/md3/gen.py       # rewrites quad.md3 in place
```

## Running them

`md3mesh.c` links against a handful of stubs and runs standalone:

```bash
nix develop --command gcc -o /tmp/md3check \
    engine/tests/md3/md3check.c engine/h2shared/md3mesh.c \
    common/strlcpy.c common/qsnprint.c \
    -Iengine/hexen2 -Iengine/h2shared -Icommon \
    -DGLQUAKE -DSDLQUAKE -DGL_DLSYM -D_GNU_SOURCE=1 -D_REENTRANT \
    $(pkg-config --cflags sdl3) -w -lm
/tmp/md3check engine/tests/md3      # exits 1 on any mismatch
```

## In the engine

Copy `quad.md3` over any `.mdl`'s base name in a scratch gamedir and the
enhanced-model path picks it up:

```bash
mkdir -p ~/hexen2/md3test/models
cp engine/tests/md3/quad.md3 ~/hexen2/md3test/models/barrel.md3
glhexen2 -basedir ~/hexen2 -game md3test +developer 1 \
         +r_enhancedmodels 1 +r_enhancedmodels_priority md3 +map demo1
```

Every barrel becomes a two-triangle wedge, and the console says so. Run it
with `r_alias_gpu 0` as well: that is the CPU path (`GL_DrawAliasFrameMD3`),
which WebGL2 always uses and desktop uses for EF_HOLEY / translucent entities.
