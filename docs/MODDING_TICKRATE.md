# The tick rate contract for modders — `sv_physfps` and `frametime`

Hexenwail runs the server and physics at **72 Hz by default**, not the 20 Hz that
the stock engine's listen server used. The cvar is `sv_physfps`
(`engine/hexen2/host.c:71`, `CVAR_ARCHIVE`, default `72`, clamped to
`[10, 250]` at `engine/hexen2/host.c:919`).

Almost all HexenC is unaffected. One idiom is not, and it fails in a distinctive
way. This page says which, why, and what to write instead.

> **Note:** `sys_ticrate` is *not* this knob. It is dedicated-server only. Any
> doc or comment still describing the client tick as 20 Hz predates
> `uhexen2-skjv` and is stale.

HexenC paths below are written the way the sources refer to each other — `h2/…`
is `gamecode/hc/h2/…` in this repo.

## The one idiom that breaks

```c
held.velocity = delta * 20;     // WRONG on any engine that is not exactly 20 Hz
```

The `20` is a hardcoded `1 / frametime` for the old tick: `1 / 0.05 == 20`. Per
tick an entity travels `velocity * frametime`, so at 20 Hz this closes exactly
100% of the gap in one tick — a perfect snap with zero tracking lag. It only
looks like "multiply by twenty"; it is really "divide by the tick interval".

At 72 Hz the same constant closes `20/72 = 28%` of the gap per tick. That is no
longer a snap, it is a proportional controller with finite gain, and a
proportional controller lags a moving target by roughly `target_speed / gain`.
At a 300 u/s strafe that is upwards of ten units — plainly visible.

### Recognising it in a bug report

The tell is the *shape* of the symptom:

> It is not "worse at 72, better at 20". It is **exact at 20**.

A value that is perfect at one specific rate and degrades smoothly in both
directions away from it is a hardcoded `1 / frametime`. Nothing else produces
that signature.

This is how it was found: BloodShot's continuous, Half-Life-2-style grab holds an
entity in front of the player. After `sv_physfps` shipped in `0.8.0-beta.r16` the
held object trailed the player, and tracked exactly again at `sv_physfps 20`
([video](https://www.youtube.com/watch?v=ex-Ozf9Wfl0)). The same mod's player-legs
model, positioned every frame by the same kind of helper, failed the same way
([video](https://www.youtube.com/watch?v=f0p4W-p7WWI)) — two features, one shared
cause.

## What to write instead

Raven's own code already demonstrates the fix, so this is "use what the base game
uses", not a new convention. `h2/client.hc:1326` scales water friction by the
engine-supplied `frametime` global:

```c
self.velocity = self.velocity - 0.8*self.waterlevel*frametime*self.velocity;
```

`frametime` is declared in `h2/global.hc:12` and written by the engine on every
server tick (`*sv_globals.frametime = host_frametime` at
`engine/hexen2/host.c:837`, where `host_frametime` has just been set to the tick
interval `1 / sv_physfps` at `engine/hexen2/host.c:957`). It is therefore always
authoritative and always tracks the cvar. Read it; never assume its value.

| Intent | Write this | Not this |
|---|---|---|
| Snap onto a target this tick | `held.velocity = delta / frametime;` | `held.velocity = delta * 20;` |
| Damped follow | `held.velocity = delta * K;` with `K` in 1/sec, comfortably below `1/frametime` | a `K` that happens to equal `1/frametime` |
| Schedule a think | `self.nextthink = time + 0.05;` — already correct | — |

Note what the middle row means: a damped follow was **always** rate-independent
in intent, and stays correct at any tick rate. Only the pathological case where
`K` coincides with `1/frametime` — which is what `* 20` is — changes behaviour.
So the bug is not "gain constants are unsafe", it is specifically "a gain equal
to the reciprocal of the tick interval was silently doing something else".

### Think intervals are fine and need no change

`HX_FRAME_TIME` (`h2/constant.hc:8`, `= 0.05`) has ~205 uses across `h2/`, and
every one is a think interval or an effect duration. Those are real-time
quantities: they stay correct at any tick rate, and a 72 Hz tick quantizes them
*more* finely than 20 Hz did, not less. `h2/crossbow.hc:359` divides an elapsed
time rather than a per-tick quantity, so it is fine too.

Do not go on a crusade replacing `HX_FRAME_TIME` with `frametime`. They mean
different things: `HX_FRAME_TIME` is "a twentieth of a second, as a duration",
`frametime` is "how long this tick was". Only the second one is about the tick.

## Shipped gamecode is clean — audited, not assumed

All 718 `.hc` files were grepped. The `velocity * 10` / `velocity * 20` sites in
`h2/` are **one-shot impulses**, applied once and then integrated ballistically
by the physics, which makes them rate-independent by construction:

| Site | What it is |
|---|---|
| `h2/skullwiz.hc:890,895` | Skull Wizard push, fired once per melee animation from the single `$skspel20` frame (`h2/skullwiz.hc:907`) |
| `h2/archer.hc:223` | Gold-arrow knockback, once on impact |
| `h2/golem.hc:1090` | Golem knockback, scaled by damage — not a reciprocal-of-tick constant |

None is a per-frame controller.

`h2/triggers.hc:1411,1416` (`trigger_magicfield_touch`) also matches the grep but
is **inside a `/* … */` block spanning lines 1407–1430** — it is commented-out
dead code and executes never. Do not cite it as a live example either way.

So this is a mod-compatibility documentation problem. It is not an engine bug and
not a shipped-content bug.

## Escape hatches for a mod that cannot be changed

Either works; the first is preferable because it is visible to the player.

1. Ship a `.cfg` in the mod directory containing `sv_physfps 20`.
2. Call it from QC — `cvar_set` is builtin #72, declared at `h2/builtin.hc:148`
   and verified present:

   ```c
   cvar_set ("sv_physfps", "20");
   ```

Both cost the mod everything `sv_physfps` was raised to fix: think chains capped
at 20 Hz, coarse monster attack cadence, and broken strafe-jump acceleration
(`SV_AirAccelerate` gains at most `addspeed` per tick, a figure that does not
scale with `frametime`). Treat them as a stopgap while the `* 20` is fixed, not
as a setting.

## Diagnosing a report of this shape

Ask for `r_lerpmove 0` first. It costs one cvar and separates two different bugs:

- Still lagging with `r_lerpmove 0` → the entity really **is** behind. That is
  this problem: a rate-dependent controller in QC.
- Correct with `r_lerpmove 0` → the entity is only **drawn** behind. That is
  client-side interpolation (`engine/hexen2/cl_parse.c:872`), a different
  investigation entirely.

## See also

- [`release-notes/0.8.0-beta.r16.md`](release-notes/0.8.0-beta.r16.md) — the release that shipped `sv_physfps`
- [`GAMECODE.md`](GAMECODE.md) — building and installing `progs.dat`
- [`../USAGE.md`](../USAGE.md) — the player-facing summary of `sv_physfps`
- `uhexen2-skjv` — the tick-rate change itself
- `uhexen2-pe7h` — this document
