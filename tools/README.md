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
`test_hcc_tail_slack.py` is a gate in `ci.yml`'s `cross` job instead,
because it needs the `hcc` that job builds.

Run the scripts directly. The Python ones need only stock `python3`, except
`pak_extract.py`, which needs Pillow.

| script | what it does |
| --- | --- |
| `qcdis.py` | disassemble `progs.dat` bytecode |
| `edict_pick.py` | pull entities of interest out of an `edicts` console dump |
| `headless-cfg.sh` | generate a scripted-run config that emits those dumps |
| `headless-drive.sh` | drive the engine's menus under Xvfb with real key events |
| `serve.sh` | drive the dedicated server (`h2ded`) on stdin, with no X at all |
| `hw-drive.sh` | run `hwsv` and a client connected to it, and screenshot the HexenWorld session |
| `pimp-nodlight-check.sh` | **local, needs Storm over Thyrion in `sot/`**: live end-to-end check that `misc_modelpimp` spawnflag 64 turns a model's dynamic lights off -- loads a SoT map twice through an external `.ent` (control, then with a flag-64 pimp added) and compares `r_dlightinfo` / `mcache` (#140) |
| `pak_extract.py` | extract textures/skins/GFX from a PAK to PNG |
| `test_demobar_copies.py` | fail if the software renderer's `SCR_DrawDemoBar` (`screen.c`) drifts from the GL one (`gl_screen.c`) beyond the two allowed lines (#123) |
| `pak2pk3.py` | convert a retail v1.11 `data1` or `portals`, the Nov. 1997 demo's `data1`, or HexenWorld 0.14 - 0.15's `hw` from `pakN.pak` to `000-pakN.pk3` (DEFLATE, same members, order, duplicates and bytes; verified before it is renamed into place). Refuses content the engine would not identify as a pk3, such as the OEM release and pre-1.11 installs (#130). `--loose` packs the loose game files into `zz-loose.pk3`. Retail data1: paks 98 MB to 46 MB (#129) |
| `upscale-pak.sh` | extract and AI-upscale PAK textures to TGA overrides |
| `artifact-save-check.sh` | check on a live engine that the selected artifact survives save, relaunch and load, including old-format saves and a pre-r11 engine (#159); needs retail data and a GPU (offscreen) |
| `progs_crc.py` | print the two CRCs `PR_ClassifyGamecode()` identifies a `progs.dat` by, and the Raven release when it is one |
| `test_progs_release_ident.py` | compile `pr_raven_images[]` and its lookup out of `pr_edict.c` and check every Raven release is named, only on both CRCs, in step with `progs_crc.py` (#160) |
| `test_wateralpha_reupload.py` | compile the real `GL_WaterAlphaByte`, the `GL_LoadTextureEx` cache test and `Mod_WaterAlphaChanged`, and check a change to `r_wateralpha` re-uploads exactly the loaded EF_TRANSPARENT alias skins (#124) |
| `test_menu_mouse_grab.py` | compile `IN_ReactivateMouse` and check that focus, unpause and `_enable_mouse` take the mouse back through it, and that no menu draw function grabs it, so menu hover keeps working (#91) |
| `test_demo_mods_gate.py` | check that the `game` command (C and Rust) refuses mods without the full game, that the main menu dims MODS and says why, and that the GL glyph fade can fade (#171) |
| `test_menu_row_notes.py` | check that the Options submenus' row notes fit `M_ROWNOTE_MAX` columns and name the cvar their row actually sets (#98) |
| `test_spawn_walk.py` | check that `SV_Physics` leaves an edict created during its walk for the next frame and that every `ED_Alloc` path stamps it (#138) |
| `test_soundfont_pin.py` | check that the TimGM6mb tarball hash agrees in `scripts/fetch-soundfont.sh`, `flake.nix`, the Flatpak manifest and `web/soundfont-NOTICE.txt` (#114) |
| `test_csqc_tail_slack.py` | compile `PR_ValidateBytecode` and check that a wide operand may run into the CSQC loader's zeroed globals tail but never start there, that builtin function records are not bounds-checked, and that the loader and `PR_ExecuteProgram` keep builtins from being entered (#120) |
| `test_render_disconnected.py` | check that `V_RenderView` draws nothing while `cls.state != ca_connected`, so the loading plaque no longer redraws the map just left after a disconnect, and that the plaque's backdrop still covers that frame (#204) |
| `test_alias_mesh_key.py` | check that an alias model finds its GPU mesh through slot and generation fields in its header, never by header address (which the loader frees before the first draw), that refused meshes are marked so the draw path does not rebuild them (#134), that the builder's scratch comes from the C heap (it runs from the draw path), and that `vid_restart` drops the table without deleting dead-context names (#387) |
| `test_dedicated_heap.py` | check that h2ded, hwsv and the client's `-dedicated` path default to a 512 MB heap, that `-heapsize` still overrides it in KB (and the usage says so), and that running out names `-heapsize` in both zone.c and its Rust port (#79) |
| `test_headless_drive_binds.py` | check that `headless-drive.sh`'s engine sandbox re-binds the userdir, basedir and engine after the throwaway `$HOME` bind, so an outdir under `$HOME` still gets its `qconsole.log` back |
| `test_lerp_pair_snaps.py` | check that `r_lerp_autodetect` records every measured pose pair and snaps only those over `r_lerp_autodetect_threshold` (either direction, read live) instead of setting `MOD_NOLERP` for the whole model; compiles the real `R_AliasPairSnaps` against a mock model (#94) |
| `test_showtris_models.py` | check that `r_showtris` outlines alias models, sprites, particles and the viewmodel through the helpers their own draws use (`R_AliasModelTransform`, `R_SpriteAxes`, `R_DrawParticles_Real`, the viewmodel helpers), and stays off on GLES (#81) |
| `test_vid_floor_200.py` | check that the video mode floor is 320x200 with one constant, and that the main menu's renderer label and version watermark yield to the menu where its canvas reaches their line (#135) |
| `test_gfx_malloc.py` | check that `W_LoadWadFile` (C and Rust) and the software renderer's conchars, backtile and loading disc load with `FS_LoadMallocFile` and release with `free`, never the 256 KB secondary zone, and that the wad differential harness still checks the release (#126) |
| `test_sbar_alpha.py` | check that `scr_sbaralpha` (default 1) fades only the bottom bar on GL -- edge indicators, inventory, overlays and the minimal HUD stay opaque -- through the pic alpha in `gl_draw.c`, and that the HUD menu row drives it on GL and `sbtrans` on software (#83) |
| `test_brush_abslight.py` | check that a brush entity's `abslight` is lit at 2A/255 like a lightmap sample of the same value (the software renderer's mapping), on both GL brush paths -- the romeric6 dark pillars (#326) |
| `test_offscreen_windowed.py` | check that `VID_SetMode` keeps SDL's offscreen driver windowed, so a headless `screenshot` reads back the frame that was drawn and not a fullscreen size the EGL surface never had (#401) |
| `test_particles_pause.py` | check that `R_UpdateParticles` advances particles by the render frame time but not while `cl.paused` (snow kept falling under the PAUSED plaque) |
| `test_webgl2_es_fixes.py` | check the WebGL2 fixes: texture swizzle gated on a cap that is false in the browser (opaque alpha written instead), no multisample OIT targets on the ES tier, web screenshots taken before the frame is presented, and the boot smoke failing on WebGL/GL errors (#112) |
| `test_contrast_parity.py` | check that `contrast` multiplies the frame, clamped to Ironwail's 1..2, in the shader, the uniform, the post-process trigger and the Display menu |
| `test_max_edicts.py` | check `max_edicts` defaults to 16384 under a 32000 `MAX_EDICTS`, that sounds on entities past 8192 go out whole on protocol 100 and are dropped on 18..21, that the client reads packed sound shorts unsigned, and that `EDICT_NUM` bounds by `sv_max_edicts` |
| `test_sv_autoload.py` | check `sv_autoload` (Ironwail's 0/1/2/3) is tried by `restart` and by a same-map `changelevel` / `changelevel2`, Hexen II's two death paths, checks the save exists after waiting for the save thread, and that saves and loads record `sv.lastsave` |
| `test_maps_resume.py` | check that picking a map in the Maps browser opens Ironwail's skill step (without leaving the menu first), which offers "Resume last game" when `autosave/<map>` exists and loads it |
| `test_joy_always_active.py` | check `joy_always_active` (default 0) gates rumble and gyro steering, and only those, on the gamepad being the input last used, tracked as upstream |
| `test_softemu_spans.py` | check that `r_softemu_spans` routes the world, brush-poly and liquid texture coordinates through `SpanUV` (highp, guarded), at 8/320 of the render width (16 for liquids), and resets around each override (#176) |
| `test_multiplayer_join.py` | compile extracted production join/filesystem functions and test protocol fallback, gamedir validation, HW/Siege precedence, and restoration |
| `test_hcc_tail_slack.py` | compile the `hcc_tail_slack/` fixtures with a given `hcc` and apply the engine's operand and function-record bounds to them and to any `progs.dat` named |

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
protocol, filesystem, cvars. `h2ded` boots in about a second and needs nothing
but the engine: every run gets `-userdir <outdir>/userdir`, and `bwrap` adds a
throwaway `$HOME` and read-only data when it is installed (`NO_BWRAP=1` skips
it). It cannot show you what anything looks like; use `headless-drive.sh` for
that.

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

---

## hw-drive.sh — a live HexenWorld session

The client's HexenWorld reader (`engine/hexen2/cl_hw.c`) runs only after
`connect hw://`, so no other driver here reaches it. `hw-drive.sh` starts
`hwsv` on loopback, boots the client through `headless-drive.sh`, connects it,
and then runs your step file with the client already in the game.

```sh
nix build .#default      -o result
nix build .#hwsv-bundled -o result-hwsv
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick nixpkgs#bubblewrap --command \
  env STEPS=tools/hw-bluestep.steps ./tools/hw-drive.sh /tmp/out-hw
```

About 90 s. It ends with `client_rc=0  server saw joins: 1  client signons: 1`
and exits non-zero if the client never signed on. Output: `server.log` (the
server console), `client/` (PNGs and `qconsole.log`), `steps.txt` (your steps
with the connect preamble in front).

**It needs retail data.** `hwsv` refuses the demo: `BASEDIR` (default
`~/hexen2`) must hold `data1/pak0.pak`, `data1/pak1.pak`, `hw/pak4.pak` and
`hw/hwprogs.dat`.

The server starts with `+deathmatch 0 +coop 0 +skill 0` because the gamecode's
cheat impulses return early otherwise; set `SVARGS=` to change that. `MAP=`,
`PORT=`, `SERVER=`, `ENGINE=`, `LOAD=`, `JOIN=` and `TIMEOUT=` are described at
the top of the script.

`hw-bluestep.steps` is the worked example and the live check for issue #309:
an invisible player walks, and the first `walk-*.png` frames must show pale
blue specks around a translucent footprint. Red specks mean wire particle type
28 is being drawn as red fire again.

### console_tab — TAB completion

```
tools/headless-drive.sh console_tab /tmp/out ~/hexen2 -condebug +"map demo1"
```

Presses TAB in a live console across eight cases. Keep both arguments:
without `+map demo1` the console cannot be opened from the main menu and the
run screenshots the untouched menu, which looks like a pass; without
`-condebug` the ambiguous-match listing is lost. Read results off the PNGs: the
line under `]` is the assertion.

### Demo playback — `scripts/demo-playback-smoke.sh`

```
nix build .#default -o result && nix build .#demodata -o result-demodata
nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
          nixpkgs#bubblewrap --command ./scripts/demo-playback-smoke.sh
```

Records a demo of `demo1`, then boots a **second, fresh** engine straight into
`+playdemo`. The second boot is the whole point: a demo played in the session
that recorded it still has the listen server's `cls.netcon`, so it survives a
client that refuses to read without one. Only a cold boot has no qsocket — the
state `startdemos` puts a mod's front end in. ~2 minutes; needs no retail data.

The verdict is `couldn't exec <name>end.cfg` in `qconsole.log`, not a
screenshot: a frozen client still prints `Playing demo from …` and the
matching `start.cfg` line.

`tools/test_demo_playback_gate.py` is the free CI half of the same invariant.

### Quitting — `scripts/quit-smoke.sh`

```
nix build .#default -o result && nix build .#demodata -o result-demodata
./scripts/quit-smoke.sh
```

Loads `demo1`, issues `quit` from a cfg the way every headless harness does,
and asserts the process exits 0 inside `LIMIT` seconds. Needs no X and no
retail data — SDL's offscreen video driver is enough — so it is the cheapest
check here at ~60 s.

The assertion is the **exit status**, never a log line: the bug it guards
(GitHub #204) let the engine print everything up to and including the `quit`
and then keep rendering, because `quit` raised the confirmation prompt and a
headless run has no key to answer it with. The two lines that hang was always
reported with, `Sending clc_disconnect` and `Client player removed`, are
printed by the harness timeout's SIGTERM on the way out, *after* the hang.

---

## progs_crc.py — which gamecode is this file?

```
progs_crc.py <progs.dat> [progs.dat ...]
```

Prints the two numbers `PR_ClassifyGamecode()` (`engine/h2shared/pr_edict.c`)
works from, and names the Raven release when the pair is one it knows:

- **file crc**: CRC over the whole file. Identifies one exact build.
- **progdefs crc**: the header's `crc` field. Identifies the interface
  generation only (`PROGS_V103_CRC` / `PROGS_V111_CRC` / `PROGS_V112_CRC`).
  Releases share these (1.08 to 1.11 all declare 38488) and so do mods, so
  only the file CRC tells them apart.

`pr_raven_images[]` matches on both and covers 1.03, 1.08, 1.09, 1.10, 1.11,
portals 1.12 and 1.12a, and the November 1997 demo. A Raven image it does not know
(a localised release, say) is reported as `Third-party`: run this on it and
add the pair to `pr_raven_images[]` and `KNOWN_RAVEN` together.
`test_progs_release_ident.py` fails if the two tables drift, and with
`HCODE_ARCHIVE=<uhexen2-hcode_archive checkout>` it re-reads the archive rows
from the binaries.
