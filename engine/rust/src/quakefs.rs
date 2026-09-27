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

//
// 4. One asymmetry decides what crosses the boundary and what does not.
//    Rust can *call* a C-variadic function on stable but cannot *define* one.
//    So FS_MakePath_VA and FS_MakePath_VABUF -- variadic exports used by
//    thirteen C call sites -- stay C in engine/rust/quakefs_variadic.c, which
//    owns nothing but the va_list plumbing and asks this module for the buffer
//    and the base half; while va(), the rotating-buffer formatter, is called
//    from here directly, because calling it is an ordinary FFI call.  The same
//    file also keeps FS_ResolveCasePath, for a different reason: it reads
//    `struct dirent`, whose layout is the platform's and not the C standard's.
//    That file is per *build*, not per target, which is why it is not
//    quakefs_target.c.

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
/// `MAX_MATCHES` from engine/h2shared/cmd.h:141 -- the cap the List* callers
/// pass into their completion buffer, so fillMatches stops at the same place.
const MAX_MATCHES: c_int = 128;
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

/// `LOADFILE_*` from quakefs.c:1912-1917: how a loaded file's buffer is
/// allocated.  Each variant names its own lifetime, and the port keeps them
/// distinct rather than unifying them -- the callers free or re-read them
/// differently, and the harness compares the bytes each returns.
const LOADFILE_ZONE: c_int = 0;
const LOADFILE_HUNK: c_int = 1;
const LOADFILE_TEMPHUNK: c_int = 2;
const LOADFILE_CACHE: c_int = 3;
const LOADFILE_STACK: c_int = 4;
const LOADFILE_MALLOC: c_int = 5;

/// The MakePath bases from common/filenames.h, as `FS_MakePath` takes them.
/// The numbers are positional in that enum and must match it.  Prefixed here
/// because the plain names are this file's own state (`fs_gamedir` and
/// friends) rather than the bases a path can be made against.
/// The values are quakefs.h:232-235, which orders them FS_BASEDIR,
/// FS_USERBASE, FS_GAMEDIR, FS_USERDIR -- not the "basedir, gamedir,
/// userdir, userbase" order the names suggest.  init_MakePath's switch is the
/// authority and this follows it, not the mnemonic.
const MAKEPATH_BASEDIR: c_int = 0;
const MAKEPATH_USERBASE: c_int = 1;
const MAKEPATH_GAMEDIR: c_int = 2;
const MAKEPATH_USERDIR: c_int = 3;

/// `MAX_FILES_IN_PACK` and `MAX_OSPATH` bounds the pack loader enforces.
const MAX_PAK_SIZE: c_long = 0x7FFFFFFF;

//============================================================================
// the shared layouts
//============================================================================

/// `dpackfile_t` from common/pakfile.h -- a pak directory entry, on disk and
/// in memory.
#[repr(C)]
#[derive(Clone, Copy)]
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
    pub archive: MzZipArchive,
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
    // numfiles is an int and files is a pointer, so the padding between them
    // is the pointer's alignment, not a constant: 4 bytes of padding on LP64,
    // none on ILP32.  The wasm arm of check-rust-abi.sh is what caught the
    // hardcoded 8 here -- the layout has to be derived for any pointer width.
    assert!(core::mem::offset_of!(PackC, files)
        == align_up(MAX_OSPATH + PTR_SIZE + 4, PTR_ALIGN));

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

//============================================================================
// the four hashindex inlines, reproduced
//============================================================================
// `Hash_First`, `Hash_Next`, `Hash_GenerateKeyString` and `Hash_GenerateKeyInt`
// are `static inline` in engine/h2shared/hashindex.h (:47, :59, :70, :90): they
// have no symbol, so no FFI can reach them and every C caller compiles its own
// copy.  quakefs.c calls the first three nine times (445-449, 604-607, 820-823,
// 1550-1580), so the faithful translation is another copy here rather than an
// export from the hashindex port -- which is what that port's own header note
// says, and why it does not export them either.
//
// The bodies are the header's, including the one detail that matters for
// parity: `*string` is a C `char`, so a byte at or above 0x80 arrives
// sign-extended, `q_tolower` leaves it negative, and the hash accumulator goes
// negative with it.  Reading the byte as `c_char` and widening reproduces that.

/// `q_isupper` from common/q_ctype.h:32 -- ASCII A-Z only.
#[inline]
fn q_isupper(c: c_int) -> bool {
    c >= b'A' as c_int && c <= b'Z' as c_int
}

/// `q_tolower` from common/q_ctype.h:89 -- `c | ('a' - 'A')` when upper, and
/// the byte unchanged (including negative) otherwise.
#[inline]
fn q_tolower(c: c_int) -> c_int {
    if q_isupper(c) {
        c | (b'a' as c_int - b'A' as c_int)
    } else {
        c
    }
}

/// `Hash_First` -- the first index in a hash entry's chain, or -1 if empty.
#[inline]
unsafe fn hash_first(hi: *mut HashIndexC, key: c_int) -> c_int {
    *(*hi).hash.add((key & (*hi).hash_mask) as usize)
}

/// `Hash_Next` -- the next index in the chain, or -1 at the end of it.
#[inline]
unsafe fn hash_next(hi: *mut HashIndexC, index: c_int) -> c_int {
    *(*hi).index_chain.add(index as usize)
}

/// `Hash_GenerateKeyString` -- the string hash, case-folded when the caller
/// says so.  Every quakefs.c call passes `false`.
#[inline]
unsafe fn hash_generate_key_string(
    hi: *mut HashIndexC,
    string: *const c_char,
    case_sensitive: c_int,
) -> c_int {
    let mut hash: c_int = 0;
    let mut p = string;
    let mut i: c_int = 0;

    if case_sensitive != 0 {
        while *p != 0 {
            hash += (*p as c_int) * (i + 119);
            p = p.add(1);
            i += 1;
        }
    } else {
        while *p != 0 {
            hash += q_tolower(*p as c_int) * (i + 119);
            p = p.add(1);
            i += 1;
        }
    }

    hash & (*hi).hash_mask
}

/// `Hash_GenerateKeyInt`.  quakefs.c has no call site for this one -- the C
/// callers that do are elsewhere -- but the header's inline is reproduced with
/// the other three so the set stays together.
#[inline]
#[allow(dead_code)]
unsafe fn hash_generate_key_int(hi: *mut HashIndexC, n: c_int) -> c_int {
    n & (*hi).hash_mask
}

//============================================================================
// miniz, which stays C
//============================================================================
// `zippack_t` embeds `mz_zip_archive` *by value*, so this struct is
// reproduced in full rather than standing in as an opaque pointer: C fills and
// reads it through the pointer, and a shorter type would move `numfiles`,
// `files` and `hash` in `ZipPackC`.  Only three of its fields are ever touched
// here -- m_pRead, m_pIO_opaque and m_total_files -- but the ones between them
// are what put those at the right offsets, and
// engine/rust/tests/abi_layout.c checks the lot against the real header on the
// host and again under emcc for ILP32.

/// `mz_file_read_func` from miniz.h.
pub type MzFileReadFunc =
    Option<unsafe extern "C" fn(opaque: *mut c_void, ofs: u64, buf: *mut c_void, n: usize) -> usize>;

/// `MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE` and
/// `MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE` from miniz.h:488.
const MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE: usize = 512;
const MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE: usize = 512;

/// `mz_zip_archive` from miniz.h:618-645.  Field names are miniz's, kept
/// verbatim so a reader can diff this against the header.
#[repr(C)]
#[allow(non_snake_case)]
pub struct MzZipArchive {
    pub m_archive_size: u64,
    pub m_central_directory_file_ofs: u64,
    pub m_total_files: u32,
    pub m_zip_mode: c_int,
    pub m_zip_type: c_int,
    pub m_last_error: c_int,
    pub m_file_offset_alignment: u64,
    pub m_pAlloc: *mut c_void,
    pub m_pFree: *mut c_void,
    pub m_pRealloc: *mut c_void,
    pub m_pAlloc_opaque: *mut c_void,
    pub m_pRead: MzFileReadFunc,
    pub m_pWrite: *mut c_void,
    pub m_pNeeds_keepalive: *mut c_void,
    pub m_pIO_opaque: *mut c_void,
    pub m_pState: *mut c_void,
}

/// `mz_zip_archive_file_stat` from miniz.h:505-543.  Filled wholesale by
/// `mz_zip_reader_file_stat`, so every field has to be here, not only the five
/// the loader reads.
#[repr(C)]
pub struct MzZipArchiveFileStat {
    pub m_file_index: u32,
    pub m_central_dir_ofs: u64,
    pub m_version_made_by: u16,
    pub m_version_needed: u16,
    pub m_bit_flag: u16,
    pub m_method: u16,
    pub m_crc32: u32,
    pub m_comp_size: u64,
    pub m_uncomp_size: u64,
    pub m_internal_attr: u16,
    pub m_external_attr: u32,
    pub m_local_header_ofs: u64,
    pub m_comment_size: u32,
    pub m_is_directory: c_int,
    pub m_is_encrypted: c_int,
    pub m_is_supported: c_int,
    pub m_filename: [c_char; MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE],
    pub m_comment: [c_char; MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE],
}

// The offsets below are the ones the C compiler reported for the host; the
// wasm32 build re-derives them from the widths, exactly as the other ports do,
// and abi_layout.c compares both.
const _: () = {
    assert!(core::mem::offset_of!(MzZipArchive, m_total_files) == 16);
    // 16 (two u64s) + three enums + padding to 8 gives the alignment field at
    // 32, and the ten pointers follow it; the whole struct rounds up to 8.
    assert!(core::mem::offset_of!(MzZipArchive, m_pRead) == 40 + 4 * PTR_SIZE);
    assert!(core::mem::offset_of!(MzZipArchive, m_pIO_opaque) == 40 + 7 * PTR_SIZE);
    assert!(core::mem::size_of::<MzZipArchive>() == align_up(40 + 9 * PTR_SIZE, 8));

    assert!(core::mem::offset_of!(MzZipArchiveFileStat, m_method) == 16 + 6);
    assert!(core::mem::offset_of!(MzZipArchiveFileStat, m_uncomp_size) == 40);
    assert!(core::mem::offset_of!(MzZipArchiveFileStat, m_is_directory) == 64 + 4);
    assert!(core::mem::offset_of!(MzZipArchiveFileStat, m_is_supported) == 64 + 4 + 8);
    assert!(core::mem::offset_of!(MzZipArchiveFileStat, m_filename) == 80);
    assert!(core::mem::size_of::<MzZipArchiveFileStat>()
        == 80 + MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE + MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE);
};

extern "C" {
    fn mz_zip_reader_init(zip: *mut MzZipArchive, size: u64, flags: c_uint) -> c_int;
    fn mz_zip_reader_end(zip: *mut MzZipArchive) -> c_int;
    fn mz_zip_reader_file_stat(
        zip: *mut MzZipArchive,
        index: c_uint,
        stat: *mut MzZipArchiveFileStat,
    ) -> c_int;
    fn mz_zip_reader_extract_to_mem(
        zip: *mut MzZipArchive,
        index: c_uint,
        buf: *mut c_void,
        size: usize,
        flags: c_uint,
    ) -> c_int;
}

//============================================================================
// libc and the engine surface the loader group calls
//============================================================================

/// `FILE` is opaque to the port; it is only ever passed to libc.
pub type LibcFile = c_void;

/// `SEEK_SET` from stdio.h.
const SEEK_SET: c_int = 0;
/// `SEEK_END` from stdio.h.
const SEEK_END: c_int = 2;

#[cfg(windows)]
const MAX_PATH: usize = 260;
#[cfg(windows)]
const CP_UTF8: c_uint = 65001;

#[cfg(windows)]
extern "system" {
    fn MultiByteToWideChar(
        code_page: c_uint,
        flags: c_uint,
        mbstr: *const c_char,
        cbmb: c_int,
        wcstr: *mut u16,
        cchwide: c_int,
    ) -> c_int;
    fn _wfopen(filename: *const u16, mode: *const u16) -> *mut LibcFile;
}

extern "C" {
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut LibcFile;
    fn fclose(f: *mut LibcFile) -> c_int;
    fn fread(ptr: *mut c_void, size: usize, n: usize, f: *mut LibcFile) -> usize;
    fn fseek(f: *mut LibcFile, off: c_long, whence: c_int) -> c_int;
    fn ftell(f: *mut LibcFile) -> c_long;
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;

    /// printsys.h: `Sys_Printf(fmt, ...)` is `CON_Printf(_PRINT_TERMONLY, ...)`.
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    /// common.h, FUNC_NORETURN: the fatal paths never return.
    fn Sys_Error(fmt: *const c_char, ...) -> !;

    fn q_strlcpy(dst: *mut c_char, src: *const c_char, size: usize) -> usize;
    fn q_strcasecmp(a: *const c_char, b: *const c_char) -> c_int;
    /// common.h:61 -- `q_strlcpy` plus a `Sys_Error` naming the caller and its
    /// line when the source does not fit.  The port passes the C file's own
    /// line numbers so the diagnostic text matches byte for byte.
    fn qerr_strlcpy(
        caller: *const c_char,
        linenum: c_int,
        dst: *mut c_char,
        src: *const c_char,
        size: usize,
    ) -> usize;
}

/// `_PRINT_TERMONLY` from engine/h2shared/printsys.h.
const PRINT_TERMONLY: c_uint = 1;

/// `LittleLong` from common/q_endian.h -- the identity on a little-endian
/// host, `LongSwap` on a big-endian one, so it is a property of the bytes read
/// rather than of the field holding them, exactly as wad.rs records.
#[inline]
fn little_long(v: c_int) -> c_int {
    #[cfg(target_endian = "little")]
    {
        v
    }
    #[cfg(target_endian = "big")]
    {
        v.swap_bytes()
    }
}

//============================================================================
// the path builders
//============================================================================

/// `FS_NUM_BUFFS` and `FS_BUFFERLEN` from quakefs.c:3533-3534.
const FS_NUM_BUFFS: usize = 4;
const FS_BUFFERLEN: usize = 1024;

/// `_PRINT_DEVEL` from engine/h2shared/printsys.h -- what the C's
/// `Con_DPrintf` macro passes as CON_Printf's first argument.
const PRINT_DEVEL: c_uint = 2;

/// `get_fs_buffer` -- four rotating 1024-byte buffers, so a caller may hold
/// two paths at once (the C comment on FS_MakePath's callers relies on it).
static mut FS_BUFFERS: [[c_char; FS_BUFFERLEN]; FS_NUM_BUFFS] = [[0; FS_BUFFERLEN]; FS_NUM_BUFFS];
static mut FS_BUFFER_IDX: c_int = 0;

unsafe fn get_fs_buffer() -> *mut c_char {
    FS_BUFFER_IDX = (FS_BUFFER_IDX + 1) & (FS_NUM_BUFFS as c_int - 1);
    // Raw arithmetic rather than indexing: indexing a `static mut` array forms
    // a reference, which the crate avoids everywhere else for the same reason.
    (&raw mut FS_BUFFERS)
        .cast::<c_char>()
        .add(FS_BUFFER_IDX as usize * FS_BUFFERLEN)
}

/// `IS_DIR_SEPARATOR` from common/filenames.h -- '\\' counts only on Windows.
#[inline]
fn is_dir_separator(c: c_char) -> bool {
    #[cfg(windows)]
    {
        c == b'/' as c_char || c == b'\\' as c_char
    }
    #[cfg(not(windows))]
    {
        c == b'/' as c_char
    }
}

/// `DIR_SEPARATOR_CHAR` from common/filenames.h.
#[inline]
const fn dir_separator_char() -> c_char {
    #[cfg(windows)]
    {
        b'\\' as c_char
    }
    #[cfg(not(windows))]
    {
        b'/' as c_char
    }
}

/// `init_MakePath` -- write the base directory into `buf`, append a separator
/// if it does not end in one, and report how many bytes that took.  Returns -1
/// when the base does not fit, which is what turns into `*error = 1`.
unsafe fn init_MakePath(base: c_int, buf: *mut c_char, siz: usize) -> c_int {
    let mut len: c_int = match base {
        MAKEPATH_USERDIR => q_strlcpy(buf, (&raw const FS_USERDIR).cast::<c_char>(), siz) as c_int,
        MAKEPATH_GAMEDIR => q_strlcpy(buf, (&raw const FS_GAMEDIR).cast::<c_char>(), siz) as c_int,
        MAKEPATH_USERBASE => q_strlcpy(
            buf,
            (*host_parms).userdir,
            siz,
        ) as c_int,
        MAKEPATH_BASEDIR => q_strlcpy(buf, fs_basedir_ptr(), siz) as c_int,
        _ => {
            Sys_Error(c"%s: Bad FS_BASE".as_ptr(), c"init_MakePath".as_ptr());
        }
    };

    if len >= siz as c_int - 1 {
        return -1;
    }
    if len != 0 && !is_dir_separator(*buf.add(len as usize)) {
        *buf.add(len as usize) = dir_separator_char();
        len += 1;
    }
    *buf.add(len as usize) = 0;
    len
}

/// `do_MakePath` -- the base, then the path, then the truncation flag.
unsafe fn do_MakePath(
    base: c_int,
    error: *mut c_int,
    buf: *mut c_char,
    siz: usize,
    path: *const c_char,
) -> *mut c_char {
    let mut len = init_MakePath(base, buf, siz);

    if len < 0 {
        if !error.is_null() {
            *error = 1;
        }
        CON_Printf(
            PRINT_DEVEL,
            c"%s: overflow (string truncated)\n".as_ptr(),
            c"do_MakePath".as_ptr(),
        );
        return buf;
    }

    len = q_strlcat(buf, path, siz) as c_int;
    if len < siz as c_int {
        if !error.is_null() {
            *error = 0;
        }
    } else {
        if !error.is_null() {
            *error = 1;
        }
        CON_Printf(
            PRINT_DEVEL,
            c"%s: overflow (string truncated)\n".as_ptr(),
            c"do_MakePath".as_ptr(),
        );
    }

    buf
}

/// `FS_MakePath` -- into the next of the four rotating buffers.
#[no_mangle]
pub unsafe extern "C" fn FS_MakePath(
    base: c_int,
    error: *mut c_int,
    path: *const c_char,
) -> *mut c_char {
    do_MakePath(base, error, get_fs_buffer(), FS_BUFFERLEN, path)
}

/// `FS_MakePath_BUF` -- into the caller's buffer.
#[no_mangle]
pub unsafe extern "C" fn FS_MakePath_BUF(
    base: c_int,
    error: *mut c_int,
    buf: *mut c_char,
    siz: usize,
    path: *const c_char,
) -> *mut c_char {
    do_MakePath(base, error, buf, siz, path)
}

/// `FSERR_MakePath_BUF` -- `do_MakePath` but fatal on overflow, with the C's
/// caller/line in the message.
unsafe fn fserr_make_path_buf(
    caller: *const c_char,
    linenum: c_int,
    base: c_int,
    buf: *mut c_char,
    siz: usize,
    path: *const c_char,
) -> *mut c_char {
    let mut err: c_int = 0;
    let p = do_MakePath(base, &mut err, buf, siz, path);

    if err != 0 {
        Sys_Error(
            c"%s: %d: string buffer overflow!".as_ptr(),
            caller,
            linenum,
        );
    }
    p
}

