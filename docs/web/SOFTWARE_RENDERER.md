# The web software renderer

`WEB_RENDERER=software` builds the browser client around uHexen2's 8bpp
software renderer, the descendant of the one Hexen II shipped with, instead of
Hexenwail's GL renderer. This document is for whoever next has to change it:
what is restored and what is new, how a frame gets from the rasterizer to the
screen, and which gates keep it from rotting.

How it sits next to the default WebGL2 build is in
[`ARCHITECTURE.md`](ARCHITECTURE.md); the launcher, storage and offline shell
are in [`../PWA.md`](../PWA.md).

## Why it exists

Live archival. `Hexenwail: SDL3/GL4.3 modernization (2025-2026)` (036a0b531 at
time of writing) deleted the rasterizer from this tree: the `r_*` and `d_*` files
went with it, and the 8bpp model layer (`model.{c,h}`) stayed on, unbuilt, until
it was removed as orphaned on 2026-08-13, six days before the restore. For months
the renderer existed only in git history. `renderer: restore the classic 8bpp software rasterizer as
a web build option` (154a27700 at time of writing) brought it back, so it
compiles and runs again, in a browser, on a phone.

It is the opt-in configuration, not the shipping one. The restore was ported from
alextnewman/hexenwail's `Web port: default to the classic software renderer on an
accelerated canvas (#8)` (0b1906add, a commit of that fork, not reachable from
this repository's refs), which made software the default and replaced the
platform layer; neither choice was taken here. Hexenwail's identity is
Ironwail-parity GPU rendering, so `webgl2` stays the default, and the software
build changes only the renderer.

## Restored files, and the rule for touching them

The rasterizer was restored from the tree just before the modernization,
`036a0b531~1`:

- scene traversal: `engine/h2shared/r_{aclip,bsp,draw,edge,efrag,light,sky,sprite,surf,vars}.c`
- rasterization: `engine/h2shared/d_*.c`
- client side: `engine/hexen2/r_{main,alias,misc}.c`
- the 8bpp model layer: `engine/h2shared/model.{c,h}`
- headers: `r_local.h`, `r_shared.h`, `d_local.h`, `d_iface.h`

At the restore commit all but three came back byte for byte: `model.c`,
`model.h` and `r_main.c` gained what the restored renderer needed to take part in
today's engine (the PimpModel hooks, the `EF_*` effect flags, the MD3
identifier). Those three have kept changing with the engine since. Among the
changes: the dlight mask and `MAX_DLIGHTS` (`model.h`, `r_main.c`), the
brush-state classification (`r_main.c`), the `r_texture_external` rename
(`model.c`, `r_main.c`), multiplayer transparency (#58, `r_main.c`), the MD3
loader and the missing-precache placeholder (`model.{c,h}`), and the #104 cvar
flags (`r_main.c`). `git log` on each file has the full list. Engine-wide work has also reached six that came back byte for
byte:
- `r_alias.c`: misc_modelpimp effect flags (#70);
- `r_draw.c`: the unified brush-state classification, and the poly count moved
  after the zero-alpha visibility check;
- `r_efrag.c`: devstats;
- `r_local.h`: the `r_texture_external` rename, the dlight changes and the
  brush-state classification;
- `r_light.c` and `r_surf.c`: the dlight mask, widened to 128 bits, and the larger
  `MAX_DLIGHTS`.

So nine of the restored files differ from the original today; do not assume any
one is pristine. To see exactly what a file carries beyond the original:

```bash
git diff --stat 036a0b531~1 HEAD -- engine/h2shared/model.c engine/hexen2/r_main.c   # etc.
```

The rule is the one in `engine/CMakeLists.txt` above `SOFT_RENDERER_SOURCES`:
change these files only for behaviour, keep the diff minimal, and never reformat
them, so that diff keeps showing deliberate changes and nothing else.

**The model layer is the historical one on purpose.** `engine/hexen2/quakeinc.h`
includes `model.h` and `d_iface.h` under `WEBSOFT` instead of `gl_model.h`: the
surface cache, sprite pixel data and skin descriptors the rasterizer needs were
dropped from the GL model layer long ago. `model.h` also deliberately does not
redefine `MAX_SKIN_HEIGHT` or `MAXALIASVERTS`, as the fork does: `d_iface.h`'s
480 is the limit the restored skin cache was written against, and the fork's 2048
would silently have raised it.

## The four adapters

Apart from `mod_placeholder.c`, which the GL build compiles too, everything new
is in four files. They exist to keep renderer `#ifdef`s out of
shared client code (`console.c`, `menu.c`, `sbar.c`, `cl_*.c`, `pr_csqc.c`),
which was written against the GL renderer's symbols: these supply software
answers to the same names. What `#ifdef`s remain are a handful of `WEBSOFT` arms,
mostly in `menu.c` (the renderer's name, and the rows it has to hide) and
`screen.c`.

| File | What it supplies |
|---|---|
| `engine/hexen2/r_soft_web.c` | the renderer-policy cvars shared code reads, the per-entity PimpModel override table, and stubs for GL-only hooks |
| `engine/h2shared/draw_soft_web.c` | the extended (Ironwail-parity) 2D API on the 8bpp framebuffer |
| `engine/h2shared/vid_soft_web.c` | the SDL3 window, the framebuffer, the resolution ladder, aspect policy and pointer mapping |
| `engine/h2shared/web_canvas_gl2.c` | the presenter: the one piece that talks to the GPU |

Their declarations are in `engine/h2shared/soft_web.h` and
`engine/h2shared/web_canvas.h`.

### r_soft_web.c

- **Policy cvars.** `gl_missile_glows`, `gl_torch_dlight`, `gl_flashintensity`,
  `gl_extra_dynamic_lights` and friends mean the same thing here as in the GL
  build (`gl_rmain.c`), and the client honours them. The GPU-only ones (`gl_fxaa`,
  `r_hdr`, `r_softemu`, `r_softemu_mdl_warp`, `scr_demobar_timeout`, ...) exist so
  shared code links, and the other four `r_softemu_*` sub-cvars only so a GL
  config round-trips; the menu
  rows that drive them are hidden under `WEBSOFT`. `R_SoftWebInitCvars` registers them from
  `VID_Init`, before config execution, which the fork declared but never called.
- **PimpModel overrides.** The `pimp_override_t` table `model.h` declares
  renderer-owned. It must behave exactly like `gl_rmain.c`'s copy, because QuakeC
  drives it through `pr_cmds.c`/`pr_edict.c`.
- **GL-only hooks, answered honestly.** `Fog_ParseServerMessage` reads and
  discards the `svc_fog` payload (byte density, three colour bytes, a short fade
  time), because the rest of the message would otherwise be parsed at the wrong
  offset. `Sky_LoadSkyBox` accepts and ignores a skybox: the rasterizer draws the
  classic two-layer scrolling sky (`r_sky.c`, `d_sky.c`). `Mod_ReuploadAliasSkins`
  does nothing because software skins are never uploaded anywhere. The water-warp
  preview calls are no-ops: `d_scan.c` warps during rasterization.

### One config, two builds: cvar defaults

Both web builds can read and write one `config.cfg`: same origin, same
`/persistent` tree. At startup each reads it back. When saving,
`Host_WriteConfiguration` rebuilds the file from `Cvar_WriteVariables`, which
writes every archived cvar **the running build registers**, and nothing else.
That has two consequences.

**A cvar both builds register with different defaults: the first writer
decides.** A value already in `config.cfg` round-trips through either build. But
a cvar not yet in the file starts at the running build's default and is written
out with it the next time that build saves, so whichever build writes the config
first sets it for both. Until
#104, a config first written by the software build carried:
- `r_wateralpha "1"` and `r_dither "0"` (`r_soft_web.c`), turning the GL build's
  water opaque and its dithering off;
- `scr_sbarscale`/`scr_menuscale`/`scr_crosshairscale "1"` (`draw_soft_web.c`),
  which pinned its auto-scaled HUD, menu and crosshair at 1x, and `scr_conalpha
  "0.8"`, which made its opaque console translucent.

The same goes for the archive flag. `r_dynamic`, `r_clearcolor` and `showfps`
were archived by the GL build and not by this one, so every software session
dropped the player's GL values. In the other direction, `gl_extra_dynamic_lights`
was archived here and is not archived in the GL build. It now matches GL, so its
value no longer persists in either build.

`tools/test_soft_cvar_defaults.py` fails on any of these. The one listed
exception is `r_waterwarp`: one Water Warp row, implemented twice, defaulting on
in the restored `r_main.c` (as shipped) and off in the GL build. By the rule
above, the software build's "on" takes effect only in a config the GL build has
never written. That makes it a policy question, not a fix.

**An archived cvar only one build registers: erased by the other.** The other
build never writes it, so whenever a session there saves `config.cfg` -- in
practice every session, since loading the config marks it changed and
`Host_WriteConfiguration` then flushes it -- the cvar is dropped. The
gate counts these without failing (`python3 tools/test_soft_cvar_defaults.py -v`
lists them), and they are open as #362:
- 92 that only the webgl2 build archives: `zoom_fov`, `r_viewmodel_fov`,
  `gl_texturemode`, `scr_conscale`, `showclock`, `scr_pixelaspect`, the `vid_*`
  window settings, most of the GL renderer's options;
- 4 that only this build archives: `vid_soft_mode`, `vid_soft_filter`,
  `vid_soft_stretch`, `r_transwater`.

Registering inert stand-ins would fix it. `r_soft_web.c` already does that for
four cvars: the `r_softemu_*` sub-cvars other than `r_softemu_mdl_warp` (which
`menu.c` needs for linkage) are registered only so a GL config round-trips. The uhexen2-a5nn.38 rule recorded in
`VID_SetSoftMode` says the opposite: an inert archived cvar is a bug, which is why
`scr_pixelaspect` is not registered here. #362 is where that gets decided. Until
then, do not serve both builds to players from one origin.

### draw_soft_web.c

The 2D layer the rasterizer shipped with (`draw.c`) predates alpha pics, alpha
fills, glyph batching and multiple UI canvases. This file adds them on top of the
8bpp framebuffer the way the software renderer always did translucency: a 4x4
ordered stipple, plus colormap darkening.

- `GL_SetCanvas` is a translation of the drawing origin (`draw_canvas_x`/`_y`),
  not a projection. `CANVAS_SBAR` is 320 wide at the bottom of the screen,
  `CANVAS_MENU` 320 wide at the top, both centred; everything else is the whole
  framebuffer. Code that switches canvas must leave the one it found, or later
  draws land in the wrong place.
- `SCR_CalcUIScale` is always 1 and `SCR_GuiSize` is the framebuffer: the UI is
  drawn unscaled into an image the presenter then scales as a whole.
- `Draw_FillAlpha` and `Draw_SetCharacterAlpha` stipple partial alpha;
  `Draw_FlushCharBatch` is a no-op, since glyphs go straight to the framebuffer.

### vid_soft_web.c

**One SDL window, one context.** Where the fork wrote its own browser platform
layer (`in_web.c`, `sys_web.c`, `snd_web.c`, `net_web.c`: 553 lines), this build
keeps SDL3. `VID_CreateWindow` makes an SDL window with an OpenGL ES 3.0 context,
so `in_sdl.c`, `sys_sdl.c`, `snd_sdl.c` and the whole launcher are the ones every
other target uses. The presenter draws through that context; a canvas only ever
hands out one.

**The framebuffer** is one byte per pixel, owned by this file. `VID_AllocBuffers`
puts the z-buffer and the surface cache (`D_SurfaceCacheForRes`) on the high hunk,
as the classic backends did, so a resolution change frees them in one step.

**The resolution ladder** is ten 4:3 render resolutions from 320x240 to 1280x960,
inside the rasterizer's `MAXWIDTH`/`MAXHEIGHT` (1280x1024, `r_shared.h`). They are
render resolutions, not display modes: the canvas always runs at the panel's
full device resolution and the presenter scales the image to it.

- `vid_soft_mode 0` (the default) picks automatically, `1`..`10` pins a rung (a
  pinned rung larger than the canvas is scaled down), and `vid_softmode` with no
  argument lists them.
- **Auto** (`VID_AutoMode`) considers rungs of at most 1024x768 pixels
  (`AUTO_MAX_PIXELS`: the rasterizer is single-threaded on the main wasm thread,
  so pixel count is the frame budget, and that is what an M1-class core sustains
  at 60 Hz with room for the server, sound and QuakeC) that the canvas would scale
  up at least 2x (`AUTO_MIN_SCALE`). Of those it prefers an exact integer upscale,
  the only way to keep pixels square and crisp, and otherwise takes the largest.
  On a canvas too small for any rung to reach 2x it falls back to 320x240.
- 683x512 is on the ladder for the 12.9-inch M1 iPad Pro: x4 is its 2732x2048
  panel exactly. 1024x768 x2 fits both iPad Pro panels with a small border.
- `vid_soft_stretch 0` keeps 4:3 (`VID_DestRect`), with bars at the sides of a
  wide canvas and above and below on a tall one such as an upright phone; `1`
  stretches the image to fill.
- `vid.guipixelaspect` is always 1: the 8bpp target has no scaled canvases, so
  `scr_pixelaspect` is not registered here.

`VID_PointerToFramebuffer` maps a pointer back to a framebuffer pixel through
whatever rectangle `VID_DestRect` chose, so window points and framebuffer pixels
are only the same coordinates by coincidence.

### web_canvas_gl2.c, the presenter

Each frame (`VID_Update`), the presenter:

1. uploads the framebuffer as an `R8UI` texture, one byte per pixel, the smallest
   possible CPU-to-GPU transfer;
2. draws one fullscreen triangle generated from `gl_VertexID` (no vertex buffer)
   into the destination rectangle, after clearing the bars only when the image
   does not cover the canvas;
3. expands each index through a 256x1 `RGBA8` palette texture in the fragment
   shader.

Palette changes (`VID_SetPalette`, and `VID_ShiftPalette` for damage, bonus and
underwater tints) re-upload only the 256x1 lookup texture, so they cost nothing
per pixel.

`vid_soft_filter 0` is nearest neighbour, the classic look. `1` is pixel-art
antialiasing: a bilinear tap whose weights are sharpened by the on-screen size of
one source texel (`fwidth`), which collapses to exact nearest neighbour at an
integer scale and only softens the seams otherwise.

`web_canvas.h` is written as an interface so a second backend (WebGPU) could
replace this one without touching the engine. Exactly one is linked.

## Telling the builds apart

- the main menu's footer names the renderer `Software 8bpp` (`M_RendererName`,
  `menu.c`);
- `VID_Init` prints `Software renderer: <resolution> on <backend> canvas <w>x<h>`,
  e.g. `Software renderer: 640 x 480 on webgl2 canvas 1280x960`;
- the launcher's runtime log line says `renderer=software rasterizer` and reports
  the palette-blit self-test, and a failure says `WebGL2 palette presenter
  self-test failed`.

## Gates

A configuration nobody builds is a configuration that has been deleted with
extra steps. These keep this one alive:

- **It compiles, and boots, on every PR.** `.github/actions/wasm-build/action.yml`
  builds `software` into its own directory as a regression guard next to the
  shipping `webgl2` build, then runs `scripts/wasm-boot-smoke.sh` on both: the
  engine has to reach `FS_Init`'s data gate and stop there with its exact message,
  not trap.
- **It links.** Shared code names renderer-policy symbols directly, so a cvar the
  GL renderer gains and the software build lacks is a link error there.
- **Its cvars match the GL build's.** `tools/test_soft_cvar_defaults.py`
  (lint.yml): every cvar a software-only file defines, default and archive flag,
  against the GL definition; see [above](#one-config-two-builds-cvar-defaults).
- **Its menus are honest, where they say so.** `tools/menu_soft_parity.py`
  (lint.yml) catches what the linker cannot: a menu row whose cvar no software
  translation unit reads. It checks each menu group that has a `WEBSOFT` arm:
  every row must be either hidden by that arm or listed as `soft-ok`, and every
  `soft-ok` row must reach a live reader. A group with no `WEBSOFT` arm is not
  checked. The Display group is one: its Preset row still writes GPU-only cvars in
  this build (`uhexen2-ibnq.7` in [`../BACKLOG.md`](../BACKLOG.md)).
- **Model effects match.** `tools/test_software_model_flags.py` (lint.yml) pins
  `r_alias.c` to the GL renderer's handling of `EF_SPIN` and `EF_FLOAT`.
- **The launcher asks for what this build needs.** CMake writes the
  configuration to `hexenwail-renderer.txt`; `scripts/wasm-assemble-artifact.sh`
  stamps it into the site's `web/app.js` as `BUILD_RENDERER_STAMP`. For a software
  build the launcher's self-test is `runPresenterDiagnostics` (the palette blit)
  instead of the GL renderer's shader families, which would refuse to launch on
  exactly the devices this build exists to serve.

## Building and running it

```bash
nix develop .#wasm
./scripts/wasm-build.sh software engine/build-software
./scripts/wasm-boot-smoke.sh engine/build-software/bin                    # to FS_Init's data gate
./scripts/wasm-boot-smoke.sh engine/build-software/bin /path/to/basedir   # into demo1
./scripts/wasm-assemble-artifact.sh dist-software "" engine/build-software/bin
```

The separate build directory keeps the two configurations from sharing a CMake
cache, and the assemble script's third argument matters: without it the script
assembles `engine/build/bin`, the webgl2 build. Serve `dist-software/` as
[`../PWA.md`](../PWA.md#local-testing) describes; the runtime log should name
`renderer=software rasterizer`.

## What it does not do

These follow from the design rather than being bugs:

- no fog, no skyboxes, no GPU post-processing (HDR, FXAA, motion blur, soft
  particles): their cvars exist where shared code names them, the menu hides
  their rows, and nothing reads them;
- no 2D scaling beyond the presenter's: the UI is drawn at the render resolution;
- no multithreading: the rasterizer runs on the main wasm thread, which is why
  auto mode stops at 1024x768.

Open work on this configuration: the Software Graphics menu's remaining rows
(#329), the Display Preset row (`uhexen2-ibnq.7`), and the demo playback bar
(#123).
