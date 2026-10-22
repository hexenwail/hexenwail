// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/hashindex.txt, the frozen behaviour of
// engine/h2shared/hashindex.c, against engine/rust/hashindex/src/ffi.rs, and
// drives long seeded operation sequences against an independent reference
// model written in this file.  No C compiler: the fixture is plain text.
// Format and regeneration are in tests/fixtures/README.md.
//
// The table is stateful, so the fixture is a script: operations (zero, seq,
// alloc, add, rem, clr, free) each followed by observations (state, look).
// An observation line is compared whole as text, so a wrong array element, size,
// mask, NULL-ness or chain order all fail with the line that disagreed.

use hashindex_replay::ffi::*;
use std::collections::BTreeMap;

const FIXTURE: &str = include_str!("fixtures/hashindex.txt");

type Hi = HashIndexC;

fn zeroed() -> Hi {
    unsafe { core::mem::zeroed() }
}

/// Free whatever the table holds, then zero the struct, as a fresh C global.
fn reset(hi: &mut Hi) {
    unsafe { Hash_Free(hi) };
    *hi = zeroed();
}

fn ints(p: *mut i32, n: usize) -> &'static [i32] {
    unsafe { core::slice::from_raw_parts(p, n) }
}

/// FNV-1a 64 over the little-endian bytes of each int (same as the generator).
fn fnv(a: &[i32]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for v in a {
        for b in v.to_le_bytes() {
            h ^= b as u64;
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    h
}

fn desc(p: *mut i32, n: usize) -> String {
    if p.is_null() {
        return "-".into();
    }
    let a = ints(p, n);
    if n <= 32 {
        a.iter().map(|v| v.to_string()).collect::<Vec<_>>().join(",")
    } else {
        format!("fnv:{:016x}", fnv(a))
    }
}

/// `state` record text for the table as it is now.
fn state(hi: &Hi) -> String {
    let n = hi.hash_size.max(0) as usize;
    format!(
        "{} {} {} {} {} {}",
        hi.hash_size,
        hi.hash_mask,
        hi.hash.is_null() as i32,
        hi.index_chain.is_null() as i32,
        desc(hi.hash, n),
        desc(hi.index_chain, n)
    )
}

/// Hash_First / Hash_Next are `static inline` in hashindex.h and stay in C, so
/// the walk is spelled out here from the two arrays, as the callers do.
fn walk(hi: &Hi, key: i32) -> Vec<i32> {
    let n = hi.hash_size as usize;
    let (h, c) = (ints(hi.hash, n), ints(hi.index_chain, n));
    let mut out = Vec::new();
    let mut i = h[(key & hi.hash_mask) as usize];
    while i != -1 && out.len() <= n {
        out.push(i);
        i = c[i as usize];
    }
    out
}

fn int(s: &str) -> i32 {
    s.parse().unwrap_or_else(|_| panic!("bad int {s:?}"))
}

#[test]
fn replay_c_fixtures() {
    let mut hi = zeroed();
    let mut n = BTreeMap::<&str, usize>::new();
    let (mut looks_deep, mut looks_cyclic, mut digest_states, mut empty_looks) = (0, 0, 0, 0);
    let mut sizes = std::collections::BTreeSet::new();

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("hashindex.txt:{}: {line}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        let kind = f[0];
        *n.entry(kind).or_default() += 1;
        match kind {
            "layout" => {
                let want: Vec<usize> = f[1..].iter().map(|s| s.parse().unwrap()).collect();
                let got = [
                    core::mem::size_of::<Hi>(),
                    core::mem::offset_of!(Hi, hash_size),
                    core::mem::offset_of!(Hi, hash),
                    core::mem::offset_of!(Hi, index_chain),
                    core::mem::offset_of!(Hi, hash_mask),
                ];
                assert_eq!(got.as_slice(), want.as_slice(), "{at}");
                let exported = [
                    HashIndexC_sizeof(),
                    HashIndexC_offsetof_hash_size(),
                    HashIndexC_offsetof_hash(),
                    HashIndexC_offsetof_index_chain(),
                    HashIndexC_offsetof_hash_mask(),
                ];
                assert_eq!(exported.as_slice(), want.as_slice(), "{at}: exported accessors");
            }
            "zero" => {
                reset(&mut hi);
            }
            "seq" => {
                reset(&mut hi);
                let size = int(f[2]);
                sizes.insert(size);
                unsafe { Hash_Allocate(&mut hi, size) };
            }
            "alloc" => unsafe { Hash_Allocate(&mut hi, int(f[1])) },
            "add" => unsafe { Hash_Add(&mut hi, int(f[1]), int(f[2])) },
            "rem" => unsafe { Hash_Remove(&mut hi, int(f[1]), int(f[2])) },
            "clr" => unsafe { Hash_Clear(&mut hi) },
            "free" => unsafe { Hash_Free(&mut hi) },
            "state" => {
                let got = state(&hi);
                assert_eq!(got, line["state ".len()..], "{at}\n(state after the previous operations)");
                if got.contains("fnv:") {
                    digest_states += 1;
                }
            }
            "look" => {
                let key = int(f[1]);
                let ids = walk(&hi, key);
                let mut got = format!("look {key} {}", ids.len());
                for i in &ids {
                    got.push_str(&format!(" {i}"));
                }
                assert_eq!(got, line, "{at}");
                if ids.len() >= 6 {
                    looks_deep += 1;
                }
                if ids.len() > hi.hash_size as usize {
                    looks_cyclic += 1;
                }
                if ids.is_empty() {
                    empty_looks += 1;
                }
            }
            "fatal" => {} // replayed by fatal_paths_call_sys_error, in a child per case
            _ => panic!("{at}: unknown record kind"),
        }
    }
    reset(&mut hi);

    let floor = |k: &str, min: usize| {
        let got = n.get(k).copied().unwrap_or(0);
        assert!(got >= min, "fixture has {got} {k} records, floor is {min}: truncated?");
    };
    floor("layout", 1);
    floor("zero", 1);
    floor("seq", 30);
    floor("alloc", 60);
    floor("add", 1200);
    floor("rem", 600);
    floor("clr", 70);
    floor("free", 80);
    floor("look", 600);
    floor("state", 500);
    floor("fatal", 23);
    assert!(looks_deep >= 20, "only {looks_deep} lookups walk 6+ entries");
    assert!(looks_cyclic >= 2, "only {looks_cyclic} bounded cyclic lookups");
    assert!(looks_cyclic <= 12, "{looks_cyclic} bounded cyclic lookups: a cycle leaked into the in-contract scripts");
    assert!(empty_looks >= 100, "only {empty_looks} empty lookups");
    assert!(digest_states >= 10, "only {digest_states} digest states (large tables)");
    for sz in [1, 2, 4, 8, 16, 32, 64, 128, 256, 1024, 65536] {
        assert!(sizes.contains(&sz), "no sequence starts at table size {sz}");
    }
}

// ---- fatal paths -------------------------------------------------------------
//
// Sys_Error never returns.  Each `fatal` record is re-run in a child copy of this
// test binary (the `fatal_child` test below, selected by an env var) that must
// die of SIGABRT with exactly the message C printed.

const TRIGGER: &str = "HASHINDEX_REPLAY_FATAL";

#[test]
fn fatal_child() {
    let Ok(line) = std::env::var(TRIGGER) else { return };
    let f: Vec<&str> = line.splitn(6, ' ').collect();
    let (setup, op, a, b) = (f[1], f[2], int(f[3]), int(f[4]));
    let mut hi = zeroed();
    unsafe {
        if let Some(sz) = setup.strip_prefix("alloc:") {
            Hash_Allocate(&mut hi, int(sz));
        } else if let Some(sz) = setup.strip_prefix("freed:") {
            Hash_Allocate(&mut hi, int(sz));
            Hash_Free(&mut hi);
        } else {
            assert_eq!(setup, "zero");
        }
        match op {
            "alloc" => Hash_Allocate(&mut hi, a),
            "add" => Hash_Add(&mut hi, a, b),
            "rem" => Hash_Remove(&mut hi, a, b),
            _ => panic!("unknown fatal op {op}"),
        }
    }
    // Returned: the child must not look like a clean abort with the message.
    eprintln!("RETURNED without calling Sys_Error");
    std::process::exit(0);
}

#[test]
fn fatal_paths_call_sys_error() {
    let mut cases = 0;
    for line in FIXTURE.lines().filter(|l| l.starts_with("fatal ")) {
        let msg = line.splitn(6, ' ').nth(5).expect("fatal record has a message");
        let out = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "fatal_child", "--nocapture", "--test-threads=1"])
            .env(TRIGGER, line)
            .output()
            .unwrap();
        let err = String::from_utf8_lossy(&out.stderr);
        assert!(!out.status.success(), "{line}: child exited cleanly\n{err}");
        #[cfg(unix)]
        {
            use std::os::unix::process::ExitStatusExt;
            assert_eq!(out.status.signal(), Some(6), "{line}: want SIGABRT, got {:?}\n{err}", out.status);
        }
        let want = format!("Sys_Error: {msg}");
        assert!(err.lines().any(|l| l == want), "{line}: stderr lacks the line {want:?}:\n{err}");
        cases += 1;
    }
    assert!(cases >= 23, "only {cases} fatal records replayed");
}

