// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/zone.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
// Copyright (C) 2026 Hexenwail contributors.
//
// zone.c is the engine's memory: one caller-supplied buffer carved three ways.
// The port's shape follows from that, and from the ownership model written
// before it started (history/rust_phase6_zone_ownership.md).
//
// 1. One buffer, three lifetimes.  `Z_*` is a first-fit zone allocator over a
//    linked list of memblocks, `Hunk_*` bumps from the bottom and the top with
//    marks, and `Cache_*` keeps reloadable records in the hunk.  They are not
//    independent: Hunk_AllocName calls Cache_FreeLow and Hunk_HighAllocName
//    calls Cache_FreeHigh, so growing the hunk evicts or *relocates* cache
//    entries -- Cache_Move re-copies an entry and rewrites the caller's
//    cache_user_t.data.
//
// 2. The port owns the structure, not the memory.  The zonelist, the memblock
//    chains, the hunk counters and the cache records are internal state; the
//    buffer they live in belongs to the caller, as does every cache_user_t
//    (embedded in qpic_t, model_t and friends) and every pointer an allocator
//    returns.  Rust never adopts any of them.
//
// 3. Failure never returns.  Twenty-nine Sys_Error sites are the whole error
//    channel; there is no NULL to check except where the C itself returns one
//    (Hunk_HighAllocName on exhaustion, Cache_Check for an empty entry).
//
// 4. The target configuration comes from outside.  One archive serves all three
//    binaries while ZONE_DEFSIZE, the secondary zone, the cache API and the
//    dedicated flag differ between them, so the module asks
//    engine/rust/zone_target.c -- the per-target shim decided on #233 -- rather
//    than assuming.  Two consequences are load-bearing and easy to get wrong:
//    Cache_Init must run even where the cache is unused, or the Cache_FreeLow
//    call sites in Hunk_AllocName would read uninitialised list heads; and the
//    "flush" command is registered only where the target has the cache API,
//    because Cmd_Exists decides whether a cvar name is refused.
//
// Deliberately not ported: the Z_DEBUG_COMMANDS reporting block
// (Hunk_Print, Z_Print, Cache_Print, Memory_Display_f, Zone_Display_f,
// Cache_Display_f, Memory_Stats_f).  Its entry points are the four
// Cmd_AddCommand calls that stand behind `#if Z_DEBUG_COMMANDS`, which is 0 in
// this tree, and every one of those functions is `static` and called from
// nowhere else -- they cannot be reached from the exported API, so the
// differential harness could not compare them even if they were ported.  Z_CheckHeap
// and the PARANOID consistency calls are kept, behind the same constants the C
// uses, because they are reachable from the exported allocators when a build
// turns those knobs on.

use core::ffi::{c_char, c_int, c_uint, c_void};

//============================================================================
// the target configuration, from engine/rust/zone_target.c
//============================================================================

extern "C" {
    /// `ZONE_DEFSIZE` as the target compiled it: 2 MB for the client, 1 MB
    /// under SERVERONLY.
    fn Zone_TargetDefSize() -> c_int;

    /// `SECZONE_SIZE` as the target compiled it: 0x40000 for the client, 0
    /// (the block compiled out) under SERVERONLY.
    fn Zone_TargetSecSize() -> c_int;

    /// Non-zero where the target compiled the cache API in -- and therefore
    /// where the C registers the "flush" command.
    fn Zone_TargetHasCache() -> c_int;

    /// The live dedicated flag.  The client reads its isDedicated variable
    /// (set from -dedicated before Memory_Init runs); the SERVERONLY targets
    /// answer 1, because hwsv's isDedicated is a macro and not a symbol.
    fn Zone_TargetDedicated() -> c_int;
}

//============================================================================
// the engine the port reaches
//============================================================================

extern "C" {
    /// engine/h2shared/common.h, FUNC_NORETURN.
    fn Sys_Error(fmt: *const c_char, ...) -> !;

    /// The real function behind the Con_Printf / Con_DPrintf macros.
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);

    fn Cmd_AddCommand(cmd_name: *const c_char, function: Option<unsafe extern "C" fn()>);

    fn COM_CheckParm(parm: *const c_char) -> c_int;

    /// `quakeparms_t *host_parms` -- see QuakeParmsC below.  The port reads the
    /// command line the way zone.c does, through the `com_argc` / `com_argv`
    /// macros (common.h:92-93), which are `host_parms->argc` and
    /// `host_parms->argv`; there is no com_argc symbol to bind.
    static host_parms: *mut QuakeParmsC;

    /// Now the Rust strlcpy port; called here exactly as the C called it.
    fn q_strlcpy(dst: *mut c_char, src: *const c_char, siz: usize) -> usize;

    fn atoi(s: *const c_char) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}

/// `_PRINT_NORMAL` / `_PRINT_DEVEL` from engine/h2shared/printsys.h.
const PRINT_NORMAL: c_uint = 0;
const PRINT_DEVEL: c_uint = 2;

//============================================================================
// constants
//============================================================================

/// `Z_MAINZONE` / `Z_SECZONE` from engine/h2shared/zone.h.
const Z_MAINZONE: c_int = 1 << 0;
const Z_SECZONE: c_int = 1 << 1;

/// The two per-zone magics.  They are what ties a memblock back to its zone:
/// Z_Free and Z_Realloc find the zone by matching `block->magic` against each
/// `zonelist_t.magic`.
const ZMAGIC: c_int = 0x1d4a11;
const ZMAGIC2: c_int = 0xf382da;

/// `hunk_t.sentinal`.
const HUNK_SENTINAL: c_int = 0x1df001ed;

/// A free fragment smaller than this is not split off; it stays part of the
/// allocated block.
const MINFRAGMENT: c_int = 64;

/// `HUNKNAME_LEN` / `CACHENAME_LEN`.
const HUNKNAME_LEN: usize = 24;
const CACHENAME_LEN: usize = 32;

/// `Z_CHECKHEAP` and `PARANOID` from the C.  Both are 0 in this tree; the
/// checks are ported and kept behind these constants so turning a knob on in
/// the C keeps working here too.
const Z_CHECKHEAP: bool = false;
const PARANOID: bool = false;

//============================================================================
// the structures
//============================================================================

