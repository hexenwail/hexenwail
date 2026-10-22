// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/huffman.txt, the frozen output of
// engine/hexenworld/shared/huffman.c, against engine/rust/src/huffman.rs.  No C
// compiler: the fixture is plain text.  Format and regeneration are in
// tests/fixtures/README.md.
//
// Every decode runs in a forked child with the packet's last byte against an
// unmapped page, as the retired differential harness ran it.  The C reads past
// a short packet on a crafted header byte; the Rust must not.  A Rust that
// does dies on the guard page (SIGSEGV), which the replay reports as a failure
// naming the fixture line, instead of reading whatever happens to follow.
//
//   dec  the C survived its guard page: the Rust must match it exactly.
//   ovr  the C overran: no in-bounds C answer exists, so the fixture holds the
//        C's decode with a 64-byte zero tail, and the Rust's answer must be a
//        strict prefix of it (it stops at the declared end, the C does not).

use huffman_rs::huffman::{
    HuffDecode, HuffEncode, HuffInit, Huffman_freq, Huffman_lookup_bits, Huffman_lookup_len,
};
use std::ffi::c_void;
use std::sync::Once;

const FIXTURE: &str = include_str!("fixtures/huffman.txt");
const MAX_OUT: usize = 2048;
const POISON: u8 = 0xa5;

extern "C" {
    fn fork() -> i32;
    fn waitpid(pid: i32, status: *mut i32, options: i32) -> i32;
    fn _exit(code: i32) -> !;
    fn alarm(seconds: u32) -> u32;
    fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut c_void;
    fn mprotect(addr: *mut c_void, len: usize, prot: i32) -> i32;
    fn munmap(addr: *mut c_void, len: usize) -> i32;
    fn sysconf(name: i32) -> i64;
}

// Linux values (the gate runs on Linux only, like the retired harness).
const PROT_NONE: i32 = 0;
const PROT_RW: i32 = 1 | 2;
const MAP_SHARED: i32 = 0x01;
const MAP_PRIVATE_ANON: i32 = 0x02 | 0x20;
const MAP_SHARED_ANON: i32 = MAP_SHARED | 0x20;
const SC_PAGESIZE: i32 = 30;

static INIT: Once = Once::new();

fn init() {
    // HuffInit builds the tree once per process, as both callers do.
    INIT.call_once(|| unsafe { HuffInit() });
}

#[repr(C)]
struct Shared {
    survived: i32,
    outlen: i32,
    out: [u8; MAX_OUT],
}

enum Decoded {
    Done { outlen: i32, out: Vec<u8> },
    Died { signal: i32 },
}

