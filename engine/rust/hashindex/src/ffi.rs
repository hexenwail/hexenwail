// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `extern "C"` boundary: five functions that replace hashindex.c.
//
// # Why the exported names are not prefixed
//
// 61 call sites across seven files call these functions directly, and
// `engine/h2shared/hashindex.h` is not modified by this port.  The exported
// names must therefore be byte-identical to the C originals.  What keeps the
// link free of duplicate symbols is that `hashindex.c` is removed from the
// build when `USE_RUST_HASHINDEX` is on (see engine/CMakeLists.txt) -- not a
// name prefix.
//
// # Why only five functions are here
//
// `Hash_First`, `Hash_Next`, `Hash_GenerateKeyString` and
// `Hash_GenerateKeyInt` are `static inline` in the header (hashindex.h:47,59,70,90).
// They have no symbol, so they cannot cross an FFI boundary at all: each C
// caller already has its own copy compiled in.  They stay in C.  This crate
// only has to keep the struct layout identical, which `#[repr(C)]` gives us.
//
// # Why the struct is not owned by Rust
//
// `hashindex_t` is embedded *by value* at eight sites -- four static globals
// (model.c:56, gl_model.c:101, draw.c:63, snd_dma.c:85), two non-static globals
// (gl_draw.c:180,198, extern'd from gl_rmisc.c:63-64), and two struct members
// (quakefs.c:69 inside `pack_t`, quakefs.c:140 inside `zippack_t`).  The C side
// allocates it, either as static storage or inside a Z_Malloc'd struct, and
// Rust only ever receives a pointer to it.  An RAII wrapper that allocated its
// own struct would be unusable here, so there is none.

use core::ffi::{c_char, c_int, c_void};

// --- C surface, declared rather than depended on ----------------------------
//
// Declaring these by hand instead of depending on the `libc` crate keeps the
// build hermetic: cargo never contacts crates.io, which would fail in a
// network-less nix sandbox.
extern "C" {
    pub fn malloc(size: usize) -> *mut c_void;
    pub fn free(p: *mut c_void);
    pub fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    pub fn abort() -> !;

    /// `engine/h2shared/sys.h:98`:
    /// `FUNC_NORETURN void Sys_Error (const char *error, ...) FUNC_PRINTF(1,2);`
    ///
    /// This is the C original's only failure path, and it never returns.  There
    /// is no error channel in the public API to translate into a `Result`.
    fn Sys_Error(fmt: *const c_char, ...) -> !;
}

/// `#define NULL_INDEX (-1)` -- hashindex.c:24
const NULL_INDEX: c_int = -1;

