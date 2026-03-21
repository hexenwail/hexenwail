# Building uHexen2

## Quick Start (any Linux)

```bash
cd engine
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Binary is at `engine/build/bin/glhexen2`.

### Dependencies

**Debian/Ubuntu:**
```bash
sudo apt install build-essential cmake pkg-config \
  libsdl3-dev libvorbis-dev libogg-dev libasound2-dev \
  libgl-dev libfluidsynth-dev
```

**Fedora:**
```bash
sudo dnf install gcc cmake pkg-config \
  SDL3-devel libvorbis-devel libogg-devel alsa-lib-devel \
  mesa-libGL-devel fluidsynth-devel
```

**Arch:**
```bash
sudo pacman -S gcc cmake pkgconf \
  sdl3 libvorbis libogg alsa-lib mesa fluidsynth
```

### Optional dependencies

- **FluidSynth** — MIDI music playback via SoundFonts. If not found, MIDI is disabled.
- **ALSA** — Linux audio. Disable with `-DUSE_ALSA=OFF`.

MP3, FLAC, and WAV decoding are built-in (dr_libs, no external dependency).

## Nix Builds

```bash
# NixOS (rpaths point to nix store)
nix build .#nixos

# Portable Linux (FHS, no nix store deps)
nix build .#linux-fhs

# Windows 64-bit (cross-compiled via MinGW)
nix build .#win64

# All platforms
nix build .#release
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `USE_SOUND` | ON | Enable sound support |
| `USE_ALSA` | ON | Enable ALSA audio (Linux) |
| `USE_CODEC_VORBIS` | ON | OGG Vorbis (requires libvorbis) |
| `USE_CODEC_MP3` | ON | MP3 (built-in dr_mp3) |
| `USE_CODEC_FLAC` | ON | FLAC (built-in dr_flac) |
| `USE_DEBUG` | OFF | Debug build |

## Game Data

The engine requires original Hexen II game data files. Place `data1/pak0.pak` and `data1/pak1.pak` in the game directory. For the Portal of Praevus expansion, also add `portals/pak3.pak`.

Run from the game data directory:
```bash
./glhexen2
./glhexen2 -portals          # Portal of Praevus
./glhexen2 -game mymod       # Load a mod
```
