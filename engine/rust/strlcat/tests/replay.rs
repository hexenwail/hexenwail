// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/strlcat.txt, the frozen output of common/strlcat.c,
// against engine/rust/src/strlcat.rs.  No C compiler: the fixture is plain
// text.  Format and regeneration are in tests/fixtures/README.md.
//
// Every record runs with guard bytes around both buffers and at unaligned
// offsets, so a write outside the destination, or any change to the source,
// fails.  The contract is also written out below (`expected`) independently of
// the port and of the fixture, and is checked over a generated grid.

use strlcat_rs::strlcat::q_strlcat;

const FIXTURE: &str = include_str!("fixtures/strlcat.txt");
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

/// The contract, stated from the C semantics and nothing else.  `dst` is the
/// `siz` bytes the function may inspect, `slen` is strlen(src).  Returns
/// (return value, destination after the call):
///   - dlen = strlen(dst) looked at no further than siz bytes (dlen == siz when
///     there is no NUL in them);
///   - return = slen + dlen, so a result >= siz means truncation, and when
///     siz < strlen(dst) the size, not the string, bounds the return;
///   - dlen == siz: nothing is written at all;
///   - otherwise min(slen, siz - 1 - dlen) source bytes are appended at dlen and
///     a NUL follows them; every other byte is untouched.
fn expected(dst: &[u8], src: &[u8], slen: usize) -> (usize, Vec<u8>) {
    let siz = dst.len();
    let dlen = dst.iter().position(|&b| b == 0).unwrap_or(siz);
    let mut out = dst.to_vec();
    if dlen < siz {
        let w = slen.min(siz - 1 - dlen);
        out[dlen..dlen + w].copy_from_slice(&src[..w]);
        out[dlen + w] = 0;
    }
    (dlen + slen, out)
}

/// Runs q_strlcat once with the source at `src_off` and the destination at
/// `dst_off` inside guarded buffers.  `dst` is the whole initial destination
/// region; the first `siz` bytes are what the function is told it owns, any
/// bytes after them are extra string content C must not scan into.  Returns
/// (ret, the whole destination region after the call).  Asserts the guard
/// bytes and the source are untouched.  When `siz == 0` the destination
/// pointer is NULL, as the C original permits.
fn run(src: &[u8], dst: &[u8], siz: usize, src_off: usize, dst_off: usize, at: &str) -> (usize, Vec<u8>) {
    let mut sbuf = vec![CANARY; GUARD + src_off + src.len() + GUARD];
    sbuf[GUARD + src_off..GUARD + src_off + src.len()].copy_from_slice(src);
    let sbefore = sbuf.clone();

    let lo = GUARD + dst_off;
    let mut dbuf = vec![CANARY; lo + dst.len() + GUARD];
    dbuf[lo..lo + dst.len()].copy_from_slice(dst);
    let ret = unsafe {
        let d = if siz == 0 {
            core::ptr::null_mut()
        } else {
            dbuf.as_mut_ptr().add(lo) as *mut core::ffi::c_char
        };
        q_strlcat(d, sbuf.as_ptr().add(GUARD + src_off) as *const core::ffi::c_char, siz)
    };

    assert_eq!(sbuf, sbefore, "{at}: source modified");
    assert!(dbuf[..lo].iter().all(|&b| b == CANARY), "{at}: wrote below dst");
    assert!(
        dbuf[lo + dst.len()..].iter().all(|&b| b == CANARY),
        "{at}: wrote past dst"
    );
    (ret, dbuf[lo..lo + dst.len()].to_vec())
}

#[test]
fn replay_c_fixtures() {
    let (mut n, mut n_size0, mut n_size1, mut n_nonul, mut n_termonly) = (0, 0, 0, 0, 0);
    let (mut n_trunc, mut n_fit, mut n_slack, mut n_junk, mut n_long) = (0, 0, 0, 0, 0);
    let (mut n_empty_dst, mut n_empty_src, mut n_dst_junk) = (0, 0, 0);

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("strlcat.txt:{}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        assert_eq!(f.len(), 6, "{at}: bad record {line:?}");
        assert_eq!(f[0], "cat", "{at}: unknown record");
        let siz: usize = f[1].parse().unwrap();
        let dst_in = data(f[2]);
        let src = data(f[3]);
        let want_ret: usize = f[4].parse().unwrap();
        let want_dst = data(f[5]);
        assert_eq!(dst_in.len(), siz, "{at}: dst-in length != siz");
        assert_eq!(want_dst.len(), siz, "{at}: dst-out length != siz");

        let slen = src.iter().position(|&b| b == 0).expect("source has no NUL");
        // The independent contract must agree with the frozen C values, so a
        // bad fixture cannot bless a bad port.
        let (eret, edst) = expected(&dst_in, &src, slen);
        assert_eq!(want_ret, eret, "{at}: fixture return is not strlen(src)+MIN(siz,strlen(dst))");
        assert_eq!(want_dst, edst, "{at}: fixture destination is not the contract's image");

        for (so, dof) in [(0usize, 0usize), (1, 3), (3, 1), (7, 5), (5, 7)] {
            let ctx = format!("{at} (src off {so}, dst off {dof}): {line:.60}");
            let (ret, dst) = run(&src, &dst_in, siz, so, dof, &ctx);
            assert_eq!(ret, want_ret, "{ctx}: return");
            assert_eq!(dst, want_dst, "{ctx}: destination bytes");
        }

        let dlen = dst_in.iter().position(|&b| b == 0).unwrap_or(siz);
        n += 1;
        if siz == 0 {
            n_size0 += 1;
        }
        if siz == 1 {
            n_size1 += 1;
        }
        if siz > 0 && dlen == siz {
            n_nonul += 1;
        }
        if siz > 0 && dlen + 1 == siz {
            n_termonly += 1;
        }
        if dlen < siz && slen > siz - 1 - dlen {
            n_trunc += 1;
        }
        if dlen < siz && slen == siz - 1 - dlen {
            n_fit += 1;
        }
        if dlen < siz && slen < siz - 1 - dlen {
            n_slack += 1;
        }
        if src.len() > slen + 1 {
            n_junk += 1;
        }
        if slen >= 63 {
            n_long += 1;
        }
        if dlen == 0 && siz > 0 {
            n_empty_dst += 1;
        }
        if slen == 0 {
            n_empty_src += 1;
        }
        if dlen + 1 < siz {
            n_dst_junk += 1;
        }
    }

    // Floors: an emptied or truncated fixture must not read as green.
    assert!(n >= 800, "records: {n}");
    assert!(n_size0 >= 20, "size 0 records: {n_size0}");
    assert!(n_size1 >= 20, "size 1 records: {n_size1}");
    assert!(n_nonul >= 80, "no-NUL-within-siz records: {n_nonul}");
    assert!(n_termonly >= 80, "terminator-only-room records: {n_termonly}");
    assert!(n_trunc >= 150, "truncation records: {n_trunc}");
    assert!(n_fit >= 60, "exact-fit records: {n_fit}");
    assert!(n_slack >= 100, "slack records: {n_slack}");
    assert!(n_junk >= 100, "junk-after-source-NUL records: {n_junk}");
    assert!(n_long >= 30, "long-source records: {n_long}");
    assert!(n_empty_dst >= 80, "empty-destination records: {n_empty_dst}");
    assert!(n_empty_src >= 60, "empty-source records: {n_empty_src}");
    assert!(n_dst_junk >= 100, "junk-after-dst-NUL records: {n_dst_junk}");
}

