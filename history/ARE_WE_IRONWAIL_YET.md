# Are We Ironwail Yet?

Feature parity tracker: **Hexenwail** vs **Ironwail**

Last updated: 2026-06-19 (MSAA with FBO resolve ✅→➖: vestigial window MSAA dropped in `5b6d57ec4`, 'Antialiasing' menu now routes to FXAA — we never had a multisampled scene FBO, only a multisampled window surface that caused the glass/screen-door bug. Added two Hexenwail exclusives: `r_lightmap_bicubic` bicubic lightmap filter (`f52826282`, uhexen2-b2f0) and `r_alias_stochastic_alpha` Wronski/Wyman hashed alpha-test probe (`c1fef9dfa`). Prior: promoted Ironwail `017fdd2` ➖→✅ — fullbright sample in `sworld_frag` moved above the alpha-test `discard`.)

Legend: ✅ Ported | 🔶 Partial | ❌ Missing | ➖ N/A (Quake-specific or irrelevant)

---

## Scorecard

| Category | ✅ | 🔶 | ❌ | ➖ |
|---|---|---|---|---|
| Rendering — GPU Pipeline | 11 | 2 | 1 | 0 |
| Rendering — Visual/Shading | 23 | 0 | 0 | 1 |
| Performance / Engine | 11 | 0 | 0 | 1 |
| UX / Menus / HUD | 24 | 0 | 0 | 1 |
| Input / Controller | 9 | 0 | 0 | 1 |
| Audio | 3 | 0 | 0 | 1 |
| Network / Protocol | 1 | 0 | 0 | 2 |
| Steam / Platform | 0 | 0 | 0 | 2 |
| **TOTAL** | **82** | **2** | **1** | **9** |

**Parity: ~97% ported, 2% partial, 1% missing** (excluding N/A)

---

## Rendering — GPU Pipeline