/// `memblock_t` -- the zone's block header.  `pad` is in the C on purpose: it
/// pads the four ints to a 64-bit boundary before the two pointers.
#[repr(C)]
pub struct MemBlockC {
    pub size: c_int, /* including the header and possibly tiny fragments */
    pub tag: c_int,  /* a tag of 0 is a free block */
    pub magic: c_int, /* should be ZMAGIC */
    pub pad: c_int,
    pub next: *mut MemBlockC,
    pub prev: *mut MemBlockC,
}

/// `memzone_t` -- one zone: its total size, the start/end cap of the block
/// list, and the rover the next first-fit search starts from.
#[repr(C)]
pub struct MemZoneC {
    pub size: c_int, /* total bytes malloced, including header */
    pub blocklist: MemBlockC,
    pub rover: *mut MemBlockC,
}

/// `zonelist_t` -- a registered zone.  `name` is the caller's string (the
/// static "MAINZONE" / "SEC_ZONE" names), not a copy.
#[repr(C)]
pub struct ZonelistC {
    pub id: c_int,
    pub magic: c_int,
    pub name: *const c_char,
    pub zone: *mut MemZoneC,
    pub next: *mut ZonelistC,
}

/// `hunk_t` -- a hunk allocation's header.  `size` includes the header.
#[repr(C)]
pub struct HunkC {
    pub sentinal: c_int,
    pub size: c_int,
    pub name: [c_char; HUNKNAME_LEN],
}

/// `cache_user_t` from zone.h -- the caller's handle on a cached object.  The
/// port reads and writes `data` and never allocates or moves the struct.
#[repr(C)]
pub struct CacheUserC {
    pub data: *mut c_void,
}

/// `cache_system_t` -- the cache's own record, stored immediately before the
/// data it describes, so `Cache_Check` reaches it as `c->data - 1`.
#[repr(C)]
pub struct CacheSystemC {
    pub size: c_int, /* including this header */
    pub user: *mut CacheUserC,
    pub name: [c_char; CACHENAME_LEN],
    pub prev: *mut CacheSystemC,
    pub next: *mut CacheSystemC,
    pub lru_prev: *mut CacheSystemC, /* for LRU flushing */
    pub lru_next: *mut CacheSystemC,
}

/// `quakeparms_t` from engine/hexen2/host.h -- the same definition is in
/// hexenworld/server/host.h and hexen2/server/host.h.  Only `argc` and `argv`
/// are read, but the whole struct is spelled out so the fields land where the
/// C compiler puts them.
#[repr(C)]
pub struct QuakeParmsC {
    pub basedir: *const c_char,
    pub userdir: *const c_char,
    pub argc: c_int,
    pub argv: *mut *mut c_char,
    pub membase: *mut c_void,
    pub memsize: c_int,
    pub errstate: c_int,
}

// The layouts are derived from the pointer width rather than written down,
// because the same archive is built for wasm32 where a pointer is four bytes.
const PTR_SIZE: usize = core::mem::size_of::<*mut c_void>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut c_void>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

const _: () = {
    assert!(core::mem::offset_of!(MemBlockC, size) == 0);
    assert!(core::mem::offset_of!(MemBlockC, tag) == 4);
    assert!(core::mem::offset_of!(MemBlockC, magic) == 8);
    assert!(core::mem::offset_of!(MemBlockC, pad) == 12);
    assert!(core::mem::offset_of!(MemBlockC, next) == 16);
    assert!(core::mem::offset_of!(MemBlockC, prev) == 16 + PTR_SIZE);
    assert!(core::mem::size_of::<MemBlockC>() == 16 + 2 * PTR_SIZE);

    assert!(core::mem::offset_of!(MemZoneC, size) == 0);
    assert!(core::mem::offset_of!(MemZoneC, blocklist) == align_up(4, PTR_ALIGN));
    assert!(core::mem::offset_of!(MemZoneC, rover)
        == align_up(4, PTR_ALIGN) + core::mem::size_of::<MemBlockC>());
    assert!(core::mem::size_of::<MemZoneC>()
        == align_up(
            align_up(4, PTR_ALIGN) + core::mem::size_of::<MemBlockC>() + PTR_SIZE,
            PTR_ALIGN
        ));

    assert!(core::mem::offset_of!(ZonelistC, id) == 0);
    assert!(core::mem::offset_of!(ZonelistC, magic) == 4);
    // name follows the two ints, so it is at 8 on every pointer width; the
    // pointers after it are what the width moves.
    assert!(core::mem::offset_of!(ZonelistC, name) == 8);
    assert!(core::mem::offset_of!(ZonelistC, zone) == 8 + PTR_SIZE);
    assert!(core::mem::offset_of!(ZonelistC, next) == 8 + 2 * PTR_SIZE);
    assert!(core::mem::size_of::<ZonelistC>()
        == align_up(8 + 3 * PTR_SIZE, PTR_ALIGN));

    assert!(core::mem::offset_of!(HunkC, sentinal) == 0);
    assert!(core::mem::offset_of!(HunkC, size) == 4);
    assert!(core::mem::offset_of!(HunkC, name) == 8);
    assert!(core::mem::size_of::<HunkC>() == 8 + HUNKNAME_LEN);

    assert!(core::mem::offset_of!(CacheUserC, data) == 0);
    assert!(core::mem::size_of::<CacheUserC>() == PTR_SIZE);

    assert!(core::mem::offset_of!(CacheSystemC, size) == 0);
    assert!(core::mem::offset_of!(CacheSystemC, user) == align_up(4, PTR_ALIGN));
    assert!(core::mem::offset_of!(CacheSystemC, name) == align_up(4, PTR_ALIGN) + PTR_SIZE);
    assert!(core::mem::offset_of!(CacheSystemC, prev)
        == align_up(4, PTR_ALIGN) + PTR_SIZE + CACHENAME_LEN);
    assert!(core::mem::offset_of!(CacheSystemC, next)
        == align_up(4, PTR_ALIGN) + PTR_SIZE + CACHENAME_LEN + PTR_SIZE);
    assert!(core::mem::offset_of!(CacheSystemC, lru_prev)
        == align_up(4, PTR_ALIGN) + PTR_SIZE + CACHENAME_LEN + 2 * PTR_SIZE);
    assert!(core::mem::offset_of!(CacheSystemC, lru_next)
        == align_up(4, PTR_ALIGN) + PTR_SIZE + CACHENAME_LEN + 3 * PTR_SIZE);
    assert!(core::mem::size_of::<CacheSystemC>()
        == align_up(
            align_up(4, PTR_ALIGN) + PTR_SIZE + CACHENAME_LEN + 4 * PTR_SIZE,
            PTR_ALIGN
        ));

    assert!(core::mem::offset_of!(QuakeParmsC, basedir) == 0);
    assert!(core::mem::offset_of!(QuakeParmsC, userdir) == PTR_SIZE);
    assert!(core::mem::offset_of!(QuakeParmsC, argc) == 2 * PTR_SIZE);
    assert!(core::mem::offset_of!(QuakeParmsC, argv) == align_up(2 * PTR_SIZE + 4, PTR_ALIGN));
    assert!(core::mem::offset_of!(QuakeParmsC, membase)
        == align_up(2 * PTR_SIZE + 4, PTR_ALIGN) + PTR_SIZE);
    assert!(core::mem::offset_of!(QuakeParmsC, memsize)
        == align_up(2 * PTR_SIZE + 4, PTR_ALIGN) + 2 * PTR_SIZE);
    assert!(core::mem::offset_of!(QuakeParmsC, errstate)
        == align_up(2 * PTR_SIZE + 4, PTR_ALIGN) + 2 * PTR_SIZE + 4);
    assert!(core::mem::size_of::<QuakeParmsC>()
        == align_up(align_up(2 * PTR_SIZE + 4, PTR_ALIGN) + 2 * PTR_SIZE + 8, PTR_ALIGN));
};

