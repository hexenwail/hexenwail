// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for common/crc.c.
//
// 16-bit, non-reflected CCITT CRC (polynomial 0x1021, init 0xffff, final xor
// 0x0000), the XMODEM variant.  The progs.dat CRC (pr_edict.c), pak directory
// CRCs that identify retail paks (quakefs.c) and model checksums all come
// through here, so the table and the update step must be bit-for-bit
// identical to the C original.
//
// `unsigned short` is u16 on every target this engine builds for; the value
// is caller-owned and only ever reached through the pointers C passes in.

use core::ffi::c_int;

const CRC_INIT_VALUE: u16 = 0xffff;
const CRC_XOR_VALUE: u16 = 0x0000;

/// Built at compile time from the polynomial rather than copied, so a typo in
/// a hand-transcribed table cannot exist.  The differential harness still
/// compares every entry's effect against crc.c's literal table.
const TABLE: [u16; 256] = build_table();
static CRCTABLE: [u16; 256] = TABLE;

const fn build_table() -> [u16; 256] {
    let mut table = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = (i as u16) << 8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
            bit += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
}

// Spot-check the generated table against crc.c's literal entries.
const _: () = {
    assert!(TABLE[0x00] == 0x0000);
    assert!(TABLE[0x01] == 0x1021);
    assert!(TABLE[0x10] == 0x1231);
    assert!(TABLE[0x80] == 0x9188);
    assert!(TABLE[0xff] == 0x1ef0);
};

/// C: `(crc << 8) ^ crctable[(crc >> 8) ^ data]`, truncated to unsigned short
/// on assignment.  u16 << 8 discards the same high bits.
#[inline(always)]
fn step(crc: u16, data: u8) -> u16 {
    (crc << 8) ^ CRCTABLE[((crc >> 8) as u8 ^ data) as usize]
}

/// `CRC_Init`
#[no_mangle]
pub unsafe extern "C" fn CRC_Init(crcvalue: *mut u16) {
    *crcvalue = CRC_INIT_VALUE;
}

/// `CRC_ProcessByte`
#[no_mangle]
pub unsafe extern "C" fn CRC_ProcessByte(crcvalue: *mut u16, data: u8) {
    *crcvalue = step(*crcvalue, data);
}

/// `CRC_ProcessBlock` -- note the C argument order: data first, crc second.
///
/// No slice is formed: C never touches `start` when count is 0, so NULL is a
/// legal pointer there.  `while (count--)` with a negative count runs until
/// the int wraps; the wrapping decrement reproduces that walk rather than
/// silently treating it as zero.  No engine caller passes a negative count.
#[no_mangle]
pub unsafe extern "C" fn CRC_ProcessBlock(
    mut start: *const u8,
    crcvalue: *mut u16,
    mut count: c_int,
) {
    let mut crc = *crcvalue;
    while count != 0 {
        count = count.wrapping_sub(1);
        crc = step(crc, *start);
        start = start.add(1);
    }
    *crcvalue = crc;
}

/// `CRC_Value`
#[no_mangle]
pub extern "C" fn CRC_Value(crcvalue: u16) -> u16 {
    crcvalue ^ CRC_XOR_VALUE
}

/// `CRC_Block` -- like the C original, returns the running value without
/// applying CRC_Value (the xor is 0 either way).
#[no_mangle]
pub unsafe extern "C" fn CRC_Block(start: *const u8, count: c_int) -> u16 {
    let mut crc: u16 = 0;
    CRC_Init(&mut crc);
    CRC_ProcessBlock(start, &mut crc, count);
    crc
}
