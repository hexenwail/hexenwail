// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/cvar.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2008-2010  O.Sezer <sezero@users.sourceforge.net>
// Copyright (C) 2026 Hexenwail contributors.
//
// Cvars are the engine's name/value store: every setting the player, a config
// file or a mod can name goes through this file.  Three properties of the C
// original shape the port, and the first two are why the ownership model in
// history/rust_phase6_cvar_ownership.md was written before the port started.
//
// 1. `cvar_t` is caller-owned.  It is a public struct in cvar.h and 105
//    translation units under engine/ reference it, nearly all as file-scope
//    globals that Cvar_RegisterVariable links into the list by pointer.  Rust
//    never allocates, copies, moves or frees one; it owns only the list head,
//    the 32-entry alias table and the two strings hanging off each node.
//
// 2. `string` and `default_string` are allocated with the engine's Z_Malloc /
//    Z_Strdup and released with Z_Free, not with a Rust allocator, so the two
//    implementations stay interchangeable if both ever run in one process.
//
// 3. Callbacks re-enter.  Cvar_SetQuick calls `var->callback(var)` as its last
//    act, and SV_Callback_Serverinfo (hexenworld/server/sv_main.c) reads other
//    cvars from inside that call.  Nothing here holds Rust-owned state across
//    the callback: the list is walked with raw pointers into caller-owned
//    nodes, and cvar_vars is re-read rather than captured.  Cvar_MoveToFront,
//    Cvar_ResetAll_f and Cvar_ResetCfg_f therefore read `next` after the
//    callback returns, exactly as the C's for-loop does.
//
// The C's statement order is part of the contract and is kept literally:
// Cvar_SetQuick's no-change return sits above the CVAR_CHANGED flag and above
// the callback (a redundant set must do nothing at all), and Cvar_Reset's
// "registered before defaults were captured" return sits above the set.
//
// cvar_t is described here rather than in a module of its own because
// info_str.rs reads `sv_highchars.integer` through Cvar_FindVar and needs the
// struct even in a harness build that links only that port; lib.rs compiles
// this module for either feature and the `#[cfg(feature = "cvar")]` below
// keeps the functions out of the info_str-only build, as sizebuf.rs does for
// msg_io.

use core::ffi::{c_char, c_float, c_int, c_uint};

// Only the cvar functions cast through c_void; the ABI half above does not, so
// the import is gated or an info_str-only build warns it is unused.
#[cfg(feature = "cvar")]
use core::ffi::c_void;

//============================================================================
// cvar_t
//============================================================================

/// `void (*)(struct cvar_s *)` from cvar.h.  `None` is the C's NULL.
pub type CvarCallback = Option<unsafe extern "C" fn(*mut CvarC)>;

/// `cvar_t` from engine/h2shared/cvar.h.
///
/// `string` is `const char *` in the C, which writes through it with a cast
/// (`memcpy((char *)var->string, ...)`), so the port spells it mutable.
/// `default_string` is last in the C on purpose -- every cvar in the tree is
/// brace-initialised positionally as `{ name, string, flags }` -- and the
/// assertions below pin the offsets so an added field cannot land before it.
#[repr(C)]
pub struct CvarC {
    pub name: *const c_char,
    pub string: *mut c_char,
    pub flags: c_uint,
    pub value: c_float,
    pub integer: c_int,
    pub callback: CvarCallback,
    pub next: *mut CvarC,
    pub default_string: *mut c_char,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut CvarC>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut CvarC>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

// Derived from the pointer width rather than written down, because the same
// archive is built for wasm32 where a pointer is four bytes; the ABI gate runs
// this check under node for exactly that reason.
const _: () = {
    assert!(core::mem::offset_of!(CvarC, name) == 0);
    assert!(core::mem::offset_of!(CvarC, string) == PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, flags) == 2 * PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, value) == 2 * PTR_SIZE + 4);
    assert!(core::mem::offset_of!(CvarC, integer) == 2 * PTR_SIZE + 8);
    assert!(core::mem::offset_of!(CvarC, callback) == align_up(2 * PTR_SIZE + 12, PTR_ALIGN));
    assert!(core::mem::offset_of!(CvarC, next)
        == align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + PTR_SIZE);
    assert!(core::mem::offset_of!(CvarC, default_string)
        == align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + 2 * PTR_SIZE);
    assert!(core::mem::size_of::<CvarC>()
        == align_up(align_up(2 * PTR_SIZE + 12, PTR_ALIGN) + 3 * PTR_SIZE, PTR_ALIGN));
};

