# huffman fixtures

`huffman.txt` is the frozen output of the C original,
`engine/hexenworld/shared/huffman.c`, replacing the live differential harness
(#475). `../replay.rs` replays it with `cargo test`.

One record per line; `#` lines are comments; hex is lowercase, `-` is empty.

    freq <idx> <bits>                     IEEE-754 bits of the C's HuffFreq[idx]
    code <sym> <len> <bits>               the C's HuffLookup[sym] after HuffInit
    enc <payload> <outlen> <packet>       HuffEncode; output buffer prefilled 0xa5
    decl <maxlen> <outlen>                HuffDecode of the preceding enc's packet
    dec <maxlen> <packet> <outlen> <bytes>   HuffDecode; the C survived its guard page
    ovr <maxlen> <packet> <outlen> <bytes>   HuffDecode; the C read past the packet
    under <inlen>                         inlen <= 0: outlen 0, nothing written

`payload` is hex, `rep:XX:N` (N copies of byte XX) or `-`. `packet` is hex, or
`raw` for the 0xff fallback (marker byte followed by the payload verbatim).
`bytes` are the first min(outlen, maxlen) bytes HuffDecode stored; everything
after them must still be the 0xa5 fill.

## What is in it

- all 256 frequency floats, bit for bit (a `build.rs` parse error shows here)
- all 256 codes of the tree BuildTree makes from them
- encodes of every single byte (always the raw fallback), 64 x every byte
  (drives each symbol's own code), 12 uniform and 36 frequency-weighted
  seeded payloads, the empty payload; each decoded back at maxlen len, len-1 and 0
- a valid packet with its padding bits as encoded, all set, all clear
- an 80-byte compressed packet truncated at every length, header kept and header 0
- every one of the 256 header bytes over one compressed packet
- raw 0xff packets decoded with maxlen below, at and above the payload; a lone 0xff
- maxlen 0..42 over one packet (the `out[maxlen - 1]` boundary)
- inlen 0, -1, -2
- 160 seeded random byte strings as packets (header biased small or 0xff)

## The overrun cases

The C's GetBit has no bound, so a crafted header byte makes it read past the
declared packet. The generator runs every decode in a forked child with the
packet's last byte against an unmapped page. If the C survives, the record is
`dec` and the Rust must match it exactly. If it dies, the record is `ovr` and
holds the C's decode of the same packet with a 64-byte zero tail (a defined
read); the Rust deliberately stops at the declared end, so its answer must be a
strict prefix of that. The Rust is also replayed on the guard page: a Rust that
reads past the packet dies there and fails the replay.

The generator builds the C twice, as hwsv does (Hunk_AllocName, stubbed) and as
the integrated client does (malloc), and aborts if the two ever differ in bytes
or in whether they overran. That is the one-allocation-path finding of the old
harness, re-proved on every `--check`.

Not frozen, an ACCEPTED LOSS against the retired live harness: bytes past
`outlen` in the encode buffer (the raw fallback leaves scratch bits of the
failed compressed attempt there; no caller reads them, so a change that alters
only those bytes passes), and
the Sys_Error paths (they need a frequency table or allocator the ABI does not
expose, and the fixed table never reaches them).

## Beyond the fixture

`../replay.rs` also encodes 1500 seeded payloads of 1..255 bytes, drawn
proportionally to the frequency table (every fifth uniform, so the raw fallback
is exercised too), and checks, from `huffman.c` HuffEncode and not from the
fixture: `outlen <= inlen + 1`, `outlen = 1 + (bits + 7) / 8` with `bits` the
sum of the code lengths unless that reaches `inlen + 1` (then `0xff` + the
payload verbatim and `outlen = inlen + 1`), a header of `8 * (outlen - 1) - bits`
in 0..=7 for compressed packets, and `decode(encode(x)) == x` at maxlen == len.

## Regenerate or audit against C (needs cc)

    bash engine/rust/huffman/tests/gen_fixtures.sh           # rewrite
    bash engine/rust/huffman/tests/gen_fixtures.sh --check   # diff against C

## Mutants (needs perl, cargo)

    bash engine/rust/huffman/tests/mutants.sh

Twenty-six mutants: Masks, PutBit, GetBit, FindTab, BuildTree (both tie-breaks,
child order, merged frequency), a perturbed frequency, the Huffman_freq
accessor, HuffEncode (bit position, advance, output offset, padding count,
length rounding, raw-fallback threshold, marker, outlen), HuffDecode (inlen
guard, raw copy size, padding count, the declared-end bound removed and one bit
late, the maxlen check and its return, the payload offset). All die under this
replay, and the same twenty-six died under the retired live harness.

One candidate is not in the list because it is equivalent for this table: moving
BuildTree's merged node into the other work slot. Slots only break frequency
ties, and the shipped table has none among merged weights, so no input can tell
the two apart.
