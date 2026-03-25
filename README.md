# Hexen II: Hexenwail

![Screenshot](docs/screenshot1.png)
*New worlds await!* ([Wheel of Karma](https://www.moddb.com/mods/wheel-of-karma-a-tulku-odyssey))

The Quake community took sezero's [QuakeSpasm](https://github.com/sezero/quakespasm) and pushed its OpenGL rendering into the modern era with [Ironwail](https://github.com/andrei-drexler/ironwail). Hexenwail does the same for Hexen II.

Built on sezero's [Hammer of Thyrion](https://github.com/sezero/uhexen2) — the definitive Hexen II engine — with contributions from Shanjaq, Inky, and many more.

A modern GL 4.3 fork for Windows and Linux: SDL3, GLSL shaders, gamepad support, and a clean codebase.

## Features

### Play with a controller
- [x] Xbox/PlayStation/Nintendo gamepad support with analog sticks
- [x] Circular deadzone and power-curve easing for smooth aiming
- [x] Controller options menu (sensitivity, deadzone, acceleration)
- [x] Hot-plug — just plug in and play
- [x] Default bindings: triggers for attack/jump, shoulders for weapon cycle
- [x] Rumble/haptic feedback on damage and weapon fire

### Comfort & accessibility
- [x] View bob intensity slider (reduce or disable head bob)
- [x] View roll intensity slider (strafe roll)
- [x] Crouch smoothing (no camera snap on duck/unduck)
- [x] Console transparency (Opaque/Light/Clear)
- [x] Dynamic lighting toggle

### Input (Ironwail-style)
- [x] Modern WASD + mouselook defaults out of the box
- [x] Scancode-based key bindings — WASD works correctly on AZERTY, Dvorak, etc.
- [x] Proper text input via SDL — keyboard layout, shift, dead keys handled by OS
- [x] Console/menu uses layout-aware typing, game uses physical key positions
- [x] Mouse-driven menus — hover highlight, click, and scroll wheel navigation
- [x] Weapon keybinds menu with type-to-search filtering
- [x] Always-run toggle (`cl_alwaysrun`)
- [x] Configurable pitch clamp range (`cl_maxpitch`/`cl_minpitch`)

### Modern rendering (GL 4.3 shader pipeline)
- [x] Full GLSL 4.30 shader pipeline — zero immediate mode, zero fixed-function
- [x] Lightmap atlas — all lightmap pages packed into one 2048x2048 texture, zero per-surface rebinds
- [x] Batched world rendering — surfaces accumulated as triangles, one draw call per texture
- [x] Render scale for crunchy pixel upscaling (25%-100%)
- [x] Retro mode: Bayer dithering + 256-color palette quantization (Ironwail-style)
- [x] Display presets: Faithful, Crunchy, Retro, Clean, Modern, Ultra (User when custom)
- [x] Classic/Smooth texture filtering toggle
- [x] MSAA anti-aliasing (via multisampled FBO)
- [x] Borderless windowed and fullscreen modes
- [x] VSync toggle (default off)
- [x] Anisotropic texture filtering
- [x] Brightness and contrast sliders (GLSL post-process shader)
- [x] Two-layer scrolling sky (solid + transparent) and skybox rendering (`r_skybox_speed`)
- [x] Shader-based fog (standard exp formula, smooth underwater fade)
- [x] Fence/chain texture transparency
- [x] Water tint color options (Classic/Blue/Green/Clear)
- [x] FOV slider (60-130)
- [x] Zoom (`+zoom`/`togglezoom` — smooth FOV transition to `zoom_fov`, bindable)
- [x] Auto draw distance by network protocol, adjustable (`r_farclip`)
- [x] FPS limiter (24/30/60/72/90/120/144/Unlimited)
- [x] Show FPS toggle
- [x] Speed display (`scr_showspeed` — horizontal speed readout)
- [x] In-game clock/timer display (`showclock 1`=game time, `2`=wall clock)
- [x] Centered console notify messages (`con_notifycenter`)
- [x] HUD layout modes (Full/Mini/Off/Clean — Clean also hides weapon)
- [x] Model animation interpolation (lerping between keyframes, `r_lerpmodels`)
- [x] Physics/render decoupling with entity interpolation (Ironwail-style)
- [x] Lightstyle interpolation (smooth flickering lights, `r_lerplightstyles`)
- [x] Fullbright pixel support for alias model skins (eye glow, torch flames)
- [x] Square particle toggle (`gl_particles 0` for classic software look)
- [x] Lightmapped liquid surfaces
- [x] Underwater screen warp
- [x] Motion blur post-process
- [x] FXAA post-process
- [x] Glow effects toggle (All/Torch Only/Off) with fog attenuation
- [x] Muzzle flash dynamic light radius scaling (`gl_flashintensity`)
- [x] Blob shadows — soft circle projected on ground, fades with height
- [x] Back-to-front transparent entity sorting (`r_alphasort`)
- [x] Entity bounding box debug visualization (`r_showbboxes`)
- [x] Polyblend intensity scale (`gl_cshiftpercent`, 0-100%)
- [x] Translucent brush entity rendering (teleport beams, etc.)
- [x] Per-entity alpha transparency (ENTALPHA protocol extension)
- [x] Model overbright toggle (`gl_overbright_models`)
- [ ] HDR rendering pipeline

### Plays nicely with mods
- [x] Protocol auto-detection (classic 15 / extended 19 / mod 21)
- [x] Case-insensitive file lookups — Windows-built mods work on Linux
- [x] Runs [Wheel of Karma](https://www.moddb.com/mods/wheel-of-karma-a-tulku-odyssey) and [Storm over Thyrion](https://www.moddb.com/mods/storm-over-thyrion) out of the box
- [x] Bottom plaque centerprint — messages prefixed with `_` display as subtitles
- [x] [PimpModel](http://earthday.free.fr/Inkys-Hexen-II-Mapping-Corner/mapping-tricks-pimp.html) per-entity model overrides (glow, spin, float, color)
- [x] Extended QuakeC builtins for mod developers (`SOLID_GHOST`, entity alpha)
- [x] 8192 max entities (up from 2048)
- [x] 2048 sound channels (up from 128) — no more sounds cutting out
- [x] TrueLightning — smooth Crusader lightning beam tracking (`cl_truelightning`)
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
- [x] OGG Vorbis, Opus, MP3, FLAC, WAV music playback (CD track fallback to `music/trackNN.ogg`)
- [x] Tracker music via libxmp (MOD, S3M, XM, IT) and Unreal UMX containers
- [x] MIDI music via FluidSynth on Linux, native Windows MIDI
- [x] Soundfont auto-detection (`snd_soundfont` cvar or `data1/soundfont.sf2`)
- [x] 2048 sound channels — no more effects cutting out in busy scenes

### Modern platform
- [x] SDL3 for video, input, and audio — Linux and Windows
- [x] CMake build system — pull the source and `cmake && make`
- [x] Nix flake for reproducible builds and Windows cross-compilation
- [x] CI pipeline — GitHub Actions builds Linux + Windows on every push, smoke tests, shellcheck lint, Nix store caching
- [x] One-command releases — `./release.sh v0.x.x` tags, pushes, and GitHub Actions builds + publishes release zips automatically
- [x] Reproducible builds — Nix flake lockfile pins exact dependency versions; `nixos-unstable` channel for latest SDL3
- [x] NixOS: FluidSynth soundfont bundled — MIDI works out of the box
- [x] HiDPI display support
- [x] Combined Display menu — rendering and video settings in one place (Ironwail-style)
- [x] Menu fade toggle — amber overlay on menu open; auto-suppressed in Display/Rendering submenus for clean settings preview
- [x] Unified platform layer — shared code in `sys_sdl.c`, minimal OS-specific code
- [x] Cleaned out legacy platform code (DOS, OS/2, Amiga, macOS, HexenWorld)
- [x] Cleaned out legacy GL capability detection — assumes GL 4.3
- [x] Flatpak packaging support (bundled in releases)
- [x] Game Options menu — view bob/roll, console alpha, dynamic light toggle
- [x] Console log always written to disk (`qconsole.log`)
- [x] GL_KHR_debug callback for GPU driver diagnostics
- [x] Raw mouse input toggle (`m_rawinput`)
- [ ] AppImage packaging

## Building

See [BUILD.md](BUILD.md) for full instructions.

**Quick start (any Linux):**
```bash
cd engine && mkdir build && cd build
cmake .. && make -j$(nproc)
```

**Requirements:** OpenGL 4.3 (2012+), SDL3, libvorbis, libogg, libopus, opusfile, libxmp, ALSA (optional), FluidSynth (optional)

**Nix:**
```bash
nix build              # NixOS
nix build .#linux-fhs  # Portable Linux binary
nix build .#win64      # Windows 64-bit (cross-compiled)
nix build .#release    # All platforms
```

## Game data

You need the original Hexen II game data files (`data1/pak0.pak`, `data1/pak1.pak`). For Portal of Praevus, add `portals/pak3.pak` and select it from the Mods menu (or launch with `-portals`).

**Flatpak:** Drop your game data into `~/.var/app/com.github.bobberb.hexenwail/.hexen2/`

To launch a mod with portals data included: `glhexen2 -mod <modname>`

**Steam Deck:** Add Hexenwail to Steam, then right-click it → Properties → Controller → set the override to **Gamepad** (or "Gamepad with Joystick Trackpad"). The default Desktop layout emulates keyboard input instead of passing the controller through to SDL.

## License

GPL-2.0-or-later. See [COPYING](COPYING).

Bundled third-party libraries:
- [dr_libs](https://github.com/mackron/dr_libs) (public domain / MIT-0) — MP3, FLAC, WAV decoders
- [SDL3](https://www.libsdl.org/) (Zlib) — platform abstraction
- [libogg/libvorbis](https://xiph.org/) (BSD-3) — OGG Vorbis audio
- [libopus/opusfile](https://opus-codec.org/) (BSD-3) — Opus audio
- [libxmp](https://github.com/libxmp/libxmp) (MIT) — tracker music (MOD, S3M, XM, IT)
- [FluidSynth](https://www.fluidsynth.org/) (LGPL-2.1) — MIDI synthesis

## Credits

Based on [uHexen2 / Hammer of Thyrion](http://uhexen2.sourceforge.net/) by O. Sezer and contributors, which is based on the Hexen II source release by [Raven Software](https://en.wikipedia.org/wiki/Raven_Software) and [id Software](https://en.wikipedia.org/wiki/Id_Software).

*The name? **Hexen** + Iron**wail** — a modernized Hexen II engine in the spirit of Ironwail for Quake.*

Incorporates code and techniques from the Quake engine modernization community:
- [Ironwail](https://github.com/andrei-drexler/ironwail) — GL 4.3 shader pipeline approach, software rendering emulation (palette dithering), render scale, gamepad input, scancode-based keyboard input, sound channel management
- [QuakeSpasm](https://sourceforge.net/projects/quakespasm/) — texture manager, fog system, console infrastructure
- [QuakeSpasm-Spiked](https://github.com/AAS/quakespasm-spiked) — protocol extensions, mod compatibility patterns
