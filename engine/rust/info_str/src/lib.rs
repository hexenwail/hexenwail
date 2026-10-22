// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles engine/rust/src/info_str.rs (and the cvar_t layout it reads,
// src/cvar.rs) unchanged so `cargo test` can replay the frozen C fixtures
// against it (tests/replay.rs).  Test-only; the engine links the files
// through the engine-rs staticlib, not this crate.
//
// info_str.rs reaches two engine symbols.  Both are stubbed here, with state
// the replay drives:
//
//   * CON_Printf(flags, fmt, ...) -- the function behind the Con_Printf macro.
//     Stable Rust cannot define a variadic function, so the stub takes one
//     fixed extra argument.  That is sound on the System V x86-64 and AArch64
//     Linux ABIs the host runs: a variadic caller passes the first variadic
//     pointer in the same register a third fixed parameter would use, and the
//     callee may ignore it.  Info_Print is the only caller with an argument
//     ("%s"); every other call has none, and the stub only reads the argument
//     when the format contains "%s".  It records "[flags]" plus the formatted
//     text, exactly as the fixture generator's C stub does.
//   * Cvar_FindVar(name) -- returns the one test cvar when "registered", NULL
//     otherwise (the pre-registration state), and counts lookups for any name
//     other than "sv_highchars".
//
// The state is process-global, as the real Info_ValueForKey's static buffers
// are, so the replay serialises its tests with one lock.

use core::ffi::{c_char, c_uint, CStr};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;

#[path = "../../src/cvar.rs"]
pub mod cvar;
#[path = "../../src/info_str.rs"]
pub mod info_str;

static LOG: Mutex<Vec<u8>> = Mutex::new(Vec::new());
static REGISTERED: AtomicBool = AtomicBool::new(false);
static BAD_LOOKUPS: AtomicUsize = AtomicUsize::new(0);
static mut TEST_CVAR: cvar::CvarC = unsafe { core::mem::zeroed() };

/// Takes the text CON_Printf has recorded since the last call.
pub fn take_log() -> Vec<u8> {
    std::mem::take(&mut *LOG.lock().unwrap())
}

/// Sets the state Cvar_FindVar reports: `None` is "not registered yet" (the
/// lookup misses), `Some(n)` is a registered sv_highchars with `.integer == n`.
pub fn set_highchars(state: Option<i32>) {
    unsafe {
        (*(&raw mut TEST_CVAR)).integer = state.unwrap_or(0);
    }
    REGISTERED.store(state.is_some(), Ordering::SeqCst);
}

/// Number of Cvar_FindVar calls for a name other than "sv_highchars" so far.
pub fn bad_lookups() -> usize {
    BAD_LOOKUPS.load(Ordering::SeqCst)
}

#[no_mangle]
pub extern "C" fn CON_Printf(flags: c_uint, fmt: *const c_char, arg: *const c_char) {
    let fmt = unsafe { CStr::from_ptr(fmt) }.to_bytes();
    let mut log = LOG.lock().unwrap();
    log.extend_from_slice(format!("[{flags}]").as_bytes());
    let mut i = 0;
    while i < fmt.len() {
        if fmt[i] == b'%' && fmt.get(i + 1) == Some(&b's') {
            log.extend_from_slice(unsafe { CStr::from_ptr(arg) }.to_bytes());
            i += 2;
        } else {
            log.push(fmt[i]);
            i += 1;
        }
    }
}

#[no_mangle]
pub extern "C" fn Cvar_FindVar(name: *const c_char) -> *mut cvar::CvarC {
    if unsafe { CStr::from_ptr(name) }.to_bytes() != b"sv_highchars" {
        BAD_LOOKUPS.fetch_add(1, Ordering::SeqCst);
        return core::ptr::null_mut();
    }
    if REGISTERED.load(Ordering::SeqCst) {
        &raw mut TEST_CVAR
    } else {
        core::ptr::null_mut()
    }
}
