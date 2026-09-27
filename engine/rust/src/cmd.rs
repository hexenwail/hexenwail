// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/cmd.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
//
// The command layer: the registry every part of the engine adds to, the
// tokenizer every console line and every script line goes through, the command
// buffer the host drains a frame at a time, and the alias table.  Four
// properties of the C original shape the port.
//
// 1. Two ownership regimes, not one.  Command nodes are **hunk**-allocated and
//    never freed (Hunk_AllocName (..., "commands")), and Cmd_AddCommand refuses
//    once host_initialized because a hunk allocation then would be stomped; the
//    caller's name pointer and C handler are stored as given.  Alias nodes and
//    their values are **zone**-allocated (Z_Malloc / Z_Strdup) and freed on
//    reuse and removal, so the alias table is entirely the port's.
//
// 2. The tokenizer hands out owned tokens and one borrowed pointer.  Each
//    cmd_argv entry is Z_Strdup'd and freed on the next call, while cmd_args
//    points **into the caller's text** -- in the engine that is Cbuf_Execute's
//    stack buffer, so it is only meaningful while the caller's frame lives.
//    That is a caller contract in the C, and the port reproduces it rather than
//    "fixing" it.
//
// 3. The command buffer sits on two other ports.  cmd_text is a sizebuf whose
//    storage SZ_Init takes from the hunk, so Cbuf_* reaches through sizebuf.rs
//    into zone.rs; cfg_enginedefaults registers into cvar.rs; and
//    Cmd_CheckCommand walks the cvar list directly, which is why this module
//    uses cvar.rs's CvarC rather than only cvar functions.  com_argc/com_argv
//    are macros over host_parms, so QuakeParmsC comes from zone.rs.
//
// 4. The target differences are presence, not constants.  The client-only list
//    surface and the four registrations at the end of Cmd_Init exist only when
//    SERVERONLY is absent, and the #if defined(H2W) dispatch arm exists only in
//    hwsv.  Both come from the per-target shim (engine/rust/cmd_target.c,
//    decided on #233): registering commands/cmdlist/cvarlist/aliaslist in
//    h2ded or hwsv would give those targets commands the C does not have, which
//    Cmd_Exists makes observable, and hwsv's NULL-handler diagnostic is
//    reproduced only where that arm is compiled.
//
//    Two arms of the C are deliberately NOT reproduced:
//
//      * Cmd_ForwardToServer() sits under H2W && !SERVERONLY, a configuration
//        no target in this tree builds (glhexen2 is GLQUAKE + H2W_INTEGRATED,
//        never H2W), so referencing it would be an undefined symbol in the
//        targets the archive is linked into -- the #233 rule.  Like info_str's
//        never-compiled client variant, it is documented rather than ported.
//      * In the non-H2W builds the C calls cmd->function() unconditionally, so
//        a NULL handler is a call through NULL.  No command in this tree
//        registers a NULL handler (the only reason the H2W arm exists is that
//        an H2W client does), so both that path and hwsv's diagnostic are
//        unreachable today.  The port still makes the branch explicit: call the
//        handler when there is one, print hwsv's diagnostic where the shim says
//        this target is H2W, and abort with a diagnostic otherwise rather than
//        silently continuing where the C would fault.
//
// The cfg_enginedefaults cvar and the built-in startup script are ordinary
// engine data: the first is registered through Cvar_RegisterVariable exactly as
// the C does, and the second is returned only where the shim says this target
// compiles the __EMSCRIPTEN__ fallback.

use crate::cvar::CvarC;
use crate::sizebuf::SizeBufC;
use crate::zone::QuakeParmsC;
use core::ffi::{c_char, c_int, c_void};

/// `MAX_ALIAS_NAME` from cmd.c.
const MAX_ALIAS_NAME: usize = 32;

/// `MAX_ARGS` from cmd.c -- the tokenizer drops tokens beyond this.
const MAX_ARGS: usize = 80;

/// `MAX_MATCHES` from cmd.h -- what ListCommands/ListCvars/ListAlias cap at.
const MAX_MATCHES: c_int = 128;

/// `sizeof(line)` in Cbuf_Execute, and `sizeof(cmd)` in Cmd_Alias_f.
const LINE_LEN: usize = 1024;

/// `cmd_source_t` from cmd.h: a two-value enum, so an int across the ABI.
/// `SRC_CLIENT` is the initial value of the exported `cmd_source`; on wasm32
/// that storage is C's and zero-initialised there, so the constant has no use
/// on that target.
#[cfg(not(target_family = "wasm"))]
const SRC_CLIENT: c_int = 0;
const SRC_COMMAND: c_int = 1;

/// `_PRINT_NORMAL` / `_PRINT_TERMONLY` from printsys.h -- the first argument
/// the C's Con_Printf and Sys_Printf macros pass to CON_Printf.
const PRINT_NORMAL: u32 = 0;
const PRINT_TERMONLY: u32 = 1;

/// `FS_USERDIR` from quakefs.h, for Cmd_WriteCommands_f.
const FS_USERDIR: c_int = 3;

/// `Z_MAINZONE` from zone.h -- the zone the C puts alias nodes, alias values,
/// tokens and the command buffer's temporary copy in.
const Z_MAINZONE: c_int = 1;

//============================================================================
// the structures the command layer shares with C
//============================================================================

/// `xcommand_t` from cmd.h.  `None` is the C's NULL.
pub type XCommand = Option<unsafe extern "C" fn()>;

/// `cmd_function_t` from cmd.c -- a hunk-allocated registry node.  `name` is
/// the caller's pointer, not a copy.
#[repr(C)]
pub struct CmdFunctionC {
    pub next: *mut CmdFunctionC,
    pub name: *const c_char,
    pub function: XCommand,
}

