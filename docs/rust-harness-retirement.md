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
  the other untouched. (strlcat has since been retired the same way; see
  "Findings: strlcat".)
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

## Findings: strlcat

Worked example: `engine/rust/strlcat/` and `scripts/check-rust-strlcat.sh`. The
strlcpy crate shape, reused: a std test-only crate compiling `src/strlcat.rs`
unchanged, no C symbols to stub, no `autotests` tricks needed.

- **No divergence from C found.** The Rust and the C agree on every fixture
  record and every grid point. Nothing needed a `fix(rust):` commit.
- **The live harness only knew about a few destination shapes.** It built four
  initial destinations (empty, NUL in the last usable byte, no NUL within `siz`,
  one case-derived partial). The fixture grid instead walks every destination
  string length 0..siz, and the replay grid goes to strings longer than `siz`
  (the `dsize < strlen(dst)` case), where `siz` bounds the return value and
  nothing may be written.
- **Record shape.** The destination is recorded before and after as the
  `siz`-byte window C may look at, with junk after the destination NUL so an
  append that clobbers the tail shows. Bytes past `siz` are not recorded (C
  never reads them); the generated grid in the replay lays a longer string out
  past `siz` and requires it untouched.
- **A mutant that is equivalent.** `n <= 1` for `n == 0` on the no-room return:
  with `n == 1` the write pointer already sits on the NUL that ends `dst`, so the
  final `*d = 0` rewrites a zero. Dropped from `mutants.sh` with a note. It
  survives the live harness for the same reason.
- **Mutant kills by crash count.** The scan-unbounded mutant dereferences the
  NULL destination when `siz == 0` and dies of SIGSEGV; that is a kill, not a
  compile failure, and `mutants.sh` counts it correctly.
- **Both tables agree.** 18 mutants: the replay kills all 18 and the live
  differential harness (kill command: rebuild the engine archive, run
  `run_diff_harness.sh`, baseline checked to exit 0 first) killed the same 18.
- **Undefined in C, not matched.** Overlapping `src`/`dst` and a source with no
  NUL. Neither is in the contract and neither is in the fixture.

## Findings: hashindex

Worked example: `engine/rust/hashindex/` and `scripts/check-rust-hashindex.sh`. A
stateful table with no return values: every observable is the memory the caller
reads afterwards.

- **Freeze sequences, not calls.** The fixture is a script of operations
  (`add`, `rem`, `clr`, `free`, `alloc`) with the expected observation after each:
  a `state` line (size, mask, NULL-ness of both pointers, both arrays, as an FNV
  digest above 32 entries) and `look` lines (the chain a `Hash_First`/`Hash_Next`
  walk visits, in order). Comparing the whole line as text means one wrong array
  element or one swapped chain position fails with the line number.
- **Defined but out of contract still counts.** Remove of an absent index, remove
  under the wrong key, `Hash_Clear` leaving `indexChain` stale, and a double add
  (a cycle: lookups are bounded at `hashSize + 1`) all have defined C results.
  They are frozen. What the C leaves undefined (an out-of-range index to
  `Hash_Remove`, a remove that walks a cycle) is excluded and noted in the fixture
  README as accepted loss.
- **The crate is a `no_std` staticlib**, like mathlib: `autotests = false` and
  `[lib] test = false, doctest = false` on it, the replay in `hashindex/replay/`
  (`[[test]] path = "../tests/replay.rs"`), and `replay/src/lib.rs` registered in
  `check-rust-notices.sh`. `cargo test --no-run` works on the staticlib and the replay
  crate. (The consolidated `engine/rust` crate cannot host `cargo test`, for the same
  panic-handler reason; that was already so and is not part of any port's replay.)
- **Sys_Error with one argument.** Every message in `hashindex.c` has at most one
  `%d`. The stub takes `(fmt, int)`, which is ABI-compatible on x86-64 and aarch64
  Linux, and substitutes the int only when the template contains `%d`, so the
  replay compares the full formatted message with what the C printed instead of
  just the template. (Reading the int when no argument was passed is harmless: it
  is a register, and it is not used.)
- **Fatal cases are data.** A `fatal <setup> <op> <a> <b> <message>` record names
  the state (`zero`, `alloc:n`, `freed:n`) and the call; the generator forks the C
  once per record, the replay re-runs the test binary as a child per record
  (`fatal_child`, selected by an env var) and requires SIGABRT plus the exact
  line. No hand-kept pair of per-case functions in two languages.
- **Independent model.** The replay carries a bucket-list model of the table
  (`Vec<Vec<i32>>`, head first) and derives `hash[]` and `indexChain[]` from it,
  including the stale slots a `Hash_Clear` leaves. It runs about 900,000 seeded
  operations at sizes 1 to 4096 with free-and-reallocate at new sizes and four key
  distributions (any key, a small colliding pool, aliases of a few buckets,
  extremes). The model is what restores the old harness's million-operation volume
  cheaply; the fixture is what ties the model's rules to the C.
- **Mutants.** 25, replay kills 25; the live harness killed 19 of the same 25
  first. The six it missed were the `Hash_Allocate` re-init guard and check order,
  `Hash_Add` accepting index -1, the NULL guards on `Hash_Remove` and `Hash_Clear`,
  and a half-size `Hash_Clear`. A guard dropped in Rust usually dies by SIGSEGV on
  the null pointer, not by an assertion: still a kill, and `mutants.sh` counts it.
- **`run_engine_smoke.sh` stays.** It is not a differential harness: it runs the
  real engine (h2ded and glhexen2 on a pak load and a map load) against a C-only
  reference build and diffs the logs. Seven gates call it, including
  `check-rust-hashindex.sh --engine`; it lives in `hashindex/tests/` for history
  only and keeps that path.
- **No porting bug found.** The Rust matched the C on every frozen record. The
  freeze did tighten coverage (see the mutant table above), not the port.
