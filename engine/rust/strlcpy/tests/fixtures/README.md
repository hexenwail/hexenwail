# strlcpy fixtures

`strlcpy.txt` is the frozen output of the C original, `common/strlcpy.c`,
replacing the live differential harness (#475). `../replay.rs` replays it with
`cargo test`.

One record per line; `#` lines are comments; hex is lowercase, `-` is empty.

    copy <siz> <src-bytes> <ret> <dst-bytes>

`src-bytes` is the source buffer: the string, its NUL, then optional junk after
the NUL (which must never be copied). `dst-bytes` is the `siz`-byte destination
after the call, which started as all `a5`. `ret` is the return value.

Contents (680 records): every source length 0..20 against every size 0..24
(size 0, size 1, truncation, exact fit `siz == len + 1`, slack); junk after the
NUL; sources of 63, 64, 127, 255 and 256 bytes against sizes 0, 1, 2, len-1,
len, len+1, len+2, len+32 and len/2; and 32 seeded random (len, siz, junk) mixes
up to 256 / 288. The replay runs each record at five source/destination offset
pairs inside 16-byte guard zones, checks the source is unmodified and nothing is
written outside the destination, passes NULL when `siz == 0` (as C permits), and
re-derives the semantic (prefix, NUL, a5 tail, return == strlen) from each
record, so a bad fixture cannot bless a bad port.

Regenerate or audit against C (needs cc):

    bash engine/rust/strlcpy/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/strlcpy/tests/gen_fixtures.sh --check   # diff against C

Mutant check (needs perl, cargo): `bash engine/rust/strlcpy/tests/mutants.sh`.
Twelve mutants: copy loop one byte short, size treated as siz+1, copy ignoring
the source NUL, terminator not NUL, size 0 writing the terminator, size 1 not
terminating, terminator clobbering the last copied byte, terminator one byte too
far, destination pointer never advancing, return value truncated, return value
off by one, return value excluding the NUL scan. All twelve die under this
replay and died under the retired live harness (run first, with a build of the
mutated engine archive as the kill command).