/// `cmdalias_t` from cmd.c -- a zone-allocated node whose `value` is owned by
/// the module.
#[repr(C)]
pub struct CmdAliasC {
    pub next: *mut CmdAliasC,
    pub name: [c_char; MAX_ALIAS_NAME],
    pub value: *mut c_char,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut c_void>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut c_void>();

const fn align_up(x: usize, align: usize) -> usize {
    (x + align - 1) & !(align - 1)
}

// Derived from the pointer width rather than written down, because the same
// archive is built for wasm32 where a pointer is four bytes; the ABI gate runs
// this check under node for exactly that reason.
const _: () = {
    assert!(core::mem::offset_of!(CmdFunctionC, next) == 0);
    assert!(core::mem::offset_of!(CmdFunctionC, name) == PTR_SIZE);
    assert!(core::mem::offset_of!(CmdFunctionC, function) == 2 * PTR_SIZE);
    assert!(core::mem::size_of::<CmdFunctionC>() == 3 * PTR_SIZE);

    assert!(core::mem::offset_of!(CmdAliasC, next) == 0);
    assert!(core::mem::offset_of!(CmdAliasC, name) == PTR_SIZE);
    assert!(core::mem::offset_of!(CmdAliasC, value) == align_up(PTR_SIZE + MAX_ALIAS_NAME, PTR_ALIGN));
    assert!(core::mem::size_of::<CmdAliasC>()
        == align_up(PTR_SIZE + MAX_ALIAS_NAME, PTR_ALIGN) + PTR_SIZE);
};

// The layout accessors let the C differential harness and
// engine/rust/tests/abi_layout.c check these against the real structs without
// restating Rust's offsets in C.
#[no_mangle]
pub extern "C" fn CmdFunctionC_sizeof() -> usize {
    core::mem::size_of::<CmdFunctionC>()
}

#[no_mangle]
pub extern "C" fn CmdFunctionC_alignof() -> usize {
    core::mem::align_of::<CmdFunctionC>()
}

#[no_mangle]
pub extern "C" fn CmdFunctionC_offsetof_next() -> usize {
    core::mem::offset_of!(CmdFunctionC, next)
}

#[no_mangle]
pub extern "C" fn CmdFunctionC_offsetof_name() -> usize {
    core::mem::offset_of!(CmdFunctionC, name)
}

#[no_mangle]
pub extern "C" fn CmdFunctionC_offsetof_function() -> usize {
    core::mem::offset_of!(CmdFunctionC, function)
}

#[no_mangle]
pub extern "C" fn CmdAliasC_sizeof() -> usize {
    core::mem::size_of::<CmdAliasC>()
}

#[no_mangle]
pub extern "C" fn CmdAliasC_alignof() -> usize {
    core::mem::align_of::<CmdAliasC>()
}

#[no_mangle]
pub extern "C" fn CmdAliasC_offsetof_next() -> usize {
    core::mem::offset_of!(CmdAliasC, next)
}

#[no_mangle]
pub extern "C" fn CmdAliasC_offsetof_name() -> usize {
    core::mem::offset_of!(CmdAliasC, name)
}

#[no_mangle]
pub extern "C" fn CmdAliasC_offsetof_value() -> usize {
    core::mem::offset_of!(CmdAliasC, value)
}

#[no_mangle]
pub extern "C" fn Cmd_MAX_ARGS() -> c_int {
    MAX_ARGS as c_int
}

#[no_mangle]
pub extern "C" fn Cmd_MAX_ALIAS_NAME() -> c_int {
    MAX_ALIAS_NAME as c_int
}

//============================================================================
// what the port calls
//============================================================================

extern "C" {
    // The zone allocator (zone.rs).  The C calls the same names, so both
    // implementations of cmd share one allocator.
    fn Z_Malloc(size: c_int, zone_id: c_int) -> *mut c_void;
    fn Z_Free(ptr: *mut c_void);
    fn Z_Strdup(s: *const c_char) -> *mut c_char;
    fn Hunk_AllocName(size: c_int, name: *const c_char) -> *mut c_void;
    fn Hunk_LowMark() -> c_int;
    fn Hunk_FreeToLowMark(mark: c_int);

    // The command buffer's sizebuf (sizebuf.rs).
    fn SZ_Init(buf: *mut SizeBufC, data: *mut u8, length: c_int);
    fn SZ_Clear(buf: *mut SizeBufC);
    fn SZ_Write(buf: *mut SizeBufC, data: *const c_void, length: c_int);

    // The cvar list (cvar.rs).
    fn Cvar_RegisterVariable(variable: *mut CvarC);
    fn Cvar_VariableString(var_name: *const c_char) -> *const c_char;
    fn Cvar_FindVarAfter(prev_name: *const c_char, with_flags: u32) -> *mut CvarC;
    fn Cvar_Command() -> c_int;
    fn Cvar_Init();

    // Still C in this tree, and present in every target: the token parser and
    // its static buffer (common.c), the filesystem entry points quakefs.c owns
    // until Phase 6 slice 4.
    fn COM_Parse(data: *const c_char) -> *const c_char;
    static mut com_token: [c_char; 1024];
    fn FS_LoadHunkFile(path: *const c_char, path_id: *mut u32) -> *mut u8;
    fn FS_MakePath(base: c_int, error: *mut c_int, path: *const c_char) -> *mut c_char;
    fn FS_FileExists(filename: *const c_char, path_id: *mut u32) -> c_int;

    /// `host_parms` from engine/hexen2/host.h.  com_argc/com_argv are macros
    /// over its argc/argv (common.h:92-93), so there is no symbol to bind.
    static mut host_parms: *mut QuakeParmsC;

    /// `host_initialized` from engine/hexen2/host.h: Cmd_AddCommand refuses
    /// once it is set, because a hunk allocation then would be stomped.
    static mut host_initialized: c_int;

    /// The real function behind the `Con_Printf(fmt, ...)` macro.  Variadic,
    /// and Rust performs no C default argument promotions: integer arguments
    /// are passed already widened to c_int.
    fn CON_Printf(flags: u32, fmt: *const c_char, ...);
    fn Sys_Error(fmt: *const c_char, ...) -> !;

    fn q_strcasecmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn q_strncasecmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    fn q_strlcat(dst: *mut c_char, src: *const c_char, siz: usize) -> usize;

    fn strcmp(s1: *const c_char, s2: *const c_char) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn strcpy(dst: *mut c_char, src: *const c_char) -> *mut c_char;
    fn strcat(dst: *mut c_char, src: *const c_char) -> *mut c_char;
    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn fclose(stream: *mut c_void) -> c_int;

    // The per-target predicates (engine/rust/cmd_target.c).
    fn Cmd_TargetHasClientLists() -> c_int;
    fn Cmd_TargetIsH2W() -> c_int;
    fn Cmd_TargetHasBuiltinStartupScript() -> c_int;
}

//============================================================================
// state
//============================================================================

/// `static sizebuf_t cmd_text` -- the command buffer.
static mut CMD_TEXT: SizeBufC = SizeBufC {
    allowoverflow: 0,
    overflowed: 0,
    data: core::ptr::null_mut(),
    maxsize: 0,
    cursize: 0,
    name: core::ptr::null(),
};

/// `cmd_source_t cmd_source` -- exported, written by Cmd_ExecuteString and
/// read by the C that dispatches commands.
#[cfg(not(target_family = "wasm"))]
#[no_mangle]
pub static mut cmd_source: c_int = SRC_CLIENT;

// rustc does not export `#[no_mangle] static`s from a wasm32 staticlib: it
// keeps the functions global and turns the data into local symbols, so the web
// client's C would not find cmd_source and the link fails on it.  There the
// storage is C's, in engine/rust/wasm_globals.c, and this module uses it like
// any other extern -- the same treatment msg_readcount, msg_badread and
// vec3_origin already get.  Same name, type and zero initial value, so nothing
// below changes.
#[cfg(target_family = "wasm")]
extern "C" {
    pub static mut cmd_source: c_int;
}

/// `static cmdalias_t *cmd_alias`.
static mut CMD_ALIAS: *mut CmdAliasC = core::ptr::null_mut();

/// `static cmd_function_t *cmd_functions`.
static mut CMD_FUNCTIONS: *mut CmdFunctionC = core::ptr::null_mut();

/// `static int cmd_argc`.
static mut CMD_ARGC: c_int = 0;

/// `static char *cmd_argv[MAX_ARGS]`.
static mut CMD_ARGV: [*mut c_char; MAX_ARGS] = [core::ptr::null_mut(); MAX_ARGS];

/// `static char cmd_null_string[] = ""`.
static mut CMD_NULL_STRING: [c_char; 1] = [0];

/// `static const char *cmd_args` -- points into the caller's text.
static mut CMD_ARGS: *const c_char = core::ptr::null();

/// `static qboolean cmd_wait`.
static mut CMD_WAIT: c_int = 0;

/// `static cvar_t cfg_enginedefaults = {"cfg_enginedefaults", "1", CVAR_NONE}`.
/// Registered through the Rust cvar port in Cmd_Init, exactly as the C does.
static mut CFG_ENGINEDEFAULTS: CvarC = CvarC {
    name: c"cfg_enginedefaults".as_ptr(),
    string: c"1".as_ptr() as *mut c_char,
    flags: 0,
    value: 0.0,
    integer: 0,
    callback: None,
    next: core::ptr::null_mut(),
    default_string: core::ptr::null_mut(),
};

/// `cmd_engine_defaults[]` -- injected right after default.cfg.
static CMD_ENGINE_DEFAULTS: &[u8] =
    b"alias zoom_in \"togglezoom\"\nalias zoom_out \"togglezoom\"\n\0";

/// `cmd_builtin_hexenrc[]` -- the `__EMSCRIPTEN__` fallback for a missing
/// hexen.rc.  Compiled in every target but returned only where the shim says
/// this target compiles that fallback.
static CMD_BUILTIN_HEXENRC: &[u8] = b"developer 0\nexec default.cfg\nexec config.cfg\nexec autoexec.cfg\nmenu_main\nstuffcmds\n\0";

/// `static qboolean done` in Cmd_StuffCmds_f.
static mut STUFFCMDS_DONE: c_int = 0;

/// `static const char *legacy_cmds[]` in Cmd_ExecuteString -- removed legacy
/// commands that an old config may still name, silently ignored.
static LEGACY_CMDS: [&[u8]; 15] = [
    b"gl_ztrick\0",
    b"gl_max_size\0",
    b"sys_delay\0",
    b"r_transwater\0",
    b"_windowed_mouse\0",
    b"vid_stretch_by_2\0",
    b"vid_config_y\0",
    b"vid_config_x\0",
    b"_vid_default_mode_win\0",
    b"_vid_default_mode\0",
    b"_vid_wait_override\0",
    b"vid_nopageflip\0",
    b"sys_quake2\0",
    b"term_escapes\0",
    b"\0",
];

//============================================================================
// the command buffer
//============================================================================

/// `Cbuf_Init`
#[no_mangle]
pub unsafe extern "C" fn Cbuf_Init() {
    // SZ_Init allocates through Hunk_AllocName when given a NULL buffer.
    SZ_Init(&raw mut CMD_TEXT, core::ptr::null_mut(), 8192);
}

/// `Cbuf_Clear`
#[no_mangle]
pub unsafe extern "C" fn Cbuf_Clear() {
    SZ_Clear(&raw mut CMD_TEXT);
}

/// `Cbuf_AddText`
#[no_mangle]
pub unsafe extern "C" fn Cbuf_AddText(text: *const c_char) {
    let text_buf = &raw mut CMD_TEXT;
    let l = strlen(text) as c_int;

    if (*text_buf).cursize + l >= (*text_buf).maxsize {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: overflow\n".as_ptr(),
            c"Cbuf_AddText".as_ptr(),
        );
        return;
    }
    SZ_Write(text_buf, text.cast::<c_void>(), l);
}

