# Nix Flake for uHexen2

This repository now includes a Nix flake for building Hammer of Thyrion (uHexen2).

## Quick Start

### Building

Build the default OpenGL version:
```bash
nix build
```

Build specific packages:
```bash
nix build .#default       # OpenGL version (glhexen2)
nix build .#hexen2-sw     # Software renderer
nix build .#h2ded         # Dedicated server
nix build .#uhexen2-full  # All binaries in one package
```

### Running

Run directly without building:
```bash
nix run
```

Or after building:
```bash
./result/bin/glhexen2
```

### Development

Enter a development shell with all dependencies:
```bash
nix develop
cd engine/hexen2
make glh2
```

## Available Packages

- **default** (glhexen2): OpenGL renderer version - recommended for modern systems
- **hexen2-sw**: Software renderer version - for systems without OpenGL
- **h2ded**: Dedicated server for multiplayer
- **uhexen2-full**: Bundle containing all three binaries

## Features

The flake builds uHexen2 with:
- SDL 1.2 support (via SDL_compat)
- OpenGL rendering
- ALSA audio support
- MP3 codec support (via libmad)
- Ogg Vorbis support
- Timidity MIDI synthesis

## Game Data

**Important:** This flake only builds the game engine. You need the original Hexen II game data files to play:

1. Copy `pak0.pak` and `pak1.pak` from your Hexen II CD to `~/.hexen2/data1/`
2. Run the h2patch utility to update the data files to version 1.11
3. For the Portal of Praevus expansion, copy `pak3.pak` to `~/.hexen2/portals/`

See the main README in `docs/README` for detailed installation instructions.

## Dependencies

The flake automatically handles all build dependencies:
- SDL (1.2 via SDL_compat)
- OpenGL libraries
- ALSA libraries
- libmad (MP3 decoder)
- libvorbis and libogg (Vorbis decoder)
- Standard build tools (gcc, make, pkg-config)

## License

The uHexen2 source code is licensed under GPLv2+. See the project homepage for details.

## More Information

- Homepage: https://uhexen2.sourceforge.net/
- Project page: https://sourceforge.net/projects/uhexen2/
- Main README: `docs/README`
