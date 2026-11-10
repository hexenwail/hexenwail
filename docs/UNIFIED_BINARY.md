# Unified `hexenwail` binary — design decision

Design record for hexenwail/hexenwail **#54** ("Investigate unified
hexenwail binary"). Written against master `346edd553` (2026-11-09).
Answers the issue's first acceptance criterion — a document covering
feasibility, mode selection, architecture, and trade-offs — and sets up
its third (if separate binaries must remain, document why, and the
recommended user-facing model). It decides nothing: the decisions in §6
belong to the maintainer.

## 0. Two corrections to the record

Read these first — both change what the investigation concludes.

### 0.1 There is no `h2ed` editor binary

The issue's third "Consider" bullet asks about "dedicated server and
editor functionality currently provided by `h2ed`". A repo-wide grep for
`h2ed` returns **zero hits anywhere outside the `h2ded` dedicated-server
name** (no source, no CMake, no flake output, no docs, no script). `h2ed`
is a typo for `h2ded`, or a confusion with a uHexen2-era name. And **no
editor exists in this tree at all**: the closest things are the
command-line map toolchain in `utils/` (`qbsp`, `light`, `vis`, `hcc`,
`pak`, `genmodel`, …) — separate small executables, not a GUI editor, not
built by `engine/CMakeLists.txt`, and not something a unified client
binary could absorb without writing an editor from scratch. (Mapping
workflow already has a `-map <name>` client flag that bypasses menus for
TrenchBroom use; see the `Host_Init` comment in `engine/hexen2/host.c`.)

So one of the four binaries the issue names does not exist. The "editor"
leg of this investigation is a one-paragraph dismissal, not a work item,
and leg 4 in the §3 table is marked not applicable.

### 0.2 Issue #340 exists — the renderer leg is gated on implementation, not filing

The `r_renderer` menu code (`engine/hexen2/menu.c`) says its cycling case
is "Dead code until #340 lands a second backend", and round-3 scoping of
this issue recorded #340 as returning 404 from the GitHub API. That
check was wrong: **#340 exists and is open** — "Second render backend
via SDL_GPU — (subtask): implement the SDL_GPU backend behind the RHI
seam", filed 2026-10-11 by bobberb, 0 comments, subtask of umbrella issue
#335. Its body: "With pipeline objects, uniform blocks,
descriptor-shaped texture binding and offline SPIR-V in place, add the
actual SDL_GPU backend as a second implementation behind the same seam."

So the renderer-unification leg is gated on #340 being *implemented*, not
on it being filed. The backend still does not exist — there is no branch
and no code for it — but the target is scoped and filed. One wrinkle:
#340's body cites "uhexen2-p4ln.5 in docs/BACKLOG.md", but
`docs/BACKLOG.md` at this commit contains no mention of `p4ln`, `SDL_GPU`,
or a second backend (that file is the retired beads tracker; anything
actively worked on lives in GitHub Issues instead). The umbrella lives in
issue #335, not in the BACKLOG file.

## 1. What actually exists (binary inventory, post-#53)