/// `Cbuf_InsertText` -- prepends, so the inserted text runs first.
#[no_mangle]
pub unsafe extern "C" fn Cbuf_InsertText(text: *const c_char) {
    let text_buf = &raw mut CMD_TEXT;
    let templen = (*text_buf).cursize;
    let mut temp: *mut c_char = core::ptr::null_mut();

    if templen != 0 {
        temp = Z_Malloc(templen, Z_MAINZONE).cast::<c_char>();
        memcpy(
            temp.cast::<c_void>(),
            (*text_buf).data.cast::<c_void>(),
            templen as usize,
        );
        SZ_Clear(text_buf);
    }

    Cbuf_AddText(text);
    SZ_Write(text_buf, c"\n".as_ptr().cast::<c_void>(), 1);

    if templen != 0 {
        SZ_Write(text_buf, temp.cast::<c_void>(), templen);
        Z_Free(temp.cast::<c_void>());
    }
}

/// `Cbuf_Execute` -- drains one command at a time, leaving the rest for the
/// next frame when Cmd_Wait_f was reached.
#[no_mangle]
pub unsafe extern "C" fn Cbuf_Execute() {
    let mut line = [0 as c_char; LINE_LEN];
    let text_buf = &raw mut CMD_TEXT;

    while (*text_buf).cursize != 0 {
        // find a \n or ; line break
        let text = (*text_buf).data as *mut c_char;
        let mut quotes: c_int = 0;
        let mut i: c_int = 0;

        while i < (*text_buf).cursize {
            let ch = *text.offset(i as isize);
            if ch == b'"' as c_char {
                quotes += 1;
            }
            if (quotes & 1) == 0 && ch == b';' as c_char {
                break; // don't break if inside a quoted string
            }
            if ch == b'\n' as c_char {
                break;
            }
            i += 1;
        }

        let linep = line.as_mut_ptr();
        if i > (LINE_LEN as c_int) - 1 {
            memcpy(
                linep.cast::<c_void>(),
                text.cast::<c_void>(),
                LINE_LEN - 1,
            );
            *linep.offset((LINE_LEN as isize) - 1) = 0;
        } else {
            memcpy(linep.cast::<c_void>(), text.cast::<c_void>(), i as usize);
            *linep.offset(i as isize) = 0;
        }

        // delete the text from the command buffer and move the rest down:
        // commands (exec, alias) can insert data at the beginning
        if i == (*text_buf).cursize {
            (*text_buf).cursize = 0;
        } else {
            i += 1;
            (*text_buf).cursize -= i;
            memmove(
                text.cast::<c_void>(),
                text.offset(i as isize).cast::<c_void>(),
                (*text_buf).cursize as usize,
            );
        }

        Cmd_ExecuteString(linep, SRC_COMMAND);

        if CMD_WAIT != 0 {
            // skip out while text still remains in buffer, leaving it for next
            // frame
            CMD_WAIT = 0;
            break;
        }
    }
}