// The layout accessors let engine/rust/tests/abi_layout.c check these against
// the C compiler's own layout on the host and, under node, on wasm32 -- the
// zone structures are private to zone.c and zone.h, so that program copies the
// definitions rather than including them, as it already does for mplane_t.
macro_rules! size_accessor {
    ($name:ident, $ty:ty) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            core::mem::size_of::<$ty>()
        }
    };
}

macro_rules! align_accessor {
    ($name:ident, $ty:ty) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            core::mem::align_of::<$ty>()
        }
    };
}

macro_rules! offset_accessor {
    ($name:ident, $ty:ty, $field:ident) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            core::mem::offset_of!($ty, $field)
        }
    };
}

size_accessor!(MemBlockC_sizeof, MemBlockC);
align_accessor!(MemBlockC_alignof, MemBlockC);
offset_accessor!(MemBlockC_offsetof_size, MemBlockC, size);
offset_accessor!(MemBlockC_offsetof_tag, MemBlockC, tag);
offset_accessor!(MemBlockC_offsetof_magic, MemBlockC, magic);
offset_accessor!(MemBlockC_offsetof_pad, MemBlockC, pad);
offset_accessor!(MemBlockC_offsetof_next, MemBlockC, next);
offset_accessor!(MemBlockC_offsetof_prev, MemBlockC, prev);

size_accessor!(MemZoneC_sizeof, MemZoneC);
offset_accessor!(MemZoneC_offsetof_size, MemZoneC, size);
offset_accessor!(MemZoneC_offsetof_blocklist, MemZoneC, blocklist);
offset_accessor!(MemZoneC_offsetof_rover, MemZoneC, rover);

size_accessor!(ZonelistC_sizeof, ZonelistC);
offset_accessor!(ZonelistC_offsetof_id, ZonelistC, id);
offset_accessor!(ZonelistC_offsetof_magic, ZonelistC, magic);
offset_accessor!(ZonelistC_offsetof_name, ZonelistC, name);
offset_accessor!(ZonelistC_offsetof_zone, ZonelistC, zone);
offset_accessor!(ZonelistC_offsetof_next, ZonelistC, next);

size_accessor!(HunkC_sizeof, HunkC);
offset_accessor!(HunkC_offsetof_sentinal, HunkC, sentinal);
offset_accessor!(HunkC_offsetof_size, HunkC, size);
offset_accessor!(HunkC_offsetof_name, HunkC, name);

size_accessor!(CacheUserC_sizeof, CacheUserC);
offset_accessor!(CacheUserC_offsetof_data, CacheUserC, data);

size_accessor!(CacheSystemC_sizeof, CacheSystemC);
align_accessor!(CacheSystemC_alignof, CacheSystemC);
offset_accessor!(CacheSystemC_offsetof_size, CacheSystemC, size);
offset_accessor!(CacheSystemC_offsetof_user, CacheSystemC, user);
offset_accessor!(CacheSystemC_offsetof_name, CacheSystemC, name);
offset_accessor!(CacheSystemC_offsetof_prev, CacheSystemC, prev);
offset_accessor!(CacheSystemC_offsetof_next, CacheSystemC, next);
offset_accessor!(CacheSystemC_offsetof_lru_prev, CacheSystemC, lru_prev);
offset_accessor!(CacheSystemC_offsetof_lru_next, CacheSystemC, lru_next);

size_accessor!(QuakeParmsC_sizeof, QuakeParmsC);
offset_accessor!(QuakeParmsC_offsetof_basedir, QuakeParmsC, basedir);
offset_accessor!(QuakeParmsC_offsetof_userdir, QuakeParmsC, userdir);
offset_accessor!(QuakeParmsC_offsetof_argc, QuakeParmsC, argc);
offset_accessor!(QuakeParmsC_offsetof_argv, QuakeParmsC, argv);
offset_accessor!(QuakeParmsC_offsetof_membase, QuakeParmsC, membase);
offset_accessor!(QuakeParmsC_offsetof_memsize, QuakeParmsC, memsize);
offset_accessor!(QuakeParmsC_offsetof_errstate, QuakeParmsC, errstate);

//============================================================================
// the ported state
//============================================================================

/// The static "MAINZONE" / "SEC_ZONE" names.  `zonelist_t.name` points at
/// these, exactly as the C's `static char mainzone[]` is pointed at.
static MAINZONE: [c_char; 9] = [b'M' as c_char, b'A' as c_char, b'I' as c_char,
    b'N' as c_char, b'Z' as c_char, b'O' as c_char, b'N' as c_char, b'E' as c_char,
    0];
