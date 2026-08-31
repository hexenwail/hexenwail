---
name: run-hexenwail
description: Build, launch, drive and screenshot the Hexenwail (uHexen2) Hexen II engine headlessly. Use when asked to run, start, launch, play, test, smoke-test or screenshot the engine, to see a renderer/menu/HUD change on screen, or to exercise gamecode, maps, savegames, protocol, shaders or cvars without a display. Covers glhexen2 (GL client, under Xvfb), h2ded (dedicated server, no X at all) and the WebAssembly/PWA web client.
---

# Running Hexenwail

Hexen II engine, C + CMake, built with Nix. Three lanes, and **which one you
want is the first decision**:

| You are changing | Drive it with | Cost |
|---|---|---|
| Gamecode, physics, savegames, protocol, filesystem, cvars | `tools/serve.sh` — `h2ded` on stdin, no X | ~5 s |
| Web client, PWA shell, GLSL shaders | `node --test` + `scripts/webgl-smoke-test.sh` | 0.4 s / ~20 s |
| Renderer, menus, HUD, input, anything visual | `tools/headless-drive.sh` — engine under Xvfb, real X key events, PNG screenshots | 25 s menu / ~75 s in-game |

Do not reach for the GUI lane to check something a cheaper lane can answer.
Under llvmpipe a map load alone costs 25 seconds.

All paths below are relative to the repo root. Every command here was run on
this box.

## Prerequisites

This box is NixOS and has `bwrap` already. **Nothing else is installed
globally — there is no `chromium`, no `node`, no `Xvfb` on `PATH`.** Each lane
brings its own tools with an ad-hoc shell:

```bash
# GUI lane
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command <cmd>

# web lane  (chromium is a 203 MiB fetch the first time, then cached)
nix shell nixpkgs#chromium nixpkgs#nodejs --command <cmd>
```

`serve.sh` needs `bwrap` and nothing else — no shell wrapper.

## Build

```bash
nix build .#default        -o result            # glhexen2 + bundled gamecode
nix build .#h2ded-bundled  -o result-h2ded      # dedicated server
nix build .#demodata       -o result-demodata   # Raven's free 1997 demo data
```

`.#demodata` is the point: the engine ships no game assets, and this is a
hash-verified `data1` in the store, so a clean machine needs no purchased copy.
Its basedir is `result-demodata/share/hexenwail` and it carries `demo1`,
`demo2`, `demo3`. Substitute a real install (`~/hexen2`) only when a check
actually needs the other 39 maps.

A no-op `nix build .#default` is ~3 s; a touched `.c` is ~1–2 minutes. Other
flake outputs: `gamecode nixos nixos-bundled h2ded h2ded-win64 win64 utils
utils-win64 linux-fhs release wasm`.

Cheapest possible "does the binary still boot" check — no X, 12 ms, and what
CI's own smoke step runs. `-help` is handled before any SDL or video init:

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  timeout -k 5 10 ./result/bin/glhexen2 -help
```

## Test

The tree's automated checks are in two places — **there is no single `make
test`**:

```bash
# 67 JS unit tests: PWA shell, zip import, save bundles, touch controls,
# plus gates on the engine's GLSL (ES-tier headers, uniform budget). 0.4 s.
nix shell nixpkgs#nodejs --command bash -c 'node --test web/test/*.test.js'

# progs field-set and gamecode-marker gates, in the derivation's checkPhase.
# --no-link matters: a bare `nix build .#gamecode` would repoint ./result
# away from the engine you are about to screenshot.
nix build .#gamecode --no-link
```

Both are what CI runs (`.github/actions/wasm-build/action.yml`, `ci.yml`).

## Run: the server lane (fast, no X)

Console commands on stdin, one per line. `wait N` paces them; `quit` is
appended for you.

```bash
printf 'map demo1\nwait 3\nstatus\nedicts\n' | \
  ./tools/serve.sh /tmp/out-serve result-demodata/share/hexenwail
