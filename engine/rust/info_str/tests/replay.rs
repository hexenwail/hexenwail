// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/info_str.txt, the frozen output of
// engine/hexenworld/shared/info_str.c (compiled as hwsv does), against
// engine/rust/src/info_str.rs, and checks the port against an independent
// reference model of the info-string semantics.  No C compiler: the fixture
// is plain text.  Format and regeneration: tests/fixtures/README.md.
//
// Five tests, each named by scripts/check-rust-info-str.sh:
//
//   replay_c_fixtures                          the fixture, at three buffer
//                                              offsets in guard zones; the
//                                              reference model must also
//                                              reproduce every record, which
//                                              keeps the model honest
//   sweep_digest_matches_c                     every string over { \ a b * } up
//                                              to length 7 through ten ops,
//                                              folded into the C's digest
//   sweep_matches_reference_model              the same sweep to length 8,
//                                              output by output, against the
//                                              model
//   random_sequences_match_reference_model     seeded random operation
//                                              sequences on one evolving
//                                              buffer, whole buffer byte for
//                                              byte after every operation
//   boundary_grid_matches_reference_model      key/value length x maxsize x
//                                              filter mode x offset around the
//                                              63/64 and >= maxsize edges
//
// The model below is written from the semantics of an info string -- fields
// separated by backslashes, read in key/value pairs -- not from the C control
// flow, so a mistake in the port cannot be mirrored into it.  Two ports of one
// loop would agree on a shared bug; the fixture (from the C) and the model
// (from the data shape) must both agree with the port.
//
// Out of scope, and C undefined behaviour rather than behaviour: a key or
// value segment of 512 bytes or more overruns the C's `pkey[512]`/`value[512]`
// stack buffers and the 512-byte rotation slots, and a maxsize larger than the
// caller's buffer lets the C write past it.  Nothing here generates either.

use info_str_rs::info_str::{
    Info_Print, Info_RemoveKey, Info_RemovePrefixedKeys, Info_SetValueForKey,
    Info_SetValueForStarKey, Info_ValueForKey,
};
use info_str_rs::{bad_lookups, set_highchars, take_log};
use std::ffi::{c_char, CStr};
use std::sync::{Mutex, MutexGuard};

const FIXTURE: &str = include_str!("fixtures/info_str.txt");

const BUFSZ: usize = 1024;
const POISON: u8 = 0xcc;
const GUARD: usize = 32;
const GUARD_BYTE: u8 = 0xa5;

/// Info_ValueForKey keeps process-global buffers, the stubs are global, and
/// the test harness runs tests on threads: one lock for all of them.
static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL.lock().unwrap_or_else(|e| e.into_inner())
}

//============================================================================
// fixture decoding
//============================================================================

fn unesc(tok: &str) -> Vec<u8> {
    if tok == "~" {
        return Vec::new();
    }
    let b = tok.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' {
            out.push(u8::from_str_radix(&tok[i + 1..i + 3], 16).unwrap());
            i += 3;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    out
}

//============================================================================
// the port, called the way the engine calls it: C strings in guard zones
//============================================================================

/// A read-only C string with guard bytes around it; `check` proves the port
/// neither wrote it nor read a different string.
struct CIn {
    v: Vec<u8>,
    snap: Vec<u8>,
    start: usize,
}

impl CIn {
    fn new(data: &[u8], off: usize) -> CIn {
        assert!(!data.contains(&0), "NUL inside a C string");
        let mut v = vec![GUARD_BYTE; GUARD + off + data.len() + 1 + GUARD];
        let start = GUARD + off;
        v[start..start + data.len()].copy_from_slice(data);
        v[start + data.len()] = 0;
        CIn { snap: v.clone(), v, start }
    }
    fn ptr(&self) -> *const c_char {
        unsafe { self.v.as_ptr().add(self.start) as *const c_char }
    }
    fn check(&self, at: &str) {
        assert_eq!(self.v, self.snap, "{at}: input string modified");
    }
}

/// A writable buffer of `size` bytes holding a C string, poison after it,
/// guard bytes on both sides.
struct PortBuf {
    v: Vec<u8>,
    start: usize,
    size: usize,
}

impl PortBuf {
    fn new(init: &[u8], size: usize, off: usize) -> PortBuf {
        assert!(!init.contains(&0));
        let mut v = vec![GUARD_BYTE; GUARD + off + size + GUARD];
        let start = GUARD + off;
        v[start..start + size].fill(POISON);
        v[start..start + init.len()].copy_from_slice(init);
        v[start + init.len()] = 0;
        PortBuf { v, start, size }
    }
    fn set(&mut self, init: &[u8]) {
        self.v[self.start..self.start + self.size].fill(POISON);
        self.v[self.start..self.start + init.len()].copy_from_slice(init);
        self.v[self.start + init.len()] = 0;
    }
    fn ptr(&mut self) -> *mut c_char {
        unsafe { self.v.as_mut_ptr().add(self.start) as *mut c_char }
    }
    fn bytes(&self) -> &[u8] {
        &self.v[self.start..self.start + self.size]
    }
    fn check(&self, at: &str) {
        assert!(self.v[..self.start].iter().all(|&b| b == GUARD_BYTE), "{at}: wrote below the buffer");
        assert!(
            self.v[self.start + self.size..].iter().all(|&b| b == GUARD_BYTE),
            "{at}: wrote past the buffer"
        );
    }
}

fn port_rm(b: &mut PortBuf, key: &[u8], off: usize, at: &str) -> Vec<u8> {
    let k = CIn::new(key, off);
    take_log();
    unsafe { Info_RemoveKey(b.ptr(), k.ptr()) };
    k.check(at);
    b.check(at);
    take_log()
}

