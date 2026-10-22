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
   crate, or must keep a link-time oracle.
4. **Mutants.** `tests/mutants.sh` applies each mutant to `src/<port>.rs`, runs the
   replay, requires a kill, restores the file. Strip compile-time asserts in the
   mutated copy, otherwise the compiler kills table mutants, not the fixtures. A
   mutant that does not compile is an error, not a kill. **Before deleting the C
   harness, run the same list against it** (pass a kill command after `--`) and
   require the same set to die. Record the list in the fixture README.
5. **Delete the C side.** `git rm` the harness `.c` and runner `.sh`. Keep the C
   original if anything reads it (notices gate, `utils/CMakeLists.txt`).
6. **Gate edit.** In `scripts/check-rust-<port>.sh`: drop the harness build/run
   step and its case-count floor; add a step that runs
   `cargo test --offline --manifest-path engine/rust/<port>/Cargo.toml`, greps for
   the replay test line `... ok`, and floors the record count. Remove `cc` from the
   tool list if nothing else needs it. Keep the symbol, single-definition and
   stray-object checks.

## Keep permanently

`check-rust-archives.sh` and `check-rust-abi.sh` (C/Rust boundary, not parity),
the symbol and link-once checks in the per-port gate, and
`check-rust-notices.sh`: it derives the copyright notices from the C original, so
the original stays in the tree. Do not delete it.

## Checklist

- [ ] `gen_fixtures.sh --check` clean; `mutants.sh` all killed; same mutants killed by the live harness first
- [ ] `grep -rn "<port>/tests\|run_diff_harness" scripts .github engine/CMakeLists.txt` finds no stale reference to the deleted files (other ports' gates may name their own harness; leave those)
- [ ] `check-rust-<port>.sh`, `check-rust-notices.sh` and `--self-test`, `check-rust-archives.sh`, `shellcheck -x` on touched scripts, then one `check-rust.sh`
- [ ] new scripts pass `shellcheck -x` (`lint.yml` lists only `check-rust.sh`, `-abi`, `-notices`; per-port gates are checked by hand)
- [ ] Real-engine evidence per #475 (demo replay or `tools/hw-drive.sh` smoke) before deleting a non-leaf oracle

## Watch for

- The fixture is only as good as the C original; the same-author caveat in #475 applies.
- Do not claim a time win for leaf ports: the crc harness ran in about 1 s of a
  16 min `check-rust.sh`. The gain is decoupling from vanilla, not wall time.
- `saturating_sub` on a signed zero goes negative; write mutants that actually
  compile and behave as intended, and read the kill reason once.

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
  `mutants.sh` runs the replay under `timeout 60` and counts that as a kill.
- **Pilot bug: `mutants.sh` swallowed the broken-mutant message.** `crc`'s loop
  ran the kill command with `2>&1` sent to `/dev/null`, so `exit 2` from a mutant
  that did not compile printed nothing. `mathlib/tests/mutants.sh` captures stderr
  and prints it on exit code 2; port that fix to `crc` if you touch it.
- **Two-sided table.** Run the same list against the live harness first: it
  killed 28 of 38 here, and the replay killed all 38. Keep the mutants that only
  the replay kills; they are what the harness was missing.
