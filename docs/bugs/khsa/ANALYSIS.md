# uhexen2-khsa — screenshot forensics (2026-08-18)

Two NVIDIA screenshots, the reporter artifact the issue has been blocked on since
2026-06-22. Both are Egyptian-hub maps, 2560x1440, ~70 FPS, nearest-filtered
textures.

* `khsa-nvidia-alias-anubis.png` — an anubis guard (**alias model**) rendered as a
  stipple ghost in an interior hall. Everything else in the frame is clean.
* `khsa-nvidia-brushent-boat.png` — the barge (**brush entity**) rendered as a
  stipple ghost: hull, deck, blue meander frieze, red/gold banner curtains, prow.
  The anubis guard standing *on* it renders solid and correct, as do the temple
  wall, water, terrain and sky behind it.

So the same defect hits an alias model in one frame and a brush entity in the
other. It is **per entity**, and it covers the *whole* entity uniformly.

## Measurements

Regions were converted to a binary mask (dark/light split at the mid-decile
midpoint) and compared against clean control regions from the same frames.

| Region | 1-px run share (dark) | autocorr @1px | mod-2 parity spread |
|---|---|---|---|
| anubis torso | 53 % | +0.07 | 0.048 |
| anubis leg | 55 % | — | — |
| boat hull (mid distance) | 49 % | +0.18 | 0.025 |
| boat red banner (near, magnified) | 43 % | — | — |
| boat canopy (far) | 34 % | +0.40 | 0.018 |
| **control** — temple wall | 3 % | +0.99 | 0.023 |
| **control** — floor | 4 % | +0.95 | 0.018 |
| **control** — sky | 0 % | — | — |

Run-length histograms in the affected regions decay ~50 / 25 / 12 / 6 % for
lengths 1 / 2 / 3 / 4 — the signature of **independent Bernoulli(p≈0.5) per
fragment**. Controls sit at 85–97 % in the longest bucket.

Coverage measures 43–59 % everywhere it appears.

## What the pixels prove

1. **Holes are byte-exact background.** Inside the anubis silhouette the surviving
   background pixels are literally `(43,31,19)` / `(48,33,22)`, identical to the
   adjacent untouched floor run. Nothing is blended over them. The fragment is
   killed, not composited → a **discard or a failed depth/stencil test**, never
   `GL_BLEND`.
2. **Granularity is one screen pixel, at every distance.** The red banners are
   heavily magnified (a uniformly red texture spanning ~10 screen px) yet still
   stipple at 1-px granularity. If the alpha came from the texture, magnification
   would produce texel-sized blocks. It does not. → the decision is made
   **per screen fragment**, not per texel.
3. **The pattern is not locked to the screen grid.** mod-2 parity spread is
   0.018–0.048 and mod-4 is 0.03–0.11 — indistinguishable from the clean controls.
   An ordered dither would light up one parity class hard.

## Ruled out by measurement

* `GL_SAMPLE_ALPHA_TO_COVERAGE` — its coverage mask is an ordered, screen-locked
  pattern; would show mod-2/mod-4 structure. Also `r_alphatocoverage` defaults 0
  and every enable site is gated on it.
* `GL_DITHER` / Bayer / `r_softemu` / `r_dither` post-process — all screen-locked
  ordered matrices, and post-process is screen-wide, not per entity.
* MSAA sample-mask effects — screen-locked.
* Texture-space alpha noise (bad `tex.a`, mip/LOD shimmer, TEX_HOLEY mask, the
  r29 trilinear-across-binarized-mips theory) — would be texel-granular under
  magnification. Finding 2 kills this. This is the strongest new result: it
  retires the r29 diagnosis as an explanation for *this* artifact.
* Bad `v_color.a` — vertex-interpolated, so it would give smooth gradients over
  the boat's large flat faces, not per-pixel noise.
* Lightmap alpha — `R_BuildLightMap` writes `dest[3] = 255` unconditionally on
  both the coloured and greyscale paths (gl_rsurf.c:517, 541), and the atlas is
  hard-forced to GL_RGBA8. `lm.a` is 1.0.
* Soft particles (`u_soft_params` leaking hot onto entity draws) — plausible
  shape, but `r_softparticles` defaults 0, gl_rmain.c:787 resets it, and the
  uniform exists only in `salias_frag`; it cannot explain the brush-entity boat.
* Stochastic/hashed alpha test — fully removed from the tree in the 2026-08-13
  teardown; `grep -rn stochastic engine/` is empty.

## What is left