// The layout accessors let the C differential harness and engine/rust/tests/
// abi_layout.c check these against the real cvar_t without restating Rust's
// offsets in C.  They are compiled for the info_str build too, which is a
// reader of cvar_t rather than a port of cvar.c.
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

//============================================================================
// the port proper
//============================================================================

//============================================================================
// the C ABI the port calls into
//============================================================================

/// `_PRINT_NORMAL` from engine/h2shared/printsys.h -- the first argument the
/// C's `Con_Printf(fmt, ...)` macro passes to CON_Printf.
#[cfg(feature = "cvar")]
const PRINT_NORMAL: c_uint = 0;

/// `Z_MAINZONE` from engine/h2shared/zone.h, the zone the C stores cvar
/// strings in.
#[cfg(feature = "cvar")]
const Z_MAINZONE: c_int = 1 << 0;

/// cvar.h flag bits.  `CVAR_NONE` is not spelled out because it is 0 and is
/// never used as an argument here.
#[cfg(feature = "cvar")]
const CVAR_ARCHIVE: c_uint = 1 << 0;
#[cfg(feature = "cvar")]
const CVAR_CHANGED: c_uint = 1 << 4;
#[cfg(feature = "cvar")]
const CVAR_ROM: c_uint = 1 << 6;
#[cfg(feature = "cvar")]
const CVAR_LOCKED: c_uint = 1 << 8;
#[cfg(feature = "cvar")]
const CVAR_REGISTERED: c_uint = 1 << 10;
#[cfg(feature = "cvar")]
const CVAR_CALLBACK: c_uint = 1 << 16;

/// `MAX_CVAR_ALIASES` from cvar.c.
#[cfg(feature = "cvar")]
const MAX_CVAR_ALIASES: c_int = 32;

/// `sizeof(value)` for the two `char value[512]` locals in the C.
#[cfg(feature = "cvar")]
const VALUE_BUF: usize = 512;

/// `sizeof(val)` for the `char val[32]` locals in Cvar_SetValue and
/// Cvar_SetValueQuick.
#[cfg(feature = "cvar")]
const VAL_BUF: usize = 32;

#[cfg(feature = "cvar")]
extern "C" {
    /// The real function behind the `Con_Printf(fmt, ...)` macro.  Variadic,
    /// and Rust performs no C default argument promotions: `%f` arguments are
    /// passed as f64 and small integers already widened to c_int, which is
    /// what the C compiler does at each of these call sites.
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);

    fn Cmd_Argv(arg: c_int) -> *const c_char;
    fn Cmd_Argc() -> c_int;
    fn Cmd_AddCommand(cmd_name: *const c_char, function: Option<unsafe extern "C" fn()>);
    fn Cmd_Exists(cmd_name: *const c_char) -> c_int;

    fn Z_Malloc(size: c_int, zone_id: c_int) -> *mut c_void;
    fn Z_Free(ptr: *mut c_void);
    fn Z_Strdup(s: *const c_char) -> *mut c_char;

    fn q_strlcpy(dst: *mut c_char, src: *const c_char, siz: usize) -> usize;
    fn q_strcasecmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn q_snprintf(str: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;

    fn atof(s: *const c_char) -> f64;
    fn strtod(s: *const c_char, endptr: *mut *mut c_char) -> f64;
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    /// `FILE *` is opaque here; the pointer is passed straight to fprintf.
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
}

//============================================================================
// the ported state
//============================================================================

/// `static cvar_t *cvar_vars` -- the list head.  C never names it (it is
/// static in cvar.c), so the port owns it outright; every node in the list is
/// a caller's.
#[cfg(feature = "cvar")]
static mut CVAR_VARS: *mut CvarC = core::ptr::null_mut();

/// `static char cvar_null_string[] = ""` -- what Cvar_VariableString returns
/// for a name that is not registered.  Not a C string literal, because the
/// returned pointer must stay valid for the caller to read.
#[cfg(feature = "cvar")]
static mut CVAR_NULL_STRING: [c_char; 1] = [0];

#[cfg(feature = "cvar")]
static mut CVAR_CONFIG_DIRTY: c_int = 0;

