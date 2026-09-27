# cvar.c port: ownership, re-entrancy and ABI model (Phase 6)

Written before the port starts, because #61 requires the ownership question
answered first and because `cvar.c` is the first Phase 6 subsystem where the
port touches state that C code also holds. Baseline `origin/master` `f233524db`;
issue #286; refs #61, #233.

Every claim below was checked against the tree, and the commands are given so
they can be re-run rather than taken on trust.

## 1. Target matrix: one C original, three targets, no divergence

```
$ grep -cE '^#\s*(if|ifdef|ifndef|elif).*(H2W|SERVERONLY|H2W_INTEGRATED)' engine/h2shared/cvar.c
0
$ grep -n "cvar.c" engine/CMakeLists.txt
627:    ${COMMONDIR}/cvar.c
1307:        ${COMMONDIR}/cvar.c
```

`cvar.c` is compiled from `COMMON_SOURCES` for `glhexen2`/`h2ded` and from
`HWSV_SOURCES` for `hwsv`, with **no** target-conditional arm — unlike
`msg_io.c` (H2W-only symbols) or `huffman.c` (two allocation strategies). The
Rust module can therefore be a single translation of one code path, and the
differential harness has exactly one C to compare against.

Symbols the port will reference, counted in one build of all three binaries
(`cmake -B engine/build -S engine -DBUILD_DEDICATED=ON -DBUILD_HEXENWORLD=ON`;
`nm engine/build/bin/<bin>`):

| symbol | glhexen2 | h2ded | hwsv | note |
|---|---:|---:|---:|---|
| `Con_Printf` → `CON_Printf` | 1 | 1 | 1 | the macro's target (see §5) |
| `Cmd_Argv` | 1 | 1 | 1 | |
| `Cmd_Argc` | 1 | 1 | 1 | |
| `Cmd_AddCommand` | 1 | 1 | 1 | |
| `Cmd_Exists` | 1 | 1 | 1 | |
| `Z_Malloc` | 1 | 1 | 1 | |
| `Z_Free` | 1 | 1 | 1 | |
| `Z_Strdup` | 1 | 1 | 1 | |
| `q_strlcpy` | 1 | 1 | 1 | |
| `fprintf` | U | U | U | libc; compiles to `__fprintf_chk@GLIBC` under fortify |

None is HexenWorld-only, so the #233 rule (every extern must resolve in every
target that links the one-object archive) is satisfied.

## 2. Ownership

| thing | owner | rule for the port |
|---|---|---|
| `cvar_vars` list head (`cvar.c:25`) | the port | Rust keeps it as its own static. C never names it (it is `static`), so the port may own it outright. |
| each `cvar_t` node | **the caller** | 105 files under `engine/` reference `cvar_t`, nearly all as file-scope globals. `Cvar_RegisterVariable` links the caller's pointer into the list. Rust must never allocate, copy, move or free a node. |
| `var->string` and `var->default_string` | the port, but allocated and released with the C allocator | `Z_Strdup`/`Z_Malloc`/`Z_Free` (`cvar.c:163,183,184,331`), mostly `Z_MAINZONE`. Using a Rust allocator would make the two implementations non-interchangeable and break `Z_Free` from C. |
| `cvar_aliases[32]` + `num_cvar_aliases` (`cvar.c:578-579`) | the port | a fixed-size static array of plain structs; entirely internal, no caller holds it. |
| `var->callback` | the caller set it; the port calls it | a C function pointer (`cvarcallback_t`) stored inside a caller-owned node. Rust must store it as-is and invoke it through the C ABI. |
| `cvar_config_dirty` (`cvar.c:138`) | the port | internal flag; also writable through `Cvar_MarkConfigDirty`. |

`Cvar_RegisterVariable` copies the caller's initial value into its own storage
before storing anything (`cvar.c:325-339`): `q_strlcpy(value, variable->string)`,
then `variable->string = NULL`, then `default_string = Z_Strdup(value)`, then
`Cvar_SetQuick`. So a caller's string literal is never retained and never freed.

**Checked and safe, though it reads like a double free.** `Cvar_RegisterAlias`
seeds `alias->string = target->default_string` (`cvar.c:661`) and then calls
`Cvar_SetQuick(alias, target->string)`. If `Cvar_SetQuick` took the free path
(`cvar.c:183`) it would release the *target's* default string. It cannot:
`Cvar_RegisterVariable(alias)` runs in between and replaces both
`alias->string` and `alias->default_string` with fresh copies of the seeded
value (`cvar.c:325-339`). The seeding is a read of the target's default, not a
sharing of it. The port must preserve exactly this ordering — seed, register
(copy), then set — or it introduces the bug the C does not have.

## 3. Re-entrancy

`Cvar_SetQuick` invokes `var->callback(var)` as its last act (`cvar.c:192-193`),
and the callbacks call back into `Cvar_*`: `SV_Callback_Serverinfo` is installed
on twelve server cvars (`engine/hexenworld/server/sv_main.c:1313-1324`) and
reads other cvars while running. So the port is re-entered while it is inside a
mutation.

