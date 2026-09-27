# cmd.c: ownership, behaviour and the target model

Written before the `cmd.c` port starts, as #61 requires and as the cvar and zone
slices did. Baseline `origin/master` `dd37696a8`. Slice issue: #290. Refs #61,
#233, #42.

`engine/h2shared/cmd.c` is 1190 lines: 24 externally-linked definitions plus the
exported `cmd_source` global, and 32 distinct `Cmd_*`/`Cbuf_*` symbols reached
from outside the file across 1017 references. The per-target mechanism these
differences need is already decided on #233 (option (a), the per-target constant
shim landed with the zone port), so nothing here reopens it.

## 1. Ownership

| thing | owner | rule for the port |
|---|---|---|
| `cmd_function_t` nodes (`cmd.c:41-46`) | **the port** | `Hunk_AllocName (..., "commands")` (`:703`), never freed. `Cmd_AddCommand` refuses once `host_initialized` (`:683`) because a hunk allocation then would be stomped. |
| `cmd->name` | **the caller** | stored as given, typically a string literal; never copied, never freed. |
| `cmd->function` | the caller | an `xcommand_t` C pointer the port stores and invokes, never invents. |
| `cmdalias_t` nodes and `value` (`:32-38`) | **the port** | `Z_Malloc (..., Z_MAINZONE)` (`:496`); `value` is `Z_Strdup`'d (`:518`) and `Z_Free`'d on replacement (`:489`) and removal (`:548`). The whole alias table is port-owned. |
| `cmd_argc`, `cmd_argv[MAX_ARGS]` (`:48-49`, `MAX_ARGS 80` at `:24`) | the port | every token is `Z_Strdup`'d and freed on the next `Cmd_TokenizeString` (`:636`). |
| `cmd_args` (`:51`) | **points into the caller's text** | set to the tokenizer's `text` argument (`:659`), not copied; it is only meaningful while the caller's buffer lives. Reproduce, do not "fix". |
| `cmd_text` (`:26`) and its storage | the port | `SZ_Init (&cmd_text, NULL, 8192)` (`:92`) allocates through `Hunk_AllocName (length, "sizebuf")`, so the command buffer is hunk memory owned by the port through **the Rust sizebuf and zone ports**. |
| `cmd_source` (`:39`) | the port | exported global, written by `Cmd_ExecuteString` and read by callers. |
| `cmd_wait` (`:53`) | the port | set by `Cmd_Wait_f` (`:67`), consumed by `Cbuf_Execute`. |
| `cfg_enginedefaults` (`:327`) | **the port**, as a cvar | registered with `Cvar_RegisterVariable (&cfg_enginedefaults)` at `:1186`, i.e. into the Rust cvar list; read at `:416` to decide whether `exec default.cfg` is honoured. |
| `cmd_engine_defaults[]` / `cmd_builtin_hexenrc[]` (`:329`, `:357`) | the port | static const text; `Cmd_StartupScript` returns one of them. |
| `static qboolean done` in `Cmd_StuffCmds_f` (`:247`) | the port | one-shot latch; the function does nothing the second time. |

## 2. Behaviour that has to survive literally

1. **The tokenizer** (`:630-671`): clears first — frees every previous
   `cmd_argv` entry, sets `cmd_argc = 0` and `cmd_args = NULL` — then skips
   bytes `<= ' '` except `\n`, treats `\n` as end-of-command, parses with
   `COM_Parse` (`com_token`), records `cmd_args` when the first token is taken,
   and clamps at `MAX_ARGS` (extra tokens are parsed and discarded).
   `COM_Parse`/`com_token` stay in C (`common.c`, all targets).
2. **`Cbuf_Execute`** (`:168-236`): splits on a `;` outside quotes or on `\n`,
   clamps a line to 1024 bytes, removes the consumed text with `memmove`,
   executes, and breaks out leaving the remainder for the next frame when
   `cmd_wait` was set.
3. **`Cbuf_AddText` overflow** (`:118-122`) is a diagnostic and a silent drop,
   not a truncation, and it triggers when `cursize + l >= maxsize`.
4. **Dispatch order** (`:818-885`): functions (case-insensitive, first match
   wins) → aliases (`Cbuf_InsertText` of the value) → `Cvar_Command` → a static
   list of removed legacy commands, silently ignored → `Unknown command "%s"`.
5. **Alias composition** (`:447-525`): the value is `argv[1..]` joined with
   single spaces plus a trailing `\n`, built in a 1024-byte buffer; overflow
   replaces the value with a bare `"\n"` rather than truncating. A repeated
   name reuses the node and frees the old value.
6. **`Cmd_AddCommand`** (`:679-710`): refuses after `host_initialized`
   (`Sys_Error`), refuses a name that is already a cvar (`Cvar_VariableString`
   non-empty) and a name already registered, with a diagnostic each.
7. **`Cmd_CheckCommand`** (`:758-789`) checks commands, then **walks the cvar
   list directly** — `Cvar_FindVarAfter ("", CVAR_NONE)` then `var->next` — then
   aliases. This is why the port needs the `cvar_t` layout, not just cvar
   functions.
8. **`Cmd_MoveToFront`** (`:791-806`): the same list surgery as the cvar port's,
   with the same "only the node after the match moves" shape.

## 3. The target model

