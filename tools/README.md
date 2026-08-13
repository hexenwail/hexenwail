# tools/

Developer analysis tooling. Nothing here is a build input — `tools/` is
deliberately absent from `filteredSrc`'s allowlist in `flake.nix`, so editing
anything in this directory leaves the engine's derivation hash untouched and
invalidates nobody's cached build. Keep it that way: if something in here ever
needs to run during a build, move it to `scripts/` instead.

These are plain scripts, run directly. The Python ones need only stock
`python3` (no third-party modules) except `pak_extract.py`, which needs Pillow.

| script | what it does |
| --- | --- |
| `qcdis.py` | disassemble `progs.dat` bytecode |
| `edict_pick.py` | pull entities of interest out of an `edicts` console dump |
| `headless-cfg.sh` | generate a scripted-run config that emits those dumps |
| `headless-drive.sh` | drive the engine's menus under Xvfb with real key events |
| `pak_extract.py` | extract textures/skins/GFX from a PAK to PNG |
| `upscale-pak.sh` | extract and AI-upscale PAK textures to TGA overrides |

---

## qcdis.py — progs.dat disassembler

Answers "does the HexenC in `gamecode/hc` actually match the bytecode players
run?" without building or installing anything.

```
qcdis.py <progs.dat> [function ...]     disassemble named functions
qcdis.py <progs.dat> --pseudo <func>    collapse temporaries into expressions
qcdis.py <progs.dat> --list [PATTERN]   list functions (regex, case-insensitive)
qcdis.py --check-opcodes                verify the opcode table vs common/pr_comp.h
```

The typical use is a cross-build diff:

```sh
nix build .#gamecode
diff <(tools/qcdis.py ~/hexen2/data1/PROGS.DAT --pseudo Use_TimeBomb) \
     <(tools/qcdis.py result/share/hexenwail/data1/progs.dat --pseudo Use_TimeBomb)
```

`--list` is the cheap first question — whether a symbol exists at all:

```sh
$ tools/qcdis.py ~/hexen2/sot/progs.dat --list '^TimeBomb'
TimeBombBoom                             invntory.hc:21159
TimeBombTouch                            invntory.hc:21173
```

### Why this and not `dhcc`

`utils/dcc` (built as `dhcc`) is a full *decompiler* and will hand you back
HexenC source, which is usually what you want when you are trying to read
code. Use it for that. `qcdis.py` exists for a different job:

- it needs no build step, so it works on any checkout and any `progs.dat`;
- it shows the raw statement stream, which is what you need to prove two
  builds are *semantically identical* rather than merely similar;
- `--pseudo` normalises away temporary allocation, so output from different
  compilers is directly diffable;
- `dhcc`'s own README notes it cannot decompile `switch` statements, which
  Hexen II gamecode uses heavily.

### progs.dat format assumptions

All struct layouts come from `common/pr_comp.h`; read it alongside this if you
are debugging the tool. Both on-disk versions are supported:

| | v6 | v7 |
| --- | --- | --- |
| statement | `ushort op; short a,b,c` (8 B) | `ushort pad; ushort op; int a,b,c` (16 B) |
| def | `ushort type; ushort ofs; int s_name` (8 B) | `ushort pad; ushort type; int ofs; int s_name` (12 B) |

The header is 15 little-endian ints (60 bytes); functions are 7 ints plus
`byte parm_size[8]` (36 bytes). Every section offset is bounds-checked against
the file size at load, so a truncated or non-progs file gets a clear error
rather than nonsense output.

**The v6 signedness trap.** v6 operands are declared `short`, but a progs with
more than 32767 globals overflows that field, and the engine reads it back
*unsigned* when it addresses a global — see the `(unsigned short)` casts on
`OPA`/`OPB`/`OPC` and the `PR_ConvertOldStmts` comment in
`engine/h2shared/pr_exec.c`, and the matching `(signed short)` narrowing in
`utils/hcc/hcc.c:PR_GenV6Stmts`. Only *jump* offsets are re-signed
(`IF`/`IFNOT` `b`, `GOTO` `a`, `SWITCH_*` `b`, `CASE` `b`, `CASERANGE` `c`).
`qcdis.py` mirrors this exactly. Reading all operands as signed — the obvious
thing to do from the header alone — silently corrupts roughly a third of all
functions in retail `data1/PROGS.DAT`.

### How far to trust the output

Things the tool gets right, and which are checked (see "Verification" below):
opcode names, operand decoding for both versions, jump targets, function
bounds, and named-global/field resolution.

Things to read carefully:

- **Static values, not runtime values.** Constants shown in braces —
  `time{0}`, `v_forward{0 0 0}` — are the values sitting in the globals block
  in the *file*. For a genuine compile-time constant that is the real value;
  for a runtime global it is just its initialiser, almost always zero. Do not
  read `v_forward{0 0 0}` as "this multiplies by zero".
- **Indirect calls.** `CALL*` prints the callee named by the operand's static
  value. When the call goes through an entity field (`self.th_missile`), that
  static value is whatever the linker left there — normally function 0, whose
  name is empty, so the line renders as `()`. The preceding `LOAD_FNC` tells
  you what is actually being called.
- **`--pseudo` is lossy by design.** It prints observable effects only and
  reuses the expression it last saw for a global, so a store through a pointer
  can be attributed to the expression that produced the pointer (stores into a
  freshly spawned entity show up as `spawn(...).field = ...`). It is built for
  diffing, not for reading control flow. Use plain disassembly for that.
