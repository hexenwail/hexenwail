// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine_rs -- the single Rust static library linked by the C engine while the
// subsystem ports are being migrated.  Features are selected by CMake so each
// subsystem retains an independent C fallback during the transition.
//
// Copyright (C) 2026 Hexenwail contributors.

#![no_std]

#[cfg(feature = "hashindex")]
#[path = "../hashindex/src/ffi.rs"]
pub mod hashindex;

#[cfg(feature = "mathlib")]
#[path = "../mathlib/src/ffi.rs"]
pub mod mathlib;

#[cfg(feature = "sizebuf")]
pub mod sizebuf;

/// There is exactly one panic handler for every combination of Rust ports.
/// Panics must never unwind through a C caller.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}

/// `compiler_builtins` emits a relocation for this symbol even with
/// `panic = "abort"`.  Keep the compatibility definition in the consolidated
/// library rather than duplicating it in every subsystem archive.
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

#[cfg(feature = "mathlib")]
/// The engine's C callers link against this existing global symbol.
#[no_mangle]
pub static mut vec3_origin: mathlib::Vec3 = [0.0, 0.0, 0.0];

extern "C" {
    fn abort() -> !;
}