fn port_rmp(b: &mut PortBuf, prefix: u8, at: &str) -> Vec<u8> {
    take_log();
    unsafe { Info_RemovePrefixedKeys(b.ptr(), prefix as c_char) };
    b.check(at);
    take_log()
}

#[allow(clippy::too_many_arguments)]
fn port_set(
    b: &mut PortBuf,
    star: bool,
    hc: Option<i32>,
    maxsize: usize,
    key: &[u8],
    value: &[u8],
    off: usize,
    at: &str,
) -> Vec<u8> {
    let k = CIn::new(key, off);
    let v = CIn::new(value, off + 1);
    take_log();
    set_highchars(hc);
    unsafe {
        if star {
            Info_SetValueForStarKey(b.ptr(), k.ptr(), v.ptr(), maxsize)
        } else {
            Info_SetValueForKey(b.ptr(), k.ptr(), v.ptr(), maxsize)
        }
    };
    set_highchars(Some(1));
    k.check(at);
    v.check(at);
    b.check(at);
    take_log()
}

fn port_print(s: &[u8], off: usize, at: &str) -> Vec<u8> {
    let c = CIn::new(s, off);
    take_log();
    unsafe { Info_Print(c.ptr()) };
    c.check(at);
    take_log()
}

fn cstr_at<'a>(p: *const c_char) -> &'a [u8] {
    unsafe { CStr::from_ptr(p) }.to_bytes()
}

/// The pointers Info_ValueForKey returns for a miss, found by probing both of
/// the C's `return ""` sites.  Whether a compiler merges them is not a
/// contract, so a miss is "one of these two", never a rotation slot.
fn empty_pointers() -> [*const c_char; 2] {
    let a = CIn::new(b"", 0);
    let b = CIn::new(b"\\a\\1", 0);
    let k = CIn::new(b"x", 0);
    unsafe { [Info_ValueForKey(a.ptr(), k.ptr()), Info_ValueForKey(b.ptr(), k.ptr())] }
}

fn port_look(info: *const c_char, key: &[u8], off: usize) -> *const c_char {
    let k = CIn::new(key, off);
    let p = unsafe { Info_ValueForKey(info, k.ptr()) };
    k.check("look");
    p
}

//============================================================================
// the reference model
//============================================================================

mod model {
    pub const MSG_KEY_BACKSLASH: &[u8] = b"[0]Can't use a key with a \\\n";
    pub const MSG_BACKSLASH: &[u8] = b"[0]Can't use keys or values with a \\\n";
    pub const MSG_QUOTE: &[u8] = b"[0]Can't use keys or values with a \"\n";
    pub const MSG_LENGTH: &[u8] = b"[0]Keys and values must be < 64 characters.\n";
    pub const MSG_EXCEEDED: &[u8] = b"[0]Info string length exceeded\n";
    pub const MSG_STAR: &[u8] = b"[0]Can't set * keys\n";

    pub fn strlen(b: &[u8]) -> usize {
        b.iter().position(|&c| c == 0).expect("unterminated model buffer")
    }

    /// One key/value pair of an info string.  `start` is where a removal of
    /// the pair begins: the backslash in front of it, or 0 for the first pair.
    #[derive(Clone, Copy, Debug)]
    pub struct Pair {
        pub start: usize,
        pub kstart: usize,
        pub kend: usize,
        pub vstart: usize,
        pub vend: usize,
    }

    /// An info string is an optional leading backslash, then fields separated
    /// by backslashes, read two at a time as key, value.  A key counts only if
    /// a value field follows it (the key must be closed by a backslash); the
    /// last value may be closed by the end of the string instead.
    pub fn pairs(s: &[u8]) -> Vec<Pair> {
        let lead = (s.first() == Some(&b'\\')) as usize;
        let mut fields = Vec::new();
        let mut st = lead;
        for i in lead..=s.len() {
            if i == s.len() || s[i] == b'\\' {
                fields.push((st, i));
                st = i + 1;
            }
        }
        let mut out = Vec::new();
        let mut i = 0;
        while 2 * i + 1 < fields.len() {
            let (ks, ke) = fields[2 * i];
            let (vs, ve) = fields[2 * i + 1];
            out.push(Pair {
                start: if i == 0 { 0 } else { fields[2 * i - 1].1 },
                kstart: ks,
                kend: ke,
                vstart: vs,
                vend: ve,
            });
            i += 1;
        }
        out
    }

    fn msg(log: &mut Vec<u8>, m: &[u8]) {
        log.extend_from_slice(m);
    }

    pub fn remove(buf: &mut [u8], log: &mut Vec<u8>, key: &[u8]) {
        if key.contains(&b'\\') {
            msg(log, MSG_KEY_BACKSLASH);
            return;
        }
        let len = strlen(buf);
        let hit = pairs(&buf[..len]).into_iter().find(|p| &buf[p.kstart..p.kend] == key);
        if let Some(p) = hit {
            // Everything from the end of the value, NUL included, moves down
            // onto the separator that opened the pair.
            buf.copy_within(p.vend..len + 1, p.start);
        }
    }

    pub fn remove_prefixed(buf: &mut [u8], prefix: u8) {
        loop {
            let len = strlen(buf);
            let hit = pairs(&buf[..len])
                .into_iter()
                .find(|p| buf.get(p.kstart).filter(|_| p.kstart < p.kend).copied().unwrap_or(0) == prefix);
            let Some(p) = hit else { return };
            let key = buf[p.kstart..p.kend].to_vec();
            let mut sink = Vec::new();
            remove(buf, &mut sink, &key);
        }
    }

