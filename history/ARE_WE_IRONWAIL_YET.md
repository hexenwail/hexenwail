# Are We Ironwail Yet?

Feature parity tracker: **Hexenwail** vs **Ironwail**

Last updated: 2026-05-05

Legend: ✅ Ported | 🔶 Partial | ❌ Missing | ➖ N/A (Quake-specific or irrelevant)

---

## Scorecard

| Category | ✅ | 🔶 | ❌ | ➖ |
|---|---|---|---|---|
| Rendering — GPU Pipeline | 6 | 0 | 6 | 0 |
| Rendering — Visual/Shading | 17 | 0 | 5 | 0 |
| Performance / Engine | 5 | 1 | 1 | 1 |
| UX / Menus / HUD | 9 | 1 | 12 | 1 |
| Input / Controller | 4 | 1 | 4 | 1 |
| Audio | 3 | 0 | 0 | 1 |
| Network / Protocol | 1 | 0 | 0 | 2 |
| Steam / Platform | 0 | 0 | 0 | 2 |
| **TOTAL** | **45** | **3** | **28** | **8** |

**Parity: 59% ported, 4% partial, 37% missing** (excluding N/A)

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
| Model frame interpolation | ✅ | `r_lerpmodels` (frame), `r_lerpmove` (origin) — both default on, two-stage RESETANIM/RESETANIM2 reset |
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
| Post-process pipeline | ✅ | Gamma, contrast, palette, dither, HDR |
| MSAA with FBO resolve | ✅ | Multisampled scene FBO |
| Gun FOV scale | ✅ | `cl_gun_fovscale` — 0–1 distortion correction blend |
| Animated sky wind system | ❌ | `r_skywind`, per-skybox wind direction/amplitude |
| Bounding box debug visualization | ❌ | `r_showbboxes` with filter modes (`r_showbboxes_think`, `r_showbboxes_health`, etc.) |
| MD3 model support | ❌ | GPU-compressed 8-byte vertex decoding; Ironwail landed this in 2025-10 (commit `f63d787`) with continued refinements through 2026-01 |
| LOD bias auto-scaling | ❌ | `gl_lodbias "auto"` based on FSAA level |
| Entity alpha radix sort | ❌ | `r_alphasort` — Hexenwail has OIT but not the radix sort path |

## Performance / Engine

| Feature | Status | Notes |
|---|---|---|
| Reduced heap usage / auto-grow | ✅ | Large maps load without `-heapsize` |
| FPS cap with menu slider | ✅ | `host_maxfps` in Display menu |
| CSQC (client-side QuakeC) | ✅ | `cl_csqc.c` |
| bmodel buffer rebuilt correctly on map change | ✅ | Ironwail fix `3ccbcda` (2026-02): `GL_DeleteBModelBuffers()` was missing before `GL_BuildBModelVertexBuffer()` in `R_NewMap`, causing GPU memory leak on map changes. We also call `GL_DeleteBModelBuffers` before rebuild. Verify `gl_rmisc.c:R_NewMap`. |
| Alias model GPU data layout | ✅ | Ironwail refactored `a65a88e` (2026-01): SSBO alignment removed per-surface, IQM bind-pose separated. Our alias pipeline layout matches conceptually — no separate SSBO alignment loop needed given our model format. |
| Faster map loading | 🔶 | Lightmap atlas yes; VBO build optimizations partial |
| Async main-thread task queue | ❌ | Non-blocking parallel work dispatch |
| Intelligent autosave system | ➖ | Hexen II saves do not map cleanly to Ironwail's health/secret/teleport trigger heuristics |
| Unicode path support | ❌ | Non-ASCII directory names |

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
| Menu key auto-repeat (navigational only) | ✅ | Ironwail commit `6a9610f` (2026-01): `M_Keydown` gains `repeat` bool arg; only arrow keys pass repeat. Hexenwail already has `M_Keydown (key, key_repeats[key] > 1)` with identical arrow-key-only filter — `menu.c:6024`, `keys.c:1099`. |
| Mods menu dirs-with-spaces | ✅ | Ironwail commit `51a911b` (2026-03): added quotes around dir name in `game` command. Hexenwail already uses `game \"%s\"` at `menu.c:4009`. |
| FSAA mode selection in menu | 🔶 | `vid_fsaa` integer only, no mode picker |
| HUD / statusbar scaling | ❌ | `scr_sbarscale` — Ironwail multi-canvas system |
| Menu scaling | ❌ | `scr_menuscale` |
| Crosshair scaling | ❌ | `scr_crosshairscale` |
| Console alpha | ❌ | `scr_conalpha` |
| Console brightness | ❌ | `scr_conbrightness` |
| Menu background style | ❌ | `scr_menubgstyle` |
| Center-print background | ❌ | `scr_centerprintbg` — Ironwail changed default to 2 (menu box) in `df5219c` (2026-01). We have the cvar and the option in menu (`menu.c:2785`) but confirm default value matches. |
| Console mouse support | ❌ | Clickable links, text selection, clipboard |
| Console notification fade | ❌ | `con_notifyfade` |
| Console max columns | ❌ | `con_maxcols` |
| Menu search with filtering | ❌ | Live filter + match highlighting |
| Menu live preview | ❌ | `ui_live_preview` fade-in/hold/fade-out |
| Show speed / show time overlays | ❌ | Speed + time HUD elements |
| Map-editor auto-check on launch | ➖ | Ironwail commit `5a983620` (2026-05): `Sys_IsStartedFromMapEditor` detects Qrucible parent process, triggers map check. Hexenwail has no equivalent function and no TrenchBroom workflow integration. Could be ported but low priority for Hexen II mapping scene. |

