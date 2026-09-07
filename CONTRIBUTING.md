# Contributing to Hexenwail

Bug reports, mod compatibility findings, code, and documentation are all welcome.
This is a hobby project — there is no CLA, no formal review board, and no obligation
to justify why you want something. A clear report and a reproducible test case are
worth as much as a patch.

## Reporting a bug

Use the [issue forms](https://github.com/hexenwail/hexenwail/issues/new/choose).
They ask for engine version, install method, OS, GPU/driver and your `qconsole.log`
because, in practice, a report missing those cannot be acted on — most rendering
bugs are driver- or GPU-specific, and the log already contains the exact driver
build. The log is written automatically on every run; you do not need to enable it.

If you are unsure whether something is a bug, ask in the
[Hexen II Discord](https://discord.gg/nC848XavDQ) first.

## The guiding principle

**Where Ironwail has already solved a problem for Quake, Hexenwail prefers its
approach for Hexen II.** [Ironwail](https://github.com/andrei-drexler/ironwail) has
had far more eyes on it than this fork ever will, so matching its cvar names,
defaults and general shape means users carry knowledge over and we inherit its
bug fixes instead of reinventing them. A patch that diverges from Ironwail is
fine, but say why in the PR.

The second principle is **mod compatibility**. The existing Hexen II mod catalogue
was built against uHexen2 and Shanjaq's fork. A change that improves the base game
but breaks a shipped mod is a regression.

## Building

Full instructions are in [docs/COMPILE](docs/COMPILE).
[`docs/INDEX.md`](docs/INDEX.md) says what every other document in `docs/` is
for, and which ones are contracts that code depends on.

```bash
nix build              # NixOS
nix build .#linux-fhs  # portable Linux binary
nix build .#win64      # Windows 64-bit, cross-compiled
```

Without Nix:

```bash
cd engine && mkdir build && cd build
cmake .. && make -j$(nproc)
```

Requires OpenGL 4.3 (2012+), SDL3, libvorbis, libogg, libopus, opusfile, libxmp.
ALSA and FluidSynth are optional; without FluidSynth (or without a soundfont)
there is no MIDI music on Linux.

Hexenwail ships no game assets. You need `data1/pak0.pak` and `data1/pak1.pak`
from a retail copy to run anything.

## What CI will check

Every PR against `master` runs [`.github/workflows/ci.yml`](.github/workflows/ci.yml).
Running the equivalent locally before you push saves a round trip:

- **Eight build targets**, not just the one you tested:
  `.#nixos`, `.#h2ded` (dedicated server), `.#hwsv` (HexenWorld server),
  `.#win64`, `.#h2ded-win64`, `.#utils` (map/model toolchain), `.#utils-win64`
  and `.#gamecode` — plus the Flatpak bundle in its own job. The server and
  tools targets exist because they link no SDL, GL or codecs and therefore rot
  silently when someone edits shared code; `.#hwsv` is there because
  `engine/h2shared/` is compiled by HexenWorld too, and a change that is right
  for Hexen II can fail to build for it (see issue #42).
- **The PWA / WebAssembly job**, which builds the WebAssembly client and runs
  `scripts/wasm-validate-artifact.sh` over the assembled artifact. Native
  targets cannot catch a browser regression, which is the entire reason this
  job exists.
- **`shellcheck`** on `scripts/mkrelease.sh`, `flatpak/hexenwail.sh` and the
  four `scripts/wasm-*.sh` / `scripts/webgl-smoke-test.sh` helpers.
- **AppStream metainfo validation** (`appstreamcli validate`), because that file
  is what GNOME Software and KDE Discover read.
- **Software-renderer menu parity** (`tools/menu_soft_parity.py`) and the
  **Ironwail scorecard** self-check (`tools/ironwail_scorecard.py --check`).
- **No bare `EMSCRIPTEN` in C sources.** Use `__EMSCRIPTEN__`. `emscripten.h`
  defines a bare `EMSCRIPTEN` alias under a deprecation pragma, so a guard spelled
  that way only works in translation units that happen to include the header — it
  fails silently rather than loudly. (The `if(EMSCRIPTEN)` uses in
  `engine/CMakeLists.txt` are CMake variables and are fine.)
- A headless **smoke test** that launches the built binary.

## Code style

There is no `.clang-format` or `.editorconfig`, and that is deliberate — this tree
carries 25 years of history from Raven, Anvil of Thyrion, Hammer of Thyrion and
uHexen2, and a blanket reformat would destroy the ability to diff against upstream.

**Match the surrounding code.** Tabs for indentation, the brace style already in the
file, and the naming conventions of the subsystem you are editing. If a file is
inherited upstream code, keep your diff minimal and surgical so it can still be
compared against `sezero/uhexen2`.

Three hard rules:

- **No legacy OpenGL.** The renderer is GL 4.3 core: no immediate mode
  (`glBegin`/`glEnd`), no fixed-function state, no hardware matrix stack
  (`glPushMatrix`, `glLoadIdentity`, …). The matrix stack is implemented in
  software and matrices are passed to shaders as uniforms.
- **C11, no compiler-specific extensions** beyond what the tree already relies on.
  The same sources must build under GCC, Clang and mingw-w64.
- **Shared client code must clear all three renderer pathways.** They are
  desktop GL 4.3 (`GLQUAKE` + `GL_DLSYM`), GLES3 / WebGL2 (`USE_GLES`, which a
  desktop `-DUSE_GLES=ON` build also takes) and the restored 8bpp software
  rasterizer (`WEBSOFT`). Shared code references renderer cvars by symbol, so
  the software pathway's failure mode is a **link error** rather than a visual
  one — it will not show up in a desktop build. Likewise `engine/h2shared/` is
  compiled by both Hexen II and HexenWorld; a Hexen II-only field or builtin
  added there needs an `#ifndef H2W` fence (issue #42 has the worked examples).

## Commit messages

Conventional-commit style, which is what the history uses:

```
fix(gamecode): stop destroying loot on a 'Bad backpack!' fallthrough
feat(menu): add a fractional HUD scale slider
docs(readme): fix three dead links
```

Common types here: `fix`, `feat`, `docs`, `chore`, `ci`, `build`.

Write the body to explain **why**, not what — the diff already says what. If the
change is subtle or the obvious alternative is wrong, say so; that comment is the
one future readers will need.

You will see a trailing `(uhexen2-xxxx)` on many commits. Those are ids from an
issue tracker the maintainers have since retired, kept because they are the thread
back to why a change was made. **You do not need one**, and you should not invent
one. There is no tracker left to look them up in; the lower-priority items that
were open when it was retired are listed in [`docs/BACKLOG.md`](docs/BACKLOG.md),
one line each, and everything active is a [GitHub issue](https://github.com/hexenwail/hexenwail/issues).

## Looking for something to work on

[GitHub Issues](https://github.com/hexenwail/hexenwail/issues) is the live list.
Beyond that, [`docs/BACKLOG.md`](docs/BACKLOG.md) holds 177 lower-priority items
carried over from the retired tracker — real work, just not queued. If you pick
one up, open a GitHub issue for it and delete the line.

## Pull requests

Target `master`. Before opening one:

- Say what you tested on: OS, GPU and driver version. "Builds clean" is not
  testing for a renderer change.
- Test the **base game and at least one mod** if you touched anything that mods
  can reach — asset loading, the HUD, gamecode, or the renderer.
- Include before/after screenshots for anything visual.
- Keep unrelated cleanup in separate commits so a regression can be bisected to
  the change that caused it.

## Licensing

Hexenwail is GPL-2.0-or-later. By contributing you agree your changes ship under
that license. Do not paste in code from engines with incompatible licensing, and
if you port code from another GPL engine (Ironwail, QuakeSpasm, FTE, vkQuake), say
so in the commit message so the provenance stays traceable.

Never contribute original Hexen II game assets. The engine is free software; the
game data is not, and this repository must stay clean of it.
