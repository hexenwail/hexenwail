# zone.c: the per-target variant matrix, and the #233 decision it forces

Written before the `zone.c` port starts, because `zone.c` is the first engine
file whose behaviour differs **per target in a way one shared archive cannot
express**. Baseline `origin/master` `141e3dd1b`; issue #288; the decision this
feeds is #233; refs #61, #42.

Every claim below is from the preprocessed translation unit, not from reading
the `#if`s by eye:

```
nix develop .#default -c bash -c '
  cc -E -DGLQUAKE                       -I engine/hexen2 -I engine/h2shared -I common engine/h2shared/zone.c > /tmp/z_gl.i
  cc -E -DSERVERONLY                    -I engine/hexen2 -I engine/h2shared -I common engine/h2shared/zone.c > /tmp/z_h2ded.i
  cc -E -DH2W -DSERVERONLY -I engine/hexenworld/server -I engine/hexenworld/shared -I engine/h2shared -I common engine/h2shared/zone.c > /tmp/z_hwsv.i'
```

## The matrix

| | `glhexen2` | `h2ded` | `hwsv` |
|---|---|---|---|
| defines | `-DGLQUAKE` | `-DSERVERONLY` | `-DH2W -DSERVERONLY` |
| `ZONE_DEFSIZE` (main zone default) | `0x200000` (2 MB) | `0x100000` (1 MB) | `0x100000` (1 MB) |
| `SECZONE_SIZE` = `MEM_STATIC_TEX + MEM_CODEC_MEM` | `0x40000` | `0` | `0` |
| secondary-zone block in `Memory_Init` | compiled in, `if (!isDedicated)` at runtime | compiled **out** | compiled **out** |
| `Cache_Init ()` in `Memory_Init` | present | absent | absent |
| `Cmd_AddCommand ("flush", Cache_Flush)` | present | absent | absent |
| whole `Cache_*` API (`zone.c:561-877`) | present | absent | absent |
| `isDedicated` | variable, `engine/hexen2/sys_unix.c:54`, set from `-dedicated` | variable `= true`, `engine/hexen2/server/sys_unix.c:49` | **macro `1`**, `engine/hexenworld/server/host.h:50` — no symbol |
| `Cache_FreeLow` / `Cache_FreeHigh` | functions | macros expanding to nothing | macros expanding to nothing |

Two constants and one symbol are therefore target- or build-dependent:

- **`MEM_CODEC_MEM` is always 0.** `CODECS_USE_ZONE` is named only in a comment
  (`h2config.h:192`) and never defined, and `LIBMAD_NEEDMEM` / `VORBIS_NEEDMEM`
  are defined nowhere in the tree. So `SECZONE_SIZE == MEM_STATIC_TEX`, and the
  secondary zone is an ordinary 256 KB in the client and nothing elsewhere.
- **`ZONE_MINSIZE` (1 MB) and `ZONE_MAXSIZE` (8 MB) are dead defines** — no use
  anywhere in the tree. They are not part of the matrix.
- **`isDedicated` is not a linkable symbol in `hwsv`.** That is the #233 rule
  biting directly: the consolidated archive is one object, `hwsv` links it, and
  an `extern static mut isDedicated` in the Rust module would be an undefined
  reference in the one target that defines it as a macro instead.

## Why one archive cannot express this

Phase 0 consolidated the ports into one static library because separate
staticlibs each carried compiler-builtins and a panic/personality definition.
One CMake configuration builds `glhexen2`, `h2ded` and `hwsv`, and all three
link the *same* native archive, built once, with one set of Cargo features.
`ZONE_DEFSIZE` is chosen by `SERVERONLY`, which differs between those binaries,
so no single implementation can carry both values — a port that hardcodes
`0x200000` gives the dedicated servers 1 MB too much; one that hardcodes
`0x100000` shrinks the client's main zone by half.

