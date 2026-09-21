// SPDX-License-Identifier: ISC
//
// Rust replacement for common/strlcpy.c.
//
// q_strlcpy is a bounded byte-string copy with no allocation or global state.
// The source must name a NUL-terminated C string; as in the C original,
// overlapping source and destination are outside the function's contract.

use core::ffi::c_char;

/// `q_strlcpy` from common/strlcpy.c.
///
/// This deliberately follows the C loop rather than using a slice: callers
/// supply raw C strings, `size == 0` must not touch `dst`, and the return value
/// must be the complete source length even when the destination truncates.
#[no_mangle]
pub unsafe extern "C" fn q_strlcpy(
    dst: *mut c_char,
    src: *const c_char,
    siz: usize,
) -> usize {
    let mut d = dst;
    let mut s = src;
    let mut n = siz;

    if n != 0 {
        loop {
            n -= 1;
            if n == 0 {
                break;
            }

            let byte = *s;
            *d = byte;
            d = d.add(1);
            s = s.add(1);
            if byte == 0 {
                break;
            }
        }
    }

    if n == 0 {
        if siz != 0 {
            *d = 0;
        }
        loop {
            let byte = *s;
            s = s.add(1);
            if byte == 0 {
                break;
            }
        }
    }

    s.offset_from(src) as usize - 1
}