/// `cvaralias_t` from cvar.c: a second spelling of a registered cvar, plus the
/// target's own callback so installing ours chains instead of replacing it.
#[cfg(feature = "cvar")]
#[derive(Clone, Copy)]
struct CvarAliasC {
    alias: *mut CvarC,
    target: *mut CvarC,
    target_callback: CvarCallback,
}

#[cfg(feature = "cvar")]
const EMPTY_ALIAS: CvarAliasC = CvarAliasC {
    alias: core::ptr::null_mut(),
    target: core::ptr::null_mut(),
    target_callback: None,
};

#[cfg(feature = "cvar")]
static mut CVAR_ALIASES: [CvarAliasC; MAX_CVAR_ALIASES as usize] = [EMPTY_ALIAS; 32];
#[cfg(feature = "cvar")]
static mut NUM_CVAR_ALIASES: c_int = 0;

//============================================================================
// lookup
//============================================================================

/// `Cvar_FindVar`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_FindVar(var_name: *const c_char) -> *mut CvarC {
    let mut var = CVAR_VARS;

    while !var.is_null() {
        if strcmp(var_name, (*var).name) == 0 {
            return var;
        }
        var = (*var).next;
    }

    core::ptr::null_mut()
}

/// `Cvar_FindVarAfter`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_FindVarAfter(
    prev_name: *const c_char,
    with_flags: c_uint,
) -> *mut CvarC {
    let mut var;

    if *prev_name != 0 {
        let prev = Cvar_FindVar(prev_name);
        if prev.is_null() {
            return core::ptr::null_mut();
        }
        var = (*prev).next;
    } else {
        var = CVAR_VARS;
    }

    while !var.is_null() {
        if ((*var).flags & with_flags) != 0 || with_flags == 0 {
            break;
        }
        var = (*var).next;
    }

    var
}

//============================================================================
// locking
//============================================================================

/// `Cvar_LockVar`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_LockVar(var_name: *const c_char) {
    let var = Cvar_FindVar(var_name);
    if !var.is_null() {
        (*var).flags |= CVAR_LOCKED;
    }
}

/// `Cvar_UnlockVar`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_UnlockVar(var_name: *const c_char) {
    let var = Cvar_FindVar(var_name);
    if !var.is_null() {
        (*var).flags &= !CVAR_LOCKED;
    }
}

/// `Cvar_UnlockAll`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_UnlockAll() {
    let mut var = CVAR_VARS;

    while !var.is_null() {
        (*var).flags &= !CVAR_LOCKED;
        var = (*var).next;
    }
}

//============================================================================
// reads
//============================================================================

/// `Cvar_VariableValue` -- the C returns `atof(...)`, a double, from a
/// function declared `float`, so the narrowing happens on return.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_VariableValue(var_name: *const c_char) -> c_float {
    let var = Cvar_FindVar(var_name);
    if var.is_null() {
        return 0.0;
    }
    atof((*var).string) as c_float
}

/// `Cvar_VariableString` -- returns the node's own storage, not a copy.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_VariableString(var_name: *const c_char) -> *const c_char {
    let var = Cvar_FindVar(var_name);
    if var.is_null() {
        return (&raw const CVAR_NULL_STRING).cast::<c_char>();
    }
    (*var).string
}

/// `Cvar_MarkConfigDirty`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_MarkConfigDirty() {
    CVAR_CONFIG_DIRTY = 1;
}

/// `Cvar_ConfigDirty`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_ConfigDirty() -> c_int {
    CVAR_CONFIG_DIRTY
}

/// `Cvar_ConfigWritten`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_ConfigWritten() {
    CVAR_CONFIG_DIRTY = 0;
}

//============================================================================
// setting
//============================================================================