    /// `hc` is sv_highchars.integer; None (not registered yet) behaves as 0.
    pub fn set(
        buf: &mut [u8],
        log: &mut Vec<u8>,
        star: bool,
        hc: Option<i32>,
        maxsize: usize,
        key: &[u8],
        value: &[u8],
    ) {
        if !star && key.first() == Some(&b'*') {
            msg(log, MSG_STAR);
            return;
        }
        if key.contains(&b'\\') || value.contains(&b'\\') {
            msg(log, MSG_BACKSLASH);
            return;
        }
        if key.contains(&b'"') || value.contains(&b'"') {
            msg(log, MSG_QUOTE);
            return;
        }
        if key.len() > 63 || value.len() > 63 {
            msg(log, MSG_LENGTH);
            return;
        }
        remove(buf, log, key);
        if value.is_empty() {
            return;
        }
        let len = strlen(buf);
        if key.len() + 1 + value.len() + 1 + len >= maxsize {
            msg(log, MSG_EXCEEDED);
            return;
        }
        let mut pair = vec![b'\\'];
        pair.extend_from_slice(key);
        pair.push(b'\\');
        pair.extend_from_slice(value);
        let strip = hc.unwrap_or(0) == 0;
        let mut w = len;
        for &b in &pair {
            let mut c = b as i32;
            if strip {
                c &= 127;
                if c < 32 {
                    continue;
                }
            }
            if c > 13 {
                buf[w] = c as u8;
                w += 1;
            }
        }
        buf[w] = 0;
    }

    /// What Info_Print writes through CON_Printf, in the stub's "[flags]text"
    /// framing (every call here is flags 0).
    pub fn print(s: &[u8]) -> Vec<u8> {
        let t = &s[(s.first() == Some(&b'\\')) as usize..];
        let mut out = Vec::new();
        let mut pos = 0;
        while pos < t.len() {
            let kend = t[pos..].iter().position(|&c| c == b'\\').map_or(t.len(), |p| pos + p);
            let key = &t[pos..kend];
            out.extend_from_slice(b"[0]");
            out.extend_from_slice(key);
            if key.len() < 20 {
                out.extend(std::iter::repeat(b' ').take(20 - key.len()));
            }
            if kend == t.len() {
                out.extend_from_slice(b"[0]MISSING VALUE\n");
                return out;
            }
            let vs = kend + 1;
            let ve = t[vs..].iter().position(|&c| c == b'\\').map_or(t.len(), |p| vs + p);
            out.extend_from_slice(b"[0]");
            out.extend_from_slice(&t[vs..ve]);
            out.push(b'\n');
            pos = if ve < t.len() { ve + 1 } else { ve };
        }
        out
    }

    #[derive(Clone, Copy, PartialEq, Debug)]
    pub enum Ret {
        Empty,
        Slot(usize),
    }

    /// Info_ValueForKey's four rotation slots.  Every call takes the next
    /// slot, and while looking for the key writes each candidate pair's value
    /// into it, so a miss leaves the last value it walked over behind.
    pub struct Rot {
        pub slot: [Vec<u8>; 4],
        pub written: [bool; 4],
        idx: usize,
    }

    impl Rot {
        pub fn new() -> Rot {
            Rot { slot: [vec![0; 512], vec![0; 512], vec![0; 512], vec![0; 512]], written: [false; 4], idx: 0 }
        }
        pub fn look(&mut self, s: &[u8], key: &[u8]) -> Ret {
            self.idx = (self.idx + 1) % 4;
            let i = self.idx;
            for p in pairs(s) {
                let v = &s[p.vstart..p.vend];
                self.slot[i][..v.len()].copy_from_slice(v);
                self.slot[i][v.len()] = 0;
                self.written[i] = true;
                if &s[p.kstart..p.kend] == key {
                    return Ret::Slot(i);
                }
            }
            Ret::Empty
        }
        pub fn text(&self, i: usize) -> &[u8] {
            &self.slot[i][..strlen(&self.slot[i])]
        }
        pub fn ret_text(&self, r: Ret) -> &[u8] {
            match r {
                Ret::Empty => b"",
                Ret::Slot(i) => self.text(i),
            }
        }
    }
}

use model::{Ret, Rot};

/// Follows the pointers the port hands back for one run of lookups and checks
/// them against the model's slot picture: a miss is one of the two empty
/// literals, a hit is a slot, the same model slot is always the same pointer,
/// two model slots are never one pointer, and every slot the model has
/// written reads what the model says.
struct PtrMap {
    empties: [*const c_char; 2],
    slot: [Option<*const c_char>; 4],
}

impl PtrMap {
    fn new(empties: [*const c_char; 2]) -> PtrMap {
        PtrMap { empties, slot: [None; 4] }
    }
    fn is_empty_ptr(&self, p: *const c_char) -> bool {
        p == self.empties[0] || p == self.empties[1]
    }
    fn check(&mut self, p: *const c_char, r: Ret, rot: &Rot, at: &str) {
        match r {
            Ret::Empty => {
                assert!(self.is_empty_ptr(p), "{at}: a miss returned a non-empty-literal pointer");
                assert!(self.slot.iter().all(|s| *s != Some(p)), "{at}: a miss returned a rotation slot");
            }
            Ret::Slot(i) => {
                assert!(!self.is_empty_ptr(p), "{at}: a hit returned the empty literal");
                match self.slot[i] {
                    Some(q) => assert_eq!(p, q, "{at}: model slot {i} moved to another buffer"),
                    None => {
                        assert!(
                            self.slot.iter().all(|s| *s != Some(p)),
                            "{at}: two rotation slots are one buffer"
                        );
                        self.slot[i] = Some(p);
                    }
                }
            }
        }
        for i in 0..4 {
            if let (Some(q), true) = (self.slot[i], rot.written[i]) {
                assert_eq!(
                    cstr_at(q),
                    rot.text(i),
                    "{at}: slot {i} reads {:?}, model says {:?}",
                    String::from_utf8_lossy(cstr_at(q)),
                    String::from_utf8_lossy(rot.text(i))
                );
            }
        }
    }
}

