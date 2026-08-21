# `misc_modelpimp` — what it grants, and how far it reaches

`pimpmodel()` is builtin #111, an extension by Inky that lets a map decorate
alias models with engine-side effects — an orb glow, a cast light, spin, float,
and the trail/transparency flags an MDL header would normally carry. It is the
only route by which map-placed props get those effects, and it is the mechanism
behind Shadows of Turmoil's and Wheel of Karma's lighting.

This document is the contract: which entity's fields the engine reads, which
effects are per-entity and which are shared across every model of the same
name, and what resets when. It exists because the reach rule is not obvious
from the spawnflag names and has already been field-reported twice as a bug —
see [Reach](#reach-who-else-is-affected).

Engine side lives in `PF_pimpmodel` (`engine/h2shared/pr_cmds.c`) and
`R_GetPimpFlags` (`engine/hexen2/gl_rmain.c`).

## The call

```c
float pimpmodel(entity ref, vector glow_color)
```

Everything else is read off `ref` — **the entity passed as the first argument,
not the entity that made the call.** That distinction matters: gamecode is free
to build a throwaway entity, stuff fields onto it and pimp that instead, and
Shadows of Turmoil's `entity_fx()` does exactly this for missiles and torch
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
(SoT's `obj_evileyes`) calls `pimpmodel` with just a colour and expects one.

## Spawnflags

| Bit | Name | Effect |
|---|---|---|
| 1 | Spin | `EF_SPIN` — rotate without the item bob |
| 2 | Float | `EF_FLOAT` — bob without rotating |
| 4 | Glow orb | `EF_GLOW` — additive sprite at the entity, coloured by `glow_color` |
| 8 | Cast light | `EF_ILLUMINATE` — allocate a dynamic light at the entity |
| 16 | Self only | keep glow and cast light on this entity instead of sharing |
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

## What resets, and when

Shared writes mutate a model that outlives the map, so they are snapshotted and
rolled back:

- `Mod_SaveAliasModelDefaults` records `flags`, `ex_flags` and `glow_settings`
  when the model loads.
- `R_NewMap` calls `Mod_RestoreAliasModelDefaults` and then
  `R_ClearPimpOverrides`, in that order, so the next map's controllers re-pimp
  from clean MDL state.

A model pimped on map A therefore cannot leak onto map B. Per-entity overrides
are also cleared when the edict is freed (`ED_Free`).

Changing `EF_TRANSPARENT`, `EF_HOLEY` or `EF_SPECIAL_TRANS` re-uploads the
model's skins, because those three decide the texture mode the skin was
uploaded with. This is a no-op in the software renderer, which samples
palettized texels directly.

## Known gap: the software / WebAssembly renderer

`EF_SPIN` and `EF_FLOAT` are honoured by the GL renderer only. `r_alias.c`
tests the raw `EF_ROTATE` MDL header bit and never consults `R_GetPimpFlags`,
so a `misc_modelpimp` granting spin or float — shared or not — does nothing in
the software build, including the `WEB_RENDERER=software` web build. Glow orbs
are likewise GL-only; `EF_ILLUMINATE` does work, because the dlight is
allocated in `cl_main.c`, which both renderers share.

## `misc_model` spawnflags are a different vocabulary

A recurring source of confusion, and the reason a field report reads as an
engine bug when it is not: **a plain `misc_model` never reaches
`PF_pimpmodel`.** Nothing in the engine spawns `misc_model` or
`misc_modelpimp`; both are gamecode, and the spawnflags a mapper sets on a
`misc_model` are interpreted entirely by the mod.

In Shadows of Turmoil (`progs.dat` crc 26905) the `misc_model` spawn function
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
repeatable. Of the locally available content, only Shadows of Turmoil and Wheel
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
shipped. Note that 1796 and 3332 do carry high bits — 256, 512, 1024 and 2048 —
that the engine currently never tests, and so silently ignores. A future
spawnflag must not claim one of those.
