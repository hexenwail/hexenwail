# Are We Ironwail Yet?

Feature parity tracker: **Hexenwail** vs **Ironwail**

Last updated: 2026-05-11

Legend: ✅ Ported | 🔶 Partial | ❌ Missing | ➖ N/A (Quake-specific or irrelevant)

---

## Scorecard

| Category | ✅ | 🔶 | ❌ | ➖ |
|---|---|---|---|---|
| Rendering — GPU Pipeline | 7 | 3 | 3 | 0 |
| Rendering — Visual/Shading | 17 | 3 | 2 | 0 |
| Performance / Engine | 7 | 1 | 2 | 1 |
| UX / Menus / HUD | 16 | 1 | 5 | 1 |
| Input / Controller | 4 | 1 | 4 | 1 |
| Audio | 3 | 0 | 0 | 1 |
| Network / Protocol | 1 | 0 | 0 | 2 |
| Steam / Platform | 0 | 0 | 0 | 2 |
| **TOTAL** | **55** | **9** | **16** | **8** |

**Parity: 69% ported, 11% partial, 20% missing** (excluding N/A)

---

## Rendering — GPU Pipeline

| Feature | Status | Notes |
|---|---|---|
| GPU frustum culling (compute shader) | ✅ | `gl_worldcull.c` |
| Indirect multi-draw for world surfaces | ✅ | `glMultiDrawElementsIndirect` per texture bucket |
| Brush-entity batched dispatch | ✅ | `r_brush_inst` (default 1) — collected by `R_CollectBrushInstances`, dispatched by `R_DrawBrushInstanced` via `gl_shader_world` (same shader as world surfaces) with per-entity `glUniformMatrix4fv` + per-(instance, texture) `glDrawElements`. Diverges from Ironwail's MDI: routing both world and brush-ent draws through one compiled program eliminates cross-shader 1-ULP `gl_Position` drift that was causing intermittent z-fight (uhexen2-a0t2 / uhexen2-mf45). |
| SSBO alias model instanced batching | ✅ | `gl_rmain.c` |
| SSBO GPU particles | ✅ | `r_part.c` |
| Order-Independent Transparency (OIT) | ✅ | Weighted blended, dual MRT |
| Decoupled renderer from server physics | ✅ | Fixed-timestep accumulator in `host.c:861` — physics at `sys_ticrate` (20 Hz), render uncapped |
| Triple-buffering / frames in flight | 🔶 | `gl_buffer.c` ring with `FRAMES_IN_FLIGHT=3` + `glFenceSync` infrastructure landed (uhexen2-8pc2, commit `32bdbea5`). Used by alias instance SSBO upload and worldcull PVS upload (uhexen2-o35n); `gl_vbo.c` immediate-mode VBO still on `glBufferData(GL_STREAM_DRAW)` pending separate-binding VAO refactor. |
| Persistent mapped buffers | 🔶 | `gl_buffer.c` opens `ARB_buffer_storage` with `GL_MAP_PERSISTENT_BIT \| GL_MAP_COHERENT_BIT` when available (uhexen2-8pc2). Used by alias instances (main + fullbright passes) and GPU world-cull PVS bitvector (uhexen2-o35n). Immediate-mode VBO (`gl_vbo.c`) still single-buffer pending VAO restructure to separate vertex attribute bindings. |
| Bindless textures | ❌ | `ARB_bindless_texture` — zero bind overhead |
| Reversed-Z depth buffer | ✅ | `ARB_clip_control` — `gl_vidsdl.c:893` detects `glClipControl`, switches clip space to `[0,1]`; `GL_Frustum` (`gl_matrix.c:222`), R_Clear/mirror split, viewmodel near-clip, sky pin all flipped to `GEQUAL` / far=0, near=1 |
| SIMD mipmap generation | ❌ | SSE2 fast-path downsample |
| IQM skeletal model support | ❌ | Runtime skeletal animation |

## Rendering — Visual/Shading

| Feature | Status | Notes |
|---|---|---|
| Shader-based fog | ✅ | `gl_fog.c`, density/RGB/fade — EXP2 falloff with /64 density divisor (matches Ironwail) |
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
| Post-process pipeline | ✅ | Gamma, contrast, palette, dither, HDR |
| MSAA with FBO resolve | ✅ | Multisampled scene FBO |
| Gun FOV scale | ✅ | `cl_gun_fovscale` — 0–1 distortion correction blend |
| Animated sky wind system | 🔶 | Global `r_skyspeed_back`/`r_skyspeed_front` (defaults 8/16). Ironwail's per-skybox direction/amplitude not ported. |
| Bounding box debug visualization | 🔶 | `r_showbboxes` 0/1/2 wired (gl_rmain.c:3056). Ironwail's filter sub-cvars (`r_showbboxes_think`, `_health`, etc.) not ported — easy to add when needed. |
| MD3 model support | ❌ | GPU-compressed 8-byte vertex decoding; Ironwail landed this in 2025-10 (commit `f63d787`) with continued refinements through 2026-01 |
| LOD bias auto-scaling | ❌ | `gl_lodbias "auto"` based on FSAA level |
| Entity alpha radix sort | 🔶 | `r_alphasort` cvar is wired and uses `qsort` (gl_rmain.c:2097). Ironwail's radix sort would be faster but the count is small (≤dozens of translucent entities per frame) — qsort is microseconds either way. |

## Performance / Engine