extern "C" {
    /// engine/hexen2/host.h -- set by Sys_Init before anything builds a path.
    static host_parms: *mut crate::zone::QuakeParmsC;
    fn q_strlcat(dst: *mut c_char, src: *const c_char, size: usize) -> usize;
}

/// The path builders read the C's own `static const char *fs_basedir` through
/// this, so there is one place that knows how it is spelled.
#[inline]
unsafe fn fs_basedir_ptr() -> *const c_char {
    FS_BASEDIR
}

/*----------------------------------------------------------------------------
 * the three helpers engine/rust/quakefs_variadic.c calls
 *
 * That file is C because a C-variadic signature cannot be *defined* in stable
 * Rust; it is not a second implementation of anything, and these are the only
 * names it needs -- the buffer it formats into, its length, and the base half
 * of do_MakePath.  The logic stays here.
 *--------------------------------------------------------------------------*/

/// The next of the four rotating buffers, for `FS_MakePath_VA`'s caller.
#[no_mangle]
pub unsafe extern "C" fn QuakeFS_GetBuffer() -> *mut c_char {
    get_fs_buffer()
}

/// `FS_BUFFERLEN` -- what that buffer can hold.
#[no_mangle]
pub extern "C" fn QuakeFS_BufferLen() -> usize {
    FS_BUFFERLEN
}

/// `init_MakePath` -- the base directory and its separator, which the shim
/// needs before it can format the rest of the path onto the end.
#[no_mangle]
pub unsafe extern "C" fn QuakeFS_MakePathPrefix(
    base: c_int,
    buf: *mut c_char,
    siz: usize,
) -> c_int {
    init_MakePath(base, buf, siz)
}

#[cfg(not(windows))]
extern "C" {
    /// engine/rust/quakefs_variadic.c, which owns it because reading
    /// `struct dirent` is an ABI concern rather than a porting one.
    pub fn FS_ResolveCasePath(
        basedir: *const c_char,
        relpath: *const c_char,
        resolved: *mut c_char,
    ) -> c_int;
}

//============================================================================
// the file syscall layer
//============================================================================
// `DO_USERDIRS` from engine/h2shared/h2config.h:57, disabled by sys.h:91 for
// Windows, OS/2 and Emscripten -- the web client gets one persistence root, so
// a separate userdir would put config.cfg and the savegames nowhere.  The
// cfg below is that same platform test, not merely a Windows one.
#[inline]
const fn do_userdirs() -> bool {
    !(cfg!(windows) || cfg!(target_family = "wasm"))
}

