# Bundled gamecode — shipping `progs.dat` beside the engine binary

`uhexen2-xsmc`. Design analysis for making a drop-in engine bring its own
gamecode, so that a player who unzips a release gets our `progs.dat` fixes
without performing a manual copy into their own `data1/`.

Companion to [GAMECODE.md](GAMECODE.md), which covers what the build produces
and how to install it by hand from a checkout. Before this landed, that document
stood on *"installing it is still a manual copy, so the fix reaches only players
who read the README"*. This one is the answer to that sentence.

**Status: implemented.** `uhexen2-xsmc` landed the substitution in
`PR_LoadProgs()` (`PR_FindBundleDir` / `PR_BundledProgsPath`, `pr_edict.c`), the
shared `Sys_GetExeDir()` it needs, and the per-platform bundle in `flake.nix`;
`uhexen2-qoy7` added the user-directory stand-down and `uhexen2-zixe` the
`Gamecode:` startup line. This document is kept as the design record — read
every "we recommend X" as "X is what shipped".

The player-facing consequence, which the two texts below are written against:
**a normal install copies nothing on either platform.** On Linux the engine
finds `<exedir>/../share/hexenwail/`; on Windows the release zip is flat, so
`gamecode/` already sits beside `glh2.exe` and satisfies lookup layer 1.

**Citation convention: `file.c :: Symbol()`, never `file.c:LINE`.** This
document cited bare line numbers until `uhexen2-2z1z`, and they went stale four
separate times in a single day — any commit inserting a line anywhere above a
citation silently invalidates it, and nothing checks. Symbols move with their
code and a vanished one is greppable. Same convention as
[SrcNotes.txt](SrcNotes.txt) and [GAMECODE.md](GAMECODE.md). Please keep it:
re-pinning numbers only buys another day.

## Verdict

It can be done safely — but **not as a search-path insertion, and not below
`data1`.**

The recommendation is a narrow substitution inside `PR_LoadProgs()`, gated on
two independent conditions, with the bundle discovered relative to the running
executable the way `soundfont.c` already discovers a bundled soundfont.

The property that makes it defensible: **mods are untouched by construction,
not by convention.** No rule anyone has to remember, no reviewer discipline, no
comment that a future edit can quietly invalidate.

## Findings

Eight findings drove the recommendation. F1 and F2 kill the two designs that
look obvious first; the rest constrain what is left.

### F1 — a searchpath entry below `data1` is inert on every real install

The intuitive design is "add our bundle as a lowest-priority gamedir, so
anything the player has wins". It does nothing.

`quakefs.c :: FS_OpenFile_Internal()` stats the exact-case path first
and falls back to `FS_ResolveCasePath()`'s `readdir` scan only on a miss:

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

On Linux against a retail install the exact-case stat for `"progs.dat"` misses
(retail ships `PROGS.DAT`), the case-insensitive fallback scan finds it, and the
lookup returns **from inside `data1`**. On Windows the filesystem is
case-insensitive, so the exact-case stat hits directly. Either way the searchpath
walk *stops at `data1`* and never reaches anything below it.

