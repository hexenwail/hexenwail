// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/hexenworld/shared/info_str.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
//
// Info strings are the backslash-separated key/value lists HexenWorld carries
// as `svs.info` (serverinfo), `localinfo` and each client's `userinfo`.  Three
// properties of the C original shape this port.
//
// 1. The file has two variants in one body, split by `#ifndef SERVERONLY`, and
//    only one of them has ever been compiled.  info_str.c appears in exactly
//    one CMake source list -- HWSV_SOURCES, engine/CMakeLists.txt -- and hwsv
//    is the only target built with SERVERONLY (and H2W).  Every Hexen II use of
//    Info_* sits inside `#if defined(H2W)` (pr_cmds.c, gl_model.c, pr_edict.c,
//    quakefs.c), so glhexen2 and h2ded never reference these symbols at all.
//    The port therefore implements the SERVERONLY variant, and the differential
//    harness compiles the C exactly as hwsv does, so it compares against the
//    code that actually ships rather than against the client variant no binary
//    in this tree contains.
//
//    The client variant differs only in the high-bit filter of
//    Info_SetValueForStarKey: it forces 7-bit characters except for the key
//    "name", lowercases "team" through q_strcasecmp, and needs neither
//    sv_highchars nor the q_ctype helpers.  If a client ever compiles this file
//    again, that branch has to be added here (and the q_strcasecmp/q_tolower
//    imports with it).
//
// 2. `Info_ValueForKey` owns real static state: `static char value[4][512]` and
//    `static int valueindex`.  The index rotates on EVERY call, including the
//    calls that return "" -- the rotation happens before anything is parsed --
//    so the four buffers exist to let two or more returned values be held at
//    once (Info_SetValueForKey's callers compare a value taken before a write
//    with one taken after: sv_main.c:1532, :1565, :1602).  Callers observe the
//    rotation, so it is part of the contract, and the harness compares
//    successive calls rather than a single one.
//
// 3. The C reads the hwsv-only global `sv_highchars` directly.  This module
//    cannot: the consolidated Rust archive is a single object member
//    (libengine_rs.a holds one engine_rs object), so any target that pulls it
//    for any other port -- glhexen2 and h2ded do, for crc/sizebuf/msg_io --
//    inherits every undefined symbol in it, and neither of those two targets
//    defines sv_highchars.  A direct extern static is therefore an immediate
//    link error outside hwsv.  The read goes through Cvar_FindVar instead; see
//    sv_highchars_integer for why that is the same value in every state the
//    engine can reach.
//
// The rest is deliberately literal: statement order, the parse loops, the
// in-place rewrite boundaries (Info_RemoveKey's memmove, Info_RemovePrefixedKeys
// restarting from `start`) and the `> 13` retention are the contract, and the
// harness compares them across successive calls and against the caller's own
// buffer, not just against a return value.

use core::ffi::{c_char, c_float, c_int, c_uint, c_void};

/// C's `pkey[512]` and `value[512]` in the parse loops, and the width of one
/// Info_ValueForKey rotation slot (`static char value[4][512]`).
const MAX_INFO_VALUE: usize = 512;

/// The `> 63` length rejection: keys and values must be < 64 characters.
const MAX_KEYVALUE: usize = 63;

/// `_PRINT_NORMAL` from engine/h2shared/printsys.h -- what the C's Con_Printf
/// macro passes as CON_Printf's first argument.
const PRINT_NORMAL: c_uint = 0;

const BACKSLASH: c_char = b'\\' as c_char;

extern "C" {
    /// The real function behind the `Con_Printf(fmt, ...)` macro: every call
    /// site here passes _PRINT_NORMAL, exactly as the macro expands.
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);

    /// Declared in engine/h2shared/cvar.h, defined in cvar.c, which both
    /// COMMON_SOURCES (glhexen2, h2ded) and HWSV_SOURCES compile -- so unlike
    /// the sv_highchars global it stands in, this symbol exists in every target
    /// that links this archive.
    fn Cvar_FindVar(var_name: *const c_char) -> *mut CvarC;

    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;
}

//============================================================================
// cvar_t
//============================================================================