extern "C" {
    /// engine/h2shared/sys.h:37 -- the size of a file, or -1.
    fn Sys_filesize(path: *const c_char) -> c_long;
    /// engine/h2shared/sys.h:33 -- `FS_ENT_FILE`/`FS_ENT_DIRECTORY` bits.
    fn Sys_FileType(path: *const c_char) -> c_int;
    fn q_snprintf(str: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;

    /// engine/hexen2/host.h:55 -- the `developer` cvar, read as an address so
    /// nothing here assumes Rust exclusivity over a C-owned global.
    static mut developer: CvarC;
}

/// `FS_OpenFile_Internal` -- the one search loop every open, existence check
/// and handle open goes through.  `paks_only` skips the directory entries, so
/// a loose file that permanently hides the pak copy can be stepped over.
unsafe fn fs_open_file_internal(
    filename: *const c_char,
    file: *mut *mut LibcFile,
    path_id: *mut c_uint,
    silent: c_int,
    paks_only: c_int,
) -> c_long {
    let mut ospath = [0 as c_char; MAX_OSPATH];

    file_from_pak = 0;
    FS_LASTZIP = core::ptr::null_mut();
    FS_LASTZIPENTRY = core::ptr::null();

    // search through the path, one element at a time
    let mut search = FS_SEARCHPATHS;
    while !search.is_null() {
        if !(*search).pack.is_null() {
            // look through all the pak file elements
            let pak = (*search).pack;
            let key = hash_generate_key_string(&mut (*pak).hash, filename, 0);
            let mut i = hash_first(&mut (*pak).hash, key);
            while i != -1 {
                let entry = (*pak).files.add(i as usize);
                if q_strcasecmp((*entry).name.as_ptr(), filename) != 0 {
                    i = hash_next(&mut (*pak).hash, i);
                    continue;
                }
                // found it!
                fs_filesize = (*entry).filelen as c_long;
                file_from_pak = 1;
                q_strlcpy(
                    (&raw mut FS_LASTFILE_SOURCE).cast::<c_char>(),
                    (*pak).filename.as_ptr(),
                    MAX_OSPATH,
                );
                if !path_id.is_null() {
                    *path_id = (*search).path_id;
                }
                if file.is_null() {
                    // for FS_FileExists()
                    return fs_filesize;
                }
                // open a new file on the pakfile
                *file = fs_fopen((*pak).filename.as_ptr(), c"rb".as_ptr());
                if (*file).is_null() {
                    Sys_Error(c"Couldn't reopen %s".as_ptr(), (*pak).filename.as_ptr());
                }
                fseek(*file, (*entry).filepos as c_long, SEEK_SET);
                return fs_filesize;
            }
        } else if !(*search).zip.is_null() {
            // look through a mounted .pk3
            let zip = (*search).zip;
            let key = hash_generate_key_string(&mut (*zip).hash, filename, 0);
            let mut i = hash_first(&mut (*zip).hash, key);
            while i != -1 {
                let entry = (*zip).files.add(i as usize);
                if q_strcasecmp((*entry).name.as_ptr(), filename) != 0 {
                    i = hash_next(&mut (*zip).hash, i);
                    continue;
                }
                // found it!
                fs_filesize = (*entry).filelen as c_long;
                // An archive member is "from a pak" for every purpose that asks.
                file_from_pak = 1;
                q_strlcpy(
                    (&raw mut FS_LASTFILE_SOURCE).cast::<c_char>(),
                    (*zip).filename.as_ptr(),
                    MAX_OSPATH,
                );
                if !path_id.is_null() {
                    *path_id = (*search).path_id;
                }
                if file.is_null() {
                    // for FS_FileExists()
                    return fs_filesize;
                }

                if (*entry).filepos == -1 {
                    // STORED, offset not resolved yet
                    let mut st: MzZipArchiveFileStat = core::mem::zeroed();
                    let mut ofs: c_int = -2;

                    if mz_zip_reader_file_stat(&mut (*zip).archive, (*entry).index, &mut st) != 0 {
                        ofs = fs_zip_data_offset(zip, st.m_local_header_ofs);
                    }
                    // A local header we cannot parse demotes the entry to the
                    // inflate path rather than failing the lookup.
                    (*entry).filepos = if ofs < 0 { -2 } else { ofs };
                }

                if (*entry).filepos >= 0 {
                    // STORED: contiguous plain bytes, served like a pak member
                    *file = fs_fopen((*zip).filename.as_ptr(), c"rb".as_ptr());
                    if (*file).is_null() {
                        Sys_Error(c"Couldn't reopen %s".as_ptr(), (*zip).filename.as_ptr());
                    }
                    fseek(*file, (*entry).filepos as c_long, SEEK_SET);
                    return fs_filesize;
                }

                // DEFLATED: no FILE * can represent this.
                *file = core::ptr::null_mut();
                FS_LASTZIP = zip;
                FS_LASTZIPENTRY = entry;
                return fs_filesize;
            }
        } else if paks_only == 0 {
            // check a file in the directory tree
            q_snprintf(
                ospath.as_mut_ptr(),
                MAX_OSPATH,
                c"%s/%s".as_ptr(),
                (*search).filename.as_ptr(),
                filename,
            );
            fs_filesize = Sys_filesize(ospath.as_ptr());
            #[cfg(not(windows))]
            {
                if fs_filesize < 0 {
                    // case-insensitive fallback for loose files
                    if FS_ResolveCasePath(
                        (*search).filename.as_ptr(),
                        filename,
                        ospath.as_mut_ptr(),
                    ) != 0
                    {
                        fs_filesize = Sys_filesize(ospath.as_ptr());
                    }
                }
            }
            if fs_filesize < 0 {
                search = (*search).next;
                continue;
            }
            q_strlcpy(
                (&raw mut FS_LASTFILE_SOURCE).cast::<c_char>(),
                ospath.as_ptr(),
                MAX_OSPATH,
            );
            if !path_id.is_null() {
                *path_id = (*search).path_id;
            }
            if file.is_null() {
                // for FS_FileExists()
                return fs_filesize;
            }
            *file = fs_fopen(ospath.as_ptr(), c"rb".as_ptr());
            if (*file).is_null() {
                Sys_Error(c"Couldn't reopen %s".as_ptr(), ospath.as_ptr());
            }
            return fs_filesize;
        }

        search = (*search).next;
    }

    // Only print "can't find" messages when developer >= 1 and not in silent
    // mode (suppresses noise from optional external textures and missing
    // assets).
    if silent == 0 && (*(&raw const developer)).integer >= 1 {
        CON_Printf(
            PRINT_TERMONLY,
            c"%s: can't find %s\n".as_ptr(),
            c"FS_OpenFile_Internal".as_ptr(),
            filename,
        );
    }

    if !file.is_null() {
        *file = core::ptr::null_mut();
    }
    fs_filesize = -1;
    FS_LASTFILE_SOURCE[0] = 0;
    fs_filesize
}

/// `FS_LastFileSource` -- the OS path of the entry that satisfied the most
/// recent lookup, empty after a failed one.
#[no_mangle]
pub unsafe extern "C" fn FS_LastFileSource() -> *const c_char {
    (&raw const FS_LASTFILE_SOURCE).cast::<c_char>()
}

/// `FS_OpenFile`
#[no_mangle]
pub unsafe extern "C" fn FS_OpenFile(
    filename: *const c_char,
    file: *mut *mut LibcFile,
    path_id: *mut c_uint,
) -> c_long {
    fs_open_file_internal(filename, file, path_id, 0, 0)
}

/// `FS_OpenFileInPak` -- pak members only, and silent because both callers
/// report the miss themselves.
unsafe fn fs_open_file_in_pak(
    filename: *const c_char,
    file: *mut *mut LibcFile,
    path_id: *mut c_uint,
) -> c_long {
    fs_open_file_internal(filename, file, path_id, 1, 1)
}

/// `FS_OpenFile_Silent`
#[no_mangle]
pub unsafe extern "C" fn FS_OpenFile_Silent(
    filename: *const c_char,
    file: *mut *mut LibcFile,
    path_id: *mut c_uint,
) -> c_long {
    fs_open_file_internal(filename, file, path_id, 1, 0)
}

/// `FS_OpenFileHandle_Internal` -- `FS_OpenFile` for callers that read
/// incrementally, and the only way to read a DEFLATED archive entry, which no
/// `FILE *` can represent.
unsafe fn fs_open_file_handle_internal(
    filename: *const c_char,
    fh: *mut FSHandleC,
    path_id: *mut c_uint,
    silent: c_int,
) -> c_long {
    if fh.is_null() {
        return -1;
    }

    memset(fh.cast::<c_void>(), 0, core::mem::size_of::<FSHandleC>());

    let mut f: *mut LibcFile = core::ptr::null_mut();
    let len = if silent != 0 {
        fs_open_file_internal(filename, &mut f, path_id, 1, 0)
    } else {
        fs_open_file_internal(filename, &mut f, path_id, 0, 0)
    };
    if len < 0 {
        return -1;
    }

    if !f.is_null() {
        (*fh).file = f;
        (*fh).pak = if file_from_pak != 0 { 1 } else { 0 };
        (*fh).start = ftell(f);
        (*fh).length = len;
        (*fh).pos = 0;
        return len;
    }

    // deflated archive entry
    (*fh).data = malloc(len as usize + 1).cast::<u8>();
    if (*fh).data.is_null() {
        CON_Printf(
            PRINT_TERMONLY,
            c"%s: out of memory for %s\n".as_ptr(),
            c"FS_OpenFileHandle_Internal".as_ptr(),
            filename,
        );
        return -1;
    }
    *(*fh).data.add(len as usize) = 0;

    if fs_zip_read_entry(FS_LASTZIP, FS_LASTZIPENTRY, (*fh).data.cast::<c_void>()) == 0 {
        CON_Printf(
            PRINT_TERMONLY,
            c"%s: corrupt deflate stream for %s in %s\n".as_ptr(),
            c"FS_OpenFileHandle_Internal".as_ptr(),
            filename,
            (*FS_LASTZIP).filename.as_ptr(),
        );
        free((*fh).data.cast::<c_void>());
        (*fh).data = core::ptr::null_mut();
        return -1;
    }

    (*fh).pak = 1; // it came out of packaged content
    (*fh).start = 0;
    (*fh).length = len;
    (*fh).pos = 0;
    len
}

/// `FS_OpenFileHandle`
#[no_mangle]
pub unsafe extern "C" fn FS_OpenFileHandle(
    filename: *const c_char,
    fh: *mut FSHandleC,
    path_id: *mut c_uint,
) -> c_long {
    fs_open_file_handle_internal(filename, fh, path_id, 0)
}

/// `FS_OpenFileHandle_Silent` -- for optional content whose absence is normal.
#[no_mangle]
pub unsafe extern "C" fn FS_OpenFileHandle_Silent(
    filename: *const c_char,
    fh: *mut FSHandleC,
    path_id: *mut c_uint,
) -> c_long {
    fs_open_file_handle_internal(filename, fh, path_id, 1)
}

/// `FS_FileExists`
#[no_mangle]
pub unsafe extern "C" fn FS_FileExists(
    filename: *const c_char,
    path_id: *mut c_uint,
) -> c_int {
    let ret = fs_open_file_internal(filename, core::ptr::null_mut(), path_id, 1, 0);
    if ret < 0 {
        0
    } else {
        1
    }
}

/// `FS_FileExistsInPak` -- is there a copy inside some pak, whether or not a
/// loose file is hiding it?  No syscalls, so ask it first.
#[no_mangle]
pub unsafe extern "C" fn FS_FileExistsInPak(
    filename: *const c_char,
    path_id: *mut c_uint,
) -> c_int {
    let ret = fs_open_file_in_pak(filename, core::ptr::null_mut(), path_id);
    if ret < 0 {
        0
    } else {
        1
    }
}

/// `FS_FileInGamedir` -- a readable loose file in fs_gamedir or fs_userdir,
/// never a pak member.
#[no_mangle]
pub unsafe extern "C" fn FS_FileInGamedir(filename: *const c_char) -> c_int {
    let mut ret = Sys_FileType(FS_MakePath(MAKEPATH_USERDIR, core::ptr::null_mut(), filename));
    if ret & FS_ENT_FILE != 0 {
        return 1;
    }

    ret = Sys_FileType(FS_MakePath(MAKEPATH_GAMEDIR, core::ptr::null_mut(), filename));
    if ret & FS_ENT_FILE != 0 {
        return 1;
    }

    0
}

/// `FS_UserdirHasFile` -- did the player put their own loose copy in the
/// userdir's `gamedir`?  False where there is no user directory, and false
/// where the userdir and the install directory are the same place.
#[no_mangle]
pub unsafe extern "C" fn FS_UserdirHasFile(
    gamedir: *const c_char,
    filename: *const c_char,
) -> c_int {
    if !do_userdirs() {
        return 0;
    }

    let mut userpath = [0 as c_char; MAX_OSPATH];
    let mut basepath = [0 as c_char; MAX_OSPATH];
    let mut ospath = [0 as c_char; MAX_OSPATH];

    FS_MakePath_BUF(
        MAKEPATH_USERBASE,
        core::ptr::null_mut(),
        userpath.as_mut_ptr(),
        MAX_OSPATH,
        gamedir,
    );
    FS_MakePath_BUF(
        MAKEPATH_BASEDIR,
        core::ptr::null_mut(),
        basepath.as_mut_ptr(),
        MAX_OSPATH,
        gamedir,
    );
    if strcmp(userpath.as_ptr(), basepath.as_ptr()) == 0 {
        return 0;
    }

    q_snprintf(
        ospath.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/%s".as_ptr(),
        userpath.as_ptr(),
        filename,
    );
    if Sys_FileType(ospath.as_ptr()) & FS_ENT_FILE != 0 {
        return 1;
    }
    #[cfg(not(windows))]
    {
        if FS_ResolveCasePath(userpath.as_ptr(), filename, ospath.as_mut_ptr()) != 0 {
            return if Sys_FileType(ospath.as_ptr()) & FS_ENT_FILE != 0 {
                1
            } else {
                0
            };
        }
    }

    0
}

//============================================================================
// the searchpath and gamedir machinery
//============================================================================

/// `fsfind_t` from engine/h2shared/sys.h:49-52 -- the platform's directory
/// enumeration state plus the current name.  Passed by address to Sys_Find*,
/// which is why its layout matters.
#[repr(C)]
pub struct FSFindC {
    pub priv_: *mut c_void,
    pub name: [c_char; MAX_OSPATH],
}

const _: () = {
    assert!(core::mem::offset_of!(FSFindC, priv_) == 0);
    assert!(core::mem::offset_of!(FSFindC, name) == PTR_SIZE);
    assert!(core::mem::size_of::<FSFindC>() == align_up(PTR_SIZE + MAX_OSPATH, PTR_ALIGN));
};

extern "C" {
    fn Sys_FindFirstFile(
        ctx: *mut FSFindC,
        path: *const c_char,
        pattern: *const c_char,
    ) -> *const c_char;
    fn Sys_FindNextFile(ctx: *mut FSFindC) -> *const c_char;
    fn Sys_FindClose(ctx: *mut FSFindC);
    fn Sys_mkdir(path: *const c_char, crash: c_int) -> c_int;
    fn qsort(
        base: *mut c_void,
        nmemb: usize,
        size: usize,
        compar: unsafe extern "C" fn(*const c_void, *const c_void) -> c_int,
    );
}

/// The `FSERR_MakePath_VABUF (…, "…%s…", …)` sites, without varargs: the tail
/// is formatted first and handed to the non-variadic builder the module owns.
/// The C's caller name and line number are passed through so an overflow reads
/// identically.
unsafe fn fserr_make_path_vabuf_pak(
    caller: *const c_char,
    linenum: c_int,
    base: c_int,
    buf: *mut c_char,
    siz: usize,
    index: c_int,
) -> *mut c_char {
    let mut tail = [0 as c_char; MAX_OSPATH];

    q_snprintf(tail.as_mut_ptr(), MAX_OSPATH, c"pak%i.pak".as_ptr(), index);
    fserr_make_path_buf(caller, linenum, base, buf, siz, tail.as_ptr())
}

unsafe fn fserr_make_path_vabuf_name(
    caller: *const c_char,
    linenum: c_int,
    base: c_int,
    buf: *mut c_char,
    siz: usize,
    name: *const c_char,
) -> *mut c_char {
    let mut tail = [0 as c_char; MAX_OSPATH];

    q_snprintf(tail.as_mut_ptr(), MAX_OSPATH, c"%s".as_ptr(), name);
    fserr_make_path_buf(caller, linenum, base, buf, siz, tail.as_ptr())
}

/// `FS_AddGameDirectory` -- set fs_gamedir/fs_userdir/fs_gamedir_nopath, add
/// the directory, then load every pak and every pk3 in it.  The C reaches the
/// userdir half with a `goto add_pakfile`; the loop here is the same control
/// flow (gamedir first, then the userdir when it is a different place).
unsafe fn fs_add_game_directory(dir: *const c_char, base_fs: c_int) {
    let mut pakfile = [0 as c_char; MAX_OSPATH];
    let path_id: c_uint;
    let mut do_userdir = 0;

    qerr_strlcpy(
        c"FS_AddGameDirectory".as_ptr(),
        947,
        (&raw mut fs_gamedir_nopath).cast::<c_char>(),
        dir,
        MAX_QPATH,
    );
    fserr_make_path_buf(
        c"FS_AddGameDirectory".as_ptr(),
        949,
        MAKEPATH_BASEDIR,
        (&raw mut FS_GAMEDIR).cast::<c_char>(),
        MAX_OSPATH,
        dir,
    );
    fserr_make_path_buf(
        c"FS_AddGameDirectory".as_ptr(),
        951,
        MAKEPATH_USERBASE,
        (&raw mut FS_USERDIR).cast::<c_char>(),
        MAX_OSPATH,
        dir,
    );

    // assign a path_id to this game directory
    if !FS_SEARCHPATHS.is_null() {
        path_id = (*FS_SEARCHPATHS).path_id << 1;
    } else {
        path_id = 1;
    }

    // Recorded where it is assigned rather than recomputed by walking the
    // searchpath later.
    if q_strcasecmp(dir, c"portals".as_ptr()) == 0 {
        FS_PORTALS_PATH_ID = path_id;
    }

    loop {
        // add any pak files in the format pak0.pak pak1.pak, ...; Hexen II
        // cannot stop at the first unavailable pak, since the mission pack has
        // only pak3 and hw only pak4.
        let mut i: c_int = 0;
        while i < 10 {
            let base = if do_userdir != 0 {
                MAKEPATH_USERDIR
            } else {
                MAKEPATH_GAMEDIR
            };
            fserr_make_path_vabuf_pak(
                c"FS_AddGameDirectory".as_ptr(),
                974,
                base,
                pakfile.as_mut_ptr(),
                MAX_OSPATH,
                i,
            );
            let pak = fs_load_pack_file(pakfile.as_ptr(), i, base_fs);
            if !pak.is_null() {
                let search = Z_Malloc(
                    core::mem::size_of::<SearchPathC>() as c_int,
                    Z_MAINZONE,
                )
                .cast::<SearchPathC>();
                (*search).path_id = path_id;
                (*search).pack = pak;
                (*search).next = FS_SEARCHPATHS;
                FS_SEARCHPATHS = search;
            }
            i += 1;
        }

        // add any .pk3 archives in this directory, alphabetically.  Pushed
        // after the numbered paks and before the loose directory, so the
        // search order is loose files, then pk3s, then paks; within the pk3s
        // the alphabetically last wins, because each is pushed onto the head.
        {
            let mut find: FSFindC = core::mem::zeroed();
            let mut zipnames = [[0 as c_char; MAX_QPATH]; MAX_PK3_PER_DIR];
            let mut numzips: usize = 0;
            let scandir = if do_userdir != 0 {
                (&raw const FS_USERDIR).cast::<c_char>()
            } else {
                (&raw const FS_GAMEDIR).cast::<c_char>()
            };

            let mut findname = Sys_FindFirstFile(&mut find, scandir, c"*.pk3".as_ptr());
            while !findname.is_null() {
                if numzips == MAX_PK3_PER_DIR {
                    CON_Printf(
                        PRINT_TERMONLY,
                        c"WARNING: more than %i pk3 files in %s, ignoring the rest\n"
                            .as_ptr(),
                        MAX_PK3_PER_DIR as c_int,
                        scandir,
                    );
                    break;
                }
                q_strlcpy(
                    zipnames[numzips].as_mut_ptr(),
                    findname,
                    MAX_QPATH,
                );
                numzips += 1;
                findname = Sys_FindNextFile(&mut find);
            }
            Sys_FindClose(&mut find);

            // Sys_FindFirstFile makes no ordering promise -- readdir order is
            // filesystem order -- so sort rather than assume.
            if numzips > 1 {
                qsort(
                    zipnames.as_mut_ptr().cast::<c_void>(),
                    numzips,
                    MAX_QPATH,
                    fs_compare_zip_names,
                );
            }

            let mut j: usize = 0;
            while j < numzips {
                let base = if do_userdir != 0 {
                    MAKEPATH_USERDIR
                } else {
                    MAKEPATH_GAMEDIR
                };
                fserr_make_path_vabuf_name(
                    c"FS_AddGameDirectory".as_ptr(),
                    1028,
                    base,
                    pakfile.as_mut_ptr(),
                    MAX_OSPATH,
                    zipnames[j].as_ptr(),
                );
                let zip = fs_load_zip_file(pakfile.as_ptr(), base_fs);
                if !zip.is_null() {
                    let search = Z_Malloc(
                        core::mem::size_of::<SearchPathC>() as c_int,
                        Z_MAINZONE,
                    )
                    .cast::<SearchPathC>();
                    (*search).path_id = path_id;
                    (*search).zip = zip;
                    (*search).next = FS_SEARCHPATHS;
                    FS_SEARCHPATHS = search;
                }
                j += 1;
            }
        }

        // add the directory itself, ~after~ the paks in it, so a loose file
        // overrides the packaged copy.
        let search =
            Z_Malloc(core::mem::size_of::<SearchPathC>() as c_int, Z_MAINZONE).cast::<SearchPathC>();
        if do_userdir != 0 {
            qerr_strlcpy(
                c"FS_AddGameDirectory".as_ptr(),
                1048,
                (*search).filename.as_mut_ptr(),
                (&raw const FS_USERDIR).cast::<c_char>(),
                MAX_OSPATH,
            );
        } else {
            qerr_strlcpy(
                c"FS_AddGameDirectory".as_ptr(),
                1049,
                (*search).filename.as_mut_ptr(),
                (&raw const FS_GAMEDIR).cast::<c_char>(),
                MAX_OSPATH,
            );
        }
        (*search).path_id = path_id;
        (*search).next = FS_SEARCHPATHS;
        FS_SEARCHPATHS = search;

        if do_userdir != 0 {
            return;
        }
        do_userdir = 1;

        // add the user's directory to the search path and its paks too, but
        // only where that is a different place from the install directory.
        if !do_userdirs() || strcmp((&raw const FS_GAMEDIR).cast::<c_char>(), (&raw const FS_USERDIR).cast::<c_char>()) == 0 {
            return;
        }
        Sys_mkdir((&raw const FS_USERDIR).cast::<c_char>(), 1);
    }
}

extern "C" {
    /// quakefs_target.c -- the per-target predicates and hooks this module uses
    /// instead of naming a symbol some binary does not have.
    fn QuakeFS_TargetIsH2W() -> c_int;
    fn QuakeFS_TargetIsH2WIntegrated() -> c_int;
    fn QuakeFS_TargetSetHwServerinfo(dir: *const c_char);

    /// engine/hexen2/host.h -- `qboolean host_initialized`.
    static host_initialized: c_int;
    /// The zone port's Cache_Flush.  Not hooked, deliberately: it is a Rust
    /// symbol in every binary now, and flushing an empty cache is a no-op, so
    /// the dedicated targets' compiled-out arms stay inert.
    fn Cache_Flush();
    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
}

/// `_PRINT_NORMAL` -- what the C's `Con_Printf (fmt, ...)` macro passes.
const PRINT_NORMAL: c_uint = 0;

/// `set_hw_dir` (`quakefs.c:1142-1153`, H2W only) -- the H2W server's way of
/// pointing the gamedir variables at "hw".  The `#ifdef SERVERONLY`
/// serverinfo write inside it is the hwsv-only hook.
unsafe fn set_hw_dir() {
    qerr_strlcpy(
        c"set_hw_dir".as_ptr(),
        1143,
        (&raw mut fs_gamedir_nopath).cast::<c_char>(),
        c"hw".as_ptr(),
        MAX_QPATH,
    );
    fserr_make_path_buf(
        c"set_hw_dir".as_ptr(),
        1145,
        MAKEPATH_BASEDIR,
        (&raw mut FS_GAMEDIR).cast::<c_char>(),
        MAX_OSPATH,
        c"hw".as_ptr(),
    );
    fserr_make_path_buf(
        c"set_hw_dir".as_ptr(),
        1147,
        MAKEPATH_USERBASE,
        (&raw mut FS_USERDIR).cast::<c_char>(),
        MAX_OSPATH,
        c"hw".as_ptr(),
    );
    QuakeFS_TargetSetHwServerinfo(c"hw".as_ptr());
}

/// `FS_HWGamedir` (H2W_INTEGRATED only in the C).  A wire string must be one
/// safe directory component: not empty, not a path, not traversal, not a
/// reserved local base directory, and only [A-Za-z0-9_.-].
#[no_mangle]
pub unsafe extern "C" fn FS_HWGamedir(dir: *const c_char) -> c_int {
    let mut p = dir.cast::<u8>();

    if *p == 0
        || strlen(dir) >= MAX_QPATH
        || strcmp(dir, c".".as_ptr()) == 0
        || !strstr(dir, c"..".as_ptr()).is_null()
        || q_strcasecmp(dir, c"data1".as_ptr()) == 0
        || q_strcasecmp(dir, c"portals".as_ptr()) == 0
    {
        return 0;
    }
    while *p != 0 {
        let c = *p;
        let ok = (c >= b'a' && c <= b'z')
            || (c >= b'A' && c <= b'Z')
            || (c >= b'0' && c <= b'9')
            || c == b'_'
            || c == b'-'
            || c == b'.';
        if !ok {
            return 0;
        }
        p = p.add(1);
    }

    if FS_HW_SAVED_PATHS.is_null() {
        // Keep the local game's paths alive, but out of the server's search
        // order; only temporary HW/mod entries above the base are freed.
        FS_HW_SAVED_PATHS = FS_SEARCHPATHS;
        q_strlcpy(
            (&raw mut FS_HW_SAVED_GAME).cast::<c_char>(),
            (&raw const fs_gamedir_nopath).cast::<c_char>(),
            MAX_QPATH,
        );
        q_strlcpy(
            (&raw mut FS_HW_SAVED_GAMEDIR).cast::<c_char>(),
            (&raw const FS_GAMEDIR).cast::<c_char>(),
            MAX_OSPATH,
        );
        q_strlcpy(
            (&raw mut FS_HW_SAVED_USERDIR).cast::<c_char>(),
            (&raw const FS_USERDIR).cast::<c_char>(),
            MAX_OSPATH,
        );
        FS_HW_SAVED_FLAGS = gameflags;
        FS_SEARCHPATHS = FS_BASE_SEARCHPATHS;
    } else {
        fs_unwind_searchpaths(FS_BASE_SEARCHPATHS, 0);
    }
    Cache_Flush();
    fs_add_game_directory(c"hw".as_ptr(), 0);
    if q_strcasecmp(dir, c"hw".as_ptr()) != 0 {
        fs_add_game_directory(dir, 0);
    }
    1
}

/// `FS_HWRestore` -- put the local game's searchpath back, as it was before
/// the H2W server took the gamedir over.
#[no_mangle]
pub unsafe extern "C" fn FS_HWRestore() {
    if FS_HW_SAVED_PATHS.is_null() {
        return;
    }
    Cache_Flush();
    fs_unwind_searchpaths(FS_BASE_SEARCHPATHS, 0);
    FS_SEARCHPATHS = FS_HW_SAVED_PATHS;
    FS_HW_SAVED_PATHS = core::ptr::null_mut();
    gameflags = FS_HW_SAVED_FLAGS;
    q_strlcpy(
        (&raw mut fs_gamedir_nopath).cast::<c_char>(),
        (&raw const FS_HW_SAVED_GAME).cast::<c_char>(),
        MAX_QPATH,
    );
    q_strlcpy(
        (&raw mut FS_GAMEDIR).cast::<c_char>(),
        (&raw const FS_HW_SAVED_GAMEDIR).cast::<c_char>(),
        MAX_OSPATH,
    );
    q_strlcpy(
        (&raw mut FS_USERDIR).cast::<c_char>(),
        (&raw const FS_HW_SAVED_USERDIR).cast::<c_char>(),
        MAX_OSPATH,
    );
}

/// `FS_Gamedir` -- set the gamedir and path to a different directory.  Hexen II
/// uses it for `-game`; HexenWorld calls it on every map change from
/// CL_ParseServerData and from SV_Gamedir_f, which is why the three reserved
/// names below exist at all.
#[no_mangle]
pub unsafe extern "C" fn FS_Gamedir(dir: *const c_char) {
    if *dir == 0
        || strcmp(dir, c".".as_ptr()) == 0
        || !strstr(dir, c"..".as_ptr()).is_null()
        || !strstr(dir, c"/".as_ptr()).is_null()
        || !strstr(dir, c"\\".as_ptr()).is_null()
        || !strstr(dir, c":".as_ptr()).is_null()
    {
        if *(&raw const host_initialized) == 0 {
            Sys_Error(c"gamedir should be a single directory name, not a path\n".as_ptr());
        } else {
            CON_Printf(
                PRINT_NORMAL,
                c"gamedir should be a single directory name, not a path\n".as_ptr(),
            );
            return;
        }
    }

    if q_strcasecmp((&raw const fs_gamedir_nopath).cast::<c_char>(), dir) == 0 {
        return; // still the same
    }

    // free up any current game dir info: the top searchpath dir will be hw and
    // any gamedirs set by this very procedure are removed.
    fs_unwind_searchpaths(FS_BASE_SEARCHPATHS, 0);

    // flush all data, so it will be forced to reload
    Cache_Flush();

    // check for reserved gamedirs
    if q_strcasecmp(dir, c"hw".as_ptr()) == 0 {
        if QuakeFS_TargetIsH2W() != 0 {
            // the hw server went back to pure hw: adjust our variables
            set_hw_dir();
        } else if QuakeFS_TargetIsH2WIntegrated() != 0 {
            // The HexenWorld client lives inside a plain Hexenwail launch whose
            // base searchpaths were built for data1; the server-directed
            // gamedir has to be mounted now or every model, sound and map it
            // references -- hw/pak4.pak especially -- is unresolvable.
            fs_add_game_directory(dir, 0);
        } else {
            // hw is reserved for hexenworld only; hexen2 should not use it
            CON_Printf(
                PRINT_NORMAL,
                c"WARNING: Gamedir not set to hw :\nIt is reserved for HexenWorld.\n".as_ptr(),
            );
        }
        return;
    }

    if q_strcasecmp(dir, c"portals".as_ptr()) == 0 {
        // hw must stay above portals in the hierarchy; hypothetical for h2
        if QuakeFS_TargetIsH2W() != 0 {
            set_hw_dir();
        }
        return;
    }

    if q_strcasecmp(dir, c"data1".as_ptr()) == 0 {
        // hypothetical: no hw mod is supposed to do this
        if QuakeFS_TargetIsH2W() != 0 {
            set_hw_dir();
        }
        return;
    }

    // a new gamedir: let's set it here
    fs_add_game_directory(dir, 0);
    // change the *gamedir serverinfo properly (hwsv only; a no-op elsewhere)
    QuakeFS_TargetSetHwServerinfo(dir);
}

//============================================================================
// writing: save files, screenshots, config
//============================================================================
extern "C" {
    /// engine/h2shared/sys.h:39.
    fn Sys_CopyFile(frompath: *const c_char, topath: *const c_char) -> c_int;
    /// The fatal diagnostic, whose C name differs per build: H2W has no
    /// Host_Error symbol at all (hexenworld/server/host.h:62 makes it a macro
    /// for SV_Error).  quakefs_variadic.c picks the name, so this module never
    /// references either one directly.
    fn QuakeFS_TargetHostError(fmt: *const c_char, ...) -> !;
    fn fwrite(ptr: *const c_void, size: usize, n: usize, f: *mut LibcFile) -> usize;
}

/// `COPY_READ_BUFSIZE` from quakefs.c:1297.
const COPY_READ_BUFSIZE: usize = 8192;

/// `FS_CopyFile` -- copy a file, creating the destination's directories.  
/// Reachable from the background save worker, so it must stay free of zone
/// allocation and console output: FS_CreatePath only needs a mutable copy,
/// which the stack provides, and the caller reports failures with both paths.
#[no_mangle]
pub unsafe extern "C" fn FS_CopyFile(
    frompath: *const c_char,
    topath: *const c_char,
) -> c_int {
    let mut tmp = [0 as c_char; MAX_OSPATH];

    if frompath.is_null() || topath.is_null() {
        return 1;
    }
    if q_strlcpy(tmp.as_mut_ptr(), topath, MAX_OSPATH) >= MAX_OSPATH {
        return 1;
    }
    // create directories up to the dest file
    let err = FS_CreatePath(tmp.as_mut_ptr());
    if err != 0 {
        return err;
    }

    Sys_CopyFile(frompath, topath)
}

/// `FS_WriteFileFromHandle` -- copy an already-open file's first `size` bytes
/// to a path, creating directories as needed.
#[no_mangle]
pub unsafe extern "C" fn FS_WriteFileFromHandle(
    fromfile: *mut LibcFile,
    topath: *const c_char,
    size: usize,
) -> c_int {
    let mut buf = [0 as c_char; COPY_READ_BUFSIZE];
    let mut tmp = [0 as c_char; MAX_OSPATH];

    if fromfile.is_null() || topath.is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: null input\n".as_ptr(),
            c"FS_WriteFileFromHandle".as_ptr(),
        );
        return 1;
    }
    if q_strlcpy(tmp.as_mut_ptr(), topath, MAX_OSPATH) >= MAX_OSPATH {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: path too long\n".as_ptr(),
            c"FS_WriteFileFromHandle".as_ptr(),
        );
        return 1;
    }

    // create directories up to the dest file
    let err = FS_CreatePath(tmp.as_mut_ptr());
    if err != 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: unable to create directory\n".as_ptr(),
            c"FS_WriteFileFromHandle".as_ptr(),
        );
        return err;
    }

    let out = fs_fopen(topath, c"wb".as_ptr());
    if out.is_null() {
        // The C names the path first and the function second here, which is
        // the wrong way round; kept as written.
        CON_Printf(
            PRINT_NORMAL,
            c"%s: unable to create %s\n".as_ptr(),
            topath,
            c"FS_WriteFileFromHandle".as_ptr(),
        );
        return 1;
    }

    let mut remaining = size;
    while remaining != 0 {
        let count = if remaining < COPY_READ_BUFSIZE {
            remaining
        } else {
            COPY_READ_BUFSIZE
        };

        if fread(buf.as_mut_ptr().cast::<c_void>(), 1, count, fromfile) != count {
            break;
        }
        if fwrite(buf.as_ptr().cast::<c_void>(), 1, count, out) != count {
            break;
        }

        remaining -= count;
    }

    fclose(out);

    if remaining == 0 {
        0
    } else {
        1
    }
}

/// `FS_WriteFile` -- write a buffer under the user's gamedir.
#[no_mangle]
pub unsafe extern "C" fn FS_WriteFile(
    filename: *const c_char,
    data: *const c_void,
    len: usize,
) -> c_int {
    let mut name = [0 as c_char; MAX_OSPATH];
    let mut err: c_int = 0;

    FS_MakePath_BUF(
        MAKEPATH_USERDIR,
        &mut err,
        name.as_mut_ptr(),
        MAX_OSPATH,
        filename,
    );
    if err != 0 {
        QuakeFS_TargetHostError(
            c"%s: %d: string buffer overflow!".as_ptr(),
            c"FS_WriteFile".as_ptr(),
            1359,
        );
    }

    let f = fs_fopen(name.as_ptr(), c"wb".as_ptr());
    if f.is_null() {
        CON_Printf(PRINT_NORMAL, c"Error opening %s\n".as_ptr(), filename);
        return 1;
    }

    CON_Printf(
        PRINT_TERMONLY,
        c"%s: %s\n".as_ptr(),
        c"FS_WriteFile".as_ptr(),
        name.as_ptr(),
    );
    let size = fwrite(data, 1, len, f);
    fclose(f);
    if size != len {
        CON_Printf(PRINT_NORMAL, c"Error in writing %s\n".as_ptr(), filename);
        return 1;
    }
    0
}