| Binary | What it is | Built by | Default? |
|---|---|---|---|
| `hexenwail` | Hexen II client, GL 4.3 (or GLES 3.0 with `-DUSE_GLES=ON`); HexenWorld *client* networking compiled in (`USE_HEXENWORLD_CLIENT=ON`, `H2W_INTEGRATED`) | `engine/CMakeLists.txt:1150–1230`, one `add_executable(${EXECUTABLE_NAME} …)` per platform branch (Emscripten / WIN32 / Unix) | Yes — flake `default = nixos-bundled` (`flake.nix:425`); `mainProgram = "hexenwail"` (`flake.nix:282`) |
| `h2ded` | Hexen II dedicated server: no renderer, video, sound, input. `SERVERONLY` define; own `hexen2/server/host.c`, `host_cmd.c`, `sys_{unix,win}.c`; links only libm/libc on Unix | `engine/CMakeLists.txt:1383–1438`, gated on `BUILD_DEDICATED` (default OFF) | No — opt-in flake outputs `.#h2ded`, `.#h2ded-bundled`, `.#h2ded-win64` |
| `hwsv` | HexenWorld dedicated server: Raven's QuakeWorld-derived fork — own protocol, own gamecode (`hwprogs.dat`), client prediction, master server. "A SEPARATE engine that happens to share h2shared with Hexen II — not a mode of the Hexen II server" (`engine/CMakeLists.txt:1443–1452`). Own source list with `-DH2W -DSERVERONLY`; `hexenworld/shared` shadows `h2shared` on the include path | `engine/CMakeLists.txt:1443–1526`, gated on `BUILD_HEXENWORLD` (default OFF) | No — opt-in `.#hwsv`, `.#hwsv-bundled` |
| `hexenwail` (WASM) | Same client as `hexenwail.html`/`.wasm`; renderer chosen at configure time by `WEB_RENDERER` (`webgl2` default, `software` opt-in) | Emscripten branch, `engine/CMakeLists.txt:1150–1221`; flake `.#wasm` (`flake.nix:1224`) | — |
| `hexenwail-gles` | Desktop client on the ES 3.0 tier (`-DUSE_GLES=ON`) | flake `.#gles` (`flake.nix:432`) | No |
| `hexenwail-win64`, `h2ded-win64`, `utils-win64` | MinGW cross builds | flake `.#win64` (`flake.nix:978`), `.#h2ded-win64` (`flake.nix:1146`), `.#utils-win64` (`flake.nix:1194`) | — |
| `hexenwail-utils` | Map/tool programs (`utils/`: `qbsp`, `light`, `vis`, `hcc`, `pak`, …) | flake `.#utils` (`flake.nix:529`) | — |

Post-#53 rename (`build: rename the Unix client binary to hexenwail (#53)`,
`1df954254` at time of writing): the Unix client is now
`hexenwail`, matching Windows (`hexenwail.exe`) and WASM
(`hexenwail.html`). The servers (`h2ded`, `hwsv`), utils tool names,
docs/release-notes and history were **untouched by design** — #53 was a
rename, not a unification, and #54 is the still-open follow-up for the
actual unification question.

## 2. The four legs, measured

Decomposed, #54 is four independent unifications with very different
sizes. They must not be estimated as one item.

### 2.1 Leg 1 — `h2ded` → `hexenwail -dedicated` (closest to feasible)

The client **already has a dedicated-server mode**: `hexenwail
-dedicated [N]` / `-listen [N]` exist (`engine/hexen2/host.c:424–460`,
`Host_FindMaxClients`; `cls.state = ca_dedicated`). In that mode
`Host_Init` skips the whole client stack — `V_Init`, `Key_Init`,
`Con_Init`, `M_Init`, `VID_Init`, `Draw_Init`, `SCR_Init`, `R_Init`,
`S_Init`, `CDAudio_Init`, `MIDI_Init`, `BGM_Init`, `CL_Init`, `IN_Init`
are all inside the `cls.state != ca_dedicated` arm
(`host.c:1553–1594`); shutdown likewise skips them (`host.c:1683`);
stdin console commands are read only when dedicated (`host.c:934`); the
main loop takes a dedicated branch paced by `sys_ticrate`
(`engine/h2shared/sys_sdl.c`, `Sys_MainFrame`).

**But there is exactly one concrete code gap:** both Unix and Windows
`main()` call `Sys_SDLInit()` unconditionally
(`engine/hexen2/sys_unix.c:797–810`, `engine/hexen2/sys_win.c:613`), and
`Sys_SDLInit` unconditionally does `SDL_Init(SDL_INIT_VIDEO |
SDL_INIT_AUDIO | SDL_INIT_EVENTS)`
(`engine/h2shared/sys_sdl.c:122–127`). So `hexenwail -dedicated` still
requires the SDL video subsystem to initialise — on a headless Linux box
that fails without a dummy video driver, where `h2ded` (SDL-free, links
only libm/libc per the flake comment at `flake.nix:442`) just runs. This
is the single gap between "the client can already be a dedicated server"
and "the client can *replace* h2ded".

A second, subtler duplication: the client-dedicated path and `h2ded`
are **two parallel server implementations**. The client uses
`hexen2/host.c` with `ca_dedicated` branches; `h2ded` drops `host.c` /
`host_cmd.c` / `host_async.c` and substitutes `hexen2/server/host.c` +
`host_cmd.c` + `server/sys_{unix,win}.c`
(`engine/CMakeLists.txt:1396–1422`). Any unification must pick one host
implementation and delete the other — a behavioural decision (e.g. the
two disagree on heap sizing: dedicated takes the client's size per #79,
see `sys_unix.c:799` vs `server/sys_unix.c`), not a mechanical merge.

