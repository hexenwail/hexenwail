// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replays tests/fixtures/mathlib.txt, the frozen output of
// engine/h2shared/mathlib.c, against engine/rust/mathlib/src/ffi.rs.  No C
// compiler: the fixture is plain text.  Format and regeneration are in
// tests/fixtures/README.md.
//
// Comparison rule, taken from the retired differential harness: integers and
// every float routine except AngleVectors must match bit for bit (the harness
// allowed 1e-6 everywhere; exact is stricter).  AngleVectors goes through the
// host libm sin/cos, so it keeps the harness's absolute 1e-6 tolerance, which
// leaves room for a different libm on another target.

use mathlib_replay::ffi::*;

const FIXTURE: &str = include_str!("fixtures/mathlib.txt");

fn fbits(s: &str) -> f32 {
    assert_eq!(s.len(), 8, "bad float bits {s:?}");
    f32::from_bits(u32::from_str_radix(s, 16).unwrap())
}

fn floats<const N: usize>(f: &[&str]) -> [f32; N] {
    let mut a = [0.0f32; N];
    for (i, w) in f[..N].iter().enumerate() {
        a[i] = fbits(w);
    }
    a
}

/// Built by field assignment from a zeroed struct, so the padding's size is not
/// spelled out here (a layout mutant must fail the layout record, not compile).
fn plane(normal: [f32; 3], dist: f32, signbits: u8) -> MPlane {
    let mut p: MPlane = unsafe { core::mem::zeroed() };
    p.normal = normal;
    p.dist = dist;
    p.r#type = 3;
    p.signbits = signbits;
    p
}

fn int(s: &str) -> i32 {
    s.parse().unwrap_or_else(|_| panic!("bad int {s:?}"))
}

fn same_bits(got: &[f32], want: &[f32], at: &str) {
    assert_eq!(got.len(), want.len());
    for (i, (g, w)) in got.iter().zip(want).enumerate() {
        assert_eq!(g.to_bits(), w.to_bits(), "{at}: element {i}: got {g:e} want {w:e}");
    }
}

/// The retired harness's rule: equal, both NaN, or within 1e-6.
fn near(got: &[f32], want: &[f32], at: &str) {
    for (i, (g, w)) in got.iter().zip(want).enumerate() {
        let ok = g == w || (g.is_nan() && w.is_nan()) || (g - w).abs() <= 1e-6;
        assert!(ok, "{at}: element {i}: got {g:e} want {w:e}");
    }
}

