# Usage

## External textures

Hexenwail supports external texture overrides — drop hi-res files into the game
directory to replace internal assets:

| Asset type | Override path | Cvar |
|------------|--------------|------|
| BSP textures | `textures/<name>.<ext>` | always on |
| BSP textures, one map only | `textures/<mapname>/<name>.<ext>` | always on |
| Model skins | `models/<model>_<skin>.<ext>` | always on |
| Particles | `particles/<name>.<ext>` | always on |
| HUD/menu graphics | `gfx/<name>.<ext>` | `r_texture_external_hud 1` |

`<ext>` is tried in the order `dds`, `ktx`, `png`, `tga`, `pcx`.

### Search order

For a BSP texture the per-map directory is searched first, then the shared
pool. `textures/demo1/wall01.png` therefore beats `textures/wall01.dds` when
you are playing `demo1`, and only that map is affected — two maps can disagree
about what `wall01` looks like without renaming anything in the BSP.

Within a single directory the compressed containers win, because they cost the
GPU a quarter of the memory and skip the CPU decode entirely.

### Texture names

Liquid textures are named `*lowlight` in the BSP, and `*` is not a legal
filename character on Windows, so write them with `#`: `textures/#lowlight.png`.

Fence textures (names beginning with `{`) are always treated as cutouts, so a
replacement works even if the file itself carries no alpha channel.

### Glow maps

A replaced texture loses the palette-index fullbright information the engine
normally derives its glow layer from, so ship the glow as a companion file:

```
textures/torch_side.png        albedo
textures/torch_side_glow.png   the bits that stay lit
```

`_luma` is accepted as a synonym for `_glow`, so packs built for FTE or
jsHexen2 work unmodified. Model skins use the same convention
(`models/mummy_0_glow.png`). Without a companion file a replaced texture simply
has no glow layer — `gl_fullbrights 0` disables the whole feature.

### Compressed textures (DDS/KTX)

A 2048×2048 RGBA texture is about 21 MB of VRAM once mipped; the same art as
BC1 is under 3 MB, and BC3 under 6 MB. For a pack spanning a whole episode that
is the difference between running and not.

Supported: DDS with `DXT1`/`DXT3`/`DXT5`, `ATI1`/`BC4U`, `ATI2`/`BC5U`, or a
DX10 header carrying BC1–BC5 or BC7; and KTX 1.1 holding any of the same
formats. Cube maps, volume textures, arrays and uncompressed containers are
rejected — the engine falls through to the PNG/TGA/PCX beside them.

Mip levels in the file are used as-is, so include a full chain. `gl_picmip`
and the GPU's maximum texture size are honoured by starting further down that
chain, which is why a container that ships a single oversized level is skipped
rather than uploaded: block data cannot be rescaled the way an RGBA image can.

Which families are available depends on the driver; the console prints what it
found at startup (`Compressed textures: S3TC yes, RGTC yes, BPTC yes`). S3TC is
an extension rather than core GL, and the ES/WebGL2 build has neither RGTC nor
BPTC.

2D assets — HUD, menu graphics and the particle sprite — deliberately do not
take a compressed source. They are uploaded unmipped at their native size,
which is exactly the case block compression damages most.

## AI upscale tool

Use `tools/upscale-pak.sh` to extract and upscale all assets from a PAK file (requires nix):

```bash
# Base game
./tools/upscale-pak.sh ~/hexen2/data1/pak0.pak ~/hexen2/data1

# Full game (palette from pak0)
./tools/upscale-pak.sh ~/hexen2/data1/pak1.pak ~/hexen2/data1 --palette ~/hexen2/data1/pak0.pak

# Portal of Praevus
./tools/upscale-pak.sh ~/hexen2/portals/pak3.pak ~/hexen2/portals --palette ~/hexen2/data1/pak0.pak
```

Options: `--scale 2|3|4` (default 4), `--upscaler realcugan|realesrgan` (default realcugan), `--denoise -1|0|1|2|3`.