//============================================================================
// script commands
//============================================================================

/// `Cmd_StuffCmds_f` -- turns `+cmd args` command-line parameters into buffer
/// text, once.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn Cmd_StuffCmds_f() {
    if STUFFCMDS_DONE != 0 {
        return;
    }
    STUFFCMDS_DONE = 1;

    let parms = host_parms;
    let argc = (*parms).argc;
    let argv = (*parms).argv;
    let mut s: c_int = 0;
    let mut i: c_int = 1;

    while i < argc {
        let arg = *argv.offset(i as isize);
        if !arg.is_null() {
            s += strlen(arg) as c_int + 1;
        }
        i += 1;
    }
    if s == 0 {
        return;
    }

    let text = Z_Malloc(s + 1, Z_MAINZONE).cast::<c_char>();
    *text = 0;

    i = 1;
    while i < argc {
        let arg = *argv.offset(i as isize);
        if arg.is_null() {
            i += 1;
            continue; // NEXTSTEP nulls out -NXHost
        }
        if *arg != b'+' as c_char {
            i += 1;
            continue;
        }
        // found a command
        if *text != 0 {
            strcat(text, c" ".as_ptr());
        }
        strcat(text, arg.offset(1));
        if i == argc - 1 {
            strcat(text, c"\n".as_ptr());
            break;
        }
        // add the arguments of the command
        i += 1;
        while i < argc {
            let a2 = *argv.offset(i as isize);
            if *a2 == b'+' as c_char || *a2 == b'-' as c_char {
                // found a new command or a new command-line switch
                strcat(text, c"\n".as_ptr());
                i -= 1;
                break;
            }
            strcat(text, c" ".as_ptr());
            strcat(text, a2);
            if i == argc - 1 {
                strcat(text, c"\n".as_ptr());
            }
            i += 1;
        }
        i += 1;
    }

    if *text != 0 {
        Cbuf_InsertText(text);
    }

    Z_Free(text.cast::<c_void>());
}

/// `Cmd_StartupScript` -- the script the host execs at startup.  The
/// `__EMSCRIPTEN__` fallback is reproduced only where the shim says this
/// target compiles it.
#[no_mangle]
pub unsafe extern "C" fn Cmd_StartupScript() -> *const c_char {
    if Cmd_TargetHasBuiltinStartupScript() != 0
        && FS_FileExists(c"hexen.rc".as_ptr(), core::ptr::null_mut()) == 0
    {
        CON_Printf(
            PRINT_NORMAL,
            c"couldn't exec hexen.rc, using built-in startup script\n".as_ptr(),
        );
        return CMD_BUILTIN_HEXENRC.as_ptr().cast::<c_char>();
    }
    c"exec hexen.rc\n".as_ptr()
}