/// `Cvar_SetQuick`, the function every other setter funnels through.
#[cfg(feature = "cvar")]
unsafe fn cvar_set_quick(var: *mut CvarC, value: *const c_char) {
    if ((*var).flags & (CVAR_ROM | CVAR_LOCKED)) != 0 {
        return;
    }
    if ((*var).flags & CVAR_REGISTERED) == 0 {
        return;
    }

    if (*var).string.is_null() {
        (*var).string = Z_Strdup(value);
    } else {
        // If no change, then DON'T do anything at all.  Some, if not all, of
        // the cvar callbacks may actually rely on this behavior -- and the
        // alias mirror relies on it to bottom out.
        if strcmp((*var).string, value) == 0 {
            return;
        }

        (*var).flags |= CVAR_CHANGED;
        // Below the no-change return, so a redundant set does not mark the
        // config stale.  Above the callback, because a callback is free to set
        // further cvars and each of those marks itself.  uhexen2-ghv0
        if ((*var).flags & CVAR_ARCHIVE) != 0 {
            CVAR_CONFIG_DIRTY = 1;
        }

        let len = strlen(value);
        if len != strlen((*var).string) {
            Z_Free((*var).string.cast::<c_void>());
            (*var).string = Z_Malloc((len + 1) as c_int, Z_MAINZONE).cast::<c_char>();
        }
        memcpy(
            (*var).string.cast::<c_void>(),
            value.cast::<c_void>(),
            len + 1,
        );
    }

    (*var).value = atof((*var).string) as c_float;
    (*var).integer = (*var).value as c_int;

    if let Some(callback) = (*var).callback {
        callback(var);
    }
}

/// `Cvar_SetQuick` as the header declares it.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetQuick(var: *mut CvarC, value: *const c_char) {
    cvar_set_quick(var, value);
}

/// The shared body of Cvar_SetValue and Cvar_SetValueQuick: `%i` when the
/// float is integral, otherwise `%f` with trailing zeroes trimmed.
///
/// The trim loop is the C's, including its shape: `--ptr > val` is tested
/// before `*ptr` is read, so it never looks below the buffer, and a '.' is
/// never trimmed away (so "1.000" becomes "1.0", not "1").
#[cfg(feature = "cvar")]
unsafe fn format_value(value: c_float, buf: *mut c_char) {
    if value == (value as c_int) as c_float {
        q_snprintf(buf, VAL_BUF, c"%i".as_ptr(), value as c_int);
    } else {
        q_snprintf(buf, VAL_BUF, c"%f".as_ptr(), value as f64);

        let mut ptr = buf;
        while *ptr != 0 {
            ptr = ptr.add(1);
        }
        loop {
            ptr = ptr.sub(1);
            if !(ptr > buf) {
                break;
            }
            if *ptr != b'0' as c_char {
                break;
            }
            if *ptr.sub(1) == b'.' as c_char {
                break;
            }
            *ptr = 0;
        }
    }
}

/// `Cvar_SetValueQuick`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetValueQuick(var: *mut CvarC, value: c_float) {
    let mut val = [0 as c_char; VAL_BUF];

    format_value(value, val.as_mut_ptr());
    cvar_set_quick(var, val.as_ptr());
}

/// `Cvar_Set`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_Set(var_name: *const c_char, value: *const c_char) {
    let var = Cvar_FindVar(var_name);

    if var.is_null() {
        // there is an error in C code if this happens
        CON_Printf(
            PRINT_NORMAL,
            c"%s: variable %s not found\n".as_ptr(),
            c"Cvar_Set".as_ptr(),
            var_name,
        );
        return;
    }

    cvar_set_quick(var, value);
}

/// `Cvar_SetValue`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetValue(var_name: *const c_char, value: c_float) {
    let mut val = [0 as c_char; VAL_BUF];

    format_value(value, val.as_mut_ptr());
    Cvar_Set(var_name, val.as_ptr());
}

/// `Cvar_SetROM`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetROM(var_name: *const c_char, value: *const c_char) {
    let var = Cvar_FindVar(var_name);
    if !var.is_null() {
        (*var).flags &= !CVAR_ROM;
        cvar_set_quick(var, value);
        (*var).flags |= CVAR_ROM;
    }
}

/// `Cvar_SetValueROM`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetValueROM(var_name: *const c_char, value: c_float) {
    let var = Cvar_FindVar(var_name);
    if !var.is_null() {
        (*var).flags &= !CVAR_ROM;
        Cvar_SetValueQuick(var, value);
        (*var).flags |= CVAR_ROM;
    }
}

