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