/// `Cmd_Exec_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_Exec_f() {
    if Cmd_Argc() != 2 {
        CON_Printf(
            PRINT_NORMAL,
            c"exec <filename> : execute a script file\n".as_ptr(),
        );
        return;
    }

    // FIXME: is this safe freeing the hunk here???
    let mark = Hunk_LowMark();
    let f = FS_LoadHunkFile(Cmd_Argv(1), core::ptr::null_mut()).cast::<c_char>();
    if f.is_null() {
        CON_Printf(PRINT_NORMAL, c"couldn't exec %s\n".as_ptr(), Cmd_Argv(1));
        return;
    }
    CON_Printf(PRINT_NORMAL, c"execing %s\n".as_ptr(), Cmd_Argv(1));

    // Two inserts, defaults first: Cbuf_InsertText prepends, so the file body
    // ends up ahead of the engine defaults, which end up ahead of the rest of
    // the buffer.  Order is default.cfg -> engine defaults -> config.cfg.
    let cfg = &raw const CFG_ENGINEDEFAULTS;
    if (*cfg).integer != 0 && q_strcasecmp(Cmd_Argv(1), c"default.cfg".as_ptr()) == 0 {
        Cbuf_InsertText(CMD_ENGINE_DEFAULTS.as_ptr().cast::<c_char>());
    }

    Cbuf_InsertText(f);
    Hunk_FreeToLowMark(mark);
}

/// `Cmd_Echo_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_Echo_f() {
    let mut i: c_int = 1;
    while i < Cmd_Argc() {
        CON_Printf(PRINT_NORMAL, c"%s ".as_ptr(), Cmd_Argv(i));
        i += 1;
    }
    CON_Printf(PRINT_NORMAL, c"\n".as_ptr());
}

/// `Cmd_Alias_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_Alias_f() {
    let mut cmd = [0 as c_char; LINE_LEN];
    let mut a: *mut CmdAliasC;

    if Cmd_Argc() == 1 {
        CON_Printf(PRINT_NORMAL, c"Current alias commands:\n".as_ptr());
        a = CMD_ALIAS;
        while !a.is_null() {
            CON_Printf(
                PRINT_NORMAL,
                c"%s : %s\n".as_ptr(),
                (*a).name.as_ptr(),
                (*a).value,
            );
            a = (*a).next;
        }
        return;
    }

    let s = Cmd_Argv(1);

    if Cmd_Argc() == 2 {
        a = CMD_ALIAS;
        while !a.is_null() {
            if strcmp(s, (*a).name.as_ptr()) == 0 {
                CON_Printf(PRINT_NORMAL, c"%s : %s\n".as_ptr(), s, (*a).value);
                return;
            }
            a = (*a).next;
        }
        CON_Printf(PRINT_NORMAL, c"No alias named %s\n".as_ptr(), s);
        return;
    }

    if strlen(s) >= MAX_ALIAS_NAME {
        CON_Printf(PRINT_NORMAL, c"Alias name is too long\n".as_ptr());
        return;
    }

    // if the alias already exists, reuse it
    a = CMD_ALIAS;
    while !a.is_null() {
        if strcmp(s, (*a).name.as_ptr()) == 0 {
            Z_Free((*a).value.cast::<c_void>());
            break;
        }
        a = (*a).next;
    }

    if a.is_null() {
        a = Z_Malloc(core::mem::size_of::<CmdAliasC>() as c_int, Z_MAINZONE).cast::<CmdAliasC>();
        (*a).next = CMD_ALIAS;
        CMD_ALIAS = a;
    }
    strcpy((*a).name.as_mut_ptr(), s);

    // copy the rest of the command line
    let cmdp = cmd.as_mut_ptr();
    *cmdp = 0; // start out with a null string
    let c = Cmd_Argc();
    let mut i: c_int = 2;
    while i < c {
        q_strlcat(cmdp, Cmd_Argv(i), LINE_LEN);
        if i != c - 1 {
            q_strlcat(cmdp, c" ".as_ptr(), LINE_LEN);
        }
        i += 1;
    }
    if q_strlcat(cmdp, c"\n".as_ptr(), LINE_LEN) >= LINE_LEN {
        CON_Printf(PRINT_NORMAL, c"alias value too long!\n".as_ptr());
        *cmdp = b'\n' as c_char; // nullify the string
        *cmdp.offset(1) = 0;
    }

    (*a).value = Z_Strdup(cmdp);
}

/// `Cmd_Unalias_f`
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn Cmd_Unalias_f() {
    let mut prev: *mut CmdAliasC = core::ptr::null_mut();

    if Cmd_Argc() != 2 {
        CON_Printf(
            PRINT_NORMAL,
            c"unalias <name> : delete alias\nunaliasall : delete all aliases\n".as_ptr(),
        );
        return;
    }

    let mut a = CMD_ALIAS;
    while !a.is_null() {
        if strcmp(Cmd_Argv(1), (*a).name.as_ptr()) == 0 {
            if !prev.is_null() {
                (*prev).next = (*a).next;
            } else {
                CMD_ALIAS = (*a).next;
            }

            Z_Free((*a).value.cast::<c_void>());
            Z_Free(a.cast::<c_void>());
            return;
        }
        prev = a;
        a = (*a).next;
    }

    CON_Printf(PRINT_NORMAL, c"No alias named %s\n".as_ptr(), Cmd_Argv(1));
}

/// `Cmd_Unaliasall_f`
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn Cmd_Unaliasall_f() {
    while !CMD_ALIAS.is_null() {
        let a = (*CMD_ALIAS).next;
        Z_Free((*CMD_ALIAS).value.cast::<c_void>());
        Z_Free(CMD_ALIAS.cast::<c_void>());
        CMD_ALIAS = a;
    }
}

//============================================================================
// command execution
//============================================================================

/// `Cmd_Argc`
#[no_mangle]
pub unsafe extern "C" fn Cmd_Argc() -> c_int {
    CMD_ARGC
}

