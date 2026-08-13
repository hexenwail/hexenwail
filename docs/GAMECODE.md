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

This output fixes (1) and makes (2) *possible* — it does not fix (2). Building
the gamecode is not the same as shipping it; wiring the artifacts into the
release is `uhexen2-8qp3`.

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

`packages.release` does **not** reference `gamecode`. Release artifacts are
engine-only today; `uhexen2-8qp3` is the work to change that.

## Installing

Nothing installs this for you. To use it:

```sh
nix build .#gamecode
mkdir -p ~/.hexen2/data1
cp result/share/hexenwail/data1/progs.dat  ~/.hexen2/data1/
cp result/share/hexenwail/data1/progs2.dat ~/.hexen2/data1/
```

`~/.hexen2` is the per-user data directory on Unix
(`engine/h2shared/userdir.h :: SYS_USERDIR_UNIX`); demo builds use
`~/.hexen2demo` (same macro, demo `#ifdef`). Windows and OS/2 have no user
directory — `engine/h2shared/sys.h :: DO_USERDIRS` is forced to 0 there — so
the files go into the install's own `data1/` beside `pak0.pak`.

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
  This is what makes a loose `data1/progs.dat` override `pak0.pak`'s copy at
  all.
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
  `portals/progs.dat`, or the copy inside `portals/pak3.pak`, wins outright.
  And "active" is a much wider condition in this fork than the name suggests:
  five flags set `check_portals` in `quakefs.c :: FS_Init()` step 2 —
  `-portals`, `-missionpack`, `-h2mp`, `-game <dir>` and `-mod <dir>`. Only
  `-noportals` overrides, and it is checked first.
- The same fall-through applies to mods: one that ships no `progs.dat` of its
  own lands on `portals/pak3.pak`'s Mission Pack v1.12 progs, *not* `data1`'s
  v1.11 — with or without anything we install. `-noportals` restores the
  `data1` fallback.
- **A bare launch with no arguments does reach `data1/progs.dat`.** That is
  the one common case where a shipped `data1` progs is the one that loads.

The practical upshot, and this is a live decision in `uhexen2-8qp3`: shipping
only the `h2` tree would silently miss every Portals player. Both `data1/` and
`portals/` have to go in the zip for the mission pack audience to receive a
gamecode fix at all.

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

Two of the questions `uhexen2-zmb3` left open have since been answered by the
owner: we **keep serving the compiled progs through cachix**, and we **bundle
it in the release zip** (`uhexen2-8qp3`). That does not make bundling free —
it changes what "vanilla" means for every bug report we receive, a cost paid on
every future issue rather than once, so the release notes should say which
gamecode a build carries.

Still out of scope, and not to be inferred from the existence of this output:

- **Savegame and demo compatibility across a progs change.** Note that the
  `-oi`/`-on` omission for `h2` exists precisely because save compatibility is
  sensitive to progs layout, so this is not hypothetical.
- **What happens when a user mixes our `data1/progs.dat` with a mod that ships
  its own.** Precedence is settled above — the mod wins — but "the mod wins" is
  the answer to file lookup, not to whether the resulting combination is
  *correct*. A mod built against Raven's v1.11 progs and running alongside our
  `data1` progs is a configuration nobody has tested.

Field context for whoever picks this up: Tome of Power Abuser pulled r11
expecting the backpack fix and asked why there is still a bug and no
`progs.dat` in the zip. The build pipeline is the answer to half of that.
