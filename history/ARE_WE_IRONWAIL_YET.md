# Are We Ironwail Yet?

Feature parity tracker: **Hexenwail** vs **Ironwail**

Last updated: 2026-04-08

Legend: ✅ Ported | 🔶 Partial | ❌ Missing | ➖ N/A (Quake-specific)

---

## Scorecard

| Category | ✅ | 🔶 | ❌ | ➖ |
|---|---|---|---|---|
| Rendering — GPU Pipeline | 6 | 0 | 6 | 0 |
| Rendering — Visual/Shading | 17 | 0 | 5 | 0 |
| Performance / Engine | 3 | 1 | 3 | 1 |
| UX / Menus / HUD | 8 | 1 | 13 | 0 |
| Input / Controller | 3 | 1 | 5 | 1 |
| Audio | 2 | 0 | 2 | 0 |
| Network / Protocol | 1 | 0 | 0 | 2 |
| Steam / Platform | 0 | 0 | 0 | 2 |
| **TOTAL** | **40** | **3** | **34** | **6** |

**Parity: 52% ported, 4% partial, 44% missing** (excluding N/A)

---

## Rendering — GPU Pipeline

| Feature | Status | Notes |
|---|---|---|
| GPU frustum culling (compute shader) | ✅ | `gl_worldcull.c` |
| Indirect multi-draw for world surfaces | ✅ | `glMultiDrawElementsIndirect` per texture bucket |
| SSBO alias model instanced batching | ✅ | `gl_rmain.c` |
| SSBO GPU particles | ✅ | `r_part.c` |
| Order-Independent Transparency (OIT) | ✅ | Weighted blended, dual MRT |
| Decoupled renderer from server physics | ✅ | Fixed-timestep accumulator in `host.c:861` — physics at `sys_ticrate` (20 Hz), render uncapped |
| Triple-buffering / frames in flight | ❌ | Ironwail uses `FRAMES_IN_FLIGHT=3` with GPU fence sync |
| Persistent mapped buffers | ❌ | `ARB_buffer_storage`, `GL_MAP_PERSISTENT_BIT` |
| Bindless textures | ❌ | `ARB_bindless_texture` — zero bind overhead |
| Reversed-Z depth buffer | ❌ | `ARB_clip_control` — eliminates z-fighting |
| SIMD mipmap generation | ❌ | SSE2 fast-path downsample |
| IQM skeletal model support | ❌ | Runtime skeletal animation |

## Rendering — Visual/Shading

| Feature | Status | Notes |
|---|---|---|
| Shader-based fog | ✅ | `gl_fog.c`, density/RGB/fade |
| Lightstyle interpolation | ✅ | `r_lerplightstyles` |
| Model frame interpolation | ✅ | `r_lerpmodels`, `r_lerpmove` |
| Overbright model lighting | ✅ | `gl_overbright_models` |
| Fast sky | ✅ | `r_fastsky` |
| Skybox support | ✅ | `svc_skybox`, cubemap loading |
| Sky fog | ✅ | `r_skyfog` |
| Sky alpha | ✅ | `r_skyalpha` |
| Lightmapped liquid surfaces | ✅ | Per-type alpha (`r_wateralpha`, `r_lavaalpha`, etc.) |
| Water warp distortion | ✅ | `r_waterwarp` |
| Projected mesh shadows | ✅ | `r_shadows`, stencil-projected |
| Fullbright texture support | ✅ | `gl_fullbrights` |
| Render scale | ✅ | `r_scale`, FBO pipeline |
| Software rendering emulation | ✅ | `r_softemu` (dithered, banded, palette LUT) |
| Post-process pipeline | ✅ | Gamma, contrast, palette, dither, FXAA, HDR |
| MSAA with FBO resolve | ✅ | Multisampled scene FBO |
| Animated sky wind system | ❌ | `r_skywind`, per-skybox wind direction/amplitude |
| Bounding box debug visualization | ❌ | `r_showbboxes` with filter modes |
| MD3 model support | ❌ | GPU-compressed 8-byte vertex decoding |
| Gun FOV scale | ✅ | `cl_gun_fovscale` — 0–1 distortion correction blend (Ironwail-style) |
| LOD bias auto-scaling | ❌ | `gl_lodbias "auto"` based on FSAA level |
| Entity alpha radix sort | ❌ | `r_alphasort` — Hexenwail has OIT but not the radix sort path |

## Performance / Engine

| Feature | Status | Notes |
|---|---|---|
| Reduced heap usage / auto-grow | ✅ | Large maps load without `-heapsize` |
| FPS cap with menu slider | ✅ | `host_maxfps` in Display menu |
| CSQC (client-side QuakeC) | ✅ | `cl_csqc.c` |
| Faster map loading | 🔶 | Lightmap atlas yes; VBO build optimizations partial |
| Async main-thread task queue | ❌ | Non-blocking parallel work dispatch |
| Intelligent autosave system | ❌ | Save on health change/secret/teleport/rest |
| Unicode path support | ❌ | Non-ASCII directory names |
| Server profiling | ➖ | `serverprofile` — stock Quake feature |

## UX / Menus / HUD