/// Decodes `pkt` in a child whose copy of the packet ends exactly at an
/// unmapped page.  Everything the child stored is returned, poison included.
fn guard_decode(pkt: &[u8], maxlen: i32) -> Decoded {
    unsafe {
        let page = sysconf(SC_PAGESIZE) as usize;
        let region = mmap(core::ptr::null_mut(), 2 * page, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
        assert_ne!(region as isize, -1, "mmap");
        assert_eq!(mprotect(region.cast::<u8>().add(page).cast(), page, PROT_NONE), 0);
        let shared = mmap(
            core::ptr::null_mut(),
            core::mem::size_of::<Shared>(),
            PROT_RW,
            MAP_SHARED_ANON,
            -1,
            0,
        )
        .cast::<Shared>();
        assert_ne!(shared as isize, -1, "mmap(shared)");
        (*shared).survived = 0;
        (*shared).outlen = -1;
        (*shared).out = [POISON; MAX_OUT];

        let slot = region.cast::<u8>().add(page - pkt.len());
        core::ptr::copy_nonoverlapping(pkt.as_ptr(), slot, pkt.len());

        let pid = fork();
        assert!(pid >= 0, "fork");
        if pid == 0 {
            alarm(10);
            let out = core::ptr::addr_of_mut!((*shared).out).cast::<u8>();
            HuffDecode(slot, out, pkt.len() as i32, &raw mut (*shared).outlen, maxlen);
            (*shared).survived = 1;
            _exit(0);
        }
        let mut status = 0i32;
        assert_eq!(waitpid(pid, &mut status, 0), pid, "waitpid");
        let result = if status & 0x7f != 0 || (status >> 8) & 0xff != 0 || (*shared).survived == 0 {
            Decoded::Died { signal: status & 0x7f }
        } else {
            Decoded::Done { outlen: (*shared).outlen, out: (*shared).out.to_vec() }
        };
        munmap(shared.cast(), core::mem::size_of::<Shared>());
        munmap(region, 2 * page);
        result
    }
}

fn hex(s: &str) -> Vec<u8> {
    if s == "-" {
        return Vec::new();
    }
    assert_eq!(s.len() % 2, 0, "odd hex data {s:?}");
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
}

/// `-`, `rep:XX:N` (N copies of one byte) or plain hex.
fn payload(s: &str) -> Vec<u8> {
    if let Some(rest) = s.strip_prefix("rep:") {
        let (b, n) = rest.split_once(':').unwrap();
        return vec![u8::from_str_radix(b, 16).unwrap(); n.parse().unwrap()];
    }
    hex(s)
}

/// What the Rust stored must be exactly `want` followed by untouched poison.
fn check_stored(at: &str, outlen: i32, out: &[u8], maxlen: i32, want: &[u8]) {
    let stored = outlen.clamp(0, maxlen.max(0)) as usize;
    assert_eq!(stored, want.len(), "{at}: fixture stores {} bytes, outlen {outlen} maxlen {maxlen}", want.len());
    assert_eq!(&out[..stored], want, "{at}: decoded bytes");
    assert!(
        out[stored..].iter().all(|&b| b == POISON),
        "{at}: the decoder wrote beyond the {stored} bytes it reports"
    );
}

fn survived(at: &str, d: Decoded) -> (i32, Vec<u8>) {
    match d {
        Decoded::Done { outlen, out } => (outlen, out),
        Decoded::Died { signal } => panic!(
            "{at}: the Rust decode died with signal {signal}: it read past the packet it was given, \
             or hung (14 = SIGALRM)"
        ),
    }
}

#[derive(Default)]
struct Counts {
    freq: usize,
    code: usize,
    enc: usize,
    enc_raw: usize,
    enc_packed: usize,
    decl: usize,
    dec: usize,
    dec_raw: usize,
    ovr: usize,
    under: usize,
    headers: std::collections::BTreeSet<u8>,
}

#[test]
fn replay_c_fixtures() {
    init();
    let mut n = Counts::default();
    let (mut last_payload, mut last_pkt): (Vec<u8>, Vec<u8>) = (Vec::new(), Vec::new());

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("huffman.txt:{}: {line}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        match f[0] {
            "freq" => {
                let i: usize = f[1].parse().unwrap();
                let want = u32::from_str_radix(f[2], 16).unwrap();
                assert_eq!(Huffman_freq(i).to_bits(), want, "{at}");
                n.freq += 1;
            }
            "code" => {
                let i: usize = f[1].parse().unwrap();
                assert_eq!(Huffman_lookup_len(i), f[2].parse::<i32>().unwrap(), "{at}: length");
                assert_eq!(Huffman_lookup_bits(i), u32::from_str_radix(f[3], 16).unwrap(), "{at}: bits");
                n.code += 1;
            }
            "enc" => {
                let data = payload(f[1]);
                let want_len: i32 = f[2].parse().unwrap();
                let want_pkt = if f[3] == "raw" {
                    n.enc_raw += 1;
                    [&[0xffu8][..], &data].concat()
                } else {
                    n.enc_packed += 1;
                    hex(f[3])
                };
                assert_eq!(want_pkt.len() as i32, want_len, "{at}: fixture length");

                let input = data.clone();
                let mut out = vec![POISON; MAX_OUT];
                let mut outlen = -1;
                unsafe { HuffEncode(input.as_ptr(), out.as_mut_ptr(), input.len() as i32, &mut outlen) };
                assert_eq!(input, data, "{at}: HuffEncode modified its input");
                assert_eq!(outlen, want_len, "{at}: outlen");
                assert_eq!(&out[..want_len as usize], &want_pkt[..], "{at}: packet");
                last_payload = data;
                last_pkt = want_pkt;
                n.enc += 1;
            }
            "decl" => {
                let maxlen: i32 = f[1].parse().unwrap();
                let want_len: i32 = f[2].parse().unwrap();
                let (outlen, out) = survived(&at, guard_decode(&last_pkt, maxlen));
                assert_eq!(outlen, want_len, "{at}: outlen");
                let stored = outlen.clamp(0, maxlen) as usize;
                check_stored(&at, outlen, &out, maxlen, &last_payload[..stored]);
                if maxlen as usize >= last_payload.len() {
                    assert_eq!(outlen as usize, last_payload.len(), "{at}: round trip length");
                }
                n.decl += 1;
            }
            "dec" => {
                let maxlen: i32 = f[1].parse().unwrap();
                let pkt = hex(f[2]);
                let want_len: i32 = f[3].parse().unwrap();
                let (outlen, out) = survived(&at, guard_decode(&pkt, maxlen));
                assert_eq!(outlen, want_len, "{at}: outlen");
                check_stored(&at, outlen, &out, maxlen, &hex(f[4]));
                if pkt.first() == Some(&0xff) {
                    n.dec_raw += 1;
                }
                n.headers.insert(pkt[0]);
                n.dec += 1;
            }
            "ovr" => {
                let maxlen: i32 = f[1].parse().unwrap();
                let pkt = hex(f[2]);
                let tail_len: i32 = f[3].parse().unwrap();
                let tail = hex(f[4]);
                let (outlen, out) = survived(&at, guard_decode(&pkt, maxlen));
                // Never longer than the C's read-on decode, and stopped at a
                // different place whenever it was not cut short by maxlen.
                assert!(outlen <= tail_len, "{at}: the Rust decoded {outlen} bytes, the C {tail_len}");
                assert!(
                    outlen > maxlen || outlen < tail_len,
                    "{at}: the C read on past its buffer but the Rust stopped at the same place \
                     ({outlen}/{tail_len} bytes)"
                );
                let stored = outlen.clamp(0, maxlen) as usize;
                check_stored(&at, outlen, &out, maxlen, &tail[..stored]);
                n.headers.insert(pkt[0]);
                n.ovr += 1;
            }
            "under" => {
                // inlen <= 0: HuffDecode decrements first, reports 0, touches nothing.
                let inlen: i32 = f[1].parse().unwrap();
                let pkt = [0xffu8; 8];
                let mut out = vec![POISON; MAX_OUT];
                let mut outlen = -12345;
                unsafe { HuffDecode(pkt.as_ptr(), out.as_mut_ptr(), inlen, &mut outlen, 64) };
                assert_eq!(outlen, 0, "{at}");
                assert!(out.iter().all(|&b| b == POISON), "{at}: wrote on the underflow path");
                n.under += 1;
            }
            other => panic!("{at}: unknown record {other:?}"),
        }
    }

    // Floors: an emptied or truncated fixture, or one missing a whole
    // category, must not read as green.
    assert_eq!(n.freq, 256, "freq records");
    assert_eq!(n.code, 256, "code records");
    assert!(n.enc >= 530, "enc records: {}", n.enc);
    assert!(n.enc_raw >= 256, "raw-fallback encodes (the 0xff path): {}", n.enc_raw);
    assert!(n.enc_packed >= 45, "compressed encodes: {}", n.enc_packed);
    assert!(n.decl >= 1350, "round-trip decodes: {}", n.decl);
    assert!(n.dec >= 480, "surviving malformed decodes: {}", n.dec);
    assert!(n.dec_raw >= 20, "raw 0xff decodes: {}", n.dec_raw);
    assert!(n.ovr >= 40, "bounded (C overran) decodes: {}", n.ovr);
    assert_eq!(n.under, 3, "inlen underflow records");
    assert_eq!(n.headers.len(), 256, "header bytes seen in dec/ovr records");
}