/// `FS_CreatePath` -- make every directory component under the user's path.
/// The path must either name a file or end in a separator when the full path
/// is meant to exist.
#[no_mangle]
pub unsafe extern "C" fn FS_CreatePath(path: *mut c_char) -> c_int {
    let mut err: c_int = 0;

    if path.is_null() || *path == 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: no path!\n".as_ptr(),
            c"FS_CreatePath".as_ptr(),
        );
        return 1;
    }

    if !strstr(path, c"..".as_ptr()).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Relative pathnames are not allowed.\n".as_ptr(),
        );
        return 1;
    }

    let userdir = (*host_parms).userdir;
    let offset = strlen(userdir);
    if offset != 0 && strstr(path, userdir) != path {
        Sys_Error(c"Attempted to create a directory out of user's path".as_ptr());
    }

    let mut ofs = path.add(offset);
    if *ofs == 0 {
        return 0; // not necessarily an error
    }
    // check for the path separator after the userdir
    if is_dir_separator(*ofs) {
        ofs = ofs.add(1);
    } else if offset != 0 {
        // if the userdir itself has no trailing separator either, then it is a
        // bad path
        if !is_dir_separator(*userdir.add(offset - 1)) {
            CON_Printf(
                PRINT_NORMAL,
                c"%s: bad path\n".as_ptr(),
                c"FS_CreatePath".as_ptr(),
            );
            return 1;
        }
    }

    while *ofs != 0 {
        let c = *ofs;
        if is_dir_separator(c) {
            // create the directory
            *ofs = 0;
            err = Sys_mkdir(path, 0);
            *ofs = c;
            if err != 0 {
                break;
            }
        }
        ofs = ofs.add(1);
    }

    err
}

//============================================================================
// the load layer
//============================================================================
extern "C" {
    fn QuakeFS_TargetBeginDisc();
    fn QuakeFS_TargetEndDisc();
    /// engine/h2shared/common.h:101 -- the basename used as a hunk tag.
    fn COM_FileBase(input: *const c_char, out: *mut c_char, outsize: usize);
    fn Hunk_AllocName(size: c_int, name: *const c_char) -> *mut c_void;
    fn Hunk_TempAlloc(size: c_int) -> *mut c_void;
    /// The zone port's cache allocator.  Direct, not hooked: it is a Rust
    /// symbol in every binary, and its only use here is the LOADFILE_CACHE arm
    /// that no dedicated target can reach.
    fn Cache_Alloc(c: *mut CacheUserC, size: c_int, name: *const c_char) -> *mut c_void;
}

/// `FS_AllocLoadBuffer` -- the allocation half of a load, split out so the
/// archive path can obtain a buffer by exactly the same rules before inflating
/// into it (uhexen2-pzha).
unsafe fn fs_alloc_load_buffer(path: *const c_char, usehunk: c_int, len: c_long) -> *mut u8 {
    let mut base = [0 as c_char; 32];

    // extract the file's base name for the hunk tag
    COM_FileBase(path, base.as_mut_ptr(), 32);

    let buf: *mut c_void = match usehunk {
        LOADFILE_HUNK => Hunk_AllocName(len as c_int + 1, base.as_ptr()),
        LOADFILE_TEMPHUNK => Hunk_TempAlloc(len as c_int + 1),
        LOADFILE_ZONE => Z_Malloc(len as c_int + 1, ZONE_NUM),
        LOADFILE_CACHE => Cache_Alloc(LOADCACHE, len as c_int + 1, base.as_ptr()),
        LOADFILE_STACK => {
            if len < LOADSIZE {
                LOADBUF.cast::<c_void>()
            } else {
                Hunk_TempAlloc(len as c_int + 1)
            }
        }
        LOADFILE_MALLOC => malloc(len as usize + 1),
        _ => {
            Sys_Error(
                c"%s: bad usehunk".as_ptr(),
                c"FS_AllocLoadBuffer".as_ptr(),
            );
        }
    };

    if buf.is_null() {
        Sys_Error(
            c"%s: not enough space for %s".as_ptr(),
            c"FS_AllocLoadBuffer".as_ptr(),
            path,
        );
    }

    let bytes = buf.cast::<u8>();
    *bytes.add(len as usize) = 0;
    bytes
}

/// `FS_ReadIntoBuffer` -- allocate, read the whole thing, and define every
/// byte even when the read comes up short.
unsafe fn fs_read_into_buffer(
    h: *mut LibcFile,
    path: *const c_char,
    usehunk: c_int,
    len: c_long,
) -> *mut u8 {
    let buf = fs_alloc_load_buffer(path, usehunk, len);

    QuakeFS_TargetBeginDisc();
    let nread = fread(buf.cast::<c_void>(), 1, len as usize, h);
    fclose(h);
    QuakeFS_TargetEndDisc();

    // A short read means the promised length and the bytes that arrived
    // disagree.  Every caller sizes its parsing from fs_filesize rather than
    // from what was read, so the gap has to be filled: Hunk_AllocName hands
    // back zeroed memory but malloc, Cache_Alloc and the STACK buffer do not,
    // and on those paths the tail would be whatever the allocator held.
    // Deliberately not an error -- the loaders' contract is unchanged.
    // uhexen2-huys.
    if nread < len as usize {
        memset(
            buf.add(nread).cast::<c_void>(),
            0,
            len as usize - nread,
        );
        CON_Printf(
            PRINT_TERMONLY,
            c"WARNING: %s: short read on %s (%lu of %ld bytes)\n".as_ptr(),
            c"FS_ReadIntoBuffer".as_ptr(),
            path,
            nread as core::ffi::c_ulong,
            len,
        );
    }

    buf
}

/// `FS_ReadZipIntoBuffer` -- inflate the DEFLATED entry the preceding
/// FS_OpenFile left in fs_lastzip.  Allocates by the same rules, so from the
/// caller's side only the bytes' origin differs.
unsafe fn fs_read_zip_into_buffer(
    path: *const c_char,
    usehunk: c_int,
    len: c_long,
) -> *mut u8 {
    let buf = fs_alloc_load_buffer(path, usehunk, len);

    QuakeFS_TargetBeginDisc();
    if fs_zip_read_entry(FS_LASTZIP, FS_LASTZIPENTRY, buf.cast::<c_void>()) == 0 {
        // Matches FS_ReadIntoBuffer's short-read contract rather than
        // erroring: callers size their parsing from fs_filesize, so the buffer
        // has to be fully defined either way.
        memset(buf.cast::<c_void>(), 0, len as usize);
        CON_Printf(
            PRINT_TERMONLY,
            c"WARNING: %s: corrupt deflate stream for %s in %s\n".as_ptr(),
            c"FS_ReadZipIntoBuffer".as_ptr(),
            path,
            (*FS_LASTZIP).filename.as_ptr(),
        );
    }
    QuakeFS_TargetEndDisc();

    buf
}

/// `FS_LoadFile`
unsafe fn fs_load_file(
    path: *const c_char,
    usehunk: c_int,
    path_id: *mut c_uint,
) -> *mut u8 {
    let mut h: *mut LibcFile = core::ptr::null_mut();

    // look for it in the filesystem or pack files
    let len = fs_open_file_internal(path, &mut h, path_id, 0, 0);
    if len < 0 {
        return core::ptr::null_mut();
    }

    if h.is_null() {
        // deflated archive entry -- inflate rather than read
        return fs_read_zip_into_buffer(path, usehunk, len);
    }

    fs_read_into_buffer(h, path, usehunk, len)
}

/// `FS_LoadHunkFileFromOSPath` -- load a loose file by its OS path, with the
/// same state left behind that the searchpath path leaves.
#[no_mangle]
pub unsafe extern "C" fn FS_LoadHunkFileFromOSPath(ospath: *const c_char) -> *mut u8 {
    let len = Sys_filesize(ospath);
    if len < 0 {
        return core::ptr::null_mut();
    }
    let h = fs_fopen(ospath, c"rb".as_ptr());
    if h.is_null() {
        return core::ptr::null_mut();
    }

    // Callers read fs_filesize, file_from_pak and FS_LastFileSource() as state
    // left behind by the load: PR_LoadProgs uses fs_filesize three times, so a
    // stale one there corrupts the CRC and the bounds check silently.
    fs_filesize = len;
    file_from_pak = 0;
    q_strlcpy(
        (&raw mut FS_LASTFILE_SOURCE).cast::<c_char>(),
        ospath,
        MAX_OSPATH,
    );

    fs_read_into_buffer(h, ospath, LOADFILE_HUNK, len)
}

/// `FS_LoadHunkFile`
#[no_mangle]
pub unsafe extern "C" fn FS_LoadHunkFile(path: *const c_char, path_id: *mut c_uint) -> *mut u8 {
    fs_load_file(path, LOADFILE_HUNK, path_id)
}

/// `FS_LoadHunkFileFromPak` -- pak members only, with the provenance state
/// left naming the pak rather than the loose file it stepped over.
#[no_mangle]
pub unsafe extern "C" fn FS_LoadHunkFileFromPak(
    path: *const c_char,
    path_id: *mut c_uint,
) -> *mut u8 {
    let mut h: *mut LibcFile = core::ptr::null_mut();

    let len = fs_open_file_in_pak(path, &mut h, path_id);
    if len < 0 {
        return core::ptr::null_mut();
    }

    if h.is_null() {
        // deflated archive entry -- inflate rather than read
        return fs_read_zip_into_buffer(path, LOADFILE_HUNK, len);
    }

    fs_read_into_buffer(h, path, LOADFILE_HUNK, len)
}

/// `FS_LoadZoneFile`
#[no_mangle]
pub unsafe extern "C" fn FS_LoadZoneFile(
    path: *const c_char,
    zone_id: c_int,
    path_id: *mut c_uint,
) -> *mut u8 {
    ZONE_NUM = zone_id;
    fs_load_file(path, LOADFILE_ZONE, path_id)
}

/// `FS_LoadTempFile`
#[no_mangle]
pub unsafe extern "C" fn FS_LoadTempFile(path: *const c_char, path_id: *mut c_uint) -> *mut u8 {
    fs_load_file(path, LOADFILE_TEMPHUNK, path_id)
}

/// `FS_LoadCacheFile` -- the client-only cache variant.  The C compiles it
/// out of the dedicated builds entirely; here it exists but is unreachable
/// there, and its only target-only symbol (Cache_Alloc) is the zone port's.
#[no_mangle]
pub unsafe extern "C" fn FS_LoadCacheFile(
    path: *const c_char,
    cu: *mut CacheUserC,
    path_id: *mut c_uint,
) {
    LOADCACHE = cu;
    fs_load_file(path, LOADFILE_CACHE, path_id);
}

/// `FS_LoadStackFile` -- uses the temp hunk when the file is larger than the
/// caller's buffer.
#[no_mangle]
pub unsafe extern "C" fn FS_LoadStackFile(
    path: *const c_char,
    buffer: *mut c_void,
    bufsize: c_long,
    path_id: *mut c_uint,
) -> *mut u8 {
    LOADBUF = buffer.cast::<u8>();
    LOADSIZE = bufsize;
    fs_load_file(path, LOADFILE_STACK, path_id)
}

/// `FS_LoadMallocFile` -- returns malloc'd memory.
#[no_mangle]
pub unsafe extern "C" fn FS_LoadMallocFile(
    path: *const c_char,
    path_id: *mut c_uint,
) -> *mut u8 {
    fs_load_file(path, LOADFILE_MALLOC, path_id)
}

//============================================================================
// listing: the search path, and the tab-completion name list
//============================================================================
extern "C" {
    fn Sys_ListDirectories(
        path: *const c_char,
        dirs: *mut [c_char; 64],
        maxdirs: c_int,
    ) -> c_int;
    /// common.h:107 -- the rotating-buffer formatter.  A C variadic; calling
    /// one is stable, only defining one is not.
    fn va(format: *const c_char, ...) -> *mut c_char;
    fn q_strncasecmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strchr(s: *const c_char, c: c_int) -> *mut c_char;
    fn Z_Strdup(s: *const c_char) -> *mut c_char;
}

/// `MAX_GAMEDIRS` from quakefs.c:2240 -- how many subdirectories "game" probes.
const MAX_GAMEDIRS: usize = 128;

/// `FS_ListSearchSubdirs` -- enumerate subdirectories of `relpath` across the
/// loose (non-pak) searchpath entries, highest priority first, skipping
/// case-insensitive duplicates.
#[no_mangle]
pub unsafe extern "C" fn FS_ListSearchSubdirs(
    relpath: *const c_char,
    dirs: *mut [c_char; 64],
    maxdirs: c_int,
) -> c_int {
    let mut path = [0 as c_char; MAX_OSPATH];
    let mut buf = [[0 as c_char; 64]; 64];
    let mut count: c_int = 0;

    if relpath.is_null() || dirs.is_null() || maxdirs <= 0 {
        return 0;
    }

    let mut search = FS_SEARCHPATHS;
    while !search.is_null() && count < maxdirs {
        if !(*search).pack.is_null() {
            search = (*search).next;
            continue;
        }
        q_snprintf(
            path.as_mut_ptr(),
            MAX_OSPATH,
            c"%s/%s".as_ptr(),
            (*search).filename.as_ptr(),
            relpath,
        );
        let n = Sys_ListDirectories(path.as_ptr(), buf.as_mut_ptr(), 64);
        let mut i: c_int = 0;
        while i < n && count < maxdirs {
            let mut dup = 0;
            let mut j: c_int = 0;
            while j < count {
                if q_strcasecmp((*dirs.add(j as usize)).as_ptr(), buf[i as usize].as_ptr()) == 0 {
                    dup = 1;
                    break;
                }
                j += 1;
            }
            if dup == 0 {
                q_strlcpy(
                    (*dirs.add(count as usize)).as_mut_ptr(),
                    buf[i as usize].as_ptr(),
                    64,
                );
                count += 1;
            }
            i += 1;
        }
        search = (*search).next;
    }

    count
}

/// `FS_Path_f` -- print the search path.
#[allow(dead_code)] // registered by FS_Init, which is the next group
unsafe extern "C" fn fs_path_f() {
    CON_Printf(PRINT_NORMAL, c"Current search path:\n".as_ptr());

    let mut s = FS_SEARCHPATHS;
    while !s.is_null() {
        if s == FS_BASE_SEARCHPATHS {
            CON_Printf(PRINT_NORMAL, c"----------\n".as_ptr());
        }
        if !(*s).pack.is_null() {
            CON_Printf(
                PRINT_NORMAL,
                c"%s (%i files)\n".as_ptr(),
                (*(*s).pack).filename.as_ptr(),
                (*(*s).pack).numfiles,
            );
        } else {
            CON_Printf(PRINT_NORMAL, c"%s\n".as_ptr(), (*s).filename.as_ptr());
        }
        s = (*s).next;
    }
}

/// `FS_FreeNameList` -- release the names the last scan handed out.
#[no_mangle]
pub unsafe extern "C" fn FS_FreeNameList() {
    while LISTNAME_COUNT != 0 {
        LISTNAME_COUNT -= 1;
        Z_Free(LISTNAMES[LISTNAME_COUNT as usize].cast::<c_void>());
    }
}

/// `addListName` -- add one already-trimmed name, skipping case-insensitive
/// duplicates.  Returns 0 when skipped, the new count when added, or -1 when
/// the list is full.
unsafe fn add_list_name(name: *const c_char) -> c_int {
    if LISTNAME_COUNT >= MAX_LISTNAMES as c_int {
        CON_Printf(
            PRINT_NORMAL,
            c"WARNING: reached maximum number of names to list\n".as_ptr(),
        );
        return -1;
    }

    let mut j: c_int = 0;
    while j < LISTNAME_COUNT {
        if q_strcasecmp(LISTNAMES[j as usize], name) == 0 {
            return 0; // duplicated name.  skip.
        }
        j += 1;
    }

    LISTNAMES[LISTNAME_COUNT as usize] = Z_Strdup(name);
    LISTNAME_COUNT += 1;
    LISTNAME_COUNT
}

/// `addFileName` -- prefix-filter a filename, strip its extension and add it.
/// The prefix is matched with the extension still on, which is what the
/// maplist command has always done.
unsafe fn add_file_name(
    filename: *const c_char,
    partial: *const c_char,
    len_partial: usize,
    ext: *const c_char,
) -> c_int {
    let mut cur_name = [0 as c_char; MAX_QPATH];
    let extlen = strlen(ext);

    if len_partial != 0 && q_strncasecmp(partial, filename, len_partial) != 0 {
        return 0; // doesn't match the prefix.  skip.
    }

    let len = q_strlcpy(cur_name.as_mut_ptr(), filename, MAX_QPATH);
    if len >= MAX_QPATH {
        return 0; // truncated: not a name we could hand back
    }
    if len <= extlen {
        return 0;
    }

    let len = len - extlen;
    if q_strcasecmp((&raw const cur_name).cast::<c_char>().add(len), ext) != 0 {
        return 0;
    }

    cur_name[len] = 0;

    add_list_name(cur_name.as_ptr())
}

/// `FS_ScanFilesEx` -- walk every searchpath collecting files with the given
/// extension.  `subdir` names a directory below the gamedir, or is NULL to
/// scan the root, in which case pak entries in subdirectories are ignored.
unsafe fn fs_scan_files_ex(
    subdir: *const c_char,
    ext: *const c_char,
    prefix: *const c_char,
    pre_len: usize,
    reset: c_int,
) {
    let mut pakdir = [0 as c_char; MAX_QPATH];
    let mut pattern = [0 as c_char; MAX_QPATH];

    if reset != 0 {
        FS_FreeNameList();
    }

    pakdir[0] = 0;
    if !subdir.is_null() {
        q_snprintf(
            pakdir.as_mut_ptr(),
            MAX_QPATH,
            c"%s/".as_ptr(),
            subdir,
        );
    }
    let dirlen = strlen(pakdir.as_ptr());
    q_snprintf(
        pattern.as_mut_ptr(),
        MAX_QPATH,
        c"*%s".as_ptr(),
        ext,
    );

    let mut search = FS_SEARCHPATHS;
    while !search.is_null() {
        if !(*search).pack.is_null() {
            let mut i: c_int = 0;
            while i < (*(*search).pack).numfiles {
                let mut name = (*(*(*search).pack).files.add(i as usize)).name.as_ptr();

                if dirlen != 0 {
                    if strncmp(pakdir.as_ptr(), name, dirlen) != 0 {
                        i += 1;
                        continue;
                    }
                    name = name.add(dirlen);
                } else if !strchr(name, b'/' as c_int).is_null() {
                    // gamedir root only
                    i += 1;
                    continue;
                }

                if add_file_name(name, prefix, pre_len, ext) < 0 {
                    return;
                }
                i += 1;
            }
        } else {
            let mut find: FSFindC = core::mem::zeroed();
            let mut findname = Sys_FindFirstFile(
                &mut find,
                if !subdir.is_null() {
                    va(
                        c"%s/%s".as_ptr(),
                        (*search).filename.as_ptr(),
                        subdir,
                    )
                } else {
                    (*search).filename.as_ptr()
                },
                pattern.as_ptr(),
            );
            while !findname.is_null() {
                if add_file_name(findname, prefix, pre_len, ext) < 0 {
                    Sys_FindClose(&mut find);
                    return;
                }
                findname = Sys_FindNextFile(&mut find);
            }
            Sys_FindClose(&mut find);
        }
        search = (*search).next;
    }
}

