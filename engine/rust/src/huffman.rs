// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/hexenworld/shared/huffman.c.
//
// HexenWorld compresses every packet it sends through this codec: HuffEncode
// on the way out from NET_SendPacket, HuffDecode on the way in, ahead of
// net_message being handed to the message reader.  Two properties of the C
// original shape this port, and both were decided before it was written.
//
// 1. One archive, two allocation paths.  The C picks its tree allocation per
//    target: `USE_HUNKMEM` is 1 for hwsv, which builds the tree once at
//    startup and never frees the hunk under it, and 0 for the integrated
//    client, which defines H2W_INTEGRATED because CL_ClearState ->
//    Host_ClearMemory does Hunk_FreeToLowMark and takes a hunk-allocated tree
//    with it, leaving HuffTree dangling -- the C's own comment says the first
//    sequenced packet then walks freed memory.  This archive is built once and
//    linked into both, so it has to pick one: malloc, for the life of the
//    process, which is what the client path already does and what hwsv's hunk
//    amounts to.  The differential harness compiles the C both ways -- with
//    H2W_INTEGRATED and without, stubbing Hunk_AllocName in the second -- and
//    requires the two to produce identical bytes before either is compared
//    with this module, so the choice is shown to be safe rather than asserted.
//
// 2. Out-of-range bit reads on malformed input.  HuffDecode walks the tree
//    with `do { ... } while (tmp->zero)` and bounds only the outer loop, by
//    `tbits = inlen*8 - *in`.  GetBit has no bound of its own, so a crafted
//    `*in` -- and the header byte is attacker-controlled, because net_udp.c
//    runs HuffDecode on whatever arrived on the UDP socket -- sends the
//    decoder reading past the `inlen` bytes the caller declared; with
//    `*in == 0` it reads one byte past and then keeps descending through
//    whatever is there until it happens to land on a leaf.  For the one
//    production caller those bits are not unmapped memory: net_udp.c receives
//    into huffbuff[65536] with recvfrom capped at sizeof(net_message_buffer),
//    so what follows the declared payload is the stale remainder of an earlier
//    packet -- huffbuff is the send buffer too.  That makes the C defined but
//    unspecified there rather than undefined, and there is still no
//    byte-for-byte answer to reproduce, because the result depends on prior
//    traffic.  This module stops decoding at the declared end instead (see the
//    bound in HuffDecode), and the harness pins that case separately: the C arm
//    is run under a guard page -- a tighter allocation than production, which
//    is what makes the read observable at all -- and the bounded result is
//    checked against an independent decoder built from the C's own code table.
//
// Everything else is deliberately literal: Masks, PutBit, GetBit, FindTab,
// BuildTree's tie-breaking and node numbering, the Sys_Error fatal paths, the
// `>= inlen + 1` raw fallback in HuffEncode, and the exact order of the
// maxlen check and the store in HuffDecode.
//
// Residual risk worth naming: the C's tree is built from float arithmetic.
// On a target where the C compiler evaluates float in wider precision (x87 on
// 32-bit x86), `work[minat2]->freq + work[minat1]->freq` can round differently
// from this module's IEEE single addition, which would change a comparison in
// BuildTree and with it the whole tree.  Every build this gate runs is
// x86-64/SSE2, where FLT_EVAL_METHOD is 0 and the two agree.  A new 32-bit
// x86 target would need this checked, not assumed.

use core::ffi::{c_char, c_int, c_uint, c_void};

// `static const float HuffFreq[256]`, generated from hufffreq.h by build.rs.
include!(concat!(env!("OUT_DIR"), "/hufffreq.rs"));

/// The C's `512 * sizeof(huffnode_t)` allocation: 256 leaves plus 255 internal
/// nodes, with one spare.  HuffTree ends up at index 510.
const HUFF_NODES: usize = 512;