/// `Cvar_RegisterVariable` -- links the caller's node in and copies its
/// initial value into storage the port owns.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_RegisterVariable(variable: *mut CvarC) {
    let mut value = [0 as c_char; VALUE_BUF];

    // first check to see if it has already been defined
    if !Cvar_FindVar((*variable).name).is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Can't register variable %s, already defined\n".as_ptr(),
            (*variable).name,
        );
        return;
    }

    // check for overlap with a command
    if Cmd_Exists((*variable).name) != 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: %s is a command\n".as_ptr(),
            c"Cvar_RegisterVariable".as_ptr(),
            (*variable).name,
        );
        return;
    }

    // link the variable in
    (*variable).next = CVAR_VARS;
    CVAR_VARS = variable;
    (*variable).flags |= CVAR_REGISTERED;

    // copy the value off, because future sets will Z_Free it.  This is also
    // what makes Cvar_RegisterAlias's seeding of alias->string from the
    // target's default_string safe: the seeding is a read, and this copies it
    // before Cvar_SetQuick could ever free anything.
    q_strlcpy(value.as_mut_ptr(), (*variable).string, VALUE_BUF);
    (*variable).string = core::ptr::null_mut();

    // remember it, so resetcfg/resetall have somewhere to go back to.
    // Z_Strdup rather than keeping the caller's pointer: most are string
    // literals and would be fine, but a few cvars are registered from
    // generated names.
    (*variable).default_string = Z_Strdup(value.as_ptr());

    if ((*variable).flags & CVAR_CALLBACK) == 0 {
        (*variable).callback = None;
    }

    // set it through the function to be consistent
    let set_rom = ((*variable).flags & CVAR_ROM) != 0;
    (*variable).flags &= !CVAR_ROM;
    cvar_set_quick(variable, value.as_ptr());
    if set_rom {
        (*variable).flags |= CVAR_ROM;
    }
}

/// `Cvar_Reset`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_Reset(name: *const c_char) {
    let var = Cvar_FindVar(name);

    if var.is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Cvar_Reset: variable %s not found\n".as_ptr(),
            name,
        );
        return;
    }
    if (*var).default_string.is_null() {
        return; // registered before defaults were captured
    }

    cvar_set_quick(var, (*var).default_string);
}

/// `Cvar_HasValue` -- numeric when both sides parse as numbers, textual
/// otherwise, so `cycle r_scale 1 2` still matches a cvar reading "1.000"
/// while the string cvars treat "1.0" and "1" as different.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_HasValue(var: *const CvarC, value: *const c_char) -> c_int {
    if var.is_null() || value.is_null() {
        return 0;
    }

    let mut endvar: *mut c_char = core::ptr::null_mut();
    let mut endval: *mut c_char = core::ptr::null_mut();

    let a = strtod((*var).string, &mut endvar);
    let b = strtod(value, &mut endval);

    if endvar != (*var).string && *endvar == 0 && endval != value as *mut c_char && *endval == 0 {
        return (a == b) as c_int;
    }

    (strcmp((*var).string, value) == 0) as c_int
}

//============================================================================
// the console commands
//============================================================================

/// `Cvar_Cycle_f` -- registered for both `cycle` and `cycleback`, and it
/// dispatches on argv[0].
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_Cycle_f() {
    if Cmd_Argc() < 3 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s <cvar> <value list>: cycle a cvar through a list of values\n".as_ptr(),
            Cmd_Argv(0),
        );
        return;
    }

    let var = Cvar_FindVar(Cmd_Argv(1));
    if var.is_null() {
        CON_Printf(
            PRINT_NORMAL,
            c"Cvar \"%s\" not found\n".as_ptr(),
            Cmd_Argv(1),
        );
        return;
    }

    // Find where we are in the list and step off it.  A list that repeats a
    // value sticks on the first copy; upstream has the same hole and it is not
    // worth the ambiguity of guessing which one was meant.
    let mut i;
    if q_strcasecmp(Cmd_Argv(0), c"cycle".as_ptr()) == 0 {
        i = 2;
        while i < Cmd_Argc() {
            if Cvar_HasValue(var, Cmd_Argv(i)) != 0 {
                break;
            }
            i += 1;
        }
        i += 1;
        if i >= Cmd_Argc() {
            i = 2;
        }
    } else {
        i = Cmd_Argc() - 1;
        while i >= 2 {
            if Cvar_HasValue(var, Cmd_Argv(i)) != 0 {
                break;
            }
            i -= 1;
        }
        i -= 1;
        if i < 2 {
            i = Cmd_Argc() - 1;
        }
    }

    cvar_set_quick(var, Cmd_Argv(i));
}

