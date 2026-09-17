// SPDX-License-Identifier: GPL-3.0-or-later
//
// hashindex_rs -- Rust port of engine/h2shared/hashindex.c
//
// Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
// Copyright (C) 2026 Hexenwail contributors.
//
// hashindex.c descends from the Doom 3 GPL source and is licensed
// GPLv3-or-later, unlike much of the surrounding engine code, which is
// GPLv2-or-later.  This reimplementation is a derivative work and carries the
// same GPLv3-or-later terms.
//
// This crate is a drop-in, link-time replacement for the five exported
// functions of hashindex.c.  See src/ffi.rs for why the exported names are
// unprefixed, and why the four `static inline` helpers are not ported.
//
// It is `no_std` on purpose: it is linked into a C executable that already
// owns the runtime, so pulling in Rust's std would install a second one for no
// benefit.

#![no_std]

use core::ffi::c_int;

/// Panic hook for the `no_std` staticlib.
///
/// The release profile sets `panic = "abort"`, so this is belt-and-braces for
/// other profiles: unwinding into a C caller is UB.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { ffi::abort() }
}

pub mod ffi;

/// Personality routine stub for the C link.
///
/// `compiler_builtins`, which rustc links into every staticlib, emits a
/// reference to `rust_eh_personality` even under `panic = "abort"` -- via its
/// `DW.ref` section, so it is a real relocation rather than a discarded weak
/// reference, and `ld` rejects the link without it:
///
///     undefined reference to `rust_eh_personality'
///
/// A C executable has no reason to define it.  The body is unreachable: with
/// `panic = "abort"` there is no unwinding to require a personality routine,
/// and this crate contains no `catch_unwind` and no code that can unwind.
/// It exists purely to satisfy the relocation.
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

/// The `hashindex_t` layout, re-exported for the C-side ABI cross-check.
pub use ffi::HashIndexC;

/// `qboolean` is `typedef int` in this codebase (`common/q_stdinc.h:126`), not
/// C99 `bool`.  Re-exported under a name that says so, so no future caller is
/// tempted to use Rust's 1-byte `bool` at the boundary.
pub type QBool = c_int;
