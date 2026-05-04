# Flatpak Build

## Requirements

- `flatpak` and `flatpak-builder` installed on your system
- Flathub remote configured

## Build and Install

```bash
# One-time setup
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08

# Build and install
flatpak-builder --user --install --force-clean build-dir flatpak/com.github.bobberb.hexenwail.yml

# Run
flatpak run com.github.bobberb.hexenwail
```

## Game Data

Place your Hexen II game data in `~/.hexen2/data1/`:
- `pak0.pak` (required)
- `pak1.pak` (required)

For Portal of Praevus, add `~/.hexen2/portals/pak3.pak`.

## Notes

- SDL3 and FluidSynth are built from source as Flatpak modules
- ALSA is disabled (PulseAudio/PipeWire via Flatpak socket)
- Soundfont not bundled — place `soundfont.sf2` in game dir or set `snd_soundfont` cvar
- Gamepad support via `--device=all` permission (Freedesktop 25.08 dropped `--device=input`)