## Game data paths

**Flatpak:** Drop your game data into `~/.var/app/io.github.hexenwail.hexenwail/.hexen2/`

To launch a mod with portals data included: `glhexen2 -mod <modname>`

## Bundled gamecode

Releases ship compiled gamecode (`progs.dat`, `progs2.dat`) beside the engine —
in `gamecode/` next to `glh2.exe` on Windows, in `share/hexenwail/` next to
`bin/` on Linux — so the gamecode fixes apply without copying anything into your
game directory.

It is used **only** when the progs the engine is about to load is the base
game's own *and* you are actually playing the base game. A mod that ships its
own `progs.dat` always wins, and so does a mod that ships none: in that case the
game keeps the retail gamecode it would have used anyway. Mods are never
affected.

**Options → Game → Gamecode from** picks the source, and takes effect on the
next new game (never mid-campaign). It names a *place*, not an author, because
the two are not the same question — see the status row below.

| Setting | Loads |
| --- | --- |
| `Bundled` | the copy shipped beside the engine. The default. |
| `Loose` | your install's own gamedir, where a loose `progs.dat` is currently sitting on top of the pak copy. |
| `Pak` | your install's own gamedir. Shown instead of `Loose` when nothing is on top of the pak copy — and offered as a *third* setting when something is, so you can reach the retail file underneath without moving anything. |

`Pak` only appears when a loose file is actually shadowing a pak copy of the
base game's gamecode; anywhere else it would load the identical file `Loose`
does. The cvar behind the row is `sv_gamecode` (0 = your install, 1 = bundled,
2 = pak only).

Pass `-vanillaprogs` to ignore the bundled gamecode entirely and load whatever
your own `data1/` or `portals/` contains — worth quoting in a bug report, since
it says precisely which gamecode you reproduced against. It outranks all three
settings above. Deleting the bundled directory has the same effect.

Which gamecode actually loaded is printed to the console at map load, ending in
whose code it is (`Raven 1.11`, `hexenwail-<date>`, `hexenwail (undated)` or
`Third-party`) — the same string the **Gamecode loaded** row in that menu shows:

```
Gamecode: progs.dat from /path/to/progs.dat (H2/v1.11, file crc 12345) -- Raven 1.11
```

If you asked for your install's own file and got Hexenwail gamecode anyway, the
engine says so and names the file to remove:

```
WARNING: gamecode source is set to the game's own file, but
  /path/to/data1/progs.dat
is Hexenwail gamecode, not your install's original.
Remove or rename it to get Raven's.
```

That happens on installs where an older release's gamecode was hand-copied into
`data1/`. A loose file always outranks the pak copy of the same name, so the
retail `PROGS.DAT` inside `pak0.pak` is unreachable while it is there — either
remove it, or use the `Pak` setting.

## Server tick rate

The server and physics run at 72 Hz, set by `sv_physfps` (default `72`, clamped
to `10`–`250`). The stock engine's listen server ran at 20 Hz, which capped every
QC think chain scheduled shorter than a tick, coarsened monster attack cadence,
and broke strafe-jump acceleration.

`sys_ticrate` is a *different*, dedicated-server-only cvar and does not affect a
listen server or single player.

Almost all mods are unaffected, but one HexenC idiom — `velocity = delta * 20`,
where the `20` is really a hardcoded `1 / frametime` — silently changes from an
exact snap into a lagging follow. If a mod's held or per-frame-positioned entity
trails the player and is *exact* at `sv_physfps 20`, that is the cause. See
[docs/MODDING_TICKRATE.md](docs/MODDING_TICKRATE.md) for the mechanism, what to
write instead, and how to tell it apart from client-side interpolation.

## Steam Deck

Add Hexenwail to Steam, then right-click it → Properties → Controller → set the override to **Gamepad** (or "Gamepad with Joystick Trackpad"). The default Desktop layout emulates keyboard input instead of passing the controller through to SDL.