/// size 0 must not touch dst even when it is a real pointer, must never read
/// it, and still returns the whole source length.
#[test]
fn size_zero_leaves_a_real_destination_alone() {
    let src = b"hello\0";
    let mut dst = [CANARY; 4];
    let r = unsafe { q_strlcat(dst.as_mut_ptr() as *mut _, src.as_ptr() as *const _, 0) };
    assert_eq!(r, 5);
    assert_eq!(dst, [CANARY; 4]);
}

/// The grid the retired differential harness covered, written from the
/// contract rather than from a fixture: every destination string length
/// 0..=40 (including ones longer than `siz`) against every source length
/// 0..=40, every size 0..=48, at source offsets {0, 3} and destination offsets
/// {0, 5}, inside guard zones.  The destination region is the string, its NUL
/// and junk, laid out contiguously; only the first `siz` bytes are the
/// function's to touch, so a destination string running past `siz` checks that
/// the scan stops at `siz` (the return is bounded by `siz`, not by
/// strlen(dst)) and that nothing beyond it, nor any guard byte, is written.
/// The whole image is compared with the contract, which also covers the
/// terminator, the append span and the untouched tail.
#[test]
fn full_dstlen_srclen_size_offset_grid_matches_the_contract() {
    const MAXD: usize = 40;
    const MAXS: usize = 40;
    const MAXSIZ: usize = 48;
    let mut checked = 0u64;
    let mut n_size_bounds_return = 0u64;
    for slen in 0..=MAXS {
        // Non-zero, never the canary or the destination byte.
        let mut s: Vec<u8> = (0..slen)
            .map(|i| match ((i * 7) % 250 + 1) as u8 {
                CANARY | 0x3c => 0x5a,
                b => b,
            })
            .collect();
        s.push(0);
        s.extend_from_slice(&[0x77; 5]); // junk after the NUL, never to be copied
        for dc in 0..=MAXD {
            // The string already in the destination: dc non-zero bytes, a NUL,
            // then junk.  `region` is long enough for every size to see a
            // prefix of it, and bytes past `siz` are not the function's.
            let mut region = vec![0u8; MAXD.max(MAXSIZ) + 2];
            for (i, b) in region.iter_mut().enumerate() {
                *b = if i < dc {
                    0x3c + (i % 5) as u8
                } else if i == dc {
                    0
                } else {
                    0x41
                };
            }
            for siz in 0..=MAXSIZ {
                let dst = &region[..siz.max(dc + 1)];
                let (eret, eimg) = {
                    let (r, img) = expected(&dst[..siz], &s, slen);
                    let mut full = dst.to_vec();
                    full[..siz].copy_from_slice(&img);
                    (r, full)
                };
                if siz < dc {
                    // dsize < strlen(dst): the size bounds the return value.
                    assert_eq!(eret, slen + siz);
                    n_size_bounds_return += 1;
                }
                for (so, dof) in [(0usize, 0usize), (3, 0), (0, 5), (3, 5)] {
                    let ctx = format!("dst string {dc} src {slen} siz {siz} src off {so} dst off {dof}");
                    let (ret, img) = run(&s, dst, siz, so, dof, &ctx);
                    assert_eq!(ret, eret, "{ctx}: return");
                    assert_eq!(img, eimg, "{ctx}: destination image");
                    checked += 1;
                }
            }
        }
    }
    assert_eq!(checked, 41 * 41 * 49 * 4);
    assert!(n_size_bounds_return >= 41 * 400, "dsize < strlen(dst) cases: {n_size_bounds_return}");
}