/// `Cvar_Inc_f`
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_Inc_f() {
    match Cmd_Argc() {
        2 => Cvar_SetValue(Cmd_Argv(1), Cvar_VariableValue(Cmd_Argv(1)) + 1.0),
        3 => {
            // The C adds a float to atof()'s double and narrows on the call,
            // so the addition happens at double width here too.
            let sum = Cvar_VariableValue(Cmd_Argv(1)) as f64 + atof(Cmd_Argv(2));
            Cvar_SetValue(Cmd_Argv(1), sum as c_float);
        }
        _ => CON_Printf(PRINT_NORMAL, c"inc <cvar> [amount] : increment a cvar\n".as_ptr()),
    }
}

/// `Cvar_Toggle_f` -- zero becomes 1, anything else becomes 0.
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_Toggle_f() {
    match Cmd_Argc() {
        2 => {
            if Cvar_VariableValue(Cmd_Argv(1)) != 0.0 {
                Cvar_Set(Cmd_Argv(1), c"0".as_ptr());
            } else {
                Cvar_Set(Cmd_Argv(1), c"1".as_ptr());
            }
        }
        _ => CON_Printf(
            PRINT_NORMAL,
            c"toggle <cvar> : toggle a cvar between 0 and 1\n".as_ptr(),
        ),
    }
}

/// `Cvar_Reset_f`
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_Reset_f() {
    match Cmd_Argc() {
        2 => Cvar_Reset(Cmd_Argv(1)),
        _ => CON_Printf(
            PRINT_NORMAL,
            c"reset <cvar> : reset a cvar to its default value\n".as_ptr(),
        ),
    }
}

/// `Cvar_ResetAll_f`
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_ResetAll_f() {
    // `next` is read after Cvar_Reset returns, because the callback it can
    // reach is free to change the list; the C's for-loop update does the same.
    let mut var = CVAR_VARS;
    while !var.is_null() {
        Cvar_Reset((*var).name);
        var = (*var).next;
    }
}

/// `Cvar_ResetCfg_f`
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_ResetCfg_f() {
    let mut var = CVAR_VARS;
    while !var.is_null() {
        if ((*var).flags & CVAR_ARCHIVE) != 0 {
            Cvar_Reset((*var).name);
        }
        var = (*var).next;
    }
}

/// `Cvar_Init`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_Init() {
    Cmd_AddCommand(c"cycle".as_ptr(), Some(Cvar_Cycle_f));
    Cmd_AddCommand(c"cycleback".as_ptr(), Some(Cvar_Cycle_f));
    Cmd_AddCommand(c"inc".as_ptr(), Some(Cvar_Inc_f));
    Cmd_AddCommand(c"toggle".as_ptr(), Some(Cvar_Toggle_f)); // uhexen2-a5nn.24
    Cmd_AddCommand(c"reset".as_ptr(), Some(Cvar_Reset_f)); // uhexen2-a5nn.24
    Cmd_AddCommand(c"resetall".as_ptr(), Some(Cvar_ResetAll_f));
    Cmd_AddCommand(c"resetcfg".as_ptr(), Some(Cvar_ResetCfg_f));
}

/// `Cvar_SetCallback`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_SetCallback(var: *mut CvarC, func: CvarCallback) {
    (*var).callback = func;
    if func.is_some() {
        (*var).flags |= CVAR_CALLBACK;
    } else {
        (*var).flags &= !CVAR_CALLBACK;
    }
}

//============================================================================
// second spellings.  uhexen2-a5nn.32
//============================================================================

/// `Cvar_FindAlias`
#[cfg(feature = "cvar")]
unsafe fn cvar_find_alias(var: *const CvarC) -> *mut CvarAliasC {
    let mut i: c_int = 0;

    while i < NUM_CVAR_ALIASES {
        let a = &raw mut CVAR_ALIASES[i as usize];
        if (*a).alias == var as *mut CvarC || (*a).target == var as *mut CvarC {
            return a;
        }
        i += 1;
    }

    core::ptr::null_mut()
}

