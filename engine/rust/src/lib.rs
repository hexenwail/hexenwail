// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine_rs -- the single Rust static library linked into every C engine
// target.  CMake always builds it with every feature; there is no C fallback.
// The features exist so each differential harness under engine/rust/*/tests
// can link the one subsystem it compares against its C original.
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
// in a harness build that leaves the sizebuf functions to the C original.  The
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

// huffman is the second port of a file in hexenworld/shared, and the first
// whose C original is compiled into two targets with a different allocation
// strategy per target -- hwsv allocates the tree from the hunk, the
// integrated client must not.  The archive is built once and linked into
// both, so this module uses malloc for both; see the module comment.
#[cfg(feature = "huffman")]
pub mod huffman;

// wad is the only port whose C original is compiled into a single target:
// wad.c is in COMMON_SOURCES and is removed again for h2ded, and HWSV_SOURCES
// never lists it.  It also owns three exported globals (wad_numlumps,
// wad_lumps, wad_base) and edits the mapped wad in place; see the module
// comment.
#[cfg(feature = "wad")]
pub mod wad;

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

/// The engine's C callers link against this existing global symbol.
///
/// Not on wasm: rustc makes `#[no_mangle] static`s local in a wasm32
/// staticlib, so the web client takes the storage from
/// engine/rust/wasm_globals.c instead.  Nothing in Rust reads it.
#[cfg(all(feature = "mathlib", not(target_family = "wasm")))]
#[no_mangle]
pub static mut vec3_origin: mathlib::Vec3 = [0.0, 0.0, 0.0];

extern "C" {
    fn abort() -> !;
}
