// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/quakefs.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
// Copyright (C) 2026 Hexenwail contributors.
//
// The file system: the searchpath list of gamedirs and mounted archives, the
// PAK and ZIP loaders, the FS_* file layer, the name-listing commands and the
// gamedir switch.  Three properties of the C original shape the port.
//
// 1. The client half calls symbols no dedicated binary has.  Host_ClearMemory,
//    CL_Disconnect, Draw_ReInit, TexMgr_NewGame, BGM_Stop, M_BuildBindList,
//    VID_Lock, Cmd_StartupScript, Con_ShowList and the client_state_t `cls` are
//    compiled into glhexen2 and the web client and into nothing else, so a
//    direct reference here would be an undefined symbol in h2ded and hwsv --
//    one archive, one object, every extern must resolve everywhere (issue
//    #233).  Every such site is a small contiguous statement group and goes
//    through engine/rust/quakefs_target.c instead, which is C compiled per
//    target and may name them freely.  See the hook list at the bottom of that
//    file.  Cache_Flush/Cache_Alloc are deliberately NOT hooked: they are the
//    zone port's symbols now, defined in every binary, and flushing an empty
//    cache is a no-op, so the dedicated targets' compiled-out arms stay inert.
//
// 2. Six globals are exported and read by C outside this file:
//    fs_gamedir_nopath, gameflags, fs_filesize, file_from_pak, oem and
//    registered.  On every target except wasm32 they are `#[no_mangle]
//    static`s here; rustc emits those as local symbols in a wasm32 staticlib,
//    so there the storage is C's, in engine/rust/wasm_globals.c, exactly as
//    cmd_source and msg_readcount are handled.
//
// 3. pack_t and zippack_t embed a hashindex_t by value, so this module uses
//    the hashindex port's HashIndexC layout rather than restating it, and the
//    hashindex/cache_user_t types come from the ports that own them.
//
// The C's statement order is the contract and is kept literally: the search
// path is built in the order FS_Init builds it, Host_Game_f unwinds to
// fs_base_nomp_searchpaths rather than fs_base_searchpaths (uhexen2-5vb6), and
// the DEVELOPER-only diagnostics keep their text.

use core::ffi::{c_char, c_int, c_long, c_uint, c_void};
use core::ffi::CStr;

use crate::cvar::CvarC;
use crate::hashindex::HashIndexC;
use crate::zone::CacheUserC;

//============================================================================
// constants
//============================================================================

/// `MAX_QPATH` from engine/hexen2/quakedef.h.
const MAX_QPATH: usize = 64;
/// `MAX_OSPATH` from common/q_stdinc.h.
const MAX_OSPATH: usize = 256;
/// `PAK_PATH_LENGTH` from common/pakfile.h -- a pak entry's name field.
const PAK_PATH_LENGTH: usize = 56;
/// `MAX_FILES_IN_PACK` from common/pakfile.h.
const MAX_FILES_IN_PACK: c_int = 2048;
/// `MAX_PK3_PER_DIR`, `MAX_FILES_IN_ZIP` and `MAX_ZIP_INFLATE` from quakefs.c.
const MAX_PK3_PER_DIR: usize = 64;
const MAX_FILES_IN_ZIP: c_int = 65536;
const MAX_ZIP_INFLATE: c_long = 64 * 1024 * 1024;
/// `MAX_LISTNAMES` from quakefs.c -- the tab-completion name buffer.
const MAX_LISTNAMES: usize = 256;
/// `MAX_PAKDATA` and `MAX_CONTENTDATA` from quakefs.c.
const MAX_PAKDATA: usize = 5;
const MAX_CONTENTDATA: usize = 3;

/// `IDPAKHEADER` from common/pakfile.h.
const IDPAKHEADER: u32 = (b'K' as u32) << 24 | (b'C' as u32) << 16 | (b'A' as u32) << 8 | (b'P' as u32);

/// `CVAR_ROM` from engine/h2shared/cvar.h -- the one cvar flag this file
/// applies, to the two read-only cvars it registers.  Spelled here rather than
/// borrowed from the cvar port, which gates its own copy on its feature.
const CVAR_ROM: c_uint = 1 << 6;