/// `cvar_t` from engine/h2shared/cvar.h -- the HexenWorld server compiles the
/// same header.  Only `integer` is ever read here; the whole struct is spelled
/// out so the offsets can be asserted here and checked against the real C
/// struct by the differential harness.
#[repr(C)]
pub struct CvarC {
    pub name: *const c_char,
    pub string: *const c_char,
    pub flags: c_uint,
    pub value: c_float,
    pub integer: c_int,
    pub callback: *mut c_void,
    pub next: *mut CvarC,
    pub default_string: *const c_char,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut CvarC>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut CvarC>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

const _: () = {
    assert!(core::mem::offset_of!(CvarC, name) == 0);
    assert!(core::mem::offset_of!(CvarC, string) == PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, flags) == 2 * PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, value) == 2 * PTR_SIZE + 4);
    assert!(core::mem::offset_of!(CvarC, integer) == 2 * PTR_SIZE + 8);
    assert!(core::mem::offset_of!(CvarC, callback)
        == align_up(2 * PTR_SIZE + 12, PTR_ALIGN));
    assert!(core::mem::offset_of!(CvarC, next)
        == align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, default_string)
        == align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + 2 * PTR_SIZE);
    assert!(core::mem::size_of::<CvarC>()
        == align_up(align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + 3 * PTR_SIZE, PTR_ALIGN));
};

// These accessors let the C differential harness check the layout against the
// real cvar_t without restating Rust's assumptions in C, as the sizebuf,
// link_ops and msg_io ports do.
#[no_mangle]
pub extern "C" fn CvarC_sizeof() -> usize {
    core::mem::size_of::<CvarC>()
}