//============================================================================
// 1. the fixture
//============================================================================

fn parse_hc(t: &str) -> Option<i32> {
    if t == "none" {
        None
    } else {
        Some(t.parse().unwrap())
    }
}

#[derive(Default)]
struct Counts {
    rot: usize,
    look: usize,
    look_miss: usize,
    look_hit: usize,
    rm: usize,
    rm_changed: usize,
    rmp: usize,
    rmp_changed: usize,
    set: usize,
    set_changed: usize,
    set_filtered: usize,
    set_msg: [usize; 6],
    print: usize,
    print_missing: usize,
    sweep: usize,
}

#[test]
fn replay_c_fixtures() {
    let _g = serial();
    let bad0 = bad_lookups();
    set_highchars(Some(1));
    let empties = empty_pointers();
    let mut c = Counts::default();

    // Lookup-run state: the port's view and the model's.
    let mut seen_ptrs: Vec<*const c_char> = Vec::new();
    let mut ptrmap = PtrMap::new(empties);
    let mut rot = Rot::new();
    let mut seen_model: Vec<usize> = Vec::new();

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("info_str.txt:{}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        match f[0] {
            "rot" => {
                c.rot += 1;
                seen_ptrs.clear();
                seen_model.clear();
                ptrmap = PtrMap::new(empties);
                rot = Rot::new();
            }
            "look" => {
                c.look += 1;
                let info = unesc(f[1]);
                let key = unesc(f[2]);
                let cls = f[3];
                let snaps: Vec<Vec<u8>> = f[4..].iter().map(|t| unesc(t)).collect();
                let ci = CIn::new(&info, 0);
                let p = port_look(ci.ptr(), &key, 1);
                ci.check(&at);

                // The port against the fixture.
                if cls == "E" {
                    c.look_miss += 1;
                    assert!(ptrmap.is_empty_ptr(p), "{at}: C returned the empty literal, port a buffer");
                } else {
                    c.look_hit += 1;
                    assert!(!ptrmap.is_empty_ptr(p), "{at}: C returned a slot, port the empty literal");
                    let idx = match seen_ptrs.iter().position(|&q| q == p) {
                        Some(i) => i,
                        None => {
                            seen_ptrs.push(p);
                            seen_ptrs.len() - 1
                        }
                    };
                    assert_eq!(cls.parse::<usize>().unwrap(), idx, "{at}: pointer class differs from C");
                }
                assert_eq!(snaps.len(), seen_ptrs.len(), "{at}: count of slots seen differs from C");
                for (i, q) in seen_ptrs.iter().enumerate() {
                    assert_eq!(cstr_at(*q), &snaps[i][..], "{at}: slot {i} reads differently from C");
                }

                // The model against the fixture: same classes, same texts.
                let r = rot.look(&info, &key);
                ptrmap.check(p, r, &rot, &at);
                match r {
                    Ret::Empty => assert_eq!(cls, "E", "{at}: model says miss, C says {cls}"),
                    Ret::Slot(i) => {
                        let idx = seen_model.iter().position(|&q| q == i).unwrap_or_else(|| {
                            seen_model.push(i);
                            seen_model.len() - 1
                        });
                        assert_eq!(cls, idx.to_string(), "{at}: model class differs from C");
                    }
                }
                assert_eq!(seen_model.len(), snaps.len(), "{at}: model slots seen differ from C");
                for (i, &slot) in seen_model.iter().enumerate() {
                    assert_eq!(rot.text(slot), &snaps[i][..], "{at}: model slot text differs from C");
                }
            }
            "rm" => {
                c.rm += 1;
                let (init, key) = (unesc(f[1]), unesc(f[2]));
                let (log, after) = (unesc(f[3]), unesc(f[4]));
                for off in [0usize, 3, 7] {
                    let mut b = PortBuf::new(&init, BUFSZ, off);
                    let got = port_rm(&mut b, &key, off + 1, &at);
                    assert_eq!(got, log, "{at}: log differs from C (offset {off})");
                    check_window(b.bytes(), &after, init.len(), &at);
                }
                let mut m = model_buf(&init);
                let mut mlog = Vec::new();
                model::remove(&mut m, &mut mlog, &key);
                assert_eq!(mlog, log, "{at}: model log differs from C");
                check_window(&m, &after, init.len(), &format!("{at} (model)"));
                c.rm_changed += (after[..model::strlen(&after)] != init[..]) as usize;
            }
            "rmp" => {
                c.rmp += 1;
                let init = unesc(f[1]);
                let prefix = u8::from_str_radix(f[2], 16).unwrap();
                let (log, after) = (unesc(f[3]), unesc(f[4]));
                assert!(log.is_empty());
                for off in [0usize, 3, 7] {
                    let mut b = PortBuf::new(&init, BUFSZ, off);
                    assert!(port_rmp(&mut b, prefix, &at).is_empty());
                    check_window(b.bytes(), &after, init.len(), &at);
                }
                let mut m = model_buf(&init);
                model::remove_prefixed(&mut m, prefix);
                // The model removes pairs; where the C leaves stale bytes
                // past the new NUL depends on the order of its memmoves, so
                // the model is compared on the string it leaves.
                let n = model::strlen(&after);
                assert_eq!(&m[..model::strlen(&m)], &after[..n], "{at}: model string differs from C");
                c.rmp_changed += (after[..n] != init[..]) as usize;
            }
            "set" => {
                c.set += 1;
                let star = f[1] == "S";
                let hc = parse_hc(f[2]);
                let maxsize: usize = f[3].parse().unwrap();
                let (init, key, value) = (unesc(f[4]), unesc(f[5]), unesc(f[6]));
                let (log, after) = (unesc(f[7]), unesc(f[8]));
                for off in [0usize, 3, 7] {
                    let mut b = PortBuf::new(&init, BUFSZ, off);
                    let got = port_set(&mut b, star, hc, maxsize, &key, &value, off + 1, &at);
                    assert_eq!(got, log, "{at}: log differs from C (offset {off})");
                    check_window(b.bytes(), &after, init.len(), &at);
                }
                let mut m = model_buf(&init);
                let mut mlog = Vec::new();
                model::set(&mut m, &mut mlog, star, hc, maxsize, &key, &value);
                assert_eq!(mlog, log, "{at}: model log differs from C");
                check_window(&m, &after, init.len(), &format!("{at} (model)"));

                let n = model::strlen(&after);
                c.set_changed += (after[..n] != init[..]) as usize;
                for (i, m) in [
                    model::MSG_BACKSLASH,
                    model::MSG_QUOTE,
                    model::MSG_LENGTH,
                    model::MSG_EXCEEDED,
                    model::MSG_STAR,
                    model::MSG_KEY_BACKSLASH,
                ]
                .iter()
                .enumerate()
                {
                    c.set_msg[i] += (log == *m) as usize;
                }
                // A write that the high-bit filter reached: a byte outside
                // 14..=127 in the pair, written with the filter on or off.
                let odd = key.iter().chain(&value).any(|&b| b < 32 || b > 127);
                c.set_filtered += (odd && log.is_empty() && n > 0) as usize;
            }
            "print" => {
                c.print += 1;
                let (s, log) = (unesc(f[1]), unesc(f[2]));
                for off in [0usize, 5] {
                    assert_eq!(port_print(&s, off, &at), log, "{at}: Info_Print log differs from C");
                }
                assert_eq!(model::print(&s), log, "{at}: model print differs from C");
                c.print_missing += log.ends_with(b"MISSING VALUE\n") as usize;
            }
            "sweep" => c.sweep += 1, // compared by sweep_digest_matches_c
            other => panic!("{at}: unknown record kind {other}"),
        }
    }

    // A truncated or emptied fixture must not read green.
    assert!(c.rot >= 25, "rot runs {}", c.rot);
    assert!(c.look >= 250, "look {}", c.look);
    assert!(c.look_miss >= 60 && c.look_hit >= 120, "miss {} hit {}", c.look_miss, c.look_hit);
    assert!(c.rm >= 120 && c.rm_changed >= 30, "rm {} changed {}", c.rm, c.rm_changed);
    assert!(c.rmp >= 80 && c.rmp_changed >= 20, "rmp {} changed {}", c.rmp, c.rmp_changed);
    assert!(c.set >= 250 && c.set_changed >= 100, "set {} changed {}", c.set, c.set_changed);
    assert!(c.set_filtered >= 10, "set with filtered bytes {}", c.set_filtered);
    for (i, name) in ["backslash", "quote", "length", "exceeded", "star", "key backslash"].iter().enumerate() {
        if i == 5 {
            continue; // reported by remove, not set: counted under rm
        }
        assert!(c.set_msg[i] >= 3, "set rejections for {name}: {}", c.set_msg[i]);
    }
    assert!(c.print >= 50 && c.print_missing >= 4, "print {} missing {}", c.print, c.print_missing);
    assert_eq!(c.sweep, 1, "sweep records");
    assert_eq!(bad_lookups(), bad0, "the port looked up a cvar other than sv_highchars");
}