/// `Cmd_Argv`
#[no_mangle]
pub unsafe extern "C" fn Cmd_Argv(arg: c_int) -> *const c_char {
    if arg < 0 || arg >= CMD_ARGC {
        return (&raw const CMD_NULL_STRING).cast::<c_char>();
    }
    CMD_ARGV[arg as usize]
}

/// `Cmd_Args` -- argv(1)..argv(argc-1) as one string; a pointer into the
/// caller's text, not a copy.
#[no_mangle]
pub unsafe extern "C" fn Cmd_Args() -> *const c_char {
    if CMD_ARGS.is_null() {
        return (&raw const CMD_NULL_STRING).cast::<c_char>();
    }
    CMD_ARGS
}

/// `Cmd_TokenizeString`
#[no_mangle]
pub unsafe extern "C" fn Cmd_TokenizeString(text: *const c_char) {
    let mut i: c_int = 0;

    // clear the args from the last string
    while i < CMD_ARGC {
        Z_Free(CMD_ARGV[i as usize].cast::<c_void>());
        i += 1;
    }

    CMD_ARGC = 0;
    CMD_ARGS = core::ptr::null();

    let mut t = text;
    loop {
        // skip whitespace up to a /n
        while *t != 0 && *t <= b' ' as c_char && *t != b'\n' as c_char {
            t = t.add(1);
        }

        if *t == b'\n' as c_char {
            // A newline seperates commands in the buffer.  The C advances the
            // pointer here too, but the loop breaks immediately, so nothing
            // reads it.
            break;
        }

        if *t == 0 {
            return;
        }

        if CMD_ARGC == 1 {
            CMD_ARGS = t;
        }

        t = COM_Parse(t);
        if t.is_null() {
            return;
        }

        if CMD_ARGC < MAX_ARGS as c_int {
            CMD_ARGV[CMD_ARGC as usize] = Z_Strdup((&raw const com_token).cast::<c_char>());
            CMD_ARGC += 1;
        }
    }
}

/// `Cmd_AddCommand` -- links the caller's node into the registry.  Refused
/// after host_initialized, for a name that is already a cvar, and for a name
/// that is already a command.
#[no_mangle]
pub unsafe extern "C" fn Cmd_AddCommand(cmd_name: *const c_char, function: XCommand) {
    if host_initialized != 0 {        // because hunk allocation would get stomped
        Sys_Error(c"Cmd_AddCommand after host_initialized".as_ptr());
    }

    // fail if the command is a variable name
    if *Cvar_VariableString(cmd_name) != 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s: %s already defined as a var\n".as_ptr(),
            c"Cmd_AddCommand".as_ptr(),
            cmd_name,
        );
        return;
    }

    // fail if the command already exists
    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        if strcmp(cmd_name, (*cmd).name) == 0 {
            CON_Printf(
                PRINT_NORMAL,
                c"%s: %s already defined\n".as_ptr(),
                c"Cmd_AddCommand".as_ptr(),
                cmd_name,
            );
            return;
        }
        cmd = (*cmd).next;
    }

    cmd = Hunk_AllocName(
        core::mem::size_of::<CmdFunctionC>() as c_int,
        c"commands".as_ptr(),
    )
    .cast::<CmdFunctionC>();
    (*cmd).name = cmd_name;
    (*cmd).function = function;
    (*cmd).next = CMD_FUNCTIONS;
    CMD_FUNCTIONS = cmd;
}

/// `Cmd_Exists`
#[no_mangle]
pub unsafe extern "C" fn Cmd_Exists(cmd_name: *const c_char) -> c_int {
    let mut cmd = CMD_FUNCTIONS;

    while !cmd.is_null() {
        if strcmp(cmd_name, (*cmd).name) == 0 {
            return 1;
        }
        cmd = (*cmd).next;
    }

    0
}

/// `Cmd_AliasExists`
#[no_mangle]
pub unsafe extern "C" fn Cmd_AliasExists(alias_name: *const c_char) -> c_int {
    let mut a = CMD_ALIAS;

    while !a.is_null() {
        if q_strcasecmp(alias_name, (*a).name.as_ptr()) == 0 {
            return 1;
        }
        a = (*a).next;
    }

    0
}

/// `Cmd_CheckCommand` -- is this a command, a cvar or an alias?  The cvar half
/// walks the list directly, which is why this module carries the cvar_t layout.
#[no_mangle]
pub unsafe extern "C" fn Cmd_CheckCommand(partial: *const c_char) -> c_int {
    if partial.is_null() || *partial == 0 {
        return 0;
    }

    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        if strcmp(partial, (*cmd).name) == 0 {
            return 1;
        }
        cmd = (*cmd).next;
    }

    let mut var = Cvar_FindVarAfter(c"".as_ptr(), 0);
    while !var.is_null() {
        if strcmp(partial, (*var).name) == 0 {
            return 1;
        }
        var = (*var).next;
    }

    let mut a = CMD_ALIAS;
    while !a.is_null() {
        if strcmp(partial, (*a).name.as_ptr()) == 0 {
            return 1;
        }
        a = (*a).next;
    }

    0
}

/// `Cmd_MoveToFront`
#[no_mangle]
pub unsafe extern "C" fn Cmd_MoveToFront(name: *const c_char) {
    let mut cmd = CMD_FUNCTIONS;

    while !cmd.is_null() {
        let next = (*cmd).next;
        if !next.is_null() && strcmp(name, (*next).name) == 0 {
            // remove from the list
            (*cmd).next = (*next).next;
            // move to the front
            (*next).next = CMD_FUNCTIONS;
            CMD_FUNCTIONS = next;
            break;
        }
        cmd = (*cmd).next;
    }
}