/// `huffnode_t`.  The C spells the trailing 3 bytes out as `pad[3]`; this is
/// the same 24-byte object with the same offsets, which the assertions below
/// pin.
#[repr(C)]
struct HuffNode {
    zero: *mut HuffNode,
    one: *mut HuffNode,
    freq: f32,
    val: u8,
    pad: [u8; 3],
}

/// `hufftab_t`.
#[derive(Clone, Copy)]
#[repr(C)]
struct HuffTab {
    bits: c_uint,
    len: c_int,
}

const _: () = {
    assert!(core::mem::offset_of!(HuffNode, zero) == 0);
    assert!(core::mem::offset_of!(HuffNode, one) == core::mem::size_of::<*mut HuffNode>());
    assert!(core::mem::offset_of!(HuffNode, freq) == 2 * core::mem::size_of::<*mut HuffNode>());
    assert!(core::mem::offset_of!(HuffNode, val) == 2 * core::mem::size_of::<*mut HuffNode>() + 4);
    assert!(core::mem::size_of::<HuffNode>() == 24);
    assert!(core::mem::offset_of!(HuffTab, bits) == 0);
    assert!(core::mem::offset_of!(HuffTab, len) == 4);
    assert!(core::mem::size_of::<HuffTab>() == 8);
};

// C: `static void *HuffMemBase`, `static huffnode_t *HuffTree` and
// `static hufftab_t HuffLookup[256]`, all filled by HuffInit and read
// afterwards.  The engine is single-threaded, as it is for the C.
static mut HUFF_MEM_BASE: *mut c_void = core::ptr::null_mut();
static mut HUFF_TREE: *mut HuffNode = core::ptr::null_mut();
static mut HUFF_LOOKUP: [HuffTab; 256] = [HuffTab { bits: 0, len: 0 }; 256];

/// `static unsigned char const Masks[8]`.
static MASKS: [u8; 8] = [0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80];

extern "C" {
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn malloc(size: usize) -> *mut c_void;
    fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}

//============================================================================
// bit plumbing
//============================================================================

/// `PutBit`.  `pos` is always >= 0 at every call site: it is
/// `bitat + len - j - 1` with `j < len`.
unsafe fn put_bit(buf: *mut u8, pos: c_int, bit: c_int) {
    let byte = (pos / 8) as usize;
    let mask = MASKS[(pos % 8) as usize];

    if bit != 0 {
        *buf.add(byte) |= mask;
    } else {
        *buf.add(byte) &= !mask;
    }
}

/// `GetBit`.  Unbounded, exactly as the C is: callers must keep `pos` inside
/// the buffer they handed in.  HuffDecode is the only caller and does.
unsafe fn get_bit(buf: *const u8, pos: c_int) -> c_int {
    let byte = (pos / 8) as usize;
    let mask = MASKS[(pos % 8) as usize];

    if (*buf.add(byte) & mask) != 0 {
        1
    } else {
        0
    }
}

//============================================================================
// tree
//============================================================================

/// `FindTab` -- walks the finished tree and fills HuffLookup.
unsafe fn find_tab(tmp: *mut HuffNode, len: c_int, bits: c_uint) {
    if tmp.is_null() {
        Sys_Error(c"no huff node".as_ptr());
    }

    if !(*tmp).zero.is_null() {
        if (*tmp).one.is_null() {
            Sys_Error(c"no one in node".as_ptr());
        }
        if len >= 32 {
            Sys_Error(c"compression screwd".as_ptr());
        }
        find_tab((*tmp).zero, len + 1, bits << 1);
        find_tab((*tmp).one, len + 1, (bits << 1) | 1);
        return;
    }

    HUFF_LOOKUP[(*tmp).val as usize].len = len;
    HUFF_LOOKUP[(*tmp).val as usize].bits = bits;
}

