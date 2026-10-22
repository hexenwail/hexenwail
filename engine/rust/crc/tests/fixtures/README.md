# crc fixtures

`crc.txt` is the frozen output of the C original, `common/crc.c`, replacing the
live differential harness (#475). `../replay.rs` replays it with `cargo test`.

One record per line; `#` lines are comments; hex is lowercase, `-` is empty data.

    init <crc>                          CRC_Init
    byte <crc-in> <data> <crc-out>      CRC_ProcessByte
    value <in> <out>                    CRC_Value
    block <data> <crc>                  CRC_Block
    pblock <seed> <data> <crc-out>      CRC_ProcessBlock

Contents: all 256 table entries (byte records with crc 0000), directed edge
values, 64 seeded (crc, byte) pairs, CRC_Value samples, the "123456789" check
value 29b1, and blocks of every length 0..40 (plain and seeded) plus longer ones
up to 512 bytes.

Beyond the fixture, `../replay.rs` compares the port with an independent
bit-by-bit CRC-CCITT written in the test, over all 2^24 (crc, byte) steps, all
2^16 `CRC_Value` inputs (the identity, `CRC_XOR_VALUE` being 0) and 5000 seeded
blocks of 0..8192 bytes at offsets 0..15 (the retired harness used 20000), and
pins the literal check value `0x29b1` for "123456789". Those run in about a
quarter of a second with `cargo test --release`.

Regenerate or audit against C (needs cc):

    bash engine/rust/crc/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/crc/tests/gen_fixtures.sh --check   # diff against C

Mutant check (needs perl, cargo): `bash engine/rust/crc/tests/mutants.sh`.
Twelve mutants (polynomial, table loop, one table entry, init, final xor, shift,
index, block loop length, block pointer step, ignored seed, dropped last byte,
CRC_Block seed). All twelve die under this replay and died under the retired
live harness.