The flake comment at `flake.nix:442–445` is also a constraint on
deletion: `h2ded` is a separate output *so CI can build it on every
push*, because "nothing else compiles the SERVERONLY half of the shared
sources, so without a gate it silently rots." Deleting the output means
finding another gate for that half, or accepting the rot risk.

**Verdict: feasible in two steps.** (a) Make `hexenwail -dedicated`
truly headless (skip the video/audio SDL init in dedicated mode — the
dedicated loop only needs `SDL_Delay`, i.e. the timer; verify what SDL3
guarantees without `SDL_Init`). (b) Only after (a), and only on a
maintainer ruling, delete the `h2ded` target and its flake outputs. Step
(a) is a separable, verifiable change; step (b) is a product decision.

### 2.2 Leg 2 — software + GL in one desktop binary (blocked on #340)

**Desktop has exactly one renderer, by construction.** GL sources
(`GL_RENDERER_SOURCES`, `engine/CMakeLists.txt:528`) vs the classic 8bpp
software rasterizer (`SOFT_RENDERER_SOURCES`, `:574`) are selected by
`if(EMSCRIPTEN AND WEB_RENDERER STREQUAL "software")` (`:1110–1114`) —
the software rasterizer **cannot be selected on a desktop build at all**;
`WEB_RENDERER` is Emscripten-only and a non-`webgl2` value on desktop is
rejected with a warning and reset (`:89–95`).

**Renderer switching exists as a contract, not a capability.** #103
(landed as `menu: register the r_renderer backend contract, row hidden
until a second backend exists (#103)`, `e7fac6220` at time of writing)
Rendering-menu row that cycles `available_renderers[]`
(`engine/hexen2/menu.c:3586–3596`, `3845–3864`). The table has **one
entry on every build** (`"software"` / `"gles"` / `"opengl"`, selected by
`#if defined(WEBSOFT)` / `USE_GLES` / default), the row is hidden while
the count is 1 (`menu.c:3653`, `:4101`), and the code comment says it is
"Dead code until #340 lands a second backend". The cvar itself is only
registered in the web software build
(`engine/hexen2/r_soft_web.c:95,145`).

**What "coexist" would actually require:** both renderer source sets
compiled into one binary behind the `r_renderer` contract, with
`VID_Init`/`R_Init` dispatching at startup (the menu comment already
notes `vid_restart` cannot change the renderer — it needs quit +
relaunch), plus resolving the `GLQUAKE`/`WEBQUAKE`/`WEBSOFT`
compile-time defines that currently select whole header sets
(`quakeinc.h` keys off them; `engine/CMakeLists.txt:174` notes they
"belong to the client alone"). That is a large refactor of
renderer-facing code, not a CMake flag flip. The software renderer also
has no desktop video backend today (its presentation path is a WebGL2
palette-LUT blit, `docs/web/SOFTWARE_RENDERER.md`).

**Verdict: no-go until #340 lands.** #340 exists and is filed (see
§0.2), but the backend itself does not. There is nothing to unify the
software renderer *with* on desktop until then, and the work cannot be
verified here (no GPU).

### 2.3 Leg 3 — `hwsv` → HexenWorld server mode (separate engine by design)

**Client side — done.** `USE_HEXENWORLD_CLIENT` is ON by default;
`hexen2/cl_hw.c` + `hexenworld/shared/pmove.c`/`pmovetst.c` compile into
the client under `H2W_INTEGRATED`, with the transport/channel ported in
Rust (`engine/CMakeLists.txt:1116–1138`). The CMake comment is explicit:
"a standalone hwcl target is intentionally not created." The remaining
client work is menu integration (`docs/BACKLOG.md:133`,
`uhexen2-6g8q.11`: server browser / connect dialog / player setup inside
the modern menu), not binary unification.

