// SPDX-License-Identifier: ISC
//
// Rust replacement for common/strlcat.c.
//
// q_strlcat is a bounded byte-string append with no allocation or global state.
// The destination must name a string of at most `siz` bytes and the source must
// be NUL-terminated; as in the C original, overlapping source and destination
// are outside the function's contract.

use core::ffi::c_char;

/// `strlen` over a C string.  Written out rather than bound to libc so the
/// consolidated crate keeps its no-dependency, `no_std` build.
///
/// # Safety
/// `s` must point at a NUL-terminated byte sequence.
unsafe fn c_strlen(mut s: *const c_char) -> usize {
    let start = s;
    while *s != 0 {
        s = s.add(1);
    }
    s.offset_from(start) as usize
}

/// `q_strlcat` from common/strlcat.c.
///
/// Appends `src` to the string in `dst` of size `siz`, at most `siz - 1` bytes
/// total.  Always NUL-terminates unless `siz` is no larger than the initial
/// length of `dst`.  Returns `strlen(src) + MIN(siz, strlen(initial dst))`; a
/// result `>= siz` means the append truncated.
///
/// This deliberately follows the C statement order rather than using a slice:
/// the destination scan reads at most `siz` bytes and never writes, `siz == 0`
/// never dereferences `dst` -- only `src` is read, for the return value -- and a
/// `dst` with no NUL inside `siz` is left untouched with `strlen(src) + siz`
/// returned.
#[no_mangle]
pub unsafe extern "C" fn q_strlcat(
    dst: *mut c_char,
    src: *const c_char,
    siz: usize,
) -> usize {
    let mut d = dst;
    let mut s = src;
    let mut n = siz;
    // Counted rather than computed as `d - dst`: `dst` may be NULL when
    // siz == 0, and the scan may not have advanced at all.
    let mut dlen = 0usize;

    // while (n-- != 0 && *d != '\0') d++;
    // The C post-decrement runs even when n is already 0, and a zero n must
    // short-circuit before *d is read -- which is what makes a NULL dst with
    // siz == 0 legal.  The wrap is discarded by the reassignment below.
    loop {
        let more = n != 0;
        n = n.wrapping_sub(1);
        if !more || *d == 0 {
            break;
        }
        d = d.add(1);
        dlen += 1;
    }

    n = siz - dlen;

    // No room even for a terminator: the C original returns without writing.
    if n == 0 {
        return dlen + c_strlen(s);
    }

    // while (*s != '\0') { if (n != 1) { *d++ = *s; n--; } s++; }
    while *s != 0 {
        if n != 1 {
            *d = *s;
            d = d.add(1);
            n -= 1;
        }
        s = s.add(1);
    }
    *d = 0;

    dlen + s.offset_from(src) as usize
}