static SEC_ZONE: [c_char; 9] = [b'S' as c_char, b'E' as c_char, b'C' as c_char,
    b'_' as c_char, b'Z' as c_char, b'O' as c_char, b'N' as c_char, b'E' as c_char,
    0];

static mut ZONELIST: *mut ZonelistC = core::ptr::null_mut();

static mut HUNK_BASE: *mut u8 = core::ptr::null_mut();
static mut HUNK_SIZE: c_int = 0;
static mut HUNK_LOW_USED: c_int = 0;
static mut HUNK_HIGH_USED: c_int = 0;
static mut HUNK_TEMPACTIVE: c_int = 0;
static mut HUNK_TEMPMARK: c_int = 0;

/// `static cache_system_t cache_head` -- the sentinel of both the address-ordered
/// list and the LRU list.  Initialised by Cache_Init in every target, including
/// the ones that never allocate a cache entry (see the module comment).
static mut CACHE_HEAD: CacheSystemC = CacheSystemC {
    size: 0,
    user: core::ptr::null_mut(),
    name: [0; CACHENAME_LEN],
    prev: core::ptr::null_mut(),
    next: core::ptr::null_mut(),
    lru_prev: core::ptr::null_mut(),
    lru_next: core::ptr::null_mut(),
};

//============================================================================
// the zone allocator
//============================================================================

/// `Z_Free`
#[no_mangle]
pub unsafe extern "C" fn Z_Free(ptr: *mut c_void) {
    if ptr.is_null() {
        Sys_Error(c"%s: NULL pointer".as_ptr(), c"Z_Free".as_ptr());
    }

    let block = (ptr as *mut u8).wrapping_sub(core::mem::size_of::<MemBlockC>())
        .cast::<MemBlockC>();
    if (*block).tag == 0 {
        Sys_Error(c"%s: freed a freed pointer".as_ptr(), c"Z_Free".as_ptr());
    }

    // The zone this block belongs to is identified by the magic the allocator
    // stamped into the block, not by a pointer kept anywhere else.
    let mut z = ZONELIST;
    while !z.is_null() {
        if (*z).magic == (*block).magic {
            break;
        }
        z = (*z).next;
    }
    if z.is_null() {
        Sys_Error(
            c"%s: freed a pointer without ZMAGIC".as_ptr(),
            c"Z_Free".as_ptr(),
        );
    }

    (*block).tag = 0; /* mark as free */

    let mut block = block;
    let mut other = (*block).prev;
    if (*other).tag == 0 {
        /* merge with previous free block */
        (*other).size += (*block).size;
        (*other).next = (*block).next;
        (*(*other).next).prev = other;
        if block == (*(*z).zone).rover {
            (*(*z).zone).rover = other;
        }
        block = other;
    }

    other = (*block).next;
    if (*other).tag == 0 {
        /* merge the next free block onto the end */
        (*block).size += (*other).size;
        (*block).next = (*other).next;
        (*(*block).next).prev = block;
        if other == (*(*z).zone).rover {
            (*(*z).zone).rover = block;
        }
    }
}

/// `Z_TagMalloc` -- first fit from the rover, splitting off a free fragment
/// when the remainder is worth keeping.
unsafe fn z_tag_malloc(z: *mut ZonelistC, size: c_int, tag: c_int) -> *mut c_void {
    if tag == 0 {
        Sys_Error(
            c"%s: tried to use a 0 tag".as_ptr(),
            c"Z_TagMalloc".as_ptr(),
        );
    }

    /* account for size of block header, and for the trash tester */
    let mut size = size;
    size += core::mem::size_of::<MemBlockC>() as c_int;
    size += 4;
    size = (size + 7) & !7; /* align to 8-byte boundary */

    let mut base = (*(*z).zone).rover;
    let mut rover = base;
    let start = (*base).prev;

    loop {
        if rover == start {
            /* scaned all the way around the list */
            return core::ptr::null_mut();
        }
        if (*rover).tag != 0 {
            base = (*rover).next;
            rover = base;
        } else {
            rover = (*rover).next;
        }
        if !((*base).tag != 0 || (*base).size < size) {
            break;
        }
    }

    /* found a block big enough */
    let extra = (*base).size - size;
    if extra > MINFRAGMENT {
        /* there will be a free fragment after the allocated block */
        let newblock = (base as *mut u8).wrapping_offset(size as isize).cast::<MemBlockC>();
        (*newblock).size = extra;
        (*newblock).tag = 0; /* free block */
        (*newblock).prev = base;
        (*newblock).magic = (*z).magic;
        (*newblock).next = (*base).next;
        (*(*newblock).next).prev = newblock;
        (*base).next = newblock;
        (*base).size = size;
    }

    (*base).tag = tag; /* no longer a free block */
    (*(*z).zone).rover = (*base).next; /* next allocation starts looking here */
    (*base).magic = (*z).magic;

    /* marker for memory trash testing */
    core::ptr::write_unaligned(
        (base as *mut u8)
            .wrapping_offset((*base).size as isize)
            .wrapping_offset(-4)
            .cast::<c_int>(),
        (*z).magic,
    );

    (base as *mut u8)
        .wrapping_offset(core::mem::size_of::<MemBlockC>() as isize)
        .cast::<c_void>()
}

/// `Z_CheckHeap`, compiled behind the C's `Z_CHECKHEAP` knob.
unsafe fn z_check_heap(zone: *mut MemZoneC) {
    let mut block = (*zone).blocklist.next;

    loop {
        if (*block).next == &raw mut (*zone).blocklist {
            break; /* all blocks have been hit */
        }
        if (block as *mut u8).wrapping_offset((*block).size as isize) != (*block).next as *mut u8 {
            Sys_Error(
                c"%s: block size does not touch the next block".as_ptr(),
                c"Z_CheckHeap".as_ptr(),
            );
        }
        if (*(*block).next).prev != block {
            Sys_Error(
                c"%s: next block doesn't have proper back link".as_ptr(),
                c"Z_CheckHeap".as_ptr(),
            );
        }
        if (*block).tag == 0 && (*(*block).next).tag == 0 {
            Sys_Error(
                c"%s: two consecutive free blocks".as_ptr(),
                c"Z_CheckHeap".as_ptr(),
            );
        }
        block = (*block).next;
    }
}

