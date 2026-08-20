# Hexenwail WASM/Emscripten Build Status

See [docs/WEBGL_RENDERER.md](docs/WEBGL_RENDERER.md) for the current WebGL2
feature matrix, fallbacks, diagnostics, and graphical regression gates.

**Update 2026-08-13:** This file otherwise describes the state as of
2026-03-26 (~5 months stale) and has not been re-verified against a browser
run since. One claim below is now confirmed wrong: commit `59343680d`
(uhexen2-unvi) wired a libTiMidity MIDI fallback codec that *does* compile and
link for WASM (the `.wasm` grew from 1.4 MB to ~1.70 MB accordingly). It is
not yet audible in a browser — no soundfont reaches the Emscripten virtual FS
yet, tracked separately in uhexen2-g3j9 — so "no MIDI" is still the practical
outcome today, but the cause has changed from "not implemented" to "implemented,
not yet wired to a soundfont source". See the corrected "Known Limitations"
entry below. Nothing else in this file has been re-verified; treat the rest as
unconfirmed rather than current.

## Current State (2026-03-26)

### ✅ Completed
- **ES 3.0 Compatibility**: All C source files compile successfully for Emscripten/WASM target
  - Fixed GL immediate mode (GL_QUADS, GL_POLYGON) with ES 3.0 compatible stubs
  - Fixed shader initialization and particle system definitions
  - Fixed dynamic vs static GL function loading
  - Fixed type definitions (GLdouble) for WASM
  - Added WebGL2 (-sFULL_ES3=1) and memory growth flags

- **Code Correctness**: All rendering pipeline adapted for ES 3.0
  - 0% immediate mode usage
  - Software matrix stack
  - GLSL 4.30 core shaders (ES 3.0 compatible)

- **Linux & Windows Builds**: Both platforms build successfully
  - Linux NixOS & FHS builds ✓
  - Windows 64-bit with MinGW ✓

- **WASM Build**: Successfully builds with shell-wasm.nix development shell
  - All C source files compile to WASM object files
  - Emscripten SDL3 port downloads and builds via network access
  - Linking completes successfully (1.4 MB .wasm binary + 245 KB .js glue code
    at the time this was written; ~1.70 MB as of `59343680d` 2026-08-13, which
    added the libTiMidity MIDI fallback codec — see the 2026-08-13 update note
    at the top of this file)
  - Fixed undefined symbol GL_AliasGPU_SetUniforms by reorganizing ifdef guards

### ✅ Resolved: Nix Sandbox and Emscripten Ports

**Original Problem**: WASM build reached 91% completion (all C files compiled) but failed at linking phase due to Emscripten's SDL3 port requiring network download during the pure Nix sandbox build.

**Solution**: Use `shell-wasm.nix` development shell for interactive WASM development builds. This environment:
- Provides network access for Emscripten port downloads
- Maintains reproducibility at the dependency level (Emscripten and tools)
- Follows Nix best practices (pure flake for release, development shell for iterative work)
- Is documented in the project CLAUDE.md and shell-wasm.nix comments

**Technical Details - Fixed**:
- Undefined symbol GL_AliasGPU_SetUniforms was inside `#ifndef __EMSCRIPTEN__` block
- This prevented the function from being compiled for WASM, but gl_rmain.c called it unconditionally
- Fixed by reorganizing ifdef guards to exclude only shader initialization functions (which use SSBOs)
- Uniform setter functions now compile for both desktop GL and WebGL2/ES 3.0

## Workarounds

### Option 1: Development Shell (Recommended)
```bash
nix develop . -f shell-wasm.nix
cd engine && mkdir -p build && cd build
emcmake cmake -DCMAKE_BUILD_TYPE=Release -DUSE_CODEC_VORBIS=OFF -DUSE_ALSA=OFF -DUSE_SDL3_STATIC=ON ..
emmake make
```

The `shell-wasm.nix` provides an Emscripten development environment for interactive builds.

### Option 2: Docker/Container Build
Build in a non-Nix environment where network access is not restricted:
```bash
docker run -it emscripten/emsdk:latest
# Then build as above
```

### Option 3: Impure Flake Build
```bash
nix build .#wasm --impure --allow-network
```

## Files Modified

### engine/CMakeLists.txt
- Increased INITIAL_MEMORY from 64MB to 128MB (needed 76MB+)
- Increased STACK_SIZE from 1MB to 2MB
- Re-enabled `-sUSE_SDL=3` for proper SDL/WebGL integration

