<!--
Thanks for the patch. Please read CONTRIBUTING.md if you have not already:
https://github.com/hexenwail/hexenwail/blob/master/CONTRIBUTING.md

Keep this template's headings; delete any section that genuinely does not apply.
-->

## What this changes

<!-- One or two sentences. The diff says what; use this space for why. -->

## Why

<!--
The problem being solved, and why this approach over the obvious alternative.
If it diverges from how Ironwail solves the same problem, say so and why —
Ironwail parity is the project's default.
-->

Fixes #

## How it was tested

<!--
"Builds clean" is not testing for anything that reaches the renderer, the HUD,
audio, input, or asset loading. Say what you actually ran.
-->

- **OS:**
- **GPU / driver:** <!-- from the GL_RENDERER / GL_VERSION lines in qconsole.log -->
- **Game data:** <!-- retail / Portal of Praevus / demo -->
- **Tested with:** <!-- base game, and which mod if the change can affect mods -->

<!-- Before/after screenshots are effectively required for anything visual. -->

## Checklist

- [ ] Builds locally (`nix build`, or CMake if you are not on Nix)
- [ ] Ran the game and reached the affected feature — not just compiled it
- [ ] Tested at least one mod, if this touches asset loading, the HUD, gamecode or the renderer
- [ ] No legacy OpenGL introduced — no immediate mode, fixed-function state, or hardware matrix stack
- [ ] Uses `__EMSCRIPTEN__`, not bare `EMSCRIPTEN`, in any new C guards (CI enforces this)
- [ ] Ran `shellcheck` if `scripts/mkrelease.sh` or `flatpak/hexenwail.sh` changed
- [ ] Added or updated a cvar's menu entry, if this adds a cvar users should be able to reach
- [ ] Updated README / USAGE / docs if behaviour or setup changed
- [ ] Unrelated cleanup is in separate commits, so a regression can be bisected

<!--
CI will build seven targets, not just the desktop client: .#nixos, .#h2ded,
.#win64, .#h2ded-win64, .#utils, .#utils-win64, and the Flatpak bundle. Shared
code that compiles for the client can still break the dedicated server or the
map/model tools, which link no SDL, GL or codecs.
-->
