# Gamecode (`progs.dat`) — building, installing, precedence

`gamecode/` holds the HexenC sources for the Hexen II game logic. `hcc`
compiles them to the progs bytecode the server VM runs. This document covers
the `gamecode` flake output added under `uhexen2-zmb3`: how to build it, how to
install the result, what it overrides, and what is still undecided.

## Why the output exists

Until this landed, nothing in the build read `gamecode/` at all. Two things
followed from that, both bad:

1. **A `.hc` edit that did not compile would land green.** Nothing checked.
2. **Every gamecode fix was inert for players.** We ship no `progs.dat`, so a
   player runs Raven's retail 1997 one. `uhexen2-9r3n` (`DropBackpack` silently
   deleting mana/health-only drops) was fixed in `bb570666a` and reached
   nobody; the `BadBackpackDump` instrumentation in `uhexen2-hwky` is the only
   way to root-cause the remaining field reports in `uhexen2-9gdx`, and testers
   on retail progs cannot run it.

This output fixed (1) and made (2) possible. `uhexen2-8qp3` then carried the
compiled gamecode in the release bundles under `gamecode/`, staged for the
player to copy, and `uhexen2-xsmc` finished (2) by making the engine load it
from beside its own executable — so the fix now reaches players who copy
nothing. See [Shipping](#shipping-in-the-release-bundle) and
[BUNDLED_GAMECODE.md](BUNDLED_GAMECODE.md).

## Building

```sh
nix build .#gamecode
```

Needs only `hcc` from `.#utils`; no SDL, GL or codecs. A few seconds per tree.

All four HexenC trees are compiled, not just Hexen II's. A gamecode fix
normally has to land in three of them at once — `bb570666a` touched `h2`, `hw`
and `portals` — so gating only `h2` would let the other two break silently,
which is the failure this output exists to prevent.

`h2` alone omits `-oi`/`-on`. Per `gamecode/COMPILE`, those two optimizations
make the engine emit warnings when loading pre-existing Hexen II saves. The
other trees have no such legacy to protect. Do not "make it consistent".

### Output layout

The layout matches gamedir names, so the tree drops straight onto an install.
It is also the layout upstream uses in `gamedata-all-1.29b.tgz`.

| Path under `result/share/hexenwail/` | Source tree | Bytes at `ce8c9d1c4` |
|---|---|---|
| `data1/progs.dat` | `gamecode/hc/h2` | 888,368 |
| `data1/progs2.dat` | `gamecode/hc/h2` (`-name progs2.src`) | 838,712 |
| `portals/progs.dat` | `gamecode/hc/portals` | 1,147,444 |
| `hw/hwprogs.dat` | `gamecode/hc/hw` | 953,380 |
| `siege/hwprogs.dat` | `gamecode/hc/siege` | 796,276 |

Those sizes are a snapshot for sanity-checking a build, not a contract — any
gamecode edit moves them.

The build is bit-reproducible (`nix build .#gamecode --rebuild` reports no
differences), so a local build and CI's produce identical bytes. Worth knowing
for the bundling question: it means an artifact can be verified against a
rebuild rather than trusted.

`hw/` and `siege/` are HexenWorld progs and this fork builds no HexenWorld
engine, so today they are compile-gate artefacts only. They are installed
rather than discarded so a HexenWorld server operator can use them, and so
nobody is tempted to "simplify" away the build steps that gate them.

### Source filtering

`packages.gamecode` uses `gamecodeSrc`, a filter separate from the engine's
`filteredSrc`, scoped to `gamecode/hc`. This is deliberate and load-bearing:
adding `gamecode` to `filteredSrc`'s allowlist would make every `.hc` edit
rebuild the engine, `h2ded`, both mingw cross builds, the WASM build and the
toolchain, and invalidate every cached build of them for every user — while a
`.hc` file cannot affect one byte of any of those. Verified against a pristine
`bb570666a` worktree: the `.drv` paths of `nixos`, `h2ded`, `win64`,
`h2ded-win64`, `utils` and `utils-win64` are unchanged by this commit.

### CI

`ci.yml` runs `nix build .#gamecode`, so a `.hc` change that breaks `hcc` fails
the build. `ci.yml` and `release.yml` build named packages one at a time and
never sweep all of `packages` — a new output is invisible to CI until it is
named explicitly. Keep it that way.

`packages.release` references `gamecode` and stages three of its five files.
See [Shipping](#shipping-in-the-release-bundle).

## Installing

This section is the source-checkout path — a developer running the engine out
of a build tree, where nothing sits beside the binary to be found.

**Players do not do any of this.** Since `uhexen2-xsmc` a release bundle loads
its own gamecode from beside the executable on both platforms, so a normal
install copies nothing; see [Shipping](#shipping-in-the-release-bundle) and
[BUNDLED_GAMECODE.md](BUNDLED_GAMECODE.md). What follows is also the recipe for
overriding that bundle with some other `progs.dat`, since `~/.hexen2` outranks
it. To use it from a checkout:

```sh
nix build .#gamecode
mkdir -p ~/.hexen2/data1
cp result/share/hexenwail/data1/progs.dat  ~/.hexen2/data1/
cp result/share/hexenwail/data1/progs2.dat ~/.hexen2/data1/
```

`~/.hexen2` is the per-user data directory on Unix
(`engine/h2shared/userdir.h :: SYS_USERDIR_UNIX`); demo builds use
`~/.hexen2demo` (same macro, demo `#ifdef`). Windows and OS/2 have no user
directory — `engine/h2shared/sys.h :: DO_USERDIRS` is forced to 0 there — so on
those a build-tree run needs the files beside the executable, in
`<exedir>/gamecode/<gamedir>/`, which is the bundle's own first lookup layer.
Do not reach for the install's `data1/` instead: that overwrites Raven's loose
`progs.dat` irreversibly (see *Retail keeps progs.dat loose*), and the bundle
outranks it anyway, so it would not even take effect.

Prefer the user directory where you have one. It takes precedence over the
install (see below), and uninstalling is `rm` on files you own rather than an
edit to the game directory.

**Copy `progs.dat` and `progs2.dat` together.** `progs2.dat` is never requested
by that name. A `maplist.txt` found on the search path maps individual maps to
a progs file, and the engine re-reads it on every map spawn
(`engine/h2shared/pr_edict.c :: PR_GetProgFilename()`, gated by
`engine/h2shared/progs.h :: USE_MULTIPLE_PROGS`). Installing one without
the other leaves the game on a new progs for most maps and Raven's 1997 progs
for the rest — a mismatch that will not announce itself.

To undo, delete the files. Nothing else is needed, but nothing else will remind
you either: the filename carries no version, and the only report of which progs
loaded is a `Con_DPrintf` in `pr_edict.c :: PR_LoadProgs()` ("Loaded %s, v%d,
%d crc"), which requires `developer 1`. A stale copy is therefore invisible
and permanent until removed by hand. That is the main hazard of this install
path and the reason it is not automated.

## Precedence — confirmed, not assumed

**A mod's own `progs.dat` reliably beats one we place in `data1/`.
Unconditionally.** This was traced rather than assumed, because it governs
whether bundling could ever be safe.

The mechanism, in order:

- The search path is a **prepend-only list** — later additions land at the
  head. `engine/h2shared/quakefs.c :: FS_AddGameDirectory()` does
  `search->next = fs_searchpaths; fs_searchpaths = search;` for each pak and
  again for the directory itself, and gives every new gamedir
  `path_id = fs_searchpaths->path_id << 1`.
- **Lookup returns the first hit and stops.**
  `quakefs.c :: FS_OpenFile_Internal()` walks
  `for (search = fs_searchpaths; search; search = search->next)` and returns
  from inside the loop on the first pak hit or loose hit.
- **`data1` is added first**, in `quakefs.c :: FS_Init()` step 1 — so it ends
  up at the *tail*, i.e. lowest priority.
- **The mod gamedir is added last**, by `FS_Init()`'s `-game`/`-mod` handling
  at the end of the function, which calls `FS_Gamedir()` →
  `FS_AddGameDirectory()` — so it ends up at the head.
- **`progs.dat` is loaded by plain relative name through that search path**,
  with no basedir or gamedir qualification: `PR_GetProgFilename()` returns
  `def_progname`, i.e. `"progs.dat"`, and `pr_edict.c :: PR_LoadProgs()` calls
  `FS_LoadHunkFile(progname, NULL)`.

Resulting order for `-game sot`, head to tail:

```
sot/ → sot/pakN.pak → portals/ → portals/pak3.pak → data1/ → data1/pak1.pak → data1/pak0.pak
```

Every combination resolves to the mod: loose or packed on either side makes no
difference, because *all* of the mod's entries sit above *all* of `data1`'s.

Two related facts, both verified:

- **Within one directory, loose files beat paks.** Hexen II inverts Quake here
  on purpose — `FS_AddGameDirectory()` adds the directory itself *after* its
  paks, and says why in a comment: "so that the dir itself will be placed above
  the pakfiles in the search order which, in turn, will allow override files".
  It is what would let a loose `data1/progs.dat` override a packed copy — but
  see below: in practice there is no packed copy to override.
- **The user directory beats the install directory.** One
  `FS_AddGameDirectory()` call adds the basedir's paks and directory, then
  loops back to add the userdir's paks and directory — later, so higher.

The engine already protects mods from a `data1` `maplist.txt` hijacking their
progs: `PR_GetProgFilename()` compares `path_id`s and logs `"ignored
maplist.txt from a gamedir with lower priority"`.

### What this means for bundling

The verdict above is about *mods*. The same mechanism has a consequence for
what we ship that is easy to miss, so state it plainly:

**Priority is mod > portals > data1, and the unit of priority is a whole
gamedir.** An entire later gamedir outranks an earlier one, paks included —
there is no per-file arbitration between them. So:

- **With the mission pack active, a `data1/progs.dat` we ship is never read.**
  `portals/progs.dat` wins outright.
  And "active" is a much wider condition in this fork than the name suggests:
  five flags set `check_portals` in `quakefs.c :: FS_Init()` step 2 —
  `-portals`, `-missionpack`, `-h2mp`, `-game <dir>` and `-mod <dir>`. Only
  `-noportals` overrides, and it is checked first.
- The same fall-through applies to mods: one that ships no `progs.dat` of its
  own lands on `portals/progs.dat`'s Mission Pack v1.12 progs, *not* `data1`'s
  v1.11 — with or without anything we install. `-noportals` restores the
  `data1` fallback.
- **A bare launch with no arguments does reach `data1/progs.dat`.** That is
  the one common case where a shipped `data1` progs is the one that loads.

The practical upshot, settled by `uhexen2-8qp3`: shipping only the `h2` tree
would silently miss every Portals player. Both `data1/` and `portals/` go in
the zip, and both were verified end-to-end (below).

### Retail keeps progs.dat loose — the paks hold no copy

Checked against a retail install (Hexen II + Portal of Praevus) rather than
assumed, because the safety of the whole install path turns on it:

| Pak | Entries | `*.dat` entries |
|---|---|---|
| `data1/pak0.pak` | 696 | none |
| `data1/pak1.pak` | 523 | none |
| `portals/pak3.pak` | 245 | none |

The gamecode ships as **loose files**: `data1/PROGS.DAT`, `data1/PROGS2.DAT`
(uppercase) and `portals/progs.dat` (lowercase). `pak0.pak` does contain
`maplist.txt`, which names `progs2.dat` in lowercase.

`maplist.txt` lives in **`data1/pak0.pak` only** — 193 bytes, 10 entries, every
one of them mapping to `progs2.dat`. `pak1.pak` has no copy and `pak3.pak` has
neither a `maplist.txt` nor any `.dat` entry, so the entire `progs2.dat`
mechanism is reachable only through that single 193-byte file at `path_id` 1.
Half of what it names is unreachable: of its ten maps, `rick3`–`rick7` ship in
no retail pak at all. The five that do exist are `rider1a`, `rider2c`, `meso9`,
`romeric6` and `eidolon`, all in `pak1.pak`.

**The count a player actually meets is five.** Scanning the entity lump of all
42 `.bsp` files in `pak0.pak` and `pak1.pak` for `"map"` keys, every one of
those five is the target of a `trigger_changelevel` in the normal campaign
flow, and `rick3`–`rick7` are named by nothing at all:

| progs2.dat map | reached from |
| --- | --- |
| `rider1a` | `village2` |
| `rider2c` | `egypt1` |
| `meso9` | `meso8` |
| `romeric6` | `romeric5` |
| `eidolon` | `castle5` |
| `rick3`–`rick7` | *(no `"map"` key in any shipped map)* |

So `gamecode/README.txt` can state the number outright rather than hedging with
"some": the five above use `progs2.dat`, every other map uses `progs.dat`.
Reproduce with `scripts/progs2_maps.py` against a retail install.

Two consequences:

1. **Overwriting a player's `progs.dat` is irreversible.** There is no packed
   copy to fall back on, so "just delete the loose file" does not restore Raven's
   gamecode the way it would in Quake. This is why the release bundle stages the
   files for a deliberate copy instead of overlaying `data1/` directly — on
   Windows an overlay would destroy the retail gamecode at unzip time.
2. **A fresh install made only from `pak0.pak` + `pak1.pak` has no gamecode
   at all** and cannot start a map. Anyone assembling `data1/` by hand needs
   the loose files too.

### Case: our lowercase `progs.dat` beats retail's `PROGS.DAT`, deterministically

We install `progs.dat` lowercase; retail's is `PROGS.DAT`. On Linux both then
sit loose in `data1/` at once, so which one loads is a real question — and the
answer is not readdir order.

`quakefs.c :: FS_OpenFile_Internal()` tries the **exact-case path first** and
only falls back to a case-insensitive directory scan if that misses:

```c
q_snprintf (ospath, sizeof(ospath), "%s/%s", search->filename, filename);
fs_filesize = Sys_filesize (ospath);        /* exact case, tried first */
#ifndef PLATFORM_WINDOWS
if (fs_filesize < 0)                        /* only on a miss */
{
    if (FS_ResolveCasePath (search->filename, filename, ospath))
        fs_filesize = Sys_filesize (ospath);
}
#endif
```

The engine asks for `"progs.dat"` — `pr_edict.c :: def_progname`, lowercase —
so with our file present the exact-case `stat` hits immediately and
`FS_ResolveCasePath()` never runs. `PROGS.DAT` is not even scanned. That is
also *why* retail's uppercase file loads today on Linux: exact case misses, and
the fallback scan matches it case-insensitively.

`progs2.dat` resolves the same way. Its name comes verbatim from the second
column of `maplist.txt`, and `pak0.pak`'s copy spells it lowercase.

`FS_ResolveCasePath()`'s `readdir` order is only reachable when *no* file
matches the requested case exactly, which is not our situation. Verified by
running the engine, not by reading alone — see below.

Note the cosmetic wart: on Linux a copy into `data1/` leaves both `progs.dat`
and `PROGS.DAT` there. Ours wins over retail's, the retail file is inert, and
that is effectively a free backup. On Windows the filesystem is
case-insensitive, so copying `progs.dat` over `PROGS.DAT` replaces its contents
and no backup survives — which is why `gamecode/README.txt` says to back up
first in the one case that still calls for this copy, feeding the files to a
different engine. Nothing on the normal Hexenwail path performs it.

## Shipping in the release bundle

`uhexen2-8qp3`. Both release zips carry a top-level `gamecode/` directory:

```
gamecode/
  README.txt
  data1/progs.dat      888,368
  data1/progs2.dat     838,712
  portals/progs.dat  1,147,444
```

### Three files, not five

`hw/hwprogs.dat` and `siege/hwprogs.dat` are built but **not shipped**. No
retail HexenWorld bytecode exists to compare them against, and this fork builds
no HexenWorld engine, so they would be untested bytes. Deferred to
`uhexen2-nr9l`. `flake.nix` names the three shipped files individually rather
than copying the tree, so adding a sixth is a deliberate edit and a vanished
path fails the build.

### Why one shared directory, not one per platform

Progs bytecode is platform-independent. Three per-platform copies would add
~5.7 MB to the download to say the same thing three times. More importantly the
files do not belong beside the executable at all — they belong in the player's
*game data* directory, which is a different place from the engine on every
platform.

So the layout is the instruction: the tree under `gamecode/` mirrors the game
directory, and `gamecode/data1/progs.dat` → `<install>/data1/progs.dat` needs no
explanation beyond itself. This matches `packages.gamecode`'s own gamedir layout
and `packages.demodata`'s "gamedirs under a basedir" convention.

### Why it is staged, not overlaid

The Windows zip is a flat overlay — it already contains a `data1/` meant to be
extracted straight into a Hexen II folder — so putting `progs.dat` there would
install it automatically. That was rejected: retail keeps `progs.dat` loose with
no packed copy (above), so an overlay silently destroys the player's only copy of
Raven's gamecode at unzip time, before they have read anything.

The cost was real and worth naming: an opt-in copy meant most players would not
perform it, so a shipped fix still did not reach them. That is the trade the
project recorded under *Installing* — "a stale copy in `data1/` is invisible and
permanent until removed by hand… the reason it is not automated". Automating an
irreversible overwrite would have contradicted it.

Of the two honest ways out named here — an installer that backs up first, or an
engine that loads our progs from its own directory rather than the game's —
`uhexen2-xsmc` took the second. It reaches every player without writing to the
game folder at all, so the overwrite hazard above never arises and the staged
tree stops being an instruction: it is now only a source for hand-feeding a
different engine.

### Verified end-to-end

Against a real retail install (Hexen II + Portal of Praevus) copied to a scratch
tree, using `h2ded -developer`, which prints `Programs occupy NNNK` and
`Loaded <name>, v6, <crc> crc`:

| Case | Retail | After installing ours |
|---|---|---|
| `data1/progs.dat` (bare launch) | 930K | **867K** |
| `data1/progs2.dat` (via `maplist.txt`, map `eidolon`) | 877K | **819K** |
| `portals/progs.dat` (`-portals`) | 1147K | **1120K** |

Each "after" figure is the shipped file's size in KiB, so every one of the three
is confirmed to be what actually loads — including `data1/progs.dat` while
retail's `PROGS.DAT` sits beside it untouched.

**Use a map that exists.** This table originally cited `map rick3`, which is in
neither `pak0.pak` nor `pak1.pak` (above). The check appeared to pass anyway,
because `sv_main.c :: SV_SpawnServer()` calls `PR_LoadProgs()` well before it
calls `Mod_ForName()` for the worldmodel — so `progs2.dat` loads and prints its
size, and *then* the map load fails. A nonexistent map produces a
correct-looking `progs2.dat` line, which is exactly the shape of result that
survives review. Use `eidolon`, or any of `romeric6` / `meso9` / `rider1a` /
`rider2c`, and confirm the map actually spawns.

Functional confirmation, stronger than sizes:
`tools/qcdis.py <progs> --list BadBackpackDump` finds the `uhexen2-hwky`
diagnostic in both shipped Hexen II and Portals files and in neither retail
file — so a tester on a shipped build can finally produce it.

### Player-visible consequences for the release notes

Both are stated in `gamecode/README.txt`, `BUILD_INFO.txt` and the generated
release notes:

1. **"Vanilla" now means two things.** A bug report may be against our gamecode
   or Raven's, and the reporter cannot tell by looking. Ask. Removing the files
   is the fastest bisection available. This cuts both ways — some past reports
   will have been gamecode bugs already fixed here.
2. **Crusader's Glyph of the Ancients damages the caster.** `TimeBombTouch`
   halves `self.dmg` and calls `T_Damage` on whoever touches it — 50 in
   deathmatch, 37 otherwise — with no owner exemption. Confirmed identical to
   retail portals 1.12 by disassembly, so this is *not* a change for a player
   coming from stock progs; it is a change only against the fork's own
   intermediate gamecode, which briefly early-returned on
   `other == self.owner`. Restored by `ce8c9d1c4` (`uhexen2-qj8s`).

## Licensing

**Finding: materially better established than the demo data, but no explicit
grant document exists.** Recorded so the bundling pass does not have to redo
the search.

What is established:

- **The upstream sources are Raven's own public releases**, not decompiled or
  leaked material. uHexen2's `README-hcode_archive.txt` states that "h2-1.11
  and portals-1.12a are the official hcode releases from Raven" and that
  "hw-src-release is the official hcode release from Raven". `gamecode/hc/siege/siegesrc.txt`
  is Raven's own Mike Gummelt (`mgummelt@mail.ravensoft.com`) handing the Siege
  source to modders directly.
- **Upstream distributes the compiled artifact, and has for about two
  decades.** `gamedata-all-1.29b.tgz` on uHexen2's SourceForge contains
  `data1/progs.dat`, `data1/progs2.dat`, `portals/progs.dat`,
  `hw/hwprogs.dat` and `siege/hwprogs.dat` as compiled binaries, and its own
  notes say "These game-data are to be included in Hammer of Thyrion v1.5.9
  binary packages".
- **Upstream draws the line between the two categories explicitly, in the same
  tarball.** That archive ships Raven's *pak* content only as xdelta patches
  (`patchdat/data1/data1pk0.xd3`) to be applied against the user's own retail
  paks — the same "point at it, don't serve it" rule `assets/demo/README.md`
  arrives at for the demo data — while shipping `progs.dat` outright. Upstream
  puts progs on the redistributable side of that line deliberately.
- **We already redistribute the inputs.** `gamecode/` is in this repository and
  in every clone of it. There is no coherent position in which publishing
  `gamecode/hc/h2/items.hc` is fine but publishing its compiled form is not.
  Compiling adds no redistribution exposure that the repository does not
  already carry.

What is *not* established: no `COPYING`, `LICENSE` or per-file copyright header
accompanies the gamecode — not in this tree, and not in upstream's
`gamedata-src-1.29b.tgz` either. The only licence assertion covering it is
structural: uHexen2's release script copies `docs/COPYING` (GPLv2) to
`LICENSE.txt` at the root of a tree containing `gamecode-<ver>/`
(`scripts/mkrelease.sh`).

### Why this does not mirror the `demodata` caution

`packages.demodata` is marked `licenses.unfree` with `hydraPlatforms = []` and
kept out of CI and the cache (`uhexen2-3vmk`, `uhexen2-118f`). That caution is
not transferred here, and the reason is that the fact patterns are inverted:

| | `demodata` | `gamecode` |
|---|---|---|
| Inputs in this repo | No — gitignored, user fetches them | Yes, tracked |
| Rights-holder published the source | No | Yes, for modding |
| Only licence found | Activision retail EULA, forbids transfer | None; upstream ships GPL at tree root |
| Upstream ships the binary | No — patches only | Yes, for ~20 years |

So `gamecode` carries `licenses.gpl2Plus`, matching every other output in the
flake and upstream's own release layout, and CI builds it normally.

If someone wants certainty *before bundling into a release artifact* — a
stronger bar than compiling in CI — the missing item is an explicit grant
covering Raven's 1997/1998 hcode releases. Nothing short of a document changes
the answer; two decades of upstream practice is evidence about practice, not
about rights.

## Not yet decided

Two of the questions `uhexen2-zmb3` left open were answered by the owner: we
**keep serving the compiled progs through cachix**, and we **bundle it in the
release zip**. Both are done — bundling landed in `uhexen2-8qp3`, above. It was
not free: it changes what "vanilla" means for every bug report we receive, a
cost paid on every future issue rather than once.

Still out of scope, and not to be inferred from the existence of this output:

- **Savegame and demo compatibility across a progs change.** Note that the
  `-oi`/`-on` omission for `h2` exists precisely because save compatibility is
  sensitive to progs layout, so this is not hypothetical.
- **What happens when a user mixes our `data1/progs.dat` with a mod that ships
  its own.** Precedence is settled above — the mod wins — but "the mod wins" is
  the answer to file lookup, not to whether the resulting combination is
  *correct*. A mod built against Raven's v1.11 progs and running alongside our
  `data1` progs is a configuration nobody has tested.

Resolved since: **reporting which progs is loaded.** It was two `Con_DPrintf`
calls in `PR_LoadProgs()`, invisible without `developer 1`, which left "which
one is this player running?" unanswerable from a normal console log.
`uhexen2-zixe` added the unconditional `Gamecode:` line naming the file's path,
version and whole-file CRC. That line is now what the release notes and
`gamecode/README.txt` ask bug reporters to paste.

`uhexen2-8r3e` then made it say *whose* code it is, which the path cannot:

```
Gamecode: progs.dat from …/data1/PROGS.DAT   (H2/v1.11,  file crc 17499) -- Raven 1.11
Gamecode: progs.dat from …/portals/progs.dat (H2MP/v1.12, file crc 20799) -- Raven 1.12a
Gamecode: progs.dat from …/share/hexenwail/data1/progs.dat (H2/v1.11, file crc 49692) -- hexenwail-2026-08-15
Gamecode: progs.dat from …/karma2/progs.dat  (H2MP/v1.12, file crc 22850) -- Third-party
```

Raven is matched on the three retail whole-file CRCs, which are fixed forever.
Ours is matched on `HexenwailGamecode_YYYYMMDD`, a marker function every tree
carries via `gamecode/hc/<tree>/ident.hc` — a *function* because `hcc` strips
global names under `-on` (`hcc.c :: WriteData()`) in exactly the trees that need
it, while function names always survive. That is also why the date rides in the
name: the one form that could hold a value is the one that loses its name. It
perturbs neither the entity field table nor the progdefs CRC; see that file for
why. Older builds of ours, which predate the marker, still identify through a
`BadBackpackDump` fallback, and degrade to a bare `hexenwail` with no date — as
does any stamp that fails to parse, since printing part of an unrecognised tail
as a date would be a confident lie.

**The date is a source constant, restamped by hand.** A build timestamp would
end `.#gamecode`'s bit-reproducibility. Two `checkPhase` gates hold it honest:
all five images must carry the same stamp, and it must equal the newest dated
entry in `gamecode/README`. The second is the load-bearing one — the fork's
policy is already that every divergence gets a dated README entry, so the build
fails at exactly the moment a change is recorded without restamping.

The same string appears in Game Options as a read-only `Gamecode loaded:` row
beneath the `Gamecode:` toggle. It is drawn as one full-width line rather than
the label/value pair the rows above use: the value column starts at x=220 on a
320-unit canvas, which leaves 12 characters, and `hexenwail-2026-08-15` is 20. The toggle is the player's *intent* and can disagree
with reality: a `progs.dat` hand-copied into the install's `data1/` — the
pre-bundle install method above — is loaded by the "Classic" setting and is
ours. The row is absent until a map has loaded, which is also the honest answer
on a client attached to someone else's server.

Field context: Tome of Power Abuser pulled r11 expecting the backpack fix and
asked why there is still a bug and no `progs.dat` in the zip. `uhexen2-zmb3`
answered the build half; `uhexen2-8qp3` answered the shipping half, leaving the
fix reaching only players who read the README and performed a manual copy.

`uhexen2-xsmc` closed that gap — a drop-in engine now brings its own gamecode
and loads it from beside its own executable, so **no copy is needed on either
platform** and the fix reaches every player who unzips a release.
`uhexen2-qoy7` then ranked the user directory above it, so a player who wants
different gamecode still wins. The design analysis, including the two
mechanisms that look obvious and are both wrong, is in
[BUNDLED_GAMECODE.md](BUNDLED_GAMECODE.md).