/// `BuildTree` -- the same 256 leaves in value order, the same 255 merges with
/// the same `<` comparisons, and the same node numbering: the internal nodes
/// are allocated from index 256 upward and HuffTree ends up on the last one.
unsafe fn build_tree(freq: &[f32; 256]) {
    // C: `USE_HUNKMEM` picks Hunk_AllocName for hwsv and malloc for the
    // integrated client; this archive is built once and linked into both, so
    // it uses malloc for both.  See the module comment.  The C's malloc path
    // also memsets the block, which this does too -- nothing read later
    // depends on it (every field the tree walk touches is written below), but
    // it keeps the pad bytes deterministic.
    let base = malloc(HUFF_NODES * core::mem::size_of::<HuffNode>()).cast::<HuffNode>();
    if base.is_null() {
        Sys_Error(c"Failed allocating memory for HuffTree".as_ptr());
    }
    memset(base.cast::<c_void>(), 0, HUFF_NODES * core::mem::size_of::<HuffNode>());
    HUFF_MEM_BASE = base.cast::<c_void>();

    let mut tmp = base;
    let mut work: [*mut HuffNode; 256] = [core::ptr::null_mut(); 256];

    for i in 0..256 {
        (*tmp).val = i as u8;
        (*tmp).freq = freq[i];
        (*tmp).zero = core::ptr::null_mut();
        (*tmp).one = core::ptr::null_mut();
        HUFF_LOOKUP[i].len = 0;
        work[i] = tmp;
        tmp = tmp.add(1);
    }

    for _ in 0..255 {
        let mut minat1: c_int = -1;
        let mut minat2: c_int = -1;
        let mut min1 = 1e30f32;
        let mut min2 = 1e30f32;

        for j in 0..256 {
            let w = work[j];
            if w.is_null() {
                continue;
            }
            if (*w).freq < min1 {
                minat2 = minat1;
                min2 = min1;
                minat1 = j as c_int;
                min1 = (*w).freq;
            } else if (*w).freq < min2 {
                minat2 = j as c_int;
                min2 = (*w).freq;
            }
        }

        if minat1 < 0 {
            Sys_Error(c"minat1: %d".as_ptr(), minat1);
        }
        if minat2 < 0 {
            Sys_Error(c"minat2: %d".as_ptr(), minat2);
        }

        (*tmp).zero = work[minat2 as usize];
        (*tmp).one = work[minat1 as usize];
        (*tmp).freq = (*work[minat2 as usize]).freq + (*work[minat1 as usize]).freq;
        (*tmp).val = 0xff;
        work[minat1 as usize] = tmp;
        work[minat2 as usize] = core::ptr::null_mut();
        tmp = tmp.add(1);
    }

    // Last incrementation in the loop above wasn't used.
    let tree = tmp.sub(1);
    HUFF_TREE = tree;
    find_tab(tree, 0, 0);
}

//============================================================================
// the ABI
//============================================================================

/// `HuffInit` -- called once per process from HWCL_InitNet (cl_hw.c) and
/// SV_InitLocal (sv_main.c).
#[no_mangle]
pub unsafe extern "C" fn HuffInit() {
    build_tree(&HUFF_FREQ);
}

