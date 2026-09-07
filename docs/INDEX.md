# What is in `docs/`

Two sets of files live here, in two different styles, for two different readers.

## Plain-text manuals (no extension)

The uHexen2-inherited user manual, kept in its original plain-text form so it
reads the same in a terminal as on the web.

| File | What it covers |
|---|---|
| [`README`](README) | The manual: features, installation, cvars, mods, known issues, links |
| [`COMPILE`](COMPILE) | Building from source — Nix, CMake, GLES, Windows cross-compile |
| [`README.music`](README.music) | CD audio, Ogg, MIDI and soundfont setup |
| [`AUTHORS`](AUTHORS) | Credits, back to Raven |
| [`COPYING`](COPYING) | GPL-2 |
| [`SrcNotes.txt`](SrcNotes.txt) | Notes for people modifying the engine sources |

## Markdown documents

Everything below is one of four kinds. The kind matters, because it tells you
whether you may edit it freely.

### Contracts — other things depend on these being true

| File | What it covers |
|---|---|
| [`MODDING_TICKRATE.md`](MODDING_TICKRATE.md) | `sv_physfps`, `frametime`, and how to write think chains that survive a tick-rate change. What a mod author is handed when their entity lags. |
| [`MODELPIMP.md`](MODELPIMP.md) | The `misc_modelpimp` reach contract — additive only, snapshot/restore per map |
| [`MODS_CORPUS.md`](MODS_CORPUS.md) | The mod corpus behind the Mods menu. **Cited by path from `engine/h2shared/quakefs.c`** — if you change the probe order here, change it there too. |
| [`GAMECODE.md`](GAMECODE.md) | Reference: what `.#gamecode` produces, search-path precedence, what ships |
| [`WEBGL_RENDERER.md`](WEBGL_RENDERER.md) | What the GLES3 / WebGL2 tier does and does not compile, and how to diagnose it |
| [`PWA.md`](PWA.md) | The web build: launcher, import, mods, saves, offline, iOS |

### Design records — history, not instructions

| File | What it covers |
|---|---|
| [`BUNDLED_GAMECODE.md`](BUNDLED_GAMECODE.md) | Why bundled gamecode works the way it does: findings, rejected designs, failure modes. Read "we recommend X" as "X is what shipped". Companion to `GAMECODE.md`. |

### Process

| File | What it covers |
|---|---|
| [`RELEASE_NOTES_STYLE.md`](RELEASE_NOTES_STYLE.md) | The voice release notes are written in |
| [`release-notes/`](release-notes/) | A copy of every tag body, so the next one has a local reference |
| [`DEMO_CONFIG.md`](DEMO_CONFIG.md) | Per-demo start/end configs and the playback bar |

### Tombstones — breadcrumbs, not live tickets

| File | What it covers |
|---|---|
| [`BACKLOG.md`](BACKLOG.md) | The 177 lower-priority items that were open when the **beads** tracker was retired, one line each, so `(uhexen2-xxxx)` ids still resolve to a sentence. There is no tracker to look them up in. Anything active is a [GitHub issue](https://github.com/hexenwail/hexenwail/issues). |
| [`RETIRED_TAGS.md`](RETIRED_TAGS.md) | Which tags were retired, why, and how to recover one from `refs/archive/` |

## Not here

- **Contributor process** — `CONTRIBUTING.md` at the repo root
- **Agent instructions** — `CLAUDE.md` and `AGENTS.md` at the repo root
- **Player setup** — `USAGE.md` at the repo root
- **HexenWorld** — `engine/hexenworld/README.md`
- **The software renderer** — currently undocumented. `engine/hexen2/r_soft_web.c`
  points at a `docs/web/SOFTWARE_RENDERER.md` that has never existed; it is
  `uhexen2-ibnq.3` in `BACKLOG.md`.
