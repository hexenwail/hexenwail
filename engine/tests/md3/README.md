# MD3 loader tests

`md3mesh.c` is the .md3 parser added under uhexen2-2ah9.  Before it, this tree
had the MD3 *vertex format* — `PV_MD3`, its SSBO upload in `gl_mesh.c`, the
decode in the instanced vertex shader — and nothing that read a file, so none
of it was reachable.

## What is actually being checked

The parser has to satisfy a contract nothing else states out loud.
`GL_MakeAliasGPUMesh` numbers pose vertices by walking the **GL command list**,
so `hdr->posedata` must be in command-list order and `hdr->poseverts` must be
that walk's count — the same invariant `GL_MakeAliasModelDisplayLists` keeps
for a `.mdl` by reordering its poses through `vertexorder[]`.  Get it wrong and
the model still loads, still draws, and is simply the wrong shape.  A unit test
catches that; a screenshot does not.

The other thing worth pinning is the normal's byte order.  MD3 stores one
little-endian short per vertex, **low byte longitude, high byte latitude**
(Quake III `tr_surface.c`).  The scaffolding shader had the two the wrong way
round, which nothing noticed because no loader existed to reach it.  The
fixture's two surfaces carry different normals for exactly this reason: read
the bytes the wrong way and both come out `(0,0,1)`.

## The fixture

`gen.py` writes `quad.md3`: two surfaces of one triangle each, two frames.
Two surfaces so the concatenation is exercised; two frames so the pose stride
is.  Frame 1 is frame 0 with every Z raised by one model unit, and the frames'
declared bounding boxes reach further than frame 0's vertices do — which is how
"bounds come from the file, not from the vertices" is checked.

```bash
python3 engine/tests/md3/gen.py       # rewrites quad.md3 in place
```

## Running them

`md3mesh.c` has no engine dependencies beyond the console, hunk and texture
entry points, so it links against a handful of stubs and runs standalone —
same shape as `engine/tests/md5`.

```bash
nix develop --command gcc -o /tmp/md3check \
    engine/tests/md3/md3check.c engine/h2shared/md3mesh.c \
    common/strlcpy.c common/qsnprint.c \
    -Iengine/hexen2 -Iengine/h2shared -Icommon \
    -DGLQUAKE -DSDLQUAKE -DGL_DLSYM -D_GNU_SOURCE=1 -D_REENTRANT \
    $(pkg-config --cflags sdl3) -w -lm
/tmp/md3check engine/tests/md3      # exits 1 on any mismatch
```

The last case feeds the loader half the file and expects a refusal: every
offset in an .md3 is self-declared, so a truncated one must not be parsed from
its own numbers.

## In the engine

`quad.md3` doubles as a substitution fixture.  Copy it over any `.mdl`'s base
name in a scratch gamedir and the enhanced-model path will pick it up:

```bash
mkdir -p ~/hexen2/md3test/models
cp engine/tests/md3/quad.md3 ~/hexen2/md3test/models/barrel.md3
glhexen2 -basedir ~/hexen2 -game md3test +developer 1 \
         +r_enhancedmodels 1 +r_enhancedmodels_priority md3 +map demo1
```

Every barrel becomes a two-triangle wedge, and the console says so.  Run it
with `r_alias_gpu 0` as well: that is the CPU streaming path
(`GL_DrawAliasFrameMD3`), which is the only path the WebGL2 build has and which
the desktop still falls back to for EF_HOLEY / translucent entities.