/// `Z_Malloc` -- returns zero-filled memory from the zone matching `zone_id`.
#[no_mangle]
pub unsafe extern "C" fn Z_Malloc(size: c_int, zone_id: c_int) -> *mut c_void {
    let mut z = ZONELIST;
    while !z.is_null() {
        if (*z).id & zone_id != 0 {
            break;
        }
        z = (*z).next;
    }
    if z.is_null() {
        Sys_Error(
            c"%s: Bad zone id %i".as_ptr(),
            c"Z_Malloc".as_ptr(),
            zone_id,
        );
    }

    if Z_CHECKHEAP {
        z_check_heap((*z).zone);
    }

    let buf = z_tag_malloc(z, size, 1);
    if buf.is_null() {
        Sys_Error(
            c"%s: failed on allocation of %i bytes".as_ptr(),
            c"Z_Malloc".as_ptr(),
            size,
        );
    }
    memset(buf, 0, size as usize);

    buf
}

/// `Z_Realloc` -- the C is free-plus-realloc, not realloc: it frees the old
/// pointer, allocates fresh, and moves what fits.  The returned pointer is not
/// required to be the same one.
#[no_mangle]
pub unsafe extern "C" fn Z_Realloc(ptr: *mut c_void, size: c_int, zone_id: c_int) -> *mut c_void {
    if ptr.is_null() {
        return Z_Malloc(size, zone_id);
    }

    let block = (ptr as *mut u8).wrapping_sub(core::mem::size_of::<MemBlockC>())
        .cast::<MemBlockC>();
    if (*block).tag == 0 {
        Sys_Error(
            c"%s: realloced a freed pointer".as_ptr(),
            c"Z_Realloc".as_ptr(),
        );
    }

    let mut z = ZONELIST;
    while !z.is_null() {
        if (*z).magic == (*block).magic {
            break;
        }
        z = (*z).next;
    }
    if z.is_null() {
        Sys_Error(
            c"%s: realloced a pointer without ZMAGIC".as_ptr(),
            c"Z_Realloc".as_ptr(),
        );
    }

    /* see Z_TagMalloc(): the usable size is the block size less the header
     * and the trash tester */
    let old_size = (*block).size - (4 + core::mem::size_of::<MemBlockC>() as c_int);
    let old_ptr = ptr;

    Z_Free(ptr);

    let mut z = ZONELIST;
    while !z.is_null() {
        if (*z).id & zone_id != 0 {
            break;
        }
        z = (*z).next;
    }
    if z.is_null() {
        Sys_Error(
            c"%s: Bad zone id %i".as_ptr(),
            c"Z_Realloc".as_ptr(),
            zone_id,
        );
    }

    let ptr = z_tag_malloc(z, size, 1);
    if ptr.is_null() {
        Sys_Error(
            c"%s: failed on allocation of %i bytes".as_ptr(),
            c"Z_Realloc".as_ptr(),
            size,
        );
    }

    if ptr != old_ptr {
        memmove(ptr, old_ptr, core::cmp::min(old_size, size) as usize);
    }
    if old_size < size {
        memset(
            (ptr as *mut u8).wrapping_offset(old_size as isize).cast::<c_void>(),
            0,
            (size - old_size) as usize,
        );
    }

    ptr
}

/// `Z_Strdup`
#[no_mangle]
pub unsafe extern "C" fn Z_Strdup(s: *const c_char) -> *mut c_char {
    let sz = strlen(s) + 1;
    let ptr = Z_Malloc(sz as c_int, Z_MAINZONE).cast::<c_char>();
    memcpy(ptr.cast::<c_void>(), s.cast::<c_void>(), sz);
    ptr
}

//============================================================================
// the hunk
//============================================================================

/// `Hunk_Check` -- walk the low hunk and check every sentinel and size.
#[no_mangle]
pub unsafe extern "C" fn Hunk_Check() {
    let mut h = HUNK_BASE.cast::<HunkC>();

    while h as *mut u8 != HUNK_BASE.wrapping_offset(HUNK_LOW_USED as isize) {
        if (*h).sentinal != HUNK_SENTINAL {
            Sys_Error(
                c"%s: trashed sentinal".as_ptr(),
                c"Hunk_Check".as_ptr(),
            );
        }
        if (*h).size < core::mem::size_of::<HunkC>() as c_int
            || (*h).size as isize + (h as *mut u8).offset_from(HUNK_BASE) > HUNK_SIZE as isize
        {
            Sys_Error(c"%s: bad size".as_ptr(), c"Hunk_Check".as_ptr());
        }
        h = (h as *mut u8).wrapping_offset((*h).size as isize).cast::<HunkC>();
    }
}

/// `Hunk_AllocName` -- low-hunk allocation, aborts when it does not fit.
#[no_mangle]
pub unsafe extern "C" fn Hunk_AllocName(size: c_int, name: *const c_char) -> *mut c_void {
    if PARANOID {
        Hunk_Check();
    }

    if size < 0 {
        Sys_Error(
            c"%s: bad size: %i for %s".as_ptr(),
            c"Hunk_AllocName".as_ptr(),
            size,
            name,
        );
    }

    let size = core::mem::size_of::<HunkC>() as c_int + ((size + 15) & !15);

    if HUNK_SIZE - HUNK_LOW_USED - HUNK_HIGH_USED < size {
        Sys_Error(
            c"%s: failed on %i bytes for %s".as_ptr(),
            c"Hunk_AllocName".as_ptr(),
            size,
            name,
        );
    }

    let h = HUNK_BASE.wrapping_offset(HUNK_LOW_USED as isize).cast::<HunkC>();
    HUNK_LOW_USED += size;

    cache_free_low(HUNK_LOW_USED);

    memset(h.cast::<c_void>(), 0, size as usize);

    (*h).size = size;
    (*h).sentinal = HUNK_SENTINAL;
    q_strlcpy((*h).name.as_mut_ptr(), name, HUNKNAME_LEN);

    h.wrapping_offset(1).cast::<c_void>()
}