/// The game-data flags from engine/h2shared/quakefs.h.
const GAME_DEMO: c_uint = 1 << 0;
const GAME_OEM: c_uint = 1 << 1;
const GAME_OEM0: c_uint = 1 << 2;
const GAME_OEM2: c_uint = 1 << 3;
const GAME_REGISTERED: c_uint = 1 << 4;
const GAME_REGISTERED0: c_uint = 1 << 5;
const GAME_REGISTERED1: c_uint = 1 << 6;
const GAME_PORTALS: c_uint = 1 << 7;
const GAME_HEXENWORLD: c_uint = 1 << 8;
const GAME_OLD_CDROM0: c_uint = 1 << 9;
const GAME_OLD_CDROM1: c_uint = 1 << 10;
const GAME_OLD_DEMO: c_uint = 1 << 11;
const GAME_REGISTERED_OLD: c_uint = 1 << 12;
const GAME_OLD_OEM: c_uint = 1 << 13;
const GAME_OLD_OEM0: c_uint = 1 << 14;
const GAME_OLD_OEM2: c_uint = 1 << 15;
const GAME_MODIFIED: c_uint = 1 << 16;

/// `FS_ENT_*` from quakefs.h.
const FS_ENT_NONE: c_int = 0;
const FS_ENT_FILE: c_int = 1 << 0;
const FS_ENT_DIRECTORY: c_int = 1 << 1;

/// `LOADFILE_*` from quakefs.c: how a loaded file's buffer is allocated.
const LOADFILE_HUNK: c_int = 1;
const LOADFILE_TEMPHUNK: c_int = 2;
const LOADFILE_CACHE: c_int = 3;
const LOADFILE_STACK: c_int = 4;
const LOADFILE_MALLOC: c_int = 5;

/// The MakePath bases from common/filenames.h, as `FS_MakePath` takes them.
/// The numbers are positional in that enum and must match it.  Prefixed here
/// because the plain names are this file's own state (`fs_gamedir` and
/// friends) rather than the bases a path can be made against.
const MAKEPATH_BASEDIR: c_int = 0;
const MAKEPATH_GAMEDIR: c_int = 1;
const MAKEPATH_USERDIR: c_int = 2;
const MAKEPATH_USERBASE: c_int = 3;

/// `MAX_FILES_IN_PACK` and `MAX_OSPATH` bounds the pack loader enforces.
const MAX_PAK_SIZE: c_long = 0x7FFFFFFF;

//============================================================================
// the shared layouts
//============================================================================

/// `dpackfile_t` from common/pakfile.h -- a pak directory entry, on disk and
/// in memory.
#[repr(C)]
pub struct PackFileC {
    pub name: [c_char; PAK_PATH_LENGTH],
    pub filepos: c_int,
    pub filelen: c_int,
}

/// `dpackheader_t` from common/pakfile.h.
#[repr(C)]
pub struct PackHeaderC {
    pub id: [c_char; 4],
    pub dirofs: c_int,
    pub dirlen: c_int,
}

/// `pakfiles_t` from quakefs.c.
#[repr(C)]
pub struct PakFilesC {
    pub name: [c_char; MAX_QPATH],
    pub filepos: c_int,
    pub filelen: c_int,
}

/// `pack_t` from quakefs.c.
#[repr(C)]
pub struct PackC {
    pub filename: [c_char; MAX_OSPATH],
    pub handle: *mut CFile,
    pub numfiles: c_int,
    pub files: *mut PakFilesC,
    pub hash: HashIndexC,
}

/// `zipfiles_t` from quakefs.c.
#[repr(C)]
pub struct ZipFilesC {
    pub name: [c_char; MAX_QPATH],
    pub index: c_uint,
    pub filelen: c_int,
    pub filepos: c_int,
}

/// `zippack_t` from quakefs.c.  `archive` is miniz's `mz_zip_archive`, which
/// stays C: the port only ever passes its address to `mz_*`.
#[repr(C)]
pub struct ZipPackC {
    pub filename: [c_char; MAX_OSPATH],
    pub handle: *mut CFile,
    pub archive: *mut c_void,
    pub numfiles: c_int,
    pub files: *mut ZipFilesC,
    pub hash: HashIndexC,
}