| Feature | Status | Notes |
|---|---|---|
| Reduced heap usage / auto-grow | ✅ | Large maps load without `-heapsize` |
| Visible-entity cap (MAX_VISEDICTS) | ✅ | Bumped 256 → 16384 in `client.h` to match Ironwail.  At 256, dense maps silently dropped entities past the cap; manifested as multi-reporter "models and brush ents pop at distance" (uhexen2-l0ac).  Companion caps (MAX_ALIAS_INSTANCES, MAX_WORLD_INSTANCES, MAX_WORLD_SURF_KEYS) also scaled. |
| Per-entity leaf cap (MAX_ENT_LEAFS) | ✅ | Bumped 16 → 32 in `progs.h` (ericw/Ironwail parity), and `SV_WriteEntitiesToClient` skips the PVS cull when an entity hit the cap (always sends).  Long brush ents that touched more than 16 BSP leaves had their leaf list truncated; the PVS cull then dropped them from the client write stream as the player moved. |
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
| HUD / statusbar scaling | ✅ | `scr_sbarscale` — `CANVAS_SBAR` in `gl_draw.c`, slider in Misc/HUD submenu (`menu.c`) |
| Menu scaling | ✅ | `scr_menuscale` — `CANVAS_MENU`, M_CenterOfs() helper, M_ScreenYToCanvasY for mouse hit-test |
| Crosshair scaling | ✅ | `scr_crosshairscale` — `CANVAS_CROSSHAIR`, slider in Misc/HUD submenu |
| Console alpha | ✅ | `scr_conalpha` — caps `Draw_ConsoleBackground` alpha, slider in Misc/HUD submenu |
| Console brightness | ✅ | `scr_conbrightness` — multiplies conback RGB, slider in Misc/HUD submenu |
| Menu background style | ❌ | `scr_menubgstyle` |
| Center-print background | ✅ | `scr_centerprintbg` (gl_screen.c:115) with menu cycle Off / Simple / Menu Box (`menu.c:2849`). Default 0 (Ironwail uses 2 since `df5219c`); Hexenwail keeps 0 to preserve stock-faithful look. |
| Console mouse support | ❌ | Clickable links, text selection, clipboard |
| Console notification fade | ❌ | `con_notifyfade` |
| Console max columns | ❌ | `con_maxcols` |
| Menu search with filtering | ❌ | Live filter + match highlighting |
| Menu live preview | ❌ | `ui_live_preview` fade-in/hold/fade-out |
| Show speed / show time overlays | ✅ | `scr_showspeed`, `showclock` (4-state: off / game-time / wall HH:MM / wall HH:MM:SS); both toggleable from Misc/HUD submenu |
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
| `0a6084a` 2026-04 | Pitch drift during cutscenes: `V_StopPitchDrift()` not called when `CL_InCutscene()` | ✅ Ported 2026-05-05: `cl_input.c:CL_AdjustAngles` early-returns with `V_StopPitchDrift()` when `cl.intermission` is set. |
| `017fdd2` 2026-01 | Dark outlines on fence textures with dynamic lights: compute plane before `discard` | ➖ N/A — Hexenwail has no clustered per-tile dynamic lighting. Our world shader (`h2shared/gl_shader.c:sworld_frag`) does not compute a plane variable at all. |
| `1011ff8` 2026-01 | Disable GL texture compression for alpha-tested surfaces | ➖ N/A — Hexenwail does not have a `gl_compress_textures` system. |
| `74d8e74` 2026-01 | Disable GL texture compression for 2D textures (HUD, conchars) | ➖ N/A — same reason as above. |
| `80387f1` 2026-01 | Crash toggling `gl_compress_textures`: cubemap textures stored pointers to stack data | ➖ N/A — no compression system. |
| `78ad272` 2026-01 | Stop controller rumble when sound buffer is cleared (e.g. modal message) | ❌ May be relevant if we have `joy_rumble`. Check `in_sdl.c:S_ClearBuffer` interaction. |
| ericw (pre-Ironwail) | MAX_ENT_LEAFS 16→32 + always-send on cap overflow in `SV_WriteEntitiesToClient` | ✅ Ported 2026-05-11 (uhexen2-l0ac follow-up): long brush ents (lifts, rotators) flickered out at distance because their leaf list overflowed and the PVS cull dropped them. |

---

## Bead Coverage

As of 2026-05-11, every Missing (❌) and Partial (🔶) item in the tables above has a tracking bead.  The umbrella epic `uhexen2-a5nn` enumerates the full set grouped by category (Rendering, Performance, Menus, Input, Models).  Run `bd show uhexen2-a5nn` for the current child list.

When porting a parity item, claim the bead with `bd update <id> --status=in_progress`, implement, update the matching row here to ✅, and close the bead with a reference to the landing commit.

---

## Priority Shortlist (highest impact, applicable to Hexen II)

### P1 — High
1. **Persistent mapped buffers** — 🔶 ring + alias instances + worldcull PVS migrated (uhexen2-o35n); `gl_vbo.c` immediate-mode upload still on `glBufferData` (needs VAO refactor to separate attribute bindings).

### P2 — Medium
5. **Triple-buffering / frames in flight** — smoother frame pacing
7. **Gyroscope aiming** — Steam Deck users
8. **Advanced gamepad deadzone curves** — inner/outer/exponent knobs
9. **Controller rumble on sound buffer clear** — correctness fix (Ironwail `78ad272`)

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
