// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/wad.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
//
// wad.c loads gfx.wad -- the tinyfont, conchars, backtile and menu pictures the
// client and the menu draw -- and then edits the *mapped bytes* rather than
// copying them.  Three properties of the C original shape this port.
//
// 1. It is compiled into exactly one target.  wad.c is in COMMON_SOURCES
//    (engine/CMakeLists.txt:716) and is removed again for h2ded (:1308, "gfx.wad
//    lumps are renderer-only data"); HWSV_SOURCES never lists it.  Every caller
//    of W_LoadWadFile, W_GetLumpName and SwapPic -- gl_draw.c, draw.c, menu.c,
//    host.c, gl_vidsdl.c -- is client-only.  So the differential harness
//    compares against the one C that ships, and the gate expects exactly one
//    definition in glhexen2 and at most one in h2ded and hwsv.
//
// 2. The mutation is in place, inside the zone allocation.  W_CleanupName is
//    documented "Can safely be performed in place" and the call site marks it
//    "CAUTION: in-place editing!!!": it writes lowercased, zero-padded names
//    straight into the mapped lump table, and SwapPic byte-swaps each TYP_QPIC
//    lump's width and height in the mapped data.  Both are observable to every
//    later reader of `wad_base`, so this module keeps a raw pointer and writes
//    through it; it does not copy the wad into Rust-owned memory.
//
// 3. The three exported globals keep the C's exact types and linkage:
//    `int wad_numlumps`, `lumpinfo_t *wad_lumps`, `byte *wad_base`.  Nothing
//    outside wad.c reads them in this tree, but they are part of the header's
//    ABI (wad.h) and of the mapped-image story: wad_lumps points into wad_base.
//    The failure paths do NOT reset them -- the C calls Sys_Error, which never
//    returns, so a failed load leaves wad_numlumps and wad_lumps describing the
//    previous wad while wad_base already points at the failed mapping.  That
//    half-updated state is reachable only through a fatal diagnostic, and it is
//    reproduced rather than tidied up.
//
// LittleLong is a compile-time macro in the C (common/q_endian.h: on a
// little-endian host it is the identity, on a big-endian host it is LongSwap).
// The port keeps that shape: `little_long` swaps under cfg(target_endian) and
// is deliberately not a no-op on a big-endian host.  Endianness is therefore a
// property of the bytes that were read, not of the field that held them, which
// is why every read goes through it explicitly.

use core::ffi::{c_char, c_int, c_uint, c_void};

/// `TYP_QPIC` from engine/h2shared/wad.h.  The one lump type W_LoadWadFile
/// byte-swaps in place; every other type is left as it was mapped.
const TYP_QPIC: c_char = 66;

/// `Z_SECZONE` from engine/h2shared/zone.h -- the zone W_LoadWadFile maps the
/// wad into, and the zone the caller's Z_Free therefore has to release.
const Z_SECZONE: c_int = 1 << 1;

/// `lumpinfo_t->name`: W_CleanupName walks all 16 bytes, not up to the NUL.
const NAME_LEN: usize = 16;

//============================================================================
// the on-disk and in-header layouts
//============================================================================

/// `wadinfo_t` from engine/h2shared/wad.h.
#[repr(C)]
pub struct WadInfoC {
    pub identification: [c_char; 4],
    pub numlumps: c_int,
    pub infotableofs: c_int,
}

/// `lumpinfo_t` from engine/h2shared/wad.h.  This is also the on-disk entry
/// layout, which is why the differential harness checks every field offset
/// against the C struct rather than only the total size.
#[repr(C)]
pub struct LumpInfoC {
    pub filepos: c_int,
    pub disksize: c_int,
    pub size: c_int,
    pub type_: c_char,
    pub compression: c_char,
    pub pad1: c_char,
    pub pad2: c_char,
    pub name: [c_char; NAME_LEN],
}

/// `qpic_t` from engine/h2shared/wad.h -- the header of a TYP_QPIC lump.  Only
/// width and height are byte-swapped; `data` is variably sized and is left
/// alone.
#[repr(C)]
pub struct QPicC {
    pub width: c_int,
    pub height: c_int,
    pub data: [u8; 4],
}

