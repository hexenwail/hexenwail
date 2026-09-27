# Rust port audit — baseline `origin/master` `f233524db`

Written 2026-09-27 for the goal "audit Rust-porting state, then revive the wad
slice as a PR-ready branch". Read-only: nothing in this document changed code.

## 0. The finding that changes the plan: the local checkout is 18 commits stale

The working checkout's `master` is `f49c07583` ("docs(rust): finish the huffman
overread wording"). `origin/master` is `f233524db`, 18 commits ahead:

```
$ git status -sb
## master...origin/master [behind 18]
$ git log --oneline -1 origin/master
f233524db fix(server): SV_CheckBottom samples the entity's own bbox, not a clip hull (#283)
```

Every appearance of "closed unmerged Rust work" in this checkout is an artifact
of that staleness. The five commits on `bobberb/rust-upstream-notices-25d430d1`
are not a lost branch: their PRs **merged upstream** (#243, #244, #247, #248;
#240's fix arrived in #252). Concretely, `wad.rs` is already on `origin/master`:

```
$ git ls-tree origin/master engine/rust/src/
... wad.rs
$ git ls-tree origin/master engine/rust/
... wad/ ... rust-toolchain.toml ... wasm_globals.c
```

`issue #247` and `issue #248` both report `MERGED`, and `origin/master`'s
`engine/CMakeLists.txt:862` reads "is linked into every engine target on every
platform. There is no C fallback". So the confirmed goal's implementation half
is **not** "port `wad.c` and land it" — that shipped. What remains is the
one thing that port's own review deliberately deferred: **issue #249**.

Lesson for the next audit: read by ref (`git show origin/master:...`), per the
"Reading Ironwail" rule in `AGENTS.md`, and check `git status -sb` before
concluding anything about what is or is not merged.

## 1. What is landed (all of it on `origin/master`, all mandatory)

Eleven ports, one Rust static library, no per-port C fallback in any engine
target. The C originals are compiled only by the differential harnesses.

| Port | Rust | C original | Gate | C in engine build? |
|---|---|---|---|---|
| hashindex | `engine/rust/hashindex/src/ffi.rs` | `engine/h2shared/hashindex.c` | `check-rust-hashindex.sh` | no |
| mathlib | `engine/rust/mathlib/src/ffi.rs` | `engine/h2shared/mathlib.c` | `check-rust-mathlib.sh` | no |
| sizebuf | `engine/rust/src/sizebuf.rs` | `engine/h2shared/sizebuf.c` | `check-rust-sizebuf.sh` | no |
| crc | `engine/rust/src/crc.rs` | `common/crc.c` | `check-rust-crc.sh` | no |
| link_ops | `engine/rust/src/link_ops.rs` | `engine/h2shared/link_ops.c` | `check-rust-link-ops.sh` | no |
| msg_io | `engine/rust/src/msg_io.rs` | `engine/h2shared/msg_io.c` | `check-rust-msg-io.sh` | no |
| info_str | `engine/rust/src/info_str.rs` | `engine/hexenworld/shared/info_str.c` | `check-rust-info-str.sh` | no |
| strlcpy | `engine/rust/src/strlcpy.rs` | `common/strlcpy.c` | `check-rust-strlcpy.sh` | no |
| strlcat | `engine/rust/src/strlcat.rs` | `common/strlcat.c` | `check-rust-strlcat.sh` | no |
| huffman | `engine/rust/src/huffman.rs` | `engine/hexenworld/shared/huffman.c` | `check-rust-huffman.sh` | no |
| wad | `engine/rust/src/wad.rs` | `engine/h2shared/wad.c` | `check-rust-wad.sh` | no |

Architecture as of `f233524db`:

- **One archive, one object.** `engine/rust` builds a single `staticlib`
  (`engine/rust/Cargo.toml`, `crate-type = ["staticlib"]`); Cargo features
  survive only so a harness can link one subsystem on its own.
  `set(RUST_ENGINE_FEATURES ...)` at `engine/CMakeLists.txt:869` lists all
  eleven. The old `-z,muldefs` workaround is gone — only its removal note
  remains in a comment (`engine/CMakeLists.txt:866`).
- **No rollback switches.** `USE_*_RS=OFF` now dies with an explanation
  (`engine/CMakeLists.txt:877-885`). This supersedes roadmap rule 2 in #61.
- **Every target.** Native, `win64`/`h2ded-win64` (cross rustc), and
  wasm32-unknown-emscripten. `engine/rust/rust-toolchain.toml` pins
  `channel = "1.97.1"`, and `scripts/check-rust.sh:56` fails the build when
  nixpkgs' rustc drifts from that pin.
- **One CI entry point.** `.github/workflows/ci.yml:174` runs
  `scripts/check-rust.sh`, which does toolchain parity, removed-switch
  rejection, one shared triple build, eleven port gates, `check-rust-abi.sh`
  (host struct layouts), and `check-rust-notices.sh` (+ self-test). The gate
  list is explicit at `scripts/check-rust.sh:86`.
- **wasm32 quirks are handled, not hidden.** `#[no_mangle]` statics become
  local symbols in a wasm32 staticlib, so `msg_readcount`, `msg_badread`,
  `vec3_origin` and `sincos_tab` live in `engine/rust/wasm_globals.c`.

## 2. Roadmap #61 has drifted and needs a decision, not more ports

#61 was written when two leaf ports existed and per-subsystem C fallbacks were
mandatory. Phase 0 (consolidated archive) and Phases 3–5 (sizebuf, msg_io,
info_str, huffman, wad) have all landed, and #248 removed the fallback switches
outright. That leaves #61's "Non-negotiable migration rules" partly describing
a build that no longer exists:

- rule 2 ("complete rollback during migration") — retired for these eleven
  subsystems by #248; the eventual "remove the fallback deliberately" step
  happened early and in bulk.
- the phase table's ordering is done through Phase 5; the next unstarted phase
  is **Phase 6 engine state infrastructure** (zone/hunk, cvar, cmd, quakefs).
- the "Near-term actions" and "Completion criteria" checklists are stale.

This is worth a comment and a rewrite of #61 rather than a new roadmap. It is
not blocking the #249 fix.

## 3. Open Rust issues

| Issue | State | Relevance |
|---|---|---|
| #249 | open | **The chosen slice.** `wad.rs` forms `*mut LumpInfoC` / `*mut QPicC` from unvalidated on-disk offsets and dereferences them through references; misalignment is UB in Rust where the C has only a caller precondition. `origin/master` still has this (`engine/rust/src/wad.rs:367,372,386,408,431`). |
| #233 | open | Single-object archive means every extern in the crate must resolve in every target. Still true, but its urgency dropped: Rust is now linked into all targets anyway, and the wasm32 route was solved with `wasm_globals.c`. Now a design note, not a blocker. |
| #60 | open, stale | "port mathlib.c (Phase 2)" — mathlib shipped in #56/#238-era work. Should be closed. |
| #61 | open | roadmap; see §2. |

## 4. Prioritized queue

1. **#249 — alignment-safe `wad.rs` offset dereferences.** P0, chosen. Small,
   bounded, no observable byte changes, and the existing differential harness
   is the acceptance criterion. The harness must gain an odd-`infotableofs`
   case, because today it only ever builds naturally aligned tables
   (`engine/rust/wad/tests/diff_harness.c:325`, `WAD_TABLE_OFS 12`).
2. **Reconcile #61 with #248** and close #60. Documentation-only, but it is the
   document future ports are sequenced against, and it currently asserts a
   fallback rule the build rejects.
3. **#233 design decision** — decide per-feature-set archives vs. the
   all-targets-accessor workaround before the first HexenWorld-only port that
   cannot route around a missing symbol.
4. **Phase 6 candidate audit** — `zone.c`, `cvar.c`, `cmd.c`, `quakefs.c`.
   Highest leverage and highest blast radius: central mutable global state,
   callbacks, allocator/lifetime semantics, save/config compatibility. Order
   zone/hunk first (everything else allocates through it), then cvar, then cmd,
   then the filesystem/package index.

## 5. Risks and gaps recorded, not fixed

- **Alignment on strict-alignment hosts.** The C original has the same
  precondition; the harness runs on x86-64 CI where unaligned scalar loads
  work. Making the Rust side alignment-safe does not make the *C harness* safe
  on such a host — but the C is migration scaffolding and is not shipped.
- **Harness case coverage.** The wad gate's floor and case list are in
  `scripts/check-rust-wad.sh`; the odd-offset case added by this goal is the
  one gap #249 names.
- **Local-checkout staleness.** Any agent working this tree should start with
  `git fetch` and `git status -sb`; a branch missing from local `master` may
  already be merged.
