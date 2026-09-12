# Plan: Port `hashindex.c` to Rust Behind an FFI Shim

**Goal**: port one self-contained engine subsystem to Rust callable from C, keeping the rest of the engine in C.

**Chosen subsystem**: `engine/h2shared/hashindex.c` + `engine/h2shared/hashindex.h`.

All facts below were verified against the tree at `worktree-mtwipv8e-mt0cjs`
(HEAD `92a9b595e`).

---

## 1. What the subsystem actually is

`hashindex.c` is a 146-line open-chained hash table lifted from Doom 3
(GPLv3+), used to map a name/key onto an index into a caller-owned array.

```c
/* engine/h2shared/hashindex.h:27-32 */
typedef struct hashindex_s {
    int  hashSize;
    int *hash;
    int *indexChain;
    int  hashMask;
} hashindex_t;
```

**Exported (linkable) symbols** — `hashindex.h:34-38`, defined in `hashindex.c`:

| Function | Behaviour on failure |
|---|---|
| `Hash_Allocate(hi, hashSize)` | `Sys_Error` if not a power of two, if already initialised, or OOM |
| `Hash_Free(hi)` | `free()` the two arrays, NULL the pointers |
| `Hash_Add(hi, key, index)` | `Sys_Error` if uninitialised or index out of range |
| `Hash_Remove(hi, key, index)` | `Sys_Error` if uninitialised |
| `Hash_Clear(hi)` | no-op when uninitialised |

**Inline (NOT linkable) helpers** — `hashindex.h:47,59,70,90`:
`Hash_First`, `Hash_Next`, `Hash_GenerateKeyString`, `Hash_GenerateKeyInt` are
all `static inline` in the header. They have no symbol and cannot be called
across an FFI boundary. They are compiled into each C caller and read the
struct fields directly.

**Allocation uses `malloc`/`free`, deliberately** — not `Z_Malloc`. The comment
at `hashindex.c:44-53` records why (uhexen2-mm4l): the zone is a fixed 2 MB
pool (1 MB on `h2ded`) and `Z_Malloc` aborts rather than returning NULL, while
two call sites size the table from archive entry counts.

**Error handling is fatal, not recoverable** — `Sys_Error` is declared
`FUNC_NORETURN` (`engine/h2shared/sys.h:98`). There is no error channel through
this API.

**Licence**: `hashindex.c` is **GPLv3-or-later** (Doom 3 lineage), unlike the
surrounding `h2shared` code which is largely **GPLv2-or-later**. A Rust
reimplementation is a derivative work and must stay GPLv3+.

---

## 2. The constraint that decides the whole design

**`hashindex_t` is embedded by value, not held behind a pointer** — 8
embedding sites (plus 2 `extern` re-declarations in `gl_rmisc.c`). Verified
sites:

| Location | Kind |
|---|---|
| `engine/h2shared/model.c:56` | `static hashindex_t hash_mod;` |
| `engine/h2shared/gl_model.c:101` | `static hashindex_t hash_mod;` |
| `engine/h2shared/draw.c:63` | `static hashindex_t hash_cachepics;` |
| `engine/h2shared/gl_draw.c:180` | `hashindex_t hash_gltextures;` (global, `extern` in `gl_rmisc.c:63`) |
| `engine/h2shared/gl_draw.c:198` | `hashindex_t hash_cachepics;` (global, `extern` in `gl_rmisc.c:64`) |
| `engine/h2shared/snd_dma.c:85` | `static hashindex_t hash_sfx;` |
| `engine/h2shared/quakefs.c:69` | member of `pack_t` (Z_Malloc'd) |
| `engine/h2shared/quakefs.c:140` | member of `zippack_t` (Z_Malloc'd) |

Consequences, and where the obvious design goes wrong:

- **Rust must not own or allocate the struct.** C allocates it as static
  storage or inside a larger Z_Malloc'd struct. Any RAII wrapper that allocates
  its own `HashIndexC` and hands out a pointer is unusable here — the C global
  already exists and Rust never gets to construct it.
- **The layout must be byte-identical**, so the header stays the source of
  truth and the Rust definition must be `#[repr(C)]` with the same field order.
- **Symbol names must be identical** (`Hash_Allocate`, not `rust_hash_allocate`).
  61 `Hash_*` references across 7 files call these directly, and the header
  itself is unchanged, so renaming would mean touching every caller for no gain.
- **Rust does not need to export the four inline helpers.** They stay in the
  header, compiled into C callers, reading the same struct fields. Rust only
  has to keep those fields in the same layout, which `#[repr(C)]` guarantees.

**Corrected FFI surface** — the entire boundary is five functions:

```rust
// engine/rust/hashindex/src/ffi.rs
use core::ffi::{c_char, c_int};

#[repr(C)]
pub struct HashIndexC {
    pub hash_size: c_int,
    pub hash: *mut c_int,
    pub index_chain: *mut c_int,
    pub hash_mask: c_int,
}

// Same names the C code already calls. No #[no_mangle] drift.
#[no_mangle] pub extern "C" fn Hash_Allocate(hi: *mut HashIndexC, hash_size: c_int) { /* … */ }
#[no_mangle] pub extern "C" fn Hash_Free(hi: *mut HashIndexC) { /* … */ }
#[no_mangle] pub extern "C" fn Hash_Add(hi: *mut HashIndexC, key: c_int, index: c_int) { /* … */ }
#[no_mangle] pub extern "C" fn Hash_Remove(hi: *mut HashIndexC, key: c_int, index: c_int) { /* … */ }
#[no_mangle] pub extern "C" fn Hash_Clear(hi: *mut HashIndexC) { /* … */ }
```

**ABI traps to respect:**

- `qboolean` is `typedef int qboolean` (`common/q_stdinc.h:126`). Never use
  Rust `bool`/`c_bool` for it — it is a 4-byte `c_int`.
- `Hash_Allocate`'s failure mode is `Sys_Error`, which does not return. A Rust
  implementation must call the C `Sys_Error` (declared `extern "C"`) rather
  than returning an error, because there is no error channel in the header.
- Allocation must go through `libc::malloc`/`libc::free` so `Hash_Free` stays
  interchangeable with the C version if both ever coexist during migration.

---

## 3. Rust crate layout

```
engine/rust/hashindex/
├── Cargo.toml          # crate-type = ["staticlib"]
├── src/lib.rs          # implementation
└── src/ffi.rs          # the five #[no_mangle] extern "C" entry points
```

`Cargo.toml`:

```toml
[package]
name = "hashindex-rs"
version = "0.1.0"
edition = "2021"

[lib]
name = "hashindex_rs"
crate-type = ["staticlib"]   # staticlib: linked into the C executables

[dependencies]
libc = "0.2"
```

A `staticlib` is required rather than `cdylib`: the engine is a normal
executable and, for the WASM/Emscripten path, has no dynamic loader.

---

## 4. Build integration — the exact touch points

### 4.1 CMake (`engine/CMakeLists.txt`)

`hashindex.c` is compiled into **two** target groups, so both need the switch:

- **line 622** — `COMMON_SOURCES` (client `glhexen2`, and the `h2ded` server,
  which derives from `COMMON_SOURCES`)
- **line 1128** — `HWSV_SOURCES` (HexenWorld server `hwsv`)

Plan:

```cmake
option(USE_RUST_HASHINDEX "Link the Rust hashindex implementation" OFF)

# Swap the C source out of both lists when the Rust lib is in use.
if(USE_RUST_HASHINDEX)
    list(REMOVE_ITEM COMMON_SOURCES ${COMMONDIR}/hashindex.c)
    list(REMOVE_ITEM HWSV_SOURCES   ${COMMONDIR}/hashindex.c)
endif()
```

Removing the C file from the source list is what prevents duplicate-symbol
errors — `Hash_Allocate` would otherwise exist in both the Rust staticlib and
the compiled C object.

Building and linking (guarded, and a no-op on `EMSCRIPTEN` in phase 1):

```cmake
if(USE_RUST_HASHINDEX AND NOT EMSCRIPTEN)
    set(HASHINDEX_RS_DIR ${UHEXEN2_TOP}/engine/rust/hashindex)
    set(HASHINDEX_RS_LIB ${CMAKE_BINARY_DIR}/rust/libhashindex_rs.a)

    add_custom_command(
        OUTPUT ${HASHINDEX_RS_LIB}
        COMMAND cargo build --release --manifest-path ${HASHINDEX_RS_DIR}/Cargo.toml
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${HASHINDEX_RS_DIR}/target/release/libhashindex_rs.a
                ${HASHINDEX_RS_LIB}
        DEPENDS ${HASHINDEX_RS_DIR}/src/lib.rs
                ${HASHINDEX_RS_DIR}/src/ffi.rs
                ${HASHINDEX_RS_DIR}/Cargo.toml
        COMMENT "Building Rust hashindex shim"
        VERBATIM)
    add_custom_target(hashindex_rs DEPENDS ${HASHINDEX_RS_LIB})

    foreach(tgt glhexen2 h2ded hwsv)
        if(TARGET ${tgt})
            add_dependencies(${tgt} hashindex_rs)
            target_link_libraries(${tgt} PRIVATE ${HASHINDEX_RS_LIB})
        endif()
    endforeach()
endif()
```

The `TARGET` guard matters: `h2ded` is only created when `BUILD_DEDICATED=ON`
and `hwsv` when `BUILD_HEXENWORLD=ON`.

### 4.2 Nix

`flake.nix` currently contains **zero** references to `cargo` or `rustc`
(`grep -c 'cargo\|rustc' flake.nix` → `0`), so the toolchain is added from
scratch.

- Add `cargo` and `rustc` to `nativeBuildInputs` of the Linux derivation
  (`flake.nix:199`, alongside `cmake` at `:200` and `pkg-config` at `:201`).
- Source filtering: `filteredSrc` (`flake.nix:79`) is an **allowlist** of
  top-level entries (`flake.nix:82`: `CMakeLists.txt`, `engine`, `utils`,
  `hw_utils`, `common`, `scripts`, `oslibs`, `libs`). `engine/rust/` sits under
  `engine`, so it is
  already included — no filter change needed. Worth confirming, because a
  forgotten entry fails the build loudly while a stray one silently costs
  cache hits.
- Cargo's `target/` directory must not be captured by the source filter; since
  the filter runs on tracked files post-gitignore, adding `engine/rust/**/target/`
  to `.gitignore` keeps it out.

### 4.3 Platform reach

| Target | Phase 1 |
|---|---|
| Linux `glhexen2` | **in scope** |
| Linux `h2ded` | in scope (shares `COMMON_SOURCES`) |
| Linux `hwsv` | in scope (separate source list, line 1128) |
| Windows/mingw cross | deferred — needs a mingw Rust target; the C build already cross-compiles |
| macOS | deferred — no macOS support exists yet in this tree at all |
| WASM/Emscripten | **out of scope** — the emscripten Rust target adds a second toolchain to a build that is already the most fragile (`uhexen2-x7s9`) |

---

## 5. Verification

There is no existing C unit-test harness for `h2shared` modules — `engine/tests/`
holds only `md3/` and `md5/`. Verification must therefore be built, and the
strongest available form is **differential testing against the C original in a
single binary**.

### 5.1 Differential harness (primary evidence)

Compile the original C file a second time with its symbols renamed, so both
implementations coexist:

```
cc -c engine/h2shared/hashindex.c -o hashindex_c.o \
   -DHash_Allocate=c_Hash_Allocate \
   -DHash_Free=c_Hash_Free \
   -DHash_Add=c_Hash_Add \
   -DHash_Remove=c_Hash_Remove \
   -DHash_Clear=c_Hash_Clear
```

(Older toolchains: `objcopy --redefine-sym Hash_Add=c_Hash_Add …`.) The
harness then builds two tables from identical pseudo-random key streams and
asserts:

1. `Hash_First`/`Hash_Next` chains are **identical in order**, not merely
   equal as sets — this is what catches a differing collision or insertion
   order.
2. `hashSize`/`hashMask` match, and both arrays are byte-identical after
   `Hash_Allocate` (both fill with `-1` via `memset`).
3. Remove-then-lookup returns the same sentinel (`-1`) in both.
4. `Hash_Clear` leaves `hash[]` equal and `indexChain[]` untouched in both.
5. Key streams sized to force collisions, plus the boundary `hashSize` values
   (16, 1024, and a non-power-of-two to confirm both abort identically).

Deterministic seeds only — a differential test that fails intermittently is
worse than none.

### 5.2 Structural checks

- `#[repr(C)]` field offsets asserted against the C header with a small
  `offsetof` comparison, so a layout drift fails the build rather than
  corrupting memory at runtime.
- Both `hashindex.c:622` and `:1128` confirmed absent from the link line when
  `USE_RUST_HASHINDEX=ON` (guards the duplicate-symbol hazard).

### 5.3 Engine-level smoke

Build both ways and compare observable behaviour:

```
cmake -B build_c     -DUSE_RUST_HASHINDEX=OFF && cmake --build build_c
cmake -B build_rust  -DUSE_RUST_HASHINDEX=ON  && cmake --build build_rust
```

The hash tables in question are keyed by model names, texture names, cachepic
names and sound names — all of which surface in the console at
`developer 1`. Loading a map and diffing the console/log output is a cheap
end-to-end check; `FS_LoadPackFile`/`FS_LoadZipFile` output ("Added packfile
… (N files)") additionally exercises the `quakefs.c` sites.

### 5.4 What is *not* claimed

The 72 Hz tick rate, `sv_physfps`, and the renderer are untouched by this
change; no rendering or physics verification is required.

---

## 6. Rollback

The switch is a single CMake option and is off by default.

1. `cmake -DUSE_RUST_HASHINDEX=OFF` (or simply omit it) — `hashindex.c` is
   back in `COMMON_SOURCES` and `HWSV_SOURCES`, the Rust staticlib is not
   linked, and the shipped behaviour is byte-identical to today's.
2. Delete `engine/rust/` and the CMake block if abandoning entirely.

Because the C header is never modified and the C source is retained in-tree,
rollback is a configure-time change, not a revert. Nothing in the engine needs
to know which implementation is linked.

---

## 7. Risks

| Risk | Assessment | Mitigation |
|---|---|---|
| Duplicate `Hash_*` symbols | **High** — silently breaks the link the moment both are compiled | Remove `hashindex.c` from both source lists under the flag; assert it in CI |
| Struct layout drift | **High** — corrupts 9 by-value sites, including non-static globals | `#[repr(C)]` + `offsetof` static assertions |
| `qboolean`/`bool` confusion | Medium — silent ABI mismatch on the key-generation signature | It is `typedef int`; use `c_int`. The helpers stay inline in C anyway |
| Fatal-error semantics | Medium — `Sys_Error` is `FUNC_NORETURN`; a Rust `panic` would unwind across the FFI boundary (UB) | Call the C `Sys_Error`; set `panic = "abort"` in the release profile |
| Licencing | Medium — `hashindex.c` is GPLv3+ amid GPLv2+ code | The port is a derivative work and must be GPLv3+ |
| Toolchain growth | Low/Medium — `flake.nix` gains `cargo`/`rustc` where it had none | Phase 1 limits this to the Linux derivation |
| WASM toolchain | Deferred by scope | Explicitly excluded from phase 1 |

---

## 8. Why this subsystem

- **Small and self-contained**: 146 lines, five exported functions, no
  dependency on engine globals — it touches only the struct it is handed.
- **Real blast radius**: ~61 call sites across 7 files, so a successful port
  proves the pattern rather than a toy.
- **Genuinely constraining**: the by-value embedding, the `static inline`
  helpers, the `FUNC_NORETURN` error path and the deliberate `malloc` are all
  representative problems a larger port would hit. A subsystem that were merely
  easy would not test the toolchain.
- **Reversible**: a CMake flag, with the C original left in place.

Rejected candidates and why: `sizebuf.c` and `zone.c` are welded to the engine
allocator (`Hunk_AllocName`, `Z_Malloc`); `cvar.c` and `cmd.c` carry heavy
global state and callback graphs; `quakefs.c` (3 846 lines) is too large for a
first step; `mathlib.c` is a viable second candidate but is mostly macros and
`static inline` in `mathlib.h`, so it offers a smaller true FFI surface.

---

## 9. Go / no-go checklist

- [ ] `HashIndexC` field offsets match `hashindex.h` (static assertion)
- [ ] Five symbols exported under their original names from the staticlib
- [ ] `hashindex.c` removed from both CMake source lists under the flag
- [ ] Differential harness: chains identical in order for ≥ 10⁶ insert/lookup ops
- [ ] Non-power-of-two `Hash_Allocate` aborts identically in both
- [ ] `glhexen2`, `h2ded`, `hwsv` all build with `-DUSE_RUST_HASHINDEX=ON`
- [ ] Engine console output identical on a map load, both configurations
- [ ] `-DUSE_RUST_HASHINDEX=OFF` byte-identical to today's build
- [ ] `cargo`/`rustc` wired into `flake.nix`; `nix build .#default` passes
- [ ] Licence header records GPLv3+ provenance

## 10. Next step

Implement phase 1 behind the flag: crate + `#[repr(C)]` struct + five
functions + the `offsetof` assertion, then the differential harness. Nothing
ships on by default, so the change is inert until the checklist is green.
