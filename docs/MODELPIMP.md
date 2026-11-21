# `misc_modelpimp` — what it grants, and how far it reaches

`pimpmodel()` is builtin #111, an extension by Inky that lets a map decorate
alias models with engine-side effects — an orb glow, a cast light, spin, float,
and the trail/transparency flags an MDL header would normally carry. It is the
only route by which map-placed props get those effects, and it is the mechanism
behind Storm over Thyrion's and Wheel of Karma's lighting.

This document is the contract: which entity's fields the engine reads, which
effects are per-entity and which are shared across every model of the same
name, and what resets when. It exists because the reach rule is not obvious
from the spawnflag names and has already been field-reported twice as a bug —
see [Reach](#reach-who-else-is-affected).

Engine side lives in `PF_pimpmodel` (`engine/h2shared/pr_cmds.c`) and
`R_GetPimpFlags` / `R_DrawAllGlows` (`engine/hexen2/gl_rmain.c`).

All of it is local to one process: `PF_pimpmodel` runs in server QC and writes
straight into the client renderer's tables, so only a client in the same
process as the server sees any pimp effect, this orb included. A remote client
of a listen server, and every client of `h2ded`, sees none.

## The call

```c
float pimpmodel(entity ref, vector glow_color)
```

Everything else is read off `ref` — **the entity passed as the first argument,
not the entity that made the call.** That distinction matters: gamecode is free
to build a throwaway entity, stuff fields onto it and pimp that instead, and
Storm over Thyrion's `entity_fx()` does exactly this for missiles and torch
flames.

| Field on `ref` | Meaning |
|---|---|
| `model` | picks the shared `qmodel_t`. No model, no effect — the call returns 0 |
| `spawnflags` | which effects to grant, and how far they reach (below) |
| `flags` | trail / transparency bits OR-ed into the model's MDL flags |
| `abslight` | glow alpha; `0` means 0.75 |
| `health` | orb radius |
| `max_health` | cast-light radius |
| `view_ofs` | orb offset from the entity origin |
| `style` | light style for the cast light |

`glow_color` is taken as 0..1 unless any component exceeds 1, in which case it
is treated as 0..255 and divided down. All-zero keeps the model's own colours.

A colour given without spawnflag 4 or 8 auto-enables the orb, because older QC
(SoT's `obj_evileyes`) calls `pimpmodel` with just a colour and expects one —
**but only on entities that set none of the r15 reach flags (16, 32, 64).**

That inference cannot simply be deleted: 40 of the 151 `misc_modelpimp`
entities shipped across Storm over Thyrion and Wheel of Karma depend on it,
almost all of them spinning puzzle keys and items that set `glow_color`,
`health` and `style` but never spawnflag 4. Requiring bit 4 unconditionally
puts every one of them out.

It is equally wrong for anyone writing against this document, where spawnflag 4
is the thing that grants the orb. So the two populations are split by the one
signal that separates them: no content written before 0.8.0-beta.r15 can set
16, 32 or 64. **Touch any reach flag and the strict contract applies — the orb
comes from spawnflag 4 and nowhere else.** Set no reach flag and the legacy
inference stands.

If you want a cast light whose colour is not also an orb, set the reach flag
you need (or 16, which is a no-op for a lone entity) and the colour will be
left to the light.

## Spawnflags

| Bit | Name | Effect |
|---|---|---|
| 1 | Spin | `EF_SPIN` — rotate without the item bob |
| 2 | Float | `EF_FLOAT` — bob without rotating |
| 4 | Glow orb | `EF_GLOW` — additive sprite at the entity, coloured by `glow_color` |
| 8 | Cast light | `EF_ILLUMINATE` — allocate a dynamic light at the entity |
| 16 | Self only | keep glow and cast light on this entity instead of sharing; with 4, **one orb, here**: at the controller's origin (see [below](#the-controller-is-never-drawn)) |
| 32 | Share motion | give siblings the spin and float as well |
| 64 | No dynamic light | grant `XF_NO_DLIGHT`; cast no dlight at all |

Bits 1–8 are Inky's originals. 16, 32 and 64 were added in 0.8.0-beta.r15 and
are **off by default**, so a map written against the original four behaves
exactly as it did.

## Reach: who else is affected

This is the part that surprises people.

An effect is granted in one of two places, and which one decides how far it
travels:

- **Per-entity** — written to the renderer's override table, keyed by edict
  number. Only that one entity is affected.
- **Shared** — written to `mod->ex_flags` on the `qmodel_t`. **Every entity on
  the map using that model file is affected**, including plain `misc_model`s
  the mapper never flagged, and including ones spawned later.

By default the split is asymmetric:

| Effect | Default reach | Override |
|---|---|---|
| `EF_GLOW`, `EF_ILLUMINATE`, `XF_NO_DLIGHT` | **shared** | spawnflag 16 keeps them per-entity |
| `EF_SPIN`, `EF_FLOAT` | **per-entity** | spawnflag 32 shares them |
| `flags` (trail / transparency) | shared, plus a per-entity layer when non-zero | — |

The asymmetry is deliberate, not an oversight. SoT-style mods place one hidden
`misc_modelpimp` specifically to light every map-placed prop sharing a model —
demo1's intro has six display items each casting from its own position, driven
by one controller per model. Flipping the rule would break that. But it is also
exactly backwards for a mapper who wants a single glowing prop among many, or a
set of siblings that all spin, which is why 16 and 32 exist.

Two consequences worth knowing:

- **Sharing is additive and never retracted within a map.** Spawnflag 16 stops
  *this* controller from writing to the shared model; it does not undo a share
  another controller already made. If two controllers name the same model and
  only one sets 16, the model is still shared.
- **Cast light stacks.** Each sibling allocates its own dlight at its own
  position, so the dlight count scales with the number of siblings. That is the
  intent of the flag, but it is also how a map runs out of dynamic lights.
  Spawnflag 64 is the lever for taking a specific model back out of the set.

### The controller is never drawn

Inky's `misc_modelpimp` sets `EF_NODRAW` on itself, and an edict whose
`effects` is exactly `EF_NODRAW` is never sent to the client
(`SV_WriteEntitiesToClient`: "don't send if flagged for NODRAW and there are no
lighting effects"). The controller has no client entity at all, so anything
that lives only on the controller is drawn from nowhere.

That is why, until #518, spawnflags 20 (4 + 16) drew **no
orb anywhere**: 16 kept the orb on the controller, and nothing drew the
controller. Now:

| Spawnflags | Orb |
|---|---|
| 4 | on every entity sharing the model, as before; **none** at the controller |
| 4 + 16 | **exactly one**, at the controller's origin, with its `glow_color` and `health` (radius) |
| neither 4 nor 8, a colour, no reach flag | inferred orb, shared, as before (#277) |

`PF_pimpmodel` records the controller's origin and model in its override and
registers it; `R_DrawAllGlows` draws every registered self-only orb after the
visible entities' orbs, skipping one whose entity the client already drew (a
visible prop pimped 4 + 16 keeps its single orb). So the way to put an orb on
one prop out of several that share a model is a 4 + 16 controller placed where
the orb should be. Spawnflag 4 alone adds nothing at the controller, because
its siblings draw the orb and an extra one would change every shipped SoT map.

The orb is drawn at the origin recorded by the last `pimpmodel` call; Inky's
controllers re-pimp every `wait`, and they do not move. It belongs to the
controller, not to the prop it sits on: remove the prop and the orb stays. It
is not PVS-culled, but it is depth-tested, so walls still hide it.

**Known gap:** a self-only cast light (spawnflags 24, 8 + 16) has the same
cause and is still dark: the dlight is allocated in `CL_RelinkEntities`, which
never sees the controller. Spawnflag 8 alone works, because each sibling casts
its own.

`r_glowinfo` lists the orbs the last frame drew (entity number, effects, radius,
colour, origin, model); `tools/pimp-orb-check.sh` uses it to check all of the
above live on a SoT map.

## What resets, and when

Shared writes mutate a model that outlives the map, so they are snapshotted and
rolled back:

- `Mod_SaveAliasModelDefaults` records `flags`, `ex_flags` and `glow_settings`
  when the model loads.
- `R_NewMap` calls `Mod_RestoreAliasModelDefaults` and then
  `R_ClearPimpOverrides`, in that order, so the next map's controllers re-pimp
  from clean MDL state. `R_ClearPimpOverrides` also empties the list of
  self-only orbs, so a loaded savegame starts with none until its controllers
  next think.

A model pimped on map A therefore cannot leak onto map B. Per-entity overrides
are also cleared when the edict is freed (`ED_Free`).

Changing `EF_TRANSPARENT`, `EF_HOLEY` or `EF_SPECIAL_TRANS` re-uploads the
model's skins, because those three decide the texture mode the skin was
uploaded with. This is a no-op in the software renderer, which samples
palettized texels directly.

## Software / WebAssembly renderer limits

`EF_SPIN` and `EF_FLOAT` drive the software alias transform as well as the
GL/GLES transform. Glow orbs remain GL-only; `EF_ILLUMINATE` does work,
because the dlight is allocated in `cl_main.c`, which both renderers share.

## Adding your own light-casting models: `r_dlight_model_list`

`pimpmodel` is per-entity, which is the wrong shape when a light fixture is
assembled from several models rather than being one pimped prop. There is no
spawnflag that can describe "these six models together are a lamp", because the
flag lives on a model.

Separately from `pimpmodel`, the engine carries a built-in group of models that
cast a dynamic light — the `rflmtrch` / `cflmtrch` / `castrch` / `rometrch` /
`egtorch` / `flame` family, assigned in `Mod_SetAliasModelExtraFlags`
(`engine/h2shared/gl_model.c`). That group was hardcoded in C, so a mod could
not join it.

`r_dlight_model_list` opens it up, on the same pattern as `r_nolerp_list`:

```
r_dlight_model_list "models/mylamp.mdl,models/mysconce.mdl"
```

- **Additive.** The built-in group is unchanged and always wins; this only adds
  models the engine did not already know about. Empty by default, so nothing
  shipped changes.
- **Whole names, case-insensitive**, exactly like `r_nolerp_list` —
  `models/mylamp.mdl`, not a prefix.
- **Read at model load.** Set it from your mod's `autoexec.cfg`; changing it
  mid-session takes effect on the next map load.
- Entries get the built-in torch treatment, colour and `TORCH_STYLE` flicker
  included. Per-model colour and radius remain the `misc_modelpimp` route —
  this is the "make it a light" switch, not a lighting editor.
- `gl_torch_dlight 0` still switches the whole group off, built-in and listed
  alike.

To confirm an entry took, `mcache` reports the group per model:

```
 757 (0x...): models/puzzle/chalice.mdl TORCH_GLOW
```

A missing `TORCH_GLOW` there means the name did not match — check the path and
extension before looking for the light elsewhere.

## `misc_model` spawnflags are a different vocabulary

A recurring source of confusion, and the reason a field report reads as an
engine bug when it is not: **a plain `misc_model` never reaches
`PF_pimpmodel`.** Nothing in the engine spawns `misc_model` or
`misc_modelpimp`; both are gamecode, and the spawnflags a mapper sets on a
`misc_model` are interpreted entirely by the mod.

In Storm over Thyrion (`progs.dat` crc 26905) the `misc_model` spawn function
never calls `pimpmodel` at all. Its spawnflags mean:

| Bit | Effect in SoT gamecode |
|---|---|
| 1 | `drawflags |= DRF_TRANSLUCENT` |
| 2 | breakable — `takedamage`, `health` defaults to 1 |
| 4 | gibs on break; precaches `gibmdl1`..`gibmdl3` |
| 8 | hidden — cleared model, `SOLID_NOT`, `EF_NODRAW` |
| 16 | destroyed when triggered; also implies 4 |
| 32 | `drawflags |= SCALE_ORIGIN_TOP` |
| 64 | `drawflags |= SCALE_ORIGIN_BOTTOM` |
| 128 | `drawflags |= SCALE_TYPE_ZONLY` |
| 262144 | pushable — `MOVETYPE_PUSHPULL`, `obj_push` touch |
| 524288 | `drawflags |= SCALE_TYPE_XYONLY` |
| 1048576 | `makestatic()` |

So bits 16, 32 and 64 already carry mod meanings on `misc_model` that have
nothing to do with the engine's reach flags of the same value. They do not
collide, because the two entities are read by different code — but a mapper
reading one table while editing the other entity will get nonsense.

To grant an engine effect you need a `misc_modelpimp` (or whatever the mod
calls `pimpmodel` from). Setting spawnflag 4 on a `misc_model` and expecting an
orb will not work in SoT, and no engine change can make it work; that is a
gamecode decision.

## Verifying a change against shipped content

The reach flags were added without breaking shipped maps, and the check is
repeatable. Of the locally available content, only Storm over Thyrion and Wheel
of Karma bind builtin #111 at all — `data1`, `portals`, Shadows of Chaos and
Game of Tomes never call it. Within those two, `pimpmodel` is reached from
exactly three QC functions: `modelpimp_think` and `modelpimp_showcase`, both
passing `self` on a `misc_modelpimp`, and SoT's `entity_fx`, which sets
`spawnflags` on a throwaway entity from its own argument.

That gives two things to enumerate before adding a spawnflag bit:

1. every `misc_modelpimp` in every shipped `.bsp` — currently 149 entities
   using spawnflags 0, 1, 4, 5, 8, 12, 13, 1796 and 3332;
2. every `spflags` argument at an `entity_fx` call site — currently 44 sites
   passing 0, 4, 8 or 12.

Neither set touches bits 16, 32 or 64, which is why r15 changed nothing that
shipped, and why using one of those bits is a safe signal that a map was
written against r15 or later — the discriminator the orb inference above relies
on. Re-running the enumeration today over both mods' `.bsp` files gives 151
`misc_modelpimp` entities with spawnflags 0, 1, 4, 5, 8, 12, 13, 29, 1796 and
3332; the single entity carrying a reach bit is sf=29 (`1|4|8|16`) in
`karma2/testmod_mathuz.bsp`, which sets bit 4 as well and so is unaffected. Note that 1796 and 3332 do carry high bits — 256, 512, 1024 and 2048 —
that the engine currently never tests, and so silently ignores. A future
spawnflag must not claim one of those.