- **Function extent is inferred.** Progs stores a start statement, not a
  length. Disassembly stops at a `DONE`/`RETURN` that butts up against the next
  function's start, and hard-stops at that start regardless. A function is also
  capped at `--max-statements` (default 400) and says so explicitly when it
  truncates — if you see that line, raise the limit rather than assuming the
  function ended.
- **`t<N>` is an unnamed global**, usually a compiler temporary. `I+` is
  `IMMEDIATE_NAME` from `common/pr_comp.h` — a pooled constant.

### Verification

`--check-opcodes` re-derives the 105-entry opcode table from the `OP_*` enum in
`common/pr_comp.h` and diffs it against the one compiled into the script. Run
it after any upstream merge that touches `pr_comp.h`:

```sh
$ tools/qcdis.py --check-opcodes
opcode table matches .../common/pr_comp.h (105 opcodes)
```

The stronger end-to-end check exploits `hcc -v6` / `-v7` emitting both formats
from one source: disassembling both builds must produce identical output.

```sh
nix build .#utils -o /tmp/u
cp -r gamecode /tmp/gc && chmod -R u+w /tmp/gc
(cd /tmp/gc && /tmp/u/bin/hcc -src hc/h2 -os -v7)      # v7 alongside the v6 from .#gamecode
```

As landed, that comparison is clean over all 1631 functions. It is also what
caught the signedness bug described above: reading v6 operands as signed made
570 of those 1631 functions disagree between the two encodings of identical
source.

---

## edict_pick.py + headless-cfg.sh — scripted runs and entity dumps

A pair. `headless-cfg.sh` writes a config that starts a map, waits, throws an
item and calls `edicts` at several points, bracketing each dump with a
`ZZZ<TAG>` marker line. `edict_pick.py` splits the resulting log back on those
markers and prints only the classnames and fields you asked for — an `edicts`
dump is thousands of lines and unreadable raw.

The delays are built from a `wait` alias ladder (`w5`/`w25`/`w125`) because the
console has no `sleep`; one `wait` is one frame. No X server is involved, so
this works on a dedicated server or any headless box.

```sh
tools/headless-cfg.sh 2 demo1 /tmp/run.cfg 108     # class 2 (crusader), throw impulse 108
glhexen2 -basedir ~/hexen2 -condebug +exec /tmp/run.cfg > /tmp/run.log
tools/edict_pick.py /tmp/run.log player,timebomb
```

```
==================== PRE ====================
  EDICT 1: classname=player origin='-918.0 -2034.0 0.0' movetype=3.0 ... health=74.0
==================== DUMP1 ====================
  ...
```

Pass `all` as the classname list to keep every entity, and `--fields` to change
which fields are printed and in what order.

---

## headless-drive.sh — menu testing without a second machine

Runs the engine under Xvfb and sends **real X key events** with `xdotool`, so
input arrives through SDL and `Key_Event` exactly as it would from a keyboard.
Menu code paths therefore actually execute — this is not a console script
pretending to be a user, and it is why menu behaviour no longer needs a
separate Windows box to verify. It screenshots each step with ImageMagick
`import`.

```sh
nix build .#default
ENGINE=result/bin/glhexen2 tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

Requires `Xvfb`, `xdotool`, `bwrap` and `import`; it checks for all four and
fails with a clear message rather than hanging. On a machine without them:

```sh
nix shell nixpkgs#xvfb nixpkgs#xdotool --command \
  env ENGINE=result/bin/glhexen2 tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

The run is wrapped in `bwrap` with a throwaway directory bound over `$HOME`, so
it cannot touch your real `~/.hexen2` config or savegames, and with the basedir
re-bound **read-only on top** — the bind order matters, because the game data
usually lives inside `$HOME` and would otherwise be hidden by the first bind.
The sandboxed `~/.hexen2/qconsole.log` is copied into the output directory.

Output goes to `<outdir>`: numbered PNGs, `engine.stdout`, `xvfb.log`,
`qconsole.log`, and the sandbox HOME under `work/`.

The scenarios in the `case` block (`noportals`, `oldmission_paladin`,
`oldmission_demoness`, `demoness_skin`, `newmission`) are the ones written for
`uhexen2-uh5c`; treat them as worked examples. The reusable parts are the
harness itself and the `shot` / `key` / `keyn` / `typ` helpers — add a scenario
rather than rewriting the setup.

Timings are deliberately generous (the engine gets 25 s to load a map). It is
slow, and that is the tradeoff for driving a real event loop.

---

## Provenance

`qcdis.py`, `edict_pick.py`, `headless-cfg.sh` and `headless-drive.sh` were
written to settle `uhexen2-9uzt` — a report that Glyph of the Ancients
projectiles drop at the player's feet. Disassembling and diffing the four glyph
launch functions showed them semantically identical between retail
`data1/PROGS.DAT` and Storm Over Thyrion, so the behaviour was stock class
design and not a defect.

The same pass surfaced `uhexen2-qj8s`: our `gamecode/hc/h2/invntory.hc` defines
`TimeBombThink` (the Crusader glyph orbits the player on a 5 s fuse) where
retail, portals and SoT all lack that symbol and use `TimeBombBoom`, which
detonates in place at +0.75 s. That divergence matters now that compiled
gamecode ships in the release zip, and `qcdis.py` is how you re-check it.