## Input / Controller

| Feature | Status | Notes |
|---|---|---|
| Full gamepad support | ✅ | SDL game controller API |
| Controller rumble | ✅ | `joy_rumble` |
| Analog stick deadzone/easing | ✅ | Basic form; Ironwail has inner/outer/exponent |
| Second gamepad binding layer | ✅ | `+altmodifier` modifier button for alternate bindings |
| Advanced deadzone curves | 🔶 | Missing inner/outer threshold, exponent curves |
| Flick stick | ❌ | `joy_flick`, `joy_flick_time`, `joy_flick_deadzone`, `joy_flick_noise_thresh` |
| Gyroscope aiming | ❌ | `gyro_enable`, `gyro_mode`, `gyro_yawsensitivity`, calibration, noise filtering |
| Controller type detection | ❌ | Xbox/PS/Nintendo button label auto-switch |
| Controller LED color | ❌ | Orange for branding |
| Steam Deck OSK detection | ➖ | Steam-specific |

## Audio

| Feature | Status | Notes |
|---|---|---|
| Multi-codec music | ✅ | OGG, FLAC, Opus, MP3, XMP, WAV, UMX |
| Spatial audio / stereo separation | ✅ | Standard 3D positioning |
| Underwater audio filter | ✅ | `snd_waterfx` — IIR low-pass on the paint buffer. Note: Ironwail also has `snd_waterfx` (`snd_dma.c:84`) — this is NOT a Hexenwail exclusive. Both implementations are independently convergent. |
| Sound filter quality | ➖ | `snd_filterquality` cleans up Ironwail's paint-time zero-stuff upsample. Our pipeline resamples at load (`snd_mem.c:30 ResampleSfx`), so the filter has no equivalent precondition here. |

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

## Bug Fixes from Ironwail (applicable to Hexenwail)

Recent Ironwail bug fixes assessed for Hexenwail applicability:

| Ironwail commit | Fix | Hexenwail status |
|---|---|---|
| `3ccbcda` 2026-02 | bmodel VBO leak on map change: `GL_DeleteBModelBuffers()` missing before rebuild in `R_NewMap` | ✅ We call delete before rebuild |
| `6a9610f` 2026-01 | Menu key auto-repeat: only navigational keys pass repeat events | ✅ Already ported |
| `51a911b` 2026-03 | Mods menu: game command not quoted, breaks dirs with spaces | ✅ Already quoted |
| `0a6084a` 2026-04 | Pitch drift during cutscenes: `V_StopPitchDrift()` not called when `CL_InCutscene()` | ❌ Hexen II has `svc_cutscene` (sets `cl.intermission=3`) but `CL_AdjustAngles` has no intermission guard. Pitch drift during Hexen II intermission screens is plausible bug. Low effort fix: add `if (cl.intermission) { V_StopPitchDrift(); return; }` at top of `cl_input.c:CL_AdjustAngles`. |
| `017fdd2` 2026-01 | Dark outlines on fence textures with dynamic lights: compute plane before `discard` | ➖ N/A — Hexenwail has no clustered per-tile dynamic lighting. Our world shader (`h2shared/gl_shader.c:sworld_frag`) does not compute a plane variable at all. |
| `1011ff8` 2026-01 | Disable GL texture compression for alpha-tested surfaces | ➖ N/A — Hexenwail does not have a `gl_compress_textures` system. |
| `74d8e74` 2026-01 | Disable GL texture compression for 2D textures (HUD, conchars) | ➖ N/A — same reason as above. |
| `80387f1` 2026-01 | Crash toggling `gl_compress_textures`: cubemap textures stored pointers to stack data | ➖ N/A — no compression system. |
| `78ad272` 2026-01 | Stop controller rumble when sound buffer is cleared (e.g. modal message) | ❌ May be relevant if we have `joy_rumble`. Check `in_sdl.c:S_ClearBuffer` interaction. |

