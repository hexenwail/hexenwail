# Sanitizer pass over the Rust port harnesses

Run 2026-09-28 against `origin/master` `915d1dcfb` (seventeen ported subsystems).
This is the first time anything in this tree has been through a memory
sanitizer: #61 lists it as an unmet quality gate, and nothing had checked it
because the differential harnesses can only see *divergence* — two
implementations that share a bug agree, and a harness that corrupts its own
fixture is invisible to them.

## What this pass does and does not cover

**Covers:** AddressSanitizer with LeakSanitizer over each port's differential
harness — the C original, the harness driver, the shim, and the Rust module
called from them. It finds memory errors in the harness substrate and in the C
reference, and it finds misuse at the FFI boundary where the allocation and the
access are both visible to the instrumented code.

**Does not cover Rust-side undefined behaviour.** Stable rustc cannot be built
with `-Zsanitizer=address` (nightly only), and this environment has no
valgrind, no clang and no Miri. So a misaligned dereference or an
out-of-bounds raw-pointer read *inside* an uninstrumented Rust module is not
detected here. That is a real limit of the pass, not a reason to skip it: the
last three defects found in review were in exactly the layer this does cover.

## Method

```bash
# A cc wrapper is needed because the harness scripts hardcode their flags;
# make sure it names the real compiler, or it will re-exec itself.
REAL_CC=$(command -v cc)
mkdir -p /tmp/asanbin
printf '#!/bin/sh\nexec %s -fsanitize=address -fno-omit-frame-pointer "$@"\n' "$REAL_CC" > /tmp/asanbin/cc
chmod +x /tmp/asanbin/cc
export PATH=/tmp/asanbin:$PATH ASAN_OPTIONS=detect_leaks=1:abort_on_error=0

# Each harness gets the feature set its own gate builds: linking the
# all-features archive does not work, because it defines the very symbols the
# individual harnesses stub (the one-object behaviour #233 describes).
cd engine/rust && cargo build --release --offline --features <the gate's list>
bash <port>/tests/run_diff_harness.sh
```

`hashindex` and `mathlib` still build from their own crates
(`engine/rust/<port>/Cargo.toml`), so they need that manifest rather than a
feature.

**One harness needs an option:** `huffman` deliberately places its packet
against an unmapped page so the C's unbounded `GetBit` (`huffman.c:184`) faults
— that fault *is* the evidence that the C overreads and the Rust must not.
AddressSanitizer intercepts it and reports the SIGSEGV through its own handler,
which the harness reads as a failure. Run it with `handle_segv=0` so the fault
reaches the harness as designed:

```bash
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 bash huffman/tests/run_diff_harness.sh
```

## Result

All seventeen harnesses pass with **zero** sanitizer reports:

| | result | | result |
|---|---|---|---|
| `cmd` | PASS (after the fix below) | `net-chan` | PASS |
| `crc` | PASS | `net-udp-hw` | PASS |
| `cvar` | PASS | `quakefs` | PASS |
| `hashindex` | PASS | `sizebuf` | PASS |
| `huffman` | PASS (`handle_segv=0`) | `strlcat` | PASS |
| `info_str` | PASS | `strlcpy` | PASS |
| `link_ops` | PASS | `wad` | PASS |
| `mathlib` | PASS | `zone` | PASS |
| `msg_io` | PASS | | |

## The one defect it found

`engine/rust/cmd/tests/diff_harness.c`'s `scenario_startup` set
`host_parms->argv` to a **block-local** `char *argv[3]` and then called
`Cmd_StuffCmds_f` after the block had closed. `Cmd_StuffCmds_f` reads the
engine's saved pointer, so both arms read a stack slot that was out of scope —
`stack-use-after-scope` at `cmd.c:259`, allocated at `diff_harness.c:997`.

It is a harness bug, not a port bug, and it is precisely the class the gate
cannot see: the two implementations agreed, because they were reading the same
stale bytes. The neighbouring case in the same function had always used a
file-scope array (`argv_plus`); the first case now does too (`argv_plain`).
Fixed in the commit that carries this note.

## What to do with this

Re-run the pass when a slice adds a harness, and before trusting a harness to
prove a pointer-heavy port. The wrapper and the per-gate feature sets are the
whole recipe; the `handle_segv` exception is the only special case.
