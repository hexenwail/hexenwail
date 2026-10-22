# strlcat fixtures

`strlcat.txt` is the frozen output of the C original, `common/strlcat.c`,
replacing the live differential harness (#475). `../replay.rs` replays it with
`cargo test --release`.

One record per line; `#` lines are comments; hex is lowercase, `-` is empty.

    cat <siz> <dst-in> <src-bytes> <ret> <dst-out>

`dst-in` is the `siz`-byte destination before the call: non-zero bytes, the first
NUL (if any) ends the string, and bytes after it are non-zero junk that must
survive. With no NUL inside `siz` bytes the destination is "full" and nothing may
be written. `src-bytes` is the source buffer: the string, its NUL, then optional
junk after the NUL (never to be copied). `dst-out` is the `siz`-byte destination
after the call. `ret` is the return value, `strlen(src) + MIN(siz, strlen(dst))`.
`siz == 0` is called with a NULL destination, as the C permits.

Contents (906 records, 75 KB): the full grid of source length 0..7 x size 0..11 x
destination string length 0..siz (the last meaning no NUL within `siz`); junk
after the source NUL; sources of 63, 64 and 255 bytes against destination strings
of 0 and 10 bytes and sizes 0, 1, dlen+1, and `dlen+len` -1/0/+1/+33; and 24
seeded random (siz, dlen, len, junk) mixes up to 129 / 89 / 99.

The replay runs each record at five source/destination offset pairs inside
16-byte guard zones, checks the source is unmodified and nothing is written
outside the destination, and re-derives the contract (`expected()` in
`replay.rs`, written from the C semantics, not from the port) for every record,
so a bad fixture cannot bless a bad port. It floors the record count and each
class (size 0, size 1, no NUL in `siz`, room for the terminator only, truncation,
exact fit, slack, empty dst, empty src, junk after either NUL, long sources).

Beyond the fixture, `full_dstlen_srclen_size_offset_grid_matches_the_contract`
runs a generated grid in guard zones: every source length 0..=256 x every size
0..=288 x source offsets {0, 3} x destination offsets {0, 5}. Destination string
lengths are every value 0..=40 for sizes up to 48, and above that the shapes the
old harness built (empty, 10 bytes, NUL at `siz - 2`, `siz - 1`, `siz`,
`siz + 1`). 4.4 million calls, about 1.4 s in release. The destination region
holds the string, its NUL and junk, so strings longer than `siz`
(`siz < strlen(dst)`, where `siz` bounds the return and nothing may be written)
are covered. It compares the whole destination image, return value, and source
against the contract.

Regenerate or audit against C (needs cc):

    bash engine/rust/strlcat/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/strlcat/tests/gen_fixtures.sh --check   # diff against C

Mutant check (needs perl, cargo): `bash engine/rust/strlcat/tests/mutants.sh`.
Eighteen mutants: scan one byte short; scan unbounded for `siz == 0` (NULL
read); scan stopping on the wrong byte; scan pointer not advancing; `dlen` not
counted; room one byte too large; no-room path skipped for `siz > 4`; no-room
return dropping or shortening the source length; truncating one byte early;
wrong copied byte; copy pointer not advancing; terminator not NUL, skipped for a
non-empty dst, or written one byte too far; return dropping the initial dst
length, counting the NUL, or clamped to `siz - 1`. All eighteen die under this
replay and died under the retired live harness (run first, with a rebuild of the
mutated engine archive as the kill command).

Equivalent mutant, left out: `n <= 1` for `n == 0` on the no-room return. With
`n == 1` the write pointer already sits on the NUL that ends `dst`, so the final
`*d = 0` rewrites a zero and the return value is unchanged.

Narrowed against the retired harness, accepted: it ran every source length
0..256 against every size 0..288 over 4 destination kinds and 8 offset pairs. The
replay grid has the same source and size range but 4 offset pairs (source {0, 3},
destination {0, 5}), and every destination string length 0..=40 only for sizes up
to 48 (above that, the 6 shapes listed above rather than every length). The
fixture adds mixes up to 129 / 89 / 99 and the long-source fixed cases. Offsets
other than those listed are not replayed.

Not covered, by design: overlapping source and destination, and a source with no
NUL. Both are outside the C contract (undefined behavior) and not matched.