const _: () = {
    assert!(core::mem::offset_of!(WadInfoC, identification) == 0);
    assert!(core::mem::offset_of!(WadInfoC, numlumps) == 4);
    assert!(core::mem::offset_of!(WadInfoC, infotableofs) == 8);
    assert!(core::mem::size_of::<WadInfoC>() == 12);

    assert!(core::mem::offset_of!(LumpInfoC, filepos) == 0);
    assert!(core::mem::offset_of!(LumpInfoC, disksize) == 4);
    assert!(core::mem::offset_of!(LumpInfoC, size) == 8);
    assert!(core::mem::offset_of!(LumpInfoC, type_) == 12);
    assert!(core::mem::offset_of!(LumpInfoC, compression) == 13);
    assert!(core::mem::offset_of!(LumpInfoC, pad1) == 14);
    assert!(core::mem::offset_of!(LumpInfoC, pad2) == 15);
    assert!(core::mem::offset_of!(LumpInfoC, name) == 16);
    assert!(core::mem::size_of::<LumpInfoC>() == 32);
    assert!(core::mem::align_of::<LumpInfoC>() == 4);

    assert!(core::mem::offset_of!(QPicC, width) == 0);
    assert!(core::mem::offset_of!(QPicC, height) == 4);
    assert!(core::mem::offset_of!(QPicC, data) == 8);
    assert!(core::mem::size_of::<QPicC>() == 12);
};

// The layout accessors let the C differential harness check these structs
// against the real wad.h ones without restating Rust's offsets in C, as the
// sizebuf, msg_io, info_str and huffman ports do.
#[no_mangle]
pub extern "C" fn WadInfoC_sizeof() -> usize {
    core::mem::size_of::<WadInfoC>()
}

#[no_mangle]
pub extern "C" fn WadInfoC_offsetof_identification() -> usize {
    core::mem::offset_of!(WadInfoC, identification)
}

#[no_mangle]
pub extern "C" fn WadInfoC_offsetof_numlumps() -> usize {
    core::mem::offset_of!(WadInfoC, numlumps)
}

