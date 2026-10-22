// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles engine/rust/mathlib/src/ffi.rs unchanged so `cargo test` can replay
// the frozen C fixtures against it (../tests/replay.rs).  Test-only; the engine
// links the file through the engine-rs staticlib, not this crate.

#[path = "../../src/ffi.rs"]
pub mod ffi;

/// Stand-in for the engine's `Sys_Error`, which ffi.rs calls from BOPS_Error and
/// FloorDivMod and which never returns.  Prints the format string and aborts.
///
/// The real one is variadic (`const char *fmt, ...`).  Stable Rust cannot
/// define a variadic function, so this takes the fixed argument only.  That is
/// sound on the System V and AArch64 Linux calling conventions the host runs:
/// a variadic caller passes the fixed argument the same way and the callee
/// ignores the rest.  The message is therefore the unformatted template.
#[no_mangle]
pub extern "C" fn Sys_Error(fmt: *const core::ffi::c_char) -> ! {
    let msg = unsafe { core::ffi::CStr::from_ptr(fmt) };
    eprintln!("Sys_Error: {}", msg.to_string_lossy());
    std::process::abort()
}