/// `typedef struct hashindex_s` -- hashindex.h:27-32, field for field.
///
/// Field order, names and types must match exactly.  The compile-time
/// assertions below pin the resulting offsets.
#[repr(C)]
pub struct HashIndexC {
    pub hash_size: c_int,
    pub hash: *mut c_int,
    pub index_chain: *mut c_int,
    pub hash_mask: c_int,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut c_int>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut c_int>();

const fn align_up(x: usize, a: usize) -> usize {
    (x + a - 1) & !(a - 1)
}

// Compile-time layout pin.  The expected offsets are written symbolically in
// terms of pointer size/alignment so the same assertion holds on ILP32 and
// LP64 -- a hardcoded 8 would silently be wrong on a 32-bit target.
//
// Two fields follow `hash_size` (a 4-byte int), so the first pointer lands on
// the next pointer-aligned boundary.  Reordering or retyping a field, or
// swapping the two pointers, breaks this at build time rather than corrupting
// memory in one of the eight by-value sites at run time.
const _: () = {
    assert!(core::mem::offset_of!(HashIndexC, hash_size) == 0);
    assert!(core::mem::offset_of!(HashIndexC, hash) == PTR_ALIGN);
    assert!(core::mem::offset_of!(HashIndexC, index_chain) == PTR_ALIGN + PTR_SIZE);
    assert!(core::mem::offset_of!(HashIndexC, hash_mask) == PTR_ALIGN + 2 * PTR_SIZE);
    assert!(
        core::mem::size_of::<HashIndexC>() == align_up(PTR_ALIGN + 2 * PTR_SIZE + 4, PTR_ALIGN)
    );
};

// --- ABI cross-check surface ------------------------------------------------
//
// Exported so the C-side differential harness can assert that its own
// `offsetof(hashindex_t, ...)` agrees with what Rust computed.  A constant
// cannot be read from C as a compile-time value, so these are accessors and
// the harness checks them at startup, before any behavioural comparison.

#[no_mangle]
pub extern "C" fn HashIndexC_sizeof() -> usize {
    core::mem::size_of::<HashIndexC>()
}

#[no_mangle]
pub extern "C" fn HashIndexC_offsetof_hash_size() -> usize {
    core::mem::offset_of!(HashIndexC, hash_size)
}

#[no_mangle]
pub extern "C" fn HashIndexC_offsetof_hash() -> usize {
    core::mem::offset_of!(HashIndexC, hash)
}

#[no_mangle]
pub extern "C" fn HashIndexC_offsetof_index_chain() -> usize {
    core::mem::offset_of!(HashIndexC, index_chain)
}

#[no_mangle]
pub extern "C" fn HashIndexC_offsetof_hash_mask() -> usize {
    core::mem::offset_of!(HashIndexC, hash_mask)
}

/// `Hash_IsPowerOfTwo` -- hashindex.c:31-34.
#[inline]
fn is_power_of_two(x: c_int) -> bool {
    x > 0 && (x & (x - 1)) == 0
}

/// `Hash_Allocate` -- hashindex.c:42-68.
///
/// `hashSize` must be a power of two.  Allocates with `malloc`, not `Z_Malloc`
/// (see the long note in the C original, uhexen2-mm4l): the zone is a small
/// fixed pool and `Z_Malloc` aborts rather than returning NULL, while two call
/// sites size this table from an archive's entry count.
#[no_mangle]
pub unsafe extern "C" fn Hash_Allocate(hi: *mut HashIndexC, hash_size: c_int) {
    // Power-of-two is checked *before* the already-initialised check, in the
    // same order as the C original, so the two error paths agree.
    if !is_power_of_two(hash_size) {
        Sys_Error(
            c"Hash_Allocate: has size %d is not power of two".as_ptr(),
            hash_size,
        );
    }

    if !(*hi).hash.is_null() {
        Sys_Error(c"Hash_Allocate: hash is already initialized".as_ptr());
    }

    (*hi).hash_size = hash_size;
    let bytes = hash_size as usize * core::mem::size_of::<c_int>();
    (*hi).hash = malloc(bytes).cast::<c_int>();
    (*hi).index_chain = malloc(bytes).cast::<c_int>();
    if (*hi).hash.is_null() || (*hi).index_chain.is_null() {
        Sys_Error(
            c"Hash_Allocate: out of memory for %d entries".as_ptr(),
            hash_size,
        );
    }

    // `memset(ptr, -1, n)` writes 0xFF to every *byte*, which is -1 for a
    // two's-complement c_int.  Replicated through the same libc call rather
    // than a Rust loop so the resulting arrays are bit-identical to the C
    // original's, including for any padding.
    memset((*hi).hash.cast::<c_void>(), -1, bytes);
    memset((*hi).index_chain.cast::<c_void>(), -1, bytes);
    (*hi).hash_mask = hash_size - 1;
}

/// `Hash_Free` -- hashindex.c:75-87.
///
/// Note what it does *not* do: `hash_size` and `hash_mask` are left untouched,
/// and only the two pointers are nulled.  The differential harness relies on
/// that, so it is preserved rather than "tidied up".
#[no_mangle]
pub unsafe extern "C" fn Hash_Free(hi: *mut HashIndexC) {
    if !(*hi).hash.is_null() {
        free((*hi).hash.cast::<c_void>());
        (*hi).hash = core::ptr::null_mut();
    }
    if !(*hi).index_chain.is_null() {
        free((*hi).index_chain.cast::<c_void>());
        (*hi).index_chain = core::ptr::null_mut();
    }
}

/// `Hash_Add` -- hashindex.c:95-108.
#[no_mangle]
pub unsafe extern "C" fn Hash_Add(hi: *mut HashIndexC, key: c_int, index: c_int) {
    if (*hi).hash.is_null() {
        Sys_Error(c"Hash_Add: hash not initialized".as_ptr());
    }

    if index < 0 || index >= (*hi).hash_size {
        Sys_Error(
            c"Hash_Add: hash index out of range %d".as_ptr(),
            index,
        );
    }

    let h = (key & (*hi).hash_mask) as isize;
    let hash = (*hi).hash;
    let chain = (*hi).index_chain;

    // Head insertion, exactly as the original: the new entry's chain slot
    // takes the current head, then becomes the head.
    *chain.offset(index as isize) = *hash.offset(h);
    *hash.offset(h) = index;
}

/// `Hash_Remove` -- hashindex.c:115-137.
#[no_mangle]
pub unsafe extern "C" fn Hash_Remove(hi: *mut HashIndexC, key: c_int, index: c_int) {
    // `k` is computed *before* the NULL check in the C original
    // (hashindex.c:117-120).  Kept in that order so the behaviour matches even
    // in the uninitialised case.
    let k = (key & (*hi).hash_mask) as isize;

    if (*hi).hash.is_null() {
        Sys_Error(c"Hash_Remove: hash not initialized".as_ptr());
    }

    let hash = (*hi).hash;
    let chain = (*hi).index_chain;

    if *hash.offset(k) == index {
        // Entry is the head of its chain: unlink by promoting the next link.
        *hash.offset(k) = *chain.offset(index as isize);
    } else {
        // Otherwise walk the chain.  Note the original does NOT report a
        // failed walk -- removing an index that is not present silently
        // updates nothing but still clears the chain slot below.
        let mut i = *hash.offset(k);
        while i != NULL_INDEX {
            if *chain.offset(i as isize) == index {
                *chain.offset(i as isize) = *chain.offset(index as isize);
                break;
            }
            i = *chain.offset(i as isize);
        }
    }
    *chain.offset(index as isize) = NULL_INDEX;
}

/// `Hash_Clear` -- hashindex.c:144-152.
///
/// Clears `hash[]` only.  `indexChain[]` is deliberately left alone -- the
/// original's comment says it is not needed, because nothing reaches the chain
/// except through `hash[]`.  Preserved as-is.
#[no_mangle]
pub unsafe extern "C" fn Hash_Clear(hi: *mut HashIndexC) {
    if (*hi).hash.is_null() {
        return;
    }

    let bytes = (*hi).hash_size as usize * core::mem::size_of::<c_int>();
    memset((*hi).hash.cast::<c_void>(), -1, bytes);
}