/// The tree HuffInit builds is a complete prefix code: Kraft sum exactly 1 and
/// no code a prefix of another.  Independent of the fixture, so a table that
/// was frozen from a broken tree would still fail.
#[test]
fn code_table_is_a_complete_prefix_code() {
    init();
    let codes: Vec<(i32, u32)> = (0..256).map(|i| (Huffman_lookup_len(i), Huffman_lookup_bits(i))).collect();
    let mut kraft: u64 = 0;
    for (i, &(len, _)) in codes.iter().enumerate() {
        assert!((1..=32).contains(&len), "symbol {i} has code length {len}");
        kraft += 1u64 << (32 - len);
    }
    assert_eq!(kraft, 1u64 << 32, "Kraft sum is not 1: the code is not complete");
    for (i, &(li, bi)) in codes.iter().enumerate() {
        for (j, &(lj, bj)) in codes.iter().enumerate() {
            if i != j && li <= lj {
                assert_ne!(bj >> (lj - li), bi, "symbol {i}'s code is a prefix of symbol {j}'s");
            }
        }
    }
}

/// Round trips over many more payloads than the fixture freezes, with the
/// checks stated from huffman.c's HuffEncode (lines 333-340) and not from the
/// fixture:
///
///   bits    = the sum of the code lengths of the payload bytes
///   outlen  = 1 + (bits + 7) / 8, header byte = 8 * (outlen - 1) - bits
///   if outlen >= inlen + 1 the encoder sends 0xff + the payload, outlen = inlen + 1
///
/// so outlen <= inlen + 1 always, a compressed packet is strictly shorter than
/// that bound and has a header in 0..=7, and decode(encode(x)) == x at
/// maxlen == len.  The code lengths are read from the Rust table, which
/// `code_table_is_a_complete_prefix_code` and the frozen `code` records tie to
/// the C.  Payloads are 1..=255 bytes, drawn with probability proportional to
/// the C's frequency table (the same weights gen_fixtures.c uses), so most take
/// the compressed path.
#[test]
fn seeded_weighted_payloads_round_trip_and_obey_the_encoder_invariants() {
    init();
    let mut cum = [0u64; 256];
    let mut sum = 0u64;
    for (i, c) in cum.iter_mut().enumerate() {
        sum += (Huffman_freq(i) * 1_000_000.0) as u64 + 1;
        *c = sum;
    }
    let mut state = 0x243f_6a88_85a3_08d3u64; // xorshift64*, seeded
    let mut next = move || {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state.wrapping_mul(0x2545_f491_4f6c_dd1d)
    };

    let (mut compressed, mut raw) = (0, 0);
    for case in 0..1500usize {
        let len = 1 + (next() % 255) as usize;
        let payload: Vec<u8> = (0..len)
            .map(|_| {
                if case % 5 == 4 {
                    (next() >> 24) as u8 // uniform: mostly the raw fallback
                } else {
                    let r = next() % sum;
                    cum.iter().position(|&c| r < c).unwrap() as u8
                }
            })
            .collect();

        let mut out = vec![POISON; 300];
        let mut outlen = -1i32;
        unsafe { HuffEncode(payload.as_ptr(), out.as_mut_ptr(), len as i32, &mut outlen) };
        let at = format!("case {case} (len {len})");
        assert!(outlen >= 1 && outlen as usize <= len + 1, "{at}: outlen {outlen} > inlen + 1");

        let bits: i32 = payload.iter().map(|&b| Huffman_lookup_len(b as usize)).sum();
        let packed = 1 + (bits + 7) / 8;
        if packed >= len as i32 + 1 {
            assert_eq!(out[0], 0xff, "{at}: expected the raw fallback");
            assert_eq!(outlen, len as i32 + 1, "{at}: raw outlen");
            assert_eq!(&out[1..=len], &payload[..], "{at}: raw payload");
            raw += 1;
        } else {
            assert_eq!(outlen, packed, "{at}: compressed outlen");
            assert!(outlen < len as i32 + 1, "{at}: compressed packet not shorter");
            assert_eq!(out[0] as i32, 8 * (outlen - 1) - bits, "{at}: padding header");
            assert!(out[0] <= 7, "{at}: padding count {}", out[0]);
            compressed += 1;
        }

        let pkt = out[..outlen as usize].to_vec();
        let (dlen, dout) = survived(&at, guard_decode(&pkt, len as i32));
        check_stored(&at, dlen, &dout, len as i32, &payload);
        assert_eq!(dlen, len as i32, "{at}: decoded length");
    }
    // Both paths must actually have been exercised.
    assert!(compressed >= 800, "compressed packets: {compressed}");
    assert!(raw >= 100, "raw fallbacks: {raw}");
}