/// `Cmd_ExecuteString` -- functions, then aliases, then cvars, then the
/// silently ignored legacy names, then the diagnostic.
#[no_mangle]
pub unsafe extern "C" fn Cmd_ExecuteString(text: *const c_char, src: c_int) {
    cmd_source = src;
    Cmd_TokenizeString(text);

    // execute the command line
    if Cmd_Argc() == 0 {
        return; // no tokens
    }

    // check functions
    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        if q_strcasecmp(CMD_ARGV[0], (*cmd).name) == 0 {
            match (*cmd).function {
                Some(f) => f(),
                None => {
                    // The C's H2W arm: an H2W client registers commands with a
                    // NULL handler so they forward to the server, and hwsv -
                    // the only H2W target here - reports it instead.  No
                    // command in this tree registers a NULL handler, and the
                    // non-H2W C has no check at all, calling through NULL; the
                    // port refuses loudly rather than continuing.
                    if Cmd_TargetIsH2W() != 0 {
                        CON_Printf(
                            PRINT_TERMONLY,
                            c"FIXME: command %s has NULL handler function\n".as_ptr(),
                            (*cmd).name,
                        );
                    } else {
                        Sys_Error(
                            c"%s: NULL handler for %s".as_ptr(),
                            c"Cmd_ExecuteString".as_ptr(),
                            (*cmd).name,
                        );
                    }
                }
            }
            return;
        }
        cmd = (*cmd).next;
    }

    // check alias
    let mut a = CMD_ALIAS;
    while !a.is_null() {
        if q_strcasecmp(CMD_ARGV[0], (*a).name.as_ptr()) == 0 {
            Cbuf_InsertText((*a).value);
            return;
        }
        a = (*a).next;
    }

    // check cvars
    if Cvar_Command() == 0 {
        // silently ignore removed legacy commands (from old configs)
        for legacy in LEGACY_CMDS.iter() {
            let lc = legacy.as_ptr().cast::<c_char>();
            if *lc == 0 {
                break;
            }
            if q_strcasecmp(CMD_ARGV[0], lc) == 0 {
                return;
            }
        }
        CON_Printf(
            PRINT_NORMAL,
            c"Unknown command \"%s\"\n".as_ptr(),
            Cmd_Argv(0),
        );
    }
}

/// `Cmd_CheckParm`
#[no_mangle]
pub unsafe extern "C" fn Cmd_CheckParm(parm: *const c_char) -> c_int {
    if parm.is_null() {
        Sys_Error(
            c"%s: null input\n".as_ptr(),
            c"Cmd_CheckParm".as_ptr(),
        );
    }

    let mut i: c_int = 1;
    while i < Cmd_Argc() {
        if q_strcasecmp(parm, Cmd_Argv(i)) == 0 {
            return i;
        }
        i += 1;
    }

    0
}

//============================================================================
// the client-only listing surface
//============================================================================

/// `ListCommands` -- compiled only where the target has the client-only list
/// surface; unreachable elsewhere, exactly as the C's symbols are.
#[no_mangle]
pub unsafe extern "C" fn ListCommands(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let mut i: c_int = 0;
    let prelen = if prefix.is_null() { 0 } else { strlen(prefix) };

    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        if prelen == 0 {
            CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*cmd).name);
            cmd = (*cmd).next;
            continue;
        }

        if q_strncasecmp(prefix, (*cmd).name, prelen) == 0 {
            if buf.is_null() {
                CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*cmd).name);
                cmd = (*cmd).next;
                continue;
            }

            if pos + i < MAX_MATCHES {
                *buf.offset((pos + i) as isize) = (*cmd).name;
            } else {
                break;
            }

            i += 1;
        }
        cmd = (*cmd).next;
    }

    i
}

/// `Cmd_List_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_List_f() {
    if Cmd_Argc() > 1 {
        ListCommands(Cmd_Argv(1), core::ptr::null_mut(), 0);
    } else {
        ListCommands(core::ptr::null(), core::ptr::null_mut(), 0);
    }
}

/// `ListCvars`
#[no_mangle]
pub unsafe extern "C" fn ListCvars(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let mut i: c_int = 0;
    let prelen = if prefix.is_null() { 0 } else { strlen(prefix) };

    let mut var = Cvar_FindVarAfter(c"".as_ptr(), 0);
    while !var.is_null() {
        if prelen == 0 {
            CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*var).name);
            var = (*var).next;
            continue;
        }

        if q_strncasecmp(prefix, (*var).name, prelen) == 0 {
            if buf.is_null() {
                CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*var).name);
                var = (*var).next;
                continue;
            }

            if pos + i < MAX_MATCHES {
                *buf.offset((pos + i) as isize) = (*var).name;
            } else {
                break;
            }

            i += 1;
        }
        var = (*var).next;
    }

    i
}

/// `Cmd_ListCvar_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_ListCvar_f() {
    if Cmd_Argc() > 1 {
        let filter = Cmd_Argv(1);
        let mut count: c_int = 0;
        let mut var = Cvar_FindVarAfter(c"".as_ptr(), 0);
        while !var.is_null() {
            if !strstr((*var).name, filter).is_null() {
                CON_Printf(
                    PRINT_NORMAL,
                    c" %s : %s\n".as_ptr(),
                    (*var).name,
                    (*var).string,
                );
                count += 1;
            }
            var = (*var).next;
        }
        CON_Printf(
            PRINT_NORMAL,
            c"%d cvar(s) matching \"%s\"\n".as_ptr(),
            count,
            filter,
        );
    } else {
        ListCvars(core::ptr::null(), core::ptr::null_mut(), 0);
    }
}

