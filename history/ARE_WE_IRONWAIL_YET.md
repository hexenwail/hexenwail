# Are We Ironwail Yet?

Feature parity tracker: **Hexenwail** vs **Ironwail**

Last updated: 2026-08-18 (**Documentation correctness pass**, prompted by a reader spotting that the Steam row justified ➖ with "Hexen II not on Steam" — Hexen II *is* sold on Steam; the real reason is that the engine needs no Steam-specific code.  Checking that one claim turned up five more, all of the same kind: statements about Ironwail written down without being checked against Ironwail.  **Bindless textures ➖ N/A → ❌ Missing** — the row asserted in three places that "Ironwail does not use `ARB_bindless_texture` either", which is false; Ironwail's path is live, extension-gated in `gl_vidsdl.c`, and used in `r_world.c`.  **Three features were wrongly demoted out of Exclusives** — `gl_waterripple` and the `gl_glows`/`gl_other_glows`/`gl_missile_glows` trio do not exist in Ironwail at all; their supporting citations (`gl_rmain.c:127-129`, `:133`, `:134`) resolve to `map_wateralpha`, `r_scale` and a blank line, i.e. they came from grepping the wrong tree.  `r_texture_external` stays demoted but with a corrected reason.  **The HD replacement packs exclusive added earlier the same day was overstated** — Ironwail already has per-map texture directories and `_glow`/`_luma` sidecars; only the DDS/KTX container path is exclusive.  **MAX_ENT_LEAFS** is 64 here, not 32 — we passed Ironwail in `32c35acdb`.  **Every `file.c:NNN` citation in the document had drifted** and has been converted to file + symbol; see the note above the porting workflow.  Verified-correct claims left alone: `sys_ticrate` 0.05 = 20 Hz, MAX_VISEDICTS 16384 = Ironwail, FRAMES_IN_FLIGHT 3, `msaa_steps[]`, and the `snd_waterfx` / zoom / `cl_gun_fovscale` demotions.  Prior entry (2026-08-18, earlier): audit against Ironwail `4fa8bfbd`; upstream queue empty; pointfile 🔶→✅; `r_alias_stochastic_alpha` dropped; MD5 row rewritten; caustics moved to Exclusives; four bug-fix ports and a tab-completion row added.  Prior entry (2026-08-13): bindlist.lst ✅ and pointfile 🔶 added; scorecard recomputed.)

Legend: ✅ Ported | 🔶 Partial | ❌ Missing | ➖ N/A (Quake-specific or irrelevant)

---

## Scorecard

| Category | ✅ | 🔶 | ❌ | ➖ |
|---|---|---|---|---|
| Rendering — GPU Pipeline | 12 | 1 | 1 | 0 |
| Rendering — Visual/Shading | 22 | 0 | 0 | 1 |
| Performance / Engine | 10 | 1 | 0 | 1 |
| UX / Menus / HUD | 28 | 0 | 0 | 1 |
| Input / Controller | 9 | 0 | 0 | 1 |
| Audio | 3 | 0 | 0 | 1 |
| Network / Protocol | 1 | 0 | 0 | 2 |
| Steam / Platform | 0 | 0 | 0 | 2 |
| **TOTAL** | **85** | **2** | **1** | **9** |

**Parity: ~97% ported, 2% partial, 1% missing** (excluding N/A).  Recomputed from the actual per-row symbols in the tables below.  The two 🔶 are MD5mesh/IQM skeletal support and the frame-major alias pose SSBO layout.  The single ❌ is bindless textures, which moved out of ➖ N/A on 2026-08-18 after Ironwail was checked rather than assumed.

The headline figure moved by less than a point but is now honestly derived: the previous 84 ✅ included Underwater caustics, which Ironwail does not have in any form and which has been moved to Exclusives.  That subtraction is offset by promoting pointfile leak visualization to ✅ and adding a console argument tab-completion row.

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
| Decoupled renderer from server physics | ✅ | Fixed-timestep accumulator in `host.c` — physics at `sys_ticrate` (20 Hz), render uncapped |
| Triple-buffering / frames in flight | ✅ | `gl_buffer.c` ring with `FRAMES_IN_FLIGHT=3` + `glFenceSync` (uhexen2-8pc2, commit `32bdbea5`). `GL_AcquireFrameResources`/`GL_ReleaseFrameResources` wired into `GL_BeginRendering`/`GL_EndRendering`. All per-frame uploads stream through the ring. |
| Persistent mapped buffers | ✅ | `gl_buffer.c` opens `ARB_buffer_storage` with `GL_MAP_PERSISTENT_BIT \| GL_MAP_COHERENT_BIT` when available (uhexen2-8pc2). Used by alias instances (main + fullbright passes), GPU world-cull PVS bitvector (uhexen2-o35n), and the immediate-mode emulator (uhexen2-y1v5: `GL_ImmEnd`/`GL_ImmDraw` route through `GL_Upload` + `glBindVertexBuffer` via ARB_vertex_attrib_binding). |
| Hi-Z occlusion culling | ✅ | Previous-frame depth pyramid + per-AABB rejection inside `cull_mark` compute (uhexen2-xd87, commits `d58198a1`/`2f8376297`).  Decoupled from the postprocess pipeline 2026-05-12 (uhexen2-9912, `bddc22128`) — standalone depth resolve in `gl_worldcull.c` works whether or not FXAA/HDR/etc. are on.  `gl_hiz_cull` flipped to default **1** after the acceptance sweep (uhexen2-8pzr) measured 44-58% cull rate on demo1 vistas, well above the ≥10% gate. |
| Bindless textures | ❌ | **Reclassified ➖ N/A → ❌ Missing on 2026-08-18.**  Every prior revision of this row asserted "Ironwail does not use `ARB_bindless_texture` either".  That is false.  Ironwail has a live, working bindless path: `gl_bindless_able` is set in `gl_vidsdl.c` behind `GL_FindExtension("GL_ARB_bindless_texture") && GL_InitFunctions(...)`, textures carry `TEXPREF_BINDLESS`, `gl_texmgr.c` calls `GL_GetTextureSamplerHandleARB` / `MakeTextureHandleResident`, the shaders compile with `#define BINDLESS %d` and real `#if BINDLESS` branches, and `r_world.c` uses it on the world draw path — which is exactly the batched use case.  So this is a genuine parity gap, not a Quake-specific irrelevance.  Our own scaffolding was still correctly **removed** in `8124ed055` (uhexen2-ubsu): its ARB entry points were declared through `GL_FUNCTION_OPT`, which `GL_Init_Functions` expands to nothing, so `gl_bindless_able` could never be true and every downstream defect sat behind a gate wired shut.  Removing dead code was right; calling the feature N/A was not.  uhexen2-im9g tracks the re-port and now carries the corrected premise. |
| Reversed-Z depth buffer | ✅ | `ARB_clip_control` — `gl_vidsdl.c` detects `glClipControl` by entry-point load, switches clip space to `[0,1]`; `GL_Frustum` (`GL_Frustum` in `gl_matrix.c`), R_Clear/mirror split, viewmodel near-clip, sky pin all flipped to `GEQUAL` / far=0, near=1 |
| SIMD mipmap generation | ✅ | `GL_MipMap_W` / `GL_MipMap_H` split with `__SSE2__` fast-paths (`_mm_avg_epu8`) in `gl_draw.c`. Combined downsample now does W-pass + H-pass with Ironwail's `(a+b+1)>>1` rounding. Scalar fallback retained for non-x86 builds. |
| MD5mesh / IQM skeletal model support | 🔶 | **~50%, loads safely but draws nothing (uhexen2-7ok0).**  The parser is sound: all five crash/corruption defects the earlier revision of this row listed were fixed 2026-06-12 by uhexen2-7ok0.1 — `md5mesh.c` parses into heap buffers (not 96KB+128KB stack arrays), stores hunk *offsets* rather than stack addresses, allocates and fills the identity bone-pose block, computes rest-pose world XYZ, and renormalizes the top-4 bone weights to sum exactly 255.  `.md5mesh` files are hooked into the loader at `Mod_LoadModel` in `gl_model.c`.  Attribute plumbing landed since: integer bone-index path (`4928f988f`, uhexen2-6wai), skeletal shader attribute locations matched to the IQM VAO (`f26c55e6e`, uhexen2-bynk), legacy draw paths guarded so they stop walking a header as a command stream (`aa01a127d`, uhexen2-zjux).  What is missing is the other half: **no `.md5anim` parser** (`numposes` hardcoded to 1) and **no draw dispatch** — `gl_shader_skeletal` has an `InitProgram` call and zero draw sites, so a loaded MD5 model renders nothing.  Still no `.iqm` parser and no `iqm.h`; `iqmvert_t` is just an internal vertex struct name.  Tracked as uhexen2-7ok0.2 (animation) and uhexen2-7ok0.3 (dispatch). |

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
| Per-entity leaf cap (MAX_ENT_LEAFS) | ✅ | **Now beyond Ironwail.** History is 16 (vanilla) → 32 (ericw / QuakeSpasm / Ironwail, where it still sits) → **64** here, bumped in `32c35acdb` after Keep.bsp re-reported popping at 32 (uhexen2-l0ac). `SV_WriteEntitiesToClient` also skips the PVS cull when an entity hits the cap, so it is always sent — that always-send path stays as the safety net for the absurd cases. Long brush ents (lifts, rotators) that touched more than 16 BSP leaves had their leaf list truncated, and the PVS cull then dropped them from the client write stream as the player moved. Cost is 256 B per edict over the 32 baseline. |
| FPS cap with menu slider | ✅ | `host_maxfps` in Display menu |
| CSQC (client-side QuakeC) | ✅ | `cl_csqc.c` |
| bmodel buffer rebuilt correctly on map change | ✅ | Ironwail fix `3ccbcda` (2026-02): `GL_DeleteBModelBuffers()` was missing before `GL_BuildBModelVertexBuffer()` in `R_NewMap`, leaking GPU memory on map change.  Hexenwail has neither of those functions — they are Ironwail's names and never existed here.  The equivalent guard lives in `R_BuildWorldVBO` (`gl_rsurf.c`), which deletes `world_vbo` / `world_ibo` / `world_vao` and frees `world_index_data` before rebuilding.  Brush entities draw out of that same buffer via `R_DrawBrushInstanced`, so there is no separate bmodel buffer to leak.  (An earlier revision of this row pointed readers at `gl_rmisc.c:R_NewMap` to verify a function that does not exist.) |
| Alias model GPU data layout | 🔶 | Ironwail `a65a88e` (2026-01) reorganized alias-model GPU pose data to frame-major order (one SSBO bind per model, not per surface) and adds `Mod_NextSurface()`. Hexenwail allocates one SSBO per `aliashdr_t` (per-model header) which avoids the old per-surface alignment loop, but the layout is **not** frame-major: multi-surface models still trigger separate SSBO allocations per surface via `gl_mesh.c`, and `ssbo_pose_md3` is a separate buffer. Full port tracked as uhexen2-48fx (open, P3, blocked on uhexen2-ayrn). |
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
| Menu key auto-repeat (navigational only) | ✅ | Ironwail commit `6a9610f` (2026-01): `M_Keydown` gains `repeat` bool arg; only arrow keys pass repeat. Hexenwail already has `M_Keydown (key, key_repeats[key] > 1)` with identical arrow-key-only filter — `menu.c` / `keys.c`. |
| Mods menu dirs-with-spaces | ✅ | Ironwail commit `51a911b` (2026-03): added quotes around dir name in `game` command. Hexenwail already uses `game \"%s\"` at `menu.c`. |
| FSAA mode selection in menu | ✅ | `vid_config_fsaa` cycle in Display submenu walks `msaa_steps[]` (0/2/4/8/16/32) clamped to `GL_MAX_SAMPLES` queried at GL init. Labels render as "Off" / "Nx" instead of bare integers. |
| HUD / statusbar scaling | ✅ | `scr_sbarscale` — `CANVAS_SBAR` in `gl_draw.c`, slider in Misc/HUD submenu (`menu.c`) |
| Menu scaling | ✅ | `scr_menuscale` — `CANVAS_MENU`, M_CenterOfs() helper, M_ScreenYToCanvasY for mouse hit-test |
| Crosshair scaling | ✅ | `scr_crosshairscale` — `CANVAS_CROSSHAIR`, slider in Misc/HUD submenu |
| Debug-text scaling | ✅ | `scr_infoscale` (Ironwail `cc03300c7`, uhexen2-r9qj) — `CANVAS_INFO` in `gl_draw.c`, sized by `SCR_InfoCanvasSize()`. Carries the showfps / showclock / showspeed readouts so they can be legible at 4K without the HUD growing with them. No menu entry: debug-only, as in Ironwail. |
| Console alpha | ✅ | `scr_conalpha` — caps `Draw_ConsoleBackground` alpha, slider in Misc/HUD submenu |
| Console brightness | ✅ | `scr_conbrightness` — multiplies conback RGB, slider in Misc/HUD submenu |
| Menu background style | ✅ | `scr_menubgstyle` (default 1) — 0=off / 1=simple dim (Draw_FadeScreen) / 2=dim+translucent backdrop quad over the menu-item area in CANVAS_MENU. Display submenu cycles Off/Simple/Menu Box. Replaces legacy `scr_menufade`. |
| Center-print background | ✅ | `scr_centerprintbg` (`gl_screen.c`) with menu cycle Off / Simple / Menu Box (`menu.c`). Default 2 (Ironwail parity, `df5219c`). Mode 1 = full-width thin dim strip (alpha 0.30), mode 2 = text-width box (alpha 0.50). |
| Console mouse support | ✅ | **Phase 1 complete** (2026-05-12: commit e091812e7): text selection via drag with visual blue highlight, clipboard copy (Ctrl+C with fallback to abort), select-all (Ctrl+A), cursor shape feedback (I-beam/pointer). **Phase 2 complete** (2026-05-13: commit 77bb5727a): URL detection (http/https/ftp), blue underlines with alpha feedback on hover, hand cursor (MCURSOR_HAND) over URLs, SDL_OpenURL on click. |
| Console notification fade | ✅ | `con_notifyfade` (default 1) — alpha ramps 1→0 over the last 1 s of `con_notifytime`. Per-quad alpha threaded through `Draw_AddCharQuad` via new `Draw_SetCharacterAlpha` setter (gl_draw.c). |
| Console max columns | ✅ | `con_maxcols` (default 0 = no cap), menu slider in Misc/HUD submenu (`menu.c`, commit `ab108d760`) |
| Menu search with filtering | ✅ | Shared `M_Filter_*` facility — type printable chars in Display/Rendering/Graphics/Game submenus to live-filter rows by case-insensitive substring; backspace edits, ESC clears (then exits on second press), up/down skip filtered rows, cursor snaps to first match on filter change. Sound (4 rows) and Gamepad (controller-driven) intentionally not wired (uhexen2-rawq). |
| Menu live preview | ✅ | Restored 2026-05-13 (commit d29f8c04a) — backdrop briefly dims for 0.8s when presets are changed in Display menu, showing the effect without menu text flashing. |
| Show speed / show time overlays | ✅ | `scr_showspeed`, `showclock` (4-state: off / game-time / wall HH:MM / wall HH:MM:SS); both toggleable from Misc/HUD submenu |
| Map-editor auto-check on launch | ➖ | Ironwail commit `5a983620` (2026-05): `Sys_IsStartedFromMapEditor` detects Qrucible parent process, triggers map check. Hexenwail has no equivalent function and no TrenchBroom workflow integration. Could be ported but low priority for Hexen II mapping scene. |
| Mod-supplied bindlist.lst rows in Key Setup | ✅ | Ironwail `096c3d952`. A mod's `bindlist.lst` merges mod-specific rows into the Key Setup menu alongside the engine's own binds. `M_BuildBindList()` in `menu.c` replaces the old compile-time `bindnames[][2]`/`NUMCOMMANDS` pair with a runtime list (`keys_bindlist[]`/`keys_numcommands`); rebuilt on `M_Init`, every `M_Menu_Keys_f`, and `Host_Game_f` (mod switch while the menu is open). No dynamic allocation — mod rows point into a static 64-entry pool. (commit `d4b01fd39`, uhexen2-7aok closed) |
| Pointfile leak visualization | ✅ | Ironwail `26902e0e2`. Map-leak `.pts` files draw as animated direction arrows (`R_ShowPointFile` / `R_EmitPointFileArrow` in `gl_rmain.c`, depth-test toggle `r_pointfile_depthtest`) instead of one particle per point — ported via the existing `R_ShowBoundingBoxes`-style immediate-draw batching (commit `3993cff2c`, uhexen2-nwx1).  The three companion pieces from the same Ironwail commit landed 2026-08-14 in `c293ae22a` (uhexen2-m183): auto-load via a `pointfile leak` argument when `cl.worldmodel->visdata` is NULL, the console leak warning, and the on-screen "Leak" label projected through `GL_GetMVP`. |
| Console argument tab-completion | ✅ | Ironwail's `arg_completion_type_t` table in `console.c` completes command *arguments*, not just command names.  Hexenwail completes map, mod and demo names (`afcc75e68`) plus `exec` / `save` / `load` / `sky` (`52aa364ec`).  Driven and regression-tested against a live console by the harness in `tools/` (`8746fe9bf`). |

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
| Underwater audio filter | ✅ | `snd_waterfx` — IIR low-pass on the paint buffer. Note: Ironwail also has `snd_waterfx` (`snd_dma.c`) — this is NOT a Hexenwail exclusive. Both implementations are independently convergent. |
| Sound filter quality | ➖ | `snd_filterquality` cleans up Ironwail's paint-time zero-stuff upsample. Our pipeline resamples at load (`ResampleSfx` in `snd_mem.c`), so the filter has no equivalent precondition here. |

## Network / Protocol

| Feature | Status | Notes |
|---|---|---|
| FitzQuake protocol extensions | ✅ | Fog, skybox, alpha — adapted to Hexen II svc numbering |
| RMQ protocol flags | ➖ | `PRFL_FLOATCOORD` etc. — Quake-specific |
| Quake 2021 re-release messages | ➖ | `svc_achievement` etc. — Quake-specific |

## Steam / Platform

| Feature | Status | Notes |
|---|---|---|
| Steam integration | ➖ | **Not** because Hexen II is absent from Steam — it is sold there, and that claim (carried by this row until 2026-08-18) was simply false.  The reason is that nothing in this engine needs Steam to be involved: the Steam build ships the same retail data files the engine already finds through its normal search paths.  Ironwail's Steam code exists to locate the Quake 2021 re-release and the rerelease-only content that comes with it (`QuakeEX.kpf`, localization, the mg3 mission pack).  Hexen II has no re-release, so there is no second install layout to discover and no Steam-specific code path to port. |
| Steam Quake 2021 auto-detect | ➖ | Quake-specific — there is no Hexen II re-release for an equivalent probe to find. |

---

## Bug Fixes from Ironwail (applicable to Hexenwail)

Recent Ironwail bug fixes assessed for Hexenwail applicability:

| Ironwail commit | Fix | Hexenwail status |
|---|---|---|
| `3ccbcda` 2026-02 | bmodel VBO leak on map change: `GL_DeleteBModelBuffers()` missing before rebuild in `R_NewMap` | ✅ Equivalent guard present — `R_BuildWorldVBO` (`gl_rsurf.c`) deletes the world VBO/IBO/VAO before rebuilding.  Ironwail's two function names do not exist in this tree. |
| `6a9610f` 2026-01 | Menu key auto-repeat: only navigational keys pass repeat events | ✅ Already ported |
| `51a911b` 2026-03 | Mods menu: game command not quoted, breaks dirs with spaces | ✅ Already quoted |
| `0a6084a` 2026-04 | Pitch drift during cutscenes: `V_StopPitchDrift()` not called when `CL_InCutscene()` | ✅ Ported 2026-05-05: `cl_input.c:CL_AdjustAngles` early-returns with `V_StopPitchDrift()` when `cl.intermission` is set. |
| `017fdd2` 2026-01 | Dark outlines on fence textures with dynamic lights: compute plane before `discard` | ✅ Ported 2026-06-12 (uhexen2-9a1l): the same hazard hits us through `texture()`'s implicit dFdx/dFdy, not a hand-written plane equation. In `sworld_frag` the fullbright mask sample was the only post-`discard` texture fetch and was producing undefined mip selection for surviving lanes in a 2×2 quad where peers discarded — manifesting as dark fence-edge outlines. Sample now happens above the alpha-test `discard`. Other discard sites audited clean. |
| `1011ff8` 2026-01 | Disable GL texture compression for alpha-tested surfaces | ➖ N/A for engine-generated textures — Hexenwail has no `gl_compress_textures` system (still open as uhexen2-8dks).  **Caveat added 2026-08-18:** `74e8eccde` gave HD replacement packs compressed DDS/KTX upload, so a pack shipping BC1 fence or HUD textures now hits exactly the block-artifact fringing these two commits guard against.  Revisit with uhexen2-r7zu. |
| `74d8e74` 2026-01 | Disable GL texture compression for 2D textures (HUD, conchars) | ➖ N/A — same reason, same caveat as above. |
| `80387f1` 2026-01 | Crash toggling `gl_compress_textures`: cubemap textures stored pointers to stack data | ➖ N/A — no compression system. |
| `78ad272` 2026-01 | Stop controller rumble when sound buffer is cleared (e.g. modal message) | ✅ Already present — `S_ClearBuffer` calls `IN_GPRumble(0, 0, 0)` at `snd_dma.c` (uhexen2-xq1c closed). |
| ericw (pre-Ironwail) | MAX_ENT_LEAFS 16→32 + always-send on cap overflow in `SV_WriteEntitiesToClient` | ✅ Ported 2026-05-11 (uhexen2-l0ac follow-up), then **exceeded** — we sit at 64, Ironwail at 32, after Keep.bsp still popped at 32 (`32c35acdb`). |
| `7a2038a` 2026-01 | Menu support for binding the gamepad alt-modifier layer | ✅ Ported 2026-06-11 (uhexen2-qeyd): `M_Keybind` redirects a base GP press to its `_ALT` variant while the modifier is held (and ignores a press of the modifier key itself); `M_Keys_Draw` previews with `Alt-???` / an ALT-layer prompt. Also fixed the menu's `<256` find/unbind scan that hid all `K_GP_*_ALT` and `K_GP_DPAD_*` bindings. |
| `39ba13eda` 2026-06 | BSP entity lump not NUL-terminated on disk; worked only by accident via Hunk_Alloc zero-fill | ✅ Ported 2026-08-13 (`df9845ca7`, uhexen2-sg5s): `Mod_LoadEntities` allocates `l->filelen + 1` and writes the terminator explicitly, in **both** `gl_model.c` and `sv_model.c`. |
| `b6bbafcf3` 2026-08 | Controller LED left lit after the engine exits | ✅ Ported 2026-08-13 (`c88650738`, uhexen2-lssr). |
| `15ab44f36` 2026-08 | Frametime hitch handling for the live options preview | ✅ Ported 2026-08-13 (uhexen2-pxsz). |
| `c03a5bdc1` 2026-08 | Console mouse selection not clamped past end-of-line | ✅ Verified/ported 2026-08-13 (uhexen2-z8je). |

---

## Bead Coverage

As of 2026-08-18, **two** features in the tables above are non-complete: MD5mesh/IQM 🔶 (~50%, parser sound but no animation and no draw dispatch — uhexen2-7ok0) and Alias model GPU data layout 🔶 (per-`aliashdr_t` but not Ironwail frame-major — uhexen2-48fx).  Pointfile leak visualization completed 2026-08-14 (uhexen2-m183, `c293ae22a`).  Bindless textures are ❌ Missing (uhexen2-im9g).  They were reclassified out of ➖ N/A on 2026-08-18 once Ironwail was actually checked: it has a working bindless path and this is a real gap.  The scaffolding removal in `8124ed055` (uhexen2-ubsu) stands — that code could never run.  MSAA with FBO resolve is ➖ — Hexenwail intentionally uses FXAA, vestigial window MSAA dropped in `5b6d57ec4`.  Scorecard: 85 ✅ / 2 🔶 / 0 ❌ / 10 ➖ (~98% parity, excluding N/A).

**Three open Ironwail ports have no row in the tables above**, because they are upstream work never started rather than partial features: uhexen2-r7zu (`TEXPREF_UNCOMPRESSED` for alpha-tested and 2D textures, blocked on uhexen2-8dks), uhexen2-ayrn (light-trace cache for alias models), uhexen2-yae (menu system improvements).  They are counted in the umbrella epic uhexen2-a5nn, not in the parity percentage.  Run `bd show uhexen2-a5nn` for the current child list.

**Ironwail porting beads are filed at P1 or above, with no exemptions** (standing rule, 2026-08-18, reaffirmed the same day).  If a bead touches Ironwail parity in any way it is P1: every gap in the tables above, the umbrella epic, blockers that would otherwise strand a P1 port (uhexen2-8dks moved for that reason), and beads that merely reference Ironwail as prior art (uhexen2-9o7u).  Two exemptions were proposed on the first pass and both were withdrawn — one of them, uhexen2-im9g, rested on the claim that Ironwail does not use `ARB_bindless_texture`, which the correctness pass showed to be false.  Deciding what counts as "really" a port was itself the failure mode, so the rule no longer asks.

Engineering caveats belong in a bead's notes, not in its priority: uhexen2-9o7u is P1 *and* carries the warning that the obvious per-pixel turb port was already tried and reverted after uhexen2-tlsh found violent aliasing at glancing angles.

**The rule is about Ironwail itself.**  It does not reach other upstreams.  Three jsHexen2 rendering beads (uhexen2-7oim planar water reflections, uhexen2-mfql bump/normal mapping, uhexen2-s4xh chrome reflections) were separately raised to P1 on 2026-08-18 by an explicit one-off call — that was a decision about those three beads, not a widening of this rule.  The jsHexen2 gamecode-vendoring workstream (uhexen2-2kcw and its gated children) stays where it is.  When checking whether the floor is satisfied, match on Ironwail; a non-Ironwail bead that looks worth promoting is a separate question to raise, not an application of this rule.

The rule exists because parity work parked at P3/P4 never surfaced in `bd ready`, so this tracker kept reporting ~97% parity while the remainder went untouched for months.

When porting a parity item, claim the bead with `bd update <id> --status=in_progress`, implement, update the matching row here to ✅, and close the bead with a reference to the landing commit.

---

## Priority Shortlist (highest impact, applicable to Hexen II)

### P1 — High (regressions surfaced 2026-05-15)
1. **Bindless textures** — ❌ genuine parity gap (uhexen2-im9g).  The removal in `8124ed055` was correct — the scaffolding could never execute — but the conclusion drawn from it was not: Ironwail *does* use `ARB_bindless_texture`, on the world draw path, gated on a real extension probe.  Re-porting still needs the two preconditions the bead records (a measured need, and hardware that can test it — llvmpipe has no `ARB_bindless_texture`, so neither this box nor CI can exercise the path).  Ironwail's implementation is the reference.

### P3 — Low
1. **MD5/IQM skeletal models** — 🔶 ~50% (uhexen2-7ok0).  The parser and attribute plumbing are done and safe; the crash list this entry used to carry was cleared 2026-06-12.  Remaining: write the `.md5anim` parser (uhexen2-7ok0.2), then add the `PV_IQM` branch to `R_DrawAliasModel` and the draw-site dispatch that finally invokes `gl_shader_skeletal` (uhexen2-7ok0.3).  Until 7ok0.3 lands, a loaded MD5 model is silently invisible.

*Async main-thread task queue verified complete 2026-05-13 (uhexen2-9v0s closed — `host_async.c`).*
*MD3 model support completed 2026-05-12 (uhexen2-f2d3, uhexen2-kaa6).*
*Console mouse support (Phase 1+2) completed 2026-05-12–2026-05-13 (uhexen2-8vw0, uhexen2-ei9r, commits e091812e7, 77bb5727a).*
*Faster map loading completed 2026-05-13 (uhexen2-3mbt).*

---

## Hexenwail Exclusives (not in Ironwail)

Features Hexenwail has that Ironwail does NOT. Verified against Ironwail `origin/master` (`4fa8bfbd`) as of 2026-08-18.

| Feature | Notes | Confirmed absent in Ironwail |
|---|---|---|
| HDR tone mapping (ACES) | `r_hdr` with exposure control | Yes — no tonemapping pipeline |
| Motion blur | `r_motionblur` with view delta tracking | Yes |
| FXAA | `gl_fxaa` toggle | Yes — Ironwail has no FXAA |
| Alpha-to-coverage cutout antialiasing | `r_alphatocoverage` — MSAA-based fence edge smoothing | Yes — no `GL_SAMPLE_ALPHA_TO_COVERAGE` usage |
| WebGL2 / WASM build | Emscripten + ES3 fallback, ~1.70 MB binary (grew from 1.4 MB when the libTiMidity MIDI fallback tier was added for WASM, uhexen2-unvi — audibility still needs a soundfont delivered to the virtual FS, tracked in uhexen2-g3j9) | Yes |
| Hexen II class system | 5 player classes with unique HUD/inventory | Yes — Quake-only engine |
| Per-mod music subdirs | `<gamedir>/music/<author>/<file>.<ext>` lookup so multiple authors can ship tracks without colliding | Yes |
| Track-name remap | `bgm_remap NN <name>` console command — points a numeric CD track at a named music file | Yes |
| Graphics presets | Crunchy/Retro/Faithful/Clean/Modern/Ultra | Yes |
| FluidSynth MIDI | Native MIDI playback | Yes |
| Bicubic lightmap filter | `r_lightmap_bicubic` — GPU bicubic upsampling of the lightmap atlas in the world fragment shader (commit `f52826282`, uhexen2-b2f0) | Yes |
| Underwater caustics | `r_caustics` (default 1), `r_caustics_intensity` — animated caustic texture projected onto world surfaces below the waterline, registered in `gl_rmisc.c`, sampled in `sworld_frag` (commit `7b82afce6`, uhexen2-6bfm).  Listed as a ported parity feature until 2026-08-18; it never was one. | Yes — `git grep -i caustic origin/master` returns nothing |
| Bloom | `r_bloom` (`gl_postprocess.c`), FBO chain rebuilt on toggle alongside `r_hdr` (`557f2358c`, uhexen2-3p6p) | Yes |
| Soft particles | `r_softparticles` — depth-aware fade of sprites into the geometry behind them, fade distance defaults to 8 (`7dc787ad1` / `ae31a7bf5`, uhexen2-mf9u) | Yes |
| Weather volumes (rain / snow) | `r_raincollide`, `r_rainintensity`, `r_snowintensity` in `r_part.c`.  Drops render as motion streaks along the fall vector, collide with world geometry, land on floors rather than dying at the brush bottom, and take a mapper-set fall speed without a protocol change.  Driven by Mathuzzz's mapper requirements and the uhexen2-2rxl field report. | Yes — Hexen II *Scourge of Armagon*-era feature with no Quake analogue |
| HD replacement packs: compressed containers | `74e8eccde` (uhexen2-0vgo) added three things; **only the third is exclusive.**  DDS and KTX containers are parsed by `img_dds.c` (DXT1/3/5, ATI1/ATI2, DX10-header BC1–BC5/BC7, KTX 1.1) and uploaded compressed instead of being inflated to 32-bit RGBA — Ironwail has no compressed-container path at all.  The other two are **parity, not exclusives**: per-map override directories and `_glow` / `_luma` sidecars both already exist in Ironwail (`gl_model.c` searches `textures/<mapname>/` before `textures/`, and loads `<name>_glow` / `<name>_luma`).  An earlier revision of this row claimed all three as exclusive; corrected 2026-08-18. | Partly — DDS/KTX yes, per-map dirs and glow sidecars no |
| Water ripple shader | `gl_waterripple` (default 2) — vertex ripple on liquid surfaces. Distinct from `r_waterwarp`, the underwater screen distortion, which both engines have and which has its own row above. | Yes — `gl_waterripple` is absent from Ironwail; restored to this table 2026-08-18 after being wrongly demoted on a bad citation |
| Hexen II glow sprites | `gl_glows` / `gl_other_glows` / `gl_missile_glows`, gated on the Hexen II effect flags `XF_TORCH_GLOW`, `XF_MISSILE_GLOW`, `XF_GLOW` and `EF_GLOW` (`gl_rmain.c`, `cl_main.c`, `cl_effect.c`) | Yes — absent from Ironwail. Ironwail's only "glow" is the `_glow`/`_luma` fullbright *texture sidecar*, a different mechanism; restored 2026-08-18 |

**Removed from exclusives (present in both):**

| Feature | Notes |
|---|---|
| Underwater audio filter | Both have `snd_waterfx` (Ironwail `snd_dma.c`). Independently implemented but same cvar name and concept. |
| Zoom system | Both have it. Hexenwail: `zoom_fov` / `zoom_speed` plus `+zoom` / `-zoom` / `togglezoom` binds in `gl_screen.c`; `cl.zoom` is lerped by `SCR_UpdateZoom` and smoothstepped into the FOV. Ironwail: `scr_zoomfov` / `scr_zoomspeed` under the same cvar names, `gl_screen.c`. Landed in `ecba5a981`; uhexen2-mfbe closed. |
| Gun FOV scale | `cl_gun_fovscale` exists in Ironwail (`gl_screen.c`, marked "Qrack" there). Shared feature, ported from Ironwail. |
| External texture overrides | Capability is in both, the *toggle* is ours. Ironwail loads external replacements unconditionally in `gl_model.c` and already searches `textures/<mapname>/` before `textures/`, and already loads `_glow` / `_luma` sidecars. Hexenwail adds the `r_texture_external` cvar to turn that off. Not an exclusive. |

**Correction, 2026-08-18.** Three rows above this line were wrong and have been moved back to Exclusives: `gl_waterripple`, and the `gl_glows` / `gl_other_glows` / `gl_missile_glows` trio. None of those cvars exist in Ironwail in any form. The rows cited Ironwail line numbers (`gl_rmain.c:127-129`, `:133`, `:134`) that resolve to `map_wateralpha`, `r_scale` and a blank line — the citations came from grepping the wrong tree. `r_texture_external` stays here, but with a corrected reason: Ironwail has the capability without the cvar.