/// `searchpath_t` from quakefs.c.
#[repr(C)]
pub struct SearchPathC {
    pub path_id: c_uint,
    pub filename: [c_char; MAX_OSPATH],
    pub pack: *mut PackC,
    pub zip: *mut ZipPackC,
    pub next: *mut SearchPathC,
}

// The paкdata/contentmark tables are internal to the C file -- `static`, never
// passed across the ABI -- so they are modelled with borrowed C strings rather
// than raw pointers, which keeps them immutable and shareable.  Their contents
// are copied from quakefs.c and the gate compares them entry for entry.

/// `pakdata_t` from quakefs.c.
struct PakDataC {
    numfiles: c_int,
    crc: c_uint,
    size: c_long,
    dirname: &'static CStr,
}

/// `contentmark_t` from quakefs.c.
struct ContentMarkC {
    name: &'static CStr,
    filelen: c_int,
}

/// The anonymous `contentdata[]` element from quakefs.c.
struct ContentDataC {
    marks: &'static [ContentMarkC],
    dirname: &'static CStr,
    gameflag: c_uint,
}

/// `fshandle_t` from quakefs.h.
#[repr(C)]
pub struct FSHandleC {
    pub file: *mut CFile,
    pub pak: c_int,
    pub start: c_long,
    pub length: c_long,
    pub pos: c_long,
    pub data: *mut u8,
}

/// `FILE` is opaque to the port: it is only ever passed to libc.
pub type CFile = c_void;

const PTR_SIZE: usize = core::mem::size_of::<*mut c_void>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut c_void>();
const LONG_SIZE: usize = core::mem::size_of::<c_long>();
const LONG_ALIGN: usize = core::mem::align_of::<c_long>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

// Pointer-width-derived, because the same archive is built for wasm32 where a
// pointer and a long are four bytes; the ABI gate runs the same checks under
// node for exactly that reason.
const _: () = {
    assert!(core::mem::offset_of!(PackFileC, filepos) == PAK_PATH_LENGTH);
    assert!(core::mem::size_of::<PackFileC>() == PAK_PATH_LENGTH + 8);

    assert!(core::mem::offset_of!(PackHeaderC, dirofs) == 4);
    assert!(core::mem::size_of::<PackHeaderC>() == 12);

    assert!(core::mem::offset_of!(PakFilesC, filepos) == MAX_QPATH);
    assert!(core::mem::size_of::<PakFilesC>() == MAX_QPATH + 8);

    assert!(core::mem::offset_of!(PackC, filename) == 0);
    assert!(core::mem::offset_of!(PackC, handle) == MAX_OSPATH);
    assert!(core::mem::offset_of!(PackC, numfiles) == MAX_OSPATH + PTR_SIZE);
    assert!(core::mem::offset_of!(PackC, files) == MAX_OSPATH + PTR_SIZE + 8 + 0);

    assert!(core::mem::offset_of!(ZipFilesC, index) == MAX_QPATH);
    assert!(core::mem::offset_of!(ZipFilesC, filelen) == MAX_QPATH + 4);
    assert!(core::mem::offset_of!(ZipFilesC, filepos) == MAX_QPATH + 8);

    assert!(core::mem::offset_of!(SearchPathC, path_id) == 0);
    assert!(core::mem::offset_of!(SearchPathC, filename) == 4);

    // fshandle_t mixes a pointer, an int and three longs, so its offsets are
    // the C compiler's padding rules and not a formula worth restating by
    // hand: derive them from the widths, exactly as the other ports do for
    // the structs whose shape depends on the target.
    assert!(core::mem::offset_of!(FSHandleC, file) == 0);
    assert!(core::mem::offset_of!(FSHandleC, pak) == PTR_SIZE);
    assert!(core::mem::offset_of!(FSHandleC, start) == align_up(PTR_SIZE + 4, LONG_ALIGN));
    assert!(core::mem::offset_of!(FSHandleC, length)
        == align_up(PTR_SIZE + 4, LONG_ALIGN) + LONG_SIZE);
    assert!(core::mem::offset_of!(FSHandleC, pos)
        == align_up(PTR_SIZE + 4, LONG_ALIGN) + 2 * LONG_SIZE);
    assert!(core::mem::offset_of!(FSHandleC, data)
        == align_up(align_up(PTR_SIZE + 4, LONG_ALIGN) + 3 * LONG_SIZE, PTR_ALIGN));
    assert!(core::mem::size_of::<FSHandleC>()
        == align_up(align_up(align_up(PTR_SIZE + 4, LONG_ALIGN) + 3 * LONG_SIZE, PTR_ALIGN) + PTR_SIZE, PTR_ALIGN));

    // HashIndexC and CacheUserC are the hashindex and zone ports' own layouts,
    // checked against the real C structs by their gates and by
    // scripts/check-rust-abi.sh; this module embeds them by value and does not
    // restate their sizes, which is the whole point of importing them.
};