| Feature | Status | Notes |
|---|---|---|
| GPU frustum culling (compute shader) | ✅ | `gl_worldcull.c` |
| Indirect multi-draw for world surfaces | ✅ | `glMultiDrawElementsIndirect` per texture bucket |
| Brush-entity batched dispatch | ✅ | `r_brush_inst` (default 1) — collected by `R_CollectBrushInstances`, dispatched by `R_DrawBrushInstanced` via `gl_shader_world` (same shader as world surfaces) with per-entity `glUniformMatrix4fv` + per-(instance, texture) `glDrawElements`. Diverges from Ironwail's MDI: routing both world and brush-ent draws through one compiled program eliminates cross-shader 1-ULP `gl_Position` drift that was causing intermittent z-fight (uhexen2-a0t2 / uhexen2-mf45). **Note:** Ironwail commit ebfdc662 optimizes the bindless fallback path for `DRAW_ID` uniform (MDI bmodel batching with `GL_ARB_shader_draw_parameters` fallback). This is not applicable to Hexenwail's architecture (uhexen2-6s0t marked not applicable). |
| SSBO alias model instanced batching | ✅ | `gl_rmain.c` |
| SSBO GPU particles | ✅ | `r_part.c` |
| Order-Independent Transparency (OIT) | ✅ | Weighted blended, dual MRT |
| Decoupled renderer from server physics | ✅ | Fixed-timestep accumulator in `host.c:861` — physics at `sys_ticrate` (20 Hz), render uncapped |
| Triple-buffering / frames in flight | ✅ | `gl_buffer.c` ring with `FRAMES_IN_FLIGHT=3` + `glFenceSync` (uhexen2-8pc2, commit `32bdbea5`). `GL_AcquireFrameResources`/`GL_ReleaseFrameResources` wired into `GL_BeginRendering`/`GL_EndRendering`. All per-frame uploads stream through the ring. |
| Persistent mapped buffers | ✅ | `gl_buffer.c` opens `ARB_buffer_storage` with `GL_MAP_PERSISTENT_BIT \| GL_MAP_COHERENT_BIT` when available (uhexen2-8pc2). Used by alias instances (main + fullbright passes), GPU world-cull PVS bitvector (uhexen2-o35n), and the immediate-mode emulator (uhexen2-y1v5: `GL_ImmEnd`/`GL_ImmDraw` route through `GL_Upload` + `glBindVertexBuffer` via ARB_vertex_attrib_binding). |
| Hi-Z occlusion culling | ✅ | Previous-frame depth pyramid + per-AABB rejection inside `cull_mark` compute (uhexen2-xd87, commits `d58198a1`/`2f8376297`).  Decoupled from the postprocess pipeline 2026-05-12 (uhexen2-9912, `bddc22128`) — standalone depth resolve in `gl_worldcull.c` works whether or not FXAA/HDR/etc. are on.  `gl_hiz_cull` flipped to default **1** after the acceptance sweep (uhexen2-8pzr) measured 44-58% cull rate on demo1 vistas, well above the ≥10% gate. |
| Bindless textures | ❌ | **Skeleton only — non-functional (uhexen2-ubsu; original audit: uhexen2-mxx5.5).** Shaders compile with `BINDLESS=0` unconditionally because `GL_Shaders_Init()` runs before `gl_bindless_able` is set in `gl_vidsdl.c`. `TEX_BINDLESS` flag has zero call sites: `TexMgr_LoadImage` and every other loader ignore it; `glt->bindless_handle` is always 0; the SSBO upload at `gl_rsurf.c:2061-2062` writes zero handles. `gltexture_t` is also defined twice (`glquake.h:130` vs `gl_texmgr.h:46`) — only one is on the draw path. Sampler objects created and never bound. No `ARB_gpu_shader_int64` capability check. The `+bindless` flag does nothing observable. |
| Reversed-Z depth buffer | ✅ | `ARB_clip_control` — `gl_vidsdl.c:893` detects `glClipControl`, switches clip space to `[0,1]`; `GL_Frustum` (`gl_matrix.c:222`), R_Clear/mirror split, viewmodel near-clip, sky pin all flipped to `GEQUAL` / far=0, near=1 |
| SIMD mipmap generation | ✅ | `GL_MipMap_W` / `GL_MipMap_H` split with `__SSE2__` fast-paths (`_mm_avg_epu8`) in `gl_draw.c`. Combined downsample now does W-pass + H-pass with Ironwail's `(a+b+1)>>1` rounding. Scalar fallback retained for non-x86 builds. |
| MD5mesh / IQM skeletal model support | 🔶 | **~30%, would crash on first real load (uhexen2-7ok0; original audit: uhexen2-mxx5.6).** Title was misleading — there is no `.iqm` parser, no `iqm.h`; `iqmvert_t` is just an internal vertex struct name. MD5mesh parser exists (`md5mesh.c`) but: 96KB+128KB stack arrays SIGSEGV on first real load; stores stack-address into hunk pointer (use-after-stack); bone-pose data never allocated, SSBO uploads garbage; rest-pose XYZ zeroed (every vertex collapses to origin); bone weights not normalized. No `.md5anim` parser anywhere — no animation, hardcoded `numposes=1`. Skeletal shader is never invoked: zero `gl_shader_skeletal` draw-site references, no `PV_IQM` branch in `R_DrawAliasModel`. d18ca709c is mislabeled — 265 of 315 lines are unrelated Bloom code. |

## Rendering — Visual/Shading

