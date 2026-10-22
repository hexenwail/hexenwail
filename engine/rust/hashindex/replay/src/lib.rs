// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles engine/rust/hashindex/src/ffi.rs unchanged so `cargo test` can replay
// the frozen C fixtures against it (../tests/replay.rs).  Test-only; the engine
// links the file through the engine-rs staticlib, not this crate.

#[path = "../../src/ffi.rs"]
pub mod ffi;

/// Stand-in for the engine's `Sys_Error`, which ffi.rs calls from every fatal
/// path and which never returns.  Prints `Sys_Error: <message>` and aborts.
///
/// The real one is variadic (`const char *fmt, ...`).  Stable Rust cannot define
/// a variadic function, so this takes the format and ONE int.  That is sound on
/// the System V and AArch64 Linux calling conventions the host runs: a variadic
/// caller passes its first int argument in the same register a fixed second
/// parameter would use, and a call with no argument leaves the register
/// unspecified, which is why the int is read only when the template has a `%d`.
/// Every message in hashindex.c has at most one `%d`.
#[no_mangle]
pub extern "C" fn Sys_Error(fmt: *const core::ffi::c_char, arg: core::ffi::c_int) -> ! {
    let template = unsafe { core::ffi::CStr::from_ptr(fmt) }.to_string_lossy();
    let msg = if template.contains("%d") {
        template.replacen("%d", &arg.to_string(), 1)
    } else {
        template.into_owned()
    };
    eprintln!("Sys_Error: {msg}");
    std::process::abort()
}