| Feature | Status | Notes |
|---|---|---|
| Mods menu | ✅ | Directory scan |
| Mouse-driven menus | ✅ | Cursor hover + click |
| Key binding menu | ✅ | `M_Menu_Keys_f` |
| Display/Sound/Game submenus | ✅ | Reorganized options |
| FOV slider | ✅ | In options menu |
| FPS counter | ✅ | `scr_showfps` |
| Borderless window | ✅ | `vid_borderless` |
| Desktop fullscreen | ✅ | `vid_config_fscr` |
| FSAA mode selection in menu | 🔶 | `vid_fsaa` integer only, no mode picker |
| HUD / statusbar scaling | ❌ | `scr_sbarscale` — Ironwail multi-canvas system |
| Menu scaling | ❌ | `scr_menuscale` |
| Crosshair scaling | ❌ | `scr_crosshairscale` |
| Console alpha | ❌ | `scr_conalpha` |
| Console brightness | ❌ | `scr_conbrightness` |
| Menu background style | ❌ | `scr_menubgstyle` |
| Center-print background | ❌ | `scr_centerprintbg` |
| Console mouse support | ❌ | Clickable links, text selection, clipboard |
| Console notification fade | ❌ | `con_notifyfade` |
| Console max columns | ❌ | `con_maxcols` |
| Menu search with filtering | ❌ | Live filter + match highlighting |
| Menu live preview | ❌ | `ui_live_preview` fade-in/hold/fade-out |
| Show speed / show time overlays | ❌ | Speed + time HUD elements |

## Input / Controller

| Feature | Status | Notes |
|---|---|---|
| Full gamepad support | ✅ | SDL game controller API |
| Controller rumble | ✅ | `joy_rumble` |
| Analog stick deadzone/easing | ✅ | Basic form; Ironwail has inner/outer/exponent |
| Advanced deadzone curves | 🔶 | Missing inner/outer threshold, exponent curves |
| Flick stick | ❌ | `joy_flick` |
| Gyroscope aiming | ❌ | `gyro_enable`, calibration, noise filtering |
| Second gamepad binding layer | ❌ | Modifier button for alternate actions |
| Controller type detection | ❌ | Xbox/PS/Nintendo button label auto-switch |
| Controller LED color | ❌ | Orange for branding |
| Steam Deck OSK detection | ➖ | Steam-specific |

## Audio

| Feature | Status | Notes |
|---|---|---|
| Multi-codec music | ✅ | OGG, FLAC, Opus, MP3, XMP, WAV, UMX |
| Spatial audio / stereo separation | ✅ | Standard 3D positioning |
| Underwater audio filter | ❌ | `snd_waterfx` |
| Sound filter quality | ❌ | `snd_filterquality` |

## Network / Protocol

| Feature | Status | Notes |
|---|---|---|
| FitzQuake protocol extensions | ✅ | Fog, skybox, alpha — adapted to Hexen II svc numbering |
| RMQ protocol flags | ➖ | `PRFL_FLOATCOORD` etc. — Quake-specific |
| Quake 2021 re-release messages | ➖ | `svc_achievement` etc. — Quake-specific |

## Steam / Platform

| Feature | Status | Notes |
|---|---|---|
| Steam integration | ➖ | Hexen II not on Steam |
| Steam Quake 2021 auto-detect | ➖ | Quake-specific |

---

## Priority Shortlist (highest impact, applicable to Hexen II)

### P1 — High
1. **HUD/menu/crosshair scaling** (`scr_sbarscale`, `scr_menuscale`, `scr_crosshairscale`)
2. **Persistent mapped buffers** — lock-free GPU upload, big perf win
3. **Reversed-Z depth** — eliminates z-fighting on large maps

### P2 — Medium
6. **Console alpha/brightness** (`scr_conalpha`, `scr_conbrightness`) — low effort
7. **Sky wind system** (`r_skywind`) — visual polish
8. **Underwater audio filter** (`snd_waterfx`) — immersion
9. **Triple-buffering / frames in flight** — smoother frame pacing
10. **Gyroscope aiming** — Steam Deck users

### P3 — Low
11. **Menu search** — nice UX for large option sets
12. **Console mouse support** — clickable links, selection
13. **Flick stick** — niche but game-agnostic
14. **IQM skeletal models** — future mod support
15. **MD3 model support** — future mod support

---

## Hexenwail Exclusives (not in Ironwail)

Features Hexenwail has that Ironwail does NOT:

| Feature | Notes |
|---|---|
| HDR tone mapping (ACES) | `r_hdr` with exposure control |
| Motion blur | `r_motionblur` with view delta tracking |
| FXAA | `gl_fxaa` toggle |
| Alpha-to-coverage | `r_alphatocoverage` |
| WebGL2 / WASM build | Emscripten + ES3 fallback, 1.4 MB binary |
| Hexen II class system | 5 player classes with unique HUD/inventory |
| Viewmodel FOV | `r_viewmodel_fov` (separate from main FOV) |
| Zoom system | `scr_zoomfov`, `scr_zoomspeed` |
| Graphics presets | Crunchy/Retro/Faithful/Clean/Modern/Ultra |
| Glow system | Per-type glow rendering with intensity control |
| Water ripple shader | `gl_waterripple` 0–3 modes |
| External texture overrides | `r_texture_external` hires replacement |
| FluidSynth MIDI | Native MIDI playback |