The rule the port follows, which is what makes that safe:

- **No Rust-owned intermediate state across the callback.** The list is walked
  with raw pointers into caller-owned nodes; there is no `Vec`, no `RefCell`, no
  borrow, no guard, and no cached count that the callback could invalidate.
  (This is the `link_ops.rs` precedent: the port mutates caller-owned state and
  says so.)
- **`cvar_vars` is re-read, not captured.** If a callback registers a variable,
  the new head must be visible to the code that resumes after the callback.
- **Alias ping-pong terminates for the reason the C documents** (`cvar.c:601-603`):
  `Cvar_SetQuick` returns before touching the callback when the value already
  equals what is being set, so the mirror stops on the first hop back. The port
  must keep the early return *above* the callback for this to hold.
- **Callback installation order is part of the contract.** An alias installs
  `Cvar_AliasCallback` on the target and chains the target's previous callback
  through `a->target_callback` (`cvar.c:669-681`). Callers that install their
  own callback afterwards replace the chain — which is why `gl_vidsdl.c:2951-2957`
  installs `VID_ConSize_f` *before* registering the alias. The port must chain,
  not replace, and must not reorder those two steps.

## 4. Threading

There is exactly one `SDL_CreateThread` in the engine
(`engine/hexen2/host_async.c:278`, the background save worker), and that file
contains no `Cvar_*` reference at all:

```
$ grep -rn "SDL_CreateThread\|pthread_create" engine --include=*.c
engine/hexen2/host_async.c:278:	save_thread = SDL_CreateThread(SaveThread_f, "SaveThread", NULL);
$ grep -c "Cvar_" engine/hexen2/host_async.c
0
```

`Cvar_WriteVariables` is reached from `Host_WriteConfiguration`
(`engine/hexen2/host.c:585`), which the save worker does not call. So all cvar
access is main-thread access, and the port needs no synchronisation — but it
also must not add any, because a lock acquired on the main thread and re-entered
from a callback would deadlock.

## 5. ABI hazards the port has to get right

1. **`cvar_t` layout is fixed by positional initialisers.** The struct is
   `{ name, string, flags, value, integer, callback, next, default_string }` and
   `default_string` is deliberately last (`cvar.h`), because `cvar_t x = { "n",
   "v", CVAR_ARCHIVE }` appears across the tree. A compile-time offset/size
   assertion block goes in the Rust module, and the differential harness checks
   every field offset against the C, as the other ports do.
2. **`cvarcallback_t` is `void (*)(struct cvar_s *)`** — a `NULL`-able C
   function pointer. `Cvar_RegisterVariable` NULLs it unless `CVAR_CALLBACK` is
   set (`cvar.c:334-335`).
3. **`Con_Printf` is a macro, not a function**:
   `#define Con_Printf(...) CON_Printf(_PRINT_NORMAL, __VA_ARGS__)`
   (`engine/h2shared/printsys.h:53,58`). The port must call the real
   `CON_Printf(int, const char *, ...)`. Rust can declare and call a C-variadic
   extern, but it does **not** perform C default argument promotions: `%f`
   arguments must be passed as `f64`, and small integers as `c_int`, exactly as
   the C compiler would. Thirteen call sites in `cvar.c` go through this.
4. **`Cvar_WriteVariables(FILE *)`** writes `"%s \"%s\"\n"` per archived cvar
   (`cvar.c:744`); the only caller is `engine/hexen2/host.c:617`. Rust calls
   libc `fprintf` with the same format, and the harness compares the produced
   bytes, not just the fact that something was written.
5. **Diagnostics carry the function name.** `__thisfunc__` is `__func__`
   (`common/compiler.h:90`), so messages read `Cvar_Set: variable x not found`.
   The renamed C in the harness prints `c_Cvar_Set: ...`, so the harness must
   normalise the rename exactly as the wad harness does.
6. **Flag bits are public ABI** (`cvar.h:67-76`), including `CVAR_ARCHIVE`,
   `CVAR_CHANGED`, `CVAR_REGISTERED`, `CVAR_CALLBACK`, `CVAR_ROM`, `CVAR_LOCKED`.
7. **List order is observable.** `Cvar_MoveToFront` and `Cvar_FindVarAfter`
   depend on it, and moving to the front changes `WriteVariables` output order.
8. **Number formatting** goes through `atof`/`q_snprintf`/`%f` trailing-zero
   trimming (`cvar.c:189-190, 199-213`); the harness must compare
   `value`/`integer` and the string bytes for values that are not round.

## 6. What the differential harness must observe

After every call: the list order and each node's name; `string` bytes;
`default_string` bytes; `flags`; `value` and `integer`; callback invocation
count and order (a recording callback installed from the harness); the alias
table and `target_callback` chaining; `cvar_config_dirty`; the exact
`Cvar_WriteVariables` output; and every diagnostic. Plus re-entrancy cases: a
callback that sets another cvar, one that sets the cvar being set, and an alias
whose target's callback is chained.