```

Read the result in **`/tmp/out-serve/qconsole.log`** — h2ded prints nothing to
stdout when stdout is not a tty, so `engine.stdout` beside it is normally
empty. That run ends `rc=0` in ~5 s and the log contains `map:     demo1`,
`players: 0 active (8 max)` and a 300-entity `edicts` dump.

`ENGINE=` overrides the binary, `TIMEOUT=` the 120 s hard kill.

## Run: the web lane (fast, no X)

The web client is a real third target — a WASM/WebGL2 PWA — and the native
builds cannot catch a regression in it. Two checks, neither needing a wasm
build:

```bash
nix shell nixpkgs#chromium nixpkgs#nodejs --command ./scripts/webgl-smoke-test.sh
```

~20 s. This compiles **the engine's actual GLSL sources** against WebGL2 under
headless Chrome + SwiftShader, then checks the framebuffer is not predominantly
black. Success is one line:

```
WebGL2 engine shaders, RGBA8 framebuffer, post-process, and non-black-frame smoke test passed.
```

**It prints `ERROR:` lines from dbus and `SharedImageManager` on the way there
and still passes.** Judge it by that final line (or the exit code), never by
the presence of stderr noise.

To build the wasm client itself, use `shell-wasm.nix` — `nix build .#wasm` is
deliberately not the supported path, because Emscripten fetches its SDL3 port
over the network at configure time and the nix sandbox has none:

```bash
nix develop . -f shell-wasm.nix     # then:
./scripts/wasm-build.sh                            # webgl2, the shipping config
./scripts/wasm-build.sh software engine/build-soft # 8bpp software rasterizer
```

## Run: the GUI lane (screenshots)

`tools/headless-drive.sh` boots the engine under Xvfb and drives it with
**real X key events via xdotool**, so input arrives through SDL and
`Key_Event` exactly as from a keyboard — menu paths genuinely execute; this is
not a scripted-console simulation.

Use the `script` scenario and a step file. Do not add a case arm to the script
for a flat sequence of keys and shots.

```bash
cat > /tmp/steps.txt <<'EOF'
shot 01-main
keyn Down 3
key Return
shot 02-mods-menu
EOF

nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env STEPS=/tmp/steps.txt \
  ./tools/headless-drive.sh script /tmp/out-mods result-demodata/share/hexenwail
```

~25 s. PNGs land in `/tmp/out-mods/`, the engine's own log in
`/tmp/out-mods/qconsole.log`. **Open the PNGs.** A run that navigated nothing
still reports `shot 01-main … shot 02-mods-menu` and exits 0. (That run above
lands on MODS and shows `Hexen II` / `Portal of Praevus (none)`.)

Step verbs: `shot NAME`, `shotf NAME` (no settle, for time series),
`key KEYSYM`, `keyn KEYSYM N`, `type TEXT`, `enter TEXT`, `console`,
`cmd TEXT` (console + command + console), `hold KEYSYM SECS`, `mouse X Y`,
`click [BUTTON]`, `wipe`, `sleep SECS`, `# comment`. An unknown verb aborts
the run rather than being skipped.

To reach the world instead of a menu, pass `+map demo1` as a trailing engine
argument and open with `sleep 25`:

```bash
cat > /tmp/steps-ingame.txt <<'EOF'
sleep 25
shot 10-ingame
cmd god
cmd chase_active 1
sleep 2
shot 11-chasecam
cmd gl_overbright 0
sleep 2
shot 12-overbright-off
EOF

nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env STEPS=/tmp/steps-ingame.txt \
  ./tools/headless-drive.sh script /tmp/out-ingame result-demodata/share/hexenwail +map demo1
```

~75 s. Confirm the commands actually landed by grepping the log for their
echoes (`godmode ON`, `]chase_active 1`) before trusting the PNGs.

Other environment: `ENGINE=` (default `./result/bin/glhexen2`), `W=`/`H=`
(default 800x600), `WORK=` (sandboxed HOME), `DISPLAY_N=`. Each run claims its
**own** X display atomically via Xvfb, so runs may be started concurrently; a
run that finds its display occupied aborts rather than screenshotting someone
else's engine. `<outdir>/pids` records the display and pids for a run that has
to be reaped by hand.

The named scenarios still in the script (`oldmission_demoness`, `console_tab`,
`ayrn_pulse`, `noportals`, …) are past bug investigations. Their comments
record what each was proving and are worth reading before writing a similar
check; their `ingame()` helper is the model for cheats-plus-movement sequences.

## Run: human path

`nix run .` on a machine with a display and game data. Useless here — it opens
a window and blocks.

## Gotchas

- **`node --test web/test/` (the directory) fails** with `MODULE_NOT_FOUND`
  and a bogus `test at web/test:1:1`. Node needs the glob:
  `node --test web/test/*.test.js`. The failure looks like a broken test
  suite, not a bad invocation.
- **`webgl-smoke-test.sh` emits `ERROR:` lines and passes anyway.** dbus
  (`UPower ... not activatable`) and `SharedImageManager::ProduceSkia` noise
  are normal under headless Chrome. Check the trailing `... passed.` line.
