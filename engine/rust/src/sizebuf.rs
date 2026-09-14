// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/sizebuf.c.
//
// The sizebuf is deliberately still caller-owned.  C allocates the structure
// (often as a global) and Rust only mutates the fields and backing storage
// supplied through the existing ABI.

use core::ffi::{c_char, c_int, c_uint, c_void};

const PRINT_TERMONLY: c_uint = 1;

extern "C" {
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn Hunk_AllocName(size: c_int, name: *const c_char) -> *mut c_void;
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;
    fn strlen(s: *const c_char) -> usize;
}

/// `sizebuf_t` from engine/h2shared/sizebuf.h.
///
/// qboolean is a four-byte C int in this engine; Rust's `bool` is not ABI
/// compatible and must not be used for either flag.
#[repr(C)]
pub struct SizeBufC {
    pub allowoverflow: c_int,
    pub overflowed: c_int,
    pub data: *mut u8,
    pub maxsize: c_int,
    pub cursize: c_int,
    pub name: *const c_char,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut u8>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut u8>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

const _: () = {
    assert!(core::mem::offset_of!(SizeBufC, allowoverflow) == 0);
    assert!(core::mem::offset_of!(SizeBufC, overflowed) == 4);
    assert!(core::mem::offset_of!(SizeBufC, data) == PTR_ALIGN);
    assert!(core::mem::offset_of!(SizeBufC, maxsize) == PTR_ALIGN + PTR_SIZE);
    assert!(core::mem::offset_of!(SizeBufC, cursize) == PTR_ALIGN + PTR_SIZE + 4);
    assert!(core::mem::offset_of!(SizeBufC, name) == PTR_ALIGN + PTR_SIZE + 8);
    assert!(core::mem::size_of::<SizeBufC>()
        == align_up(PTR_ALIGN + 2 * PTR_SIZE + 8, PTR_ALIGN));
};

// These accessors let the C differential harness check the complete ABI
// without duplicating Rust layout assumptions in C.
#[no_mangle]
pub extern "C" fn SizeBufC_sizeof() -> usize {
    core::mem::size_of::<SizeBufC>()
}

#[no_mangle]
pub extern "C" fn SizeBufC_alignof() -> usize {
    core::mem::align_of::<SizeBufC>()
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_allowoverflow() -> usize {
    core::mem::offset_of!(SizeBufC, allowoverflow)
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_overflowed() -> usize {
    core::mem::offset_of!(SizeBufC, overflowed)
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_data() -> usize {
    core::mem::offset_of!(SizeBufC, data)
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_maxsize() -> usize {
    core::mem::offset_of!(SizeBufC, maxsize)
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_cursize() -> usize {
    core::mem::offset_of!(SizeBufC, cursize)
}

#[no_mangle]
pub extern "C" fn SizeBufC_offsetof_name() -> usize {
    core::mem::offset_of!(SizeBufC, name)
}

#[inline]
unsafe fn clear(buf: &mut SizeBufC) {
    buf.cursize = 0;
    buf.overflowed = 0;
}

/// `SZ_Init` -- preserve the caller-owned structure and allocation boundary.
#[no_mangle]
pub unsafe extern "C" fn SZ_Init(
    buf: *mut SizeBufC,
    data: *mut u8,
    mut length: c_int,
) {
    // C uses memset(buf, 0, sizeof(*buf)) before inspecting data.
    memset(
        buf.cast::<c_void>(),
        0,
        core::mem::size_of::<SizeBufC>(),
    );
    let buf = &mut *buf;

    if !data.is_null() {
        buf.data = data;
        buf.maxsize = length;
    } else {
        if length < 256 {
            length = 256;
        }
        buf.data = Hunk_AllocName(length, c"sizebuf".as_ptr()).cast::<u8>();
        buf.maxsize = length;
    }
}

/// `SZ_Clear` -- clear logical contents, not the backing bytes.
#[no_mangle]
pub unsafe extern "C" fn SZ_Clear(buf: *mut SizeBufC) {
    clear(&mut *buf);
}

/// `SZ_GetSpace` -- return an interior pointer into the caller's buffer.
#[no_mangle]
pub unsafe extern "C" fn SZ_GetSpace(buf: *mut SizeBufC, length: c_int) -> *mut c_void {
    let buf = &mut *buf;

    // C's addition is an int expression.  Valid engine calls stay in range;
    // wrapping here avoids introducing Rust's debug-only overflow panic in the
    // standalone harness while retaining the original comparison.
    if buf.cursize.wrapping_add(length) > buf.maxsize {
        if buf.allowoverflow == 0 {
            Sys_Error(
                c"SZ_GetSpace: overflow without allowoverflow set\n%s: currently %d of %d, requested %d"
                    .as_ptr(),
                if buf.name.is_null() {
                    c"unnamed buffer".as_ptr()
                } else {
                    buf.name
                },
                buf.cursize,
                buf.maxsize,
                length,
            );
        }

        if length > buf.maxsize {
            Sys_Error(
                c"SZ_GetSpace: %i is > full buffer size (%s, %d)".as_ptr(),
                length,
                if buf.name.is_null() {
                    c"unnamed buffer".as_ptr()
                } else {
                    buf.name
                },
                buf.maxsize,
            );
        }

        CON_Printf(
            PRINT_TERMONLY,
            c"SZ_GetSpace: overflow\n%s: currently %d of %d, requested %d\n".as_ptr(),
            if buf.name.is_null() {
                c"unnamed buffer".as_ptr()
            } else {
                buf.name
            },
            buf.cursize,
            buf.maxsize,
            length,
        );
        clear(buf);
        buf.overflowed = 1;
    }

    let data = buf.data.offset(buf.cursize as isize);
    buf.cursize = buf.cursize.wrapping_add(length);
    data.cast::<c_void>()
}

/// `SZ_Write` -- copy bytes into the space returned by SZ_GetSpace.
#[no_mangle]
pub unsafe extern "C" fn SZ_Write(
    buf: *mut SizeBufC,
    data: *const c_void,
    length: c_int,
) {
    let dst = SZ_GetSpace(buf, length);
    memcpy(dst, data, length as usize);
}

/// `SZ_Print` -- append a C string, replacing an existing trailing NUL.
#[no_mangle]
pub unsafe extern "C" fn SZ_Print(buf: *mut SizeBufC, data: *const c_char) {
    let len = (strlen(data) as c_int).wrapping_add(1);
    let buf_ref = &*buf;

    if buf_ref.cursize == 0 || *buf_ref.data.offset(buf_ref.cursize as isize - 1) != 0 {
        let dst = SZ_GetSpace(buf, len);
        memcpy(dst, data.cast::<c_void>(), len as usize);
    } else {
        // The C source asks for len - 1 bytes, then backs the returned pointer
        // up over the previous terminator before copying the full string.
        let dst = SZ_GetSpace(buf, len - 1).offset(-1);
        memcpy(dst, data.cast::<c_void>(), len as usize);
    }
}
