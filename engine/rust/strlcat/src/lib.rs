// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles engine/rust/src/strlcat.rs unchanged so `cargo test` can replay the
// frozen C fixtures against it (tests/replay.rs).  Test-only; the engine
// links the file through the engine-rs staticlib, not this crate.

#[path = "../../src/strlcat.rs"]
pub mod strlcat;