/// `FS_ScanFiles` -- the common case: one extension, starting a fresh list.
unsafe fn fs_scan_files(
    subdir: *const c_char,
    ext: *const c_char,
    prefix: *const c_char,
    pre_len: usize,
) {
    fs_scan_files_ex(subdir, ext, prefix, pre_len, 1);
}

/// `fillMatches` -- hand the scanned names to a tab-completion caller in the
/// ListCommands() shape: fill `buf[pos+i]`, return the count added.
unsafe fn fill_matches(buf: *mut *const c_char, pos: c_int) -> c_int {
    if buf.is_null() {
        return 0;
    }

    let mut i: c_int = 0;
    while i < LISTNAME_COUNT {
        if pos + i >= MAX_MATCHES {
            break;
        }
        *buf.add((pos + i) as usize) = LISTNAMES[i as usize];
        i += 1;
    }

    i
}

/// The `strlen(prefix)` the listers hand to the scanner.
#[inline]
unsafe fn prefix_len(prefix: *const c_char) -> usize {
    if prefix.is_null() {
        0
    } else {
        strlen(prefix)
    }
}

/// `ListMaps`
#[no_mangle]
pub unsafe extern "C" fn ListMaps(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    fs_scan_files(c"maps".as_ptr(), c".bsp".as_ptr(), prefix, prefix_len(prefix));
    fill_matches(buf, pos)
}

/// `ListDemos`
#[no_mangle]
pub unsafe extern "C" fn ListDemos(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    fs_scan_files(core::ptr::null(), c".dem".as_ptr(), prefix, prefix_len(prefix));
    fill_matches(buf, pos)
}

/// `ListCfgs`
#[no_mangle]
pub unsafe extern "C" fn ListCfgs(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    fs_scan_files(core::ptr::null(), c".cfg".as_ptr(), prefix, prefix_len(prefix));
    fill_matches(buf, pos)
}

/// `ListSkies` -- the union of png, tga and pcx `_rt` faces with the suffix
/// trimmed; the right face is enough to offer a name.
#[no_mangle]
pub unsafe extern "C" fn ListSkies(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let pre_len = prefix_len(prefix);

    fs_scan_files_ex(c"gfx/env".as_ptr(), c"_rt.png".as_ptr(), prefix, pre_len, 1);
    fs_scan_files_ex(c"gfx/env".as_ptr(), c"_rt.tga".as_ptr(), prefix, pre_len, 0);
    fs_scan_files_ex(c"gfx/env".as_ptr(), c"_rt.pcx".as_ptr(), prefix, pre_len, 0);
    fill_matches(buf, pos)
}

/// `ListSaves` -- directories directly under the userdir holding an info.dat,
/// which is the same test Host_Loadgame_f applies before it reads one.
#[no_mangle]
pub unsafe extern "C" fn ListSaves(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let mut alldirs = [[0 as c_char; MAX_QPATH]; MAX_GAMEDIRS];
    let mut path = [0 as c_char; MAX_OSPATH];
    let pre_len = prefix_len(prefix);

    FS_FreeNameList();

    let numdirs = Sys_ListDirectories(
        (&raw const FS_USERDIR).cast::<c_char>(),
        alldirs.as_mut_ptr(),
        MAX_GAMEDIRS as c_int,
    );

    let mut i: c_int = 0;
    while i < numdirs {
        if pre_len != 0
            && q_strncasecmp(prefix, alldirs[i as usize].as_ptr(), pre_len) != 0
        {
            i += 1;
            continue;
        }
        q_snprintf(
            path.as_mut_ptr(),
            MAX_OSPATH,
            c"%s/%s/info.dat".as_ptr(),
            (&raw const FS_USERDIR).cast::<c_char>(),
            alldirs[i as usize].as_ptr(),
        );
        if Sys_FileType(path.as_ptr()) != FS_ENT_FILE {
            i += 1;
            continue;
        }
        if add_list_name(alldirs[i as usize].as_ptr()) < 0 {
            break;
        }
        i += 1;
    }

    fill_matches(buf, pos)
}

//============================================================================
// the getters
//============================================================================

/// `FS_GetBasedir`
#[no_mangle]
pub unsafe extern "C" fn FS_GetBasedir() -> *const c_char {
    fs_basedir_ptr()
}

/// `FS_GetUserbase`
#[no_mangle]
pub unsafe extern "C" fn FS_GetUserbase() -> *const c_char {
    (*host_parms).userdir
}

/// `FS_GetGamedir`
#[no_mangle]
pub unsafe extern "C" fn FS_GetGamedir() -> *const c_char {
    (&raw const FS_GAMEDIR).cast::<c_char>()
}

/// `FS_GetUserdir`
#[no_mangle]
pub unsafe extern "C" fn FS_GetUserdir() -> *const c_char {
    (&raw const FS_USERDIR).cast::<c_char>()
}

/// `FS_GetPortalsPathID`
#[no_mangle]
pub unsafe extern "C" fn FS_GetPortalsPathID() -> c_uint {
    FS_PORTALS_PATH_ID
}

/// `FS_GetGamedirPathID` -- the path_id of the gamedir added last, which is
/// the one fs_gamedir names; 0 before any gamedir exists.
#[no_mangle]
pub unsafe extern "C" fn FS_GetGamedirPathID() -> c_uint {
    if FS_SEARCHPATHS.is_null() {
        0
    } else {
        (*FS_SEARCHPATHS).path_id
    }
}

//============================================================================
// the FS_* stdio replacements
//============================================================================
// These exist so a caller can read non-sequentially from a file reopened on a
// pak member, where the stream's own position says nothing about where the
// member starts.  A DEFLATED archive entry has no stream at all, so the same
// handle also covers memory-backed reads -- which is why every one of these
// checks `fh->data` before touching `fh->file`.

/// `SEEK_CUR` from stdio.h (SEEK_SET and SEEK_END are above).
const SEEK_CUR: c_int = 1;
/// `EOF` from stdio.h.
const EOF: c_int = -1;
/// `EBADF`, `EFAULT` and `EINVAL` from errno.h -- 9, 14 and 22 on Linux, the
/// same in mingw's and Emscripten's errno.h, which is what the C passes to
/// `errno` here.
const EBADF: c_int = 9;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;

#[cfg(unix)]
extern "C" {
    fn __errno_location() -> *mut c_int;
}
#[cfg(windows)]
extern "C" {
    fn _errno() -> *mut c_int;
}

/// Set `errno`, the way the C's `errno = EBADF` does.
#[inline]
unsafe fn set_errno(value: c_int) {
    #[cfg(unix)]
    {
        *__errno_location() = value;
    }
    #[cfg(windows)]
    {
        *_errno() = value;
    }
}

extern "C" {
    fn clearerr(f: *mut LibcFile);
    fn ferror(f: *mut LibcFile) -> c_int;
    fn fgetc(f: *mut LibcFile) -> c_int;
    fn fgets(s: *mut c_char, size: c_int, f: *mut LibcFile) -> *mut c_char;
}

/// `FS_fread`
#[no_mangle]
pub unsafe extern "C" fn FS_fread(
    ptr: *mut c_void,
    size: usize,
    nmemb: usize,
    fh: *mut FSHandleC,
) -> usize {
    if fh.is_null() {
        set_errno(EBADF);
        return 0;
    }
    if ptr.is_null() {
        set_errno(EFAULT);
        return 0;
    }
    if size == 0 || nmemb == 0 {
        // no error, just zero bytes wanted
        set_errno(0);
        return 0;
    }

    let mut byte_size: c_long = (nmemb * size) as c_long;
    if byte_size > (*fh).length - (*fh).pos {
        // just read to end
        byte_size = (*fh).length - (*fh).pos;
    }
    let bytes_read: c_long;
    if !(*fh).data.is_null() {
        // inflated archive entry, already in memory
        memcpy(
            ptr,
            (*fh).data.add((*fh).pos as usize).cast::<c_void>(),
            byte_size as usize,
        );
        bytes_read = byte_size;
    } else {
        bytes_read = fread(ptr, 1, byte_size as usize, (*fh).file) as c_long;
    }
    (*fh).pos += bytes_read;

    // fread() must return the number of elements read, not the total number
    // of bytes.  A partially read last element still counts as whole.
    let mut nmemb_read = (bytes_read as usize) / size;
    if (bytes_read as usize) % size != 0 {
        nmemb_read += 1;
    }

    nmemb_read
}

/// `FS_fseek`
#[no_mangle]
pub unsafe extern "C" fn FS_fseek(fh: *mut FSHandleC, offset: c_long, whence: c_int) -> c_int {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }

    // the relative file position shouldn't be smaller than zero or bigger
    // than the filesize
    let mut offset = offset;
    match whence {
        SEEK_SET => {}
        SEEK_CUR => offset += (*fh).pos,
        SEEK_END => offset = (*fh).length + offset,
        _ => {
            set_errno(EINVAL);
            return -1;
        }
    }

    if offset < 0 {
        set_errno(EINVAL);
        return -1;
    }

    if offset > (*fh).length {
        // just seek to end
        offset = (*fh).length;
    }

    if (*fh).data.is_null() {
        let ret = fseek((*fh).file, (*fh).start + offset, SEEK_SET);
        if ret < 0 {
            return ret;
        }
    }

    (*fh).pos = offset;
    0
}

/// `FS_fclose`
#[no_mangle]
pub unsafe extern "C" fn FS_fclose(fh: *mut FSHandleC) -> c_int {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }
    if !(*fh).data.is_null() {
        free((*fh).data.cast::<c_void>());
        (*fh).data = core::ptr::null_mut();
        return 0;
    }
    fclose((*fh).file)
}

/// `FS_ftell`
#[no_mangle]
pub unsafe extern "C" fn FS_ftell(fh: *mut FSHandleC) -> c_long {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }
    (*fh).pos
}

/// `FS_rewind`
#[no_mangle]
pub unsafe extern "C" fn FS_rewind(fh: *mut FSHandleC) {
    if fh.is_null() {
        return;
    }
    if (*fh).data.is_null() {
        clearerr((*fh).file);
        fseek((*fh).file, (*fh).start, SEEK_SET);
    }
    (*fh).pos = 0;
}

/// `FS_feof`
#[no_mangle]
pub unsafe extern "C" fn FS_feof(fh: *mut FSHandleC) -> c_int {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }
    if (*fh).pos >= (*fh).length {
        return -1;
    }
    0
}

/// `FS_ferror`
#[no_mangle]
pub unsafe extern "C" fn FS_ferror(fh: *mut FSHandleC) -> c_int {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }
    if !(*fh).data.is_null() {
        // a memory-backed handle has no stream to fault
        return 0;
    }
    ferror((*fh).file)
}

/// `FS_fgetc`
#[no_mangle]
pub unsafe extern "C" fn FS_fgetc(fh: *mut FSHandleC) -> c_int {
    if fh.is_null() {
        set_errno(EBADF);
        return EOF;
    }
    if (*fh).pos >= (*fh).length {
        return EOF;
    }
    if !(*fh).data.is_null() {
        let c = *(*fh).data.add((*fh).pos as usize);
        (*fh).pos += 1;
        return c as c_int;
    }
    (*fh).pos += 1;
    fgetc((*fh).file)
}

/// `FS_fgets`
#[no_mangle]
pub unsafe extern "C" fn FS_fgets(
    s: *mut c_char,
    size: c_int,
    fh: *mut FSHandleC,
) -> *mut c_char {
    if FS_feof(fh) != 0 {
        return core::ptr::null_mut();
    }

    let mut size = size;
    if size as c_long > ((*fh).length - (*fh).pos) + 1 {
        size = ((*fh).length - (*fh).pos + 1) as c_int;
    }

    if !(*fh).data.is_null() {
        // same contract as fgets(): copy up to and including the first
        // newline, stop at size-1 bytes, always NUL-terminate.
        if size < 2 {
            return core::ptr::null_mut();
        }
        let mut i: c_int = 0;
        while i < size - 1 && (*fh).pos < (*fh).length {
            let c = *(*fh).data.add((*fh).pos as usize) as c_char;
            (*fh).pos += 1;
            *s.add(i as usize) = c;
            i += 1;
            if c as u8 == b'\n' {
                break;
            }
        }
        *s.add(i as usize) = 0;
        return if i != 0 { s } else { core::ptr::null_mut() };
    }

    let ret = fgets(s, size, (*fh).file);
    (*fh).pos = ftell((*fh).file) - (*fh).start;

    ret
}

/// `FS_filelength`
#[no_mangle]
pub unsafe extern "C" fn FS_filelength(fh: *mut FSHandleC) -> c_long {
    if fh.is_null() {
        set_errno(EBADF);
        return -1;
    }
    (*fh).length
}

//============================================================================
// the mod commands
//============================================================================
extern "C" {
    /// engine/h2shared/common.h:89, 112.
    fn COM_CheckParm(parm: *const c_char) -> c_int;
    fn COM_StrCompare(arg1: *const c_void, arg2: *const c_void) -> c_int;
    fn atoi(s: *const c_char) -> c_int;
    fn rand() -> c_int;
    fn Sys_DoubleTime() -> f64;
    /// The cmd port's own functions.
    fn Cmd_AddCommand(cmd_name: *const c_char, function: Option<unsafe extern "C" fn()>);
    fn Cmd_Argc() -> c_int;
    fn Cmd_Argv(arg: c_int) -> *const c_char;
    fn Cbuf_AddText(text: *const c_char);
    fn Cbuf_Clear();
    /// The cvar port's.
    fn Cvar_RegisterVariable(variable: *mut CvarC);
    fn Cvar_SetROM(var_name: *const c_char, value: *const c_char);
    /// The other hooks this group needs.
    fn QuakeFS_TargetIsServerOnly() -> c_int;
    fn QuakeFS_TargetHasClientCommands() -> c_int;
    fn QuakeFS_TargetOldProtocolRequest() -> c_int;
    fn QuakeFS_TargetClientReset();
    fn QuakeFS_TargetClientClearState();
    fn QuakeFS_TargetClientReinit();
    fn QuakeFS_TargetVidLock();
    fn QuakeFS_TargetStartupScript() -> *const c_char;
    fn QuakeFS_TargetShowList(num: c_int, list: *const *const c_char);
}

/// `BSPVERSION`, `BSP2VERSION`, `HEADER_LUMPS` and `LUMP_ENTITIES` from
/// common/bspfile.h:57-65.  BSP2's version is the four characters "BSP2"
/// read as a little-endian int, spelled the way the header spells it.
const BSPVERSION: c_int = 29;
const BSP2VERSION: c_int =
    (b'B' as c_int) << 0 | (b'S' as c_int) << 8 | (b'P' as c_int) << 16 | (b'2' as c_int) << 24;
const HEADER_LUMPS: usize = 15;
const LUMP_ENTITIES: usize = 0;

/// `lump_t` from common/bspfile.h:59-63.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LumpC {
    pub fileofs: c_int,
    pub filelen: c_int,
}

/// `dheader_t` from common/bspfile.h:91-96.  Both BSP formats this engine
/// loads share it: version 29 and BSP2 differ in the width of node, leaf and
/// edge records, never in the header.
#[repr(C)]
pub struct DHeaderC {
    pub version: c_int,
    pub lumps: [LumpC; HEADER_LUMPS],
}

const _: () = {
    assert!(core::mem::size_of::<LumpC>() == 8);
    assert!(core::mem::offset_of!(DHeaderC, version) == 0);
    assert!(core::mem::offset_of!(DHeaderC, lumps) == 4);
    assert!(core::mem::size_of::<DHeaderC>() == 4 + HEADER_LUMPS * 8);
};

/// `BigShort` from common/q_endian.h -- a byte swap on a little-endian host.
#[inline]
fn big_short(v: u16) -> u16 {
    #[cfg(target_endian = "little")]
    {
        v.swap_bytes()
    }
    #[cfg(target_endian = "big")]
    {
        v
    }
}

/// `FS_IsGamedir` -- does `dir` under `basedir` look like game data?  Gamecode
/// (progs.dat or hwprogs.dat), any pak0..pak9, any .pk3, or a maps/ directory
/// holding at least one .bsp.
#[no_mangle]
pub unsafe extern "C" fn FS_IsGamedir(basedir: *const c_char, dir: *const c_char) -> c_int {
    let mut path = [0 as c_char; MAX_OSPATH];

    q_snprintf(
        path.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/%s/progs.dat".as_ptr(),
        basedir,
        dir,
    );
    if Sys_FileType(path.as_ptr()) == FS_ENT_FILE {
        return 1;
    }

    // ...or hwprogs.dat: a HexenWorld mod carries gamecode just as much as a
    // Hexen II one does (uhexen2-3m0h).
    q_snprintf(
        path.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/%s/hwprogs.dat".as_ptr(),
        basedir,
        dir,
    );
    if Sys_FileType(path.as_ptr()) == FS_ENT_FILE {
        return 1;
    }

    let mut i: c_int = 0;
    while i < 10 {
        q_snprintf(
            path.as_mut_ptr(),
            MAX_OSPATH,
            c"%s/%s/pak%d.pak".as_ptr(),
            basedir,
            dir,
            i,
        );
        if Sys_FileType(path.as_ptr()) == FS_ENT_FILE {
            return 1;
        }
        i += 1;
    }

    // ...or any .pk3: a mod shipped as a single archive is still a mod
    // (uhexen2-pzha).  Unlike the pak probe this cannot be a fixed set of
    // names.
    q_snprintf(
        path.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/%s".as_ptr(),
        basedir,
        dir,
    );
    let mut find: FSFindC = core::mem::zeroed();
    let findname = Sys_FindFirstFile(&mut find, path.as_ptr(), c"*.pk3".as_ptr());
    let found = !findname.is_null();
    Sys_FindClose(&mut find);
    if found {
        return 1;
    }

    // ...or a maps/ directory holding at least one .bsp: a pure map pack
    // ships no gamecode and no archive, but it is still a mod the player
    // dropped in (uhexen2-3m0h).  The .bsp probe matters -- a bare maps/
    // directory, or one holding only .lit or .ent sidecars, is not a map pack.
    q_snprintf(
        path.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/%s/maps".as_ptr(),
        basedir,
        dir,
    );
    if Sys_FileType(path.as_ptr()) != FS_ENT_DIRECTORY {
        return 0;
    }

    let mut find: FSFindC = core::mem::zeroed();
    let findname = Sys_FindFirstFile(&mut find, path.as_ptr(), c"*.bsp".as_ptr());
    let found = !findname.is_null();
    Sys_FindClose(&mut find);

    if found {
        1
    } else {
        0
    }
}