fn model_buf(init: &[u8]) -> Vec<u8> {
    let mut m = vec![POISON; BUFSZ];
    m[..init.len()].copy_from_slice(init);
    m[init.len()] = 0;
    m
}

/// The recorded window (everything the call can have touched) must match, and
/// everything after it must still be poison.
fn check_window(buf: &[u8], window: &[u8], old_len: usize, at: &str) {
    let newlen = model::strlen(window);
    assert_eq!(window.len(), old_len.max(newlen) + 1, "{at}: window shape");
    assert_eq!(&buf[..window.len()], window, "{at}: buffer differs from C");
    assert!(buf[window.len()..].iter().all(|&b| b == POISON), "{at}: bytes written past the window");
}

//============================================================================
// 2. the small-alphabet sweep
//============================================================================

const SWEEP_BUF: usize = 64;

struct Fnv(u64);

impl Fnv {
    fn new() -> Fnv {
        Fnv(0xcbf29ce484222325)
    }
    fn put(&mut self, b: &[u8]) {
        for &c in b {
            self.0 ^= c as u64;
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

/// Every string over { '\\', 'a', 'b', '*' } of length 0..=maxlen, in the
/// generator's order (length, then base-4 big-endian).
fn sweep_strings(maxlen: usize) -> impl Iterator<Item = Vec<u8>> {
    const ALPHA: [u8; 4] = [b'\\', b'a', b'b', b'*'];
    (0..=maxlen).flat_map(|len| {
        (0..4usize.pow(len as u32)).map(move |idx| {
            let mut v = idx;
            let mut s = vec![0u8; len];
            for i in (0..len).rev() {
                s[i] = ALPHA[v % 4];
                v /= 4;
            }
            s
        })
    })
}

/// The ten operations of the sweep, serialised exactly as gen_fixtures.c's
/// `sweep` folds them into its digest.
fn sweep_serialise(
    s: &[u8],
    look: &mut dyn FnMut(&[u8], &[u8]) -> Vec<u8>,
    op: &mut dyn FnMut(usize, &[u8], &mut Vec<u8>, &mut Vec<u8>),
    print: &mut dyn FnMut(&[u8]) -> Vec<u8>,
) -> Vec<u8> {
    let mut out = Vec::new();
    for (id, key) in [(1u8, &b"a"[..]), (2, b""), (3, b"*")] {
        out.push(id);
        out.extend_from_slice(&look(s, key));
        out.push(0);
    }
    for id in 4..=10usize {
        let mut buf = vec![POISON; SWEEP_BUF];
        buf[..s.len()].copy_from_slice(s);
        buf[s.len()] = 0;
        let mut log = Vec::new();
        op(id, s, &mut buf, &mut log);
        out.push(id as u8);
        out.extend_from_slice(&buf);
        out.push(0xfe);
        out.extend_from_slice(&log);
        out.push(0xff);
    }
    out.push(11);
    out.extend_from_slice(&print(s));
    out.push(0xff);
    out
}

/// (key, value, maxsize, hc, star) for sweep ops 7..=10.
fn sweep_set_args(id: usize) -> (&'static [u8], &'static [u8], usize, i32, bool) {
    match id {
        7 => (b"a", b"b", 16, 1, true),
        8 => (b"*a", b"b", 16, 1, false),
        9 => (b"b", b"\x80x\x01y", 12, 0, true),
        10 => (b"a", b"", 16, 1, true),
        _ => unreachable!(),
    }
}

fn serialise_port(s: &[u8]) -> Vec<u8> {
    let mut look = |s: &[u8], key: &[u8]| {
        let ci = CIn::new(s, 0);
        cstr_at(port_look(ci.ptr(), key, 0)).to_vec()
    };
    let mut op = |id: usize, _s: &[u8], buf: &mut Vec<u8>, log: &mut Vec<u8>| {
        let mut pb = PortBuf::new(&buf[..model::strlen(buf)], SWEEP_BUF, 2);
        *log = match id {
            4 => port_rm(&mut pb, b"a", 0, "sweep"),
            5 => port_rm(&mut pb, b"", 0, "sweep"),
            6 => port_rmp(&mut pb, b'*', "sweep"),
            _ => {
                let (k, v, max, hc, star) = sweep_set_args(id);
                port_set(&mut pb, star, Some(hc), max, k, v, 0, "sweep")
            }
        };
        buf.copy_from_slice(pb.bytes());
    };
    let mut print = |s: &[u8]| port_print(s, 0, "sweep");
    sweep_serialise(s, &mut look, &mut op, &mut print)
}

fn serialise_model(s: &[u8]) -> Vec<u8> {
    let mut rot = Rot::new();
    let mut look = |s: &[u8], key: &[u8]| {
        let r = rot.look(s, key);
        rot.ret_text(r).to_vec()
    };
    let mut op = |id: usize, _s: &[u8], buf: &mut Vec<u8>, log: &mut Vec<u8>| match id {
        4 => model::remove(buf, log, b"a"),
        5 => model::remove(buf, log, b""),
        6 => model::remove_prefixed(buf, b'*'),
        _ => {
            let (k, v, max, hc, star) = sweep_set_args(id);
            model::set(buf, log, star, Some(hc), max, k, v)
        }
    };
    let mut print = |s: &[u8]| model::print(s);
    sweep_serialise(s, &mut look, &mut op, &mut print)
}

fn sweep_port(maxlen: usize) -> (usize, u64) {
    set_highchars(Some(1));
    let mut h = Fnv::new();
    let mut n = 0;
    for s in sweep_strings(maxlen) {
        h.put(&serialise_port(&s));
        n += 1;
    }
    (n, h.0)
}

#[test]
fn sweep_digest_matches_c() {
    let _g = serial();
    let bad0 = bad_lookups();
    let recs: Vec<&str> = FIXTURE.lines().filter(|l| l.starts_with("sweep ")).collect();
    assert_eq!(recs.len(), 1, "expected exactly one sweep record");
    let f: Vec<&str> = recs[0].split(' ').collect();
    let maxlen: usize = f[1].parse().unwrap();
    let count: usize = f[2].parse().unwrap();
    let digest = u64::from_str_radix(f[3], 16).unwrap();
    assert!(maxlen >= 7 && count >= 21845, "the sweep record shrank: maxlen {maxlen} count {count}");

    let (n, d) = sweep_port(maxlen);
    assert_eq!(n, count, "sweep string count differs from C");
    assert_eq!(d, digest, "sweep digest differs from C: {d:016x} vs {digest:016x}");
    assert_eq!(bad_lookups(), bad0);
}

#[test]
fn sweep_matches_reference_model() {
    let _g = serial();
    let bad0 = bad_lookups();
    set_highchars(Some(1));
    let mut n = 0;
    for s in sweep_strings(8) {
        let (p, m) = (serialise_port(&s), serialise_model(&s));
        if p != m {
            let at = p.iter().zip(&m).position(|(a, b)| a != b).unwrap_or(p.len().min(m.len()));
            panic!(
                "sweep string {:?}: port and model differ at serialised byte {at}\nport  {:?}\nmodel {:?}",
                String::from_utf8_lossy(&s),
                &p[at.saturating_sub(8)..(at + 24).min(p.len())],
                &m[at.saturating_sub(8)..(at + 24).min(m.len())]
            );
        }
        n += 1;
    }
    assert_eq!(n, 87381);
    assert_eq!(bad_lookups(), bad0);
}

//============================================================================
// 3. seeded random operation sequences
//============================================================================

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545f4914f6cdd1d)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() >> 11) as usize % n
    }
    fn pct(&mut self, p: usize) -> bool {
        self.below(100) < p
    }
}

