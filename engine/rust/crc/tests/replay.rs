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