/// `Hunk_Alloc` -- the unnamed form.
#[no_mangle]
pub unsafe extern "C" fn Hunk_Alloc(size: c_int) -> *mut c_void {
    Hunk_AllocName(size, c"unknown".as_ptr())
}

/// `Hunk_LowMark`
#[no_mangle]
pub unsafe extern "C" fn Hunk_LowMark() -> c_int {
    HUNK_LOW_USED
}

/// `Hunk_FreeToLowMark` -- note that the C zeroes the released region.
#[no_mangle]
pub unsafe extern "C" fn Hunk_FreeToLowMark(mark: c_int) {
    if mark < 0 || mark > HUNK_LOW_USED {
        Sys_Error(
            c"%s: bad mark %i".as_ptr(),
            c"Hunk_FreeToLowMark".as_ptr(),
            mark,
        );
    }
    memset(
        HUNK_BASE.wrapping_offset(mark as isize).cast::<c_void>(),
        0,
        (HUNK_LOW_USED - mark) as usize,
    );
    HUNK_LOW_USED = mark;
}

/// `Hunk_HighMark`
#[no_mangle]
pub unsafe extern "C" fn Hunk_HighMark() -> c_int {
    if HUNK_TEMPACTIVE != 0 {
        HUNK_TEMPACTIVE = 0;
        Hunk_FreeToHighMark(HUNK_TEMPMARK);
    }

    HUNK_HIGH_USED
}

/// `Hunk_FreeToHighMark`
#[no_mangle]
pub unsafe extern "C" fn Hunk_FreeToHighMark(mark: c_int) {
    if HUNK_TEMPACTIVE != 0 {
        HUNK_TEMPACTIVE = 0;
        Hunk_FreeToHighMark(HUNK_TEMPMARK);
    }
    if mark < 0 || mark > HUNK_HIGH_USED {
        Sys_Error(
            c"%s: bad mark %i".as_ptr(),
            c"Hunk_FreeToHighMark".as_ptr(),
            mark,
        );
    }
    memset(
        HUNK_BASE
            .wrapping_offset((HUNK_SIZE - HUNK_HIGH_USED) as isize)
            .cast::<c_void>(),
        0,
        (HUNK_HIGH_USED - mark) as usize,
    );
    HUNK_HIGH_USED = mark;
}

/// `Hunk_HighAllocName` -- high-hunk allocation.  Unlike the low-hunk form it
/// reports failure by returning NULL, and that asymmetry is part of the
/// contract.
#[no_mangle]
pub unsafe extern "C" fn Hunk_HighAllocName(size: c_int, name: *const c_char) -> *mut c_void {
    if size < 0 {
        Sys_Error(
            c"%s: bad size: %i".as_ptr(),
            c"Hunk_HighAllocName".as_ptr(),
            size,
        );
    }

    if HUNK_TEMPACTIVE != 0 {
        Hunk_FreeToHighMark(HUNK_TEMPMARK);
        HUNK_TEMPACTIVE = 0;
    }

    if PARANOID {
        Hunk_Check();
    }

    let size = core::mem::size_of::<HunkC>() as c_int + ((size + 15) & !15);

    if HUNK_SIZE - HUNK_LOW_USED - HUNK_HIGH_USED < size {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: failed on %i bytes\n".as_ptr(),
            c"Hunk_HighAllocName".as_ptr(),
            size,
        );
        return core::ptr::null_mut();
    }

    HUNK_HIGH_USED += size;
    cache_free_high(HUNK_HIGH_USED);

    let h = HUNK_BASE
        .wrapping_offset((HUNK_SIZE - HUNK_HIGH_USED) as isize)
        .cast::<HunkC>();

    memset(h.cast::<c_void>(), 0, size as usize);
    (*h).size = size;
    (*h).sentinal = HUNK_SENTINAL;
    q_strlcpy((*h).name.as_mut_ptr(), name, HUNKNAME_LEN);

    h.wrapping_offset(1).cast::<c_void>()
}

/// `Hunk_TempAlloc` -- space from the top of the hunk, released by the next
/// call or by any high-mark operation.
#[no_mangle]
pub unsafe extern "C" fn Hunk_TempAlloc(size: c_int) -> *mut c_void {
    let size = (size + 15) & !15;

    if HUNK_TEMPACTIVE != 0 {
        Hunk_FreeToHighMark(HUNK_TEMPMARK);
        HUNK_TEMPACTIVE = 0;
    }

    HUNK_TEMPMARK = Hunk_HighMark();

    let buf = Hunk_HighAllocName(size, c"temp".as_ptr());

    HUNK_TEMPACTIVE = 1;

    buf
}

/// `Hunk_Strdup`
#[no_mangle]
pub unsafe extern "C" fn Hunk_Strdup(s: *const c_char, name: *const c_char) -> *mut c_char {
    let sz = strlen(s) + 1;
    let ptr = Hunk_AllocName(sz as c_int, name).cast::<c_char>();
    memcpy(ptr.cast::<c_void>(), s.cast::<c_void>(), sz);
    ptr
}

//============================================================================
// the cache
//============================================================================

/// `Cache_Move` -- relocate an entry, or drop it if it will not fit elsewhere.
/// The caller's cache_user_t.data is rewritten to the new address.
unsafe fn cache_move(c: *mut CacheSystemC) {
    /* we are clearing up space at the bottom, so only allocate it late */
    let new_cs = cache_try_alloc((*c).size, 1);
    if !new_cs.is_null() {
        memcpy(
            new_cs.wrapping_offset(1).cast::<c_void>(),
            c.wrapping_offset(1).cast::<c_void>(),
            ((*c).size as usize) - core::mem::size_of::<CacheSystemC>(),
        );
        (*new_cs).user = (*c).user;
        memcpy(
            (*new_cs).name.as_mut_ptr().cast::<c_void>(),
            (*c).name.as_ptr().cast::<c_void>(),
            CACHENAME_LEN,
        );
        Cache_Free((*c).user);
        (*(*new_cs).user).data = new_cs.wrapping_offset(1).cast::<c_void>();
    } else {
        Cache_Free((*c).user); /* tough luck... */
    }
}