**Server side — separate engine, not a mode.** `hwsv` compiles
HexenWorld's own `server/sv_*.c` + `world.c` with `-DH2W -DSERVERONLY`,
with `hexenworld/shared` shadowing `h2shared` so its `quakedef.h` /
`protocol.h` / `progdefs.h` win (`engine/CMakeLists.txt:1443–1526`). Its
protocol, gamecode (`hwprogs.dat`, needs official `hw/pak4.pak` at
runtime), master server and prediction are disjoint from Hexen II's
`qsocket` server. Folding it into `hexenwail` would mean compiling two
engines with conflicting `quakedef.h` definitions into one process and
selecting between them at startup — the highest-risk leg — and it cuts
directly across the in-flight BACKLOG work `uhexen2-6g8q`, which
deliberately lands the `hwsv` port *before* the client
(`docs/BACKLOG.md:139`, `uhexen2-6g8q.4`: "This lands before the client
on purpose"). Unifying the server now would churn code that work is
actively porting.

**Mode selection if it ever happens:** the natural shape is already
visible — `-dedicated` selects Hexen II server mode in the client today;
a HexenWorld server mode would need an additional selector (e.g.
`-hexenworld -dedicated` or a `-mode` flag) plus a `GAME_HEXENWORLD`
gamedir gate (`FS_Init` already has one). No such flag exists today.

**Verdict: no-go now.** Needs a maintainer ruling on the whole
HexenWorld direction, and must wait for `uhexen2-6g8q` to land.

### 2.4 Leg 4 — `h2ed` editor → unified binary (not applicable)

See §0.1. No editor exists; nothing to unify.

## 3. Feasibility table

| # | Leg | Concrete shape | Size if attempted | Feasibility |
|---|---|---|---|---|
| 1 | `h2ded` → `hexenwail -dedicated` | Client gains a true headless dedicated mode, then `h2ded` target + `server/host.c` + `server/sys_*.c` deleted; flake `.#h2ded*` outputs removed or turned into aliases | M (headless fix) + M (deletion + packaging + migration) | Headless fix: go. Deletion: needs maintainer ruling |
| 2 | Software + GL in one desktop binary | Both renderer source sets in one binary; `r_renderer` selects at startup (quit + relaunch); `GLQUAKE`/`WEBQUAKE`/`WEBSOFT` defines de-compile-timed; desktop video backend for the software rasterizer written | L | No-go: gated on #340 landing a second backend and a renderer refactor; cannot be verified here (no GPU) |
| 3 | `hwsv` → HexenWorld server mode | HexenWorld server sources compiled into the client behind a mode flag; resolve the `H2W` header shadowing so two `quakedef.h` worlds coexist in one process | L | No-go: separate engine by design; cuts across in-flight `uhexen2-6g8q` port work; needs maintainer ruling on the whole HexenWorld direction |
| 4 | `h2ed` editor → unified binary | — | — | Not applicable: no editor exists (§0.1) |

## 4. Recommended user-facing model (for now)

Keep `hexenwail` (client), `h2ded`, and `hwsv` as separate binaries.
Document `hexenwail -dedicated` as the already-existing unified
*Hexen II* path, with its one headless gap (§2.1) named as the next step.
This is the issue's acceptance criterion (3): separate binaries remain,
for the reasons above, and the recommended model is the current one plus
the headless fix.

What that means concretely for users today:

- Players run `hexenwail`. Nothing changes.
- Hexen II server operators run `h2ded` (or `hexenwail -dedicated` where
  a display/SDL video init is available). After the headless fix, the two
  are interchangeable for serving; `h2ded` stays as the minimal,
  SDL-free option.
- HexenWorld server operators run `hwsv`. Nothing changes.
- The WASM output stays a client; servers are excluded on Emscripten by
  design (see §5).

## 5. Mode selection, platforms, and trade-offs

### Mode selection (as it exists, and as a future unification would extend it)

- `-dedicated [N]` / `-listen [N]` — exist today; select Hexen II server
  mode in the client (`engine/hexen2/host.c:424–460`).
- `r_renderer` — contract exists, one backend per binary; cycles the
  compiled-in backends once #340 lands a second one; quit + relaunch to
  switch (`engine/hexen2/menu.c:3586–3596`, `3845–3864`).
- A future HexenWorld server mode would need a new selector (e.g.
  `-hexenworld -dedicated` or a `-mode` flag) plus a `GAME_HEXENWORLD`
  gamedir gate. No such flag exists today.
- The runtime-selectable shape the issue asks about already exists in
  miniature. The question is whether to extend that shape, not invent it.

### Platforms

- **Windows:** the client is a `WIN32` (windowed, no console) executable
  (`engine/CMakeLists.txt:1227`); `h2ded` is deliberately *not* `WIN32` —
  a console program with a plain `main()` whose output is the server
  console, allocating its own console when `-dedicated` (`:1424–1426`;
  `engine/hexen2/sys_win.c:572–590`). A unified Windows binary must
  decide console-vs-windowed at startup: either always allocate a console
  (changes the player experience), or stay `WIN32` and `AllocConsole()`
  only in server mode (the `sys_win.c` code already does the latter half).
  This is a small but real design point (§6.4); it cannot be verified in
  this container (no Windows toolchain run here — MinGW cross builds are
  flake outputs `.#win64` / `.#h2ded-win64`).
- **WebAssembly:** servers are excluded by design — `BUILD_DEDICATED` and
  `BUILD_HEXENWORLD` warn and skip on Emscripten
  (`engine/CMakeLists.txt:1393`, `:1457`); the UDP transport cannot run in
  a browser. Unification must therefore be **native-only**; the WASM
  output stays a client, and its `WEB_RENDERER` compile-time choice is
  untouched.
- **Packaging surface a unification would touch:** flake outputs
  (`default`/`nixos-bundled`, `gles`, `h2ded`, `h2ded-bundled`, `hwsv`,
  `hwsv-bundled`, `win64`, `h2ded-win64`, `utils`, `wasm`), AppImage
  (`appimage/AppRun`), Flatpak, `README.md:55,212–213` (which documents
  `.#h2ded` / `.#hwsv` as supported), `docs/COMPILE`, `USAGE.md`, and the
  release workflow. Any removal of a binary is a user-facing breaking
  change needing a migration note (the issue's acceptance criterion 2
  explicitly asks for one). Note: the `checks.nixos` install-check output
  from the round-1 #481 patch is **not** in the tree — #481 did not land
  upstream — so it is not part of the packaging surface today.

### Trade-offs

- **Binary size (unquantified — measurement left open).** A unified
  binary would link SDL3 + GL + all audio codecs + both server stacks
  into every install, including headless servers that today link only
  libm/libc (`flake.nix:442–467` comment). No sizes were measured (no
  builds run here — and no build is possible in this container). State it
  as an open measurement (compare `nix build .#h2ded` vs a unified binary
  once one exists) rather than asserting a number.
- **Headless-server minimal dependencies vs one fat binary.** The
  current split lets a server install carry no SDL, no GL, no codecs.
  Unification trades that away for one artifact to ship. Whether that
  trade is acceptable is a maintainer call (§6.1, §6.5), not a technical
  finding.
- **Verification cost.** Any unified binary needs: a native build, a
  Windows cross build (`#win64`), a WASM build (`#wasm`), a GPU run to
  prove the client still renders, and a headless server run to prove the
  server modes still serve. None of those verifications is possible
  offline in this container. A patch that deletes binaries without those
  gates would be unreviewable.

## 6. Sequenced follow-ups

1. **Headless `-dedicated` (M, go — separate follow-up, not part of
   this doc).** In `sys_unix.c` / `sys_win.c` `main()`, skip
   `Sys_SDLInit(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_EVENTS)` when
   `isDedicated` is set (the dedicated loop only needs `SDL_Delay`, i.e.
   the timer; verify what SDL3 guarantees without `SDL_Init`), or
   initialise a video-less subset. Keep `h2ded` intact. Verify headless
   with `tools/headless-drive.sh` / `tools/headless-cfg.sh` (no display,
   no GPU). This is the only code change in #54's orbit that is small,
   safe, and verifiable in this container — but it is a precondition, not
   the unification itself, and it should land as its own issue/PR, not
   smuggled inside the doc.
2. **#340 (SDL_GPU second backend) before any renderer unification.**
   #340 is filed and open (see §0.2); the backend still has to be built.
   Until it lands, leg 2 has no target.
3. **Defer the `hwsv` decision until `uhexen2-6g8q` lands.** The
   HexenWorld port is deliberately landing the server first; do not churn
   it.

## 7. Decisions needed from the maintainer (before any implementation)

1. **Should `h2ded` and/or `hwsv` ever be deleted?** If the answer is
   "no — minimal headless servers are a feature" (the flake's current
   stance), then #54 resolves to this doc alone (criterion 3) and
   implementation work is just the headless fix in §6.1. If "yes", which
   release, and is a compatibility alias / migration period required?
2. **Is a HexenWorld server mode in the client even desired?** Or does
   `hwsv` stay a separate engine permanently, with unification limited to
   the already-done HexenWorld *client* integration?
3. **Renderer direction:** is the planned second backend SDL_GPU (#340,
   filed and open — the backend itself is still to be built)? Without
   that landing, the software+GL leg has no target.
4. **Windows console model** for a unified binary (always console vs
   `AllocConsole()` in server mode only).
5. **Binary-size budget**, if any: is linking SDL3/GL/codecs into every
   headless server install acceptable, or is that alone disqualifying?

## 8. Evidence log

- Issue body + metadata: `GET
  https://api.github.com/repos/hexenwail/hexenwail/issues/54` (open, 0
  comments). Issue #340: same API returns 200 — open, created
  2026-10-11, "Second render backend via SDL_GPU — (subtask): implement
  the SDL_GPU backend behind the RHI seam", subtask of umbrella #335, 0
  comments. (An earlier scoping check recorded a 404 for #340; re-checked
  2026-11-09, it exists.)
- `git rev-parse HEAD` → `346edd553c7d35ca620aae36d68ac3ac1e06a72e`.
- `h2ed` grep: zero hits repo-wide (excluding the `h2ded` name itself;
  132 files mention `h2ded`).
- Client target: `engine/CMakeLists.txt:1150–1230`
  (`add_executable(${EXECUTABLE_NAME} …)` at :1151, :1227, :1230;
  `EXECUTABLE_NAME` set to `hexenwail` at :1150, :1226, :1229); renderer
  source selection `:528`, `:574`, `:1110–1114`; `WEB_RENDERER` `:83–95`;
  `BUILD_DEDICATED` `:55`, `BUILD_HEXENWORLD` `:56`,
  `USE_HEXENWORLD_CLIENT` `:57`; `h2ded` `:1383–1438`
  (`add_executable(h2ded …)` :1426); `hwsv` `:1443–1526`
  (`add_executable(hwsv …)` :1507).
- Dedicated mode: `engine/hexen2/host.c:424–460`
  (`-dedicated`/`-listen`), `:1553–1594` (client stack skipped), `:934`
  (stdin console); `engine/hexen2/sys_unix.c:795` (`isDedicated`),
  `:797–810` (unconditional `Sys_SDLInit`); `engine/h2shared/sys_sdl.c:122–127`
  (`SDL_INIT_VIDEO|AUDIO|EVENTS`); `engine/hexen2/sys_win.c:572–590`
  (`AllocConsole` in dedicated mode), `:613` (`Sys_SDLInit`).
- Renderer contract: `engine/hexen2/menu.c:3586–3596` (one-entry table),
  `:3653`, `:3845–3864`, `:4101`; `r_renderer` cvar only in
  `engine/hexen2/r_soft_web.c:95,145`; #103 commit `e7fac6220`.
- HexenWorld client integration: `engine/CMakeLists.txt:1116–1138`;
  BACKLOG `uhexen2-6g8q` lines 112–113, 132–139 (`.4` at :139: "This
  lands before the client on purpose").
- Flake outputs (read from text only — no `nix eval` was possible here):
  `flake.nix` — client `mainProgram` `:282`, `default =
  self.packages.${system}.nixos-bundled` `:425`, `gles` `:432`, `h2ded`
  `:446–475` ("headless, links only libm/libc"), `hwsv` `:485–518`,
  `utils` `:529`, `win64` `:978`, `h2ded-win64` `:1146`, `utils-win64`
  `:1194`, `wasm` `:1224–1299`. No `checks.` output and no
  `doInstallCheck` in `flake.nix` at this commit — the #481 install
  check did not land upstream.
- #53 rename: `build: rename the Unix client binary to hexenwail (#53)`
  (`1df954254` at time of writing); servers/utils explicitly untouched.
- README server table: `README.md:55`, `:212–213`.
- BACKLOG has no `p4ln` / `SDL_GPU` / "second backend" mention, despite
  #340's body citing "uhexen2-p4ln.5 in docs/BACKLOG.md" — the umbrella
  lives in issue #335.
