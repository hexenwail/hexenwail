# Hexenwail PWA / GitHub Pages build

This repository now includes a GitHub-Pages-deployable, installable PWA shell for the WebAssembly build of Hexenwail.

## Goals

- same-origin deployment (no CDN/runtime fetches)
- installable on iPadOS via **Add to Home Screen**
- fully offline play after the first online load plus local asset import
- no cross-origin isolation requirement (single-threaded WASM; no pthreads / SharedArrayBuffer)

## Current architecture

- **Engine build:** Emscripten + SDL3 + WebGL2 / ES 3.0
- **PWA shell:** `web/index.html`, `web/app.js`, `web/sw.js`, `web/manifest.webmanifest`
- **Persistent asset storage:** OPFS first, IndexedDB fallback
- **Service worker:** caches launcher/runtime assets only; never caches user-imported game data
- **Base runtime path:** engine launches with `-basedir /persistent`

User-imported assets are mirrored into the runtime filesystem before `main()` is called.

## Local build

A working dev-shell path already exists in `WASM_BUILD_STATUS.md`. Outside Nix, a plain emsdk install is also fine.

### Option A: existing Nix dev shell

```bash
nix develop . -f shell-wasm.nix
cd engine
mkdir -p build
cd build
emcmake cmake -DCMAKE_BUILD_TYPE=Release -DUSE_CODEC_VORBIS=OFF -DUSE_ALSA=OFF -DUSE_SDL3_STATIC=ON ..
emmake make
```

### Option B: direct emsdk install

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh

cd /path/to/hexenwail/engine
mkdir -p build
cd build
emcmake cmake -DCMAKE_BUILD_TYPE=Release -DUSE_CODEC_VORBIS=OFF -DUSE_ALSA=OFF -DUSE_SDL3_STATIC=ON ..
emmake make
```

Build output lands in `engine/build/bin/`.

## Assemble a local PWA site

Copy the PWA shell plus engine artifacts into one directory:

```bash
mkdir -p dist
cp -r web/. dist/
cp engine/build/bin/hexenwail.js dist/
cp engine/build/bin/hexenwail.wasm dist/
cp engine/build/bin/hexenwail.html dist/engine-shell-debug.html
```

If Emscripten emits optional extra files such as `hexenwail.data` or `hexenwail.worker.js`, copy them too.

## Local testing

Service workers require HTTPS or `localhost`. `localhost` is allowed, so a local static server is sufficient:

```bash
cd dist
python3 -m http.server 8000
```

Then open <http://localhost:8000/>.

Recommended checks:

1. open once while online
2. import `pak0.pak` and `pak1.pak`
3. reload
4. disconnect networking / use airplane mode
5. reload again and verify the shell plus imported assets still work

## GitHub Pages deployment

A workflow is provided at `.github/workflows/pages.yml`.

### One-time repository setting

GitHub Pages **must** be configured manually in the repository settings:

- **Settings → Pages → Source → GitHub Actions**

The workflow YAML cannot flip that repository setting by itself.

### Workflow behavior

The workflow:

1. checks out the repo
2. installs emsdk directly on the Ubuntu runner
3. runs the lightweight Node-based PWA tests
4. builds the WASM client with `emcmake` / `emmake`
5. assembles a `dist/` site
6. deploys it with the supported Pages actions

All launcher URLs are relative (`./...`) so the site works under project Pages paths such as:

- `https://<user>.github.io/hexenwail/`

## Installing on iPadOS

1. Open the deployed site in Safari.
2. Wait for the first online load to finish.
3. Use **Share → Add to Home Screen**.
4. Launch the installed app.
5. Import your legally acquired Hexen II assets:
   - loose `.pak` / `.ogg` files
   - a directory selected with the folder picker (where supported)
   - or a `.zip` archive

Use assets from your own copy of Hexen II / Portal of Praevus, for example from GOG or Steam. This project does **not** include game data.

## Import behavior

Recognized inputs include:

- `pak0.pak`, `pak1.pak`, `pak2.pak`
- `pak3.pak` (mapped to `portals/`)
- files already inside `data1/`, `portals/`, `hw/`, or `music/`
- `.ogg` music files (loose files default to `data1/music/`)

ZIP imports are extracted entirely client-side in the browser. The importer rejects:

- absolute paths
- `..` traversal
- oversized archives / entries beyond the configured safety limits

Re-importing a file with the same logical path overwrites the previous local copy.

## Save game persistence

Local save games are just as important to keep offline as the imported PAK/OGG assets, so the
launcher treats the whole runtime data directory (`/persistent`, mounted at the engine's
`-basedir`) as a single persisted tree — not only the files you explicitly imported:

- the engine writes save games (`.hsv`, `clients.gip`, etc.) under the same `data1/`
  tree as the imported PAK files, via its normal `save`/`load` console commands
- the launcher periodically diffs the in-memory runtime filesystem against persistent
  browser storage (every ~10 seconds during play) and writes back anything new or changed,
  including save games created since the last sync
- an additional sync is forced when the tab is hidden/backgrounded (`visibilitychange`),
  when the page is about to be unloaded or bfcache-frozen (`pagehide`, `beforeunload`,
  `freeze`), and immediately after any asset import
- on the next launch (online or fully offline), the persisted tree — imported assets and
  save games alike — is restored into the runtime filesystem before `main()` is called, so
  saved games survive reloads, app restarts, and offline play sessions
- **"Clear imported data & saves"** intentionally clears the entire persisted tree, including
  save games, since they share the same storage; the in-app confirmation dialog says so
  explicitly before it destroys anything

Caveats:

- if the tab/app is killed abruptly (e.g. the OS force-quits it under memory pressure)
  before a sync completes, save-game writes since the last sync point could be lost — quit
  normally (switch away or close the tab) rather than force-killing when possible
- browser storage eviction under storage pressure (see below) can still remove save games
  along with everything else; requesting persistent storage via the Storage panel reduces
  but does not eliminate this risk

## Offline readiness

The service worker caches the launcher shell and WASM runtime files. Imported user assets live separately in OPFS / IndexedDB and are not part of the service worker cache.

Practical ways to confirm readiness:

- the in-app offline status text reports when the service worker has taken control
- reload once after installation/import
- then test in airplane mode
- on macOS, Safari Web Inspector can also verify the service worker and storage state remotely from an attached iPad

## Browser / iPadOS limitations

- **Pointer Lock:** not currently available on iPadOS Safari, so true desktop-style mouselook is limited there
- **Display mode:** installed iPadOS PWAs use `standalone`; this is effectively edge-to-edge but not true unrestricted fullscreen
- **Storage persistence:** `navigator.storage.persist()` is requested, but Safari can still evict data under storage pressure
- **Renderer feature gap vs desktop:** WebGL2 / ES 3.0 has no SSBOs, so `r_alias_gpu` remains disabled in WASM builds
- **Import size limits:** large ZIP archives can still hit memory/storage constraints on tablets
- **Audio feature parity:** the documented WASM build path currently disables Vorbis and ALSA-specific paths; browser audio parity still needs broader runtime validation

## Troubleshooting

### “Unable to find a proper Hexen II installation”

You have not imported a valid registered Hexen II data set yet. Import at least:

- `data1/pak0.pak`
- `data1/pak1.pak`

### Service worker updates seem stuck

Reload once while online so the new shell version can install, then reload again to let the updated worker control the page.

### Browser storage problems

Use **Clear imported data** in the launcher, then re-import the assets. If Safari has evicted storage, you will need to import again.

### ZIP import failed

The archive may contain unsupported paths, exceed the configured resource caps, or contain formats outside the recognized Hexen II layout.

### Mouse capture is incomplete on iPad

That is expected today. Pointer Lock is the main blocker on iPadOS Safari. External keyboards remain recommended, and virtual touch controls are future work.