/// `ListAlias`
#[no_mangle]
pub unsafe extern "C" fn ListAlias(
    prefix: *const c_char,
    buf: *mut *const c_char,
    pos: c_int,
) -> c_int {
    let mut i: c_int = 0;
    let prelen = if prefix.is_null() { 0 } else { strlen(prefix) };

    let mut a = CMD_ALIAS;
    while !a.is_null() {
        if prelen == 0 {
            CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*a).name.as_ptr());
            a = (*a).next;
            continue;
        }

        if q_strncasecmp(prefix, (*a).name.as_ptr(), prelen) == 0 {
            if buf.is_null() {
                CON_Printf(PRINT_NORMAL, c" %s\n".as_ptr(), (*a).name.as_ptr());
                a = (*a).next;
                continue;
            }

            if pos + i < MAX_MATCHES {
                *buf.offset((pos + i) as isize) = (*a).name.as_ptr();
            } else {
                break;
            }

            i += 1;
        }
        a = (*a).next;
    }

    i
}

/// `Cmd_ListAlias_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_ListAlias_f() {
    if Cmd_Argc() > 1 {
        ListAlias(Cmd_Argv(1), core::ptr::null_mut(), 0);
    } else {
        ListAlias(core::ptr::null(), core::ptr::null_mut(), 0);
    }
}

/// `Cmd_WriteCommands_f` -- dumps the three lists to commands.txt.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_WriteCommands_f() {
    let fh = fopen(
        FS_MakePath(FS_USERDIR, core::ptr::null_mut(), c"commands.txt".as_ptr()),
        c"w".as_ptr(),
    );
    if fh.is_null() {
        return;
    }

    fprintf(fh, c"\n\nConsole Commands:\n".as_ptr());
    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        fprintf(fh, c"   %s\n".as_ptr(), (*cmd).name);
        cmd = (*cmd).next;
    }

    fprintf(fh, c"\n\nAlias Commands:\n".as_ptr());
    let mut a = CMD_ALIAS;
    while !a.is_null() {
        fprintf(fh, c"   %s :\n\t%s\n".as_ptr(), (*a).name.as_ptr(), (*a).value);
        a = (*a).next;
    }

    fprintf(fh, c"\n\nConsole Variables:\n".as_ptr());
    let mut var = Cvar_FindVarAfter(c"".as_ptr(), 0);
    while !var.is_null() {
        fprintf(fh, c"   %s\n".as_ptr(), (*var).name);
        var = (*var).next;
    }

    fclose(fh);
}

//============================================================================
// apropos / find, and the initialisation
//============================================================================

/// `Cmd_Apropos_f` -- one search across commands, aliases and cvars.
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_Apropos_f() {
    let substr = Cmd_Argv(1);
    let mut count: c_int = 0;

    if *substr == 0 {
        CON_Printf(
            PRINT_NORMAL,
            c"%s <substring> : search commands, aliases and cvars\n".as_ptr(),
            Cmd_Argv(0),
        );
        return;
    }

    let mut cmd = CMD_FUNCTIONS;
    while !cmd.is_null() {
        if !strstr((*cmd).name, substr).is_null() {
            CON_Printf(PRINT_NORMAL, c"  command %s\n".as_ptr(), (*cmd).name);
            count += 1;
        }
        cmd = (*cmd).next;
    }

    let mut a = CMD_ALIAS;
    while !a.is_null() {
        if !strstr((*a).name.as_ptr(), substr).is_null() {
            CON_Printf(PRINT_NORMAL, c"  alias   %s\n".as_ptr(), (*a).name.as_ptr());
            count += 1;
        }
        a = (*a).next;
    }

    let mut var = Cvar_FindVarAfter(c"".as_ptr(), 0);
    while !var.is_null() {
        if !strstr((*var).name, substr).is_null() {
            CON_Printf(
                PRINT_NORMAL,
                c"  cvar    %s \"%s\"\n".as_ptr(),
                (*var).name,
                (*var).string,
            );
            count += 1;
        }
        var = (*var).next;
    }

    CON_Printf(
        PRINT_NORMAL,
        c"%d match(es) for \"%s\"\n".as_ptr(),
        count,
        substr,
    );
}

/// `Cmd_Init` -- registers the console commands and the engine-defaults cvar.
#[no_mangle]
pub unsafe extern "C" fn Cmd_Init() {
    // register our commands
    Cmd_AddCommand(c"stuffcmds".as_ptr(), Some(Cmd_StuffCmds_f));
    Cmd_AddCommand(c"exec".as_ptr(), Some(Cmd_Exec_f));
    Cmd_AddCommand(c"echo".as_ptr(), Some(Cmd_Echo_f));
    Cmd_AddCommand(c"alias".as_ptr(), Some(Cmd_Alias_f));
    Cmd_AddCommand(c"unalias".as_ptr(), Some(Cmd_Unalias_f));
    Cmd_AddCommand(c"unaliasall".as_ptr(), Some(Cmd_Unaliasall_f));
    Cmd_AddCommand(c"wait".as_ptr(), Some(Cmd_Wait_f));

    // The client-only four.  Registering them in h2ded or hwsv would give
    // those targets commands the C does not have, which Cmd_Exists makes
    // observable -- so the target answers, not the port.
    if Cmd_TargetHasClientLists() != 0 {
        Cmd_AddCommand(c"commands".as_ptr(), Some(Cmd_WriteCommands_f));
        Cmd_AddCommand(c"cmdlist".as_ptr(), Some(Cmd_List_f));
        Cmd_AddCommand(c"cvarlist".as_ptr(), Some(Cmd_ListCvar_f));
        Cmd_AddCommand(c"aliaslist".as_ptr(), Some(Cmd_ListAlias_f));
    }

    // Registered outside the SERVERONLY guard above: these need only the
    // command/cvar lists, which h2ded has.
    Cmd_AddCommand(c"apropos".as_ptr(), Some(Cmd_Apropos_f));
    Cmd_AddCommand(c"find".as_ptr(), Some(Cmd_Apropos_f));

    Cvar_RegisterVariable(&raw mut CFG_ENGINEDEFAULTS);

    Cvar_Init();
}

/// `Cmd_Wait_f`
#[no_mangle]
#[allow(non_snake_case)]
unsafe extern "C" fn Cmd_Wait_f() {
    CMD_WAIT = 1;
}
