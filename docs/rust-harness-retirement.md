# Retiring a Rust-vs-C differential harness

Recipe from the `crc` pilot (#475). The harness pinned the Rust port to vanilla C
while porting. Once behavior is stable, freeze that oracle as text fixtures and
delete the C side, so deliberate fixes that diverge from vanilla do not fight it.
Leaf ports first (`mathlib`, `strlcpy`, `huffman`); `sv_main` last.

Worked example: `engine/rust/crc/` and `scripts/check-rust-crc.sh`.

## Steps

1. **Fixture generator.** Add `engine/rust/<port>/tests/gen_fixtures.c`: links the
   unmodified C original, uses the harness's seeded RNG, prints records to stdout.
   `gen_fixtures.sh` builds and runs it; `--check` diffs against the committed
   file. This is the audit trail: the fixture is always re-derivable from C.
2. **Pick cases.** Smallest set that still kills the mutants (step 4), plus the
   harness's directed edge cases. Replace exhaustive loops with the cheapest
   sweep that reaches every table entry/branch. Target tens of KB, text only.
3. **Replay test.** The engine crate is `no_std` with a panic handler, so it
   cannot host `cargo test`. Add a test-only crate `engine/rust/<port>/` (see
   `crc/Cargo.toml`, `src/lib.rs` with `#[path = "../../src/<port>.rs"]`) and
   `tests/replay.rs` using `include_str!` on the fixture. Replay at unaligned
   buffer offsets, check inputs are unmodified, and assert per-record-kind floors
   so a truncated fixture cannot read green. `.gitignore`: `/target/`, `Cargo.lock`.
   Ports whose module needs C symbols (Sys_Error, Hunk_*) need stubs in the test
   crate, or must keep a link-time oracle. If the port's own crate is the
   `no_std` staticlib (mathlib), set `autotests = false` and `[lib] test = false,
   doctest = false` there, so a plain `cargo test` on it is a no-op instead of
   trying to build `tests/replay.rs` against a panic handler. **Run replays with
   `cargo test --release`**: the engine ships release with `panic=abort`.
   Where the harness was exhaustive, restore that cheaply with an independent
   reference written in the test (crc: a bit-by-bit CCITT over all 2^24 steps;
   strlcpy: the semantic checked over the full length x size x offset grid;
   huffman: encoder size invariants over seeded round trips), not a big fixture.
4. **Mutants.** `tests/mutants.sh` applies each mutant to `src/<port>.rs`, runs the
   replay, requires a kill, restores the file. Strip compile-time asserts in the
   mutated copy, otherwise the compiler kills table mutants, not the fixtures. A
   mutant that does not compile is an error, not a kill. **Before deleting the C
   harness, run the same list against it** (pass a kill command after `--`) and
   require the same set to die. Record the list in the fixture README. The kill
   command runs `cargo test --release`.
5. **Delete the C side.** `git rm` the harness `.c` and runner `.sh`. Keep the C
   original if anything reads it (notices gate, `utils/CMakeLists.txt`).
6. **Gate edit.** In `scripts/check-rust-<port>.sh`: drop the harness build/run
   step and its case-count floor; add a step that runs
   `cargo test --release --offline --manifest-path engine/rust/<port>/Cargo.toml`,
   greps for every replay test line `... ok`, and floors the record count. **Keep
   `cc` and run `tests/gen_fixtures.sh --check` in the gate** (it is 1-4 s): it
   is the only thing that re-proves the frozen values against the C, so drift in
   the C or a hand-edited fixture must fail the gate. Keep the symbol,
   single-definition and stray-object checks.

## Keep permanently

`check-rust-archives.sh` and `check-rust-abi.sh` (C/Rust boundary, not parity),
the symbol and link-once checks in the per-port gate, and
`check-rust-notices.sh`: it derives the copyright notices from the C original, so
the original stays in the tree. Do not delete it.

## Checklist

- [ ] `gen_fixtures.sh --check` clean; `mutants.sh` all killed; same mutants killed by the live harness first
- [ ] `grep -rn "<port>/tests\|run_diff_harness" scripts .github engine/CMakeLists.txt` finds no stale reference to the deleted files (other ports' gates may name their own harness; leave those)
- [ ] `check-rust-<port>.sh`, `check-rust-notices.sh` and `--self-test`, `check-rust-archives.sh`, `shellcheck -x` on touched scripts, then one `check-rust.sh`
- [ ] new scripts pass `shellcheck -x` and are added to the list in `lint.yml`
- [ ] Real-engine evidence per #475 (demo replay or `tools/hw-drive.sh` smoke) before deleting a non-leaf oracle

## Watch for

- The fixture is only as good as the C original; the same-author caveat in #475 applies.
- Do not claim a time win for leaf ports: the crc harness ran in about 1 s of a
  16 min `check-rust.sh`. The gain is decoupling from vanilla, not wall time.
- `saturating_sub` on a signed zero goes negative; write mutants that actually
  compile and behave as intended, and read the kill reason once.

## Findings: strlcpy

- `strlcpy` and `strlcat` are separate files and gates
  (`src/strlcpy.rs`, `src/strlcat.rs`, `check-rust-strlcpy.sh`,
  `check-rust-strlcat.sh`, own harness dirs). Not entangled: retiring one leaves
  the other untouched. `check-rust-strlcat.sh` builds with `--features ...,strlcpy,strlcat`
  and keeps its live harness.
- Pointer-writing ports: record the destination as the full `siz`-byte buffer
  (started all `a5`) so the fixture captures the untouched tail, and replay in
  guard zones. `siz == 0` is called with NULL, as the C original permits.
- The port needs no C symbols, so no stubs; the old harness's `Sys_Error`,
  `CON_Printf`, `Hunk_AllocName` stubs only existed because it linked the whole
  `libengine_rs.a`. The test-only crate drops them.
- Fixture size is driven by long sources: 75 KB for 680 records. Keep random
  mixes small; the grid carries the boundary coverage.
- Mutants killed by a panic or SIGABRT (size-0 NULL write) are real kills but
  are not "could not compile"; `mutants.sh` counts them correctly.
- To run mutants against the live harness, the kill command must rebuild the
  engine archive (`cargo build --release ... --features ...`) then run
  `run_diff_harness.sh`; check the unmutated baseline exits 0 first so a build
  failure is not read as a kill.

## Findings from `huffman` (#475)

Worked example: `engine/rust/huffman/` and `scripts/check-rust-huffman.sh`. A
stateful port with a build step, two C variants and a deliberate divergence from
the C.

- **Statics.** The generator `#include`s `huffman.c` instead of linking it, so
  it can dump `HuffLookup` and `HuffFreq` (file-scope statics) as fixture
  records. The second C variant is linked separately under renamed symbols.
- **Both C variants stay audited.** The hwsv build (Hunk_AllocName) and the
  integrated client build (malloc) are both run by `gen_fixtures.sh`, which
  aborts if they differ. The fixture records one answer; the cross-check of the
  two lives in the generator, not in the gate.
- **Divergence from C becomes a record kind.** The C overreads on a crafted
  header byte, the Rust does not. The generator forks each decode against a
  guard page: survivors are `dec` (exact match required), deaths are `ovr` (the
  C's answer with a defined zero tail; the Rust must be a strict prefix). The
  replay forks too, so a Rust overread is a reported SIGSEGV, not UB.
- **Build script.** `huffman.rs` `include!`s a table `engine/rust/build.rs`
  generates from `hufffreq.h`. The test crate sets `build = "../build.rs"`, and
  `build.rs` now looks for the header from both manifest depths. Any port
  with generated input can reuse this.
- **C symbols.** Only `Sys_Error` is needed; the test crate stubs it as an
  aborting function with the fixed prefix of the variadic signature (ABI
  compatible on x86-64 and aarch64). `malloc`, `memset`, `memcpy` come from libc.
  The `Hunk_*` stub is needed only by the generator.
- **Encode tail.** After the raw fallback the encode buffer past `outlen` holds
  scratch bits from the failed compressed attempt. Not frozen; no caller reads it.
- **Compressible inputs.** Random uniform bytes almost always take the raw 0xff
  fallback with this table. Compressed packets need payloads drawn from the
  frequency table (`weighted_symbol` in the generator); without them half the
  encoder is untested. Floor the compressed-encode count separately.
- **Perl anchors.** An unanchored `s/HUFF_FREQ\[index\]/.../` rewrote the doc
  comment, not the code, and the mutant "survived". Anchor mutants on code-only
  context and read at least one mutated file.
- **Equivalent mutant.** Swapping which work slot BuildTree's merged node takes
  only matters for frequency ties; the shipped table has none among merged
  weights. Not a fixture gap; left out of `mutants.sh` with a note in the README.

## Findings from `mathlib` (#475)

- **The port already has a crate.** `mathlib/` is a `no_std` staticlib with a
  `#[panic_handler]`, so it cannot host `cargo test`. The test-only crate lives in
  a subdirectory (`mathlib/replay/`, `[[test]] path = "../tests/replay.rs"`), and
  its `src/lib.rs` is registered in `check-rust-notices.sh` like crc's.
- **C symbols: stub them, fixed arguments only.** `ffi.rs` calls the variadic
  `Sys_Error`. Stable Rust cannot define a variadic function; a stub taking just
  `fmt` links and works on the System V / AArch64 Linux ABIs. Error paths that
  never return are covered by re-running the test binary as a child
  (`current_exe()` plus an env trigger) and requiring SIGABRT and the message.
  `sin`/`cos`/`floor` resolve from the host libm; no stub.
- **Freezing the oracle found two things the live harness hid.** Its 1e-6
  tolerance and 12 angles let `anglemod` run in `f32` where C runs in `double`
  (fixed in its own commit), and `Invert24To16(256..512)` overflows an `int`, which
  is undefined in C (excluded from the fixture, noted in its README). Compare bit
  for bit by default; keep a tolerance only where libm is involved.
- **Pick mutants against the C-visible behaviour, then check for equivalents.**
  Two early mutants were equivalent (`+ 0.5` inside a `floor` of an integer;
  `num > 0` vs `num >= 0` in `NearestMultiple`) and survived honestly. Regexes
  that start with a bare token can match the doc comment first; anchor them on
  code.
- **A mutant can hang.** `GreatestCommonDivisor` with `/` for `%` loops forever.
  `mutants.sh` runs the replay under `timeout 120` and counts that as a kill.
- **Pilot bug: `mutants.sh` swallowed the broken-mutant message.** `crc`'s loop
  ran the kill command with `2>&1` sent to `/dev/null`, so `exit 2` from a mutant
  that did not compile printed nothing. `mathlib/tests/mutants.sh` captures stderr
  and prints it on exit code 2; port that fix to `crc` if you touch it.
- **Two-sided table.** Run the same list against the live harness first: it
  killed 28 of 38 here, and the replay killed all 38. Keep the mutants that only
  the replay kills; they are what the harness was missing.

## Findings: info_str (#475)

Worked example: `engine/rust/info_str/` and `scripts/check-rust-info-str.sh`. A
port with process-global state, two stubbed C symbols and a C original that
only one target compiles.

- **No porting bug.** The port, the fixture (frozen from the C) and an
  independent reference model agreed on the first run: all 918 fixture records,
  every string over `{ \ a b * }` up to length 8, 3000 seeded random sequences
  of 60 operations compared byte for byte over the whole 1024-byte buffer after
  every call, and a 34560-case key/value length x `maxsize` x filter grid. No
  `fix(rust):` commit.
- **One C variant.** `info_str.c` is in `HWSV_SOURCES` alone, so the generator
  compiles it as hwsv does (`-DH2W -DSERVERONLY`) and only that variant is
  frozen. The `#ifndef SERVERONLY` client branch is compiled into no binary.
- **A model written from the data shape, not the control flow.** The model
  treats an info string as an optional leading backslash then fields split on
  backslashes, read as key/value pairs; removal, prefixed removal, set and print
  are defined on that. It also has to reproduce every fixture record, which is
  what stops it drifting from the C. The model is the cheap way to get an
  exhaustive malformed-input sweep without a big fixture.
- **Small-alphabet sweep as a digest.** Strings over `{ \ a b * }` to length 7
  are run through ten operations in the C, and every output (return text, whole
  64-byte buffer, log) is folded into one FNV-1a digest, one fixture line. The
  replay folds the Rust the same way. The serialisation lives in both
  `gen_fixtures.c` and `replay.rs` and must change together. Use this when an
  exhaustive enumeration is cheap to run and expensive to store.
- **Pointer identity is not a contract.** `Info_ValueForKey` returns one of four
  static slots on a hit and a `""` literal on a miss, from two different return
  sites. Whether a compiler merges the two literals is not defined. The
  generator and the replay probe both sites once and record a miss as class `E`;
  hits are classed by first appearance in the run, and every slot seen so far has
  its text recorded after each call, so a slot a later (even missed) lookup
  overwrote is checked.
- **Variadic C stub.** `CON_Printf(flags, fmt, ...)` cannot be defined in stable
  Rust, so `info_str/src/lib.rs` stubs it with one fixed extra pointer argument,
  sound on the System V and AArch64 Linux ABIs (the same argument as the
  `Sys_Error` stub in mathlib). It records `[flags]text`, as the generator's C
  stub does, so every message, and the flags word, is compared. `Cvar_FindVar`
  is stubbed too, with a registered and a not-registered state and a counter for
  lookups of any name but `sv_highchars`.
- **Process-global state.** The rotation slots, the log and the cvar stub are
  global, so the replay tests share one lock; `cargo test` runs them on threads.
- **Generator pitfall.** `cvar_t sv_highchars = { "sv_highchars", "1", ... }`
  leaves `.integer` at 0 (the brace initialiser sets only name, string and
  flags). The generator sets it to 1 explicitly before anything that depends on
  it, the way the engine's registration path does.
- **Buffer windows.** For writing operations the fixture records the window
  `0 .. max(old, new) + 1` of a poison-filled buffer, so the NUL and the stale
  bytes `memmove` leaves past it are part of the record, and the generator
  asserts the C wrote nothing beyond it.
- **Mutants: 41, same set killed by both.** The replay kills all 41; the same
  list run against the live differential harness (kill command rebuilding the
  engine archive and running `run_diff_harness.sh`, baseline exit 0 checked
  first) also killed all 41, so unlike mathlib this harness had no gap the replay
  closes. Two equivalent mutants are left out and noted in `mutants.sh`
  (`l < 20` -> `l <= 20` in `Info_Print`, and the `!= 0` rewrite of the cvar
  test). A mutant can hang the replay (observed: one ran for minutes before a
  timeout was added), so the kill command runs under `timeout 60`, which counts
  as a kill.
- **Accepted losses.** A key or value segment of 512 bytes or more overruns the
  C's `pkey`/`value` stack buffers and the rotation slots, and a `maxsize` above
  the caller's buffer lets the C write past it; both are undefined in C and are
  not generated. Miss-pointer identity and the absolute rotation slot are not
  frozen.
