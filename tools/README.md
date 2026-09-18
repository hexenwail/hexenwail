# tools/

Developer analysis tooling. `tools/` is not an input to the engine's derivation
in `flake.nix`, so editing anything here does not rebuild the engine.

**Two exceptions:** `tools/qcdis.py` and `tools/check_progs_fields.py` are
build inputs to `.#gamecode`, which runs them in its check phase. Editing
either one rebuilds `.#gamecode`. A new build-time tool must be added to the
`gamecodeSrc` filter in `flake.nix`, or belongs in `scripts/` instead.

Some scripts are CI gates, run from `.github/workflows/lint.yml`:
`menu_soft_parity.py`, `test_software_model_flags.py`,
`test_multiplayer_join.py` and `ironwail_scorecard.py --check`.

Run the scripts directly. The Python ones need only stock `python3`, except
`pak_extract.py`, which needs Pillow.

| script | what it does |
| --- | --- |
| `qcdis.py` | disassemble `progs.dat` bytecode |
| `edict_pick.py` | pull entities of interest out of an `edicts` console dump |
| `headless-cfg.sh` | generate a scripted-run config that emits those dumps |
| `headless-drive.sh` | drive the engine's menus under Xvfb with real key events |
| `serve.sh` | drive the dedicated server (`h2ded`) on stdin, with no X at all |
| `pak_extract.py` | extract textures/skins/GFX from a PAK to PNG |
| `upscale-pak.sh` | extract and AI-upscale PAK textures to TGA overrides |
| `progs_crc.py` | print the two CRCs `PR_ClassifyGamecode()` identifies a `progs.dat` by |
| `test_multiplayer_join.py` | compile extracted production join/filesystem functions and test protocol fallback, gamedir validation, HW/Siege precedence, and restoration |

---

## qcdis.py — progs.dat disassembler

Checks whether the HexenC in `gamecode/hc` matches the bytecode players run,
without building anything. To read gamecode as source, use `utils/dcc`
(`dhcc`) instead; `qcdis.py` is for proving two builds are semantically
identical.

```
qcdis.py <progs.dat> [function ...]     disassemble named functions
qcdis.py <progs.dat> --pseudo <func>    collapse temporaries into expressions
qcdis.py <progs.dat> --list [PATTERN]   list functions (regex, case-insensitive)
qcdis.py --check-opcodes                verify the opcode table vs common/pr_comp.h
```

Cross-build diff:

```sh
nix build .#gamecode
diff <(tools/qcdis.py ~/hexen2/data1/PROGS.DAT --pseudo Use_TimeBomb) \
     <(tools/qcdis.py result/share/hexenwail/data1/progs.dat --pseudo Use_TimeBomb)
```

Does a symbol exist at all:

```sh
$ tools/qcdis.py ~/hexen2/sot/progs.dat --list '^TimeBomb'
TimeBombBoom                             invntory.hc:21159
TimeBombTouch                            invntory.hc:21173
```

Both progs versions (v6 and v7) are supported; struct layouts come from
`common/pr_comp.h`. v6 operands are read unsigned except jump offsets, matching
the engine (`engine/h2shared/pr_exec.c`) and `hcc`. Reading them all as signed
corrupts about a third of the functions in retail `data1/PROGS.DAT`.

Reading the output:

- **Braced constants are file values, not runtime values.** `v_forward{0 0 0}`
  is the initialiser in the globals block, not what the code multiplies by.
- **Indirect calls** through an entity field usually render as `()`. The
  preceding `LOAD_FNC` shows what is actually called.
- **`--pseudo` is lossy.** It is for diffing, not for reading control flow.
- **Function extent is inferred.** A function is capped at `--max-statements`
  (default 400) and prints a truncation line when it hits it; raise the limit
  rather than assuming the function ended.
- **`t<N>`** is an unnamed global, usually a compiler temporary. `I+` is an
  `IMMEDIATE_NAME` pooled constant.

### Verification

Run `--check-opcodes` after any upstream merge that touches `pr_comp.h`:

```sh
$ tools/qcdis.py --check-opcodes
opcode table matches .../common/pr_comp.h (105 opcodes)
```

End-to-end check: build the same source as v7 and v6, and the disassembly of
both must be identical.

```sh
nix build .#utils -o /tmp/u
cp -r gamecode /tmp/gc && chmod -R u+w /tmp/gc
(cd /tmp/gc && /tmp/u/bin/hcc -src hc/h2 -os -v7)      # v7 alongside the v6 from .#gamecode
```

---

## edict_pick.py + headless-cfg.sh — scripted runs and entity dumps

