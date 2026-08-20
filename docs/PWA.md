# Hexenwail PWA / GitHub Pages build

This repository now includes a GitHub-Pages-deployable, installable PWA shell for the WebAssembly build of Hexenwail, including a rudimentary phone mode for iPhone/iOS Safari.

## Goals

- same-origin deployment (no CDN/runtime fetches)
- installable on iPadOS/iOS via **Add to Home Screen**
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

## Installing on iPadOS / iOS

1. Open the deployed site in Safari.
2. Wait for the first online load to finish.
3. Use **Share → Add to Home Screen**.
4. Launch the installed app.
5. Import your legally acquired Hexen II assets:
   - loose `.pak` / `.ogg` files
   - a directory selected with the folder picker (where supported)
   - or a `.zip` archive
6. Press **Start game** (**Start game (demo)** when only `pak0.pak` is present).
   Importing assets never starts the engine on its own, so the launcher stays
   reachable and a pending shell update can install first.

Use assets from your own copy of Hexen II / Portal of Praevus, for example from GOG or Steam. This project does **not** include game data.

## Phone mode

On phone-sized coarse-pointer devices, starting the engine switches the page from
the launcher into a game-focused viewport: the header and import panels disappear,
the canvas fills the visual viewport, safe-area insets are honored, page scrolling
is disabled only while playing, and resize/rotation events are forwarded to the
SDL/WebAssembly renderer. Landscape is strongly recommended on iPhone because it
leaves room for the game view plus touch controls.

The in-game **☰** button opens a small phone overlay. It can resume play, send
Escape to the engine menu, or **Sync & exit to launcher**. The latter first
syncs the runtime filesystem to browser storage and then reloads the page to get
a fresh WebAssembly runtime. The launcher's own **Exit to launcher** button does
the same thing from a desktop or tablet layout, and is enabled only while the
engine is running.

Selecting **Quit** inside Hexen II uses a browser-specific Emscripten path: the
engine performs its normal shutdown, cancels the browser main loop, notifies the
launcher, and the launcher syncs saves before returning to a stopped launcher
state. The current WASM runtime is treated as not safely restartable after this
shutdown, so the launcher shows **Restart game**, which performs the same
sync-then-reload flow. Fatal engine errors are reported separately and also ask
for a clean restart.

### Touch mappings

In auto mode, touch controls are shown only when the launcher believes the device
is a touch-only phone environment: a coarse pointer with no hover/fine pointer,
no connected gamepad, and a viewport short side no larger than 820 CSS pixels.
If a touchpad/mouse, physical keyboard activity, pen, wheel, or controller is
detected, auto mode hides the overlay and releases all held touch inputs. The
launcher setting can choose auto/on/off behavior, left- or right-handed layout,
and look sensitivity; **Always show** displays the controls on any running
device.
Preferences are stored in local browser storage, separate from imported game
assets and saves.

Default mappings reuse existing engine input bindings:

- left virtual stick: `W` / `S` forward and back, `A` / `D` strafe left and right
- right look region: relative mouse-look deltas through a small JS-to-C bridge
- **Atk**: primary attack (`MOUSE1`)
- **Jump**: jump (`SPACE`)
- **Use**: lift/use interaction (`K_GP_LTHUMB`, default `impulse 13`)
- **◀ / ▶**: previous/next weapon (`K_GP_LSHOULDER` / `K_GP_RSHOULDER`)
- phone overlay **Send Esc/Menu**: Escape

The controls use Pointer Events and track each touch by pointer ID, so moving,
looking, and firing can happen at the same time. Held keys/buttons are released
on cancellation, backgrounding, orientation changes, overlay opening, and quit to
avoid stuck movement or fire.

Hardware keyboard, mouse, and physical gamepad support are preserved. On iPad
and desktop, touch controls stay hidden by default unless explicitly enabled.

## Import behavior

Recognized inputs include:

- `pak0.pak`, `pak1.pak`, `pak2.pak`
- `pak3.pak` (mapped to `portals/`)
- loose `progs.dat` / `progs2.dat` (mapped to `data1/`; the demo ships its
  gamecode outside the pak)
- files already inside `data1/`, `portals/`, `hw/`, or `music/`
- `.ogg` music files (loose files default to `data1/music/`)

ZIP imports are extracted entirely client-side in the browser. The importer rejects:

- absolute paths
- `..` traversal
- oversized archives / entries beyond the configured safety limits

Re-importing a file with the same logical path overwrites the previous local copy.