| Feature | Status | Notes |
|---|---|---|
| Shader-based fog | ✅ | `gl_fog.c`, density/RGB/fade — EXP2 falloff with /64 density divisor (matches Ironwail) |
| Lightstyle interpolation | ✅ | `r_lerplightstyles` |
| Model frame interpolation | ✅ | `r_lerpmodels` (master toggle), `r_lerp_viewmodel` (weapon-only, default off) — Ironwail pose-driven lerp ported (commits `e92a2f401`, `e3be2acce`): tracks `(previouspose, currentpose)` at render time instead of server-frame edges; measured-interval lerptime adapts to observed animation speed; stale-state threshold (`2×lerptime`) snaps to prevent zombie-pose blending. Both cvars toggleable from Options → Game ("Anim Smoothing" / "Smooth Weapon"). Client-side substitute for Ironwail's `LERP_FINISH` server protocol bit, which Hexen II's update protocol lacks. |
| World lightmap overbright | ✅ | `gl_overbright` (default 1) — Ironwail-style: lightmap atlas built at `>>(7+1)` (true 1× range), world/brush fragment shaders multiply `tex*lm*2.0` so combined product can saturate to white. Live toggle re-stitches atlas via `R_RebuildAllLightmaps` callback. (commit `9bd137aef`, uhexen2-f29y) |
| Overbright model lighting | ✅ | `gl_overbright_models` |
| Fast sky | ✅ | `r_fastsky` |
| Skybox support | ✅ | `svc_skybox`, cubemap loading |
| Sky fog | ✅ | `r_skyfog` |
| Sky alpha | ✅ | `r_skyalpha` |
| Lightmapped liquid surfaces | ✅ | Per-type alpha (`r_wateralpha`, `r_lavaalpha`, etc.) |
| Underwater caustics | ✅ | `r_caustics` (default 1), `r_caustics_intensity` — animated caustic texture projected onto world surfaces below the waterline. Registered in `gl_rmisc.c`, sampled in `sworld_frag`. (commit `7b82afce6`, uhexen2-6bfm closed) |
| Water warp distortion | ✅ | `r_waterwarp` |
| Projected mesh shadows | ✅ | `r_shadows`, stencil-projected |
| Fullbright texture support | ✅ | `gl_fullbrights` |
| Render scale | ✅ | `r_scale`, FBO pipeline |
| Software rendering emulation | ✅ | `r_softemu` (dithered, banded, palette LUT) |
| Post-process pipeline | ✅ | Gamma, contrast, palette, dither, HDR |
| MSAA with FBO resolve | ➖ | Hexenwail uses FXAA instead. The scene FBO was never multisampled (`PP_CreateFBO` uses `samples=0`); the SDL window previously accepted MSAA hints when 'Antialiasing' was on, but that multisampled the default framebuffer only — causing the glass/screen-door bug via `GL_SAMPLE_ALPHA_TO_COVERAGE` (uhexen2-zroc). Vestigial window MSAA dropped in commit `5b6d57ec4`; 'Antialiasing' menu item now routes to FXAA. |
| Gun FOV scale | ✅ | `cl_gun_fovscale` — 0–1 distortion correction blend |
| Animated sky wind system | ✅ | Global `r_skyspeed_back`/`r_skyspeed_front` (defaults 8/16) plus per-skybox wind via Ironwail-format `gfx/env/<name>wind.cfg` (`skywind dist yaw period pitch`) — parsed by `Sky_LoadWindCfg`, triangle-wave phase oscillation via `Sky_UpdateWind`, scaled by global `r_skywind` (default 1), pushed to `u_wind` on `gl_shader_sky` (uhexen2-typa). |
| Bounding box debug visualization | ✅ | `r_showbboxes` 0/1/2 + `r_showbboxes_think` / `r_showbboxes_health` filters + `r_showbboxes_targets` target/targetname highlighting + `r_showbboxes_links` directed reference lines (green = focused → X via QC entity-typed field, red = X → focused).  Center-ray pick → focused entity drawn in white; `health > 0` entities tint red.  uhexen2-4ej9 added `ED_NumFieldDefs` / `ED_FieldDefAt` to `pr_edict.c` so the renderer can walk `pr_fielddefs` directly. |
| MD3 model support | ✅ | GPU-compressed 8-byte vertex decoding (Ironwail parity, commits f63d787+a65a88e). Ported 2026-05-12: Phase 1–5 complete (loader, GPU upload path, shader decode). Supports MD3 animation frames and multiple surfaces per model. (uhexen2-f2d3, uhexen2-kaa6 closed). |
| LOD bias auto-scaling | ✅ | `gl_lodbias` with `"auto"` mode — derives bias from active MSAA sample count (uhexen2-dax2, `e40a74d6c`). |
| Entity alpha radix sort | ✅ | `r_alphasort` uses a 4-pass LSD radix sort over the IEEE-754 bit pattern of the squared distance (`R_AlphaSortRadix` in `gl_rmain.c`). Bits are inverted so the ascending unsigned sort yields descending output directly. Stable, O(n); matches Ironwail. |

## Performance / Engine

