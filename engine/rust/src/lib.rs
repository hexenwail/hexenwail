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

// msg_io reads and writes through the sizebuf ABI, so it needs SizeBufC even
// when the sizebuf functions themselves are left to the C original.  The
// layout lives in one place; which half of sizebuf.rs is compiled is decided
// there.
#[cfg(any(feature = "sizebuf", feature = "msg_io"))]
pub mod sizebuf;

#[cfg(feature = "crc")]
pub mod crc;

#[cfg(feature = "link_ops")]
pub mod link_ops;

#[cfg(feature = "msg_io")]
pub mod msg_io;

// info_str is the one port whose C original lives in hexenworld/shared rather
// than h2shared, and the one whose only compiled variant is hwsv's (H2W plus
// SERVERONLY).  It reads the cvar table rather than a global; see the module
// comment.
#[cfg(feature = "info_str")]
pub mod info_str;

#[cfg(feature = "strlcpy")]
pub mod strlcpy;

#[cfg(feature = "strlcat")]
pub mod strlcat;

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
