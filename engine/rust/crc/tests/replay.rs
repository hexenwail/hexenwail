// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/crc.txt, the frozen output of common/crc.c, against
// engine/rust/src/crc.rs.  No C compiler: the fixture is plain text.  Format
// and regeneration are in tests/fixtures/README.md.
//
// Every block is also replayed at unaligned offsets, and each block result must
// equal the byte-at-a-time chain, as the retired differential harness checked.

use crc_rs::crc::{CRC_Block, CRC_Init, CRC_ProcessBlock, CRC_ProcessByte, CRC_Value};

const FIXTURE: &str = include_str!("fixtures/crc.txt");

fn hex16(s: &str) -> u16 {
    assert_eq!(s.len(), 4, "bad u16 {s:?}");
    u16::from_str_radix(s, 16).unwrap()
}

fn data(s: &str) -> Vec<u8> {
    if s == "-" {
        return Vec::new();
    }
    assert_eq!(s.len() % 2, 0, "odd hex data");
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

/// Runs `f` on a copy of `d` placed at `off` bytes into a larger buffer, and
/// checks the input bytes came back untouched.
fn at_offset<R>(d: &[u8], off: usize, f: impl Fn(*const u8) -> R) -> R {
    let mut buf = vec![0xa5u8; off + d.len() + 8];
    buf[off..off + d.len()].copy_from_slice(d);
    let before = buf.clone();
    let r = f(unsafe { buf.as_ptr().add(off) });
    assert_eq!(buf, before, "crc modified its input");
    r
}

#[test]
fn replay_c_fixtures() {
    let (mut n_init, mut n_byte, mut n_value, mut n_block, mut n_pblock) = (0, 0, 0, 0, 0);
    let mut table_sweep = 0;

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("crc.txt:{}: {line}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        match f[0] {
            "init" => {
                let mut c = 0x1234u16;
                unsafe { CRC_Init(&mut c) };
                assert_eq!(c, hex16(f[1]), "{at}");
                n_init += 1;
            }
            "byte" => {
                let mut c = hex16(f[1]);
                if c == 0 {
                    table_sweep += 1;
                }
                unsafe { CRC_ProcessByte(&mut c, u8::from_str_radix(f[2], 16).unwrap()) };
                assert_eq!(c, hex16(f[3]), "{at}");
                n_byte += 1;
            }
            "value" => {
                assert_eq!(CRC_Value(hex16(f[1])), hex16(f[2]), "{at}");
                n_value += 1;
            }
            "block" | "pblock" => {
                let seeded = f[0] == "pblock";
                let seed = if seeded { hex16(f[1]) } else { 0xffff };
                let d = data(f[if seeded { 2 } else { 1 }]);
                let want = hex16(f[if seeded { 3 } else { 2 }]);
                for off in [0usize, 1, 3, 7] {
                    let got = at_offset(&d, off, |p| unsafe {
                        if seeded {
                            let mut c = seed;
                            CRC_ProcessBlock(p, &mut c, d.len() as i32);
                            c
                        } else {
                            CRC_Block(p, d.len() as i32)
                        }
                    });
                    assert_eq!(got, want, "{at} (offset {off})");
                }
                // Block path must equal the byte path.
                let mut chained = seed;
                for &b in &d {
                    unsafe { CRC_ProcessByte(&mut chained, b) };
                }
                assert_eq!(chained, want, "{at} (byte chain)");
                if seeded {
                    n_pblock += 1;
                } else {
                    n_block += 1;
                }
            }
            other => panic!("{at}: unknown record {other:?}"),
        }
    }

    // Floors: an emptied or truncated fixture must not read as green.
    assert_eq!(n_init, 1);
    assert_eq!(table_sweep, 256 + 4, "crc=0 byte records must cover all 256 table entries");
    assert!(n_byte >= 300, "byte records: {n_byte}");
    assert!(n_value >= 6, "value records: {n_value}");
    assert!(n_block >= 40, "block records: {n_block}");
    assert!(n_pblock >= 40, "pblock records: {n_pblock}");
}