| Feature | Status | Notes |
|---|---|---|
| Reduced heap usage / auto-grow | ✅ | Large maps load without `-heapsize` |
| Visible-entity cap (MAX_VISEDICTS) | ✅ | Bumped 256 → 16384 in `client.h` to match Ironwail.  At 256, dense maps silently dropped entities past the cap; manifested as multi-reporter "models and brush ents pop at distance" (uhexen2-l0ac).  Companion caps (MAX_ALIAS_INSTANCES, MAX_WORLD_INSTANCES, MAX_WORLD_SURF_KEYS) also scaled. |
| Per-entity leaf cap (MAX_ENT_LEAFS) | ✅ | Bumped 16 → 32 in `progs.h` (ericw/Ironwail parity), and `SV_WriteEntitiesToClient` skips the PVS cull when an entity hit the cap (always sends).  Long brush ents that touched more than 16 BSP leaves had their leaf list truncated; the PVS cull then dropped them from the client write stream as the player moved. |
| FPS cap with menu slider | ✅ | `host_maxfps` in Display menu |
| CSQC (client-side QuakeC) | ✅ | `cl_csqc.c` |
| bmodel buffer rebuilt correctly on map change | ✅ | Ironwail fix `3ccbcda` (2026-02): `GL_DeleteBModelBuffers()` was missing before `GL_BuildBModelVertexBuffer()` in `R_NewMap`, causing GPU memory leak on map changes. We also call `GL_DeleteBModelBuffers` before rebuild. Verify `gl_rmisc.c:R_NewMap`. |
| Alias model GPU data layout | 🔶 | Ironwail `a65a88e` (2026-01) reorganized alias-model GPU pose data to frame-major order (one SSBO bind per model, not per surface) and adds `Mod_NextSurface()`. Hexenwail allocates one SSBO per `aliashdr_t` (per-model header) which avoids the old per-surface alignment loop, but the layout is **not** frame-major: multi-surface models still trigger separate SSBO allocations per surface via `gl_mesh.c:667-690`, and `ssbo_pose_md3` is a separate buffer. Full port tracked as uhexen2-48fx (open, P3, blocked on uhexen2-ayrn). |
| Skybox cache (precache stutter elimination) | ✅ | Ironwail `0603c2bb` port: `skybox_t` struct + LRU-eviction linked list caches up to 16 skyboxes so `precache_sky` commands at map start don't re-upload 6 faces each time. Safe flush on `VID_Restart` and map change. (commit `78933d173`, uhexen2-uqan closed) |
| Faster map loading | ✅ | Lightmap atlas + BSP VBO packing optimized (uhexen2-3mbt, 2026-05-13) |
| Async main-thread task queue | ✅ | `Host_InvokeOnMainThread()` + `AsyncQueue_Drain()` in `host_async.c` — ring buffer with SDL mutex/condition, drained each frame in `Host_Frame`. Emscripten fallback (synchronous). Plus background save thread (uhexen2-9v0s closed). |
| Intelligent autosave system | ➖ | Hexen II saves do not map cleanly to Ironwail's health/secret/teleport trigger heuristics |
| Unicode path support | ✅ | UTF-8 to UTF-16 conversion on Windows (0aa7d3595); POSIX unchanged on Linux. Supports cyrillic, accented Latin, CJK directory names. (uhexen2-ogmq closed). |

## UX / Menus / HUD