The secondary zone is not the same problem: it is *gated on `isDedicated`*, and
in the shipped matrix `SERVERONLY` implies a dedicated process, so an
implementation that always compiles the block and evaluates the gate would
create no secondary zone in `h2ded`/`hwsv` and the right one in the client —
if it can read the flag. It cannot read it directly (`hwsv` has a macro), which
is the second reason this needs a mechanism rather than a decision made in the
port.

The cache API's compile-time absence is **not** a problem: no `SERVERONLY`
target calls it, so the functions may exist in the archive and simply be
unreachable, exactly as `info_str`'s symbols are in the targets that do not
call them.

## The options

### (a) Per-target constant shim — recommended

One small C file compiled into every engine target with that target's own
defines, exporting what the Rust module cannot know by itself:

```c
/* engine/rust/zone_target.c -- compiled per target, like wasm_globals.c */
int Zone_TargetDefSize (void)    { return ZONE_DEFSIZE; }
int Zone_TargetSecSize  (void)   { return MEM_STATIC_TEX + MEM_CODEC_MEM; }
int Zone_TargetDedicated(void)   { return isDedicated; }   /* macro 1 in hwsv */
```

The file is compiled with `-DSERVERONLY` for `h2ded` and `hwsv` and without it
for `glhexen2`, so **one source yields three correct answers**, and the Rust
module becomes target-agnostic: it asks, it does not assume.

- Cost: one new first-party C file in the build (a migration tool, to be retired
  in Phase 11 with the other C originals); the Rust module is no longer
  self-contained for these three values.
- Precedent: `engine/rust/wasm_globals.c` is exactly this for the wasm32
  statics, and `info_str.rs` routes around a missing symbol through
  `Cvar_FindVar` for the same reason.
- Faithful in every configuration including `glhexen2 -dedicated` (2 MB main
  zone, no secondary zone), because the gate reads the live variable there.
- Keeps `zone.h` and every exported symbol unchanged, per migration rule 3.

### (b) Per-feature-set archives

CMake builds the crate once per feature set (`serveronly` and not) and links
the matching archive into each binary. This is #233's option 2.

- Cost: more than one cargo build per configuration, three archives, the
  feature matrix multiplied by every landing port, and the Windows/wasm cross
  builds multiplied with it. Each binary still links exactly one archive, so
  the duplicate-runtime-symbol property that motivated Phase 0 survives.
- Benefit: no new C, faithful compile-time variants for today's case and for
  any future HexenWorld-only code that cannot be routed around.
- Risk: it re-opens a build architecture that #248 deliberately closed, and the
  next port inherits the need to get the feature sets right.

### (c) Runtime derivation with no shim — rejected

`isDedicated ? 1 MB : 2 MB` matches `glhexen2`, `h2ded` and `hwsv` for the
zone size, but it is wrong for `glhexen2 -dedicated` (the C keeps 2 MB), and it
still cannot read `isDedicated` in `hwsv`. It also encodes a coincidence —
`SERVERONLY ⇒ dedicated` — as if it were the rule.

### (d) Pass the configuration through `Memory_Init` — considered

Widening `Memory_Init (void *buf, int size)` to carry the default zone size,
the secondary-zone size and the dedicated flag moves the same data through the
existing call, with `host.c` and `server/host.c` supplying their own values:
no new C file, and the Rust module stays pure. It changes a public header and
both call sites, which rule 3 asks to avoid where practical, and it makes every
future per-target value a signature change instead of a shim line. Listed for
completeness; (a) is the same idea with the ABI held still.

## Recommendation

**(a)**, with the shim's three functions defined exactly once per target and a
gate check that each binary reports the value its own build compiled (`nm`/
runtime assertion), so the mechanism cannot silently regress. If the intent is
to settle #233 permanently rather than to unblock `zone.c`, **(b)** is the
answer that scales — but it should then be a deliberate build-architecture
change with its own issue, not a side effect of this port.

## What this does not change

`cvar.c`, `wad.c` and the nine earlier ports have no per-target behaviour, so
neither choice touches them; their gates and the exactly-once symbol checks stay
as they are.
