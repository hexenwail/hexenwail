# Hexen II mod corpus — reference set for the Mods menu

Source: hexenworld.org's "Add-Ons, Levels & Tools" collection, preserved at
<https://discmaster.textfiles.com/browse/43363> as
`h2_hexenworld_dot_org_mods.zip` (39 mod directories, ~130 MB, archived
2016-01-01, after hexenworld.org went down in March 2025).

This is the closest thing that exists to a complete snapshot of the Hexen II
mod scene, and it is the corpus the Mods menu is developed and regression
tested against (uhexen2-3m0h). Titles and descriptions below are transcribed
from the archive's own `readme.txt`; they are the authority for the display
names shipped in `mods_known[]` in `engine/hexen2/menu.c`.

Install convention the readme documents: drop the mod folder into the Hexen II
install directory and launch with `-game <dirname>`. That is exactly what the
Mods menu automates via `game <dirname>`, so the corpus needs no special
handling to be testable.

| dir | title | kind | notes |
|---|---|---|---|
| `acorn` | Magic Acorn | gameplay | new artifact, grows into a tree |
| `ahumado` | Ahumado's Skull | maps | 2-level custom mission |
| `apocbot` | Apocalypse Bots for Hexen 2 | gameplay | multiplayer bots |
| `bigspdrs` | Spiders! | gameplay | larger spiders |
| `black` | Hexen II: Black Plague | maps | 2-level custom mission |
| `bodies` | Bodies | gameplay | corpses persist, give xp when destroyed |
| `db` | DungeonBreak | gamemode | **HexenWorld** (`hwprogs.dat`), team DM variant |
| `eyeofra` | The Eye of Ra | gameplay | new artifact, rotating sunbeams |
| `ffmus` | Final Fantasy Music | assets | music replacement |
| `fo4d` | Fortress of Four Doors | maps | loose `maps/` only — 17 `.bsp`, no gamecode, no pak |
| `h2ctf` | HexDev Hexen II CTF | gamemode | CTF, Hexen II (not HexenWorld) |
| `hcbots` | CronosBot for Hexen 2 | gameplay | deathmatch bots |
| `hexarena` | HexArena | gamemode | **HexenWorld** (`hwprogs.dat`), 1v1 RocketArena-style |
| `hwctf` | HexenWorld CTF | gamemode | **HexenWorld** (`hwprogs.dat`) |
| `hwcycle` | Map cycling for HW | server | **HexenWorld** (`hwprogs.dat`) |
| `leech` | Weapon Leech | gameplay | toggle Tome of Power for xp |
| `lightng` | Lightning Sorcery | gameplay | Necromancer Bone Shards -> lightning |
| `mpbyrino` | Mission Pack by Rino | maps | 22-level custom mission |
| `mpbyrino2` | Mission Pack by Rino 2 | maps | 12-level custom mission |
| `ndm` | DM pak for Hexen II | maps | 5 deathmatch maps |
| `orbmeek` | The Orb of the Meek | gameplay | new artifact, shrinks targets |
| `ovinoimp` | Ovinomancer Imp | gameplay | new Imp Lord ranged attack |
| `peanut` | Hexen II: Project Peanut | gameplay | magic system, spells, magic shop |
| `q2sounds` | Quake 2 - Hexen 2 Player Sound Pack | assets | player sound replacement |
| `quake` | Quake sound for Hexen 2 & Quake weapons | assets | fusion of two mods |
| `qweapons` | Quake weapons | assets | Paladin weapon replacement |
| `raven` | Ravenhurst | maps | unfinished, 2 levels |
| `redmed` | Red Medusa | gameplay | reinstates the cut red medusa |
| `rk` | Rival Kingdoms | gamemode | **HexenWorld** (`hwprogs.dat`), Siege variant, 8 classes |
| `rvnlrd` | RavenLord | gameplay | Necromancer buffs |
| `siege` | Siege | gamemode | **HexenWorld** (`hwprogs.dat`), crown-steal teamplay |
| `speeed` | Speeeed's Imagination | maps | single custom level |
| `spiders` | Arachnophobia | gameplay | summonable spider followers |
| `spike` | Deadly Sheep | gameplay | Assassin crossbow fires sheep |
| `sprgnth` | Super Gauntlets | gameplay | Paladin gauntlets consume blue mana |
| `SuperNecro` | SuperNecro | gameplay | Necromancer weapons, 2x rate, no ammo |
| `thebarongastonehousebyrino` | The Baron Gastone House | maps | 3-level custom mission |
| `tt` | The Tyrant's Tome | maps | loose `maps/` only — 1 `.bsp`, no gamecode, no pak |
| `xability` | Extra Abilities | gameplay | new per-class starting abilities |

## What the corpus exercises

- **Opaque directory names.** `rvnlrd`, `sprgnth`, `xability`, `fo4d`, `tt`,
  `rk`, `db` say nothing to a player. This is why the menu carries a display
  name table rather than listing bare directory names.
- **Row overflow.** `thebarongastonehousebyrino` is 27 characters against a
  21-character label column, and it is not a contrived case — it is in the
  corpus. Labels are ellipsis-truncated.
- **Case sensitivity.** `SuperNecro` is the only mixed-case entry, so it is the
  test for case-insensitive sorting and filtering.
- **HexenWorld-only mods.** Six entries — `db`, `hexarena`, `hwctf`,
  `hwcycle`, `rk`, `siege` — ship a loose `hwprogs.dat` and no `progs.dat`.
  They cannot run under the Hexen II client. The menu detects them at scan
  time and draws them with an `[HW]` tag, unselectable.
- **Scale.** 39 entries plus `data1` and `portals` is well past what fits on a
  320x200 menu page, which is what motivates the search filter and scrolling.

## Verified against this engine

The archive was unpacked and every directory run through `FS_IsGamedir`'s
actual predicate on 2026-08-28. Seven of the thirty-nine failed it and were
therefore invisible to both the mods menu and `game` tab-completion:

| dir | why it was missed |
|---|---|
| `db`, `hexarena`, `hwctf`, `hwcycle`, `rk`, `siege` | loose `hwprogs.dat`, no `progs.dat`/pak/pk3 |
| `fo4d`, `tt` | loose `maps/*.bsp` only, no gamecode, no pak/pk3 |

`Host_Game_f` never consults `FS_IsGamedir` — it mounts any directory that
exists — so all seven were launchable from the console the whole time. The
defect was discovery, not capability. `FS_IsGamedir` now also accepts
`hwprogs.dat` and a `maps/` directory containing at least one `.bsp`, which
brings all 39 into the list.

Other findings from the survey:

- **No filename case variants.** Every `progs.dat`, `hwprogs.dat` and
  `pakN.pak` in the corpus is lowercase, so the case-sensitive probes on Linux
  are safe against this corpus. That is a property of this archive, not a
  guarantee about Hexen II mods generally.
- **No `.pk3` anywhere.** The pk3 branch of `FS_IsGamedir` (uhexen2-pzha) is
  not exercised by this corpus; it needs its own test asset.
- **Composition.** 20 ship a loose `progs.dat`, 13 ship a `pakN.pak`, 2 ship
  both (`ahumado`, `peanut`), 6 ship `hwprogs.dat`, and 2 ship neither
  gamecode nor an archive (`fo4d`, `tt`). That accounts for all 39.

## Still unverified

No entry has been launched. The survey confirms what the scanner sees on
disk; it does not confirm that each mod loads, that its gamecode is compatible
with this engine, or which of them want `portals` mounted alongside. That
needs a run per mod and is the remaining part of Stage 0.