| Feature | Status | Notes |
|---|---|---|
| Mods menu | ✅ | Directory scan |
| Mouse-driven menus | ✅ | Cursor hover + click |
| Key binding menu | ✅ | `M_Menu_Keys_f` |
| Display/Sound/Game submenus | ✅ | Reorganized options |
| FOV slider | ✅ | In options menu |
| FPS counter | ✅ | `showfps` (cvar name; menu label "Show FPS") |
| Borderless window | ✅ | `vid_borderless` |
| Desktop fullscreen | ✅ | `vid_config_fscr` |
| Menu key auto-repeat (navigational only) | ✅ | Ironwail commit `6a9610f` (2026-01): `M_Keydown` gains `repeat` bool arg; only arrow keys pass repeat. Hexenwail already has `M_Keydown (key, key_repeats[key] > 1)` with identical arrow-key-only filter — `menu.c:6024`, `keys.c:1099`. |
| Mods menu dirs-with-spaces | ✅ | Ironwail commit `51a911b` (2026-03): added quotes around dir name in `game` command. Hexenwail already uses `game \"%s\"` at `menu.c:4009`. |
| FSAA mode selection in menu | ✅ | `vid_config_fsaa` cycle in Display submenu walks `msaa_steps[]` (0/2/4/8/16/32) clamped to `GL_MAX_SAMPLES` queried at GL init. Labels render as "Off" / "Nx" instead of bare integers. |
| HUD / statusbar scaling | ✅ | `scr_sbarscale` — `CANVAS_SBAR` in `gl_draw.c`, slider in Misc/HUD submenu (`menu.c`) |
| Menu scaling | ✅ | `scr_menuscale` — `CANVAS_MENU`, M_CenterOfs() helper, M_ScreenYToCanvasY for mouse hit-test |
| Crosshair scaling | ✅ | `scr_crosshairscale` — `CANVAS_CROSSHAIR`, slider in Misc/HUD submenu |
| Console alpha | ✅ | `scr_conalpha` — caps `Draw_ConsoleBackground` alpha, slider in Misc/HUD submenu |
| Console brightness | ✅ | `scr_conbrightness` — multiplies conback RGB, slider in Misc/HUD submenu |
| Menu background style | ✅ | `scr_menubgstyle` (default 1) — 0=off / 1=simple dim (Draw_FadeScreen) / 2=dim+translucent backdrop quad over the menu-item area in CANVAS_MENU. Display submenu cycles Off/Simple/Menu Box. Replaces legacy `scr_menufade`. |
| Center-print background | ✅ | `scr_centerprintbg` (gl_screen.c:116) with menu cycle Off / Simple / Menu Box (`menu.c:2849`). Default 2 (Ironwail parity, `df5219c`). Mode 1 = full-width thin dim strip (alpha 0.30), mode 2 = text-width box (alpha 0.50). |
| Console mouse support | ✅ | **Phase 1 complete** (2026-05-12: commit e091812e7): text selection via drag with visual blue highlight, clipboard copy (Ctrl+C with fallback to abort), select-all (Ctrl+A), cursor shape feedback (I-beam/pointer). **Phase 2 complete** (2026-05-13: commit 77bb5727a): URL detection (http/https/ftp), blue underlines with alpha feedback on hover, hand cursor (MCURSOR_HAND) over URLs, SDL_OpenURL on click. |
| Console notification fade | ✅ | `con_notifyfade` (default 1) — alpha ramps 1→0 over the last 1 s of `con_notifytime`. Per-quad alpha threaded through `Draw_AddCharQuad` via new `Draw_SetCharacterAlpha` setter (gl_draw.c). |
| Console max columns | ✅ | `con_maxcols` (default 0 = no cap), menu slider in Misc/HUD submenu (`menu.c`, commit `ab108d760`) |
| Menu search with filtering | ✅ | Shared `M_Filter_*` facility — type printable chars in Display/Rendering/Graphics/Game submenus to live-filter rows by case-insensitive substring; backspace edits, ESC clears (then exits on second press), up/down skip filtered rows, cursor snaps to first match on filter change. Sound (4 rows) and Gamepad (controller-driven) intentionally not wired (uhexen2-rawq). |
| Menu live preview | ✅ | Restored 2026-05-13 (commit d29f8c04a) — backdrop briefly dims for 0.8s when presets are changed in Display menu, showing the effect without menu text flashing. |
| Show speed / show time overlays | ✅ | `scr_showspeed`, `showclock` (4-state: off / game-time / wall HH:MM / wall HH:MM:SS); both toggleable from Misc/HUD submenu |
| Map-editor auto-check on launch | ➖ | Ironwail commit `5a983620` (2026-05): `Sys_IsStartedFromMapEditor` detects Qrucible parent process, triggers map check. Hexenwail has no equivalent function and no TrenchBroom workflow integration. Could be ported but low priority for Hexen II mapping scene. |

## Input / Controller