`headless-cfg.sh` writes a config that starts a map, waits, throws an item and
calls `edicts` at several points, marking each dump with a `ZZZ<TAG>` line.
`edict_pick.py` splits the log on those markers and prints only the classnames
and fields you ask for. No X server is needed.

```sh
tools/headless-cfg.sh 2 demo1 /tmp/run.cfg 108     # class 2 (crusader), throw impulse 108
glhexen2 -basedir ~/hexen2 -condebug +exec /tmp/run.cfg > /tmp/run.log
tools/edict_pick.py /tmp/run.log player,timebomb
```

Pass `all` as the classname list to keep every entity, and `--fields` to choose
which fields are printed and in what order.

---

## serve.sh — the dedicated server on stdin

Try this first for anything below the renderer: gamecode, physics, savegames,
protocol, filesystem, cvars. `h2ded` boots in about a second and needs only
`bwrap`. It cannot show you what anything looks like; use `headless-drive.sh`
for that.

```sh
nix build .#h2ded-bundled -o result-h2ded
nix build .#demodata      -o result-demodata
printf 'map demo1\nwait 3\nstatus\nedicts\n' | \
  tools/serve.sh /tmp/out result-demodata/share/hexenwail
```

One console command per line. `wait N` sleeps N seconds (the engine's own
`wait` is one frame). A final `quit` is appended for you.

Read `<outdir>/qconsole.log`: **h2ded prints nothing to stdout when stdout is
not a tty**, so `engine.stdout` is normally empty. `-condebug` is passed for
you, and the run uses a throwaway `$HOME`.

---

## headless-drive.sh — menu testing under Xvfb

Runs the engine under Xvfb and sends real X key events with `xdotool`, so menu
code runs exactly as it would from a keyboard. Each step is screenshotted with
ImageMagick `import`.

```sh
nix build .#default
ENGINE=result/bin/glhexen2 tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

Requires `Xvfb`, `xdotool`, `bwrap` and `import`. Without them:

```sh
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env ENGINE=result/bin/glhexen2 tools/headless-drive.sh noportals /tmp/out ~/hexen2
```

(`Xvfb` ships in `xorg-server`; there is no `nixpkgs#xvfb`.)

The run uses a throwaway `$HOME` with the basedir bound read-only, so it cannot
touch your real config or savegames. Output in `<outdir>`: numbered PNGs,
`engine.stdout`, `xvfb.log`, `qconsole.log`, and the sandbox HOME under `work/`.

**`script` is the general-purpose scenario.** It reads steps from `$STEPS`, one
verb per line:

```sh
printf 'shot 01-main\nkeyn Down 3\nkey Return\nshot 02-mods\n' > /tmp/steps.txt
STEPS=/tmp/steps.txt tools/headless-drive.sh script /tmp/out ~/hexen2
```

Verbs: `shot` / `shotf` / `key` / `keyn` / `type` / `enter` / `console` / `cmd`
/ `hold` / `mouse` / `click` / `wipe` / `sleep`, plus `#` comments. `type`
types without submitting, `enter` submits, and `cmd` wraps one console command
in the grave-key toggle. An unrecognised verb aborts the run.

The other `case` arms are worked examples. Add one only when a run needs real
logic (a loop, a control condition). Timings are generous: the engine gets
25 s to load a map.

### console_tab — TAB completion

```
tools/headless-drive.sh console_tab /tmp/out ~/hexen2 -condebug +"map demo1"
```

Presses TAB in a live console across eight cases. Keep both arguments:
without `+map demo1` the console cannot be opened from the main menu and the
run screenshots the untouched menu, which looks like a pass; without
`-condebug` the ambiguous-match listing is lost. Read results off the PNGs: the
line under `]` is the assertion.

---

## progs_crc.py — which gamecode is this file?

```
progs_crc.py <progs.dat> [progs.dat ...]
```

Prints the two numbers `PR_ClassifyGamecode()` (`engine/h2shared/pr_edict.c`)
works from, and flags whether the first is recognised:

- **file crc**: CRC over the whole file. Identifies one exact build; this is
  what `retail_crcs[]` holds.
- **progdefs crc**: the header's `crc` field. Identifies the interface
  generation only (`PROGS_V103_CRC` / `PROGS_V111_CRC` / `PROGS_V112_CRC`).
  Mods share these with retail, so only the file CRC tells a mod from Raven's
  file.

`retail_crcs[]` only covers 1.11/1.12a, so 1.03 or 1.09 retail gamecode is
reported as `Third-party`. To fix that, run this on the `progs.dat` from that
release and add the file CRCs.