const KEYS: &[&[u8]] = &[
    b"a", b"b", b"name", b"team", b"_pw", b"_x", b"*gamedir", b"*spectator", b"topcolor", b"k", b"", b"*", b"_",
];
const VALS: &[&[u8]] = &[b"1", b"0", b"two words", b"hexenwail", b"x", b"", b"Z", b"12345678", b"-"];
const ODD: &[u8] = &[1, 13, 14, 31, 127, 128, 200, 255];

fn rand_key(r: &mut Rng) -> Vec<u8> {
    match r.below(100) {
        0..=69 => KEYS[r.below(KEYS.len())].to_vec(),
        70..=79 => vec![b'k'; 58 + r.below(10)],
        _ => (0..1 + r.below(5)).map(|_| b'a' + r.below(6) as u8).collect(),
    }
}

fn rand_val(r: &mut Rng) -> Vec<u8> {
    match r.below(100) {
        0..=49 => VALS[r.below(VALS.len())].to_vec(),
        50..=59 => vec![b'v'; 58 + r.below(10)],
        60..=84 => (0..1 + r.below(4))
            .map(|_| if r.pct(50) { ODD[r.below(ODD.len())] } else { b'a' + r.below(6) as u8 })
            .collect(),
        _ => (0..1 + r.below(6)).map(|_| b'A' + r.below(8) as u8).collect(),
    }
}