### flake.nix
- Added lenient error handling in buildPhase (build continues even if make fails)
- Added fallback HTML generation if WASM linking fails
- Made installPhase handle missing files gracefully

### New: shell-wasm.nix
- Development shell with Emscripten, CMake, Node.js, SDL3
- Designed for interactive WASM development
- Allows network access for port downloads

## Future Directions

1. **Pre-cache Approach**: Package Emscripten ports as Nix derivations
   - Pro: Flake builds would work in pure mode
   - Con: Significant effort to create/maintain

2. **Non-Flake Approach**: Move WASM to shell-nix only
   - Pro: Simpler, clearer separation of concerns
   - Con: Less integrated with rest of build system

3. **Docker CI/CD**: Build WASM in container pipeline
   - Pro: Network-unrestricted environment
   - Con: Requires Docker infrastructure

## Testing

WASM build now links successfully! Next steps:
- Deploy hexenwail.html, hexenwail.js, hexenwail.wasm to web server
- Open hexenwail.html in browser (Chrome, Firefox, Safari with WebGL2 support)
- Test HTML canvas rendering and WebGL initialization
- Test game data loading and initial rendering
- Test keyboard/mouse input handling
- Verify audio playback (currently disabled with USE_CODEC_VORBIS=OFF for WASM)
- Profile performance using browser DevTools

## Known Limitations (WASM)

- SSBOs (Shader Storage Buffer Objects) not available in WebGL2/ES 3.0
  - r_alias_gpu forced to 0 (disabled) in WASM builds — confirmed still true,
    `gl_rmain.c` sets `r_alias_gpu` default to `"0"` under `__EMSCRIPTEN__`
  - Alias model rendering falls back to streaming path
- Audio codecs: the flake's `packages.wasm` recipe still passes
  `-DUSE_CODEC_VORBIS=OFF`, and pkg-config codec discovery (Vorbis/XMP/Opus)
  is skipped entirely under `EMSCRIPTEN` in `engine/CMakeLists.txt` — WAV/FLAC/MP3
  (dr_libs, no external deps) are unaffected.
- MIDI synthesis: **outdated as of 2026-08-13.** FluidSynth itself is still
  unavailable for WASM (its glib/dbus/jack dependency closure cannot target
  Emscripten), but that is no longer the whole story — `59343680d`
  (uhexen2-unvi) added libTiMidity as a pure-C tier-2 MIDI fallback codec that
  *does* compile and link for WASM, reading the same `TimGM6mb.sf2` FluidSynth
  uses elsewhere. It has not produced audible MIDI in a browser yet because no
  soundfont is delivered into the Emscripten virtual filesystem (`SF_FindSoundFont()`
  has nothing to find there) — that gap is tracked in uhexen2-g3j9, not this file.
- No ALSA for platform audio control
- Networking limited to WebSocket/loopback due to browser sandbox

## Build Status Summary

| Platform | Status | Command |
|----------|--------|---------|
| Linux (NixOS) | ✅ Complete | `nix build .#nixos` |
| Linux (FHS) | ✅ Complete | `nix build .#linux-fhs` |
| Windows 64-bit | ✅ Complete | `nix build .#win64` |
| WASM (Flake) | ⚠️ Sandbox Limited | `nix build .#wasm --impure --allow-network` (if network access granted) |
| WASM (Dev Shell) | ✅ Complete | `nix develop -f shell-wasm.nix` (Recommended for dev builds) |


## PWA / GitHub Pages update (2026-08-07)

### ✅ Added in this session
- New root-level `web/` launcher assets for a GitHub Pages deployment target
  - `web/index.html`: responsive installable launcher shell with safe-area handling, storage/import UI, fullscreen button, and iPadOS notes
  - `web/app.js`: OPFS-first asset import pipeline with IndexedDB fallback, ZIP import, runtime filesystem population before `main()`, and periodic save/config sync back to browser storage
  - `web/sw.js`: versioned service worker precache for launcher/runtime assets only
  - `web/manifest.webmanifest` + placeholder icons for installability
- New `.github/workflows/pages.yml`
  - Builds the WASM target on Ubuntu using a direct emsdk install instead of relying on Nix sandboxed port fetching
  - Assembles a Pages-ready `dist/` directory
  - Deploys with GitHub's supported Pages actions
- Lightweight Node tests for ZIP parsing, path mapping, and launcher asset validation

