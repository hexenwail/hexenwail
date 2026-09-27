# quakefs.c: ownership, lifetimes, the variant matrix, and the #233 verdict

Written before the `quakefs.c` port starts, as #61 requires and as the cvar,
zone and cmd slices did. Baseline `origin/master` `9f336799e`. Slice issue:
#292. Refs #61, #233, #42.

`engine/h2shared/quakefs.c` is 3910 lines and the widest API of any slice: the
`FS_*` file layer, the PAK and ZIP loaders, the search-path list and the gamedir
state, with 127 distinct `FS_*`/`COM_*` symbols reached from outside the file.

## 1. Ownership

| thing | owner | rule for the port |
|---|---|---|
| `fs_searchpaths`, `fs_base_searchpaths`, `fs_base_nomp_searchpaths` (`:154-163`) | the port | the linked list of directories and packs. Every entry and the names, handles and mapped archives inside them are the port's; callers only ever hold a `searchpath_t *` transiently. |
| `fs_lastzip`, `fs_lastzipentry` (`:175-176`) | the port | a one-entry cache of the last zip looked up. |
| `fs_basedir`, `fs_gamedir`, `fs_userdir`, `fs_gamedir_nopath`, `fs_portals_path_id`, `gameflags` (`:178-189`) | the port | long-lived global state read by everything, including the `MakePath` family and `HS_*`. The model below says which of it becomes explicit Rust state; nothing outside the file writes it today except through the `FS_*` API. |
| the `pakdata_t` tables (`:203`, `:217`, `:223`, `:229`), `mark_*` content marks (`:265`, `:275`, `:283`) and `pop[]` (`:307`) | the port | data, not code: known-pak identity with CRCs and the content markers used to tell a full install from a demo. These are exactly the kind of table a port can silently mistranscribe, so the gate compares them entry for entry against the C (the lesson from `cmd.c`'s invented legacy name). |
| `loadcache` and the `cache_user_t` it points at (`:1912-1914`) | the port, client only | the `LOADFILE_CACHE` path hands out memory owned by the cache, which the caller may hold while the file is re-read. |
| data returned by `FS_LoadFile`/`FS_LoadHunkFile`/`FS_LoadZoneFile`/`FS_LoadCacheFile` | **the caller** | each `loadfile_t` variant names its own lifetime in the `LOADFILE_*` enum; the port keeps them distinct rather than unifying them. |
| `qerr_*`/`FSERR_*` path buffers (`:327-355`) | mixed | the `MakePath` family writes into a caller buffer *or* a `static` one depending on the entry point; the port must keep that split, since callers rely on the static buffer surviving until the next call. |

## 2. The variant matrix, from the compiler rather than from reading `#if`s

Preprocessed and compiled five ways (client include order for the first four,
hwsv's for the last), then compared by the undefined-symbol sets of the
resulting objects:

| variant | defines | undefined symbols |
|---|---|---:|
| `glhexen2` | `GLQUAKE H2W_INTEGRATED` | 93 |
| wasm webgl2 client | `GLQUAKE` | 93 |
| wasm software client | `WEBQUAKE WEBSOFT` | 92 |
| `h2ded` | `SERVERONLY` | 67 |
| `hwsv` | `H2W SERVERONLY` | 68 |

Findings that matter:

- **`H2W_INTEGRATED` changes no symbol reference.** glhexen2 and the wasm webgl2
  client differ in 47 preprocessed lines and in *zero* referenced symbols; the
  wasm client is additionally missing `H2W_INTEGRATED` altogether, because
  `CLIENT_DEFINITIONS` only gains it under `USE_HEXENWORLD_CLIENT AND NOT
  EMSCRIPTEN`. So the client/H2W_INTEGRATED arms are a **behaviour** difference,
  not a symbol one.
- **`GLQUAKE` is worth exactly one symbol**: the wasm software client differs
  from the webgl2 client only by `TexMgr_NewGame`, from the `#ifdef GLQUAKE` arm
  at `:3244` inside the non-dedicated reinit block.
- **The client variants reference 26 symbols the dedicated ones do not**:
  `BGM_Stop`, `Cache_Alloc`, `Cache_Flush`, `CL_Disconnect`, `cls`,
  `Cmd_StartupScript`, `Con_ShowList`, `Draw_ReInit`, `Host_ClearMemory`,
  `Host_ShutdownServer`, `Host_WriteConfiguration`, `M_BuildBindList`,
  `TexMgr_NewGame`, `VID_Lock`, `va`, `isDedicated`, `q_strncasecmp`, `rand`,
  `Sys_DoubleTime`, `Z_Strdup`, `Cbuf_AddText`, `Cbuf_Clear`, `Cmd_Argc`,
  `Cmd_Argv`, `COM_StrCompare` and `__isoc23_strtol`. Several of those exist in
  no dedicated binary at all — `Cache_Alloc` and `Cache_Flush` are compiled out
  of `SERVERONLY` by the zone port's own matrix, `TexMgr_NewGame`/`Draw_ReInit`/
  `M_BuildBindList`/`VID_Lock` are client-only.
- h2ded and hwsv differ from each other by `Host_Error`/`sv_protocol` (h2ded
  only) and `Info_SetValueForStarKey`/`SV_Error`/`svs` (hwsv only).

## 2b. The exported globals, and the wasm-storage trap they set

Six globals are declared `extern` in `quakefs.h` and defined in `quakefs.c`:
`fs_gamedir_nopath[MAX_QPATH]` (`:181`), `gameflags` (`:189`), `fs_filesize`
(`:1248`), `file_from_pak` (`:1249`), and the two read-only cvars `oem` and
`registered` (`:191-192`).  The first four are read by C outside the file --

Readers include `gl_draw.c`, `gl_model.c`, `model.c`, `menu.c`, `sbar.c`,
`pr_edict.c`, `sv_main.c`, `host_cmd.c` and `cl_hw.c`, so none of them can be
made Rust-private the way `wad.c`'s three globals were.

They are therefore the same trap `cmd_source` hit: rustc emits `#[no_mangle]`
**statics as local symbols** in a wasm32 staticlib while keeping functions
global, so on that target the C's reads would fail to link. The port defines
each on every target except wasm32 and declares it `extern` there, with the
storage added to `engine/rust/wasm_globals.c` using the engine's own types from
`quakefs.h` and `cvar.h` — six entries after `msg_readcount`, `msg_badread`,
`vec3_origin` and `cmd_source`.  The two cvars are registered by `FS_Init`
(`:3282-3283`), so they take the same treatment even though only C reads them
through the cvar table rather than by name.

## 3. The #233 verdict

**The per-target shim still suffices — but it has to grow from values and
predicates into behaviour hooks, and this is the first slice that needs them.**

The reason is the finding above: the client-only *references* sit inside
functions that every target compiles (`FS_LoadFile`'s `LOADFILE_CACHE` arm at
`:1949`, the `Cache_Flush` call at `:1179`, the `Draw_BeginDisc`/`EndDisc` pair
at `:1918`, the non-dedicated reinit whose `GLQUAKE` arm is at `:3244`, the hwsv `svs.info` write
at `:1233`). A Rust translation that referenced those symbols directly would be
one archive with undefined references in `h2ded` and `hwsv` — precisely the
failure #233 describes, now with a live trigger rather than a hypothetical one.

What makes it survivable is that every divergent site is a **small, localised
statement**, not a differing code region: a cache call, a disc marker pair, one
serverinfo write, one client reinit, and a handful of search-path decisions.
The shim file is C compiled per target, so it may name target-only symbols
freely; one exported hook per site gives the Rust module a target-agnostic call
and keeps the symbol out of the archive:

| hook | arm it replaces | dedicated/no-op form |
|---|---|---|
| `QuakeFS_TargetFlushCache` | `:1179` `Cache_Flush()` | nothing |
| `QuakeFS_TargetBeginDisc` / `EndDisc` | `:1918` macros | nothing |
| `QuakeFS_TargetLoadCache` | `:1949` `Cache_Alloc(loadcache, …)` | unreachable |
| `QuakeFS_TargetClientReinit` | the `:3244` `GLQUAKE` arm and its neighbours (`Draw_ReInit`, `TexMgr_NewGame`) | nothing |
| `QuakeFS_TargetSetHwServerinfo` | `:1233` `Info_SetValueForStarKey(svs.info, …)` | nothing |

Two further facts keep this inside the mechanism rather than outside it:

- `Info_SetValueForStarKey` is itself a **Rust port** (`info_str.rs`), so it is
  defined in the archive for every target; only `svs`, the hwsv-only global,
  needs the hook.
- The registrations at `:3286` (`maplist`, `randmap`) are the same shape as
  `cmd.c`'s client-only commands: a target predicate decides whether they are
  registered, because a command that exists in one target and not another is
  observable through `Cmd_Exists`.

**What would have forced per-feature-set archives instead** — and did not
happen: a *large* differing region that would amount to porting two functions,
or a differing **data layout** (`searchpath_t`/`pack_t` sized differently per
target). Neither occurs: the structures are target-independent, and the arms
gate small statements.

This is recorded on #233 as the first behaviour-hook case, and it is an
extension of the approved option (a), not a change to the build: one archive,
one crate, the same per-target C file pattern, the exactly-once symbol property
intact.

## 4. Cross-port calls

Now Rust, so these resolve inside the archive: `Z_Malloc`/`Z_Free`/`Z_Strdup`
(11/8/1 uses), `Hunk_AllocName`/`Hunk_TempAlloc` (zone), `Hash_Allocate`/
`Hash_Add`/`Hash_Free` (hashindex), and `Info_SetValueForStarKey` (info_str) in
the hwsv arm.

**`pack_t` (`:63-70`) and `zippack_t` (`:133-141`) embed a `hashindex_t` by
value**, so the port must use the `hashindex` port's `HashIndexC` layout rather
than restating it — the third shared-layout case after `CvarC` (cvar/info_str)
and `QuakeParmsC` (zone/cmd).

Still C: `mz_*` from vendored **miniz** (the zip reader: `mz_zip_reader_init`,
`…_file_stat`, `…_get_num_files`, `…_extract_to_mem`, `…_end`), which stays a C
dependency until Phase 11 replaces or externalises it, plus the three entry
points that cannot move in this port — `FS_MakePath_VA`/`FS_MakePath_VABUF`
(C-variadic, which stable Rust cannot define, so they stay in
`engine/rust/quakefs_variadic.c`) and `FS_ResolveCasePath` (walks
`struct dirent`, a glibc layout, not a standard one).  The `FS_*` syscall
helpers themselves *are* ported — this note originally listed them as staying
C, which the port disproved.

## 5. Tables the gate must compare against the C, entry for entry

- `pakdata[]`, `demo_pakdata[]`, `oem0_pakdata[]`, `old_pakdata[]` — names, sizes
  and CRCs of known paks.
- `mark_data1_pak0[]`, `mark_data1_pak1[]`, `mark_portals[]` — content marks.
- `pop[]` — the CRC popcount table used by `check_known_paks`.
- `skyfaces`/`skyexts`/`skydirs` (`:2930-2932`) — skybox naming.
- the `Cmd_AddCommand` registration list in `FS_Init`, client-only entries
  included.

## 6. What the differential harness must observe

After every call: the whole searchpath list with each entry's kind, path bytes
and (for packs) the embedded hash contents and file offsets; the gamedir,
userdir, basedir and portals-path state; open handles and read positions for the
`FS_*` file API; the `loadfile_t` variants and the bytes they return; the
versioned save path selection; the registered command names; and every
diagnostic. Each shipped variant gets its own arm where it differs, and variants
that must agree are required to agree before either is compared with Rust.
Failure paths — missing file, malformed zip, truncated pack, bad `path_id`,
`FS_LoadFile` of a directory — run in child processes where the C aborts.

**Benchmark decision**: none. The port's own work per call is list traversal and
path formatting; the cost of the file layer is the syscalls underneath it, which
the port does not change and which a two-implementation micro-benchmark would
measure rather than the port. This is recorded in the gate's header rather than
left implicit, as #292 requires.

## 7. Port notes from reading the loaders (for whoever resumes)

Recorded while porting, so the next run does not have to rediscover them.  Line
references are to `quakefs.c` at `9f336799e`.

**Why the entry arrays are `malloc`, not `Z_Malloc` (uhexen2-mm4l).** Both
loaders allocate their `pakfiles_t`/`zipfiles_t` array with `malloc` and say so
in a comment: `MAX_FILES_IN_PACK * sizeof(pakfiles_t)` is 2048 * 72 = 144 KB,
7% of the client's 2 MB zone, and `MAX_FILES_IN_ZIP * sizeof(zipfiles_t)` is
65536 * 76 = 4.75 MB — 2.5x the whole pool — so the entry-count check alone was
admitting archives the zone could not serve.  The searchpath holds every
mounted archive at once (`MAX_PK3_PER_DIR` = 64 per gamedir).  The port must
keep `malloc`/`free` here and must not "improve" it to `Z_Malloc`, and it must
not need the zeroing: both parse loops write every field of the entries they
keep.  `pack_t` and `zippack_t` themselves *are* `Z_Malloc`'d, and the zip one
is `memset` before use.

**The STORED / DEFLATED split is per entry, not per archive.** `zipfiles_t.filepos`
carries the state: `-1` means "STORED, offset not resolved yet", `-2` means
"must be inflated", anything else is a real data offset.  STORED entries are
served exactly like pak members (reopen, seek, hand back the `FILE *`) and
`FS_ZipDataOffset` resolves their offset lazily on first open by reading the
30-byte local header, because the central directory's offset points at the
local header whose name/extra lengths may differ.  DEFLATED entries are
inflated into memory by `FS_ZipReadEntry`.  `FS_OpenFile_Internal` sets
`fs_lastzip`/`fs_lastzipentry` only for the DEFLATED case and returns the
uncompressed length with `*file == NULL`, which is what makes `FS_OpenFile`
and the `fshandle_t` path diverge.

**Two ceilings, enforced at different times.** `MAX_ZIP_INFLATE` (64 MB) is
checked at *mount* for any entry with `method != 0`, so a zip bomb costs one
warning naming the archive rather than a mysterious failure at open
(uhexen2-k4lc); `0x7fffffff` is the length ceiling that keeps sizes in a
`long`/`fseek`.  Both skip the entry; neither aborts.

**Hash sizing is not `Hash_Allocate`'s business.** The pak loader picks the
smallest power of two greater than the file count by a loop from 1, the zip
loader from a floor of 16; `Hash_Allocate` aborts if it is not a power of two.
`Hash_GenerateKeyString(..., false)` (case-insensitive) is used on both paths
so a pk3 authored on a case-sensitive filesystem resolves like a pak.

**`gameflags` is accumulate-only, with one exception.** Every loader ORs into
it; only `GAME_PORTALS` is ever cleared, by `Host_Game_f`, because it doubles as
"portals content is reachable now" (uhexen2-lx4m).  An archive adds
`GAME_MODIFIED` unconditionally and additionally `check_known_zip`'s flags when
it is a base mount.

**`FS_UnwindSearchpaths` is centralised for `fs_portals_path_id`** (uhexen2-5vb6):
the three old open-coded loops differed only in narration, and an id that
outlives the entries it names would hand `PR_ShouldSubstituteProgs` a stale
answer.

**`check_known_paks` returns a flag chosen by (paknum, numfiles, crc)**, with a
per-paknum cascade of demo/OEM/old-edition fallbacks, and `check_known_zip` is
the container-agnostic half: it asks whether one archive holds every marked
member at its exact size (`zip_has_marks`), with a wrong size an immediate NO
rather than a keep-looking.

**A work item the model did not have: four `static inline` hashindex helpers.**
`quakefs.c` calls `Hash_First`, `Hash_Next`, `Hash_GenerateKeyString` and
`Hash_GenerateKeyInt` nine times, and all four are `static inline` in
`engine/h2shared/hashindex.h` (`:47`, `:59`, `:70`, `:90`) — compiled into each
C caller, not exported as symbols.  A Rust port cannot call them, so it must
either reproduce those four bodies in Rust against the `HashIndexC` layout (the
same way the header's other inlines are handled, and what migration rule 3
anticipates when it says to audit `static inline` functions) or the hashindex
port must start exporting them, which would change a landed port.  Reproducing
them in `quakefs.rs` is the smaller change and keeps the hashindex port as it
is; either way the differential harness has to compare the two, because a hash
key that differs by one bit turns a lookup into a miss rather than into a
visible error.