/// `Cache_FreeLow` -- evict or move entries until the low hunk can grow to
/// `new_low_hunk`.  In the SERVERONLY C this is a macro that expands to
/// nothing; here it runs in every target, which is why Cache_Init must have
/// been called even where nothing is ever cached.
unsafe fn cache_free_low(new_low_hunk: c_int) {
    loop {
        let c = CACHE_HEAD.next;
        if c == &raw mut CACHE_HEAD {
            return; /* nothing in cache at all */
        }
        if c as *mut u8 >= HUNK_BASE.wrapping_offset(new_low_hunk as isize) {
            return; /* there is space to grow the hunk */
        }
        cache_move(c); /* reclaim the space */
    }
}

/// `Cache_FreeHigh` -- the same at the top of the hunk.
unsafe fn cache_free_high(new_high_hunk: c_int) {
    let mut prev: *mut CacheSystemC = core::ptr::null_mut();

    loop {
        let c = CACHE_HEAD.prev;
        if c == &raw mut CACHE_HEAD {
            return; /* nothing in cache at all */
        }
        if (c as *mut u8).wrapping_offset((*c).size as isize)
            <= HUNK_BASE.wrapping_offset((HUNK_SIZE - new_high_hunk) as isize)
        {
            return; /* there is space to grow the hunk */
        }
        if c == prev {
            Cache_Free((*c).user); /* didn't move out of the way */
        } else {
            cache_move(c); /* try to move it */
            prev = c;
        }
    }
}

/// `Cache_UnlinkLRU`
unsafe fn cache_unlink_lru(cs: *mut CacheSystemC) {
    if (*cs).lru_next.is_null() || (*cs).lru_prev.is_null() {
        Sys_Error(c"%s: NULL link".as_ptr(), c"Cache_UnlinkLRU".as_ptr());
    }

    (*(*cs).lru_next).lru_prev = (*cs).lru_prev;
    (*(*cs).lru_prev).lru_next = (*cs).lru_next;

    (*cs).lru_prev = core::ptr::null_mut();
    (*cs).lru_next = core::ptr::null_mut();
}

/// `Cache_MakeLRU`
unsafe fn cache_make_lru(cs: *mut CacheSystemC) {
    if !(*cs).lru_next.is_null() || !(*cs).lru_prev.is_null() {
        Sys_Error(c"%s: active link".as_ptr(), c"Cache_MakeLRU".as_ptr());
    }

    (*CACHE_HEAD.lru_next).lru_prev = cs;
    (*cs).lru_next = CACHE_HEAD.lru_next;
    (*cs).lru_prev = &raw mut CACHE_HEAD;
    CACHE_HEAD.lru_next = cs;
}

/// `Cache_TryAlloc` -- find room between the hunk marks for an entry whose
/// size already includes its header and padding.
unsafe fn cache_try_alloc(size: c_int, nobottom: c_int) -> *mut CacheSystemC {
    /* is the cache completely empty? */
    if nobottom == 0 && CACHE_HEAD.prev == &raw mut CACHE_HEAD {
        if HUNK_SIZE - HUNK_HIGH_USED - HUNK_LOW_USED < size {
            Sys_Error(
                c"%s: out of hunk memory (failed to allocate %i bytes)".as_ptr(),
                c"Cache_TryAlloc".as_ptr(),
                size,
            );
        }

        let new_cs = HUNK_BASE
            .wrapping_offset(HUNK_LOW_USED as isize)
            .cast::<CacheSystemC>();
        memset(
            new_cs.cast::<c_void>(),
            0,
            core::mem::size_of::<CacheSystemC>(),
        );
        (*new_cs).size = size;

        CACHE_HEAD.prev = new_cs;
        CACHE_HEAD.next = new_cs;
        (*new_cs).prev = &raw mut CACHE_HEAD;
        (*new_cs).next = &raw mut CACHE_HEAD;

        cache_make_lru(new_cs);
        return new_cs;
    }

    /* search from the bottom up for space */
    let mut new_cs = HUNK_BASE
        .wrapping_offset(HUNK_LOW_USED as isize)
        .cast::<CacheSystemC>();
    let mut cs = CACHE_HEAD.next;

    loop {
        if nobottom == 0 || cs != CACHE_HEAD.next {
            if (cs as *mut u8).offset_from(new_cs as *mut u8) as c_int >= size {
                /* found space */
                memset(
                    new_cs.cast::<c_void>(),
                    0,
                    core::mem::size_of::<CacheSystemC>(),
                );
                (*new_cs).size = size;

                (*new_cs).next = cs;
                (*new_cs).prev = (*cs).prev;
                (*(*cs).prev).next = new_cs;
                (*cs).prev = new_cs;

                cache_make_lru(new_cs);

                return new_cs;
            }
        }

        /* continue looking */
        new_cs = (cs as *mut u8).wrapping_offset((*cs).size as isize).cast::<CacheSystemC>();
        cs = (*cs).next;

        if cs == &raw mut CACHE_HEAD {
            break;
        }
    }

    /* try to allocate one at the very end */
    if (HUNK_BASE
        .wrapping_offset((HUNK_SIZE - HUNK_HIGH_USED) as isize)
        .offset_from(new_cs as *mut u8)) as c_int
        >= size
    {
        memset(
            new_cs.cast::<c_void>(),
            0,
            core::mem::size_of::<CacheSystemC>(),
        );
        (*new_cs).size = size;

        (*new_cs).next = &raw mut CACHE_HEAD;
        (*new_cs).prev = CACHE_HEAD.prev;
        (*CACHE_HEAD.prev).next = new_cs;
        CACHE_HEAD.prev = new_cs;

        cache_make_lru(new_cs);

        return new_cs;
    }

    core::ptr::null_mut() /* couldn't allocate */
}

/// `Cache_Flush`
#[no_mangle]
pub unsafe extern "C" fn Cache_Flush() {
    while CACHE_HEAD.next != &raw mut CACHE_HEAD {
        Cache_Free((*CACHE_HEAD.next).user); /* reclaim the space */
    }
}

/// `Cache_Report`
#[no_mangle]
pub unsafe extern "C" fn Cache_Report() {
    CON_Printf(
        PRINT_DEVEL,
        c"%4.1f megabyte data cache\n".as_ptr(),
        ((HUNK_SIZE - HUNK_HIGH_USED - HUNK_LOW_USED) as f64) / ((1024 * 1024) as f64),
    );
}