//============================================================================
// the tables, copied from quakefs.c
//============================================================================

/// `pakdata[]` -- Raven's shipped paks, by file count, directory CRC and size.
static PAKDATA: [PakDataC; MAX_PAKDATA] = [
    PakDataC { numfiles: 696, crc: 34289, size: 22704056, dirname: c"data1" },
    PakDataC { numfiles: 523, crc: 2995, size: 75601170, dirname: c"data1" },
    PakDataC { numfiles: 183, crc: 4807, size: 17742721, dirname: c"data1" },
    PakDataC { numfiles: 245, crc: 1478, size: 49089114, dirname: c"portals" },
    PakDataC { numfiles: 102, crc: 41062, size: 10780245, dirname: c"hw" },
];

/// `demo_pakdata[]` -- the Nov 1997 demo's single pak.
static DEMO_PAKDATA: [PakDataC; 1] = [
    PakDataC { numfiles: 797, crc: 22780, size: 27750257, dirname: c"data1" },
];

/// `oem0_pakdata[]` -- the Matrox m3D bundle's Continent of Blackmarsh pak.
static OEM0_PAKDATA: [PakDataC; 1] = [
    PakDataC { numfiles: 697, crc: 9787, size: 22720659, dirname: c"data1" },
];

/// `old_pakdata[]` -- the pre-1.11 CDs, demos and OEM bundles.
static OLD_PAKDATA: [PakDataC; 7] = [
    PakDataC { numfiles: 697, crc: 53062, size: 21714275, dirname: c"data1" },
    PakDataC { numfiles: 525, crc: 47762, size: 76958474, dirname: c"data1" },
    PakDataC { numfiles: 701, crc: 20870, size: 23537707, dirname: c"data1" },
    PakDataC { numfiles: -1, crc: 0, size: 22719295, dirname: c"data1" },
    PakDataC { numfiles: -1, crc: 0, size: 17739969, dirname: c"data1" },
    PakDataC { numfiles: 98, crc: 25864, size: 10678369, dirname: c"hw" },
    PakDataC { numfiles: 40, crc: 48258, size: 3357888, dirname: c"hw" },
];

/// `mark_data1_pak0[]` -- the v1.11 registered pak0's fingerprint.
static MARK_DATA1_PAK0: [ContentMarkC; 6] = [
    ContentMarkC { name: c"maplist.txt", filelen: 193 },
    ContentMarkC { name: c"puzzles.txt", filelen: 1131 },
    ContentMarkC { name: c"default.cfg", filelen: 2793 },
    ContentMarkC { name: c"gfx.wad", filelen: 10732 },
    ContentMarkC { name: c"gfx/palette.lmp", filelen: 768 },
    ContentMarkC { name: c"maps/demo1.bsp", filelen: 1896452 },
];

/// `mark_data1_pak1[]` -- the v1.11 registered pak1's fingerprint.
static MARK_DATA1_PAK1: [ContentMarkC; 4] = [
    ContentMarkC { name: c"gfx/pop.lmp", filelen: 256 },
    ContentMarkC { name: c"maps/eidolon.bsp", filelen: 528796 },
    ContentMarkC { name: c"maps/cath.bsp", filelen: 2013328 },
    ContentMarkC { name: c"maps/egypt1.bsp", filelen: 2338472 },
];

