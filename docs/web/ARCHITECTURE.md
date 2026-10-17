# The web build: one client, two renderers

Hexenwail's browser client is the same engine as the desktop one, compiled with
Emscripten, and it comes in two configurations that differ only in the renderer.
This document explains how they fit together. The pieces have their own docs:

- [`../PWA.md`](../PWA.md): the launcher, storage, offline shell, phone mode,
  imports, saves, deployment;
- [`../WEBGL_RENDERER.md`](../WEBGL_RENDERER.md): the GL renderer on the
  OpenGL ES 3.0 / WebGL2 tier;
- [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md): the restored 8bpp rasterizer.

## The choice is made at configure time

`WEB_RENDERER` in `engine/CMakeLists.txt` picks one; it is ignored outside
Emscripten.

| `WEB_RENDERER` | Client defines | Renderer sources | Role |
|---|---|---|---|
| `webgl2` (default) | `GLQUAKE SDLQUAKE NO_SPLASHES`, and `USE_GLES` forced on | `GL_RENDERER_SOURCES` | the shipping build |
| `software` | `WEBQUAKE WEBSOFT SDLQUAKE NO_SPLASHES` | `SOFT_RENDERER_SOURCES` | opt-in; live archival of uHexen2's 8bpp software renderer |

Both also get `__EMSCRIPTEN__`. The defines answer different questions, which is
why there are several:

- **`__EMSCRIPTEN__`**: running in a browser at all.
- **`USE_GLES`**: the graphics API tier. Everything the ES path does differently
  (static entry points instead of `dlsym`, no SSBO or compute, `#version 300 es`
  headers, an ES context) follows from the API level, not from the browser, so it
  is keyed on this and a desktop `-DUSE_GLES=ON` build reaches the same path.
- **`WEBQUAKE`**: stands in for `GLQUAKE` where shared code asks whether the
  extended 2D API exists (`draw.h`, `vid.h`, and one site each in `screen.c` and
  `menu.c`). Only the software build sets it, because only there is `GLQUAKE`
  absent; the webgl2 build has `GLQUAKE` and needs no stand-in.
- **`WEBSOFT`**: the rasterizer, and nothing else. `quakeinc.h` uses it to include
  the historical `model.h`/`d_iface.h`/`soft_web.h` set in place of
  `glquake.h`/`gl_model.h`; menu groups use it to hide rows the rasterizer cannot
  honour.

## Everything else is shared

Only the renderer is swapped. In both configurations:

- **the platform layer is SDL3**: `in_sdl.c`, `sys_sdl.c` and `snd_sdl.c`, the same
  files every desktop target uses. The software build creates an SDL window with an
  ES 3.0 context too, purely to present its framebuffer.
- **the client, server, QuakeC VM and filesystem** are the desktop ones, and so is
  the Rust archive (`engine/rust`), built for `wasm32-unknown-emscripten`.
- **the launcher is one PWA**: `web/index.html`, `web/app.js`, `web/lib/`,
  `web/sw.js`. It does not care which engine it serves, apart from one stamp
  (below).

The alextnewman/hexenwail fork, which this web build was mined from, went the
other way (its `Web port: default to the classic software renderer on an
accelerated canvas (#8)`, 0b1906add, is the commit the restore was ported from): an Emscripten-only tree, a bespoke browser platform layer instead of
SDL, a `WEBGL2QUAKE` renderer macro, and software as the default. None of that was
ported. The fork's own `docs/web/` described that architecture, which is why those
documents were not brought over and these were written instead.

## From build to browser

1. **Build.** `scripts/wasm-build.sh [webgl2|software] [build-dir]` is the one
   configure-and-build recipe; CI, the Pages deploy and a person at a terminal all
   run it. `nix develop .#wasm` provides Emscripten (nixpkgs') and the Rust
   target; CI instead installs a pinned emsdk (`.github/actions/wasm-build`), so
   a local build is close to, not identical with, CI's. `nix build .#wasm` also
   builds the default configuration, in the sandbox, but it is a different path:
   `web/` is outside its source filter, so it links with emcc's default shell
   and installs that as `index.html` with the `.js` and `.wasm`, but not
   `app.js` or the rest of `web/`, and no `hexenwail-renderer.txt`. That is
   neither a working launcher nor the `bin/` directory the steps below read.
2. **Stamp.** CMake writes the configuration into `hexenwail-renderer.txt` next to
   the binary.
3. **Assemble.** `scripts/wasm-assemble-artifact.sh` copies `web/` and the engine
   into a site, reads the stamp (refusing anything but `webgl2` or `software`; a
   bin directory with no stamp at all is taken as `webgl2`), and writes it into
   the site's copy of `web/app.js` as `BUILD_RENDERER_STAMP`. Served unassembled from
   `web/`, the placeholder survives, which reads as webgl2.
4. **Gate.** Before offering Start, the launcher runs the self-test that
   configuration needs: `runWebGLDiagnostics` (the GL renderer's shader families
   and an RGBA8 framebuffer) for webgl2, `runPresenterDiagnostics` (just the
   palette blit) for software. A failed test blocks the launch and says why,
   rather than starting an engine that would draw a black canvas.
5. **Run.** The launcher writes the player's imported files from browser storage
   (OPFS, or IndexedDB where OPFS is missing) into Emscripten's in-memory
   filesystem under `/persistent`, then calls `main` with `-basedir /persistent`
   and any mod or mission-pack flag. Saves flow back the other way during play.

## What CI holds each configuration to

`.github/actions/wasm-build/action.yml`, run by `ci.yml` on every PR and by
`pages.yml` on deploy:

- the PWA's Node tests and a headless WebGL2 smoke test of the engine's real GLSL;
- the Rust/C ABI layouts at wasm32's 4-byte pointers;
- **both** configurations built, the software one into its own directory as a
  regression guard: nothing else would compile it;
- **both** booted in headless Chromium by `scripts/wasm-boot-smoke.sh`, which must
  reach `FS_Init`'s data gate and stop there with its exact message instead of
  trapping;
- the artifact assembled from the webgl2 build, which is what Pages deploys.

The software configuration has further source-level gates in `lint.yml`; they
are listed in [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md#gates).

## Known limits

- `qconsole.log` is written to the MEMFS working directory, not the persisted
  basedir: `LOG_Init` runs before `FS_Init` applies `-basedir`. That keeps an
  ever-growing log out of the 10-second IndexedDB sync, at the cost that it is
  lost on reload and cannot be pulled for a bug report. Deliberate; revisit if
  testers need logs from the web build (GitHub #150 g).