/// `Cvar_AliasCallback`
#[cfg(feature = "cvar")]
#[allow(non_snake_case)]
unsafe extern "C" fn Cvar_AliasCallback(var: *mut CvarC) {
    let a = cvar_find_alias(var);

    if a.is_null() {
        return;
    }

    // Not a recursion hazard: Cvar_SetQuick returns without touching the
    // callback when the value is already what it is being set to, so the
    // mirror bottoms out on the first hop back.
    if var == (*a).alias {
        cvar_set_quick((*a).target, (*var).string);
    } else {
        cvar_set_quick((*a).alias, (*var).string);
        // Chained rather than replaced.  Aliasing a cvar that already had a
        // callback must not silently stop it firing -- that failure is
        // invisible at the console, where both names would still read back
        // correctly while whatever the callback did stopped happening.
        if let Some(callback) = (*a).target_callback {
            callback(var);
        }
    }
}

/// `Cvar_RegisterAlias`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_RegisterAlias(alias: *mut CvarC, target: *mut CvarC) {
    if ((*target).flags & CVAR_REGISTERED) == 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: %s is not registered\n".as_ptr(),
            c"Cvar_RegisterAlias".as_ptr(),
            (*target).name,
        );
        return;
    }
    if !cvar_find_alias(target).is_null() || !cvar_find_alias(alias).is_null() {
        // Refused rather than supported: a chain would make the ping-pong
        // above ambiguous, and two spellings is the problem to solve, not a
        // thing to have more of.
        CON_Printf(
            PRINT_NORMAL,
            c"%s: %s is already aliased\n".as_ptr(),
            c"Cvar_RegisterAlias".as_ptr(),
            (*target).name,
        );
        return;
    }
    if NUM_CVAR_ALIASES == MAX_CVAR_ALIASES {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: too many cvar aliases\n".as_ptr(),
            c"Cvar_RegisterAlias".as_ptr(),
        );
        return;
    }

    (*alias).flags &= !CVAR_ARCHIVE;
    // Seeded from the target's DEFAULT, not its current value, so that
    // resetall and resetcfg send both names to the same place.  The set below
    // then brings it to the target's current value.  This is a read of the
    // target's default_string, not a sharing of it: Cvar_RegisterVariable
    // copies the value into storage of the alias's own before anything can
    // free the target's.
    (*alias).string = (*target).default_string;
    Cvar_RegisterVariable(alias);
    if ((*alias).flags & CVAR_REGISTERED) == 0 {
        return; // name clash: Cvar_RegisterVariable already said so
    }

    let a = &raw mut CVAR_ALIASES[NUM_CVAR_ALIASES as usize];
    (*a).alias = alias;
    (*a).target = target;
    (*a).target_callback = (*target).callback;
    NUM_CVAR_ALIASES += 1;

    Cvar_SetCallback(alias, Some(Cvar_AliasCallback));
    Cvar_SetCallback(target, Some(Cvar_AliasCallback));

    cvar_set_quick(alias, (*target).string);
}

//============================================================================
// console dispatch, ordering and config output
//============================================================================

/// `Cvar_Command` -- what Cmd_ExecuteString calls when argv[0] is not a
/// command.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_Command() -> c_int {
    // check variables
    let v = Cvar_FindVar(Cmd_Argv(0));
    if v.is_null() {
        return 0;
    }

    // perform a variable print or set
    if Cmd_Argc() == 1 {
        CON_Printf(
            PRINT_NORMAL,
            c"\"%s\" is \"%s\"\n".as_ptr(),
            (*v).name,
            (*v).string,
        );
        return 1;
    }

    Cvar_Set((*v).name, Cmd_Argv(1));
    1
}

/// `Cvar_MoveToFront` -- the list order is observable through
/// Cvar_FindVarAfter and Cvar_WriteVariables, so the surgery is the C's.
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_MoveToFront(name: *const c_char) {
    let mut var = CVAR_VARS;

    while !var.is_null() {
        let next = (*var).next;
        if !next.is_null() && strcmp(name, (*next).name) == 0 {
            // remove from the list
            (*var).next = (*next).next;
            // move to the front
            (*next).next = CVAR_VARS;
            CVAR_VARS = next;
            break;
        }
        var = (*var).next;
    }
}

/// `Cvar_WriteVariables`
#[cfg(feature = "cvar")]
#[no_mangle]
pub unsafe extern "C" fn Cvar_WriteVariables(f: *mut c_void) {
    let mut var = CVAR_VARS;

    while !var.is_null() {
        if ((*var).flags & CVAR_ARCHIVE) != 0 {
            fprintf(f, c"%s \"%s\"\n".as_ptr(), (*var).name, (*var).string);
        }
        var = (*var).next;
    }
}