### Build-system adjustments
- Emscripten link flags now explicitly keep the build single-threaded and browser-oriented:
  - `-sENVIRONMENT=web`
  - `-sALLOW_MEMORY_GROWTH=1`
  - `-sFORCE_FILESYSTEM=1`
  - `-sINVOKE_RUN=0`
  - `-sNO_EXIT_RUNTIME=1`
  - `-sEXPORTED_RUNTIME_METHODS=['FS','callMain','ccall']`
  - `-sEXPORTED_FUNCTIONS=['_main','_Hexenwail_TouchKey','_Hexenwail_TouchLook','_Hexenwail_ResizeCanvas']`
- `ccall` and the three `Hexenwail_*` exports exist for the phone-mode touch
  controls: naming a function in `EXPORTED_FUNCTIONS` is what keeps
  `EMSCRIPTEN_KEEPALIVE` from being dead-stripped at link time, and `ccall` is
  how the launcher reaches them.
- The browser shell still uses a custom `--shell-file`, now aligned with the new PWA launcher structure.

### Verified on port into this tree (2026-08-19)
- `nix-shell shell-wasm.nix` + `emcmake`/`emmake` links cleanly with the new flags, emitting
  `bin/hexenwail.{html,js,wasm}`; the generated JS exports both `callMain` and `FS`, and the
  PWA shell is substituted into `hexenwail.html`
- Desktop `nix build` stays green — the new flags live inside `if(EMSCRIPTEN)` and do not reach it
- `node --test web/test/*.test.js` passes (8/8)
- Phone-mode touch controls: `hexenwail.js` really exports `Hexenwail_TouchKey`,
  `Hexenwail_TouchLook` and `Hexenwail_ResizeCanvas`, so the launcher's `ccall`
  bridge resolves against a linked symbol rather than a stripped one

### Still not fully verified here
- Real GitHub Pages deployment still requires the repo setting **Settings → Pages → Source → GitHub Actions**
- Actual iPadOS hardware validation (install flow, storage persistence behavior, Pointer Lock limitations in practice) remains follow-up QA work
- Phone-mode touch controls have only been exercised through the Node DOM tests;
  stick feel, look sensitivity and the auto-gate's behavior on real phones and on
  an iPad with a keyboard/trackpad still need hardware QA

## Headless browser verification (2026-08-19)

First run of the assembled PWA artifact in a real browser. Method: headless
Chromium 151 with ANGLE/SwiftShader, driven over CDP; both `WEB_RENDERER`
configurations assembled with `scripts/wasm-assemble-artifact.sh` and served
over `http://127.0.0.1` (localhost is a secure context, so OPFS and the service
worker behave as they do on Pages); retail `pak0.pak` + `pak1.pak` +
`PROGS.DAT` + `PROGS2.DAT` + `Strings.txt` imported through the launcher's own
ZIP path as a 49 MB deflate archive rooted at `data1/`.

### Now confirmed in a browser, both `webgl2` and `software`
- ZIP import completes (5 files, ~1 s), OPFS backend selected, and the files
  land in the runtime filesystem at their exact sizes under `/persistent/data1/`
- **The `fe379df22` `-basedir` fix works.** The engine logs
  `FS_Init: basedir changed to: /persistent`, then
  `Added packfile /persistent/data1/pak0.pak (696 files)` and
  `Added packfile /persistent/data1/pak1.pak (523 files)`, then
  `Playing the registered version.` — not the shareware fallback, and no
  "Unable to find a proper Hexen II installation"
- Engine reaches `======== Hexen II Initialized =========` with no `Sys_Error`
  and no structural diagnostic block; the launch button's trusted click starts
  the engine and the status line settles on "Hexenwail running"
- Both renderers put a real frame on the canvas: the Hexen II console over the
  title art at startup, and the main menu (SINGLE PLAYER / MULTIPLAYER /
  OPTIONS / MODS / HELP / INTRO / QUIT) after Escape. The `webgl2` self-test
  reports 11 shaders and a passing RGBA8 FBO; the `software` build reports
  `400 x 300` rasterised and presented through the palette blit
- Escape opens and closes the menu identically in both configurations
  (~7 % / ~10 % of canvas pixels change per toggle), and an idle console frame
  still ticks — the blinking cursor moves ~0.02 % of pixels between captures