/// `mark_portals[]` -- the Portal of Praevus fingerprint.
static MARK_PORTALS: [ContentMarkC; 4] = [
    ContentMarkC { name: c"default.cfg", filelen: 3164 },
    ContentMarkC { name: c"maps/keep1.bsp", filelen: 2500124 },
    ContentMarkC { name: c"maps/tibet1.bsp", filelen: 2463304 },
    ContentMarkC { name: c"maps/tibet7.bsp", filelen: 2749412 },
];

/// `contentdata[]` -- which marks identify which edition, and where Raven
/// shipped it.
static CONTENTDATA: [ContentDataC; MAX_CONTENTDATA] = [
    ContentDataC { marks: &MARK_DATA1_PAK0, dirname: c"data1", gameflag: GAME_REGISTERED0 },
    ContentDataC { marks: &MARK_DATA1_PAK1, dirname: c"data1", gameflag: GAME_REGISTERED1 },
    ContentDataC { marks: &MARK_PORTALS, dirname: c"portals", gameflag: GAME_PORTALS },
];

/// `pop[]` -- the graphic that has to be in the pak for registered features.
/// Used only by the CRC walk in check_known_paks.
static POP: [u16; 128] = [
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x6600, 0x0000, 0x0000, 0x0000, 0x6600, 0x0000,
    0x0000, 0x0066, 0x0000, 0x0000, 0x0000, 0x0000, 0x0067, 0x0000,
    0x0000, 0x6665, 0x0000, 0x0000, 0x0000, 0x0000, 0x0065, 0x6600,
    0x0063, 0x6561, 0x0000, 0x0000, 0x0000, 0x0000, 0x0061, 0x6563,
    0x0064, 0x6561, 0x0000, 0x0000, 0x0000, 0x0000, 0x0061, 0x6564,
    0x0064, 0x6564, 0x0000, 0x6469, 0x6969, 0x6400, 0x0064, 0x6564,
    0x0063, 0x6568, 0x6200, 0x0064, 0x6864, 0x0000, 0x6268, 0x6563,
    0x0000, 0x6567, 0x6963, 0x0064, 0x6764, 0x0063, 0x6967, 0x6500,
    0x0000, 0x6266, 0x6769, 0x6a68, 0x6768, 0x6a69, 0x6766, 0x6200,
    0x0000, 0x0062, 0x6566, 0x6666, 0x6666, 0x6666, 0x6562, 0x0000,
    0x0000, 0x0000, 0x0062, 0x6364, 0x6664, 0x6362, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0062, 0x6662, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0061, 0x6661, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x6500, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x6400, 0x0000, 0x0000, 0x0000,
];

//============================================================================
// the ported state
//============================================================================