#[test]
fn replay_c_fixtures() {
    let mut n = std::collections::BTreeMap::<&str, usize>::new();
    let mut bops_sig = [0usize; 8];
    let mut bops_res = [0usize; 4];
    let (mut nan_true, mut nan_false) = (0, 0);
    let (mut fdm_neg, mut fdm_neg_exact, mut fdm_pos) = (0, 0, 0);
    let (mut near_neg, mut near_adjusted) = (0, 0);
    let (mut inv_small, mut inv_big) = (0, 0);

    for (lineno, line) in FIXTURE.lines().enumerate() {
        let at = format!("mathlib.txt:{}: {line}", lineno + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        let kind = f[0];
        match kind {
            "layout" => {
                let want: Vec<usize> = f[1..].iter().map(|s| s.parse().unwrap()).collect();
                assert_eq!(size_of::<MPlane>(), want[0], "{at}: sizeof");
                assert_eq!(MathlibMPlane_sizeof(), want[0], "{at}: exported sizeof");
                assert_eq!(MathlibMPlane_offsetof_normal(), want[1], "{at}");
                assert_eq!(MathlibMPlane_offsetof_dist(), want[2], "{at}");
                assert_eq!(MathlibMPlane_offsetof_type(), want[3], "{at}");
                assert_eq!(MathlibMPlane_offsetof_signbits(), want[4], "{at}");
            }
            "isnan" => {
                let x = fbits(f[1]);
                let want = int(f[2]);
                assert_eq!(unsafe { Q_isnan(x) }, want, "{at}");
                if want == 1 { nan_true += 1 } else { nan_false += 1 }
            }
            "anglemod" => {
                let got = unsafe { anglemod(fbits(f[1])) };
                assert_eq!(got.to_bits(), fbits(f[2]).to_bits(), "{at}: got {got:e}");
            }
            "log2" => assert_eq!(unsafe { Q_log2(int(f[1])) }, int(f[2]), "{at}"),
            "inv" => {
                let v = int(f[1]);
                assert_eq!(unsafe { Invert24To16(v) }, int(f[2]), "{at}");
                if v < 256 { inv_small += 1 } else { inv_big += 1 }
            }
            "gcd" => {
                let got = unsafe { GreatestCommonDivisor(int(f[1]), int(f[2])) };
                assert_eq!(got, int(f[3]), "{at}");
            }
            "nearest" => {
                let (num, mul) = (int(f[1]), int(f[2]));
                assert_eq!(unsafe { NearestMultiple(num, mul) }, int(f[3]), "{at}");
                if num < 0 {
                    near_neg += 1;
                    if num % mul != 0 {
                        near_adjusted += 1;
                    }
                }
            }
            "anglevec" => {
                let ang: [f32; 3] = floats(&f[1..]);
                let want: [f32; 9] = floats(&f[4..]);
                let (mut fw, mut rt, mut up) = ([0.0f32; 3], [0.0f32; 3], [0.0f32; 3]);
                unsafe { AngleVectors(ang.as_ptr(), fw.as_mut_ptr(), rt.as_mut_ptr(), up.as_mut_ptr()) };
                let got: Vec<f32> = fw.iter().chain(&rt).chain(&up).copied().collect();
                near(&got, &want, &at);
                // The C original skips right and up when they are NULL; forward
                // must come out the same.
                let mut fw2 = [0.0f32; 3];
                unsafe {
                    AngleVectors(ang.as_ptr(), fw2.as_mut_ptr(), core::ptr::null_mut(), core::ptr::null_mut())
                };
                same_bits(&fw2, &fw, &at);
            }
            "rot" => {
                let a: [[f32; 3]; 3] = unsafe { core::mem::transmute(floats::<9>(&f[1..])) };
                let b: [[f32; 3]; 3] = unsafe { core::mem::transmute(floats::<9>(&f[10..])) };
                let want: [f32; 9] = floats(&f[19..]);
                let mut o = [[0.0f32; 3]; 3];
                unsafe { R_ConcatRotations(&a, &b, &mut o) };
                same_bits(o.as_flattened(), &want, &at);
            }
            "xform" => {
                let a: [[f32; 4]; 3] = unsafe { core::mem::transmute(floats::<12>(&f[1..])) };
                let b: [[f32; 4]; 3] = unsafe { core::mem::transmute(floats::<12>(&f[13..])) };
                let want: [f32; 12] = floats(&f[25..]);
                let mut o = [[0.0f32; 4]; 3];
                unsafe { R_ConcatTransforms(&a, &b, &mut o) };
                same_bits(o.as_flattened(), &want, &at);
            }
            "bops" => {
                let sig = int(f[1]);
                let normal: [f32; 3] = floats(&f[2..]);
                let dist = fbits(f[5]);
                let mins: [f32; 3] = floats(&f[6..]);
                let maxs: [f32; 3] = floats(&f[9..]);
                let want = int(f[12]);
                let pl = plane(normal, dist, sig as u8);
                let got = unsafe { BoxOnPlaneSide(mins.as_ptr(), maxs.as_ptr(), &pl) };
                assert_eq!(got, want, "{at}");
                bops_sig[sig as usize] += 1;
                bops_res[want as usize] += 1;
            }
            "fdm" => {
                let (numer, denom) = (int(f[1]), int(f[2]));
                let (mut q, mut r) = (-12345, -12345);
                unsafe { FloorDivMod(numer as f64, denom as f64, &mut q, &mut r) };
                assert_eq!((q, r), (int(f[3]), int(f[4])), "{at}");
                if numer >= 0 {
                    fdm_pos += 1;
                } else {
                    fdm_neg += 1;
                    if numer % denom == 0 {
                        fdm_neg_exact += 1;
                    }
                }
            }
            other => panic!("{at}: unknown record {other:?}"),
        }
        *n.entry(kind).or_default() += 1;
    }

    // Floors: an emptied or truncated fixture must not read as green, and each
    // routine's branches must stay represented.
    let floor = |k: &str, min: usize| {
        let got = n.get(k).copied().unwrap_or(0);
        assert!(got >= min, "{k} records: {got}, floor {min}");
    };
    assert_eq!(n["layout"], 1);
    floor("isnan", 40);
    floor("anglemod", 60);
    floor("log2", 90);
    floor("inv", 40);
    floor("gcd", 100);
    floor("nearest", 250);
    floor("anglevec", 120);
    floor("rot", 40);
    floor("xform", 40);
    floor("bops", 120);
    floor("fdm", 190);
    assert!(nan_true >= 15 && nan_false >= 25, "isnan outcomes: {nan_true} nan, {nan_false} not");
    assert!(inv_small >= 5 && inv_big >= 30, "inv branches: {inv_small} small, {inv_big} big");
    assert!(near_neg >= 100 && near_adjusted >= 60, "nearest negatives: {near_neg}, adjusted {near_adjusted}");
    assert!(fdm_pos >= 90 && fdm_neg >= 90 && fdm_neg_exact >= 20, "fdm: {fdm_pos} pos, {fdm_neg} neg, {fdm_neg_exact} exact");
    for (s, c) in bops_sig.iter().enumerate() {
        assert!(*c >= 14, "bops signbits {s}: {c} records");
    }
    for (r, c) in bops_res.iter().enumerate().skip(1) {
        assert!(*c >= 20, "bops result {r}: {c} records");
    }
}

/// The three `if` guards in the C AngleVectors: NULL angles or forward is a
/// no-op, NULL right and up are skipped (forward still written).
#[test]
fn angle_vectors_null_guards() {
    let ang = [10.0f32, 20.0, 30.0];
    let mut fw = [7.0f32; 3];
    unsafe {
        AngleVectors(core::ptr::null(), fw.as_mut_ptr(), core::ptr::null_mut(), core::ptr::null_mut());
        AngleVectors(ang.as_ptr(), core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut());
    }
    assert_eq!(fw, [7.0; 3]);
}

/// BoxOnPlaneSide with bad signbits and FloorDivMod with a non-positive
/// denominator call Sys_Error, which never returns.  Re-run this test binary
/// as a child with the trigger set and require that it aborts with the message.
fn aborts(trigger: &str, test: &str, needle: &str) {
    if std::env::var("MATHLIB_REPLAY_TRIGGER").as_deref() == Ok(trigger) {
        unsafe {
            match trigger {
                "bops" => {
                    let p = plane([1.0; 3], 0.0, 8);
                    BoxOnPlaneSide([0.0f32; 3].as_ptr(), [1.0f32; 3].as_ptr(), &p);
                }
                "fdm0" | "fdm-1" => {
                    let d = if trigger == "fdm0" { 0.0 } else { -1.0 };
                    let (mut q, mut r) = (0, 0);
                    FloorDivMod(5.0, d, &mut q, &mut r);
                }
                _ => unreachable!(),
            }
        }
        panic!("{trigger}: returned instead of calling Sys_Error");
    }
    let out = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", test, "--nocapture", "--test-threads=1"])
        .env("MATHLIB_REPLAY_TRIGGER", trigger)
        .output()
        .unwrap();
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(!out.status.success(), "{trigger}: child exited cleanly");
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(out.status.signal(), Some(6), "{trigger}: want SIGABRT, got {:?}\n{err}", out.status);
    }
    assert!(err.contains(needle), "{trigger}: stderr lacks {needle:?}:\n{err}");
}

#[test]
fn bops_bad_signbits_calls_sys_error() {
    aborts("bops", "bops_bad_signbits_calls_sys_error", "BoxOnPlaneSide:  Bad signbits");
}

#[test]
fn floordivmod_zero_denominator_calls_sys_error() {
    aborts("fdm0", "floordivmod_zero_denominator_calls_sys_error", "bad denominator");
}

#[test]
fn floordivmod_negative_denominator_calls_sys_error() {
    aborts("fdm-1", "floordivmod_negative_denominator_calls_sys_error", "bad denominator");
}