- **`bwrap` cannot `--ro-bind` a path that reaches through a symlink into the
  read-only store.** Both drivers `readlink -f` their basedir and engine for
  this. Symptom if you write your own: `Can't mkdir parents for <path>: No
  such file or directory`, naming a path that plainly exists. (`headless-drive.sh`
  used `cd && pwd` for its basedir and hit exactly this on
  `result-demodata`; fixed 2026-08-29.)
- **The console cannot be opened from the main menu.** Escape reopens the menu,
  a menu eats the grave key, and with nothing connected the engine puts the
  menu straight back. `key_dest` must be `key_game`, so `cmd`/`console` steps
  need `+map <something>`. A run that ignores this screenshots an untouched
  menu N times and looks exactly like a pass.
- **h2ded is silent on a non-tty stdout.** Everything is in `qconsole.log`
  under the sandboxed `$HOME` (both drivers copy it out for you). `-condebug`
  is already passed by `serve.sh`.
- **Piping several commands into h2ded used to concatenate them** —
  `map demo1\nstatus\nquit\n` ran as the single command `map demo1statusquit`,
  and only surfaced because that map name happened not to exist. Fixed
  2026-08-29 in `engine/hexen2/server/host.c` (`Cbuf_AddText` needs an explicit
  `"\n"`; `Sys_ConsoleInput` strips it, and the drain loop queues every buffered
  line before the frame's `Cbuf_Execute`). Rebuild `.#h2ded-bundled` if a run
  reports a mangled command.
- **`engine/hexen2/host.c` and `host_cmd.c` are NOT in the dedicated build.**
  `engine/CMakeLists.txt:886` `REMOVE_ITEM`s them (along with `host_async.c`,
  `net_loop.c` and `common/wad.c`) and substitutes
  `engine/hexen2/server/host.c` / `server/host_cmd.c`. Patch the wrong copy and
  the build succeeds, the store path changes, and the behaviour does not.
- **Hexen II's `default.cfg` has no WASD bindings.** Movement steps need
  `cmd bind w +forward` first, or `hold w` does nothing.
- **llvmpipe is the renderer here** — the engine logs `GL_RENDERER: llvmpipe
  (LLVM 21.1.8, 256 bits)`, GL 4.6 / GLSL 4.60, enough for the engine's GL 4.3
  requirement. It is correct but slow, and it is the wrong tool for judging a
  vendor-specific rendering bug; a RenderDoc capture from real hardware beats a
  speculative build.
- **`tools/` is deliberately almost entirely outside the flake's `filteredSrc`
  allowlist** (only `qcdis.py` and `check_progs_fields.py` are in), so editing
  `headless-drive.sh` or `serve.sh` does not invalidate anyone's cached build.
  Keep it that way; anything that must run during a build belongs in `scripts/`.
- **`nix build` without `-o`/`--no-link` repoints `./result`.** The GUI lane
  defaults to `ENGINE=./result/bin/glhexen2`, so building any other output
  (`.#gamecode`, `.#h2ded`, …) bare will silently leave the driver pointing at
  a derivation that has no `bin/glhexen2`. Always pass `-o <name>` or
  `--no-link`.
- Both drivers `rm -rf` their outdir on start and bind a throwaway `$HOME` over
  the real one, so a run cannot touch `~/.hexen2`.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `MODULE_NOT_FOUND` from `node --test` | You passed the directory. Use `web/test/*.test.js`. |
| `WebGL2 smoke test requires Chrome or Chromium` | Not inside the `nix shell nixpkgs#chromium nixpkgs#nodejs` wrapper. |
| `Can't mkdir parents for …` | bwrap through an unresolved symlink — `readlink -f` the path. |
| `engine exited early; see …/engine.stdout` | Read that file. Usually a bad basedir or a bwrap bind, not the engine. |
| `no window after 90s` | Xvfb or GL failed to come up; `xvfb.log` in the outdir. |
| `Couldn't spawn server maps/<garbage>.bsp` | Concatenated console commands — stale `h2ded`, rebuild it. |
| `headless-drive: missing required tool: Xvfb` | Not inside the `nix shell` line above. |
| `serve` reports `TIMED OUT` | The server never saw `quit`; check `qconsole.log` for `stdin at EOF`. |
| Screenshots all show the same menu | Input never landed — check `key_dest`, and that step verbs are spelled right. |
| `WARNING: 640x480 not found in fullscreen modes` | Harmless under Xvfb; the engine falls back and runs windowed. |