/// `ListGames` -- everything Host_Game_f accepts: data1 unconditionally,
/// portals when installed, and every other game data directory under both
/// roots.
#[no_mangle]
pub unsafe extern "C" fn ListGames(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let mut alldirs = [[0 as c_char; MAX_QPATH]; MAX_GAMEDIRS];
    let mut path = [0 as c_char; MAX_OSPATH];
    let pre_len = prefix_len(prefix);

    FS_FreeNameList();

    // fs_basedir, not host_parms->basedir: Host_Game_f resolves its argument
    // against the former, which -basedir moves (uhexen2-5mhd).
    if pre_len == 0 || q_strncasecmp(prefix, c"data1".as_ptr(), pre_len) == 0 {
        add_list_name(c"data1".as_ptr());
    }

    q_snprintf(
        path.as_mut_ptr(),
        MAX_OSPATH,
        c"%s/portals".as_ptr(),
        fs_basedir_ptr(),
    );
    if Sys_FileType(path.as_ptr()) == FS_ENT_DIRECTORY {
        if pre_len == 0 || q_strncasecmp(prefix, c"portals".as_ptr(), pre_len) == 0 {
            add_list_name(c"portals".as_ptr());
        }
    }

    // Both roots: a gamedir under the userdir mounts exactly as one under the
    // basedir does, and addListName de-duplicates case-insensitively.  The
    // second pass is skipped where the two are the same directory
    // (uhexen2-3m0h).
    let mut numdirs = Sys_ListDirectories(
        fs_basedir_ptr(),
        alldirs.as_mut_ptr(),
        MAX_GAMEDIRS as c_int,
    );
    if strcmp(fs_basedir_ptr(), (*host_parms).userdir) != 0 {
        numdirs += Sys_ListDirectories(
            (*host_parms).userdir,
            alldirs.as_mut_ptr().add(numdirs as usize),
            MAX_GAMEDIRS as c_int - numdirs,
        );
    }

    let mut i: c_int = 0;
    while i < numdirs {
        let dir = alldirs[i as usize].as_ptr();

        // hw holds HexenWorld's data, which only the hwcl/hwsv binaries can
        // use; the single player client has nothing to do with it
        if q_strcasecmp(dir, c"hw".as_ptr()) == 0 {
            i += 1;
            continue;
        }
        if pre_len != 0 && q_strncasecmp(prefix, dir, pre_len) != 0 {
            i += 1;
            continue;
        }
        if FS_IsGamedir(fs_basedir_ptr(), dir) == 0
            && FS_IsGamedir((*host_parms).userdir, dir) == 0
        {
            i += 1;
            continue;
        }
        if add_list_name(dir) < 0 {
            break;
        }
        i += 1;
    }

    fill_matches(buf, pos)
}

/// The `Cmd_Argc() > 1 ? Cmd_Argv(1) : NULL` preamble every lister command
/// shares.
#[inline]
unsafe fn cmd_prefix_arg() -> (*const c_char, usize) {
    if Cmd_Argc() > 1 {
        let prefix = Cmd_Argv(1);
        (prefix, strlen(prefix))
    } else {
        (core::ptr::null(), 0)
    }
}

/// The shared tail of maplist/skies/games: nothing found, else sort and show.
unsafe fn show_name_list(empty_msg: *const c_char, found_fmt: *const c_char) {
    if LISTNAME_COUNT == 0 {
        CON_Printf(PRINT_NORMAL, empty_msg);
        return;
    }

    CON_Printf(PRINT_NORMAL, found_fmt, LISTNAME_COUNT);
    if LISTNAME_COUNT > 1 {
        qsort(
            (&raw mut LISTNAMES).cast::<c_void>(),
            LISTNAME_COUNT as usize,
            PTR_SIZE,
            com_str_compare_thunk,
        );
    }
    QuakeFS_TargetShowList(LISTNAME_COUNT, (&raw const LISTNAMES).cast::<*const c_char>());
    CON_Printf(PRINT_NORMAL, c"\n".as_ptr());

    FS_FreeNameList();
}

/// `COM_StrCompare` is a C function, so it cannot be passed to `qsort`
/// directly through a Rust `unsafe extern "C" fn` item without naming it; this
/// thunk is the one place that does.
unsafe extern "C" fn com_str_compare_thunk(a: *const c_void, b: *const c_void) -> c_int {
    COM_StrCompare(a, b)
}

/// `FS_Maplist_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn FS_Maplist_f() {
    let (prefix, pre_len) = cmd_prefix_arg();

    fs_scan_files(c"maps".as_ptr(), c".bsp".as_ptr(), prefix, pre_len);
    show_name_list(
        c"No maps found.\n\n".as_ptr(),
        c"Found %d maps:\n\n".as_ptr(),
    );
}

/// `FS_BuildMapList` -- the scan maplist and randmap already do, exposed so
/// the maps browser menu can drive it too (uhexen2-a5nn.13).  The names live
/// in the shared scan storage until FS_FreeNameList or the next scan.
#[no_mangle]
pub unsafe extern "C" fn FS_BuildMapList(prefix: *const c_char) -> c_int {
    fs_scan_files(c"maps".as_ptr(), c".bsp".as_ptr(), prefix, prefix_len(prefix));

    if LISTNAME_COUNT > 1 {
        qsort(
            (&raw mut LISTNAMES).cast::<c_void>(),
            LISTNAME_COUNT as usize,
            PTR_SIZE,
            com_str_compare_thunk,
        );
    }

    LISTNAME_COUNT
}

/// `FS_MapListName`
#[no_mangle]
pub unsafe extern "C" fn FS_MapListName(i: c_int) -> *const c_char {
    if i < 0 || i >= LISTNAME_COUNT {
        return core::ptr::null();
    }
    LISTNAMES[i as usize]
}

/// `FS_MapTitle_Quoted` -- walk to a quoted value, stopping at '}' so the
/// scan cannot leave worldspawn for the next entity.
unsafe fn fs_map_title_quoted(
    mut p: *const c_char,
    out: *mut c_char,
    outsize: usize,
) -> *const c_char {
    let mut n: usize = 0;

    while *p != 0 && *p != b'"' as c_char && *p != b'}' as c_char {
        p = p.add(1);
    }
    if *p != b'"' as c_char {
        return core::ptr::null();
    }
    p = p.add(1);
    while *p != 0 && *p != b'"' as c_char {
        if n + 1 < outsize {
            *out.add(n) = *p;
            n += 1;
        }
        p = p.add(1);
    }
    if *p != b'"' as c_char {
        return core::ptr::null();
    }
    *out.add(n) = 0;
    p.add(1)
}

/// `FS_GetMapTitle` -- the worldspawn "message" key of maps/<name>.bsp, which
/// is what the maps browser lists instead of a filename.
#[no_mangle]
pub unsafe extern "C" fn FS_GetMapTitle(
    mapname: *const c_char,
    out: *mut c_char,
    outsize: usize,
) -> c_int {
    let mut path = [0 as c_char; MAX_QPATH];
    let mut buf = [0 as c_char; 8192];
    let mut key = [0 as c_char; 64];
    let mut val = [0 as c_char; 256];

    if out.is_null() || outsize == 0 {
        return 0;
    }
    *out = 0;
    if mapname.is_null() || *mapname == 0 {
        return 0;
    }

    q_snprintf(
        path.as_mut_ptr(),
        MAX_QPATH,
        c"maps/%s.bsp".as_ptr(),
        mapname,
    );
    let mut fh: FSHandleC = core::mem::zeroed();
    if FS_OpenFileHandle_Silent(path.as_ptr(), &mut fh, core::ptr::null_mut()) < 0 {
        return 0;
    }

    let mut header: DHeaderC = core::mem::zeroed();
    if FS_fread(
        (&mut header as *mut DHeaderC).cast::<c_void>(),
        1,
        core::mem::size_of::<DHeaderC>(),
        &mut fh,
    ) != core::mem::size_of::<DHeaderC>()
    {
        FS_fclose(&mut fh);
        return 0;
    }

    let version = little_long(header.version);
    if version != BSPVERSION && version != BSP2VERSION {
        FS_fclose(&mut fh);
        return 0;
    }

    let ofs = little_long(header.lumps[LUMP_ENTITIES].fileofs);
    let mut len = little_long(header.lumps[LUMP_ENTITIES].filelen);
    if ofs < 0 || len <= 0 {
        FS_fclose(&mut fh);
        return 0;
    }
    if len > 8192 - 1 {
        len = 8192 - 1;
    }

    if FS_fseek(&mut fh, ofs as c_long, SEEK_SET) != 0 {
        FS_fclose(&mut fh);
        return 0;
    }
    let got = FS_fread(buf.as_mut_ptr().cast::<c_void>(), 1, len as usize, &mut fh);
    FS_fclose(&mut fh);
    if got == 0 {
        return 0;
    }
    buf[got] = 0;

    let mut p = buf.as_ptr();
    while *p != 0 && *p != b'{' as c_char {
        p = p.add(1);
    }
    if *p == 0 {
        return 0;
    }
    p = p.add(1);

    loop {
        p = fs_map_title_quoted(p, key.as_mut_ptr(), 64);
        if p.is_null() {
            break;
        }
        p = fs_map_title_quoted(p, val.as_mut_ptr(), 256);
        if p.is_null() {
            break;
        }
        if q_strcasecmp(key.as_ptr(), c"message".as_ptr()) == 0 {
            q_strlcpy(out, val.as_ptr(), outsize);
            return if *out != 0 { 1 } else { 0 };
        }
    }

    0
}

/// `FS_RandMap_f` -- pick one of the installed maps and start it.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn FS_RandMap_f() {
    let (prefix, pre_len) = cmd_prefix_arg();

    fs_scan_files(c"maps".as_ptr(), c".bsp".as_ptr(), prefix, pre_len);

    if LISTNAME_COUNT == 0 {
        CON_Printf(PRINT_NORMAL, c"No maps found.\n".as_ptr());
        return;
    }

    // rand() is never seeded anywhere in this engine, and the callers that
    // matter -- cl_tent.c's debris angles and everything downstream of the
    // gamecode's RNG -- are written against that determinism.  Seeding it from
    // a console command would change all of them; mix the clock in locally
    // instead, so two randmaps in one session differ and nothing else is
    // touched.
    let which = ((rand() as c_uint) + ((Sys_DoubleTime() * 1000.0) as c_uint))
        % (LISTNAME_COUNT as c_uint);

    CON_Printf(
        PRINT_NORMAL,
        c"Starting map %s...\n".as_ptr(),
        LISTNAMES[which as usize],
    );
    Cbuf_AddText(va(c"map %s\n".as_ptr(), LISTNAMES[which as usize]));

    FS_FreeNameList();
}

/// `FS_Skies_f` -- the skyboxes `sky` will accept.  gl_sky.c's suf[] and its
/// two search directories are restated because quakefs.c is renderer-
/// independent and is also compiled into the software client, which has no
/// gl_sky.c to extern them from.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn FS_Skies_f() {
    static SKYFACES: [&[u8]; 6] = [b"rt\0", b"bk\0", b"lf\0", b"ft\0", b"up\0", b"dn\0"];
    static SKYEXTS: [&[u8]; 3] = [b".png\0", b".tga\0", b".pcx\0"];
    static SKYDIRS: [&[u8]; 2] = [b"gfx/env\0", b"skies\0"];

    let (prefix, pre_len) = cmd_prefix_arg();
    let mut suffix = [0 as c_char; 16];
    let mut reset: c_int = 1;

    for d in 0..2 {
        for f in 0..6 {
            for e in 0..3 {
                q_snprintf(
                    suffix.as_mut_ptr(),
                    16,
                    c"_%s%s".as_ptr(),
                    SKYFACES[f].as_ptr().cast::<c_char>(),
                    SKYEXTS[e].as_ptr().cast::<c_char>(),
                );
                fs_scan_files_ex(
                    SKYDIRS[d].as_ptr().cast::<c_char>(),
                    suffix.as_ptr(),
                    prefix,
                    pre_len,
                    reset,
                );
                reset = 0;
            }
        }
    }

    show_name_list(
        c"No skyboxes found.\n\n".as_ptr(),
        c"Found %d skyboxes:\n\n".as_ptr(),
    );
}

/// `FS_Games_f` -- the same scan ListGames does, rendered to the console
/// instead of into a completion buffer; fillMatches returns early on a NULL
/// buffer, which is what makes passing NULL here safe.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn FS_Games_f() {
    let (prefix, _pre_len) = cmd_prefix_arg();

    ListGames(prefix, core::ptr::null_mut(), 0);
    show_name_list(
        c"No game directories found.\n\n".as_ptr(),
        c"Found %d game directories:\n\n".as_ptr(),
    );
}

/// `CheckRegistered` -- the pop.txt graphic has to be in the pak for the
/// registered features, and this verifies it byte for byte.
#[no_mangle]
pub unsafe extern "C" fn CheckRegistered() -> c_int {
    let mut check = [0u16; 128];
    let mut fh: FSHandleC = core::mem::zeroed();

    // Through the handle, not a raw FILE *: an install repacked as a .pk3
    // keeps pop.lmp in a deflated entry, and FS_OpenFile hands back a NULL
    // FILE * for those.  Reading it the old way silently demoted a registered
    // install to shareware.
    if FS_OpenFileHandle(c"gfx/pop.lmp".as_ptr(), &mut fh, core::ptr::null_mut()) < 0 {
        return -1;
    }

    if FS_fread(
        check.as_mut_ptr().cast::<c_void>(),
        1,
        core::mem::size_of_val(&check),
        &mut fh,
    ) != core::mem::size_of_val(&check)
    {
        FS_fclose(&mut fh);
        return -1;
    }
    FS_fclose(&mut fh);

    for i in 0..128 {
        if POP[i] != big_short(check[i]) {
            CON_Printf(PRINT_TERMONLY, c"Corrupted data file\n".as_ptr());
            return -1;
        }
    }

    0
}

/// `Host_Game_f` -- runtime mod switching: "game <dirname>" to switch, "game"
/// to print the current one.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Host_Game_f() {
    let mut path = [0 as c_char; MAX_OSPATH];

    if Cmd_Argc() < 2 {
        CON_Printf(
            PRINT_NORMAL,
            c"Current game directory: %s\n".as_ptr(),
            (&raw const fs_gamedir_nopath).cast::<c_char>(),
        );
        return;
    }

    let mut dir = Cmd_Argv(1);

    // optional second arg: 1 = include portals data
    let use_portals = Cmd_Argc() >= 3 && atoi(Cmd_Argv(2)) != 0;

    // validate
    if *dir == 0
        || strcmp(dir, c".".as_ptr()) == 0
        || !strstr(dir, c"..".as_ptr()).is_null()
        || !strstr(dir, c"/".as_ptr()).is_null()
        || !strstr(dir, c"\\".as_ptr()).is_null()
    {
        CON_Printf(
            PRINT_NORMAL,
            c"gamedir should be a single directory name, not a path\n".as_ptr(),
        );
        return;
    }

    // switching back to base game
    if q_strcasecmp(dir, c"data1".as_ptr()) == 0 {
        dir = c"data1".as_ptr();
    }

    // already the current game?
    if q_strcasecmp((&raw const fs_gamedir_nopath).cast::<c_char>(), dir) == 0 {
        CON_Printf(PRINT_NORMAL, c"Already running: %s\n".as_ptr(), dir);
        return;
    }

    // validate that the directory exists (skip for data1)
    if q_strcasecmp(dir, c"data1".as_ptr()) != 0 {
        // fs_basedir, not host_parms->basedir: -basedir moves the former and
        // never touches the latter (uhexen2-5mhd).
        q_snprintf(
            path.as_mut_ptr(),
            MAX_OSPATH,
            c"%s/%s".as_ptr(),
            fs_basedir_ptr(),
            dir,
        );
        if Sys_FileType(path.as_ptr()) != FS_ENT_DIRECTORY {
            // ...or under the userdir: FS_AddGameDirectory mounts both and
            // requires neither to exist, so a mod installed only under
            // ~/.hexen2/<dir> loads perfectly well (uhexen2-3m0h).
            q_snprintf(
                path.as_mut_ptr(),
                MAX_OSPATH,
                c"%s/%s".as_ptr(),
                (*host_parms).userdir,
                dir,
            );
            if Sys_FileType(path.as_ptr()) != FS_ENT_DIRECTORY {
                CON_Printf(
                    PRINT_NORMAL,
                    c"Game directory \"%s\" not found\n".as_ptr(),
                    dir,
                );
                return;
            }
        }
    }

    // === FULL ENGINE RESET ===

    // save config to the old mod's directory, stop everything, flush the
    // memory, and drop the stale client state
    QuakeFS_TargetClientReset();
    QuakeFS_TargetClientClearState();

    // discard any pending commands from the old mod
    Cbuf_Clear();

    // Reset the filesystem back to the base game before adding new game
    // paths.  Unwind to fs_base_nomp_searchpaths, NOT fs_base_searchpaths:
    // the latter includes portals whenever the session was launched with
    // -game, and stopping there left pak3 on the path across every later
    // switch (uhexen2-5vb6).
    fs_unwind_searchpaths(FS_BASE_NOMP_SEARCHPATHS, 0);
    FS_BASE_SEARCHPATHS = FS_BASE_NOMP_SEARCHPATHS;

    // The unwind above just took pak3/pak4 off the searchpath, so drop the
    // flag that says they are on it.  gameflags is accumulate-only except for
    // this one bit, and left stale it is fatal rather than cosmetic:
    // CL_ParseServerInfo calls CL_LoadInfoStrings whenever it is set, and that
    // Host_Errors on a missing infolist.txt in the middle of a map load
    // (uhexen2-lx4m).  It is re-set below when portals is genuinely re-added.
    gameflags &= !GAME_PORTALS;

    Cache_Flush();

    // optionally add portals as base for custom mods
    if use_portals
        && q_strcasecmp(dir, c"data1".as_ptr()) != 0
        && q_strcasecmp(dir, c"portals".as_ptr()) != 0
    {
        q_snprintf(
            path.as_mut_ptr(),
            MAX_OSPATH,
            c"%s/portals".as_ptr(),
            fs_basedir_ptr(),
        );
        if Sys_FileType(path.as_ptr()) == FS_ENT_DIRECTORY {
            fs_add_game_directory(c"portals".as_ptr(), 1);
            FS_BASE_SEARCHPATHS = FS_SEARCHPATHS;
        }
    }

    // add the new game directory (skip for data1 -- already in base)
    if q_strcasecmp(dir, c"data1".as_ptr()) != 0 {
        let base_fs = if q_strcasecmp(dir, c"portals".as_ptr()) == 0 {
            1
        } else {
            0
        };
        fs_add_game_directory(dir, base_fs);
    } else {
        // reset gamedir tracking to data1
        qerr_strlcpy(
            c"Host_Game_f".as_ptr(),
            3471,
            (&raw mut fs_gamedir_nopath).cast::<c_char>(),
            c"data1".as_ptr(),
            MAX_QPATH,
        );
        FS_MakePath_BUF(
            MAKEPATH_BASEDIR,
            core::ptr::null_mut(),
            (&raw mut FS_GAMEDIR).cast::<c_char>(),
            MAX_OSPATH,
            c"data1".as_ptr(),
        );
        FS_MakePath_BUF(
            MAKEPATH_USERBASE,
            core::ptr::null_mut(),
            (&raw mut FS_USERDIR).cast::<c_char>(),
            MAX_OSPATH,
            c"data1".as_ptr(),
        );
    }

    // Reload the client's view of the new mod and hold the video mode across
    // the config exec that follows
    QuakeFS_TargetClientReinit();
    QuakeFS_TargetVidLock();
    Cbuf_AddText(c"unbindall\nunaliasall\n".as_ptr());
    Cbuf_AddText(QuakeFS_TargetStartupScript());
    Cbuf_AddText(c"vid_unlock\n".as_ptr());
    CON_Printf(PRINT_NORMAL, c"\ngame changed to \"%s\"\n".as_ptr(), dir);
}