#[no_mangle]
pub extern "C" fn CvarC_alignof() -> usize {
    core::mem::align_of::<CvarC>()
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_name() -> usize {
    core::mem::offset_of!(CvarC, name)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_string() -> usize {
    core::mem::offset_of!(CvarC, string)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_flags() -> usize {
    core::mem::offset_of!(CvarC, flags)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_value() -> usize {
    core::mem::offset_of!(CvarC, value)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_integer() -> usize {
    core::mem::offset_of!(CvarC, integer)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_callback() -> usize {
    core::mem::offset_of!(CvarC, callback)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_next() -> usize {
    core::mem::offset_of!(CvarC, next)
}

#[no_mangle]
pub extern "C" fn CvarC_offsetof_default_string() -> usize {
    core::mem::offset_of!(CvarC, default_string)
}

/// `sv_highchars.integer` from engine/hexenworld/server/sv_main.c, read through
/// the cvar table instead of the global.
///
/// The C reads the global directly and this module cannot (see the module
/// comment).  The two are the same value in every state the engine reaches,
/// and the reason is init ordering, not luck:
///
///   * `cvar_t sv_highchars = {"sv_highchars", "1", CVAR_NONE}` is registered by
///     pointer -- `Cvar_RegisterVariable(&sv_highchars)`, sv_main.c:1393, inside
///     SV_InitLocal.  So Cvar_FindVar("sv_highchars") returns exactly
///     &sv_highchars after that call, and `.integer` is the same field of the
///     same object the C reads.
///   * Registration cannot fail or land on another variable: the two ways
///     Cvar_RegisterVariable bails out are an existing cvar of that name and a
///     command of that name, and everything that could create either -- server
///     configs (`exec server.cfg`) and the command line's +commands -- is
///     stuffed strictly after SV_InitLocal, at sv_main.c:1699 and :1701.
///   * Before registration the two agree as well: Cvar_FindVar returns NULL
///     and the answer is 0, which is what the C global's `.integer` holds --
///     the brace initialiser sets only name, string and flags, so the rest of
///     the struct is zero.  That is the ordering that applies to any Info_ call
///     reached during init, and it is why NULL must map to 0 rather than to
///     "filtering off".
///
/// Cvar_VariableValue is NOT equivalent and is deliberately not used: it
/// returns the float `value`, so "0.5" would give 0.5 there while the C's
/// `.integer` is 0 -- opposite filtering decisions.
///
/// Read once per Info_SetValueForStarKey call rather than per copied byte as
/// the C does: the copy loop calls nothing that could set a cvar, so the value
/// cannot change inside it.
unsafe fn sv_highchars_integer() -> c_int {
    let var = Cvar_FindVar(c"sv_highchars".as_ptr());

    if var.is_null() {
        0
    } else {
        (*var).integer
    }
}

//============================================================================
// value buffers
//============================================================================

/// C: `static char value[4][512]` -- four rotation slots, so the value a caller
/// already holds is not overwritten by the next lookup.
static mut INFO_VALUE: [[c_char; MAX_INFO_VALUE]; 4] = [[0; MAX_INFO_VALUE]; 4];

/// C: `static int valueindex`.  Advanced at the top of every
/// Info_ValueForKey call, including calls that return "".
static mut INFO_VALUE_INDEX: usize = 0;

/// C returns a string literal `""` on every miss; that pointer is a different
/// object from the rotation slot, which is why the harness must compare the
/// text and the pointer's slot, not the address.
const EMPTY: *const c_char = c"".as_ptr();

//============================================================================
// lookup
//============================================================================

/// `Info_ValueForKey`
#[no_mangle]
pub unsafe extern "C" fn Info_ValueForKey(
    s: *const c_char,
    key: *const c_char,
) -> *const c_char {
    let mut pkey = [0 as c_char; MAX_INFO_VALUE];
    let mut s = s;

    // The rotation is unconditional and happens before any parsing, so a miss
    // still consumes a slot.
    INFO_VALUE_INDEX = (INFO_VALUE_INDEX + 1) % 4;

    if *s == BACKSLASH {
        s = s.add(1);
    }

    loop {
        let mut o = pkey.as_mut_ptr();
        while *s != BACKSLASH {
            if *s == 0 {
                return EMPTY;
            }
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;
        s = s.add(1);

        let value = (&raw mut INFO_VALUE[INFO_VALUE_INDEX]).cast::<c_char>();

        // The C repeats `if (!*s) return ""` inside this loop, which the loop
        // condition already covers; there is no other exit.
        let mut o = value;
        while *s != BACKSLASH && *s != 0 {
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;

        if strcmp(key, pkey.as_ptr()) == 0 {
            return value;
        }

        if *s == 0 {
            return EMPTY;
        }
        s = s.add(1);
    }
}

//============================================================================
// removal
//============================================================================

/// `Info_RemoveKey` -- rewrites the caller's buffer in place.
#[no_mangle]
pub unsafe extern "C" fn Info_RemoveKey(s: *mut c_char, key: *const c_char) {
    let mut pkey = [0 as c_char; MAX_INFO_VALUE];
    let mut value = [0 as c_char; MAX_INFO_VALUE];
    let mut s = s;

    if !strstr(key, c"\\".as_ptr()).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Can't use a key with a \\\n".as_ptr(),
        );
        return;
    }

    loop {
        // Where this pair begins: the separator that precedes it, or the
        // pair's own first byte when it is the first pair in the string.  The
        // memmove below starts here, which is why removing a pair also removes
        // the separator before it.
        let start = s;
        if *s == BACKSLASH {
            s = s.add(1);
        }

        let mut o = pkey.as_mut_ptr();
        while *s != BACKSLASH {
            if *s == 0 {
                return;
            }
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;
        s = s.add(1);

        let mut o = value.as_mut_ptr();
        while *s != BACKSLASH && *s != 0 {
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;

        if strcmp(key, pkey.as_ptr()) == 0 {
            // memmove from the separator that preceded the pair (or from the
            // pair itself at the start of the string), NUL included.  This is
            // the whole in-place mutation boundary: everything after the
            // removed pair shifts left, and nothing else in the buffer moves.
            memmove(
                start.cast::<c_void>(),
                s.cast::<c_void>(),
                strlen(s) + 1,
            );
            return;
        }

        if *s == 0 {
            return;
        }
    }
}

/// `Info_RemovePrefixedKeys` -- rewrites the caller's buffer in place, possibly
/// several times: after each removal the walk restarts from `start`.
#[no_mangle]
pub unsafe extern "C" fn Info_RemovePrefixedKeys(start: *mut c_char, prefix: c_char) {
    let mut pkey = [0 as c_char; MAX_INFO_VALUE];
    let mut value = [0 as c_char; MAX_INFO_VALUE];
    let mut s = start;

    loop {
        if *s == BACKSLASH {
            s = s.add(1);
        }

        let mut o = pkey.as_mut_ptr();
        while *s != BACKSLASH {
            if *s == 0 {
                return;
            }
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;
        s = s.add(1);

        let mut o = value.as_mut_ptr();
        while *s != BACKSLASH && *s != 0 {
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;

        if pkey[0] == prefix {
            Info_RemoveKey(start, pkey.as_ptr());
            s = start;
        }

        if *s == 0 {
            return;
        }
    }
}

//============================================================================
// writing
//============================================================================

/// `Info_SetValueForStarKey` -- the SERVERONLY variant, which is the one hwsv
/// compiles.  See the module comment for the client variant's differences.
#[no_mangle]
pub unsafe extern "C" fn Info_SetValueForStarKey(
    s: *mut c_char,
    key: *const c_char,
    value: *const c_char,
    maxsize: usize,
) {
    // C: `char newvalue[1024]`.  Only reached with key and value <= 63 bytes,
    // because of the length rejection below, so it holds "\key\value".
    let mut newvalue = [0u8; 1024];

    if !strstr(key, c"\\".as_ptr()).is_null() || !strstr(value, c"\\".as_ptr()).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Can't use keys or values with a \\\n".as_ptr(),
        );
        return;
    }

    if !strstr(key, c"\"".as_ptr()).is_null() || !strstr(value, c"\"".as_ptr()).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Can't use keys or values with a \"\n".as_ptr(),
        );
        return;
    }

    if strlen(key) > MAX_KEYVALUE || strlen(value) > MAX_KEYVALUE {
        CON_Printf(
            PRINT_NORMAL,
            c"Keys and values must be < 64 characters.\n".as_ptr(),
        );
        return;
    }

    // The removal happens before the empty-value early return: setting a key to
    // "" deletes it.
    Info_RemoveKey(s, key);
    if *value == 0 {
        return;
    }

    // >= maxsize, not > maxsize, for the null termination; +1 for each of key
    // and value for the added `\` characters.  All size_t in the C, so the sum
    // is computed unsigned here too.
    if strlen(key) + 1 + strlen(value) + 1 + strlen(s) >= maxsize {
        CON_Printf(PRINT_NORMAL, c"Info string length exceeded\n".as_ptr());
        return;
    }

    // C: `sprintf (newvalue, "\\%s\\%s", key, value)`.  Keys and values cannot
    // contain `\` at this point -- the check above rejected them -- so this is
    // the same byte string sprintf produces.
    let mut n = 0;
    newvalue[n] = b'\\';
    n += 1;
    let keylen = strlen(key);
    let mut i = 0;
    while i < keylen {
        newvalue[n] = *key.add(i) as u8;
        n += 1;
        i += 1;
    }
    newvalue[n] = b'\\';
    n += 1;
    let vallen = strlen(value);
    let mut i = 0;
    while i < vallen {
        newvalue[n] = *value.add(i) as u8;
        n += 1;
        i += 1;
    }
    newvalue[n] = 0;

    // Append, do not overwrite: `s += strlen (s)` first.
    let mut s = s.add(strlen(s));

    // Only copy ascii values.  With sv_highchars set (the default) a byte is
    // kept when it is > 13, i.e. 14..255 through; with it clear the high bit is
    // stripped and anything below 32 or above 127 is dropped, which leaves
    // 14..127.
    let highchars = sv_highchars_integer();
    let mut v = newvalue.as_ptr();
    while *v != 0 {
        let mut c = *v as c_int;
        v = v.add(1);

        if highchars == 0 {
            c &= 127;
            if c < 32 || c > 127 {
                continue;
            }
        }

        if c > 13 {
            *s = c as c_char;
            s = s.add(1);
        }
    }
    *s = 0;
}

/// `Info_SetValueForKey`
#[no_mangle]
pub unsafe extern "C" fn Info_SetValueForKey(
    s: *mut c_char,
    key: *const c_char,
    value: *const c_char,
    maxsize: usize,
) {
    if *key == b'*' as c_char {
        CON_Printf(PRINT_NORMAL, c"Can't set * keys\n".as_ptr());
        return;
    }

    Info_SetValueForStarKey(s, key, value, maxsize);
}

/// `Info_Print`
#[no_mangle]
pub unsafe extern "C" fn Info_Print(s: *const c_char) {
    let mut key = [0 as c_char; MAX_INFO_VALUE];
    let mut value = [0 as c_char; MAX_INFO_VALUE];
    let mut s = s;

    if *s == BACKSLASH {
        s = s.add(1);
    }

    while *s != 0 {
        let mut o = key.as_mut_ptr();
        while *s != 0 && *s != BACKSLASH {
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }

        let l = o.offset_from(key.as_ptr());
        if l < 20 {
            // Pad the key to 20 columns, then terminate at key[20] rather than
            // at o -- the C writes the terminator there in both branches.
            memset(
                o.cast::<c_void>(),
                b' ' as c_int,
                (20 - l) as usize,
            );
            key[20] = 0;
        } else {
            *o = 0;
        }
        CON_Printf(PRINT_NORMAL, c"%s".as_ptr(), key.as_ptr());

        if *s == 0 {
            CON_Printf(PRINT_NORMAL, c"MISSING VALUE\n".as_ptr());
            return;
        }

        let mut o = value.as_mut_ptr();
        s = s.add(1);
        while *s != 0 && *s != BACKSLASH {
            *o = *s;
            o = o.add(1);
            s = s.add(1);
        }
        *o = 0;

        if *s != 0 {
            s = s.add(1);
        }
        CON_Printf(PRINT_NORMAL, c"%s\n".as_ptr(), value.as_ptr());
    }
}