#[no_mangle]
pub extern "C" fn WadInfoC_offsetof_infotableofs() -> usize {
    core::mem::offset_of!(WadInfoC, infotableofs)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_sizeof() -> usize {
    core::mem::size_of::<LumpInfoC>()
}

#[no_mangle]
pub extern "C" fn LumpInfoC_alignof() -> usize {
    core::mem::align_of::<LumpInfoC>()
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_filepos() -> usize {
    core::mem::offset_of!(LumpInfoC, filepos)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_disksize() -> usize {
    core::mem::offset_of!(LumpInfoC, disksize)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_size() -> usize {
    core::mem::offset_of!(LumpInfoC, size)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_type() -> usize {
    core::mem::offset_of!(LumpInfoC, type_)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_compression() -> usize {
    core::mem::offset_of!(LumpInfoC, compression)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_pad1() -> usize {
    core::mem::offset_of!(LumpInfoC, pad1)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_pad2() -> usize {
    core::mem::offset_of!(LumpInfoC, pad2)
}

#[no_mangle]
pub extern "C" fn LumpInfoC_offsetof_name() -> usize {
    core::mem::offset_of!(LumpInfoC, name)
}

#[no_mangle]
pub extern "C" fn QPicC_sizeof() -> usize {
    core::mem::size_of::<QPicC>()
}

#[no_mangle]
pub extern "C" fn QPicC_alignof() -> usize {
    core::mem::align_of::<QPicC>()
}

#[no_mangle]
pub extern "C" fn QPicC_offsetof_width() -> usize {
    core::mem::offset_of!(QPicC, width)
}

#[no_mangle]
pub extern "C" fn QPicC_offsetof_height() -> usize {
    core::mem::offset_of!(QPicC, height)
}

#[no_mangle]
pub extern "C" fn QPicC_offsetof_data() -> usize {
    core::mem::offset_of!(QPicC, data)
}

/// `LittleLong` as the C macro expands on this host, so the differential
/// harness can compare the two directly instead of only through a decoded
/// field.  On a little-endian host that is the identity; on a big-endian host
/// it is LongSwap, which `swap_bytes` reproduces bit for bit (LongSwap builds
/// the value out of the four bytes with shifts, so the sign bit travels with
/// them).
#[no_mangle]
pub extern "C" fn Wad_LittleLong(v: c_int) -> c_int {
    little_long(v)
}

#[inline]
fn little_long(v: c_int) -> c_int {
    #[cfg(target_endian = "little")]
    {
        v
    }
    #[cfg(target_endian = "big")]
    {
        v.swap_bytes()
    }
}

//============================================================================
// the exported state
//============================================================================

/// `int wad_numlumps` -- the number of lumps in the wad last successfully
/// loaded, still holding the previous wad's count if the load failed.
#[no_mangle]
pub static mut wad_numlumps: c_int = 0;

/// `lumpinfo_t *wad_lumps` -- the lump table, inside `wad_base`.
#[no_mangle]
pub static mut wad_lumps: *mut LumpInfoC = core::ptr::null_mut();

/// `byte *wad_base` -- the mapped wad, or NULL before the first load.  The C
/// initialiser is `= NULL`; this is the same value in .bss.
#[no_mangle]
pub static mut wad_base: *mut u8 = core::ptr::null_mut();

extern "C" {
    /// engine/h2shared/quakefs.h.  Returns the wad mapped into the zone named
    /// by zone_id, or NULL.
    fn FS_LoadZoneFile(path: *const c_char, zone_id: c_int, path_id: *mut c_uint)
        -> *mut u8;

    /// engine/h2shared/zone.h.
    fn Z_Free(ptr: *mut c_void);

    /// engine/h2shared/common.h, FUNC_NORETURN: the fatal paths below never
    /// return, which is why the state they leave behind is the state the
    /// engine dies with rather than a state any caller can observe and repair.
    fn Sys_Error(fmt: *const c_char, ...) -> !;

    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
}

//============================================================================
// name cleanup
//============================================================================

/// `W_CleanupName` -- lowercases and zero-pads to `NAME_LEN` bytes.
///
/// Two details are the contract, and both are pinned by the differential
/// harness.  The first is that it rewrites the caller's buffer, which here is
/// the mapped wad itself, so `input` and `out` are the same pointer.  The
/// second is the loop shape: the C breaks out on the first NUL and *then*
/// zero-fills the rest, so a source name of exactly 16 non-zero bytes produces
/// a 16-byte result with no terminator at all -- and a source name with a NUL
/// anywhere in it gets zeros from that byte onwards, not preserved padding.
///
/// The C name is kept: it is the same function, and a reviewer searching for
/// the in-place edit contract has one spelling to look for.
#[allow(non_snake_case)]
unsafe fn W_CleanupName(input: *const c_char, out: *mut c_char) {
    let mut i = 0;

    while i < NAME_LEN {
        let c = *input.add(i) as u8;
        if c == 0 {
            break;
        }

        // The C takes `in[i]` as a signed char into an `int` and compares
        // against 'A'..'Z'; bytes >= 0x80 are negative there and negative here
        // after the u8 read, so the range test is false either way.  Copying
        // the byte unchanged is the same behaviour as the C's `out[i] = c`.
        let c = if (b'A'..=b'Z').contains(&c) {
            c + (b'a' - b'A')
        } else {
            c
        };

        *out.add(i) = c as c_char;
        i += 1;
    }

    while i < NAME_LEN {
        *out.add(i) = 0;
        i += 1;
    }
}

//============================================================================
// loading
//============================================================================

/// `W_LoadWadFile`
#[no_mangle]
pub unsafe extern "C" fn W_LoadWadFile(filename: *const c_char) {
    if !wad_base.is_null() {
        Z_Free(wad_base.cast::<c_void>());
    }

    // path_id is NULL: the C passes it and never looks at it.
    wad_base = FS_LoadZoneFile(filename, Z_SECZONE, core::ptr::null_mut());
    if wad_base.is_null() {
        // The zone is already released by the failed load, but wad_base now
        // holds the NULL the call returned -- the C writes it before testing.
        Sys_Error(
            c"%s: couldn't load %s".as_ptr(),
            c"W_LoadWadFile".as_ptr(),
            filename,
        );
    }

    let header = wad_base.cast::<WadInfoC>();

    if (*header).identification[0] != b'W' as c_char
        || (*header).identification[1] != b'A' as c_char
        || (*header).identification[2] != b'D' as c_char
        || (*header).identification[3] != b'2' as c_char
    {
        // Identification is read as four plain chars, not through LittleLong:
        // the check is byte-order independent on purpose, and "2DAW" -- the
        // same four bytes reversed -- is rejected here too.
        Sys_Error(
            c"Wad file %s doesn't have WAD2 id\n".as_ptr(),
            filename,
        );
    }

    wad_numlumps = little_long((*header).numlumps);
    let infotableofs = little_long((*header).infotableofs);

    // The C computes `(lumpinfo_t *)(wad_base + infotableofs)`; wrapping keeps
    // that a byte offset into the mapping instead of an inbounds assumption, so
    // a malformed table offset cannot turn into a Rust-level precondition
    // failure that the C original does not have.
    wad_lumps = wad_base
        .wrapping_offset(infotableofs as isize)
        .cast::<LumpInfoC>();

    let mut i = 0;
    while i < wad_numlumps {
        let lump_p = wad_lumps.wrapping_offset(i as isize);

        // filepos and size are the two fields read back out of the table as
        // integers; disksize, type, compression and the pads are left as they
        // are -- type in particular is compared against TYP_QPIC below without
        // any conversion.
        (*lump_p).filepos = little_long((*lump_p).filepos);
        (*lump_p).size = little_long((*lump_p).size);

        W_CleanupName((*lump_p).name.as_ptr(), (*lump_p).name.as_mut_ptr());

        if (*lump_p).type_ == TYP_QPIC {
            SwapPic(
                wad_base
                    .wrapping_offset((*lump_p).filepos as isize)
                    .cast::<QPicC>(),
            );
        }

        i += 1;
    }
}

//============================================================================
// lookup
//============================================================================

/// `W_GetLumpinfo`
#[no_mangle]
pub unsafe extern "C" fn W_GetLumpinfo(name: *const c_char) -> *mut LumpInfoC {
    let mut clean = [0 as c_char; NAME_LEN];

    W_CleanupName(name, clean.as_mut_ptr());

    let mut i = 0;
    while i < wad_numlumps {
        let lump_p = wad_lumps.wrapping_offset(i as isize);

        if strcmp(clean.as_ptr(), (*lump_p).name.as_ptr()) == 0 {
            return lump_p;
        }

        i += 1;
    }

    // The name reported is the caller's, not the cleaned one.
    Sys_Error(
        c"%s: %s not found".as_ptr(),
        c"W_GetLumpinfo".as_ptr(),
        name,
    );
}

/// `W_GetLumpName` -- a pointer into the mapping, not a copy.
#[no_mangle]
pub unsafe extern "C" fn W_GetLumpName(name: *const c_char) -> *mut c_void {
    let lump = W_GetLumpinfo(name);

    wad_base
        .wrapping_offset((*lump).filepos as isize)
        .cast::<c_void>()
}

// `W_GetLumpNum` is `#if 0` in the C original ("no callers") and stays out of
// the port for the same reason.

//============================================================================
// automatic byte swapping
//============================================================================

/// `SwapPic` -- called from W_LoadWadFile and directly from gl_draw.c, draw.c
/// and menu.c, so it is part of the same ABI and lives with the rest of the
/// port.  It swaps the two header words in place and leaves `data` alone.
#[no_mangle]
pub unsafe extern "C" fn SwapPic(pic: *mut QPicC) {
    (*pic).width = little_long((*pic).width);
    (*pic).height = little_long((*pic).height);
}
