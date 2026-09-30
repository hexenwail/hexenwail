# Detecting Hexenwail from HexenC

**Contract.** A mod may rely on everything on this page. The cvar name, its
encoding and its monotonicity will not change.

Asked for by Mathuzzz (Realm of Shadows), 2026-08-26, to gate engine-specific
content: *"How can I make engine-dependent conditions in HC? Is it possible?"*
Issue [#279](https://github.com/hexenwail/hexenwail/issues/279).

## The short version

```c
float(string s) cvar = #45;

float engine;
engine = cvar("hexenwail");

if (engine >= 80031)
{
    // Hexenwail 0.8.0-beta.r31 or newer
}
else if (engine)
{
    // some older Hexenwail
}
else
{
    // uHexen2, FTE, Raven's original, anything else
}
```

That is the whole mechanism. `hexenwail` is read-only and always numeric.

## Why a cvar and not `checkextension()`

`checkextension()` exists in this engine, but **you cannot use it portably**,
and that is not a temporary state of affairs.

Builtin numbers differ. `checkextension` is **#130 here and #99 in DarkPlaces
and FTE**. That is deliberate: Hexen II already spends #99 on
`matchAngleToSlope`, and the numbers near the end of Raven's table mean
different things in different builds — #113 is `PF_Fixme` in the shipping
build, `PF_cos` under `QUAKE2` and `PF_name_print` in HexenWorld, and H2W runs
to #120. There is no free number that means one thing everywhere, so `#130`
opens a Hexenwail range above all of Raven's numbering.

A mod that writes `float(string s) checkextension = #130;` therefore gets a
garbage builtin under FTE, and `= #99` gets `matchAngleToSlope` here.

`cvar()` is **#45 in every Hexen II engine**, including FTE's Hexen II support.
It is the only probe that compiles and behaves the same everywhere.

This engine's `checkextension()` also currently advertises nothing and returns
0 for every name — it records the question so `developer 1` can report what a
mod looked for, which is how we decide what to implement next. Ask for what you
need on the issue tracker.

## The value

`MAJOR * 1000000 + MINOR * 10000 + PATCH * 100 + REV`

| Release | `cvar("hexenwail")` |
|---|---|
| 0.8.0-beta.r31 | `80031` |
| 0.9.0-beta.r1 | `90001` |
| 1.0.0 | `1000000` |

It increases across every component, so a plain `>=` is always the right test.
The alpha/beta phase is **not** encoded: it would have to sort somewhere, and
no mod should branch on it.

`0` means *not this engine*. An unknown cvar reads as 0 on every engine, and
that is the detection mechanism — so the name `hexenwail` will never be reused
for anything else.

## What you may assume

- **It is read-only.** `CVAR_ROM`. A player cannot set it from the console or a
  config to fake the answer, so you may trust it as an engine assertion rather
  than a user preference.
- **It is not archived.** It is deliberately kept out of `config.cfg`: a value
  written by one build and read back by another would be lying about the engine
  it is in.
- **It is registered before any QuakeC runs.** `SV_Init`, which both the client
  and `h2ded` run, so it is readable from server-side QuakeC on both.
- **It cannot drift from the release.** The value is stamped from
  `HW_VERSION_NUM` in `engine/hexen2/quakedef.h`, and
  `engine/tests/version_cvar_test.c` fails the build if those numbers ever
  disagree with `HW_BASE_VERSION`.

## Other engines

If you need to tell FTE apart as well, ask Spike for FTE's equivalent probe and
branch on both — `cvar()` works there too, so the shape of the test is the
same. We do not document another engine's cvars here, because we cannot promise
they will keep working.
