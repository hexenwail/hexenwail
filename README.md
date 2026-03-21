# Hexen II: Hexenwail

![Screenshot](docs/screenshot1.png)
*New worlds await!* (Storm over Thyrion)

A modernized Hexen II engine for Windows and Linux, forked from [uHexen2 / Hammer of Thyrion](http://uhexen2.sourceforge.net/).

Modernizes the engine in the spirit of [Ironwail](https://github.com/andrei-drexler/ironwail) (for Quake), providing a stable, clean platform for both the original game and modern mods that rely on extended engine features.

## Why this fork?

This fork merges [Hammer of Thyrion](http://uhexen2.sourceforge.net/) with [Shanjaq's mod-support additions](https://github.com/Shanjaq/uhexen2), then modernizes the engine in the spirit of what [Ironwail](https://github.com/andrei-drexler/ironwail) did for Quake — SDL3, gamepad support, GLSL shaders, and a clean codebase targeting Linux and Windows only. Upstream Hammer of Thyrion updates are pulled in as they become available.

## Features

### Play with a controller
- [x] Xbox/PlayStation/Nintendo gamepad support with analog sticks
- [x] Circular deadzone and power-curve easing for smooth aiming
- [x] Controller options menu (sensitivity, deadzone, acceleration)
- [x] Hot-plug — just plug in and play
- [x] Default bindings: triggers for attack/jump, shoulders for weapon cycle
- [ ] Rumble/haptic feedback on damage and weapon fire

### Keyboard input (Ironwail-style)
- [x] Modern WASD + mouselook defaults out of the box
- [x] Scancode-based key bindings — WASD works correctly on AZERTY, Dvorak, etc.
- [x] Proper text input via SDL — keyboard layout, shift, dead keys handled by OS
- [x] Console/menu uses layout-aware typing, game uses physical key positions

### Modern rendering (GL 4.3 shader pipeline)
- [x] Full GLSL 4.30 shader pipeline — zero immediate mode, zero fixed-function
- [x] Render scale for crunchy pixel upscaling (25%-100%)
- [x] Retro mode: Bayer dithering + 256-color palette quantization (Ironwail-style)
- [x] Display presets: Crunchy (25% scale, point-sampled), Retro, Modern, Custom
- [x] Classic/Smooth texture filtering toggle
- [x] MSAA anti-aliasing (via multisampled FBO)
- [x] Borderless windowed and fullscreen modes
- [x] VSync toggle
- [x] Anisotropic texture filtering
- [x] Brightness and contrast sliders (GLSL post-process shader)
- [x] Skybox rendering
- [x] Shader-based fog
- [x] Fence/chain texture transparency
- [x] Glow effects with fog attenuation
- [x] Water tint color options (Classic/Blue/Green/Clear)
- [x] FOV slider (60-130)
- [x] FPS limiter (24/30/60/72/90/120/144/Unlimited)
- [x] Show FPS toggle
- [x] HUD layout modes (Full/Mini/Off/Clean — Clean also hides weapon)
- [ ] Bloom
- [ ] Lightmapped liquid surfaces
- [ ] Underwater warp effect
- [ ] Lightstyle interpolation
- [ ] Physics/render decoupling with entity interpolation

### Plays nicely with mods
- [x] Protocol auto-detection (classic protocol 15 / extended protocol 19)
- [ ] Runs [Wheel of Karma](https://www.moddb.com/mods/wheel-of-karma-a-tulku-odyssey) and [Storm over Thyrion](https://www.moddb.com/mods/storm-over-thyrion) out of the box
- [x] [PimpModel](http://earthday.free.fr/Inkys-Hexen-II-Mapping-Corner/mapping-tricks-pimp.html) per-entity model overrides (glow, spin, float, color)
- [x] Extended QuakeC builtins for mod developers
- [x] 8192 max entities (up from 2048)
- [x] 2048 sound channels (up from 128) — no more sounds cutting out
- [x] Missing model precache is a warning, not a crash
- [x] Higher audio quality (44.1 kHz default)
- [x] Mods menu — browse and switch mods without restarting, with active mod indicator
- [x] Portal of Praevus and base game selectable from Mods menu
- [x] Portals data toggle — include mission pack assets when launching custom mods
- [x] Full engine reset on mod switch (filesystem, textures, lightmaps, command buffer)
- [x] Config saved per-mod before switching — no bind bleed between mods
- [x] `game <modname>` console command for runtime mod switching
- [x] Intro cinematic link in main menu (opens in browser)

### Music and sound
- [x] OGG, MP3, FLAC, WAV music playback (built-in decoders, zero dependencies)
- [x] MIDI music via FluidSynth on Linux, native Windows MIDI
- [x] Soundfont auto-detection (`snd_soundfont` cvar or `data1/soundfont.sf2`)
- [x] 2048 sound channels — no more effects cutting out in busy scenes

### Modern platform
- [x] SDL3 for video, input, and audio — Linux and Windows
- [x] CMake build system — pull the source and `cmake && make`
- [x] Nix flake for reproducible builds and Windows cross-compilation
- [x] NixOS: FluidSynth soundfont bundled — MIDI works out of the box
- [x] HiDPI display support
- [x] Options organized into Display/Sound/Game submenus
- [x] Display menus show live preview — no screen tint or fade overlay
- [x] Unified platform layer — shared code in `sys_sdl.c`, minimal OS-specific code
- [x] Cleaned out legacy platform code (DOS, OS/2, Amiga, macOS, HexenWorld)
- [x] Cleaned out legacy GL capability detection — assumes GL 4.3
- [x] Flatpak packaging support
- [ ] Mouse-driven UI navigation

## Building

See [BUILD.md](BUILD.md) for full instructions.

**Quick start (any Linux):**
```bash
cd engine && mkdir build && cd build
cmake .. && make -j$(nproc)
```

**Requirements:** OpenGL 4.3 (2012+), SDL3, libvorbis, libogg, ALSA (optional), FluidSynth (optional)

**Nix:**
```bash
nix build              # NixOS
nix build .#linux-fhs  # Portable Linux binary
nix build .#win64      # Windows 64-bit (cross-compiled)
nix build .#release    # All platforms
```

## Game data

You need the original Hexen II game data files (`data1/pak0.pak`, `data1/pak1.pak`). For Portal of Praevus, add `portals/pak3.pak` and select it from the Mods menu (or launch with `-portals`).

## License

GPL-2.0-or-later. See [COPYING](COPYING).

Bundled third-party libraries:
- [dr_libs](https://github.com/mackron/dr_libs) (public domain / MIT-0) — MP3, FLAC, WAV decoders
- [SDL3](https://www.libsdl.org/) (Zlib) — platform abstraction
- [libogg/libvorbis](https://xiph.org/) (BSD-3) — OGG audio

## Credits

Based on [uHexen2 / Hammer of Thyrion](http://uhexen2.sourceforge.net/) by O. Sezer and contributors, which is based on the Hexen II source release by [Raven Software](https://en.wikipedia.org/wiki/Raven_Software) and [id Software](https://en.wikipedia.org/wiki/Id_Software).

*The name? **Hexen** + Iron**wail** — a modernized Hexen II engine in the spirit of Ironwail for Quake.*

Incorporates code and techniques from the Quake engine modernization community:
- [Ironwail](https://github.com/andrei-drexler/ironwail) — GL 4.3 shader pipeline approach, software rendering emulation (palette dithering), render scale, gamepad input, scancode-based keyboard input, sound channel management
- [QuakeSpasm](https://sourceforge.net/projects/quakespasm/) — texture manager, fog system, console infrastructure
- [QuakeSpasm-Spiked](https://github.com/AAS/quakespasm-spiked) — protocol extensions, mod compatibility patterns
