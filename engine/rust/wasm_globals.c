/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * wasm_globals.c -- the data the Rust engine archive does not provide on the
 * WebAssembly client.  Compiled into the Emscripten build only.
 *
 * 1. Exported statics.  msg_io.rs owns msg_readcount and msg_badread, and
 *    lib.rs owns vec3_origin, as `#[no_mangle] static mut`.  On every native
 *    target that makes them ordinary global data in libengine_rs.a.  For
 *    wasm32-unknown-emscripten rustc keeps the archive's functions global but
 *    emits those statics as local symbols (`d msg_readcount.0`), with or
 *    without #[used], so the C that reads them -- cl_parse.c, cl_tent.c,
 *    gl_sky.c and the rest -- fails to link.  On that target the Rust
 *    declares them extern (see the `target_family = "wasm"` arms) and this
 *    file defines them, with the C types and the zero initial values the Rust
 *    statics have.
 *
 * 2. sincos_tab.  mathlib.c defines this read-only table only when
 *    USE_SINCOS_TABLE is set, which mathlib.h does for builds that are
 *    neither GLQUAKE nor SERVERONLY -- in this tree, only the restored 8bpp
 *    software renderer (WEB_RENDERER=software).  The Rust mathlib port was
 *    written against the GL and server targets and has no copy, and on this
 *    target it could not export one anyway, so the table comes from the same
 *    sincos.h here, under the same condition.
 *
 * wad.rs owns three more (wad_numlumps, wad_lumps, wad_base) and they are
 * deliberately not here: wad.h declares them but no C outside wad.c ever
 * read them, so on wasm they are Rust-private and nothing needs the storage.
 * If C starts reading one, the web link fails on it -- loudly -- and it
 * belongs in this file with an extern in wad.rs, as for the three above.
 *
 * engine/CMakeLists.txt adds this file to Emscripten builds only, and
 * scripts/check-rust-abi.sh --wasm links it into its layout check.
 */

/* The engine's own declarations, so a type that drifts from them is a
 * compile error here rather than a silent size mismatch at link time.
 * quakedef.h brings mathlib.h, sizebuf.h and msg_io.h in the order they
 * need, as mathlib.c includes it. */
#include "quakedef.h"

int		msg_readcount;
qboolean	msg_badread;
vec3_t		vec3_origin;

#ifdef USE_SINCOS_TABLE
const float sincos_tab[SINCOS_SIZE] = {
#include "sincos.h"
};
#endif