A per-fragment, 1-px, screen-space, ~50 % kill that is not grid aligned, applied
to a whole entity, on NVIDIA only. That leaves essentially two families:

* **Alpha test where `color.a` lands right on `u_alpha_threshold`** with the
  fragment-to-fragment variation coming from something screen-space rather than
  texel-space. Both shaders gate on `if (color.a < u_alpha_threshold) discard;`.
* **Depth/stencil test failing per fragment**, i.e. the entity's interpolated
  depth ties with whatever is already in the buffer to within ~1 ULP so the
  GEQUAL/LEQUAL comparison flips per pixel. There is no Z-prepass in the engine
  (grep found none), so this needs the entity to be submitted twice through
  vertex paths whose position math differs by a rounding step — e.g. the
  CPU-streaming alias path (world-space verts, identity model matrix) versus the
  GPU-instanced path (model-space verts + per-instance matrix). `invariant
  gl_Position` is already on sworld/salias/sskeletal/salias_inst, but `invariant`
  only pins *identical* expressions; it does not make those two different
  expressions agree.

The depth family fits the reporter's odd clues better than the alpha family
does — "always the same entity", "shifts between builds" (memory layout decides
which entity lands on which path), "re-running the same map fixes it", "a
different map in between breaks it again", "destroying the barrel fixes it",
and NVIDIA-only (its shader compiler is free to fuse the MVP multiply
differently between two programs where AMD's happens to match).

Caveat: the r5 magenta probe (fragment shader replaced with solid magenta,
tester reported "fully magenta") argues *against* the depth family, since a
depth-test kill survives any fragment-shader change. That report is ambiguous —
it is not recorded whether the tester was looking at a currently-broken entity
at the time, and the defect is intermittent per map load.

## Cheapest next experiments

1. Have the tester toggle **`r_alias_gpu 0/1`** on a broken frame and report
   whether the *same* entity breaks under both. (The 2026-06-22 note says the
   legacy path still showed it, but that predates several path changes.)
2. `r_novis 1`, `r_dynamic 0`, `gl_overbright 0` — each cheap, each isolates one
   input to `color.a`.
3. Decisive: a **RenderDoc capture** of a broken frame. One capture settles
   alpha-vs-depth in seconds by inspecting the depth buffer and the killed
   fragments. This is now the single highest-value remaining artifact.
4. If a capture is impossible, a temporary `r_entdebug` that forces
   `u_alpha_threshold = 0` (never discard) for entity draws separates the two
   families: if the stipple survives, it is depth; if it vanishes, it is alpha.

## Still needed from the reporter

* The **map name** for each screenshot (both Egyptian hub) and the **build tag**.
* A **savegame** standing in front of the broken entity.
* Confirmation that these two frames came from one session, and whether
  re-running the map from the console cleared them.

---

# Addendum — map identified: SoT (Shadows of Turmoil) Egypt (2026-08-18)

Reporter confirms the map is Shadows of Turmoil's Egypt. `sot/maps/egypt.bsp`
and `egypt2.bsp` are present on the dev box, so the entity lumps were parsed
directly.

## The stippled boat is a specific, named entity

```
classname  func_train_mp
model      *5              <- BSP submodel: it IS a brush entity, confirmed
targetname ship
abslight   0.7             <- byte 178
```

That settles the brush-vs-model question the screenshots could only suggest.

## `abslight` is the common factor between the two frames

`abslight` sets `MLS_ABSLIGHT` in `drawflags` (`quakedef.h:195`, value 7 in the
low 3 bits) plus a separate 0–255 `e->abslight` byte. Critically, **any non-zero
MLS style routes an entity off the fast batched path on *both* sides**:

* brush: `gl_rsurf.c:2701-2702` — the VBO-batched submodel fast path requires
  `(e->drawflags & MLS_MASKIN) == MLS_NONE`. The ship fails it and falls to the
  legacy per-surface `R_RenderBrushPoly`.
* brush, again: `gl_rmain.c:2089` — the instanced brush collector `continue`s on
  any MLS style, for the same reason (uhexen2-j7rp).
* alias: same `MLS_MASKIN` dispatch at `gl_rmain.c:1361` / `2547`, where
  `ambientlight = shadelight = e->abslight` replaces the light trace.

`R_RenderBrushPoly` then does `GL_ImmColor4f(intensity, intensity, intensity,
alpha_val)` with `intensity = e->abslight / 255.0f` (gl_rsurf.c:827-845) — i.e.
these entities go through the **immediate-mode emulation vertex path in
gl_vbo.c**, which the fast paths bypass entirely.

That is the shared code path the screenshots were pointing at, and it is the
first mechanism found that covers an alias model and a brush entity at once.

egypt.bsp abslight census — 11 distinct values across ~120 entities; the only
brush entities carrying it are the ship (0.7), two `func_button` (1.0) and
thirteen `func_illusionary` (1.0). egypt2.bsp has one `func_train_mp` at 1.0 and
two `func_illusionary` at 0.3.

Note also `gl_rmain.c:3277-3282` (the r10 `r_aliasinfo` comment) already
suspected exactly this: "if e->alpha != 0, or drawflags has DRF_TRANSLUCENT or
any MLS_*, the model is NOT on the opaque path and the symptom may be
entity-side". The map data now confirms the affected entities do carry `abslight`.

## Consistency check against the frames

`intensity = abslight/255` is applied as a flat vertex colour multiplier. The
stippled anubis in frame 1 renders nearly black — surviving pixels measure
`(6,4,3)` and `(18,7,3)` against a `(62,43,21)` floor. egypt.bsp has six
`obj_statue_tut` at `abslight 0.4` (intensity 0.4) and a spread of statue/misc
models at 0.2–0.4. A 0.4 multiplier on an already-dim hall is consistent with
what was measured. The anubis standing on the ship's deck in frame 2 renders
solid and correctly lit — consistent with a normally-lit entity with no
`abslight`.

This is consistent, not proven: the exact entity in frame 1 has not been
identified, and a savegame or the map name for that specific frame would nail it.

## Note on the 1-px granularity claim

The like-for-like version of the argument, which is the defensible one: in the
*same frame at comparable depth*, the world floor renders in flat runs of 6+
screen pixels (one texel spans ~6 px there — the raw dump shows `(43,31,19)`
repeated across x=0..5), while the anubis leg beside it stipples with 55 % of
dark runs at length 1. The stipple is finer than the texel grid at that
distance. That is why texture-sourced alpha does not explain it — not an
independent measurement of the skin's texel size, which the screenshots cannot
give.

## Secondary thread worth keeping

SoT ships **external TGA skins** for these models (`sot/models/guard_*.tga`,
`guards_0.tga` alongside `guard.mdl`, `guard2.mdl`, `guardS.mdl`). External
replacement skins take a different upload path from internal 8-bit MDL skins and
carry a real alpha channel from the file. The engine also already has a
`GL_Upload8` workaround commented "for SoT mod compatibility" for
non-multiple-of-4 texture sizes, so SoT content is known to stress this area.
Lower priority than the `abslight` path given the 1-px finding, but it is the
obvious second thing to look at, and it would not explain the brush-entity ship.

## Hardware scope (corrected by the reporter, 2026-08-18)

Two testers, **NVIDIA GTX 980 (Maxwell GM204)** and **GTX 1080 (Pascal GP104)**.
The AMD dev box (Renoir/Vega) has never shown it. A local repro attempt was
proposed above and is **withdrawn** — the negative result is already known.

What the split does add: Maxwell and Pascal are different architectures sharing
NVIDIA's driver and compiler lineage. Both showing it rules out a one-off driver
regression, and it confirms this is driver divergence rather than SoT content
being malformed — the same content renders correctly on AMD.

## Falsifiable test that needs no new build

If `abslight` is the trigger then **every** abslight entity in egypt.bsp should
stipple, not just the ship:

| count | entity | abslight |
|---|---|---|
| 13 | `func_illusionary` (`tr35X`..`tr47X`) | 1.0 |
| 2 | `func_button` (`timeon`) | 1.0 |
| 6 | `obj_statue_tut` | 0.4 |
| 1 | `func_train_mp` `*5` (`ship`) | 0.7 — already confirmed broken |

* all stippled → abslight confirmed; search narrows to legacy
  `R_RenderBrushPoly` / the `gl_vbo.c` immediate-mode path
* some but not all → abslight is a correlate, and whatever separates the two
  groups is the real lead
* none of the others → hypothesis dead

egypt2.bsp cross-check: one `func_train_mp` at 1.0 (`statact`), two
`func_illusionary` at 0.3.

## Open tension, stated plainly

`abslight` is a fixed map key, so it predicts the same entities break
**deterministically on every load**. That contradicts the 2026-06-22 barrel
clues — "re-running the same map from console fixes it", "a different map in
between breaks it again", "shifts between hexenwail versions". Either those
clues belong to a *separate* defect that got merged into this issue, or
`abslight` is only a predisposing condition and something nondeterministic
decides whether a predisposed entity actually breaks.

The census above discriminates this too: if the identical set breaks on every
load, the barrel clues are a different bug and should be split out of khsa.

## Revised artifact priority

1. The no-build abslight census — cheapest, falsifiable, no round-trip.
2. A RenderDoc capture from the 980 or 1080 box — still settles alpha-vs-depth
   outright.
3. Only then a probe build, and if one is made it should be a targeted abslight
   bypass (force `MLS_ABSLIGHT` entities onto the fast path, or intensity=1)
   rather than another blind fragment-pipeline probe.

---

# Addendum 2 — angle-dependence, and a correction (2026-08-18)

Reporter: a video shows the player moving around guards and **their correct
texture popping in and out as the view angle changes**.

## Correction — the 1-px argument was wrong

The claim in the main section, that 1-px granularity rules out texture-space
alpha, is **withdrawn**.

At the mip LOD the hardware selects, texels map ~1:1 to screen pixels *by
construction* — that is what LOD selection does. So texture-space alpha noise is
exactly what you should expect to see at ~1 screen pixel on any minified
surface. The like-for-like comparison does not hold either: the world floor is
magnified at LOD 0 while a 256×256 MDL skin on a mid-distance model is minified
at a higher LOD. Different textures, different resolutions, different LODs.

The alpha-test / mip-filtering family is back on the table, and the
angle-dependence actively supports it — view angle changes UV derivatives
(foreshortening) and anisotropic tap patterns, both of which move edge texels
across a discard threshold.

Relatedly, r29's `MIN_FILTER` demotion was reverted in `8c88a5f29`
(uhexen2-izii), so master runs full trilinear + anisotropic on FENCE/HOLEY
today. The revert comment itself concedes it "removed one of two sources of an
effect that remains".

## Confirmed mechanism: PimpModel grant/rollback asymmetry (uhexen2-mr2l)

**Grant** — `pr_cmds.c:490-494`, when a `misc_modelpimp` carries `flags` with
EF_HOLEY (16384):

```c
mod->flags = mod->orig_flags | (new_flags & 0x01ffffff);
if ((mod->flags & EF_HOLEY) && !(mod->orig_flags & EF_HOLEY))
    Mod_ReuploadAliasSkins(mod);
```

`Mod_ReuploadAliasSkins` re-runs `Mod_LoadAllSkins` with the granted flags, so
the skins go up through the `TEX_HOLEY` path in `GL_Upload8` — **every
palette-index-0 texel becomes alpha 0**. This writes the *shared* `qmodel_t`, so
it affects every entity using that model.

**Rollback** — `Mod_RestoreAliasModelDefaults` (gl_model.c), called from
`R_NewMap` on map change, restores `mod->flags = mod->orig_flags` and
**never re-uploads the skins**.

So a model pimped EF_HOLEY on map A carries HOLEY skin alpha into map B while
its flags say opaque. The opaque path uses `u_alpha_threshold = 0.01` — but the
discard is unconditional (the khsa r20 comment in `salias_frag` records that
gating it on `threshold > 0.5` was tried and reverted). `0 < 0.01`, so
**discard still fires** on every index-0 texel, and mip/aniso filtering makes
the boundary flicker as the angle changes.

Survival depends on the model not being purged on the transition; the purge is
gated on `flush_textures && gl_purge_maptex`.

## Why this fits the 2026-06-22 clue set

That note narrowed the search to "something shared across all instances of a
model, corrupted by a map transition, memory-layout dependent, and hit by the
CPU-streaming path". `mod->flags` plus the shared GL skin texture *is* that
thing. `Mod_Extradata`'s cache reload inside `Mod_ReuploadAliasSkins` can evict
other models, giving the memory-layout dependence. It is a texture, not a mesh,
so `r_alias_gpu 0` still shows it. Re-running the same map fixes it (clean purge
+ reload); a different map in between breaks it (stale HOLEY skin, restored
flags).

## Content measurement

SoT guard skins are **21–25 % palette index 0** (guard.mdl skins 0/1/4/5/7/8/10
measure 16–25 %; guard2.mdl and guard_split.mdl ~21 %). Under `TEX_HOLEY` that
whole fraction becomes alpha 0 — the right order of magnitude for the measured
43–59 % coverage loss once mip/aniso spread is added. Index 0's palette colour
is black, which matches the near-black surviving pixels.

## What is *not* closed

The EF_HOLEY pimp grants in SoT target `parchment.mdl`, `book.mdl`,
`empty.mdl`, `empty2.mdl`, `empty4.mdl` — **never `guard.mdl`**, verified across
all 20 SoT maps. `PF_pimpmodel` resolves via `Mod_FindName(model_name)`, so it
does not misdirect by index. The direct chain to the guard is therefore not
established. Either (a) some other path grants EF_HOLEY to guard.mdl, (b)
`Mod_ReuploadAliasSkins` clobbers a neighbouring texture slot, or (c) the guards
are a distinct defect. Note `Mod_FindName` *creates* a stub entry when the model
is not precached and the following `Mod_Extradata` loads it mid-map, which can
evict other models — a plausible route to (b).

## Ruled out this pass

* SoT's external guard TGAs are clean — `guard_3/6/9.tga` are 32-bit with alpha
  uniformly 255; `guard_10.tga` and `guards_0.tga` are 24-bit.
* `guard.mdl` and siblings have header flags `0x0` — no EF_HOLEY /
  EF_TRANSPARENT / EF_SPECIAL_TRANS baked.
* `Mod_FixupMissingModelFlags` is tightly scoped to `models/ball.mdl`, so the
  recent zo7x fix is not granting EF_HOLEY to anything else.

## Next

Fix uhexen2-mr2l regardless — it is a real bug on its own. Then one question to
the tester, no build required: **do the guards render correctly on a fresh load
of egypt (new game / direct map load), versus after arriving from another map?**
That tests the whole chain in one answer.


---

# Addendum 3 — r9 tested, not fixed (2026-08-18)

Mathuzzz tested 0.8.0-beta.r9. The uhexen2-mr2l pimpmodel skin fixes **do not**
resolve the screen-door. The stale-HOLEY-skin mechanism is eliminated as the
cause.

That was always the weak link, and Addendum 2 said so: the EF_HOLEY pimp grants
in SoT target `parchment`/`book`/`empty*.mdl` and never `guard.mdl`. mr2l stays
closed on its own merits — the EF_TRANSPARENT grant path really was leaving
SoT's candles, waterfalls and crown pickup silently opaque — it is simply not
this bug. The khsa→mr2l dependency has been removed.

## Also eliminated: stencil

Worth writing down because it looked good on paper. A garbage stencil buffer
would produce per-fragment ~50% coverage loss with byte-exact background, and
would plausibly differ between vendors — Mesa tends to zero-fill VRAM, NVIDIA
does not.

It does not happen here. `R_SetupGL` clears stencil unconditionally every frame
when `have_stencil` (`gl_rmain.c:4619-4623`), and every `glEnable(GL_STENCIL_TEST)`
site has a matching disable (`gl_rsurf.c:2563`, `gl_rmain.c:1106`,
`gl_sky.c:1643`).

## Running elimination list

A2C · ordered dither · `r_softemu`/`r_dither` · MSAA sample masks · lightmap
alpha · `v_color.a` · soft particles · stochastic alpha (absent from tree) ·
external TGA skins (alpha uniformly 255) · `Mod_FixupMissingModelFlags` (scoped
to `ball.mdl`) · the r29 mipmap-filter theory · mr2l stale HOLEY skins · stencil.

## Established, and not in dispute

* fragments are **discarded**, not blended — holes are byte-exact background
* ~50% coverage loss, one screen pixel, not aligned to the screen grid
* hits **both** alias models (guards) and brush entities (the egypt ship)
* NVIDIA only, across two architectures (980 Maxwell, 1080 Pascal); never AMD
* the correct texture pops in and out with **view angle** → mip LOD /
  anisotropic taps are involved
* cross-map, and per the 2026-06-22 clues tied to map **transitions**

## Process

Per the 2026-06-22 pivot note: **do not ship another speculative probe build.**
That pattern has now cost ~30 builds across a multi-day Discord loop, and the
defect has still never been directly observed by a developer. The remaining
candidates cannot be separated by reading code. They need one observation.

Two cheap things are still missing:

1. Mathuzzz said "not fixed" but has not answered the discriminating question —
   for each map, did he **arrive from another map** or load it **directly**
   (`map <name>` / new game), and does re-loading the same map clear it? A
   cold-direct-load sighting kills the entire map-transition family in one reply.
2. **A RenderDoc capture** from either reporter. Now the highest-value artifact
   by a wide margin: it shows the guard's actual draw call, the skin texture
   actually bound with its alpha channel, and the live `u_alpha_threshold`. That
   separates every remaining candidate at once.