/// `Cache_Init` -- must run in every target, cached or not.
unsafe fn cache_init() {
    CACHE_HEAD.next = &raw mut CACHE_HEAD;
    CACHE_HEAD.prev = &raw mut CACHE_HEAD;
    CACHE_HEAD.lru_next = &raw mut CACHE_HEAD;
    CACHE_HEAD.lru_prev = &raw mut CACHE_HEAD;
}

/// `Cache_Free`
#[no_mangle]
pub unsafe extern "C" fn Cache_Free(c: *mut CacheUserC) {
    if (*c).data.is_null() {
        Sys_Error(c"%s: not allocated".as_ptr(), c"Cache_Free".as_ptr());
    }

    let cs = ((*c).data as *mut CacheSystemC).wrapping_sub(1);

    (*(*cs).prev).next = (*cs).next;
    (*(*cs).next).prev = (*cs).prev;
    (*cs).next = core::ptr::null_mut();
    (*cs).prev = core::ptr::null_mut();

    (*c).data = core::ptr::null_mut();

    cache_unlink_lru(cs);
}

/// `Cache_Check` -- the data, or NULL when the entry is not cached; a hit also
/// moves the entry to the head of the LRU list.
#[no_mangle]
pub unsafe extern "C" fn Cache_Check(c: *mut CacheUserC) -> *mut c_void {
    if (*c).data.is_null() {
        return core::ptr::null_mut();
    }

    let cs = ((*c).data as *mut CacheSystemC).wrapping_sub(1);

    /* move to head of LRU */
    cache_unlink_lru(cs);
    cache_make_lru(cs);

    (*c).data
}

/// `Cache_Alloc` -- round the size up, find room (evicting from the LRU tail
/// until something fits), and hand back the data.
#[no_mangle]
pub unsafe extern "C" fn Cache_Alloc(
    c: *mut CacheUserC,
    size: c_int,
    name: *const c_char,
) -> *mut c_void {
    if !(*c).data.is_null() {
        Sys_Error(
            c"%s: %s is already allocated".as_ptr(),
            c"Cache_Alloc".as_ptr(),
            name,
        );
    }

    if size <= 0 {
        Sys_Error(
            c"%s: bad size %i for %s".as_ptr(),
            c"Cache_Alloc".as_ptr(),
            size,
            name,
        );
    }

    let size = (size + core::mem::size_of::<CacheSystemC>() as c_int + 15) & !15;

    /* find memory for it */
    loop {
        let cs = cache_try_alloc(size, 0);
        if !cs.is_null() {
            q_strlcpy((*cs).name.as_mut_ptr(), name, CACHENAME_LEN);
            (*c).data = cs.wrapping_offset(1).cast::<c_void>();
            (*cs).user = c;
            break;
        }

        /* free the least recently used cached data */
        if CACHE_HEAD.lru_prev == &raw mut CACHE_HEAD {
            /* not enough memory at all */
            Sys_Error(c"%s: out of memory".as_ptr(), c"Cache_Alloc".as_ptr());
        }
        Cache_Free((*CACHE_HEAD.lru_prev).user);
    }

    Cache_Check(c)
}

//============================================================================
// initialization
//============================================================================

/// `Memory_InitZone` -- carve one zone out of the hunk and link it in.
unsafe fn memory_init_zone(name: *const c_char, id: c_int, magic: c_int, size: c_int) {
    let z = Hunk_AllocName(core::mem::size_of::<ZonelistC>() as c_int, name)
        .cast::<ZonelistC>();
    (*z).id = id;
    (*z).magic = magic;
    (*z).name = name;
    (*z).zone = Hunk_AllocName(size, name).cast::<MemZoneC>();

    /* set the entire zone to one free block */
    let block = ((*z).zone as *mut u8)
        .wrapping_offset(core::mem::size_of::<MemZoneC>() as isize)
        .cast::<MemBlockC>();
    (*(*z).zone).blocklist.next = block;
    (*(*z).zone).blocklist.prev = block;
    (*(*z).zone).blocklist.tag = 1; /* in use block */
    (*(*z).zone).blocklist.magic = 0;
    (*(*z).zone).blocklist.size = 0;
    (*(*z).zone).rover = block;

    (*block).prev = &raw mut (*(*z).zone).blocklist;
    (*block).next = &raw mut (*(*z).zone).blocklist;
    (*block).tag = 0; /* free block */
    (*block).magic = magic;
    (*block).size = size - core::mem::size_of::<MemZoneC>() as c_int;

    /* add to linked list */
    (*z).next = ZONELIST;
    ZONELIST = z;
}

/// `Memory_Init`
#[no_mangle]
pub unsafe extern "C" fn Memory_Init(buf: *mut c_void, size: c_int) {
    HUNK_BASE = buf.cast::<u8>();
    HUNK_SIZE = size;
    HUNK_LOW_USED = 0;
    HUNK_HIGH_USED = 0;

    /* Every target, including the ones where the cache API is compiled out of
     * the C: Hunk_AllocName calls Cache_FreeLow unconditionally here, and that
     * walks this list.  An empty cache returns on its first test, so the
     * behaviour matches the C's no-op macro. */
    cache_init();

    let mut zonesize = Zone_TargetDefSize();

    let p = COM_CheckParm(c"-zone".as_ptr());
    if p != 0 && p < (*host_parms).argc - 1 {
        zonesize = atoi(*(*host_parms).argv.offset(p as isize + 1)) * 1024;
    }

    memory_init_zone(MAINZONE.as_ptr(), Z_MAINZONE, ZMAGIC, zonesize);

    let secsize = Zone_TargetSecSize();
    if secsize > 0 {
        let mut zonesize = 0;
        if Zone_TargetDedicated() == 0 {
            zonesize += secsize;
        }
        if zonesize > 0 {
            memory_init_zone(SEC_ZONE.as_ptr(), Z_SECZONE, ZMAGIC2, zonesize);
        }
    }

    if Zone_TargetHasCache() != 0 {
        Cmd_AddCommand(c"flush".as_ptr(), Some(Cache_Flush));
    }
}
