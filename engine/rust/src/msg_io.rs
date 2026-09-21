// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/msg_io.c -- the little-endian message
// reader/writer that both Hexen II and HexenWorld speak through.
//
// Three properties of the C original shape this port and are not obvious from
// the file itself.
//
// 1. The C file is compiled twice, with different code.  hwsv compiles it with
//    -DH2W, and that is the only build which defines MSG_WriteAngle16,
//    MSG_WriteUsercmd, MSG_ReadStringLine, MSG_ReadAngle16 and
//    MSG_ReadUsercmd; glhexen2 and h2ded compile it without, so those five
//    symbols do not exist in a Hexen II binary at all.  The consolidated
//    static library is built once and linked into all three, so it cannot hold
//    two definitions of one symbol, and this module defines the union instead.
//
//    That is safe because the H2W-only functions are purely additive.  Nothing
//    compiled without H2W calls any of them: the only callers of the two that
//    touch a usercmd_t are hexenworld/server/sv_user.c and sv_ents.c, and
//    MSG_ReadStringLine's only caller is hexenworld/server/sv_main.c -- all
//    three are hwsv-only.  The HexenWorld client half that *is* linked into
//    glhexen2 (hexen2/cl_hw.c, built with H2W_INTEGRATED) writes and reads its
//    usercmds by hand, precisely so it does not need the H2W msg_io.
//
// 2. MSG_WriteUsercmd / MSG_ReadUsercmd therefore take the *HexenWorld*
//    usercmd_t, not Hexen II's.  The two structs have the same size (28 bytes)
//    and different layouts -- Hexen II is {vec3_t viewangles; float
//    forwardmove, sidemove, upmove; byte lightlevel} with forwardmove at
//    offset 12, HexenWorld is {byte msec; vec3_t angles; short forwardmove,
//    sidemove, upmove; byte buttons, impulse, light_level} with forwardmove at
//    offset 16.  UsercmdC below is the HexenWorld one, because that is what
//    every caller passes; its offsets are asserted at compile time here and
//    checked against the C by msg_io/tests/diff_harness.c.
//
// 3. The PARANOID range checks at the top of the C writers are dead code in
//    every configuration this tree can produce: h2config.h ends its option
//    list with an unconditional `#undef PARANOID`, and no CMake target, flake
//    attribute or build script passes -DPARANOID.  They are deliberately not
//    ported.  If PARANOID is ever revived, the three checks are range tests on
//    the argument of MSG_WriteChar/MSG_WriteByte/MSG_WriteShort, fatal through
//    Sys_Error("%s: range error", __thisfunc__).
//
// The reading half keeps the C's single-piece state: a caller-owned buffer
// pointer (defaulting to the C global `net_message`), a read cursor and a
// sticky bad-read flag, all three reachable from C.  cl_hw.c assigns
// msg_readcount directly to rewind a packet, so these are real exported
// globals and not module-private state.

use core::ffi::{c_char, c_float, c_int, c_void};

use crate::sizebuf::SizeBufC;

extern "C" {
    // Provided by the sizebuf port when that feature is on, and by the C
    // original when it is off.  Either way this is the same ABI, and this
    // module has no opinion about which one is linked.
    fn SZ_GetSpace(buf: *mut SizeBufC, length: c_int) -> *mut c_void;
    fn SZ_Write(buf: *mut SizeBufC, data: *const c_void, length: c_int);
    fn strlen(s: *const c_char) -> usize;
    fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;

    /// Defined by hexen2/net_main.c for the Hexen II targets and by
    /// hexenworld/shared/net_udp.c for hwsv.  Every target that links this
    /// module defines it.
    static mut net_message: SizeBufC;
}

//============================================================================
// usercmd_t, HexenWorld flavour
//============================================================================

/// `usercmd_t` from engine/hexenworld/shared/protocol.h, the one hwsv passes
/// to MSG_WriteUsercmd/MSG_ReadUsercmd.  See the module comment: Hexen II's
/// usercmd_t is the same size with different offsets, and no Hexen II caller
/// reaches these two functions.
#[repr(C)]
pub struct UsercmdC {
    pub msec: u8,
    pub angles: [c_float; 3],
    pub forwardmove: i16,
    pub sidemove: i16,
    pub upmove: i16,
    pub buttons: u8,
    pub impulse: u8,
    pub light_level: u8,
}