Mods are imported separately and are *not* mapped into `data1/` — see [Playing
mods](#playing-mods).

## Getting the free demo

When no `data1/pak0.pak` has been imported yet, the launcher shows a **Get the free
demo (13 MB)** button next to the import controls. It downloads Raven's November 1997
three-level Hexen II demo, verifies it, and installs it through the same import path a
hand-picked file takes; the launch button then reads **Start game (demo)**. The button
disappears once any `pak0.pak` is present.

Hexenwail hosts no game data. The button points the browser at a third party exactly as
`scripts/get_demo.sh` does from a shell — see `assets/demo/README.md` for why the project
does not mirror Raven's data itself.

### Where the bytes come from

`web/lib/demo-fetch.js` holds every host-specific value in one `HEXEN2_DEMO_SOURCE`
object. Two sources are tried in order:

1. `./demo/hexen2demo.zip` on the site's own origin, probed with a `HEAD` request.
   Absent from the public Pages deploy; see below.
2. `https://archive.org/cors/hexen2demo_nov1997-linux-x86_64/hexen2demo_nov1997-linux-x86_64.tgz`.
   archive.org's `/cors/` endpoint echoes the requesting `Origin`, which is what makes it
   fetchable from a page at all. SourceForge, the upstream home of the same tarball, sends
   no CORS headers and serves browser user-agents an interstitial.

Gzip is decompressed with `DecompressionStream('gzip')` and the tar is unpacked by a small
parser in the same file; there are no third-party libraries. A browser without
`DecompressionStream` disables the button with an explanatory tooltip — the same constraint
already applies to ZIP import, which needs the later `deflate-raw` format.

### What is verified

Every file the button installs is pinned by size and SHA-256:

| Install path | Bytes | SHA-256 |
|---|---|---|
| `data1/pak0.pak` | 27,750,257 | `0d4aa01a9909771dfa8e5be27db5d6628dc92f1406998c1a89c27d4748aaf151` |
| `data1/progs.dat` | 886,592 | `6981e0076329c95fbf41ff2c62767d4ea02e277eee888323bbe0a1ece4a8f62a` |
| `data1/hexen.rc` | 340 | `fd30dba85635d879f5d72043e8a914c5465ea77068950c38513d95cde21ce339` |
| `data1/default.cfg` | 1,875 | `8199a001d204cf6b0ac20627143b0febc317054ea28ccda548d45418ee250536` |
| `data1/autoexec.cfg` | 1 | `01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b` |
| `data1/maps/demo2.txt` | 572 | `5a07f04b94bce17625e200774d2e414553624bcd38c0971c8085ba957ae8af7a` |
| `data1/maps/demo2.ent` | 55,132 | `7823ecb49d00fde6101412975f8df2a91f4c37232696c3c0c8a3d78340aad35b` |

`pak0.pak` and `progs.dat` are required — the demo `pak0.pak` contains no gamecode, so the
loose `progs.dat` is not optional. `pak0.pak`'s MD5 (`8e598d82bf53436ed7a0e133aa4b9f09`) is
also the engine's own genuine-demo fingerprint in `engine/h2shared/quakefs.c`.

Nothing else in the archive is installed: the 1997 engine binaries and the bundled docs
tree are ignored. Files are written only after every digest has matched, so a failed
download or a mismatch leaves nothing behind and can simply be retried. The pinned
tarball SHA-256 (`e26e0f2d…`) is advisory: a mismatch is logged, not fatal, because other
packagings of the same demo carry identical game files in a different container.

### Self-hosting the archive

Publish `demo/hexen2demo.zip` beside the launcher and it wins the probe, so the download
never leaves your origin — useful for offline or air-gapped deployments. The ZIP needs a
`data1/` layout (or any layout the normal importer recognizes) and its files must match the
digests above; anything else is rejected. The public Pages deploy does not ship this file
and the service worker never precaches it.

## Playing mods

The **Mods** panel is the browser's version of the command line's `-game`:

```
hexen2 -basedir /path/to/hexen2 -game mymod     # desktop
Import mod folder -> pick "mymod" -> Active mod: mymod -> Start   # launcher
```

**Import mod folder** opens a directory picker. The folder you pick *is* the gamedir: its
name becomes the directory the engine is launched with, and its contents are stored under
that name with their internal structure intact (`mymod/progs.dat`, `mymod/maps/e1m1.bsp`,
`mymod/pak0.pak`, …). Pick the mod's own folder — if a download wraps the mod in an extra
directory, descend into it first, or the wrapper becomes the gamedir instead.

Unlike the base-game importer, nothing here is flattened into `data1/`: that mapper exists
to turn a retail install into the layout the engine expects, and a mod's whole meaning is
that its files sit *above* `data1` in the search path.

### Gamedir names

The folder name is folded down to what the engine accepts as a single path component
(`FS_Gamedir` refuses any name containing `/`, `\`, `:` or `..`): lowercased, anything
outside `a-z 0-9 _ -` replaced with `-`, and truncated to 32 characters. `Keep 2.0!`
becomes `keep-2-0`. `data1`, `portals` and `hw` are refused — the engine ignores `-game`
for all three, so importing one as a mod would create a control that cannot do anything.

### Which files are imported

Mod content is accepted by file type, since a mod's payload is whatever its `progs.dat`
asks for:

`.pak` · `.dat` `.cfg` `.rc` `.lst` `.txt` · `.bsp` `.lit` `.ent` · `.mdl` `.spr` ·
`.lmp` `.wad` `.pcx` `.tga` `.png` `.jpg` `.jpeg` `.dds` `.ktx` ·
`.wav` `.ogg` `.mp3` `.flac` `.opus` `.mid` `.it` `.s3m` `.umx` · `.dem`

Anything else — archives, installers, scripts, `.gip`/`.hsv` savegames — is ignored and
listed under the panel. ZIP archives are not unpacked by the mod importer; extract the mod
first and import the resulting folder.

### Full game only

`-game` requires a registered install. On a demo install the engine fatals at startup with
*"You must have the full version of Hexen II to play modified games"*, so the launcher does
not pass the flag without `data1/pak1.pak`: an imported mod stays selected and listed, the
panel says why it will not load, and the demo still launches unmodded. Should the engine
refuse a gamedir for any other reason, its own message is what the runtime log shows.

Passing `-game` also folds `portals/` into the search path below the mod, because mods
commonly depend on mission pack models. With no mission pack installed the engine prints
`Missing or invalid mission pack installation` once and continues — that line is expected,
not a failed launch. Launching the mission pack *itself* is a different flag (`-portals`)
and is not offered by this panel yet.

### Removing a mod

**Remove** deletes the selected gamedir from both the runtime filesystem and persistent
storage, **including any save games stored under it**. Nothing else is touched; "Clear
imported data & saves" remains the way to remove everything at once.

### Mods and saves

Save bundles follow the gamedir. With `-game mymod` the engine writes to
`/persistent/mymod/s0/…`, and the exporter, the manifest and the importer all accept an
imported gamedir alongside `data1`, `portals` and `hw` — with the same allowlist shape, so
a bundle can still only address `<gamedir>/s<N>/`. Importing a bundle whose gamedir is not
installed here warns before it commits: the files land, but nothing loads them until the
mod is imported too.

## Save game persistence

Local save games are just as important to keep offline as the imported PAK/OGG assets, so the
launcher treats the whole runtime data directory (`/persistent`, mounted at the engine's
`-basedir`) as a single persisted tree — not only the files you explicitly imported:

- the engine writes save games (`.hsv`, `clients.gip`, etc.) under the same `data1/`
  tree as the imported PAK files, via its normal `save`/`load` console commands — or
  under `<gamedir>/` when a mod is active, since `-game` moves the userdir with it
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

## Portable save bundles

The launcher’s **Saves & Backup** section can move save games between devices without
an account, upload, cloud service, or backend:

1. After saving, return to the launcher and choose **Export saves**. The launcher first
   completes its runtime-to-browser-storage sync.
2. Use the iPadOS Share sheet to save the dated `.hexenwail-save.zip` file to **Files** or
   **iCloud Drive** (desktop browsers download the same file).
3. On the other device, import your legally acquired matching game data first, then choose
   **Import saves** and select the bundle from Files/iCloud Drive.
4. Review the date, game directories, file count, size, and any compatibility warning,
   then confirm the import. Reload before loading an imported save if the engine is already
   running.

Bundles are ordinary ZIP files with a versioned `hexenwail-save.json` manifest and save
files under `saves/data1/`, `saves/portals/`, or `saves/hw/`. They contain only recognized
engine save-slot files (such as `s0/info.dat` and `.gip` state), never PAKs, OGG music,
runtime binaries, caches, or other imported commercial game data. The manifest records
file SHA-256 hashes and sizes and records local PAK paths, sizes, and hashes for comparison
only; it does not embed PAKs.

Choose **Merge saves** to replace only bundle paths that already exist, or **Replace saves**
to delete existing recognized save files before adding the bundle. Neither mode deletes PAKs,
music, or other assets. The importer verifies the manifest, ZIP safety limits, paths,
duplicates, file sizes, and SHA-256 hashes before writing anything. It rejects unsupported
future formats and unsafe, unexpected, or undeclared ZIP entries. If persistent storage
cannot complete a write, affected save files are restored from a local rollback snapshot.

Compatibility warnings mean that required base-game or expansion PAKs are absent or differ
from the exporting device. They do not put commercial data in the bundle: import matching
legal assets separately. A bundle can be exported and imported entirely offline; browser
storage and iCloud Drive/Files behavior remain subject to their own available space and
sync timing. Because abrupt app termination can interrupt the normal ten-second save sync,
export from the launcher after switching away from play rather than relying on a force-quit
to preserve the most recent save.

## Offline readiness

The service worker caches the launcher shell and WASM runtime files. Imported user assets live separately in OPFS / IndexedDB and are not part of the service worker cache.

The cache name is versioned per deployment: `web/sw.js` ships the placeholder
`__HEXENWAIL_BUILD_VERSION__`, and `scripts/wasm-assemble-artifact.sh`
substitutes the commit SHA when it assembles `dist/`. Activation deletes every
older `hexenwail-pwa-` cache, so a deploy cannot be served a mix of old and new
shell files. `scripts/wasm-validate-artifact.sh` fails the build if the
placeholder survives into the artifact.

Practical ways to confirm readiness:

- the in-app offline status text reports when the service worker has taken control
- reload once after installation/import
- then test in airplane mode
- on macOS, Safari Web Inspector can also verify the service worker and storage state remotely from an attached iPad

## Browser / iPadOS limitations

- **Pointer Lock:** not currently available on iPadOS Safari, so true desktop-style mouselook is limited there
- **Phone look:** iPhone/iOS Safari also lacks Pointer Lock; phone mode feeds relative drag deltas through an explicit engine bridge instead of relying on browser mouse capture
- **Display mode:** installed iPadOS PWAs use `standalone`; this is effectively edge-to-edge but not true unrestricted fullscreen
- **Restart after Quit:** after the engine's normal Quit, the page returns to the launcher but reloads before starting a new game because the same WebAssembly runtime is not assumed to be restartable
- **Storage persistence:** `navigator.storage.persist()` is requested, but Safari can still evict data under storage pressure
- **Renderer feature gap vs desktop:** WebGL2 / ES 3.0 has no SSBOs, so `r_alias_gpu` remains disabled in WASM builds
- **Import size limits:** large ZIP archives can still hit memory/storage constraints on tablets
- **Audio feature parity:** the documented WASM build path currently disables Vorbis and ALSA-specific paths; browser audio parity still needs broader runtime validation

## Troubleshooting

### Launcher says “running” but the canvas stays black

Use the **Runtime log** card in the launcher. It keeps the latest engine startup
status plus stdout and stderr in the page, including failures that would otherwise
only be visible in browser developer tools.

### “Unable to find a proper Hexen II installation”

The engine did not recognize the data you imported as any Hexen II edition.
Import a complete set for the edition you own:

- full game: `data1/pak0.pak` **and** `data1/pak1.pak`. A registered `pak0.pak`
  on its own is not a playable subset and stops with exactly this message.
- Nov 1997 demo: its `data1/pak0.pak` plus the loose `data1/progs.dat` that
  ships beside it.

### Service worker updates seem stuck

The launcher checks for an update whenever it opens and whenever it returns to
the foreground (`pageshow` and `visibilitychange`), which is what an installed
iOS PWA does instead of a page load. When a new worker takes control while the
launcher is idle the page reloads itself; if the engine is running, the offline
status text reports that the update installs on the next exit to the launcher.

If it still looks stale, exit to the launcher while online and background/restore
the app once. Imported game data and saves are outside the service worker cache
and are never affected by a shell update.

### Browser storage problems

Use **Clear imported data** in the launcher, then re-import the assets. If Safari has evicted storage, you will need to import again.

### ZIP import failed

The archive may contain unsupported paths, exceed the configured resource caps, or contain formats outside the recognized Hexen II layout.

### The demo download failed

The status line names the reason. A network or CORS error means archive.org was
unreachable from this browser; a verification failure means the bytes that arrived were
not the Nov 1997 demo, and nothing was written either way — press the button again. If the
button is disabled with a tooltip about `DecompressionStream`, the browser is too old for
either the demo download or ZIP import; import `pak0.pak` as a loose file instead.

### Mouse capture is incomplete on iPad/iPhone

That is expected today. Pointer Lock is the main blocker on iPadOS Safari. External keyboards, mice, and physical controllers remain supported; iPhone phone mode adds explicit touch controls for movement, looking, attack, jump, use, weapon switching, and menu access.
