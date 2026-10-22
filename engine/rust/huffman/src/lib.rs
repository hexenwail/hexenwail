// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles engine/rust/src/huffman.rs unchanged so `cargo test` can replay the
// frozen C fixtures against it (tests/replay.rs).  Test-only; the engine
// links the file through the engine-rs staticlib, not this crate.

#[path = "../../src/huffman.rs"]
pub mod huffman;

use std::ffi::{c_char, CStr};

/// The one engine symbol huffman.rs reaches (malloc, memset and memcpy come
/// from libc, which std already links).  Declared variadic on the Rust side
/// of huffman.rs; defined here with the fixed prefix only, which is ABI
/// compatible on the x86-64 and aarch64 hosts the gate runs on because the
/// caller, not the callee, owns the variadic registers.  Every call site in
/// huffman.rs is a fatal path the fixed frequency table cannot reach, so
/// reaching it during a replay is a failure: print the message and abort.
#[no_mangle]
pub extern "C" fn Sys_Error(fmt: *const c_char) -> ! {
    let msg = unsafe { CStr::from_ptr(fmt) }.to_string_lossy();
    eprintln!("Sys_Error called by huffman.rs: {msg}");
    std::process::abort()
}