| Feature | Status | Notes |
|---|---|---|
| Full gamepad support | ✅ | SDL game controller API |
| Controller rumble | ✅ | `joy_rumble` |
| Analog stick deadzone/easing | ✅ | Inner deadzone + power-curve easing (`joy_deadzone_look/move`, `joy_exponent`/`_move`) |
| Second gamepad binding layer | ✅ | `+altmodifier` modifier button for alternate bindings. Bindable from the Keys menu (Ironwail `7a2038a`, uhexen2-qeyd): hold the alt-modifier while in bind mode and the row shows `Alt-???`, the prompt reads "Press a gamepad button for the ALT layer", and a base GP press is redirected to its `_ALT` variant. The menu find/unbind scans were also widened from `<256` to `<MAX_KEYS` so the `K_GP_*_ALT` (256–267) and `K_GP_DPAD_*` (268–271) bindings are finally visible and clearable in the menu. |
| Outer-edge deadzone saturation | ✅ | `joy_outer_threshold_look/move` (uhexen2-0g4t) — per-stick saturation thresholds replace the hardcoded 0.02 in IN_ApplyDeadzone |
| Flick stick | ✅ | `joy_flick`, `joy_flick_time`, `joy_flick_deadzone`, `joy_flick_noise_thresh`, `joy_flick_recenter`, `joy_flick_adjust_speed` (uhexen2-98oo / uhexen2-s2vv). Tri-state machine in `IN_FlickStickUpdate` (idle/rotating/tracking), smoothstep animation, noise-gated 1:1 tracking. |
| Gyroscope aiming | ✅ | `gyro_enable`, `gyro_mode` (always / suppress-on-stick / +gyroactive / inverted), `gyro_turning_axis`, `gyro_yawsensitivity`, `gyro_pitchsensitivity`, `gyro_noise_thresh`, `gyro_calibration_x/y/z`, `gyro_calibrate` command, `+gyroactive`/`-gyroactive` binds (uhexen2-xpbi / uhexen2-s2vv). |
| Controller type detection | ✅ | `IN_GetGamepadType` reads `SDL_GetGamepadType` on open; `Key_KeynumToDisplayString` renders brand-correct face-button labels in the bind menu (Xbox A/B/X/Y, PS Cross/Circle/Square/Triangle, Switch B/A/Y/X with physical-print swap). uhexen2-asln. |
| Controller LED color | ✅ | `joy_led` "r g b" 0-255 string; applied via `SDL_SetGamepadLED` on gamepad open and on cvar change (uhexen2-3fpt). |
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
| `017fdd2` 2026-01 | Dark outlines on fence textures with dynamic lights: compute plane before `discard` | ✅ Ported 2026-06-12 (uhexen2-9a1l): the same hazard hits us through `texture()`'s implicit dFdx/dFdy, not a hand-written plane equation. In `sworld_frag` the fullbright mask sample was the only post-`discard` texture fetch and was producing undefined mip selection for surviving lanes in a 2×2 quad where peers discarded — manifesting as dark fence-edge outlines. Sample now happens above the alpha-test `discard`. Other discard sites audited clean. |
| `1011ff8` 2026-01 | Disable GL texture compression for alpha-tested surfaces | ➖ N/A — Hexenwail does not have a `gl_compress_textures` system. |
| `74d8e74` 2026-01 | Disable GL texture compression for 2D textures (HUD, conchars) | ➖ N/A — same reason as above. |
| `80387f1` 2026-01 | Crash toggling `gl_compress_textures`: cubemap textures stored pointers to stack data | ➖ N/A — no compression system. |
| `78ad272` 2026-01 | Stop controller rumble when sound buffer is cleared (e.g. modal message) | ✅ Already present — `S_ClearBuffer` calls `IN_GPRumble(0, 0, 0)` at `snd_dma.c:684` (uhexen2-xq1c closed). |
| ericw (pre-Ironwail) | MAX_ENT_LEAFS 16→32 + always-send on cap overflow in `SV_WriteEntitiesToClient` | ✅ Ported 2026-05-11 (uhexen2-l0ac follow-up): long brush ents (lifts, rotators) flickered out at distance because their leaf list overflowed and the PVS cull dropped them. |
| `7a2038a` 2026-01 | Menu support for binding the gamepad alt-modifier layer | ✅ Ported 2026-06-11 (uhexen2-qeyd): `M_Keybind` redirects a base GP press to its `_ALT` variant while the modifier is held (and ignores a press of the modifier key itself); `M_Keys_Draw` previews with `Alt-???` / an ALT-layer prompt. Also fixed the menu's `<256` find/unbind scan that hid all `K_GP_*_ALT` and `K_GP_DPAD_*` bindings. |