- `requestAnimationFrame` runs at 58–60 Hz in both configurations, i.e. the
  engine's per-frame work is not blocking the page (SwiftShader numbers are
  proof of life, not performance data)
- The launcher's own persistence plumbing round-trips: a file written under
  `/persistent/data1/s0/` is synced to OPFS on `pagehide`, survives a full page
  reload, and is restored into the runtime filesystem, with the paks still
  detected and the service worker reporting "Offline shell ready"

### Found by this pass (not fixed here)
- **Engine savegames and `config.cfg` are written outside the synced tree.**
  `DO_USERDIRS` is 1 on Emscripten (`engine/h2shared/sys.h:82` only disables it
  for Windows/OS2), so `parms.userdir` resolves to `/home/web_user/.hexen2`
  (`engine/hexen2/sys_unix.c:624`), `fs_userdir` becomes
  `/home/web_user/.hexen2/data1` (`engine/h2shared/quakefs.c:2009`), and every
  save path goes through `FS_MakePath(FS_USERDIR, ...)`
  (`engine/hexen2/host_cmd.c:678` and friends). `web/app.js` only ever walks
  `BASE_DIR` (`web/app.js:270`), so none of it is persisted. Confirmed at
  runtime: after a launch, `/home/web_user/.hexen2/data1/config.cfg` and
  `/home/web_user/.hexen2/qconsole.log` exist while `/persistent` still holds
  only the five imported files. The Storage panel's claim that saves are
  "written by the engine under your imported data1/ folder and are synced" does
  not hold, and "Export saves" would find nothing.
- **A paks-only import leaves the engine with no key bindings.** `hexen.rc` is
  not inside `pak0.pak` or `pak1.pak` (they carry `default.cfg` and no `.rc`),
  so `exec hexen.rc` (`engine/hexen2/host.c:1124`) fails — the runtime log shows
  `couldn't exec hexen.rc` — and with it the whole `default.cfg` → `config.cfg`
  → `autoexec.cfg` → `stuffcmds` chain. `Key_Init` binds only console, pause and
  gamepad keys (`engine/hexen2/keys.c:1181`), so nothing moves the player.
  Meanwhile `mapImportedPath` rejects a loose `hexen.rc`
  (`web/lib/paths.js:52`), the file input's `accept` list is `.pak,.ogg,.zip`,
  and the requirements line asks only for `pak0.pak` + `pak1.pak`. Importing the
  `data1/` folder, or a ZIP rooted at `data1/` that includes `hexen.rc`, does
  work.
- **`GL_TEXTURE_LOD_BIAS` is not a valid `texParameter` enum on WebGL2/ES 3.0.**
  A direct probe confirms `0x8501` returns `INVALID_ENUM` while
  `GL_TEXTURE_MAX_ANISOTROPY_EXT` is accepted. `engine/h2shared/gl_draw.c:577`,
  `:2403` and `:2552` call it unconditionally, so the `webgl2` build logs
  `GL error GL_INVALID_ENUM (0x0500) detected at end of frame` and `gl_lodbias`
  silently does nothing there. Pre-existing engine code; this pass only made it
  visible, because the PWA now surfaces the engine's log.
- **The game canvas is not bounded by the window outside phone mode.** `body`
  uses `min-height: 100vh` with no height cap and only the phone-mode rule pins
  `height: 100dvh` + `overflow: hidden` (`web/index.html:37`), so the `1fr`
  viewport row grows to the control panel's content height. In a 1280x720
  window the engine reports `Drawable: 832x2371` and the canvas is mostly below
  the fold.

### Still unverifiable this way
- Real-GPU behaviour: SwiftShader is a CPU rasteriser, so driver-specific
  rendering, actual frame rates and compressed-texture paths are untested
- iOS/iPadOS install, standalone-mode lifecycle and Pointer Lock limitations
- Touch controls and the touch-only auto-gate, which key off trusted touch
  events on real hardware
- Audio output (headless Chromium has no ALSA device; the engine reports the
  SDL emscripten driver opening successfully but nothing was heard)

Harness notes for whoever repeats this: `Page.captureScreenshot` with a `clip`
returns a stale or black surface for the WebGL canvas — capture the whole
viewport with `captureBeyondViewport: false` and crop afterwards. Key events
injected with `Input.dispatchKeyEvent` are consumed before page-level capture
listeners see them, so "no keydown observed" is not evidence that input failed;
drive the engine through its `Hexenwail_TouchKey` export when a pixel-independent
signal is needed.
