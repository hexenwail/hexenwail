# zone.c: ownership, lifetimes and the target configuration the port needs

Written before the `zone.c` port starts, as #61 requires and as
`rust_phase6_cvar_ownership.md` did for the cvar slice. Baseline
`origin/master` `141e3dd1b`; issue #288; the per-target variant matrix and the
#233 decision it forces are in `rust_phase6_zone_target_matrix.md`.

Claims below cite `engine/h2shared/zone.c` by line; the file is 1335 lines and
exposes twenty symbols: `Memory_Init`, the `Z_*` zone allocator, the `Hunk_*`
allocator with marks, and the `Cache_*` API (client only).

## 1. Ownership

| thing | owner | rule for the port |
|---|---|---|
| `hunk_base`, `hunk_size`, `hunk_low_used`, `hunk_high_used`, `hunk_tempactive`, `hunk_tempmark` (`zone.c:361-368`) | the port | entirely internal (`static`). `hunk_base` points at caller-supplied memory; the port never frees it. |
| the `zonelist` (`:123`) and every `zonelist_t`/`memzone_t`/`memblock_t` inside the hunk | the port | internal structure, allocated out of the hunk by `Memory_InitZone` (`:1262`). |
| `cache_head` and the `cache_system_t` records (`:575`, `:564-573`) | the port | internal, hunk-backed; the LRU order is observable through `Cache_Check`. |
| `cache_user_t` (`zone.h:122-125`) | **the caller** | embedded by callers — `qpic_t.cache` in `gl_draw.c`/`draw.c`, `model_t.cache` in `gl_model.c`, the `cache` member in `snd_dma.c` and friends. The port reads and writes `c->data` and must never allocate, move or adopt the struct. `Cache_Alloc` requires `c->data == NULL`; `Cache_Free` requires it non-NULL; both abort otherwise. |
| every pointer `Z_Malloc`/`Z_Realloc`/`Z_Strdup`/`Hunk_*` returns | **the caller** after return | the port hands out ownership; `Z_Free` is then the caller's call. |
| the `FILE *` in `Hunk_Print`/`Z_Print`/`Cache_Print` (`:913`, `:1090`, `:1018`) | the caller (debug paths, `Z_DEBUG_COMMANDS == 0`) | not part of the shipped build; the port keeps the functions reachable but they are dead code today. |

## 2. Three lifetimes over one buffer

All three allocators carve the same caller-supplied buffer, and they interact:

- **zone (`Z_*`)** — `Z_TagMalloc` (`:181`) is a first-fit search over the
  zone's `memblock_t` list with a rover and coalescing `Z_Free` (`:134`). It is
  the small-object allocator; the C's own comment says "the zone calls are
  pretty much only used for small strings and structures, all big things are
  allocated on the hunk".
- **hunk (`Hunk_*`)** — a bump allocator from the bottom (`hunk_low_used`) and
  from the top (`hunk_high_used`), with marks (`Hunk_LowMark`,
  `Hunk_HighMark`) and `Hunk_TempAlloc` (`:525`) which frees the *previous*
  temp block before taking a new one. `Hunk_FreeToLowMark`/`HighMark` **zero the
  released region** (`:441-465`), so a later allocation does not see stale
  bytes — observable, and part of parity.
- **cache (`Cache_*`, client only)** — records live in the hunk and hold a
  caller's `cache_user_t *`. `Hunk_AllocName` calls `Cache_FreeLow` after
  moving the low mark (`:415`) and `Hunk_HighAllocName` calls `Cache_FreeHigh`
  (`:505`), so **the hunk's growth evicts or relocates cache entries**:
  `Cache_Move` (`:582`) re-copies an entry elsewhere in the hunk and rewrites
  `c->data`. That is why the marks and the cache cannot be ported as separate
  lifetimes — they are one mechanism.

## 3. Contracts that have to be reproduced literally

1. **Failure is non-returning.** Twenty-nine `Sys_Error` sites. Spot checks:
   `Z_Malloc` bad zone id and failed allocation (`:278`, `:285`), `Z_Free` on a
   freed/foreign pointer (`:144`, `:154`), `Z_Realloc` on a freed pointer or
   without `ZMAGIC` (`:303`, `:313`), `Hunk_AllocName` bad size and exhaustion
   (`:405`, `:410`), `Hunk_HighAllocName` bad size (`:484`),
   `Hunk_FreeTo*Mark` bad marks (`:444`, `:468`), `Cache_TryAlloc` out of hunk
   memory (`:693`), `Cache_Free` not allocated (`:804`), `Cache_Alloc` already
   allocated / bad size / out of memory (`:850`, `:853`, `:871`),
   `Cache_UnlinkLRU` NULL link (`:658`). All take the C `Sys_Error`
   (FUNC_NORETURN) — there is no error channel.
2. **`Z_Realloc` is free-plus-realloc, not `realloc`.** `:291-340`: it walks to
   the block header, looks the zone up by `block->magic`, frees the pointer,
   allocates fresh, `memmove`s `min(old_size, size)` bytes when the address
   moved, and zero-fills any growth. `NULL` behaves as `Z_Malloc`. The returned
   pointer is *not* required to be the same one, and callers that hold interior
   pointers across it would break in the C too.
3. **Alignment follows from the header sizes, not from an assertion.** `Z_Malloc`
   returns `block + sizeof(memblock_t)`; `Hunk_AllocName` returns `h + 1` after
   rounding to 16 bytes and placing a `hunk_t` (`:396-427`). The same reasoning
   the `wad.rs` port had to apply to on-disk offsets applies here in reverse:
   the port must not *weaken* these guarantees, and any field access through a
   derived pointer should stay within what the C guarantees.