---

## Bead Coverage

As of 2026-06-19, three features are non-complete: bindless ❌ Missing (skeleton only — uhexen2-ubsu), MD5 🔶 Partial (~30%, parser only — uhexen2-7ok0), and Alias model GPU data layout 🔶 Partial (per-aliashdr_t but not Ironwail frame-major — uhexen2-48fx). MSAA with FBO resolve reclassified ➖ (Hexenwail intentionally uses FXAA; vestigial window MSAA dropped in `5b6d57ec4`). The umbrella epic `uhexen2-a5nn` enumerates the full set grouped by category. Run `bd show uhexen2-a5nn` for the current child list. Scorecard: 82 ✅ / 2 🔶 / 1 ❌ / 9 ➖ (~97% parity).

When porting a parity item, claim the bead with `bd update <id> --status=in_progress`, implement, update the matching row here to ✅, and close the bead with a reference to the landing commit.

---

## Priority Shortlist (highest impact, applicable to Hexen II)

### P1 — High (regressions surfaced 2026-05-15)
1. **Bindless textures** — ❌ Skeleton only (uhexen2-ubsu). Either complete the implementation (shader-init ordering fix, plumb `TEX_BINDLESS` through every loader, deduplicate `gltexture_t`, add `ARB_gpu_shader_int64` capability gate, remove unused sampler-object code) or revert the `+bindless` flag and the bindless code paths.

### P3 — Low
1. **MD5/IQM skeletal models** — 🔶 ~30% (uhexen2-7ok0). Fix critical bugs first (stack overflow, use-after-stack, missing bone-pose alloc, zeroed rest-pose XYZ, weight normalization), then add `.md5anim` parser, then add `PV_IQM` shader branch + draw-site dispatch.

*Async main-thread task queue verified complete 2026-05-13 (uhexen2-9v0s closed — `host_async.c`).*
*MD3 model support completed 2026-05-12 (uhexen2-f2d3, uhexen2-kaa6).*
*Console mouse support (Phase 1+2) completed 2026-05-12–2026-05-13 (uhexen2-8vw0, uhexen2-ei9r, commits e091812e7, 77bb5727a).*
*Faster map loading completed 2026-05-13 (uhexen2-3mbt).*

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
| Bicubic lightmap filter | `r_lightmap_bicubic` — GPU bicubic upsampling of the lightmap atlas in the world fragment shader (commit `f52826282`, uhexen2-b2f0) | Yes |
| Stochastic alias alpha | `r_alias_stochastic_alpha` — Wronski/Wyman hashed alpha-test probe for alias model cutout surfaces; temporally stable under TAA-style accumulation (commit `c1fef9dfa`) | Yes |

**Removed from exclusives (present in both):**

| Feature | Notes |
|---|---|
| Underwater audio filter | Both have `snd_waterfx`. Ironwail: `snd_dma.c:84`. Independently implemented but same cvar name and concept. |
| Zoom system | Ironwail has it (`gl_screen.c:108-109` — `zoom_fov` / `zoom_speed`); Hexen II has no scoped weapons or zoom-driven gameplay, so the `cl.zoom` field at `client.h:210` is an unused stub. Bead uhexen2-mfbe demoted to P4 — intentionally not pursuing parity. |
| Gun FOV scale | `cl_gun_fovscale` exists in Ironwail (`gl_screen.c:117`). This is a shared feature Hexenwail ported from Ironwail. |
| Water ripple shader | `gl_waterripple` exists in Ironwail (`gl_rmain.c:133`). Not a Hexenwail exclusive. |
| External texture overrides | `r_texture_external` exists in Ironwail (`gl_rmain.c:134`). Not a Hexenwail exclusive. |
| Glow system | `gl_glows`, `gl_other_glows`, `gl_missile_glows` exist in Ironwail (`gl_rmain.c:127-129`). Hexenwail's glow variables are more Hexen II-specific in usage but the cvar names are shared. |