| | glhexen2 | h2ded | hwsv |
|---|---|---|---|
| defines | `-DGLQUAKE` | `-DSERVERONLY` | `-DH2W -DSERVERONLY` |
| client-only list surface (`:906-1066`) | present | absent | absent |
| registrations of `commands`/`cmdlist`/`cvarlist`/`aliaslist` (`:1173-1179`) | present | absent | absent |
| `#if defined(H2W)` dispatch arm (`:835`) | absent | absent | present, takes the `Sys_Printf ("FIXME: command %s has NULL handler function")` branch |
| `Cmd_ForwardToServer()` arm | **not built anywhere** | | |
| `Cmd_StartupScript` built-in `hexen.rc` fallback (`:374-383`) | absent | absent | absent |

The startup-script row was missed in the first pass of this note, which swept for
`H2W`/`SERVERONLY`/`H2W_INTEGRATED` and not for `__EMSCRIPTEN__`: under the
browser build `Cmd_StartupScript` returns the compiled-in
`cmd_builtin_hexenrc` instead of `"exec hexen.rc\n"`, because a wasm client
normally has no loose `hexen.rc` and would otherwise never exec `default.cfg`
or `config.cfg`. That target is the **wasm client**, not one of the three
native binaries: glhexen2, h2ded and hwsv all report the fallback as absent,
and the WebAssembly build is the only one that compiles it in.

Three predicates therefore come from the per-target shim, in the same shape as
`Zone_Target*`: `Cmd_TargetHasClientLists` (not `SERVERONLY`),
`Cmd_TargetIsH2W`, and `Cmd_TargetHasBuiltinStartupScript`
(`__EMSCRIPTEN__`). The first is not cosmetic:
registering the four list commands in `h2ded`/`hwsv` would give those targets
commands the C does not have, and `Cmd_Exists` is what decides whether a cvar
name is refused, so it is observable from the cvar port.

**Verified per binary** after the port landed (disassembly of a clean build), in
the order **client-lists / is-H2W / builtin-startup**: glhexen2 `1/0/0`, h2ded
`0/0/0`, hwsv `0/1/0`.  All three native binaries therefore report the startup
fallback as `0`; only the wasm client will see it set, which the PWA job builds.

`Cmd_ForwardToServer()` needs `H2W && !SERVERONLY`, a configuration **no target
in this tree builds** (glhexen2 is `GLQUAKE` + `H2W_INTEGRATED`, never `H2W`;
hwsv is `H2W` + `SERVERONLY`). As with `info_str`'s never-compiled client
variant, the port documents that branch rather than referencing a symbol that
does not exist everywhere — which is exactly the #233 rule.

**The NULL handler.** The H2W arm exists because an H2W *client* registers
commands with a NULL handler and forwards them to the server. No command in this
tree registers a NULL handler (`grep` finds none), so in glhexen2 and h2ded the
C's `cmd->function ()` would be a call through NULL and is unreachable, and in
hwsv the FIXME branch is unreachable for the same reason. The port should still
be explicit rather than accidental: invoke the handler when there is one, print
the hwsv diagnostic when the target is H2W, and treat the remaining case as
unreachable — aborting with a diagnostic is honest where the C would fault, and
the harness pins the reachability claim.

## 4. Cross-port calls

Now Rust, so these resolve inside the archive:

- `Z_Malloc`, `Z_Free`, `Z_Strdup` (zone) — the alias nodes, the tokens.
- `Hunk_AllocName` (zone) — the command nodes and, through `SZ_Init`, `cmd_text`.
- `SZ_Init`, `SZ_Clear`, `SZ_Write` (sizebuf) — the command buffer.
- `Cvar_RegisterVariable`, `Cvar_VariableString`, `Cvar_Command`,
  `Cvar_FindVarAfter` (cvar), and the `cvar_t` layout itself.

Still C, present in every target: `COM_Parse` and `com_token` (`common.c`),
`FS_MakePath`/`FS_USERDIR`/`FS_LoadHunkFile` (quakefs, slice 4),
`Con_Printf`/`CON_Printf`, `Sys_Printf`, `Sys_Error`, `q_strcasecmp`,
`q_strlcat` (Rust strlcat), and the libc string functions.

**Two shared layouts are needed**, and both follow the pattern the cvar
module already uses (ABI half unconditional, functions behind the feature):

- `CvarC` from `cvar.rs`, for `cfg_enginedefaults` and `Cmd_CheckCommand`'s walk
  — `lib.rs` already compiles that module for `any (cvar, info_str)`; `cmd`
  joins that list.
- `QuakeParmsC` from `zone.rs`, because `com_argc`/`com_argv` are macros over
  `host_parms->argc/argv` (`common.h:92-93`), not symbols. `zone.rs` currently
  compiles every function whenever the module is included, so sharing it means
  gating the zone functions on `feature = "zone"` (as `cvar.rs` does) and
  compiling the module for `any (zone, cmd)`. A separate shared module would be
  the alternative; the guards are the smaller change and keep the layout in the
  port that owns it.

## 5. What the differential harness must observe

After every call: the registry order and each command's name and handler
identity; the alias list, names and value bytes; the tokenizer's `cmd_argc`,
every `cmd_argv` byte, and `cmd_args` (its offset into the caller's buffer
rather than its address, since the two arms use different buffers); `cmd_source`;
`cmd_wait`; the whole `cmd_text` buffer and cursor; the registered command names
in order; and every diagnostic. Failure paths — unknown command, alias loops,
malformed quoting, missing `\n`, buffer overflow, `Cmd_AddCommand` after
`host_initialized`, alias and command name collisions — run in child processes
where the C aborts.