//============================================================================
// FS_Init
//============================================================================

/// `ENABLE_OLD_RETAIL` and `ENABLE_OLD_DEMO` from engine/h2shared/h2config.h
/// :101 and :119 -- both 0 in this tree, so both refusals are live.
const ENABLE_OLD_RETAIL: c_int = 0;
const ENABLE_OLD_DEMO: c_int = 0;
/// `PROTOCOL_RAVEN_111` from engine/hexen2/protocol.h:29.
const PROTOCOL_RAVEN_111: c_int = 18;

/// `FS_Init` -- the whole search path, in the C's order, because that order is
/// what gives the paths their meaning: data1 and its paks, then the checks
/// that decide whether this is a real installation at all, then the mission
/// pack, then hw for HexenWorld, and only then any -game/-mod override.
///
/// The H2MP branch of the mission-pack selection is dead in this tree
/// (h2config.h undefines it), so only the H2W and plain forms are ported; the
/// C's `#if !defined(H2W)` guards on `sv_protocol` and on the modified-games
/// refusal are the shim's QuakeFS_TargetOldProtocolRequest and
/// QuakeFS_TargetIsServerOnly, since `sv_protocol` does not exist in hwsv.
#[no_mangle]
pub unsafe extern "C" fn FS_Init() {
    let mut check_portals: c_int = 0;

    Cvar_RegisterVariable(&raw mut oem);
    Cvar_RegisterVariable(&raw mut registered);

    Cmd_AddCommand(c"path".as_ptr(), Some(fs_path_f));
    // The five client-only ones, behind the predicate: a command that exists
    // in one target and not another is observable through Cmd_Exists.
    if QuakeFS_TargetHasClientCommands() != 0 {
        Cmd_AddCommand(c"maplist".as_ptr(), Some(FS_Maplist_f));
        Cmd_AddCommand(c"randmap".as_ptr(), Some(FS_RandMap_f));
        Cmd_AddCommand(c"skies".as_ptr(), Some(FS_Skies_f));
        Cmd_AddCommand(c"games".as_ptr(), Some(FS_Games_f));
        Cmd_AddCommand(c"game".as_ptr(), Some(Host_Game_f));
    }

    // -basedir <path> overrides the system supplied base directory
    let i = COM_CheckParm(c"-basedir".as_ptr());
    if i != 0 && i < (*host_parms).argc - 1 {
        FS_BASEDIR = *(*host_parms).argv.add((i + 1) as usize);
        if *FS_BASEDIR == 0 {
            Sys_Error(c"Bad argument to -basedir".as_ptr());
        }
        if !do_userdirs() {
            (*host_parms).userdir = *(*host_parms).argv.add((i + 1) as usize);
        }
        CON_Printf(
            PRINT_TERMONLY,
            c"%s: basedir changed to: %s\n".as_ptr(),
            c"FS_Init".as_ptr(),
            FS_BASEDIR,
        );
    } else {
        FS_BASEDIR = (*host_parms).basedir;
    }

    // step 1: start up with data1 by default
    fs_add_game_directory(c"data1".as_ptr(), 1);

    if (gameflags & GAME_REGISTERED0) != 0 && (gameflags & GAME_REGISTERED1) != 0 {
        gameflags |= GAME_REGISTERED;
    }
    if (gameflags & GAME_OEM0) != 0 && (gameflags & GAME_OEM2) != 0 {
        gameflags |= GAME_OEM;
    }
    if (gameflags & GAME_OLD_CDROM0) != 0 && (gameflags & GAME_OLD_CDROM1) != 0 {
        gameflags |= GAME_REGISTERED_OLD;
    }
    if (gameflags & GAME_OLD_OEM0) != 0 && (gameflags & GAME_OLD_OEM2) != 0 {
        gameflags |= GAME_OLD_OEM;
    }

    // check for bad installations (mix'n'match data)
    let registered_bits = GAME_REGISTERED | GAME_REGISTERED_OLD;
    let demos = GAME_DEMO | GAME_OLD_DEMO;
    let oems = GAME_OEM0 | GAME_OLD_OEM0 | GAME_OEM2 | GAME_OLD_OEM2;
    if (gameflags & GAME_REGISTERED0 != 0 && gameflags & GAME_OLD_CDROM1 != 0)
        || (gameflags & GAME_REGISTERED1 != 0 && gameflags & GAME_OLD_CDROM0 != 0)
        || (gameflags & (GAME_OEM2 | GAME_OLD_OEM2) != 0 && gameflags & (registered_bits | demos) != 0)
        || (gameflags & (GAME_REGISTERED1 | GAME_OLD_CDROM1) != 0
            && gameflags & (demos | oems) != 0)
    {
        Sys_Error(c"Bad Hexen II installation: mixed data from incompatible versions".as_ptr());
    }

    if ENABLE_OLD_DEMO == 0 && (gameflags & GAME_OLD_DEMO) != 0 {
        Sys_Error(c"Old version of Hexen II demo isn't supported".as_ptr());
    }
    if ENABLE_OLD_RETAIL == 0
        && (gameflags & (GAME_OLD_CDROM0 | GAME_OLD_CDROM1 | GAME_OLD_OEM0 | GAME_OLD_OEM2)) != 0
    {
        Sys_Error(c"You must patch your installation with Raven's 1.11 update".as_ptr());
    }

    // finish the base filesystem setup
    if (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) != 0 {
        Cvar_SetROM(c"registered".as_ptr(), c"1".as_ptr());
        CON_Printf(PRINT_TERMONLY, c"Playing the registered version.\n".as_ptr());
    } else if (gameflags & GAME_OEM) != 0 {
        Cvar_SetROM(c"oem".as_ptr(), c"1".as_ptr());
        CON_Printf(
            PRINT_TERMONLY,
            c"Playing the oem (Matrox m3D bundle) version \"Continent of Blackmarsh\"\n"
                .as_ptr(),
        );
    } else if (gameflags & (GAME_DEMO | GAME_OLD_DEMO)) != 0 {
        CON_Printf(PRINT_TERMONLY, c"Playing the demo version.\n".as_ptr());
    } else {
        // No proper Raven data.  The two forms differ only in how they name
        // the fetch helper, and the text is the C's verbatim (uhexen2-49ep).
        #[cfg(windows)]
        Sys_Error(
            c"Unable to find a proper Hexen II installation.\n\nWanted a \"data1\" directory containing pak0.pak, under:\n    %s\n\nIf you own Hexen II (Steam, GOG or the CD), copy that\ninstallation's data1 directory to the path above.\n\nIf you don't, the free three-level 1997 demo works. A\nhelper that downloads and verifies it ships next to this\nexecutable:\n    get_demo.cmd\n\n(double-click it, or run it from cmd). In a source\ncheckout it is scripts\\get_demo.cmd instead."
                .as_ptr(),
            FS_BASEDIR,
        );
        #[cfg(not(windows))]
        Sys_Error(
            c"Unable to find a proper Hexen II installation.\n\nWanted a \"data1\" directory containing pak0.pak, under:\n    %s\n\nIf you own Hexen II (Steam, GOG or the CD), copy that\ninstallation's data1 directory to the path above.\n\nIf you don't, the free three-level 1997 demo works. A\nhelper that downloads and verifies it ships next to this\nexecutable:\n    ./get_demo.sh\n\nIn a source checkout it is scripts/get_demo.sh instead, or\nrun: nix run .#get-demo -- \"%s\""
                .as_ptr(),
            FS_BASEDIR,
            FS_BASEDIR,
        );
    }

    if (gameflags & (GAME_OLD_DEMO | GAME_REGISTERED_OLD | GAME_OLD_OEM)) != 0 {
        CON_Printf(
            PRINT_TERMONLY,
            c"Using old/unsupported, pre-1.11 version pak files.\n".as_ptr(),
        );
    }
    if (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) != 0 {
        if CheckRegistered() != 0 {
            Sys_Error(c"Unable to verify retail version data.".as_ptr());
        }
    }
    if QuakeFS_TargetIsServerOnly() == 0
        && (gameflags & GAME_MODIFIED) != 0
        && (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) == 0
    {
        Sys_Error(
            c"You must have the full version of Hexen II to play modified games".as_ptr(),
        );
    }

    // Mark the end of step 1.  Everything at or below this point is the base
    // game and nothing else; Host_Game_f unwinds here to return to data1.
    // uhexen2-5vb6.
    FS_BASE_NOMP_SEARCHPATHS = FS_SEARCHPATHS;

    // step 2: portals directory (mission pack)
    if QuakeFS_TargetIsH2W() != 0 {
        // hwsv: portals only for a registered install, and -noportals wins
        if COM_CheckParm(c"-noportals".as_ptr()) == 0
            && (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) != 0
        {
            check_portals = 1;
        }
    } else {
        if COM_CheckParm(c"-noportals".as_ptr()) != 0 {
            check_portals = 0;
        } else {
            check_portals = if COM_CheckParm(c"-portals".as_ptr()) != 0
                || COM_CheckParm(c"-missionpack".as_ptr()) != 0
                || COM_CheckParm(c"-h2mp".as_ptr()) != 0
            {
                1
            } else {
                0
            };
            let mut i = COM_CheckParm(c"-game".as_ptr());
            if i != 0 && i < (*host_parms).argc - 1 {
                check_portals = 1;
            }
            i = COM_CheckParm(c"-mod".as_ptr());
            if i != 0 && i < (*host_parms).argc - 1 {
                check_portals = 1;
            }
        }
        if check_portals != 0 && (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) == 0 {
            Sys_Error(
                c"Portal of Praevus requires registered version of Hexen II".as_ptr(),
            );
        }
    }
    // The old-protocol refusal is a non-H2W guard in the C, and the shim
    // answers it: hwsv has no sv_protocol to ask about.
    if QuakeFS_TargetOldProtocolRequest() != 0 && check_portals != 0 {
        Sys_Error(c"Old protocol request not compatible with the Mission Pack".as_ptr());
    }

    if check_portals != 0 {
        let mark = FS_SEARCHPATHS;
        fs_add_game_directory(c"portals".as_ptr(), 1);
        if (gameflags & GAME_PORTALS) == 0 {
            // back out searchpaths from invalid mission pack installations,
            // because the portals directory is reserved for the mission pack
            CON_Printf(
                PRINT_TERMONLY,
                c"Missing or invalid mission pack installation\n".as_ptr(),
            );
            CON_Printf(
                PRINT_NORMAL,
                c"Missing or invalid mission pack installation\n".as_ptr(),
            );

            fs_unwind_searchpaths(mark, 1);
            // back to data1 -- all three, the way Host_Game_f's own
            // reset-to-data1 does it, since leaving fs_gamedir_nopath on
            // "portals" while the path holds only data1 makes the engine
            // misreport which game it is running (uhexen2-1bmj).
            qerr_strlcpy(
                c"FS_Init".as_ptr(),
                3471,
                (&raw mut fs_gamedir_nopath).cast::<c_char>(),
                c"data1".as_ptr(),
                MAX_QPATH,
            );
            FS_MakePath_BUF(
                MAKEPATH_BASEDIR,
                core::ptr::null_mut(),
                (&raw mut FS_GAMEDIR).cast::<c_char>(),
                MAX_OSPATH,
                c"data1".as_ptr(),
            );
            FS_MakePath_BUF(
                MAKEPATH_USERBASE,
                core::ptr::null_mut(),
                (&raw mut FS_USERDIR).cast::<c_char>(),
                MAX_OSPATH,
                c"data1".as_ptr(),
            );
        }
        // nothing to do on success: the portals entry stays where
        // FS_AddGameDirectory put it, below fs_base_searchpaths.
    }

    // step 3: hw directory (hexenworld)
    if QuakeFS_TargetIsH2W() != 0 {
        fs_add_game_directory(c"hw".as_ptr(), 1);
        if (gameflags & GAME_HEXENWORLD) == 0 {
            Sys_Error(c"You must have the HexenWorld data installed".as_ptr());
        }
        // hw is added ABOVE portals, so one pointer cannot mark a base that
        // keeps hw and drops the mission pack; HexenWorld never rolls back to
        // data1 at runtime, so the two marks collapse.
        FS_BASE_NOMP_SEARCHPATHS = FS_SEARCHPATHS;
    }

    // this is the end of our base searchpath: any gamedirs set later, from
    // -game, exec'd configs or the server, are freed up to here.
    FS_BASE_SEARCHPATHS = FS_SEARCHPATHS;

    let mut i = COM_CheckParm(c"-game".as_ptr());
    if i == 0 {
        i = COM_CheckParm(c"-mod".as_ptr());
    }
    if i != 0 {
        // only registered versions can do -game/-mod
        if (gameflags & (GAME_REGISTERED | GAME_REGISTERED_OLD)) == 0 {
            Sys_Error(
                c"You must have the full version of Hexen II to play modified games".as_ptr(),
            );
        }
        // add basedir/gamedir as an override game.  Portals sits below it
        // already (step 2 put it there), and it is deliberately not re-added
        // when step 2 rolled it back: that rollback means the mission pack
        // failed validation, and putting the directory back would serve a
        // broken install's files to the mod anyway (uhexen2-6h8x/ofgb).
        if i < (*host_parms).argc - 1 {
            FS_Gamedir(*(*host_parms).argv.add((i + 1) as usize));
        }
    }
}

//============================================================================
// what cannot come across, and where it goes instead
//============================================================================
// `FS_MakePath_VA` (quakefs.h:239) and `FS_MakePath_VABUF` (:242) are the
// file's only two C-variadic exports, with thirteen call sites in host.c,
// host_cmd.c, menu.c, server/host_cmd.c and sv_ccmds.c.  Rust cannot *define* a
// C-variadic function on stable -- `c_variadic` is unstable, and defining one
// contradicts the crate's no-nightly, no-dependency rule -- so those two entry
// points stay in C, in a small shim beside engine/rust/quakefs_target.c that
// formats with `q_vsnprintf` and calls the non-variadic core this module owns
// (`FS_MakePath` / `FS_MakePath_BUF`).  The same applies to the internal
// `FSERR_MakePath_VABUF`, whose two callers can format through `q_snprintf`
// themselves instead, so only the exported pair needs the shim.
//
// Nothing else in the file is variadic, so this is a bounded exception rather
// than a second implementation: the path *logic* -- which base directory, the
// separator, the truncation diagnostic -- is still this module's.

//============================================================================
// the loader group
//============================================================================

/// `FS_FOpen` -- `fopen`, except on Windows where the path is UTF-8 and the CRT
/// wants UTF-16 (quakefs.c:38-56).
unsafe fn fs_fopen(path: *const c_char, mode: *const c_char) -> *mut LibcFile {
    #[cfg(windows)]
    {
        let mut widepath = [0u16; MAX_PATH];
        let mut widemode = [0u16; 16];

        let pathlen = MultiByteToWideChar(
            CP_UTF8,
            0,
            path,
            -1,
            widepath.as_mut_ptr(),
            MAX_PATH as c_int,
        );
        let modelen = MultiByteToWideChar(
            CP_UTF8,
            0,
            mode,
            -1,
            widemode.as_mut_ptr(),
            16 as c_int,
        );

        if pathlen == 0 || modelen == 0 {
            return core::ptr::null_mut();
        }

        _wfopen(widepath.as_ptr(), widemode.as_ptr())
    }
    #[cfg(not(windows))]
    {
        fopen(path, mode)
    }
}

/// `FS_CompareZipNames` -- the `qsort` comparator the pk3 scan sorts names
/// with.  Reproduced here with the hashinlines because it belongs to the
/// loader half of the file.
#[allow(dead_code)] // used once the directory scan lands
unsafe extern "C" fn fs_compare_zip_names(a: *const c_void, b: *const c_void) -> c_int {
    q_strcasecmp(a.cast::<c_char>(), b.cast::<c_char>())
}

/// The `sys_printf!` sites, written out: `CON_Printf` with `_PRINT_TERMONLY`.
#[inline]
unsafe fn sys_print1(fmt: *const c_char, a1: *const c_char) {
    CON_Printf(PRINT_TERMONLY, fmt, a1);
}

#[inline]
unsafe fn sys_print2i(fmt: *const c_char, a1: *const c_char, a2: c_int) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2);
}

#[inline]
unsafe fn sys_print2u(fmt: *const c_char, a1: *const c_char, a2: c_uint) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2);
}

#[inline]
unsafe fn sys_print2s(fmt: *const c_char, a1: *const c_char, a2: *const c_char) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2);
}

#[inline]
unsafe fn sys_print3(fmt: *const c_char, a1: *const c_char, a2: *const c_char, a3: c_int) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2, a3);
}

#[inline]
unsafe fn sys_print_pakfiles(fmt: *const c_char, a1: *const c_char, a2: c_int, a3: c_int) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2, a3);
}

#[inline]
unsafe fn sys_print_zipfiles(fmt: *const c_char, a1: *const c_char, a2: c_uint, a3: c_int) {
    CON_Printf(PRINT_TERMONLY, fmt, a1, a2, a3);
}

/// `check_known_paks` -- fingerprint the container: a CRC over the pak
/// directory plus its file count, and the gamedir it was shipped in.
unsafe fn check_known_paks(paknum: c_int, numfiles: c_int, crc: u16) -> c_uint {
    if paknum >= MAX_PAKDATA as c_int {
        return GAME_MODIFIED;
    }

    let i = paknum as usize;
    if strcmp((&raw const fs_gamedir_nopath).cast::<c_char>(), PAKDATA[i].dirname.as_ptr()) != 0 {
        return GAME_MODIFIED; // Raven didn't ship like that
    }

    if numfiles != PAKDATA[i].numfiles {
        match paknum {
            0 => {
                if numfiles == DEMO_PAKDATA[0].numfiles && crc == DEMO_PAKDATA[0].crc as u16 {
                    return GAME_DEMO;
                }
                if numfiles == OEM0_PAKDATA[0].numfiles && crc == OEM0_PAKDATA[0].crc as u16 {
                    return GAME_OEM0;
                }
                if numfiles == OLD_PAKDATA[2].numfiles && crc == OLD_PAKDATA[2].crc as u16 {
                    return GAME_OLD_DEMO;
                }
                if numfiles == OLD_PAKDATA[0].numfiles && crc == OLD_PAKDATA[0].crc as u16 {
                    return GAME_OLD_CDROM0;
                }
                if numfiles == OLD_PAKDATA[3].numfiles && crc == OLD_PAKDATA[3].crc as u16 {
                    return GAME_OLD_OEM0;
                }
                return GAME_MODIFIED;
            }
            1 => {
                if numfiles == OLD_PAKDATA[1].numfiles && crc == OLD_PAKDATA[1].crc as u16 {
                    return GAME_OLD_CDROM1;
                }
                return GAME_MODIFIED;
            }
            2 => {
                if numfiles == OLD_PAKDATA[4].numfiles && crc == OLD_PAKDATA[4].crc as u16 {
                    return GAME_OLD_OEM2;
                }
                return GAME_MODIFIED;
            }
            4 => {
                if numfiles == OLD_PAKDATA[5].numfiles && crc == OLD_PAKDATA[5].crc as u16 {
                    return GAME_HEXENWORLD;
                }
                return GAME_MODIFIED;
            }
            _ => return GAME_MODIFIED,
        }
    }

    if crc != PAKDATA[i].crc as u16 {
        return GAME_MODIFIED;
    }

    match paknum {
        0 => GAME_REGISTERED0,
        1 => GAME_REGISTERED1,
        2 => GAME_OEM2,
        3 => GAME_PORTALS,
        4 => GAME_HEXENWORLD,
        _ => GAME_MODIFIED,
    }
}