// ---- independent reference model --------------------------------------------
//
// A table is a list of buckets; a bucket is a Vec of indices, head first.  No
// arrays, no chain pointers, nothing shared with ffi.rs.  The arrays the C
// exposes are derived from it:
//   hash[b]  = head of bucket b, or -1
//   chain[i] = the element after i in its bucket, or -1, for a PRESENT i;
//              for an absent i, whatever it last held ("stale"): -1 when never
//              added or removed, the old successor when Hash_Clear dropped it
//              (Hash_Clear leaves indexChain alone on purpose).

struct Model {
    mask: i32,
    buckets: Vec<Vec<i32>>,
    stale: Vec<i32>,
}

impl Model {
    fn new(size: usize) -> Model {
        Model { mask: size as i32 - 1, buckets: vec![Vec::new(); size], stale: vec![-1; size] }
    }
    fn b(&self, key: i32) -> usize {
        (key & self.mask) as usize
    }
    fn add(&mut self, key: i32, idx: i32) {
        let b = self.b(key);
        self.buckets[b].insert(0, idx);
    }
    fn rem(&mut self, key: i32, idx: i32) {
        let b = self.b(key);
        if let Some(p) = self.buckets[b].iter().position(|&i| i == idx) {
            self.buckets[b].remove(p);
        }
        self.stale[idx as usize] = -1; // C clears the slot whether or not it was present
    }
    fn clear(&mut self) {
        for b in &mut self.buckets {
            for (p, &i) in b.iter().enumerate() {
                self.stale[i as usize] = b.get(p + 1).copied().unwrap_or(-1);
            }
            b.clear();
        }
    }
    fn look(&self, key: i32) -> Vec<i32> {
        self.buckets[self.b(key)].clone()
    }
    fn hash(&self) -> Vec<i32> {
        self.buckets.iter().map(|b| b.first().copied().unwrap_or(-1)).collect()
    }
    fn chain(&self) -> Vec<i32> {
        let mut c = self.stale.clone();
        for b in &self.buckets {
            for (p, &i) in b.iter().enumerate() {
                c[i as usize] = b.get(p + 1).copied().unwrap_or(-1);
            }
        }
        c
    }
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        (self.0.wrapping_mul(0x2545_f491_4f6c_dd1d) >> 32) as u32
    }
    fn below(&mut self, n: usize) -> usize {
        self.next() as usize % n
    }
}