/// A well-formed or deliberately malformed info string: missing leading or
/// trailing separators, an odd number of fields, doubled separators, empty
/// and overlong keys and values, duplicate keys.
fn rand_info(r: &mut Rng, pairs: usize) -> Vec<u8> {
    let mut out = Vec::new();
    for i in 0..pairs {
        if i > 0 || !r.pct(10) {
            out.push(b'\\');
        }
        out.extend(rand_key(r));
        if r.pct(4) {
            out.push(b'\\');
        }
        out.push(b'\\');
        if !(i == pairs - 1 && r.pct(12)) {
            out.extend(rand_val(r));
        }
    }
    if r.pct(10) {
        out.push(b'\\');
    }
    out
}

struct Counters {
    sets: usize,
    set_msgs: usize,
    set_written: usize,
    removes: usize,
    removes_hit: usize,
    rmps: usize,
    rmps_hit: usize,
    looks: usize,
    look_hits: usize,
    prints: usize,
    resets: usize,
}

#[test]
fn random_sequences_match_reference_model() {
    let _g = serial();
    let bad0 = bad_lookups();
    set_highchars(Some(1));
    let empties = empty_pointers();
    let mut n = Counters {
        sets: 0,
        set_msgs: 0,
        set_written: 0,
        removes: 0,
        removes_hit: 0,
        rmps: 0,
        rmps_hit: 0,
        looks: 0,
        look_hits: 0,
        prints: 0,
        resets: 0,
    };

    for seq in 0..3000u64 {
        let mut r = Rng(0x9e3779b97f4a7c15 ^ seq.wrapping_mul(0xd1342543de82ef95) | 1);
        let off = (seq % 8) as usize;
        let init = { let np = r.below(5); rand_info(&mut r, np) };
        let mut pb = PortBuf::new(&init, BUFSZ, off);
        let mut mb = model_buf(&init);
        let mut rot = Rot::new();
        let mut ptrs = PtrMap::new(empties);

        for step in 0..60 {
            let at = format!("seq {seq} step {step}");
            let len = model::strlen(&mb);
            match r.below(100) {
                0..=39 => {
                    n.sets += 1;
                    let star = r.pct(50);
                    let hc = [None, Some(0), Some(1), Some(1), Some(7), Some(-1)][r.below(6)];
                    let mut key = rand_key(&mut r);
                    let mut value = rand_val(&mut r);
                    if r.pct(4) {
                        key.push(b'\\');
                    } else if r.pct(4) {
                        value.push(b'"');
                    } else if r.pct(8) {
                        value.clear();
                    }
                    let need = key.len() + value.len() + 2 + len;
                    let maxsize = match r.below(10) {
                        0..=5 => (need + r.below(7)).saturating_sub(3),
                        6..=7 => 4 + r.below(300),
                        _ => 1000,
                    }
                    .min(BUFSZ - 8);
                    let plog = port_set(&mut pb, star, hc, maxsize, &key, &value, r.below(4), &at);
                    let mut mlog = Vec::new();
                    model::set(&mut mb, &mut mlog, star, hc, maxsize, &key, &value);
                    assert_eq!(plog, mlog, "{at}: set log");
                    n.set_msgs += !mlog.is_empty() as usize;
                    n.set_written += (model::strlen(&mb) > len) as usize;
                }
                40..=54 => {
                    n.removes += 1;
                    let infostr = &mb[..len];
                    let key = if r.pct(50) {
                        match model::pairs(infostr).get(r.below(4)) {
                            Some(p) => infostr[p.kstart..p.kend].to_vec(),
                            None => rand_key(&mut r),
                        }
                    } else {
                        rand_key(&mut r)
                    };
                    let plog = port_rm(&mut pb, &key, r.below(4), &at);
                    let mut mlog = Vec::new();
                    model::remove(&mut mb, &mut mlog, &key);
                    assert_eq!(plog, mlog, "{at}: remove log");
                    n.removes_hit += (model::strlen(&mb) < len) as usize;
                }
                55..=64 => {
                    n.rmps += 1;
                    let prefix = [b'_', b'*', b'_', b'n', b'a', 0u8, 0xe9][r.below(7)];
                    let plog = port_rmp(&mut pb, prefix, &at);
                    model::remove_prefixed(&mut mb, prefix);
                    assert!(plog.is_empty());
                    n.rmps_hit += (model::strlen(&mb) < len) as usize;
                }
                65..=89 => {
                    n.looks += 1;
                    let infostr = mb[..len].to_vec();
                    let key = if r.pct(60) {
                        match model::pairs(&infostr).get(r.below(5)) {
                            Some(p) => infostr[p.kstart..p.kend].to_vec(),
                            None => rand_key(&mut r),
                        }
                    } else {
                        rand_key(&mut r)
                    };
                    let p = port_look(pb.ptr(), &key, r.below(4));
                    let m = rot.look(&infostr, &key);
                    ptrs.check(p, m, &rot, &at);
                    assert_eq!(cstr_at(p), rot.ret_text(m), "{at}: lookup text");
                    n.look_hits += (m != Ret::Empty) as usize;
                    pb.check(&at);
                }
                90..=96 => {
                    n.prints += 1;
                    let s = mb[..len].to_vec();
                    assert_eq!(port_print(&s, r.below(4), &at), model::print(&s), "{at}: print");
                }
                _ => {
                    n.resets += 1;
                    let init = { let np = r.below(5); rand_info(&mut r, np) };
                    pb.set(&init);
                    mb = model_buf(&init);
                }
            }
            assert_eq!(pb.bytes(), &mb[..], "{at}: buffer differs from model");
            pb.check(&at);
        }
    }

    assert!(n.sets > 50_000 && n.set_msgs > 3_000 && n.set_written > 20_000, "sets {} msgs {} written {}", n.sets, n.set_msgs, n.set_written);
    assert!(n.removes > 20_000 && n.removes_hit > 5_000, "removes {} hit {}", n.removes, n.removes_hit);
    assert!(n.rmps > 10_000 && n.rmps_hit > 1_000, "rmps {} hit {}", n.rmps, n.rmps_hit);
    assert!(n.looks > 30_000 && n.look_hits > 10_000, "looks {} hits {}", n.looks, n.look_hits);
    assert!(n.prints > 8_000 && n.resets > 3_000, "prints {} resets {}", n.prints, n.resets);
    assert_eq!(bad_lookups(), bad0, "the port looked up a cvar other than sv_highchars");
}

