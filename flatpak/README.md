# Flatpak Build

## Requirements

- `flatpak` and `flatpak-builder` installed on your system
- Flathub remote configured

## Build and Install

```bash
# One-time setup
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.freedesktop.Platform//25.08 org.freedesktop.Sdk//25.08

# Build and install
flatpak-builder --user --install --force-clean build-dir flatpak/io.github.hexenwail.hexenwail.yml

# Run
flatpak run io.github.hexenwail.hexenwail
```

## Game Data

Place your Hexen II game data in
`~/.var/app/io.github.hexenwail.hexenwail/.hexen2/data1/`:
- `pak0.pak` (required)
- `pak1.pak` (required)

For Portal of Praevus, add
`~/.var/app/io.github.hexenwail.hexenwail/.hexen2/portals/pak3.pak`.

That is the host side of the sandbox's `--persist=.hexen2`: inside the
sandbox it is `~/.hexen2`, but the app cannot see your real `~/.hexen2`.
With no game data present, the launcher offers to download Raven's free
1997 demo into the same directory.

## Notes

- SDL3 and FluidSynth are built from source as Flatpak modules
- ALSA is disabled (PulseAudio/PipeWire via Flatpak socket)
- A General MIDI soundfont (TimGM6mb, GPL-2) is bundled at
  `/app/share/soundfonts/default.sf2` so MIDI music works out of the box.
  Override it with the `snd_soundfont` cvar or by placing `soundfont.sf2`
  in the game dir.
- Gamepad support via `--device=all` permission (Freedesktop 25.08 dropped `--device=input`)
