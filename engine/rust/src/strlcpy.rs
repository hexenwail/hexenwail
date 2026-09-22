// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for common/strlcpy.c.
//
// Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
// Copyright (C) 2026 Hexenwail contributors.
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// common/strlcpy.c comes from OpenBSD under the ISC licence above.  That
// licence permits this port to be distributed under the GPL, as the rest of
// the engine is; the notice is kept because the ISC licence requires it in
// every copy.
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