const _: () = {
    assert!(core::mem::offset_of!(UsercmdC, msec) == 0);
    assert!(core::mem::offset_of!(UsercmdC, angles) == 4);
    assert!(core::mem::offset_of!(UsercmdC, forwardmove) == 16);
    assert!(core::mem::offset_of!(UsercmdC, sidemove) == 18);
    assert!(core::mem::offset_of!(UsercmdC, upmove) == 20);
    assert!(core::mem::offset_of!(UsercmdC, buttons) == 22);
    assert!(core::mem::offset_of!(UsercmdC, impulse) == 23);
    assert!(core::mem::offset_of!(UsercmdC, light_level) == 24);
    assert!(core::mem::size_of::<UsercmdC>() == 28);
    assert!(core::mem::align_of::<UsercmdC>() == 4);
};

// These accessors let the C differential harness check the layout against the
// real HexenWorld struct without restating Rust's assumptions in C, as the
// sizebuf and link_ops ports do.
#[no_mangle]
pub extern "C" fn UsercmdC_sizeof() -> usize {
    core::mem::size_of::<UsercmdC>()
}

#[no_mangle]
pub extern "C" fn UsercmdC_alignof() -> usize {
    core::mem::align_of::<UsercmdC>()
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_msec() -> usize {
    core::mem::offset_of!(UsercmdC, msec)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_angles() -> usize {
    core::mem::offset_of!(UsercmdC, angles)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_forwardmove() -> usize {
    core::mem::offset_of!(UsercmdC, forwardmove)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_sidemove() -> usize {
    core::mem::offset_of!(UsercmdC, sidemove)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_upmove() -> usize {
    core::mem::offset_of!(UsercmdC, upmove)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_buttons() -> usize {
    core::mem::offset_of!(UsercmdC, buttons)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_impulse() -> usize {
    core::mem::offset_of!(UsercmdC, impulse)
}

#[no_mangle]
pub extern "C" fn UsercmdC_offsetof_light_level() -> usize {
    core::mem::offset_of!(UsercmdC, light_level)
}

// CM_* from engine/hexenworld/shared/protocol.h: the usercmd bit flags.
const CM_ANGLE1: c_int = 1 << 0;
const CM_ANGLE3: c_int = 1 << 1;
const CM_FORWARD: c_int = 1 << 2;
const CM_SIDE: c_int = 1 << 3;
const CM_UP: c_int = 1 << 4;
const CM_BUTTONS: c_int = 1 << 5;
const CM_IMPULSE: c_int = 1 << 6;
const CM_MSEC: c_int = 1 << 7;

//============================================================================
// writing
//============================================================================

/// The C `(int)` conversion the quantizing writers use.
///
/// C leaves a float-to-int conversion whose value cannot be represented
/// undefined (C11 6.3.1.4).  x86-64 answers an out-of-range conversion with
/// INT_MIN (cvttss2si's "integer indefinite"); AArch64 saturates.  Rust's `as`
/// saturates, so this matches the C exactly on AArch64 and matches it on
/// x86-64 for every input whose scaled magnitude stays below 2^31.
///
/// The only inputs where the two disagree are positive ones: +Inf, a coord
/// whose |f| * 8 reaches 2^31, or an angle past the 2^31 fixed-point range.
/// No caller can produce one -- MSG_WriteCoord receives world coordinates and
/// MSG_WriteAngle an `anglemod` result in [0, 360) -- so the port takes the
/// defined behaviour rather than reproducing one platform's manifestation of
/// undefined behaviour.  The differential harness states the same boundary and
/// covers NaN and -Inf, which both implementations agree on.
#[inline]
fn quantize(f: c_float) -> c_int {
    f as c_int
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteChar(sb: *mut SizeBufC, c: c_int) {
    let buf = SZ_GetSpace(sb, 1).cast::<u8>();
    *buf = c as u8;
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteByte(sb: *mut SizeBufC, c: c_int) {
    let buf = SZ_GetSpace(sb, 1).cast::<u8>();
    *buf = c as u8;
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteShort(sb: *mut SizeBufC, c: c_int) {
    let buf = SZ_GetSpace(sb, 2).cast::<u8>();
    *buf = (c & 0xff) as u8;
    // C's `c >> 8` is an arithmetic shift, so a negative c yields 0xff here
    // once truncated to a byte.  Rust's signed shift is arithmetic too.
    *buf.add(1) = (c >> 8) as u8;
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteLong(sb: *mut SizeBufC, c: c_int) {
    let buf = SZ_GetSpace(sb, 4).cast::<u8>();
    *buf = (c & 0xff) as u8;
    *buf.add(1) = ((c >> 8) & 0xff) as u8;
    *buf.add(2) = ((c >> 16) & 0xff) as u8;
    *buf.add(3) = (c >> 24) as u8;
}

/// The C writes `dat.l` after `LittleLong`, i.e. the four bytes of the float's
/// bit pattern in little-endian order on any host.  `to_le_bytes` is that,
/// spelled portably.
#[no_mangle]
pub unsafe extern "C" fn MSG_WriteFloat(sb: *mut SizeBufC, f: c_float) {
    let bytes = f.to_bits().to_le_bytes();
    SZ_Write(sb, bytes.as_ptr().cast::<c_void>(), 4);
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteString(sb: *mut SizeBufC, s: *const c_char) {
    if s.is_null() {
        SZ_Write(sb, c"".as_ptr().cast::<c_void>(), 1);
    } else {
        SZ_Write(sb, s.cast::<c_void>(), strlen(s) as c_int + 1);
    }
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteCoord(sb: *mut SizeBufC, f: c_float) {
    if f >= 0.0 {
        MSG_WriteShort(sb, quantize(f * 8.0 + 0.5));
    } else {
        MSG_WriteShort(sb, quantize(f * 8.0 - 0.5));
    }
}

/// LordHavoc's change is load-bearing: this rounds to nearest rather than
/// toward zero, and both halves of the round-to-nearest happen in the sign
/// branch.
#[no_mangle]
pub unsafe extern "C" fn MSG_WriteAngle(sb: *mut SizeBufC, f: c_float) {
    if f >= 0.0 {
        MSG_WriteByte(sb, quantize(f * (256.0 / 360.0) + 0.5) & 255);
    } else {
        MSG_WriteByte(sb, quantize(f * (256.0 / 360.0) - 0.5) & 255);
    }
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteAngle16(sb: *mut SizeBufC, f: c_float) {
    if f >= 0.0 {
        MSG_WriteShort(sb, quantize(f * (65536.0 / 360.0) + 0.5) & 65535);
    } else {
        MSG_WriteShort(sb, quantize(f * (65536.0 / 360.0) - 0.5) & 65535);
    }
}

#[no_mangle]
pub unsafe extern "C" fn MSG_WriteUsercmd(
    buf: *mut SizeBufC,
    cmd: *const UsercmdC,
    long_msg: c_int,
) {
    let mut bits: c_int = 0;
    if (*cmd).angles[0] != 0.0 {
        bits |= CM_ANGLE1;
    }
    if (*cmd).angles[2] != 0.0 {
        bits |= CM_ANGLE3;
    }
    if (*cmd).forwardmove != 0 {
        bits |= CM_FORWARD;
    }
    if (*cmd).sidemove != 0 {
        bits |= CM_SIDE;
    }
    if (*cmd).upmove != 0 {
        bits |= CM_UP;
    }
    if (*cmd).buttons != 0 {
        bits |= CM_BUTTONS;
    }
    if (*cmd).impulse != 0 {
        bits |= CM_IMPULSE;
    }
    if (*cmd).msec != 0 {
        bits |= CM_MSEC;
    }

    MSG_WriteByte(buf, bits);
    if long_msg != 0 {
        MSG_WriteByte(buf, (*cmd).light_level as c_int);
    }

    if bits & CM_ANGLE1 != 0 {
        MSG_WriteAngle16(buf, (*cmd).angles[0]);
    }
    MSG_WriteAngle16(buf, (*cmd).angles[1]);
    if bits & CM_ANGLE3 != 0 {
        MSG_WriteAngle16(buf, (*cmd).angles[2]);
    }

    if bits & CM_FORWARD != 0 {
        MSG_WriteChar(buf, quantize((*cmd).forwardmove as c_float * 0.25));
    }
    if bits & CM_SIDE != 0 {
        MSG_WriteChar(buf, quantize((*cmd).sidemove as c_float * 0.25));
    }
    if bits & CM_UP != 0 {
        MSG_WriteChar(buf, quantize((*cmd).upmove as c_float * 0.25));
    }

    if bits & CM_BUTTONS != 0 {
        MSG_WriteByte(buf, (*cmd).buttons as c_int);
    }
    if bits & CM_IMPULSE != 0 {
        MSG_WriteByte(buf, (*cmd).impulse as c_int);
    }
    if bits & CM_MSEC != 0 {
        MSG_WriteByte(buf, (*cmd).msec as c_int);
    }
}

//============================================================================
// reading
//============================================================================

/// Read cursor into the selected message.  `cl_hw.c` writes this directly.
#[no_mangle]
pub static mut msg_readcount: c_int = 0;

/// Set by any read that runs off the end of the message, and sticky until the
/// next MSG_BeginReading/MSG_BeginReadingFrom.  qboolean is a four-byte C int
/// here, so this must not be a Rust bool.
#[no_mangle]
pub static mut msg_badread: c_int = 0;

/// The buffer the readers consume.  Defaults to `net_message`, so a read that
/// arrives before any MSG_BeginReading behaves as it did when net_message was
/// referenced directly, rather than dereferencing NULL.
static mut MSG_READBUF: *mut SizeBufC = &raw mut net_message;

#[no_mangle]
pub unsafe extern "C" fn MSG_BeginReadingFrom(message: *mut SizeBufC) {
    MSG_READBUF = message;
    msg_readcount = 0;
    msg_badread = 0;
}

#[no_mangle]
pub unsafe extern "C" fn MSG_BeginReading() {
    MSG_BeginReadingFrom(&raw mut net_message);
}

// Returns -1 and sets msg_badread if no more characters are available.
#[no_mangle]
pub unsafe extern "C" fn MSG_ReadChar() -> c_int {
    let msg = MSG_READBUF;

    if msg_readcount + 1 > (*msg).cursize {
        msg_badread = 1;
        return -1;
    }

    let c = *(*msg).data.add(msg_readcount as usize) as i8 as c_int;
    msg_readcount += 1;

    c
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadByte() -> c_int {
    let msg = MSG_READBUF;

    if msg_readcount + 1 > (*msg).cursize {
        msg_badread = 1;
        return -1;
    }

    let c = *(*msg).data.add(msg_readcount as usize) as c_int;
    msg_readcount += 1;

    c
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadShort() -> c_int {
    let msg = MSG_READBUF;

    if msg_readcount + 2 > (*msg).cursize {
        msg_badread = 1;
        return -1;
    }

    // C builds the value in int and then casts to short, so the result is
    // sign-extended; the byte arithmetic below is the same two's-complement
    // wrap, spelled in u16 to avoid an overflow.
    let data = (*msg).data;
    let i = msg_readcount as usize;
    let c = (u16::from(*data.add(i)) | (u16::from(*data.add(i + 1)) << 8)) as i16 as c_int;

    msg_readcount += 2;

    c
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadLong() -> c_int {
    let msg = MSG_READBUF;

    if msg_readcount + 4 > (*msg).cursize {
        msg_badread = 1;
        return -1;
    }

    // C sums four int terms whose top one overflows the sign bit for byte
    // values above 127, which GCC and Clang both wrap.  Going through u32 is
    // that same wrap without the overflow.
    let data = (*msg).data;
    let i = msg_readcount as usize;
    let c = (u32::from(*data.add(i))
        | (u32::from(*data.add(i + 1)) << 8)
        | (u32::from(*data.add(i + 2)) << 16)
        | (u32::from(*data.add(i + 3)) << 24)) as c_int;

    msg_readcount += 4;

    c
}

/// There is deliberately no bounds check here, and the C has none either: a
/// truncated float read takes the four bytes past the end of the message and
/// leaves msg_badread clear.  Callers detect that by the reader that follows.
#[no_mangle]
pub unsafe extern "C" fn MSG_ReadFloat() -> c_float {
    let msg = MSG_READBUF;
    let data = (*msg).data;
    let i = msg_readcount as usize;

    let mut b = [0u8; 4];
    b[0] = *data.add(i);
    b[1] = *data.add(i + 1);
    b[2] = *data.add(i + 2);
    b[3] = *data.add(i + 3);
    msg_readcount += 4;

    c_float::from_bits(u32::from_le_bytes(b))
}

/// The two string readers each own a 2048-byte static buffer, exactly as the C
/// does -- so they do not clobber one another -- and the returned pointer is
/// only valid until the next call to the same function.  The loop bound is the
/// C's `l < sizeof(string) - 1`, which leaves room for the terminator at
/// string[2047] and never overruns.
static mut MSG_STRING: [c_char; 2048] = [0; 2048];
static mut MSG_STRING_LINE: [c_char; 2048] = [0; 2048];

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadString() -> *const c_char {
    let string = (&raw mut MSG_STRING).cast::<c_char>();
    let mut l: usize = 0;

    loop {
        let c = MSG_ReadByte();
        if c == -1 || c == 0 {
            break;
        }
        *string.add(l) = c as c_char;
        l += 1;
        if l >= 2048 - 1 {
            break;
        }
    }

    *string.add(l) = 0;

    string
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadStringLine() -> *const c_char {
    let string = (&raw mut MSG_STRING_LINE).cast::<c_char>();
    let mut l: usize = 0;

    loop {
        let c = MSG_ReadByte();
        if c == -1 || c == 0 || c == b'\n' as c_int {
            break;
        }
        *string.add(l) = c as c_char;
        l += 1;
        if l >= 2048 - 1 {
            break;
        }
    }

    *string.add(l) = 0;

    string
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadCoord() -> c_float {
    MSG_ReadShort() as c_float * (1.0 / 8.0)
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadAngle() -> c_float {
    MSG_ReadChar() as c_float * (360.0 / 256.0)
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadAngle16() -> c_float {
    MSG_ReadShort() as c_float * (360.0 / 65536.0)
}

#[no_mangle]
pub unsafe extern "C" fn MSG_ReadUsercmd(move_: *mut UsercmdC, long_msg: c_int) {
    // The C memsets sizeof(*move) -- 28 bytes of the HexenWorld struct, and
    // the same 28 the Hexen II one occupies, so the memset size does not
    // depend on which struct the caller had in mind.
    memset(
        move_.cast::<c_void>(),
        0,
        core::mem::size_of::<UsercmdC>(),
    );

    let bits = MSG_ReadByte();
    if long_msg != 0 {
        (*move_).light_level = MSG_ReadByte() as u8;
    } else {
        (*move_).light_level = 0;
    }

    // read current angles
    if bits & CM_ANGLE1 != 0 {
        (*move_).angles[0] = MSG_ReadAngle16();
    } else {
        (*move_).angles[0] = 0.0;
    }
    (*move_).angles[1] = MSG_ReadAngle16();
    if bits & CM_ANGLE3 != 0 {
        (*move_).angles[2] = MSG_ReadAngle16();
    } else {
        (*move_).angles[2] = 0.0;
    }

    // read movement
    if bits & CM_FORWARD != 0 {
        (*move_).forwardmove = (MSG_ReadChar() * 4) as i16;
    }
    if bits & CM_SIDE != 0 {
        (*move_).sidemove = (MSG_ReadChar() * 4) as i16;
    }
    if bits & CM_UP != 0 {
        (*move_).upmove = (MSG_ReadChar() * 4) as i16;
    }

    // read buttons
    if bits & CM_BUTTONS != 0 {
        (*move_).buttons = MSG_ReadByte() as u8;
    } else {
        (*move_).buttons = 0;
    }

    if bits & CM_IMPULSE != 0 {
        (*move_).impulse = MSG_ReadByte() as u8;
    } else {
        (*move_).impulse = 0;
    }

    // read time to run command
    if bits & CM_MSEC != 0 {
        (*move_).msec = MSG_ReadByte() as u8;
    } else {
        (*move_).msec = 0;
    }
}
