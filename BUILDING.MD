# uHexen2 Release Build Guide

This document explains how to build uHexen2 for multiple platforms at once.

## Quick Start

### Using Nix (Recommended)

Build all platforms (Linux, Windows 32-bit, Windows 64-bit) at once:

```bash
# Using the convenience script
./build-release.sh nix

# Or using make
make nix-release

# Or directly with nix
nix build .#release
```

The output will be in `result/release/` with subdirectories for each platform:
- `linux-x86_64/` - Linux 64-bit build
- `windows-i686/` - Windows 32-bit build
- `windows-x86_64/` - Windows 64-bit build

### Using CMake

Build all platforms using CMake (requires mingw-w64 cross-compiler):

```bash
# Using the convenience script
./build-release.sh cmake

# Or using make
make release
```

The output will be in `release/` directory.

## Individual Platform Builds

### Nix

```bash
# Linux only
nix build .#default
# or: make nix-linux

# Windows 32-bit only
nix build .#win32
# or: make nix-win32

# Windows 64-bit only
nix build .#win64
# or: make nix-win64
```

### CMake

```bash
# Linux
mkdir -p engine/build && cd engine/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Windows 32-bit (requires mingw32)
mkdir -p engine/build-win32 && cd engine/build-win32
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw32.cmake
make -j$(nproc)

# Windows 64-bit (requires mingw64)
mkdir -p engine/build-win64 && cd engine/build-win64
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw64.cmake
make -j$(nproc)
```

## Prerequisites

### For Nix Builds

- Nix package manager with flakes enabled
- That's it! Nix handles all dependencies automatically

### For CMake Builds

**Linux:**
- CMake 3.16+
- GCC
- SDL development libraries
- OpenGL development libraries
- Audio codec libraries (libmad, libvorbis, libflac, etc.)

**Windows Cross-Compilation:**
- mingw-w64 toolchain (both 32-bit and 64-bit)
  - On Debian/Ubuntu: `apt install mingw-w64`
  - On Fedora: `dnf install mingw64-gcc mingw32-gcc`
  - On Arch: `pacman -S mingw-w64-gcc`

## Release Package Contents

Each platform subdirectory contains:

- `bin/` - Executables and required DLLs (Windows only)
- `share/` - Game data files (if applicable)

For Windows builds, all required DLLs are automatically included.

## Build Options

The following CMake options are enabled by default in release builds:

- `USE_CODEC_MP3=ON` - MP3 audio support
- `USE_CODEC_VORBIS=ON` - Ogg Vorbis audio support
- `USE_CODEC_FLAC=ON` - FLAC audio support
- `USE_ALSA=ON` - ALSA audio support (Linux only)

## Troubleshooting

### "i686-w64-mingw32-gcc not found"

Install the mingw-w64 32-bit toolchain. The cmake build script will skip Windows builds if the toolchain is not found.

### "x86_64-w64-mingw32-gcc not found"

Install the mingw-w64 64-bit toolchain. The cmake build script will skip Windows builds if the toolchain is not found.

### Nix builds fail

Make sure you have Nix flakes enabled:

```bash
# Add to ~/.config/nix/nix.conf
experimental-features = nix-command flakes
```

## Distribution

After building, you can distribute the contents of each platform directory:

1. Zip/tar each platform directory separately
2. Include game documentation and license files
3. Add instructions for obtaining game data (pak files)

Example:

```bash
cd result/release  # or release/ for cmake builds

# Create distribution archives
tar czf uhexen2-linux-x86_64.tar.gz linux-x86_64/
zip -r uhexen2-windows-i686.zip windows-i686/
zip -r uhexen2-windows-x86_64.zip windows-x86_64/
```

## See Also

- `make help` - List all available build targets
- `./build-release.sh --help` - Build script options
- `CLAUDE.md` - Project development guidelines
