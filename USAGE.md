# Usage

## External textures

Hexenwail supports external texture overrides — drop hi-res TGA/PNG/PCX files into the game directory to replace internal assets:

| Asset type | Override path | Cvar |
|------------|--------------|------|
| BSP textures | `textures/<name>.tga` | `r_texture_external 1` |
| Model skins | `models/<model>_<skin>.tga` | `r_texture_external 1` |
| HUD/menu graphics | `gfx/<name>.tga` | `r_texture_external_hud 1` |

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

## Steam Deck

Add Hexenwail to Steam, then right-click it → Properties → Controller → set the override to **Gamepad** (or "Gamepad with Joystick Trackpad"). The default Desktop layout emulates keyboard input instead of passing the controller through to SDL.