/// `HuffDecode`.
///
/// `in` is the received packet: `in[0]` is the padding-bit count (or 0xff for
/// the raw fallback), `in[1..inlen]` is the payload.  At most `maxlen` bytes
/// are written to `out`; `*outlen` reports the decoded length, which can
/// exceed `maxlen` when the caller's buffer was too small to hold it.
#[no_mangle]
pub unsafe extern "C" fn HuffDecode(
    input: *const u8,
    out: *mut u8,
    inlen: c_int,
    outlen: *mut c_int,
    maxlen: c_int,
) {
    let inlen = inlen - 1;

    if inlen < 0 {
        *outlen = 0;
        return;
    }

    if *input == 0xff {
        if inlen > maxlen {
            memcpy(
                out.cast::<c_void>(),
                input.add(1).cast::<c_void>(),
                maxlen as usize,
            );
        } else if inlen != 0 {
            memcpy(
                out.cast::<c_void>(),
                input.add(1).cast::<c_void>(),
                inlen as usize,
            );
        }
        *outlen = inlen;
        return;
    }

    let data_bits = inlen * 8;
    let tbits = data_bits - *input as c_int;
    let mut bits: c_int = 0;
    *outlen = 0;
    let mut out = out;

    while bits < tbits {
        let mut tmp = HUFF_TREE;

        loop {
            // The one deliberate divergence from the C.  The C's GetBit has
            // no bound of its own, so a crafted padding count (`*input`
            // smaller than the real one, down to 0) leaves the decoder
            // mid-code at the end of the payload and it reads on into
            // whatever follows the buffer -- for as many bytes as it takes to
            // reach a leaf.  The header byte is attacker-controlled here:
            // net_udp.c hands HuffDecode whatever arrived on the socket.  So
            // the read is bounded by the `inlen` bytes the caller declared,
            // and a code that does not complete inside them is abandoned
            // rather than completed from whatever follows -- which in
            // production is the stale tail of huffbuff, not unmapped memory,
            // so the C is unspecified there rather than undefined and has no
            // portable answer to match.
            if bits >= data_bits {
                return;
            }

            tmp = if get_bit(input.add(1), bits) != 0 {
                (*tmp).one
            } else {
                (*tmp).zero
            };
            bits += 1;

            if (*tmp).zero.is_null() {
                break;
            }
        }

        *outlen += 1;
        if *outlen > maxlen {
            return; // out[maxlen - 1] is written already
        }
        *out = (*tmp).val;
        out = out.add(1);
    }
}

/// `HuffEncode`.
#[no_mangle]
pub unsafe extern "C" fn HuffEncode(
    input: *const u8,
    out: *mut u8,
    inlen: c_int,
    outlen: *mut c_int,
) {
    let mut bitat: c_int = 0;

    for i in 0..inlen {
        let tab = HUFF_LOOKUP[*input.add(i as usize) as usize];
        let mut t = tab.bits;

        for j in 0..tab.len {
            put_bit(out.add(1), bitat + tab.len - j - 1, (t & 1) as c_int);
            t >>= 1;
        }

        bitat += tab.len;
    }

    *outlen = 1 + (bitat + 7) / 8;
    *out = (8 * (*outlen - 1) - bitat) as u8;

    if *outlen >= inlen + 1 {
        // Compression did not help: send the payload raw behind 0xff as the
        // padding count, which HuffDecode recognises.
        *out = 0xff;
        // A negative inlen would make the C's memcpy take a huge size_t.
        // Netchan and net_udp never pass one -- both encode a length they
        // have just read back from a buffer -- so the C is being spared an
        // unreachable undefined behaviour here, not given a different answer.
        if inlen > 0 {
            memcpy(
                out.add(1).cast::<c_void>(),
                input.cast::<c_void>(),
                inlen as usize,
            );
        }
        *outlen = inlen + 1;
    }
}

//============================================================================
// accessors for the differential harness
//============================================================================

// The harness compares this module's state with the C's, and neither the
// frequency table nor HuffLookup is reachable from outside the C translation
// unit.  These three accessors expose the Rust side so the comparison can be
// made, in the same spirit as the layout accessors the sizebuf, msg_io and
// info_str ports export.

/// `HUFF_FREQ[index]`, so the harness can compare all 256 values against
/// `static const float HuffFreq[256]` bit for bit -- BuildTree compares with
/// `<`, so a single differing float would rewrite every encoded byte.
#[no_mangle]
pub extern "C" fn Huffman_freq(index: usize) -> f32 {
    HUFF_FREQ[index]
}

/// `HuffLookup[val].len`, filled by FindTab from the built tree.
#[no_mangle]
pub extern "C" fn Huffman_lookup_len(val: usize) -> c_int {
    unsafe { HUFF_LOOKUP[val].len }
}

/// `HuffLookup[val].bits`, filled by FindTab from the built tree.
#[no_mangle]
pub extern "C" fn Huffman_lookup_bits(val: usize) -> c_uint {
    unsafe { HUFF_LOOKUP[val].bits }
}