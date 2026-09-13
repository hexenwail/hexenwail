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

/// Personality routine stub for the C link.
///
/// `compiler_builtins`, which rustc links into every staticlib, emits a
/// reference to `rust_eh_personality` even under `panic = "abort"` -- via its
/// `DW.ref` section, so it is a real relocation rather than a discarded weak
/// reference, and `ld` rejects the link without it.
///
/// This crate contains no `catch_unwind` and no code that can unwind, so the
/// only way to abort is to call `Sys_Error` directly.
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}