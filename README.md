# Hexen II: Hexenwail

![Screenshot](docs/screenshot1.png)
*New worlds await!* ([Wheel of Karma](https://www.moddb.com/mods/wheel-of-karma-a-tulku-odyssey))

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

### Comfort & accessibility
- [x] View bob intensity slider (reduce or disable head bob)
- [x] View roll intensity slider (strafe roll)
- [x] Console transparency (Opaque/Light/Clear)
- [x] Dynamic lighting toggle

### Keyboard input (Ironwail-style)
- [x] Modern WASD + mouselook defaults out of the box
- [x] Scancode-based key bindings — WASD works correctly on AZERTY, Dvorak, etc.
- [x] Proper text input via SDL — keyboard layout, shift, dead keys handled by OS
- [x] Console/menu uses layout-aware typing, game uses physical key positions

### Modern rendering (GL 4.3 shader pipeline)
- [x] Full GLSL 4.30 shader pipeline — zero immediate mode, zero fixed-function
- [x] Lightmap atlas — all lightmap pages packed into one 2048x2048 texture, zero per-surface rebinds
- [x] Batched world rendering — surfaces accumulated as triangles, one draw call per texture
- [x] Render scale for crunchy pixel upscaling (25%-100%)
- [x] Retro mode: Bayer dithering + 256-color palette quantization (Ironwail-style)
- [x] Display presets: Crunchy (25% scale, point-sampled), Retro, Modern, User
- [x] Classic/Smooth texture filtering toggle
- [x] MSAA anti-aliasing (via multisampled FBO)
- [x] Borderless windowed and fullscreen modes
- [x] VSync toggle (default off)
- [x] Anisotropic texture filtering
- [x] Brightness and contrast sliders (GLSL post-process shader)
- [x] Two-layer scrolling sky (solid + transparent) and skybox rendering
- [x] Shader-based fog (standard exp formula, smooth underwater fade)
- [x] Fence/chain texture transparency
- [x] Glow effects with fog attenuation
- [x] Water tint color options (Classic/Blue/Green/Clear)
- [x] FOV slider (60-130)
- [x] Adjustable draw distance (`r_farclip`)
- [x] FPS limiter (24/30/60/72/90/120/144/Unlimited)
- [x] Show FPS toggle
- [x] In-game clock/timer display (`showclock 1`=game time, `2`=wall clock)
- [x] HUD layout modes (Full/Mini/Off/Clean — Clean also hides weapon)
- [x] Model animation interpolation (lerping between keyframes)
- [x] Physics/render decoupling with entity interpolation (Ironwail-style)
- [x] Lightstyle interpolation (smooth flickering lights)
- [x] Fullbright pixel support for alias model skins (eye glow, torch flames)
- [x] Square particle toggle (`gl_particles 0` for classic software look)
- [ ] Lightmapped liquid surfaces
- [ ] Underwater warp effect
- [ ] FXAA post-process
- [ ] HDR rendering pipeline

### Plays nicely with mods
- [x] Protocol auto-detection (classic 15 / extended 19 / mod 21)
- [x] Case-insensitive file lookups — Windows-built mods work on Linux
- [x] Runs [Wheel of Karma](https://www.moddb.com/mods/wheel-of-karma-a-tulku-odyssey) and [Storm over Thyrion](https://www.moddb.com/mods/storm-over-thyrion) out of the box
- [x] Bottom plaque centerprint — messages prefixed with `_` display as subtitles
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
- [x] `-mod <name>` launch flag — sets game directory with portals in search path
- [x] Intro cinematic link in main menu (opens in browser)

### Music and sound
- [x] OGG, MP3, FLAC, WAV music playback (CD track fallback to `music/trackNN.ogg`)
- [x] MIDI music via FluidSynth on Linux, native Windows MIDI
- [x] Soundfont auto-detection (`snd_soundfont` cvar or `data1/soundfont.sf2`)
- [x] 2048 sound channels — no more effects cutting out in busy scenes

### Modern platform
- [x] SDL3 for video, input, and audio — Linux and Windows
- [x] CMake build system — pull the source and `cmake && make`
- [x] Nix flake for reproducible builds and Windows cross-compilation
- [x] CI pipeline — GitHub Actions builds Linux + Windows on every push, with Nix store caching for fast rebuilds
- [x] One-command releases — `./release.sh v0.x.x` tags, pushes, and GitHub Actions builds + publishes release zips automatically
- [x] Reproducible builds — Nix flake lockfile pins exact dependency versions; `nixos-unstable` channel for latest SDL3
- [x] NixOS: FluidSynth soundfont bundled — MIDI works out of the box
- [x] HiDPI display support
- [x] Combined Display menu — rendering and video settings in one place (Ironwail-style)
- [x] Display menu shows live preview — no screen tint or fade overlay
- [x] Unified platform layer — shared code in `sys_sdl.c`, minimal OS-specific code
- [x] Cleaned out legacy platform code (DOS, OS/2, Amiga, macOS, HexenWorld)
- [x] Cleaned out legacy GL capability detection — assumes GL 4.3
- [x] Flatpak packaging support
- [x] Game Options menu — view bob/roll, console alpha, dynamic light toggle
- [ ] Mouse-driven UI navigation
- [ ] AppImage packaging

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

To launch a mod with portals data included: `glhexen2 -mod <modname>`

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
