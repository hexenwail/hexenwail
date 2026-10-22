# hashindex fixtures

`hashindex.txt` is the frozen behaviour of the C original,
`engine/h2shared/hashindex.c`, replacing the live differential harness (#475).
`../replay.rs` replays it with `cargo test --release` (host crate `../../replay/`,
which compiles `src/ffi.rs` unchanged).

The table is stateful, so the fixture is a SCRIPT, not a list of single calls. One
record per line; `#` lines are comments (the generator writes the entry names that
produced a string key there). Ints are decimal.

    layout <sizeof> <off hashSize> <off hash> <off indexChain> <off hashMask>
    zero <name>               zeroed hashindex_t, nothing allocated
    seq <name> <size>         zeroed hashindex_t, then Hash_Allocate(size)
    alloc <size>              Hash_Allocate on the current struct
    add <key> <idx>           Hash_Add
    rem <key> <idx>           Hash_Remove
    clr                       Hash_Clear
    free                      Hash_Free
    look <key> <n> <idx>...   walk Hash_First/Hash_Next from key: n entries, in chain order
    state <size> <mask> <hash==NULL> <chain==NULL> <hash[]> <indexChain[]>
    fatal <setup> <op> <a> <b> <message>

Operations are inputs; `look` and `state` are the expected observations, compared
as whole lines, so one wrong array element, mask, pointer or chain position fails
with that line. `state` writes both arrays in full up to 32 entries, and above that
`fnv:<16 hex>`, an FNV-1a 64 of the little-endian bytes (the 65536-entry table).
`-` is a NULL array. `look` is bounded at `hashSize + 1` entries so a cycle ends.

`fatal` is a call that ends in `Sys_Error`: `setup` is `zero`, `alloc:<n>` or
`freed:<n>`, `op` is `alloc`, `add` or `rem`. The generator forks each one against
the C and freezes the message it printed (arguments substituted); the replay runs it
in a child copy of the test binary and requires SIGABRT and that exact line.

Contents (about 3400 records, 93 KB): the struct layout; empty tables at sizes 1, 2, 8
and 128 (look, clear and double free on them); size 1 (mask 0); one bucket filled LIFO
then emptied from head, middle and tail; interleaved buckets; remove of an absent
index and remove twice; `Hash_Clear` leaving `indexChain` stale and later
operations reading the stale slots; remove under the wrong key; extreme keys
(0, -1, INT_MAX, INT_MIN, aliases of a bucket); duplicate keys; a double add of
one index (a cycle, so lookups are bounded); the real string keys
`Hash_GenerateKeyString` makes, case-sensitive and not, with collisions between
`progs.dat` and `PROGS.DAT`; growth (free, allocate bigger, re-add) from 8 to 128
and back to 2; a 65536-entry table; twelve seeded sequences of 60 to 400 steps at
sizes 2 to 1024 (adds, removes, absent removes, lookups, clears, free plus
reallocate) over four key distributions; and 23 fatal cases covering every
`Sys_Error` in the file, including the order of the two `Hash_Allocate` checks.
The replay floors every record kind, so a truncated fixture fails.

## Beyond the fixture

`../replay.rs` also drives about 900,000 seeded operations at table sizes 1 to 4096
against a reference model it carries: a table is a list of buckets, each a `Vec` of
indices head first. It shares nothing with `ffi.rs` (no arrays, no chain pointers).
`hash[]` and `indexChain[]` are derived from the buckets, including the stale
`indexChain` slots `Hash_Clear` leaves behind. Every lookup, every `Hash_Next` on a
chain member, and (every operation for tables up to 64, every 61st above) both
whole arrays are compared, across clears and free plus reallocate at a new size.
The model is not frozen from C: it states the semantics from the C source, and the
fixture is what proves those semantics against the C's actual output.

## Not covered, on purpose

- `Hash_GenerateKeyString`, `Hash_GenerateKeyInt`, `Hash_First` and `Hash_Next` are
  `static inline` in `hashindex.h`: no symbol, not ported, still compiled from C.
  The replay re-implements the two walkers only to read the arrays, and the
  generator calls the real header for the string keys.
- Accepted loss, undefined in C: `Hash_Remove` takes no range check on `index`. An index outside `[0, hashSize)` reads or writes out of bounds in C
  and in Rust, so neither side is replayed there. `Hash_Add` is checked and is
  covered (`fatal ... add`).
- Accepted loss: a `Hash_Remove` loop on a table that already holds a cycle (from a
  double add) may never end, in C as in Rust. The fixture's cyclic cases do not
  remove through the cycle except the one case that terminates (`double-add-in-chain`).
- A failed `malloc` (the `out of memory` message) cannot be provoked portably.
- The stub `Sys_Error` in `../../replay/src/lib.rs` takes the format and one int.
  Stable Rust cannot define a variadic function; this is ABI-compatible on x86-64
  and aarch64 Linux and covers every message in the file (at most one `%d`).

Regenerate or audit against C (needs cc and the engine headers, so run it inside
`nix develop`):

    bash engine/rust/hashindex/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/hashindex/tests/gen_fixtures.sh --check   # diff against C

## Mutants

`bash engine/rust/hashindex/tests/mutants.sh` (needs perl, cargo) applies 26 mutants
to `src/ffi.rs`, one at a time, and requires the release-profile replay to die for
each. They cover the power-of-two test (size 0 and 2^n - 1), the re-initialise guard
and the order of the two `Hash_Allocate` checks, the `indexChain` fill, the mask,
`Hash_Free` leaving each pointer (and zeroing the mask), `Hash_Add` (no link to the
old head, head never updated, `>` for `>=`, accepting -1, wrong bucket, NULL guard),
`Hash_Remove` (half mask, head test, head promotion, relink, walk, stale slot, NULL guard),
`Hash_Clear` (half clear, also clearing `indexChain`, NULL guard) and the struct
layout (pointers swapped, compile-time asserts stripped so the layout record has to
catch it), and `Hash_Remove`'s head test dropped. All 26 die.

The first 25 against the retired live harness before it was deleted: it killed 19
and 6 survived (`Allocate` re-init guard and check order, `Add` accepting -1, the
`Remove` and `Clear` NULL guards, `Clear` half the table), so every mutant the
harness killed is killed by the replay and the replay kills 6 more. The harness
checked four fatal cases (never `Remove` or `Clear` on an unallocated or freed
table, never index -1), and its one clear case held entries only in the low half of
the table, so a clear of half the array looked fine.

Equivalent mutants left out: `Hash_Remove` computing the bucket before or after the
NULL check (unobservable, both reach the same `Sys_Error`). Dropping the
`hash[k] == index` head test in `Hash_Remove` is NOT equivalent (the chain walk
starts below the head and never unlinks it); it is mutant 'Remove head test dropped'.
