# info_str fixtures

`info_str.txt` is the frozen output of the C original,
`engine/hexenworld/shared/info_str.c`, compiled as hwsv compiles it
(`-DH2W -DSERVERONLY`), replacing the live differential harness (#475).
`../replay.rs` replays it with `cargo test --release`.

Only the SERVERONLY variant is frozen: `info_str.c` is in `HWSV_SOURCES` alone
and the `#ifndef SERVERONLY` client branch is compiled into no binary (module
comment of `engine/rust/src/info_str.rs`). If a client ever compiles the file
again, that branch needs its own records.

One record per line; `#` lines are comments. A string is one token: printable
ASCII as itself (a backslash is literal, so info strings read as info
strings), everything else as `%xx` (NUL, space, control and high bytes, `%` and
`~`), and the empty string as `~`.

    rot                                       start of a lookup run
    look <info> <key> <class> <slot>...       Info_ValueForKey
    rm <init> <key> <log> <after>             Info_RemoveKey
    rmp <init> <prefix> <log> <after>         Info_RemovePrefixedKeys (prefix: 2 hex digits)
    set <S|K> <hc> <maxsize> <init> <key> <value> <log> <after>
                                              Info_SetValueForStarKey (S) / Info_SetValueForKey (K)
    print <info> <log>                        Info_Print
    sweep <maxlen> <count> <fnv1a64>          small-alphabet sweep digest

- `look`: `class` is `E` for a miss (the empty literal) or the index of the
  returned pointer among the rotation slots seen so far in this run, in order
  of first appearance. `slot...` is the current text of every slot seen so far
  after the call, so a value a later call overwrote (a miss rewrites the slot it
  took) is visible. The two addresses of the C's `return ""` are probed once and
  both read as `E`: whether a compiler merges them is not a contract.
- `log` is what CON_Printf received, framed `[flags]text` per call (flags is
  `_PRINT_NORMAL`, 0, always). `~` when nothing was printed.
- `after` is the window of the 1024-byte buffer (started as the string, then
  0xcc poison) the call can have touched: `0 .. max(old, new) + 1`. It includes
  the NUL and any stale bytes `memmove` left past it. The generator asserts that
  the C wrote nothing beyond the window; the replay requires the same of the Rust
  and compares the rest of the buffer to poison.
- `set`: `hc` is `sv_highchars.integer`, or `none` for "not registered yet" (the
  Rust's `Cvar_FindVar` misses; the C global's `.integer` is 0, so both filter).
- `sweep`: FNV-1a 64 over the results of every string over `{ \ a b * }` of
  length 0..=7 (21845 strings) through ten operations (three lookups, two
  removals, a prefixed removal, four sets, a print), whole 64-byte buffers and
  logs included. Serialisation is in `gen_fixtures.c` `sweep` and
  `replay.rs` `sweep_serialise`; it must change in both together.

Contents (918 records): directed lookups (basic, malformed, 100-character
key and value, the four-slot rotation with a miss in the middle, a miss that
overwrites a held slot) and 24 seeded random runs; removal (first, middle,
last, only, absent, duplicate, empty key, no leading backslash, trailing
backslash, key without value, backslash in the key) and 110 random; prefixed
removal (`_`, `*`, NUL, a high byte; adjacent and repeated keys) and 70 random;
sets (append, replace, delete by `""`, `*` keys through both setters, the
backslash, quote and 63/64 rejections, the `>= maxsize` boundary on both sides
including the check running after the removal, `sv_highchars` none / 0 / 1 / 2
and negative against 0x01, 0x0d, 0x0e, 0x1f, 0x7f, 0x80, 0xff) and 220 random;
prints (20-column padding at 19, 20, 21, missing value, trailing and doubled
backslashes) and 40 random.

## Beyond the fixture

`../replay.rs` is not only the fixture. It carries an independent reference
model of the info-string semantics, written from the data shape (fields split
on backslashes, read as key/value pairs) and not from the C control flow:

- the model must reproduce every fixture record, so it cannot drift from the C;
- `sweep_matches_reference_model`: every string over `{ \ a b * }` up to length 8
  (87381 strings) through the ten sweep operations, port against model, byte for
  byte;
- `random_sequences_match_reference_model`: 3000 seeded sequences of 60
  operations on one evolving buffer (sets through both setters at every
  `sv_highchars` state and near the `maxsize` edge, removals, prefixed removals,
  lookups, prints, buffer resets), the whole 1024-byte buffer and the log
  compared after every operation, the rotation slots tracked by pointer;
- `boundary_grid_matches_reference_model`: nine init strings x key and value
  lengths 0..65 x four key kinds x three filter states x `maxsize` from need-2 to
  need+2, at eight buffer offsets.

Every call runs in guard zones (the buffer, the key and the value are each
surrounded by 0xa5 bytes, inputs are checked unmodified) at unaligned offsets.
The tests share one lock: `Info_ValueForKey` owns process-global buffers and the
`CON_Printf` / `Cvar_FindVar` stubs are global too.

## Accepted losses and C undefined behaviour

- A key or value segment of 512 bytes or more overruns the C's `pkey[512]` /
  `value[512]` stack buffers and the 512-byte rotation slots; a `maxsize` larger
  than the caller's buffer lets the C write past it. Neither is generated.
  The Rust copies into the same fixed-size arrays and is equally undefined there.
- The address identity of the miss literal is not frozen (see `look`).
- The exact slot a first lookup lands on depends on how many lookups ran before
  in the process, so classes are relative to the run.

## Regenerate and audit

Needs cc:

    bash engine/rust/info_str/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/info_str/tests/gen_fixtures.sh --check   # diff against C

`check-rust-info-str.sh` runs `--check`, so drift in the C or a hand edit of the
fixture fails the gate.

## Mutant check

Needs perl, cargo: `bash engine/rust/info_str/tests/mutants.sh`.
41 mutants: the 63/64 limit both ways, key and value length checks dropped,
the `>= maxsize` check as `>` and with each separator forgotten, rotation of
three slots / never advancing / advancing only on a hit / every lookup in slot
0, the leading separator not skipped, a key mismatch returning, the removal
`memmove` dropping the NUL / keeping the separator / starting one byte early,
a backslash accepted in a removal key, prefixed removal inverted / stopping
after one key / not restarting, `*` keys allowed through `Info_SetValueForKey`
and `_` rejected instead, `maxsize` capped by one, an empty value not deleting,
the old key not removed first, append turned into overwrite, the value copy cut
at 62, the quote and backslash checks removed, `sv_highchars` inverted, the
`c > 13` retention at 13 and 14, the `c < 32` bound at 31, the high bit not
stripped, an unregistered cvar reading as on, a lookup of another cvar name,
`Info_Print` padding to 19, losing the MISSING VALUE text, not skipping the
leading or the value separator, and the CON_Printf flags word changed. All 41
die under this replay, and all 41 died under the retired live harness (run
first, kill command rebuilding the engine archive then running
`run_diff_harness.sh`, baseline checked green). Two equivalent mutants are left
out and noted in `mutants.sh`. A mutant that hangs the replay is killed by the
60 s timeout.
