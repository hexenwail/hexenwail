# Hexenwail WASM/Emscripten Build Status

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
  - `-sEXPORTED_RUNTIME_METHODS=['FS','callMain']`
- The browser shell still uses a custom `--shell-file`, now aligned with the new PWA launcher structure.

### Still not fully verified here
- Actual `emcc`/`emmake` compilation was not re-run in this sandbox unless an environment with emsdk is available
- Real GitHub Pages deployment still requires the repo setting **Settings → Pages → Source → GitHub Actions**
- Actual iPadOS hardware validation (install flow, storage persistence behavior, Pointer Lock limitations in practice) remains follow-up QA work
