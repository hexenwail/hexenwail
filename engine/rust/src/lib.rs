// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine_rs -- one Rust static library per C engine target. CMake builds a
// target-specific feature set and links exactly one archive into each binary;
// there is no C fallback. Differential harnesses enable their own subsets.
//
// Copyright (C) 2026 Hexenwail contributors.
//
// Every module translated from a C file carries that file's copyright lines,
// and for a non-GPL original (the ISC strlcpy/strlcat, which are distributed
// here under the GPL) its full permission notice, in its own header.  scripts/check-rust-notices.sh enforces it and
// holds the map from each module to its C original; a new port is added there.

#![no_std]

#[cfg(any(feature = "hashindex", feature = "quakefs"))]
#[path = "../hashindex/src/ffi.rs"]
pub mod hashindex;

#[cfg(feature = "mathlib")]
#[path = "../mathlib/src/ffi.rs"]
pub mod mathlib;

// msg_io reads and writes through the sizebuf ABI, so it needs SizeBufC even
// in a harness build that leaves the sizebuf functions to the C original.  The
// layout lives in one place; which half of sizebuf.rs is compiled is decided
// there.
#[cfg(any(feature = "sizebuf", feature = "msg_io", feature = "cmd", feature = "net_udp_hw", feature = "net_chan"))]
pub mod sizebuf;

// cvar is the Phase 6 engine-state port, and info_str reads
// `sv_highchars.integer` through Cvar_FindVar, so it needs cvar_t even in a
// harness build that links only that port.  Same split as sizebuf/msg_io: the
// layout is unconditional in cvar.rs and the functions are behind
// `feature = "cvar"`.
#[cfg(any(feature = "cvar", feature = "info_str", feature = "cmd", feature = "quakefs", feature = "net_chan"))]
pub mod cvar;

// zone is the Phase 6 allocator/ownership port.  It has no ABI half of its own
// that another port needs -- its structs are private to zone.c -- so it is
// compiled only for its own feature.
#[cfg(any(feature = "zone", feature = "cmd", feature = "quakefs"))]
pub mod zone;

// cmd is the Phase 6 command/registry port.  It needs cvar_t (Cmd_CheckCommand
// walks the list directly), SizeBufC (cmd_text), QuakeParmsC (com_argc/com_argv
// are macros over host_parms) and the per-target predicates in cmd_target.c.
#[cfg(feature = "cmd")]
pub mod cmd;

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
/// `panic = "abort"`. Each target links one staticlib with this definition,
/// rather than multiple per-subsystem archives with duplicate runtimes.
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

#[cfg(feature = "quakefs")]
pub mod quakefs;

// net_udp_hw is HexenWorld's transport.  Its C original is compiled into
// glhexen2 and hwsv only, and net.h renames every symbol of it in the client,
// so the module exports the unambiguous HWNET_* spelling and the per-target C
// shim owns the C-visible storage and the plain NET_* names hwsv uses; see the
// module header.
#[cfg(feature = "net_udp_hw")]
pub mod net_udp_hw;

// HexenWorld's channel layer: the packet header, the reliable queue and the
// bandwidth choke above the transport.  It shares the sizebuf ABI (net_message
// and the channel's own buffers) and the cvar ABI (its two cvars), and like the
// transport it is built into glhexen2 and hwsv only.
#[cfg(feature = "net_chan")]
pub mod net_chan;
