// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/strlcpy.txt, the frozen output of common/strlcpy.c,
// against engine/rust/src/strlcpy.rs.  No C compiler: the fixture is plain
// text.  Format and regeneration are in tests/fixtures/README.md.
//
// Like the retired differential harness, every record runs with guard bytes
// around both buffers and at unaligned offsets, so a write outside the
// destination, or any change to the source, fails.

use strlcpy_rs::strlcpy::q_strlcpy;

const FIXTURE: &str = include_str!("fixtures/strlcpy.txt");
const GUARD: usize = 16;
const CANARY: u8 = 0xa5;

fn data(s: &str) -> Vec<u8> {
    if s == "-" {
        return Vec::new();
    }
    assert_eq!(s.len() % 2, 0, "odd hex data");
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

/// Runs q_strlcpy once with the source at `src_off` and the destination at
/// `dst_off` inside guarded buffers; returns (ret, destination bytes).
/// Asserts guard bytes and the source are untouched.  When `siz == 0` the
/// destination pointer is NULL, as the C original permits.
fn run(src: &[u8], siz: usize, src_off: usize, dst_off: usize, at: &str) -> (usize, Vec<u8>) {
    let mut sbuf = vec![CANARY; GUARD + src_off + src.len() + GUARD];
    sbuf[GUARD + src_off..GUARD + src_off + src.len()].copy_from_slice(src);
    let sbefore = sbuf.clone();

    let mut dbuf = vec![CANARY; GUARD + dst_off + siz + GUARD];
    let ret = unsafe {
        let d = if siz == 0 {
            core::ptr::null_mut()
        } else {
            dbuf.as_mut_ptr().add(GUARD + dst_off) as *mut core::ffi::c_char
        };
        q_strlcpy(d, sbuf.as_ptr().add(GUARD + src_off) as *const core::ffi::c_char, siz)
    };

    assert_eq!(sbuf, sbefore, "{at}: source modified");
    let lo = GUARD + dst_off;
    assert!(dbuf[..lo].iter().all(|&b| b == CANARY), "{at}: wrote below dst");
    assert!(dbuf[lo + siz..].iter().all(|&b| b == CANARY), "{at}: wrote past dst");
    (ret, dbuf[lo..lo + siz].to_vec())
}

#[test]
fn replay_c_fixtures() {
    let (mut n, mut n_size0, mut n_size1, mut n_exact, mut n_trunc) = (0, 0, 0, 0, 0);
    let (mut n_slack, mut n_junk, mut n_long, mut n_empty_src) = (0, 0, 0, 0);

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("strlcpy.txt:{}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        assert_eq!(f.len(), 5, "{at}: bad record {line:?}");
        assert_eq!(f[0], "copy", "{at}: unknown record");
        let siz: usize = f[1].parse().unwrap();
        let src = data(f[2]);
        let want_ret: usize = f[3].parse().unwrap();
        let want_dst = data(f[4]);
        assert_eq!(want_dst.len(), siz, "{at}: dst length != siz");

        // The string is everything before the first NUL; its length is the
        // return value by definition, independent of the fixture.
        let len = src.iter().position(|&b| b == 0).expect("source has no NUL");
        assert_eq!(want_ret, len, "{at}: fixture return is not strlen(src)");
        // Semantic oracle written out, so a bad fixture cannot bless a bad port.
        if siz > 0 {
            let keep = len.min(siz - 1);
            assert_eq!(&want_dst[..keep], &src[..keep], "{at}: fixture prefix");
            assert_eq!(want_dst[keep], 0, "{at}: fixture lacks NUL");
            assert!(want_dst[keep + 1..].iter().all(|&b| b == CANARY), "{at}: fixture tail");
        }

        for (so, dof) in [(0usize, 0usize), (1, 3), (3, 1), (7, 5), (5, 7)] {
            let ctx = format!("{at} (src off {so}, dst off {dof}): {line:.60}");
            let (ret, dst) = run(&src, siz, so, dof, &ctx);
            assert_eq!(ret, want_ret, "{ctx}: return");
            assert_eq!(dst, want_dst, "{ctx}: destination bytes");
        }

        n += 1;
        if siz == 0 {
            n_size0 += 1;
        }
        if siz == 1 {
            n_size1 += 1;
        }
        if siz == len + 1 {
            n_exact += 1;
        }
        if siz > 0 && siz <= len {
            n_trunc += 1;
        }
        if siz > len + 1 {
            n_slack += 1;
        }
        if src.len() > len + 1 {
            n_junk += 1;
        }
        if len >= 63 {
            n_long += 1;
        }
        if len == 0 {
            n_empty_src += 1;
        }
    }

    // Floors: an emptied or truncated fixture must not read as green.
    assert!(n >= 600, "records: {n}");
    assert!(n_size0 >= 30, "size 0 records: {n_size0}");
    assert!(n_size1 >= 30, "size 1 records: {n_size1}");
    assert!(n_exact >= 30, "exact-fit records: {n_exact}");
    assert!(n_trunc >= 200, "truncation records: {n_trunc}");
    assert!(n_slack >= 200, "slack records: {n_slack}");
    assert!(n_junk >= 40, "junk-after-NUL records: {n_junk}");
    assert!(n_long >= 40, "long-source records: {n_long}");
    assert!(n_empty_src >= 25, "empty-source records: {n_empty_src}");
}

/// size 0 must not touch dst even when it is a real pointer, and must still
/// return the full source length.
#[test]
fn size_zero_leaves_a_real_destination_alone() {
    let src = b"hello\0";
    let mut dst = [CANARY; 4];
    let r = unsafe {
        q_strlcpy(dst.as_mut_ptr() as *mut _, src.as_ptr() as *const _, 0)
    };
    assert_eq!(r, 5);
    assert_eq!(dst, [CANARY; 4]);
}

/// The full grid the retired differential harness covered, written straight
/// from the semantic rather than from a fixture: every source length 0..=256
/// against every size 0..=288 at source and destination offsets 0..=7, inside
/// guard zones.  Checks, directly: the first min(len, siz-1) bytes are copied,
/// a NUL follows them (siz > 0), the return is strlen(src), the rest of the
/// destination is untouched, nothing outside it is written, and the source is
/// unchanged.  A NUL-free source byte pattern with junk after the terminator
/// proves nothing past the NUL is copied.
#[test]
fn full_length_size_offset_grid_matches_the_semantic() {
    const MAXLEN: usize = 256;
    const MAXSIZ: usize = 288;
    let mut checked = 0u64;
    for len in 0..=MAXLEN {
        // Non-zero, never the canary, so a stray copy or a missed write shows.
        let mut s: Vec<u8> = (0..len)
            .map(|i| match ((i * 7) % 250 + 1) as u8 {
                CANARY => 0x5a,
                b => b,
            })
            .collect();
        s.push(0);
        s.extend_from_slice(&[0x77; 5]); // junk after the NUL, never to be copied
        for so in 0..8usize {
            let mut sbuf = vec![CANARY; GUARD + so + s.len() + GUARD];
            sbuf[GUARD + so..GUARD + so + s.len()].copy_from_slice(&s);
            let sbefore = sbuf.clone();
            for dof in 0..8usize {
                let mut dbuf = vec![CANARY; GUARD + dof + MAXSIZ + GUARD];
                for siz in 0..=MAXSIZ {
                    dbuf.fill(CANARY);
                    let lo = GUARD + dof;
                    let ret = unsafe {
                        let d = if siz == 0 {
                            core::ptr::null_mut()
                        } else {
                            dbuf.as_mut_ptr().add(lo) as *mut core::ffi::c_char
                        };
                        q_strlcpy(d, sbuf.as_ptr().add(GUARD + so) as *const core::ffi::c_char, siz)
                    };
                    let ctx = || format!("len {len} siz {siz} src off {so} dst off {dof}");
                    assert_eq!(ret, len, "{}: return != strlen", ctx());
                    assert_eq!(sbuf, sbefore, "{}: source modified", ctx());
                    assert!(dbuf[..lo].iter().all(|&b| b == CANARY), "{}: wrote below dst", ctx());
                    assert!(
                        dbuf[lo + siz..].iter().all(|&b| b == CANARY),
                        "{}: wrote past dst",
                        ctx()
                    );
                    if siz > 0 {
                        let keep = len.min(siz - 1);
                        assert_eq!(&dbuf[lo..lo + keep], &s[..keep], "{}: prefix", ctx());
                        assert_eq!(dbuf[lo + keep], 0, "{}: no NUL after the copy", ctx());
                        assert!(
                            dbuf[lo + keep + 1..lo + siz].iter().all(|&b| b == CANARY),
                            "{}: tail inside dst touched",
                            ctx()
                        );
                    }
                    checked += 1;
                }
            }
        }
    }
    assert_eq!(checked, 257 * 8 * 8 * 289);
}