/// `static searchpath_t *fs_searchpaths` -- every directory and archive on the
/// path, most recently added first.
static mut FS_SEARCHPATHS: *mut SearchPathC = core::ptr::null_mut();
/// `static searchpath_t *fs_base_searchpaths` -- the base without gamedirs.
static mut FS_BASE_SEARCHPATHS: *mut SearchPathC = core::ptr::null_mut();
/// `static searchpath_t *fs_base_nomp_searchpaths` -- the base before the
/// mission pack was folded in.  Host_Game_f unwinds to this one; see the long
/// comment at the unwind site (uhexen2-5vb6).
static mut FS_BASE_NOMP_SEARCHPATHS: *mut SearchPathC = core::ptr::null_mut();
/// `static zippack_t *fs_lastzip`, `static const zipfiles_t *fs_lastzipentry`
/// -- state left by the most recent FS_OpenFile_Internal, set only when the
/// lookup landed on a DEFLATED entry.
static mut FS_LASTZIP: *mut ZipPackC = core::ptr::null_mut();
static mut FS_LASTZIPENTRY: *const ZipFilesC = core::ptr::null();
/// `static const char *fs_basedir`.
static mut FS_BASEDIR: *const c_char = core::ptr::null();
/// `static char fs_gamedir[MAX_OSPATH]` and `static char fs_userdir[...]`.
static mut FS_GAMEDIR: [c_char; MAX_OSPATH] = [0; MAX_OSPATH];
static mut FS_USERDIR: [c_char; MAX_OSPATH] = [0; MAX_OSPATH];
/// `static unsigned int fs_portals_path_id`.
static mut FS_PORTALS_PATH_ID: c_uint = 0;
/// `static byte *loadbuf`, `static cache_user_t *loadcache`, `static long
/// loadsize`, `static int zone_num` -- the LOADFILE_STACK and LOADFILE_CACHE
/// state, set by FS_LoadFile's callers.
static mut LOADBUF: *mut u8 = core::ptr::null_mut();
static mut LOADCACHE: *mut CacheUserC = core::ptr::null_mut();
static mut LOADSIZE: c_long = 0;
static mut ZONE_NUM: c_int = 0;
/// `static int listname_count`, `static char *listnames[MAX_LISTNAMES]`.
static mut LISTNAME_COUNT: c_int = 0;
static mut LISTNAMES: [*mut c_char; MAX_LISTNAMES] = [core::ptr::null_mut(); MAX_LISTNAMES];
/// `static char fs_lastfile_source[MAX_OSPATH]`.
static mut FS_LASTFILE_SOURCE: [c_char; MAX_OSPATH] = [0; MAX_OSPATH];
/// `static searchpath_t *fs_hw_saved_paths` and the gamedir it restores.
static mut FS_HW_SAVED_PATHS: *mut SearchPathC = core::ptr::null_mut();
static mut FS_HW_SAVED_GAME: [c_char; MAX_QPATH] = [0; MAX_QPATH];
static mut FS_HW_SAVED_GAMEDIR: [c_char; MAX_OSPATH] = [0; MAX_OSPATH];
static mut FS_HW_SAVED_USERDIR: [c_char; MAX_OSPATH] = [0; MAX_OSPATH];
static mut FS_HW_SAVED_FLAGS: c_uint = 0;

/*----------------------------------------------------------------------------
 * the exported globals
 *
 * On every target but wasm32 the port owns the storage; there rustc emits
 * #[no_mangle] statics as local symbols and the C in
 * engine/rust/wasm_globals.c owns it, with these declarations pointing at it.
 * Same names, types and initial values either way.
 *--------------------------------------------------------------------------*/

/// `char fs_gamedir_nopath[MAX_QPATH]` -- the current gamedir's name.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut fs_gamedir_nopath: [c_char; MAX_QPATH] = [0; MAX_QPATH];
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut fs_gamedir_nopath: [c_char; MAX_QPATH];
}

/// `unsigned int gameflags` -- which editions' data was found at startup.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut gameflags: c_uint = 0;
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut gameflags: c_uint;
}

/// `long fs_filesize` -- the size of the last file opened through the FS API.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut fs_filesize: c_long = 0;
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut fs_filesize: c_long;
}

/// `int file_from_pak` -- whether the last file opened came from a pak.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut file_from_pak: c_int = 0;
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut file_from_pak: c_int;
}

/// `cvar_t oem = {"oem", "0", CVAR_ROM}` -- registered by FS_Init.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut oem: CvarC = CvarC {
    name: c"oem".as_ptr(),
    string: c"0".as_ptr() as *mut c_char,
    flags: CVAR_ROM,
    value: 0.0,
    integer: 0,
    callback: None,
    next: core::ptr::null_mut(),
    default_string: core::ptr::null_mut(),
};
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut oem: CvarC;
}

/// `cvar_t registered = {"registered", "0", CVAR_ROM}` -- registered by
/// FS_Init.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut registered: CvarC = CvarC {
    name: c"registered".as_ptr(),
    string: c"0".as_ptr() as *mut c_char,
    flags: CVAR_ROM,
    value: 0.0,
    integer: 0,
    callback: None,
    next: core::ptr::null_mut(),
    default_string: core::ptr::null_mut(),
};
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut registered: CvarC;
}

// ==== PORT CONTINUES ====
