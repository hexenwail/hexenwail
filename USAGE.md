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

**Flatpak:** Drop your game data into `~/.var/app/com.github.bobberb.hexenwail/.hexen2/`

To launch a mod with portals data included: `glhexen2 -mod <modname>`

## Steam Deck

Add Hexenwail to Steam, then right-click it → Properties → Controller → set the override to **Gamepad** (or "Gamepad with Joystick Trackpad"). The default Desktop layout emulates keyboard input instead of passing the controller through to SDL.