/// count == 0 never dereferences the pointer, so NULL is legal (the C contract).
#[test]
fn null_pointer_with_zero_count() {
    let mut c = 0xbeefu16;
    unsafe { CRC_ProcessBlock(core::ptr::null(), &mut c, 0) };
    assert_eq!(c, 0xbeef);
    assert_eq!(unsafe { CRC_Block(core::ptr::null(), 0) }, 0xffff);
}

// ---------------------------------------------------------------------------
// Exhaustive cross-check against an INDEPENDENT reference.
//
// The fixture is a sample.  The retired differential harness was exhaustive
// (every (crc, byte) step, every CRC_Value input, 20000 blocks up to 8192
// bytes at offsets 0..15), and the fixture alone does not match that.  This
// restores it without a C compiler or a big fixture: `ref_step` is a plain
// bit-by-bit CRC-CCITT (poly 0x1021, MSB first, no table) written here and
// sharing nothing with src/crc.rs, whose step is table driven.  The fixture
// records above are what tie the port to the actual bytes of common/crc.c;
// this section shows the port is the CCITT function everywhere else.
//
// Sized for `cargo test --release`, which is how the gate and mutants.sh run
// it (the engine ships release); a debug build takes noticeably longer.
// ---------------------------------------------------------------------------

const POLY: u16 = 0x1021;

fn ref_step(mut crc: u16, byte: u8) -> u16 {
    crc ^= (byte as u16) << 8;
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 { (crc << 1) ^ POLY } else { crc << 1 };
    }
    crc
}

fn ref_block(seed: u16, d: &[u8]) -> u16 {
    d.iter().fold(seed, |c, &b| ref_step(c, b))
}

/// xorshift64*, seeded: deterministic and unrelated to the code under test.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
}

#[test]
fn reference_pins_the_published_check_value() {
    // CRC-16/CCITT-FALSE of "123456789", the published check value.  Asserted
    // against the literal so a wrong reference cannot validate a wrong port.
    assert_eq!(ref_block(0xffff, b"123456789"), 0x29b1);
    assert_eq!(unsafe { CRC_Block(b"123456789".as_ptr(), 9) }, 0x29b1);
}

#[test]
fn every_crc_byte_step_matches_the_reference() {
    // All 2^24 (crc, byte) pairs.
    for crc in 0..=0xffffu32 {
        for byte in 0..=0xffu32 {
            let mut c = crc as u16;
            unsafe { CRC_ProcessByte(&mut c, byte as u8) };
            assert_eq!(c, ref_step(crc as u16, byte as u8), "crc {crc:04x} byte {byte:02x}");
        }
    }
}

#[test]
fn crc_value_matches_the_reference_for_every_input() {
    // common/crc.c: CRC_Value(x) is `x ^ CRC_XOR_VALUE`, and CRC_XOR_VALUE is
    // 0x0000 for this CRC-CCITT variant, so it is the identity.  Every one of
    // the 2^16 inputs, against the literal.
    for x in 0..=0xffffu32 {
        assert_eq!(CRC_Value(x as u16), x as u16 ^ 0x0000, "CRC_Value({x:04x})");
    }
}

#[test]
fn seeded_blocks_up_to_8192_bytes_at_every_offset_match_the_reference() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    for i in 0..5000usize {
        // Edge lengths first, then random ones up to 8192.
        let len = match i {
            0 => 0,
            1 => 1,
            2 => 8192,
            3 => 8191,
            _ => (rng.next() % 8193) as usize,
        };
        let mut d = vec![0u8; len];
        for b in d.iter_mut() {
            *b = rng.next() as u8;
        }
        let seed = if i % 2 == 0 { 0xffff } else { rng.next() as u16 };
        let want = ref_block(seed, &d);
        let off = i % 16; // every offset 0..15, cycling
        let got = at_offset(&d, off, |p| unsafe {
            let mut c = seed;
            CRC_ProcessBlock(p, &mut c, len as i32);
            c
        });
        assert_eq!(got, want, "block {i}: len {len} offset {off} seed {seed:04x}");
        if seed == 0xffff {
            let got = at_offset(&d, off, |p| unsafe { CRC_Block(p, len as i32) });
            assert_eq!(got, want, "CRC_Block {i}: len {len} offset {off}");
        }
    }
}