---

## Priority Shortlist (highest impact, applicable to Hexen II)

### P1 — High
1. **HUD/menu/crosshair scaling** (`scr_sbarscale`, `scr_menuscale`, `scr_crosshairscale`) — critical for high-DPI
2. **Persistent mapped buffers** — lock-free GPU upload, big perf win
3. **Reversed-Z depth** — eliminates z-fighting on large maps
4. **Pitch drift during intermission** — `cl_input.c:CL_AdjustAngles` needs `cl.intermission` guard + `V_StopPitchDrift()` call (Ironwail `0a6084a`; effort: 5 lines)

### P2 — Medium
5. **Console alpha/brightness** (`scr_conalpha`, `scr_conbrightness`) — low effort
6. **Sky wind system** (`r_skywind`) — visual polish
7. **Triple-buffering / frames in flight** — smoother frame pacing
8. **Gyroscope aiming** — Steam Deck users
9. **Advanced gamepad deadzone curves** — inner/outer/exponent knobs
10. **Controller rumble on sound buffer clear** — correctness fix (Ironwail `78ad272`)

### P3 — Low
11. **Menu search** — nice UX for large option sets
12. **Console mouse support** — clickable links, selection
13. **Flick stick** — niche but game-agnostic
14. **IQM skeletal models** — future mod support
15. **MD3 model support** — future mod support

---

## Hexenwail Exclusives (not in Ironwail)

Features Hexenwail has that Ironwail does NOT. Verified against Ironwail origin/master as of 2026-05-01.

| Feature | Notes | Confirmed absent in Ironwail |
|---|---|---|
| HDR tone mapping (ACES) | `r_hdr` with exposure control | Yes — no tonemapping pipeline |
| Motion blur | `r_motionblur` with view delta tracking | Yes |
| FXAA | `gl_fxaa` toggle | Yes — Ironwail has no FXAA |
| Alpha-to-coverage cutout antialiasing | `r_alphatocoverage` — MSAA-based fence edge smoothing | Yes — no `GL_SAMPLE_ALPHA_TO_COVERAGE` usage |
| WebGL2 / WASM build | Emscripten + ES3 fallback, 1.4 MB binary | Yes |
| Hexen II class system | 5 player classes with unique HUD/inventory | Yes — Quake-only engine |
| Per-mod music subdirs | `<gamedir>/music/<author>/<file>.<ext>` lookup so multiple authors can ship tracks without colliding | Yes |
| Track-name remap | `bgm_remap NN <name>` console command — points a numeric CD track at a named music file | Yes |
| Graphics presets | Crunchy/Retro/Faithful/Clean/Modern/Ultra | Yes |
| FluidSynth MIDI | Native MIDI playback | Yes |

**Removed from exclusives (present in both):**

| Feature | Notes |
|---|---|
| Underwater audio filter | Both have `snd_waterfx`. Ironwail: `snd_dma.c:84`. Independently implemented but same cvar name and concept. |
| Zoom system | Both have `zoom_fov` / `zoom_speed`. Ironwail: `gl_screen.c:108-109`. Our codebase has a `cl.zoom` field (`client.h:210`) but no registered cvars — **the zoom system is incomplete/stub in Hexenwail and should NOT be listed as an exclusive.** |
| Gun FOV scale | `cl_gun_fovscale` exists in Ironwail (`gl_screen.c:117`). This is a shared feature Hexenwail ported from Ironwail. |
| Water ripple shader | `gl_waterripple` exists in Ironwail (`gl_rmain.c:133`). Not a Hexenwail exclusive. |
| External texture overrides | `r_texture_external` exists in Ironwail (`gl_rmain.c:134`). Not a Hexenwail exclusive. |
| Glow system | `gl_glows`, `gl_other_glows`, `gl_missile_glows` exist in Ironwail (`gl_rmain.c:127-129`). Hexenwail's glow variables are more Hexen II-specific in usage but the cvar names are shared. |