//============================================================================
// 4. boundary grid
//============================================================================

#[test]
fn boundary_grid_matches_reference_model() {
    let _g = serial();
    let bad0 = bad_lookups();
    let inits: [&[u8]; 9] = [
        b"",
        b"\\",
        b"\\a\\1",
        b"a\\1",
        b"\\a\\1\\b\\2\\c\\3",
        b"\\k\\v\\k\\w",
        b"\\a\\",
        b"\\a\\1\\b",
        b"\\n\\zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\\a\\1",
    ];
    let lens = [0usize, 1, 2, 5, 62, 63, 64, 65];
    let hcs = [None, Some(0), Some(1)];
    let mut cases = 0usize;
    let mut accepted = 0usize;
    let mut rejected_len = 0usize;
    let mut rejected_max = 0usize;

    for init in inits {
        for &kl in &lens {
            for &vl in &lens {
                for (ks, star) in [(b'x', false), (b'*', true), (b'*', false), (b'a', true)] {
                    let mut key = vec![ks; kl];
                    if kl > 1 && ks == b'a' {
                        key[1..].fill(b'k');
                    }
                    let mut value = vec![b'v'; vl];
                    if vl > 2 {
                        value[1] = 0x80;
                        value[2] = 0x01;
                    }
                    // need = the length the check compares with maxsize
                    let mut probe = model_buf(init);
                    let mut sink = Vec::new();
                    model::remove(&mut probe, &mut sink, &key);
                    let need = kl + vl + 2 + model::strlen(&probe);
                    for hc in hcs {
                        for maxsize in need.saturating_sub(2)..=need + 2 {
                            let off = cases % 8;
                            let at = format!("grid init {:?} kl {kl} vl {vl} max {maxsize}", String::from_utf8_lossy(init));
                            let mut pb = PortBuf::new(init, BUFSZ, off);
                            let plog = port_set(&mut pb, star, hc, maxsize, &key, &value, off, &at);
                            let mut mb = model_buf(init);
                            let mut mlog = Vec::new();
                            model::set(&mut mb, &mut mlog, star, hc, maxsize, &key, &value);
                            assert_eq!(plog, mlog, "{at}: log");
                            assert_eq!(pb.bytes(), &mb[..], "{at}: buffer");
                            cases += 1;
                            accepted += (mlog.is_empty() && model::strlen(&mb) != model::strlen(&probe)) as usize;
                            rejected_len += (mlog == model::MSG_LENGTH) as usize;
                            rejected_max += (mlog == model::MSG_EXCEEDED) as usize;
                        }
                    }
                }
            }
        }
    }
    assert!(cases > 30_000, "grid cases {cases}");
    assert!(accepted > 4_000 && rejected_len > 8_000 && rejected_max > 5_000, "accepted {accepted} len {rejected_len} max {rejected_max}");
    assert_eq!(bad_lookups(), bad0);
}