The same holds for `~/.hexen2/data1/progs.dat` (the userdir entry shares the
gamedir's `path_id` and sits above the basedir entry), and for the Nov 1997 demo,
whose `data1/` carries a lowercase loose `progs.dat` that hits on the first stat.

So a synthetic lowest-priority gamedir is **not a delivery mechanism**. It is
only a *rescue* mechanism, for an install that has no gamecode at all — a case
that today cannot start a map anyway.

The dangerous part is not that it fails. It is that **it would pass a "nothing
broke" review precisely because it does nothing.** Every regression test passes,
every mod behaves, and no player ever loads our progs.

### F2 — giving our files a new `path_id` silently kills `progs2.dat`

`pr_edict.c :: PR_GetProgFilename()` contains:

```c
FH.length = FS_OpenFile (maplist_name, &FH.file, &id1);
FH.pak = file_from_pak;
if (FH.file == NULL)
    return def_progname;
else if (FS_FileExists(def_progname, &id0) && id1 < id0)
{
    Con_DPrintf("ignored %s from a gamedir with lower priority\n", maplist_name);
    goto _fail;                                 /* discard the maplist */
}
```

`id1` is the `path_id` of the gamedir `maplist.txt` came from; `id0` is
`progs.dat`'s. The guard exists so a `data1` `maplist.txt` cannot hijack a mod's
gamecode.

`maplist.txt` exists in exactly one place in a retail install — `data1/pak0.pak`,
`path_id` 1. It is 193 bytes, ten entries, every one of them naming `progs2.dat`.
`pak1.pak` has no copy and `pak3.pak` has neither a `maplist.txt` nor any `.dat`
entry.

Now suppose a bundled searchpath entry takes `path_id = fs_searchpaths->path_id
<< 1` (`quakefs.c :: FS_AddGameDirectory()`) like every other gamedir, and suppose it wins the
`progs.dat` lookup. Then `progs.dat` resolves at `path_id` 2 while `maplist.txt`
still sits at 1. `id1 < id0` becomes true, the guard fires, the maplist is
discarded — and **`progs2.dat` never loads on any map**, on any install, forever.

The only report is a `Con_DPrintf` requiring `developer 1`. The game does not
crash. The five reachable `progs2.dat` maps (`eidolon`, `romeric6`, `meso9`,
`rider1a`, `rider2c`) simply run the wrong gamecode.

Any searchpath-based design must therefore share `data1`'s `path_id`. That is a
correct-but-load-bearing subtlety with **no compiler check and no test to
enforce it** — exactly the kind of invariant that survives the commit that
introduces it and dies three commits later.

### F3 — `fs_gamedir_nopath` is reliable, with one pre-existing exception

`fs_gamedir_nopath` (`quakefs.c`, file scope) is the engine's own answer to "which game
am I in". It is set by `quakefs.c :: FS_AddGameDirectory()` and by
`quakefs.c :: Host_Game_f()`, and is correct at every call site
except one:

The **invalid-mission-pack rollback** (`quakefs.c :: Host_Game_f()`) unwinds the
searchpath and restores `fs_gamedir` / `fs_userdir` to `data1`:

```c
fs_searchpaths = mark;
/* back to data1 */
FS_MakePath_BUF (FS_BASEDIR, NULL, fs_gamedir, sizeof(fs_gamedir), "data1");
FS_MakePath_BUF (FS_USERBASE,NULL, fs_userdir, sizeof(fs_userdir), "data1");
```

— but leaves `fs_gamedir_nopath` reading `"portals"`.

This is pre-existing and not caused by anything here. It also already makes
`Host_Game_f`'s "already running that game" check wrong in
that state: a player in the rolled-back configuration who selects Portals from
the Mods menu is told they are already in it.

Worth fixing on its own merits, and worth knowing about before keying a gate on
that variable.

### F4 — runtime gamedir switching is first-class, so launch-time gates are stale

`quakefs.c :: Host_Game_f()` rebuilds the searchpath at runtime and can
add `portals` with a fresh `path_id`:

```c
/* optionally add portals as base for custom mods */
if (use_portals && q_strcasecmp(dir, "data1") && q_strcasecmp(dir, "portals"))
{
    q_snprintf (path, sizeof(path), "%s/portals", host_parms->basedir);
    if (Sys_FileType(path) == FS_ENT_DIRECTORY)
    {
        FS_AddGameDirectory ("portals", true);
        fs_base_searchpaths = fs_searchpaths;
    }
}
```

Consequence: **any rule keyed on `com_argv` or `COM_CheckParm("-game")` is stale
the moment the player uses the Mods menu**, in both directions. Launched bare
and switched into a mod, a `-game`-based gate keeps substituting inside the mod.
Launched with `-game` and switched back to Hexen II, it refuses to substitute
when it should.

Gating must be **recomputed from filesystem state at every `PR_LoadProgs()`** —
that is, per map spawn (`sv_main.c :: SV_SpawnServer()`).

### F5 — `basedir` cannot express "beside the binary"

`basedir` is `getcwd()` on Unix (`sys_unix.c :: Sys_GetBasedir()`) and `GetCurrentDirectory()` on Windows (`sys_win.c :: Sys_GetBasedir()`).
It is the working directory the game was *launched from*, which for a desktop
launcher, a Steam shortcut or a file-manager double-click is frequently not the
install directory.

`soundfont.c` already solved this, with a `static const char *exe_dir (void)`
that resolved `/proc/self/exe` — but it was `static`, Linux-only, and lived in a
translation unit `h2ded` does not compile: `soundfont.c` reaches only
`ALL_SOURCES` via the two `SOUNDFONT_SOURCES` assignments in
`engine/CMakeLists.txt` (one per MIDI backend that needs it: FluidSynth and the
libTiMidity fallback), consumed where `ALL_SOURCES` is assembled, while `h2ded`
is built from the separate `SERVER_SOURCES` list in the `BUILD_DEDICATED` block
(`engine/CMakeLists.txt :: SERVER_SOURCES`).

So it needed promoting to a shared `Sys_GetExeDir()` with per-platform backings
(`/proc/self/exe`, `GetModuleFileName`), removing the duplicate rather than
adding a second one. **That is what shipped**: `uhexen2-hgi3` moved it to
`engine/h2shared/sys_exedir.c :: Sys_GetExeDir()`, declared in
`engine/h2shared/sys.h`, compiled into both the client and `h2ded`;
`soundfont.c` now calls it rather than carrying its own copy.

### F6 — the Windows package already has the right shape

The Windows release zip is flat, and `gamecode/` is already a root-level
subdirectory sitting beside `glh2.exe` (`.github/workflows/release.yml`, the `Package` step):

```sh
winpkg=hexenwail-windows
mkdir -p "$winpkg/data1"
cp staging/windows-x86_64/bin/* "$winpkg/"
...
cp -r staging/gamecode "$winpkg/"
```

So `<exedir>/gamecode/data1/progs.dat` is **already the shipped path on
Windows**. No packaging change at all.

Only Linux needs a layout change: there the executable lives at
`linux-x86_64/bin/` (`flake.nix`, the `release` derivation's Linux portable tree) while `gamecode/` is at the zip root,
so `<exedir>/gamecode/` does not exist.

### F7 — the CRC table is a real safety net

`PR_LoadProgs()` (`pr_edict.c :: PR_LoadProgs()`, crc switch) switches on `progs->crc` against a
fixed table of known-good values and `Host_Error`s on anything else:

```c
switch (progs->crc) {
#if !defined(H2W) /* HEXEN2 PROGS: */
case PROGS_V103_CRC:  def = globals_v103; progvstr = "H2/v1.03";  break;
case PROGS_V111_CRC:  def = globals_v111; progvstr = "H2/v1.11";  break;
case PROGS_V112_CRC:  def = globals_v112; progvstr = "H2MP/v1.12"; break;
...
default:
    Host_Error ("Unexpected crc ( %d ) for %s", progs->crc, progname);
```

A structurally incompatible bundled progs therefore **fails loudly at map load**
rather than running silently against the wrong globals layout. There is also a
version check immediately above it (`PROG_VERSION_V6` / `V7`).

(This finding predates 59343680d, which added ~130 lines to `pr_edict.c`
ahead of `PR_LoadProgs()` for the bundle-substitution path; the crc switch
itself is unchanged in content, just further down the file than when this was
written. Citation deliberately left symbolic rather than re-pinned — see the
note under F5.)

This is a genuine safety net and worth naming: the worst outcome of shipping a
broken bundle is a clear error naming the file, not corrupted gameplay.

### F8 — ground truth on this machine

Measured, not assumed, against a retail install plus the four mods present:

| Path | Case | Form | Size |
|---|---|---|---|
| `data1/PROGS.DAT` | uppercase | loose | 952,888 B |
| `data1/PROGS2.DAT` | uppercase | loose | 898,060 B |
| `portals/progs.dat` | lowercase | loose | 1,174,956 B |

Neither `data1` file has a lowercase twin. No pak in the install contains a
`.dat` entry of any kind.

| Mod | `progs.dat` | `progs2.dat` | `maplist.txt` | paks |
|---|---|---|---|---|
| `sot` | loose, 1,421,514 B | — | — | — |
| `soc` | loose, 1,257,778 B | — | — | — |
| `karma2` | loose, 1,076,806 B | — | — | — |
| `GameOfTomes` | loose, 1,072,312 B | — | — | — |

All four ship a loose lowercase `progs.dat`; **none ships `progs2.dat` or
`maplist.txt`.**

Because of that, **none of the four is affected by any candidate design.** Each
one's own `progs.dat` wins the lookup at its own `path_id` before any bundle is
consulted. The entire mod exposure of this feature lives in the *gamecode-less*
mod case — pure map packs, which supply maps and nothing else and therefore run
`data1`'s gamecode.

## Candidates

### Candidate A — searchpath overlay in `FS_AddGameDirectory()`

**Placement**: inside `quakefs.c :: FS_AddGameDirectory()`, after the
gamedir's own basedir and userdir entries are pushed, insert one more
`searchpath_t` pointing at `<bundle>/<dir>` — **sharing the gamedir's
`path_id`** rather than taking a new one.

**What it gets right**: it is the engine's own idiom. Everything the filesystem
layer already does — case fallback, pak enumeration, `FS_FileExists`, path_id
reporting — works unchanged, because the bundle is just another directory on the
path. `path` prints it, so the configuration is self-documenting.

**What it risks**:

- **F2 is live and unenforced.** The shared-`path_id` requirement is the whole
  reason `progs2.dat` keeps working, and nothing in the code or the test suite
  states it. A later refactor that "tidies up" the `path_id` assignment to match
  its neighbours breaks `progs2.dat` on every install, reported only under
  `developer 1`.
- **Blast radius is every filename in the bundle directory.** A searchpath entry
  is a standing offer to shadow *anything*. Ship one stray `default.cfg`,
  `strings.txt` or `.mdl` in the bundle tree — now or in five years — and it
  overrides the player's. The design intends to be about two files; the
  mechanism is not.
- **`Host_Game_f`'s runtime portals add is a second call site.** `quakefs.c :: Host_Game_f()`
  calls `FS_AddGameDirectory("portals", true)` at runtime; both paths must
  behave identically or the Mods menu produces a different filesystem than the
  command line did.
- **On Windows there is no userdir.** `fs_userdir` collapses onto the install
  directory, so the bundle entry outranks the player's own `data1/progs.dat`
  with no per-file escape short of deleting the bundle.

### Candidate B — explicit-path substitution in `PR_LoadProgs()`

**Placement**: `pr_edict.c :: PR_LoadProgs()`, between
`PR_GetProgFilename()` and `FS_LoadHunkFile()`:

```c
progname = PR_GetProgFilename();
progs = (dprograms_t *)FS_LoadHunkFile (progname, NULL);
```

becomes a load that first consults the bundle, by absolute path, for that exact
filename — and falls through to the existing `FS_LoadHunkFile()` unchanged
whenever the gate says no.

**What it gets right**: it cannot affect any file other than the one
`PR_GetProgFilename()` just named — `progs.dat` or `progs2.dat`, and nothing
else, ever. It never participates in the F2 comparison at all, because
`PR_GetProgFilename()` has already finished by the time the substitution
happens: the maplist and `progs.dat` are still both compared at their real
`path_id`s in the real searchpath. It is recomputed per map spawn, so F4's
runtime switching is handled for free.

**What it risks**: `path` no longer tells the whole truth. The searchpath the
console prints will show `data1` while the loaded gamecode came from somewhere
else entirely. That is a real diagnostic cost and it is why the startup print
becomes **mandatory** under this design rather than a nice-to-have.

### Candidate C — synthetic lowest-priority gamedir

**Placement**: a `FS_AddGameDirectory()`-like call at the *bottom* of the
searchpath during `FS_Init()`, below `data1`.

**Rejected by F1.** On every real install — retail on Linux, retail on Windows,
and the Nov 1997 demo — the `data1` lookup succeeds and the walk never descends
to it. Its only unique capability is rescuing an install that has no gamecode at
all, which is not the problem being solved.

## Recommendation — Candidate B (this is what shipped)

Where to read the code this section argues for:

| Design element | Shipped as |
| --- | --- |
| bundle discovery, three layers | `pr_edict.c :: PR_FindBundleDir()` |
| the gating predicate | `pr_edict.c :: PR_BundledProgsPath()` |
| the substitution itself | `pr_edict.c :: PR_LoadProgs()`, via `FS_LoadHunkFileFromOSPath()` |
| resolving `<exedir>` | `sys_exedir.c :: Sys_GetExeDir()` |
| the opt-out | `-vanillaprogs`, checked in `PR_BundledProgsPath()` |
| the startup provenance line | the `Gamecode:` print, `uhexen2-zixe` / `f619026e1` |

Four reasons it was chosen, in weight order:

1. **Bounded blast radius.** B cannot shadow anything but the two files it is
   about. A searchpath entry is a standing offer to shadow any filename that
   ever lands in that directory, by anyone, at any future date. This is the
   reason that outweighs the rest even if every other consideration were a tie.

2. **`progs2.dat` is correct by construction, not by discipline.** B never
   enters the `id1 < id0` comparison in `PR_GetProgFilename()`, so F2 is not a
   risk that has been *mitigated* — it is a risk that does not exist. Candidate
   A requires a future maintainer to know why a `path_id` looks anomalous.

3. **The rule reads like the requirement.** "If the progs we are about to load
   is the base game's own, use ours instead" is one sentence, and the code is
   the same sentence. A searchpath insertion expresses the same intent
   indirectly, through priority arithmetic, three functions away from
   `PR_LoadProgs()`.

4. **It fails closed in every constructible state**, including runtime gamedir
   switches, the F3 stale-`nopath` state, a missing bundle, and a partially
   extracted one. The default in every unrecognised situation is "load exactly
   what the engine loads today".

### The gating predicate

```c
/* Decide whether the progs about to be loaded should come from the
 * bundle instead of the player's game directory.  Recomputed on every
 * call: PR_LoadProgs() runs per map spawn, and the gamedir can change
 * at runtime through Host_Game_f (the Mods menu).  See F4. */
static qboolean PR_BundledProgsPath (const char *progname,
                                     char *out, size_t outsz)
{
	unsigned int	path_id;
	const char	*bundle;

	/* Off switch: command line only, never a cvar.  See "Off switch". */
	if (COM_CheckParm("-vanillaprogs"))
		return false;

	if (!(bundle = PR_FindBundleDir()))       /* no bundle shipped/extracted */
		return false;

	/* Condition 1 -- is this the BASE GAME'S file?
	 * Ask the filesystem where progname actually resolves.  path_id 1 is
	 * data1, assigned first in FS_AddGameDirectory (quakefs.c:400-401).
	 * Anything else means a mod or portals owns this file, and we do not
	 * touch it. */
	if (!FS_FileExists(progname, &path_id))
		return false;                     /* no gamecode at all: unchanged error */
	if (path_id != 1U)                        /* 1U == data1, quakefs.c:401 */
		return false;

	/* Condition 2 -- is the base game the game the PLAYER IS IN?
	 * fs_gamedir_nopath is the engine's own answer (quakefs.c:87). */
	if (q_strcasecmp(fs_gamedir_nopath, "data1") != 0)
		return false;

	q_snprintf (out, outsz, "%s/data1/%s", bundle, progname);
	return Sys_FileType(out) == FS_ENT_FILE;
}
```

and the `portals` arm is the same shape with `"portals"` on both sides — its
`path_id` being the one `portals` was assigned, and `fs_gamedir_nopath` reading
`"portals"`.

Note the ordering: **`FS_FileExists` is asked first and its answer is used**,
rather than assuming where the file lives. That is what makes the predicate
robust against every searchpath arrangement the engine can produce, including
ones nobody has thought of yet.

### Why both conditions, and not either one

They answer different questions, and each alone has a hole.

- `path_id` answers **"is this the base game's file?"** — it is a fact about
  where the bytes came from.
- `fs_gamedir_nopath` answers **"is the base game the game the player is in?"**
  — it is a fact about intent.

**Hole in `path_id` alone**: a pure map pack (`-game mappack`, no `progs.dat` of
its own) resolves `progs.dat` from whichever gamedir beneath it has one. On a
mission-pack install that is **`portals`**, not `data1`, because `-game` by
itself pulls `portals` onto the path (`quakefs.c :: FS_Init()`, pushed at
`:1698-1701`); only where `portals/` is absent or `-noportals` was passed does
it fall to `data1` at `path_id` 1. Either way the resolved `path_id` is one we
ship a bundle for, so condition 1 passes, and substituting there would silently
change the gamecode underneath a mod — the precise thing this design promises
not to do. Condition 2 catches both, because `fs_gamedir_nopath` reads
`"mappack"`.

The `portals` route is the common one, since it is taken by essentially every
mod launch; it is failure mode 1, and it is the reason this AND cannot be
reduced to its first term.

**Hole in `fs_gamedir_nopath` alone**: launch `-portals` against an install
whose `portals/` has no `progs.dat` (or where the mission pack rolled back,
F3). `fs_gamedir_nopath` reads `"portals"`, but the file that actually resolves
is `data1`'s. Substituting the *portals* bundle for a *data1* file swaps in
gamecode for a different gamedir than the one that supplied it. Condition 1
catches it, because the resolved `path_id` is `data1`'s.

Neither hole is exotic. Both are reachable from the shipped Mods menu.

### Worked example: `-game sot` → Mods menu → "Hexen II"

1. **Launch `glhexen2 -game sot`.** `FS_AddGameDirectory("sot")` pushes the mod
   at a fresh `path_id` and sets `fs_gamedir_nopath = "sot"`.
2. **Map spawn.** `PR_LoadProgs()` at `sv_main.c :: SV_SpawnServer()`. `PR_GetProgFilename()`
   returns `"progs.dat"` — `sot` ships no `maplist.txt` (F8), so the maplist
   path is not taken. `FS_FileExists("progs.dat", &path_id)` resolves inside
   `sot/` and reports `sot`'s id. **Condition 1 fails. No substitution.** Sot's
   own 1,421,514-byte gamecode loads, exactly as today.
3. **Player opens the Mods menu and selects "Hexen II".** `Host_Game_f()`
   unwinds to `fs_base_searchpaths` and sets `fs_gamedir_nopath = "data1"`
   (`quakefs.c :: Host_Game_f()`). The searchpath is `data1` at `path_id` 1.
4. **Next map spawn.** `PR_LoadProgs()` runs again — per spawn, not per launch.
   `progs.dat` now resolves from `data1` at `path_id` 1: **condition 1 passes.**
   `fs_gamedir_nopath` reads `"data1"`: **condition 2 passes.** Our bundled
   `data1/progs.dat` loads.

The point of the example is step 4. A gate written as
`if (COM_CheckParm("-game")) return false;` would still be looking at a command
line containing `-game sot` and would refuse to substitute — forever, for the
rest of the session. And the mirror case (launch bare, switch *into* `sot` via
the menu) would have that same gate cheerfully substituting inside the mod.
Both directions are broken by any launch-time gate; F4 is not theoretical.

## Scope decision — gamedir identity, not fall-through

**Ship and prefer `portals/progs.dat`, but only when `portals` is the actual
gamedir.**

The two substitutions are not equally risky and should not be justified
together:

- The **`data1`** substitution is *structurally* mod-proof. A mod that ships
  gamecode wins condition 1; a mod that does not is caught by condition 2.
  There is no configuration in which it reaches a mod.
- The **`portals`** substitution is the only one that could reach one, because
  `portals` is deliberately used as a *base* for custom mods
  (`quakefs.c :: Host_Game_f()`, `-mod`). Gating on `fs_gamedir_nopath` removes that
  exposure completely.

The cost of gating that way is explicit and accepted: **a pure map pack layered
over portals keeps retail gamecode.** Those players do not get our fixes.

That is the right default for a first release. Widening it later — to "fall
through to the bundle whenever the resolved file is the base game's, regardless
of the current gamedir" — is one line and one condition removed. Narrowing it
after a mod breakage is a rollback, a point release, and a credibility cost with
mod authors. The asymmetry decides it.

The same reasoning declines the F1 rescue case: when no gamecode exists at all,
condition 1's `FS_FileExists` fails and the engine produces its existing error.
Rescuing that install is a widening available later, not a requirement now.

## Off switch — `-vanillaprogs`

A command-line flag. Not a cvar, and not presence-of-file alone.

**Why not a cvar.** Cvars are mutable mid-session and `PR_LoadProgs()` runs per
map spawn, so a cvar would let the gamecode change *between levels of one
campaign* — the worst possible time, and a state that no bug report would ever
describe accurately. Worse, a `CVAR_ARCHIVE` one would write the setting into
the player's own config tree, making an engine-packaging decision persist into
an install the engine may not own.

**Why not presence-of-file alone.** "Delete the bundle" is destructive to the
download, and on Windows — where the bundle sits beside `glh2.exe` among the
DLLs — it is undiscoverable. It should nonetheless keep working as the ultimate
escape hatch, because a mechanism that survives a corrupted config is worth
having.

**Why that name.** `-noprogs` and `-nogamecode` both read as "load no progs at
all", which is not what happens and would be a reasonable thing for someone to
try. `-vanillaprogs` restores exactly the meaning of "vanilla" that
[GAMECODE.md](GAMECODE.md#player-visible-consequences-for-the-release-notes)
records us having lost — a bug report can say "reproduced with `-vanillaprogs`"
and mean something precise.

**Where it goes.** The `help_strings[]` tables and `USAGE.md`. Note that there
are **three** such tables, not four: `engine/hexen2/sys_unix.c :: help_strings[]`,
`engine/hexen2/server/sys_unix.c :: help_strings[]` and `engine/hexen2/server/sys_win.c :: help_strings[]`.
`engine/hexen2/sys_win.c` is a `WinMain` GUI entry point with no `help_strings`
and no `PrintHelp` — nothing to add there.

## Bundle location

Follow `soundfont.c :: SF_FindSoundFont()`'s layering, in
order:

1. **`<exedir>/gamecode/`** — the portable/zip layout. Already the shipped path
   on Windows (F6).
2. **`<exedir>/../share/hexenwail/`** — the FHS/Nix layout. This is already
   exactly where `packages.gamecode` installs (`flake.nix`, `packages.gamecode`'s `installPhase`:
   `$out/share/hexenwail/data1`, `.../portals`), so the convention costs nothing
   to adopt.
3. **A compile-time `-DBUNDLED_GAMECODE_DIR=...`**, mirroring `SOUNDFONT_PATH`
   (`engine/CMakeLists.txt`, `add_definitions(-DSOUNDFONT_PATH=...)` inside the
   `FLUIDSYNTH_FOUND` branch), for distribution packagers who put the
   data somewhere neither of the above finds.

All three resolve through `Sys_GetExeDir()` (F5), never through `basedir`.

## Packaging consequences

**Windows: no change.** `gamecode/` is already a root-level directory beside
`glh2.exe` (F6).

**Linux: add `share/hexenwail/{data1,portals}` under `linux-x86_64/`** —
roughly 2.9 MB (867K + 819K + 1120K) added to the Linux zip. The root-level
`gamecode/` staging tree stays exactly as it is, because it remains the
manual-install path and the thing `gamecode/README.txt` describes.

This originally landed under `linux-x86_64-nixos/` as well. That tree has since
been dropped from `.#release` entirely (uhexen2-2tia): `.#nixos` leaves the
binary's RPATH pointing at absolute `/nix/store` paths, so it is startable only
on the machine that built it, and NixOS users are served by the flake
(`nix run github:bobberb/hexenwail`) rather than by a download.

**Do not reference `${gamecode}` from the engine derivation.** Doing so would
put the gamecode derivation into the engine's `.drv` hash, and every `.hc` edit
would then rebuild the engine, `h2ded`, both mingw cross builds, the WASM build
and the toolchain — destroying exactly the property `gamecodeSrc` exists to
protect (`flake.nix :: gamecodeSrc`). The `release` derivation may reference it,
because `release` is an aggregate whose whole job is to depend on everything;
the engine derivation may not.

## Failure modes

Thirteen, in an arc: 1–5 are ways this feature could **reach a mod**, 6–9 ways
it could **override the player**, 10–11 ways it could **degrade badly**, and
12–13 what it costs **us**.

1. **`portals` joins the searchpath on the mere presence of `-game`, so a
   `path_id` test alone would substitute under every gamecode-less mod.** This
   is the largest single divergence from the model of the world Candidate A
   assumes, and it is the entire reason the gate is a two-condition AND.

   Any of `-portals`, `-missionpack`, `-h2mp`, `-game <dir>` or `-mod <dir>`
   sets `check_portals` (`quakefs.c :: FS_Init()` — the `#else` arm is the live
   one, since no build in this tree defines `H2MP` or `H2W`), and `portals` is
   then pushed onto the path (`quakefs.c :: FS_Init()`). So `glhexen2 -game
   mappack` yields `data1`, `portals`, `mappack`, and a mappack with no gamecode
   of its own resolves `progs.dat` **from `portals`** — not from `data1`.

   Under a `path_id`-only rule the portals arm's condition 1 passes there, and
   our `portals/progs.dat` runs underneath every gamecode-less mod on the
   machine: precisely the thing this design promises not to do. What stops it is
   condition 2, `fs_gamedir_nopath` reading `"mappack"`.

   It is the "hole in `path_id` alone" stated above, in the form it actually
   takes on most machines: that argument holds whether the map pack resolves
   from `data1` or from `portals`, but the `portals` route is the one nearly
   every mod launch takes, and it needs no unusual install to reach.

   **Do not "simplify" the AND away.** The second condition looks redundant on
   any install where `portals/` is absent, which is exactly the install a
   maintainer is most likely to be testing on.

2. **Mods that ship only part of the gamecode set.** All four mods present ship
   `progs.dat` and nothing else — no `progs2.dat`, no `maplist.txt` (F8). With
   one of them active, `data1`'s `maplist.txt` is rejected by the priority test
   (`id1` is 1, `id0` is the mod's, so `id1 < id0` fires) and the mod's single
   `progs.dat` serves every map, including the five that would otherwise want
   `progs2.dat`. That is the behaviour today and this feature does not change it
   in either direction.

   The interesting variant is hypothetical: a mod shipping its own `maplist.txt`
   naming a file it does not itself ship. The maplist survives the priority test
   — both ids are the mod's — `PR_GetProgFilename()` returns `"progs2.dat"`, and
   the lookup falls through to `portals` or `data1` for that name. Candidate B
   declines to substitute, because `fs_gamedir_nopath` is the mod. Correct, and
   conservative in the direction we want: a half-specified intent gets retail
   bytes, not ours.

3. **A stale launch-time gate after a runtime gamedir switch (F4).** Substituting
   inside a mod, or refusing to substitute in the base game, for the rest of the
   session. Prevented by recomputing per `PR_LoadProgs()`; reintroduced by any
   future caching of the gate result.

4. **Stale `fs_gamedir_nopath` after the invalid-mission-pack rollback (F3).**
   `nopath` reads `"portals"` while the searchpath is `data1`. Condition 1 makes
   this fail closed rather than substitute the wrong tree, but the underlying
   inconsistency should be fixed on its own merits.

5. **`progs2.dat` silently dropped (F2).** Only reachable under Candidate A, and
   only observable with `developer 1`. Listed because it is the failure this
   design is shaped to make impossible, and because that shaping is the reason
   to prefer B.

6. **Non-registered data — demo, OEM and mix'n'match installs.** Our gamecode is
   built from the 1.11 tree. The Nov 1997 demo and the OEM/bundle release are
   different versions of the game, and the demo additionally ships a loose
   lowercase `progs.dat` that wins the first stat (F1) inside a tree whose
   gamecode we have never compared against ours.

   Gate the whole feature on `gameflags & GAME_REGISTERED`, set only when `pak0`
   *and* `pak1` both match the 1.11 fingerprints (`quakefs.c :: FS_Init()`, from
   the `GAME_REGISTERED0` / `GAME_REGISTERED1` returns at `quakefs.c :: check_known_paks()`).
   One line removes an entire class of unknowns from the test matrix. Unlike the
   two conditions it is also *not* a per-spawn question: the fingerprinting
   happens once in `FS_Init` and the answer cannot change for the session.

   It does not subsume the F1 rescue case the scope decision declines, and
   should not be confused with it: a retail install with its `progs.dat` deleted
   is still `GAME_REGISTERED`, and is declined by condition 1 instead. Two
   independent gates, both failing closed, for two different reasons.

7. **A player who deliberately installed a different `progs.dat`** — a
   translation, a balance patch, a hand-built experiment dropped into `data1/`.
   Under Candidate B their file resolves at `path_id` 1 in the game they are in,
   which is exactly the state that says "substitute". The feature overrides a
   choice they made on purpose.

   **Settled by a third condition — a stand-down, giving the ordering
   `user directory > bundle > install directory`** (`uhexen2-qoy7`, landed after
   the audit that reproduced the problem). Conditions 1 and 2 decide that the
   bundle *may* substitute; condition 3 decides that it *must not*, because the
   player has installed their own copy in `~/.hexen2/<gamedir>/`.

   The two losing locations are not symmetric, and that is the whole reason this
   is decidable rather than a coin-flip. The install's `data1/PROGS.DAT` is
   Raven's file and superseding it **is** the feature — every retail tree has
   one, so ranking it above the bundle would leave the bundle dead code on every
   real install. But nothing creates `~/.hexen2/data1/progs.dat`: not retail, not
   any installer, not the engine (see *Do not do this*). A file there is a
   deliberate act, and it is the only statement of intent the engine gets.

   **`path_id` cannot make this distinction**, which is why the stand-down is a
   separate filesystem probe rather than a refinement of condition 1.
   `FS_AddGameDirectory` assigns a gamedir's basedir entry and its userdir entry
   the **same** `path_id`: it is computed once at `quakefs.c :: FS_AddGameDirectory()` and stored
   on both entries at `:445`, the second time round the `goto add_pakfile` loop
   the userdir pass takes (`:453-461`). Both therefore read `1U` for `data1` and
   the predicate has no way to tell them apart — the same fact F1 relies on when
   it says the userdir entry "shares the gamedir's `path_id`". The audit
   demonstrated it: a foreign `progs.dat` in `~/.hexen2/data1/` and the same file
   in the install's `data1/` both lost to the bundle.

   `FS_UserdirHasFile()` (`quakefs.c`) answers it instead. It resolves the same
   way `FS_OpenFile_Internal` resolves a loose file, case-fold included, so a
   hand-copied `PROGS.DAT` is seen exactly when the searchpath would have seen
   it. Verification row B5 pins the behaviour down.

   **The recourse is still not the same on both platforms, and the asymmetry
   has to be documented rather than discovered.** On Windows `DO_USERDIRS` is
   forced to 0 (`sys.h :: DO_USERDIRS`), `fs_userdir` collapses onto the install
   directory, and there is no location that expresses "mine, not yours".
   `FS_UserdirHasFile()` compiles to a constant `false` there, deliberately: a
   probe of the userdir on Windows would be a probe of the install directory,
   which every retail tree populates, and the feature would be inert. A Windows
   player's `<install>/data1/progs.dat` loses to the bundle, so their recourse
   is `-vanillaprogs`, or **replacing the file inside `gamecode/` beside
   `glh2.exe`** — the bundle is our file, not Raven's, so overwriting it risks
   nothing and re-extracting the zip undoes it. Prefer that to "delete the
   bundle": it is reversible and it keeps the other two gamedirs intact.

   Same user action, two behaviours, decided by a `#define` in `sys.h`. It
   belongs in the release notes, not only here. `gamecode/README.txt` states the
   split under *RUNNING SOME OTHER GAMECODE*, where the players who hit it will
   read it: `~/.hexen2/` on Unix, the `gamecode/` folder on Windows. Neither is
   framed as an install step, because on both platforms the bundle is already
   live and the normal player copies nothing.

8. **Savegames crossing a progs change.** This is *the* genuinely new risk
   relative to `uhexen2-8qp3`. The manual copy let the player pick the moment;
   bundling makes it happen on an engine upgrade, potentially mid-campaign.

   *Mitigating*: saves are text, keyed by field **name** rather than offset; an
   unknown field produces `Con_Printf ("'%s' is not a field\n", ...)`
   (`pr_edict.c :: ED_ParseEdict()`) and is skipped, not a failure; `SAVEGAME_VERSION` is
   unchanged (`quakedef.h :: SAVEGAME_VERSION`, currently 5); and the `h2` tree deliberately
   omits `-oi`/`-on` for exactly this reason (`flake.nix`, `packages.gamecode`'s `buildPhase` — the two `h2` `hcc` lines).

   *Aggravating*: the `portals` tree **is** built with `-oi -on`
   (`flake.nix`, `packages.gamecode`'s `buildPhase` — the `portals` `hcc` line). The portals cross-load is therefore the one to test, and
   the one most likely to warn.

9. **Demo playback across a progs change.** Demos replay recorded network
   traffic, but stat and effect semantics can shift with gamecode. Old demos
   under new progs, and vice versa, are both reachable by any player who keeps a
   `.dem` around.

10. **A structurally incompatible bundled progs.** Caught loudly by the CRC and
    version checks (F7) with a `Host_Error` naming the file, at map load rather
    than at startup. Contained, but the message a player sees is a CRC number,
    which is not self-explanatory.

11. **Absent or partially extracted bundle.** A player who extracts only `bin/`,
    or a distro package that omits the data, must get today's behaviour exactly.
    `PR_FindBundleDir()` returning NULL and the final `Sys_FileType` check are
    both fail-closed for this reason; a bundle containing `data1/progs.dat` but
    not `portals/progs.dat` must degrade per-file, not per-install.

12. **Diagnostic opacity.** `path` will show `data1` while the running gamecode
    came from the bundle, so a developer reading a pasted console log draws the
    wrong conclusion. This is Candidate B's stated cost and the reason the
    unconditional startup print (`uhexen2-zixe`, landed in `f619026e1`) is a
    hard prerequisite rather than a companion nicety: it is the only thing that
    puts the real path into a log nobody thought to ask for.

13. **"Vanilla" gets harder to say, not easier.** `uhexen2-8qp3` turned "which
    gamecode is this player running?" into a question that has to be asked on
    every bug report; this feature makes *our* gamecode the default answer
    rather than an opt-in. That is not a one-time cost paid at merge — it is
    paid again on every future issue, by whoever triages it.

    Two things keep it tractable: the startup print (failure mode 12), which
    makes the answer appear in any pasted log without being asked for, and
    `-vanillaprogs`, which turns "reproduce without our gamecode" into a
    one-word instruction. **This is why neither of them is optional.** Shipping
    the substitution without both of them converts every report into a round
    trip.

## Verification matrix

This is the handoff to whoever implements. **Every row runs under both `h2ded`
and the GL client** — `h2ded` because it is the fastest way to read
`Con_DPrintf` output, and the GL client because it is what ships and because
only it has a Mods menu.

### Precedence

| # | Configuration | Expected progs |
|---|---|---|
| P1 | bare launch | bundled `data1/progs.dat` |
| P2 | `-portals` | bundled `portals/progs.dat` |
| P3 | `-noportals` | bundled `data1/progs.dat` |
| P4 | `-game sot` | `sot/progs.dat` (untouched) |
| P5 | `-game soc` | `soc/progs.dat` (untouched) |
| P6 | `-game karma2` | `karma2/progs.dat` (untouched) |
| P7 | `-game GameOfTomes` | `GameOfTomes/progs.dat` (untouched) |
| P8 | each of P1–P7 `+ -vanillaprogs` | the player's own file in every case |
| P9 | gamecode-less mod (pure map pack) | retail `data1/PROGS.DAT` — **not** the bundle (scope decision) |
| P10 | `data1` gamecode hidden/renamed away | unchanged "couldn't load progs.dat" error — the bundle does **not** rescue |

P8 is the row that proves the off switch, and it must be run for all seven
configurations, not just the bare one. P9 and P10 are the two rows where the
expected result is "the new feature does nothing"; both are deliberate, and both
will look like bugs to anyone who has not read the scope decision.

### Behaviour

| # | Check | Method |
|---|---|---|
| B1 | `progs2.dat` still loads | `map eidolon` — **not `rick3`**, which is in no retail pak; see [GAMECODE.md](GAMECODE.md#verified-end-to-end). Confirm the map actually spawns, then confirm the `progs2.dat` size line. |
| B2 | `progs2.dat` on the other four | `romeric6`, `meso9`, `rider1a`, `rider2c` |
| B3 | runtime gamedir switching | Mods menu: Hexen II → `sot` → Hexen II → Portals, spawning a map after each switch. This is the F4 row and the one no command-line test covers. |
| B4 | case handling | Linux install with uppercase `PROGS.DAT` only; then with both cases present; confirm which loads and that it matches the documented rule |
| B5 | userdir override | `~/.hexen2/data1/progs.dat` present — **the player's file must load, not the bundle** (F7). Then the same file in the install's `data1/` instead: **the bundle must still win**, which is the feature. Both rows in one run, since the difference between them is the whole point. |
| B6 | absent bundle | delete `<exedir>/gamecode/`; engine must behave exactly as today |
| B7 | partial bundle | `data1/progs.dat` present, `portals/progs.dat` absent; must degrade per file |
| B8 | functional proof | `tools/qcdis.py <progs> --list BadBackpackDump` — present in the bundled files, absent in retail. This is the row that proves the *right bytes* loaded, rather than merely that *some* file did. |

### Savegames — four crossings

| # | Save made under | Loaded under |
|---|---|---|
| S1 | retail `data1` progs | bundled `data1` progs |
| S2 | bundled `data1` progs | retail `data1` progs (`-vanillaprogs`) |
| S3 | retail `portals` progs | bundled `portals` progs |
| S4 | bundled `portals` progs | retail `portals` progs (`-vanillaprogs`) |

S3 and S4 are the ones to watch: the `portals` tree is built with `-oi -on`
(failure mode 8). A `'%s' is not a field` print is acceptable; a `Host_Error`,
a lost inventory or a broken objective is not.

### Demos

| # | Check |
|---|---|
| D1 | a demo recorded on retail progs, played back on bundled progs |
| D2 | a demo recorded on bundled progs, played back with `-vanillaprogs` |
| D3 | the shipped `.dem` files, if any, under both |

### Packaging

| # | Check |
|---|---|
| K1 | unzip the Linux artifact to a clean directory, run from **outside** it (`cd /tmp && /path/to/linux-x86_64/bin/glhexen2`) — this is the row that proves F5, since `basedir` will be wrong by construction |
| K2 | ~~same for `linux-x86_64-nixos`~~ — n/a, that tree is no longer built into `.#release` (uhexen2-2tia). Note that the flake install path carries **no** bundled gamecode: `.#nixos` installs `bin/glhexen2` and an empty `share/hexenwail/`, so `nix run` users run retail gamecode. Tracked as uhexen2-9die |
| K3 | unzip the Windows artifact into a retail Hexen II directory and run in place |
| K4 | confirm the root-level `gamecode/` staging tree is still present and still matches its README |
| K5 | confirm the engine derivation's `.drv` hash is unchanged by a `.hc` edit |

K1 is not optional. Testing from inside the extracted directory makes `basedir`
and `<exedir>` coincide, which is precisely the case that cannot distinguish a
correct implementation from one that reads `basedir`.

## Do not do this

The most valuable section for a future reader. Each item is a design that looks
reasonable, and each is wrong for a reason recorded above.

- **Never copy the bundle into the player's `data1/`, even with a backup.**
  Retail ships `progs.dat` loose with no packed copy (GAMECODE.md, *Retail keeps
  progs.dat loose*). An automated overwrite destroys the player's only copy of
  Raven's gamecode. "We took a backup" is not an answer: the backup is a file in
  a directory the player did not ask us to write to, and it will be deleted,
  moved or forgotten.

- **Never write into `~/.hexen2/data1/` either.** It looks safer because it is
  "our" tree, and it is not: it is the player's config tree, it takes precedence
  over the install, and a stale copy there is invisible and permanent until
  removed by hand.

- **Never add the bundle above `-game` / `-mod`.** A mod's gamecode wins. That
  is not a preference, it is the contract mod authors have relied on for two
  decades.

- **Never give a bundled searchpath entry a fresh `path_id`.** It silently kills
  `progs2.dat` on every install, reported only under `developer 1` (F2).

- **Never place a bundled searchpath entry below `data1`.** It is inert on every
  real install, and it passes review *because* it is inert (F1).

- **Never gate on `COM_CheckParm("-game")` or anything else derived from
  `com_argv`.** Runtime gamedir switching is first-class; the command line stops
  describing reality the moment the player opens the Mods menu (F4).

- **Never make the off switch a cvar.** `PR_LoadProgs()` runs per map spawn, so
  a cvar lets gamecode change between levels of one campaign, and a
  `CVAR_ARCHIVE` one writes into the player's tree.

- **Never reference `${gamecode}` from the engine derivation.** It puts the
  gamecode into the engine's input hash and makes every `.hc` edit rebuild the
  entire toolchain — the exact outcome `gamecodeSrc` was written to prevent.

- **Never ship `hw` or `siege` progs in the bundle.** No retail HexenWorld
  bytecode exists to compare them against and this fork builds no HexenWorld
  engine (`flake.nix`, and `uhexen2-nr9l`). Gate the whole feature
  `#if !defined(H2W)` so this stays true by construction rather than by the
  packaging script's choice of filenames.

## Prerequisites

In dependency order:

1. **`Sys_GetExeDir()`** — promote `soundfont.c`'s `static exe_dir()` to a
   shared, cross-platform helper reachable from `h2ded` (F5).
2. **The unconditional startup print** (`uhexen2-zixe`) — **already satisfied**,
   landed in `f619026e1`. Mandatory under Candidate B rather than a nicety
   (failure modes 12 and 13); kept on this list so that a future reader treats
   it as a dependency of the substitution and not as an unrelated coincidence
   that may be reverted.
3. **Fix `fs_gamedir_nopath` in the mission-pack rollback** (F3) — independently
   correct, and it removes one fail-closed corner from this feature's state
   space.
