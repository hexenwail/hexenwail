// SPDX-License-Identifier: GPL-3.0-or-later
//
// mathlib_rs -- Rust port of engine/h2shared/mathlib.c
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
// Copyright (C) 2026 Hexenwail contributors.
//
// mathlib.c is the Quake-derived engine code and is licensed GPLv2-or-later.
// This reimplementation is a derivative work.  It is distributed under
// GPLv3-or-later, which that licence's "or any later version" option permits;
// GPL-2.0-or-later would be equally valid.
//
// This crate is a drop-in, link-time replacement for the 12 exported
// functions of mathlib.c.  See src/ffi.rs for why the exported names are
// unprefixed, and why the four `static inline` helpers are not ported.
//
// It is `no_std` on purpose: it is linked into a C executable that already
// owns the runtime, so pulling in Rust's std would install a second one for no
// benefit.

#![no_std]

pub mod ffi;

/// Panic handler for the `no_std` staticlib.
///
/// The release profile sets `panic = "abort"`, so this is belt-and-braces for
/// other profiles: unwinding into a C caller is UB.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { ffi::abort() }
}

// The canonical origin vector (0, 0, 0).
//
// Kept in Rust to match the C-side `vec3_origin` global from
// `engine/h2shared/mathlib.c`.  The engine reads and writes this
// through its own objects; we just ensure the symbol is present.
//
// This is `static mut` to avoid Rust visibility semantics --
// `#[no_mangle]` controls the linkage.  It is `extern "C"` so the
// initializer is emitted unconditionally, not merged into the BSS.
#[no_mangle]
pub static mut vec3_origin: ffi::Vec3 = [0.0, 0.0, 0.0];