4. **`hunk_t.sentinal == HUNK_SENTINAL` and `memblock_t.magic == ZMAGIC` are
   checked** (`Z_CheckHeap` under `Z_CHECKHEAP`, `Hunk_Check` under `PARANOID`,
   `Hunk_Print`'s consistency pass at `:954-960`). `Z_CHECKHEAP` and `PARANOID`
   are build-configuration knobs, off in the shipped build; the port keeps the
   checks compiled since they cost nothing when the knobs are off, and the
   harness can turn them on.
5. **The cache's LRU order is observable.** `Cache_Check` moves the entry to
   the head (`:823-840`); `Cache_Alloc` evicts `cache_head.lru_prev->user`
   (`:870`) until the allocation fits. The harness must compare the LRU order,
   not just the returned pointers.
6. **`Cache_Alloc` rounds `size` up** by `sizeof(cache_system_t)` and 16 bytes
   (`:856`), copies `name` with `q_strlcpy` into a 32-byte field (`:864`), and
   returns `Cache_Check (c)` so the caller sees the same pointer.
7. **`Memory_Init` carves the zones out of the hunk** (`:1291-1325`) after
   setting `hunk_base`/`hunk_size`/the marks, and its zone size comes from
   `ZONE_DEFSIZE` unless `-zone <KB>` is on the command line (`COM_CheckParm`,
   `com_argc`, `com_argv`).
8. **The `flush` command is registered only in the non-`SERVERONLY` build**
   (`:1323`). This matters more than it looks: with one Rust implementation it
   would be all too easy to register it everywhere, and then `h2ded`/`hwsv`
   would *gain* a command — visible to `Cmd_Exists` (which decides whether a
   cvar name is refused) and to the console. The port must therefore know
   whether the target it is compiled into has the cache API.

## 4. The target configuration the module needs from outside

From the matrix document, four values differ by target and **cannot be
computed inside a single implementation**:

| value | glhexen2 | h2ded | hwsv |
|---|---|---|---|
| default main-zone size (`ZONE_DEFSIZE`) | `0x200000` | `0x100000` | `0x100000` |
| secondary-zone size (`MEM_STATIC_TEX + MEM_CODEC_MEM`) | `0x40000` | `0` | `0` |
| dedicated flag (live `isDedicated`) | variable, `-dedicated` | `true` | macro `1` |
| has the cache API (registers `flush`) | yes | no | no |

Whichever mechanism #233 lands, the port must take these four as *input*, not
as assumptions, and the same input must serve the harness (which compiles the C
both ways and needs the Rust to be told which variant it is being compared
against).

Two subtleties the port must handle:

- **`Cache_Init` must run even where the cache is unused.** In `SERVERONLY` the
  C calls `Cache_FreeLow(x)`/`Cache_FreeHigh(x)` as macros that expand to
  nothing, so the list heads are never read. A single implementation that keeps
  the cache code will call `Cache_FreeLow` from `Hunk_AllocName` in `h2ded` too,
  and that dereferences `cache_head.next` — so the cache must be initialised in
  every target even though nothing will ever be allocated in it. With the cache
  empty, `Cache_FreeLow` returns on its first test, so the observable behaviour
  is unchanged.
- **The dedicated flag is read at `Memory_Init` time, not at build time.** In
  the client it comes from `-dedicated` (`engine/hexen2/sys_unix.c:790`), so the
  input has to be a live read, not a constant folded at compile time.

## 5. Interactions with landed work

- **`huffman.rs` uses `malloc`, deliberately, where hwsv's C used
  `Hunk_AllocName`.** The port must not "fix" that: `CL_ClearState` →
  `Host_ClearMemory` frees to a low mark, which would dangle a hunk-allocated
  tree (`engine/rust/src/huffman.rs` documents it). Porting `zone.c` does not
  change that reasoning, but the ownership note above is where it is recorded.
- **`q_strlcpy` is already the Rust `strlcpy.rs`.** `Cache_Alloc` and
  `Hunk_AllocName` will therefore call from one port into another through the
  same C symbol they call today — no special handling, but it means the zone
  harness links the strlcpy feature as well, or stubs it.
- **`q_strcasecmp` is still C** (`engine/h2shared/common.c`) and is used by the
  debug/report paths (`Hunk_Print`, `Memory_Display_f`, `Cache_Display_f`).
- **Callers of `Memory_Init`**: `engine/hexen2/host.c:1530` (client) and
  `engine/hexen2/server/host.c:704` (dedicated), both
  `Memory_Init (host_parms->membase, host_parms->memsize)` — neither passes the
  zone sizes, which is why the constants live inside the module today.

## 6. What the differential harness must observe

After every call: the full `zonelist` chain (ids, magics, names, and the block
lists with each block's offset in the hunk, size, tag and magic), the three hunk
globals plus the temp mark, `hunk_base` identity, every `cache_system_t` with
its offset, size, name, user pointer and LRU position, every `cache_user_t`
the harness owns, the LRU order, and every diagnostic. The allocation offsets
matter more than the return values: the point of this port is *where* things
land, and a port that returns the right pointer from the wrong offset has
changed the memory layout of every caller.

Failure paths (bad zone id, bad mark, double free, `Cache_Free` not allocated,
`Cache_Alloc` already allocated, exhaustion) run in child processes, as the
other ports do.