/// `zip_has_marks` -- does this one archive hold every listed member, each at
/// its exact size?
unsafe fn zip_has_marks(
    zip: *mut ZipPackC,
    marks: &[ContentMarkC],
) -> c_int {
    for mark in marks {
        let mut found = 0;
        let key = hash_generate_key_string(&mut (*zip).hash, mark.name.as_ptr(), 0);
        let mut j = hash_first(&mut (*zip).hash, key);
        while j != -1 {
            let file = (*zip).files.add(j as usize);
            if q_strcasecmp((*file).name.as_ptr(), mark.name.as_ptr()) != 0 {
                j = hash_next(&mut (*zip).hash, j);
                continue;
            }
            // Right name, wrong size is a NO rather than a keep-looking.
            if (*file).filelen != mark.filelen {
                return 0;
            }
            found = 1;
            break;
        }
        if found == 0 {
            return 0;
        }
    }

    1
}

/// `check_known_zip` -- the container-agnostic half of `check_known_paks`:
/// ask what is inside instead of fingerprinting the archive format.
unsafe fn check_known_zip(zip: *mut ZipPackC) -> c_uint {
    let mut flags: c_uint = 0;

    for cd in CONTENTDATA.iter() {
        if strcmp((&raw const fs_gamedir_nopath).cast::<c_char>(), cd.dirname.as_ptr()) != 0 {
            continue; // Raven didn't ship it there
        }
        if zip_has_marks(zip, cd.marks) != 0 {
            flags |= cd.gameflag;
        }
    }

    flags
}

/// `FS_ZipRead` -- miniz's I/O callback, over the same FILE the rest of the
/// file layer uses so that UTF-8 archive paths keep working on Windows.
unsafe extern "C" fn fs_zip_read(
    opaque: *mut c_void,
    ofs: u64,
    buf: *mut c_void,
    n: usize,
) -> usize {
    let f = opaque as *mut LibcFile;

    if fseek(f, ofs as c_long, SEEK_SET) != 0 {
        return 0;
    }
    fread(buf, 1, n, f)
}

/// `FS_ZipDataOffset` -- resolve where a STORED entry's bytes start, from the
/// local header rather than the central directory.
unsafe fn fs_zip_data_offset(zip: *mut ZipPackC, mut localhdr: u64) -> c_int {
    /// `ZIP_LOCALHDR_SIZE` from quakefs.c.
    const ZIP_LOCALHDR_SIZE: usize = 30;
    /// `ZIP_LOCALHDR_SIG` from quakefs.c.
    const ZIP_LOCALHDR_SIG: c_uint = 0x0403_4b50;

    let mut hdr = [0u8; ZIP_LOCALHDR_SIZE];

    if fseek((*zip).handle, localhdr as c_long, SEEK_SET) != 0 {
        return -1;
    }
    if fread(
        hdr.as_mut_ptr().cast::<c_void>(),
        1,
        ZIP_LOCALHDR_SIZE,
        (*zip).handle,
    ) != ZIP_LOCALHDR_SIZE
    {
        return -1;
    }

    // little-endian on the wire regardless of host byte order
    let sig = hdr[0] as c_uint
        | (hdr[1] as c_uint) << 8
        | (hdr[2] as c_uint) << 16
        | (hdr[3] as c_uint) << 24;
    if sig != ZIP_LOCALHDR_SIG {
        return -1;
    }

    let namelen = hdr[26] as c_uint | (hdr[27] as c_uint) << 8;
    let extralen = hdr[28] as c_uint | (hdr[29] as c_uint) << 8;

    localhdr += ZIP_LOCALHDR_SIZE as u64 + namelen as u64 + extralen as u64;
    if localhdr > 0x7fff_ffffu64 {
        return -1; // past what fseek()/filepos can address; inflate instead
    }

    localhdr as c_int
}

/// `FS_LoadPackFile` -- load a pak's header and directory into a new
/// `pack_t`.
unsafe fn fs_load_pack_file(
    packfile: *const c_char,
    paknum: c_int,
    base_fs: c_int,
) -> *mut PackC {
    let mut header: PackHeaderC = core::mem::zeroed();
    let mut info = [PackFileC {
        name: [0; PAK_PATH_LENGTH],
        filepos: 0,
        filelen: 0,
    }; MAX_FILES_IN_PACK as usize];

    let packhandle = fs_fopen(packfile, c"rb".as_ptr());
    if packhandle.is_null() {
        return core::ptr::null_mut();
    }

    fread(
        (&mut header as *mut PackHeaderC).cast::<c_void>(),
        1,
        core::mem::size_of::<PackHeaderC>(),
        packhandle,
    );
    if header.id[0] != b'P' as c_char
        || header.id[1] != b'A' as c_char
        || header.id[2] != b'C' as c_char
        || header.id[3] != b'K' as c_char
    {
        sys_print1(c"WARNING: %s is not a packfile, ignored\n".as_ptr(), packfile);
        fclose(packhandle);
        return core::ptr::null_mut();
    }

    header.dirofs = little_long(header.dirofs);
    header.dirlen = little_long(header.dirlen);

    let numpackfiles = header.dirlen / core::mem::size_of::<PackFileC>() as c_int;

    if header.dirlen < 0 || header.dirofs < 0 {
        Sys_Error(
            c"Invalid packfile %s (dirlen: %i, dirofs: %i)".as_ptr(),
            packfile,
            header.dirlen,
            header.dirofs,
        );
    }
    if numpackfiles == 0 {
        sys_print1(c"WARNING: %s has no files, ignored\n".as_ptr(), packfile);
        fclose(packhandle);
        return core::ptr::null_mut();
    }
    if numpackfiles > MAX_FILES_IN_PACK {
        sys_print_pakfiles(
            c"WARNING: %s has %i files (max. allowed is %i), ignored\n".as_ptr(),
            packfile,
            numpackfiles,
            MAX_FILES_IN_PACK,
        );
        fclose(packhandle);
        return core::ptr::null_mut();
    }

    // malloc, NOT Z_Malloc: one full pak's directory is 7% of the client's
    // 2 MB zone and a mod shipping a dozen would exhaust it (uhexen2-mm4l).
    let newfiles = malloc(numpackfiles as usize * core::mem::size_of::<PakFilesC>()).cast::<PakFilesC>();
    if newfiles.is_null() {
        Sys_Error(
            c"%s: out of memory for %i pak entries".as_ptr(),
            c"FS_LoadPackFile".as_ptr(),
            numpackfiles,
        );
    }

    fseek(packhandle, header.dirofs as c_long, SEEK_SET);
    fread(
        info.as_mut_ptr().cast::<c_void>(),
        1,
        header.dirlen as usize,
        packhandle,
    );

    // crc the directory
    let mut crc: u16 = 0;
    CRC_Init(&mut crc);
    for i in 0..header.dirlen as usize {
        CRC_ProcessByte(&mut crc, info.as_ptr().cast::<u8>().add(i).read());
    }

    // check for modifications
    if base_fs != 0 {
        gameflags |= check_known_paks(paknum, numpackfiles, crc);
    } else {
        gameflags |= GAME_MODIFIED;
    }

    let pack = Z_Malloc(core::mem::size_of::<PackC>() as c_int, Z_MAINZONE).cast::<PackC>();
    // get the hash table size from the number of files in the pak
    let mut i: c_int = 1;
    while i < MAX_FILES_IN_PACK {
        if i > numpackfiles {
            break;
        }
        i <<= 1;
    }
    Hash_Allocate(&mut (*pack).hash, i);

    // parse the directory
    for n in 0..numpackfiles as usize {
        qerr_strlcpy(
            c"FS_LoadPackFile".as_ptr(),
            604,
            (*newfiles.add(n)).name.as_mut_ptr(),
            info[n].name.as_ptr(),
            MAX_QPATH,
        );
        (*newfiles.add(n)).filepos = little_long(info[n].filepos);
        (*newfiles.add(n)).filelen = little_long(info[n].filelen);
        let key = hash_generate_key_string(
            &mut (*pack).hash,
            (*newfiles.add(n)).name.as_ptr(),
            0,
        );
        Hash_Add(&mut (*pack).hash, key, n as c_int);
    }

    qerr_strlcpy(
        c"FS_LoadPackFile".as_ptr(),
        611,
        (*pack).filename.as_mut_ptr(),
        packfile,
        MAX_OSPATH,
    );
    (*pack).handle = packhandle;
    (*pack).numfiles = numpackfiles;
    (*pack).files = newfiles;

    sys_print2i(
        c"Added packfile %s (%i files)\n".as_ptr(),
        packfile,
        numpackfiles,
    );
    pack
}

/// `FS_LoadZipFile` -- mount a .pk3/.zip as a searchpath entry.  Returns NULL
/// on anything malformed: these are optional content, so a bad archive costs a
/// warning rather than a Sys_Error.
unsafe fn fs_load_zip_file(zipfile: *const c_char, base_fs: c_int) -> *mut ZipPackC {
    let handle = fs_fopen(zipfile, c"rb".as_ptr());
    if handle.is_null() {
        return core::ptr::null_mut();
    }

    if fseek(handle, 0, SEEK_END) != 0 {
        fclose(handle);
        return core::ptr::null_mut();
    }
    let filesize = ftell(handle);
    if filesize <= 0 {
        fclose(handle);
        return core::ptr::null_mut();
    }

    let zip = Z_Malloc(core::mem::size_of::<ZipPackC>() as c_int, Z_MAINZONE).cast::<ZipPackC>();
    memset(zip.cast::<c_void>(), 0, core::mem::size_of::<ZipPackC>());
    (*zip).handle = handle;
    (*zip).archive.m_pRead = Some(fs_zip_read);
    (*zip).archive.m_pIO_opaque = handle;

    if mz_zip_reader_init(&mut (*zip).archive, filesize as u64, 0) == 0 {
        sys_print1(
            c"WARNING: %s is not a valid zip archive, ignored\n".as_ptr(),
            zipfile,
        );
        Z_Free(zip.cast::<c_void>());
        fclose(handle);
        return core::ptr::null_mut();
    }

    // mz_zip_reader_get_num_files() sits inside a block Ironwail disabled;
    // m_total_files is the public field it would have returned.
    let numentries = (*zip).archive.m_total_files;
    if numentries == 0 {
        sys_print1(c"WARNING: %s has no files, ignored\n".as_ptr(), zipfile);
        mz_zip_reader_end(&mut (*zip).archive);
        Z_Free(zip.cast::<c_void>());
        fclose(handle);
        return core::ptr::null_mut();
    }
    if numentries > MAX_FILES_IN_ZIP as u32 {
        sys_print_zipfiles(
            c"WARNING: %s has %u files (max. allowed is %i), ignored\n".as_ptr(),
            zipfile,
            numentries,
            MAX_FILES_IN_ZIP,
        );
        mz_zip_reader_end(&mut (*zip).archive);
        Z_Free(zip.cast::<c_void>());
        fclose(handle);
        return core::ptr::null_mut();
    }

    // malloc, NOT Z_Malloc: MAX_FILES_IN_ZIP entries is 4.75 MB against a
    // fixed 2 MB zone, and the searchpath holds every mounted archive at once.
    let newfiles = malloc(numentries as usize * core::mem::size_of::<ZipFilesC>()).cast::<ZipFilesC>();
    if newfiles.is_null() {
        Sys_Error(
            c"%s: out of memory for %u zip entries".as_ptr(),
            c"FS_LoadZipFile".as_ptr(),
            numentries,
        );
    }

    // smallest power of two that covers the entry count, floor of 16
    let mut hashsize: c_int = 16;
    while (hashsize as u32) < numentries {
        hashsize <<= 1;
    }
    Hash_Allocate(&mut (*zip).hash, hashsize);

    let mut numfiles: c_int = 0;
    let mut i: u32 = 0;
    while i < numentries {
        let mut stat: MzZipArchiveFileStat = core::mem::zeroed();

        if mz_zip_reader_file_stat(&mut (*zip).archive, i, &mut stat) == 0 {
            i += 1;
            continue;
        }
        if stat.m_is_directory != 0 {
            i += 1;
            continue;
        }
        if stat.m_is_supported == 0 {
            // encrypted, or a compression method miniz cannot decode
            sys_print2s(
                c"WARNING: %s: unsupported entry %s, skipped\n".as_ptr(),
                zipfile,
                stat.m_filename.as_ptr(),
            );
            i += 1;
            continue;
        }
        if stat.m_uncomp_size > 0x7fff_ffffu64 {
            sys_print2s(
                c"WARNING: %s: entry %s too large, skipped\n".as_ptr(),
                zipfile,
                stat.m_filename.as_ptr(),
            );
            i += 1;
            continue;
        }
        if stat.m_method != 0 && stat.m_uncomp_size > MAX_ZIP_INFLATE as u64 {
            let mb = ((stat.m_uncomp_size + (1 << 20) - 1) >> 20) as c_uint;
            let limit = (MAX_ZIP_INFLATE >> 20) as c_uint;
            CON_Printf(
                PRINT_TERMONLY,
                c"WARNING: %s: deflated entry %s inflates to %u MB, over the %u MB limit -- skipped\n"
                    .as_ptr(),
                zipfile,
                stat.m_filename.as_ptr(),
                mb,
                limit,
            );
            i += 1;
            continue;
        }
        if strlen(stat.m_filename.as_ptr()) >= MAX_QPATH {
            sys_print2s(
                c"WARNING: %s: entry name too long, skipped: %s\n".as_ptr(),
                zipfile,
                stat.m_filename.as_ptr(),
            );
            i += 1;
            continue;
        }

        let entry = newfiles.add(numfiles as usize);
        q_strlcpy((*entry).name.as_mut_ptr(), stat.m_filename.as_ptr(), MAX_QPATH);
        (*entry).index = i;
        (*entry).filelen = stat.m_uncomp_size as c_int;
        // STORED entries get a real offset resolved on first open; anything
        // else is flagged as needing inflation.  -1 means "not yet resolved".
        (*entry).filepos = if stat.m_method == 0 { -1 } else { -2 };

        let key = hash_generate_key_string(&mut (*zip).hash, (*entry).name.as_ptr(), 0);
        Hash_Add(&mut (*zip).hash, key, numfiles);
        numfiles += 1;
        i += 1;
    }

    if numfiles == 0 {
        sys_print1(
            c"WARNING: %s has no usable files, ignored\n".as_ptr(),
            zipfile,
        );
        Hash_Free(&mut (*zip).hash);
        free(newfiles.cast::<c_void>());
        mz_zip_reader_end(&mut (*zip).archive);
        Z_Free(zip.cast::<c_void>());
        fclose(handle);
        return core::ptr::null_mut();
    }

    q_strlcpy((*zip).filename.as_mut_ptr(), zipfile, MAX_OSPATH);
    (*zip).numfiles = numfiles;
    (*zip).files = newfiles;

    // An archive still counts as content the original game never shipped; what
    // is new is that it can now ALSO be identified as Raven's own content.
    gameflags |= GAME_MODIFIED;
    if base_fs != 0 {
        gameflags |= check_known_zip(zip);
    }

    sys_print2i(c"Added archive %s (%i files)\n".as_ptr(), zipfile, numfiles);
    zip
}

/// `FS_ZipReadEntry` -- inflate a DEFLATED entry into a caller buffer, which
/// must hold `entry->filelen` bytes.
unsafe fn fs_zip_read_entry(zip: *mut ZipPackC, entry: *const ZipFilesC, buf: *mut c_void) -> c_int {
    if (*entry).filelen == 0 {
        return 1; // nothing to do; an empty entry is not an error
    }
    if mz_zip_reader_extract_to_mem(
        &mut (*zip).archive,
        (*entry).index,
        buf,
        (*entry).filelen as usize,
        0,
    ) != 0
    {
        1
    } else {
        0
    }
}

/// `FS_UnwindSearchpaths` -- pop and free every searchpath entry above
/// `mark`, leaving `fs_searchpaths` at `mark`.  Centralised because of
/// `fs_portals_path_id`: an id that outlives the entries it names would hand
/// PR_ShouldSubstituteProgs a stale answer (uhexen2-5vb6).
unsafe fn fs_unwind_searchpaths(mark: *mut SearchPathC, verbose: c_int) {
    while FS_SEARCHPATHS != mark {
        if !(*FS_SEARCHPATHS).pack.is_null() {
            if verbose != 0 {
                sys_print1(
                    c"Removed packfile %s\n".as_ptr(),
                    (*(*FS_SEARCHPATHS).pack).filename.as_ptr(),
                );
            }
            fclose((*(*FS_SEARCHPATHS).pack).handle);
            free((*(*FS_SEARCHPATHS).pack).files.cast::<c_void>());
            Hash_Free(&mut (*(*FS_SEARCHPATHS).pack).hash);
            Z_Free((*FS_SEARCHPATHS).pack.cast::<c_void>());
        } else if !(*FS_SEARCHPATHS).zip.is_null() {
            if verbose != 0 {
                sys_print1(
                    c"Removed archive %s\n".as_ptr(),
                    (*(*FS_SEARCHPATHS).zip).filename.as_ptr(),
                );
            }
            mz_zip_reader_end(&mut (*(*FS_SEARCHPATHS).zip).archive);
            fclose((*(*FS_SEARCHPATHS).zip).handle);
            free((*(*FS_SEARCHPATHS).zip).files.cast::<c_void>());
            Hash_Free(&mut (*(*FS_SEARCHPATHS).zip).hash);
            Z_Free((*FS_SEARCHPATHS).zip.cast::<c_void>());
        } else if verbose != 0 {
            sys_print1(
                c"Removed path %s\n".as_ptr(),
                (*FS_SEARCHPATHS).filename.as_ptr(),
            );
        }
        if FS_PORTALS_PATH_ID != 0 && (*FS_SEARCHPATHS).path_id == FS_PORTALS_PATH_ID {
            FS_PORTALS_PATH_ID = 0;
        }
        let next = (*FS_SEARCHPATHS).next;
        Z_Free(FS_SEARCHPATHS.cast::<c_void>());
        FS_SEARCHPATHS = next;
    }
}

extern "C" {
    /// The zone port's allocator, defined in every target.
    fn Z_Malloc(size: c_int, zone_id: c_int) -> *mut c_void;
    fn Z_Free(ptr: *mut c_void);
    /// `Z_MAINZONE` from engine/h2shared/zone.h.
    /// (Spelled as a value at each call site below.)
    /// The hashindex port's five exported functions.
    fn Hash_Allocate(hi: *mut HashIndexC, hash_size: c_int);
    fn Hash_Free(hi: *mut HashIndexC);
    fn Hash_Add(hi: *mut HashIndexC, key: c_int, index: c_int);
    /// The crc port's byte-at-a-time API, used to fingerprint a pak directory.
    fn CRC_Init(crc: *mut u16);
    fn CRC_ProcessByte(crc: *mut u16, b: u8);
}

/// `Z_MAINZONE` from engine/h2shared/zone.h.
const Z_MAINZONE: c_int = 1 << 0;

// ==== PORT CONTINUES ====