#[derive(Default)]
struct Counts {
    ops: usize,
    adds: usize,
    rems: usize,
    absent_rems: usize,
    looks: usize,
    deep_looks: usize,
    clears: usize,
    regrows: usize,
    compares: usize,
}

fn pick_key(r: &mut Rng, mode: usize, size: usize) -> i32 {
    match mode {
        0 => r.next() as i32,                                   // any key
        1 => r.below(size * 2) as i32,                          // small pool
        2 => r.below(4) as i32 + size as i32 * (r.below(9) as i32 - 4), // a few buckets, aliased
        _ => [0, -1, 1, i32::MAX, i32::MIN, 1 << 30, -2, 255, 256, -256][r.below(10)],
    }
}

fn compare_all(hi: &Hi, m: &Model, size: usize, at: &str) {
    assert_eq!(hi.hash_size as usize, size, "{at}: hashSize");
    assert_eq!(hi.hash_mask, size as i32 - 1, "{at}: hashMask");
    assert_eq!(ints(hi.hash, size), m.hash().as_slice(), "{at}: hash[]");
    assert_eq!(ints(hi.index_chain, size), m.chain().as_slice(), "{at}: indexChain[]");
}

fn drive(first_size: usize, seed: u64, nops: usize, mode: usize, c: &mut Counts) {
    const SIZES: [usize; 7] = [1, 2, 8, 64, 256, 1024, 4096];
    let mut r = Rng(seed);
    let mut size = first_size;
    let mut hi = zeroed();
    unsafe { Hash_Allocate(&mut hi, size as i32) };
    let mut m = Model::new(size);
    let mut present = vec![false; size];
    let mut keyof = vec![0i32; size];
    let mut live = 0usize;
    let every = if size <= 64 { 1 } else { 61 };

    for op in 0..nops {
        let at = format!("size {first_size} seed {seed:#x} mode {mode} op {op}");
        c.ops += 1;
        match r.below(100) {
            0..=47 => {
                // add into an index that is free
                let mut idx = None;
                for _ in 0..6 {
                    let cand = r.below(size);
                    if !present[cand] { idx = Some(cand); break; }
                }
                if idx.is_none() {
                    idx = present.iter().position(|&p| !p);
                }
                if let Some(i) = idx {
                    let key = pick_key(&mut r, mode, size);
                    keyof[i] = key;
                    present[i] = true;
                    live += 1;
                    unsafe { Hash_Add(&mut hi, key, i as i32) };
                    m.add(key, i as i32);
                    c.adds += 1;
                }
            }
            48..=71 => {
                let mut idx = None;
                for _ in 0..6 {
                    let cand = r.below(size);
                    if present[cand] { idx = Some(cand); break; }
                }
                if idx.is_none() && live > 0 {
                    idx = present.iter().position(|&p| p);
                }
                if let Some(i) = idx {
                    present[i] = false;
                    live -= 1;
                    unsafe { Hash_Remove(&mut hi, keyof[i], i as i32) };
                    m.rem(keyof[i], i as i32);
                    c.rems += 1;
                }
            }
            72..=76 => {
                // remove an index that is not in the table (never added, removed, or cleared)
                let mut idx = None;
                for _ in 0..6 {
                    let cand = r.below(size);
                    if !present[cand] { idx = Some(cand); break; }
                }
                if let Some(i) = idx {
                    let key = pick_key(&mut r, mode, size);
                    unsafe { Hash_Remove(&mut hi, key, i as i32) };
                    m.rem(key, i as i32);
                    c.absent_rems += 1;
                }
            }
            77..=91 => {
                let key = if live > 0 && r.next() & 1 == 1 {
                    keyof[r.below(size)]
                } else {
                    pick_key(&mut r, mode, size)
                };
                let (got, want) = (walk(&hi, key), m.look(key));
                assert_eq!(got, want, "{at}: chain for key {key}");
                c.looks += 1;
                if want.len() >= 4 { c.deep_looks += 1; }
                // Hash_Next on every member agrees with the model's successor
                for (p, &i) in want.iter().enumerate() {
                    let next = ints(hi.index_chain, size)[i as usize];
                    assert_eq!(next, want.get(p + 1).copied().unwrap_or(-1), "{at}: Hash_Next({i})");
                }
            }
            92..=94 => {
                unsafe { Hash_Clear(&mut hi) };
                m.clear();
                present.iter_mut().for_each(|p| *p = false);
                live = 0;
                c.clears += 1;
            }
            95..=96 => {
                // free, then allocate again at a different size: the caller's "grow"
                unsafe { Hash_Free(&mut hi) };
                assert!(hi.hash.is_null() && hi.index_chain.is_null(), "{at}: Free left a pointer");
                assert_eq!(hi.hash_size as usize, size, "{at}: Free must keep hashSize");
                assert_eq!(hi.hash_mask, size as i32 - 1, "{at}: Free must keep hashMask");
                size = SIZES[r.below(SIZES.len())];
                unsafe { Hash_Allocate(&mut hi, size as i32) };
                m = Model::new(size);
                present = vec![false; size];
                keyof = vec![0; size];
                live = 0;
                c.regrows += 1;
            }
            _ => compare_all(&hi, &m, size, &at),
        }
        if op % every == 0 {
            compare_all(&hi, &m, size, &at);
            c.compares += 1;
        }
    }
    compare_all(&hi, &m, size, "final");
    unsafe { Hash_Free(&mut hi) };
}

#[test]
fn model_matches_port_on_long_seeded_sequences() {
    let mut c = Counts::default();
    for (si, &size) in [1usize, 2, 4, 8, 16, 64, 256, 1024, 4096].iter().enumerate() {
        for mode in 0..4 {
            let seed = 0x9e37_79b9_7f4a_7c15u64.wrapping_mul((si * 4 + mode + 1) as u64);
            drive(size, seed, 25_000, mode, &mut c);
        }
    }
    assert!(c.ops >= 900_000, "only {} operations ran", c.ops);
    assert!(c.adds >= 200_000 && c.rems >= 100_000, "adds {} rems {}", c.adds, c.rems);
    assert!(c.absent_rems >= 10_000, "only {} absent-index removals", c.absent_rems);
    assert!(c.looks >= 100_000 && c.deep_looks >= 5_000, "looks {} deep {}", c.looks, c.deep_looks);
    assert!(c.clears >= 5_000, "only {} clears", c.clears);
    assert!(c.regrows >= 2_000, "only {} free+realloc cycles", c.regrows);
    assert!(c.compares >= 100_000, "only {} full-array comparisons", c.compares);
}
