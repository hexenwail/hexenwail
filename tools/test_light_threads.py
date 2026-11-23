#!/usr/bin/env python3
"""light writes the same BSP with any number of threads (#530).

light's worker threads each took the next free run of dlightdata as they
finished a face, so with more than one thread the order of the faces in the
lighting lump, and every face's lightofs, followed the thread schedule.
Nothing was lost, but -threads 8 wrote a different BSP on each run, and so
did every tool downstream of it.

This test compiles the map tools/test_jsh2colour_threads.py generates (many
small faces under many lights, so the threads finish faces in a different
order every run) with the qbsp that sits next to the light under test, then
checks that each -threads N lit BSP is byte-identical to the -threads 1 one,
in both the BSP29 and the BSP2 format.

With retail Hexen II data present (--retail, default ~/hexen2), it relights
keep2.bsp and demo1.bsp the same way.  With no data the retail part prints
SKIP and the test still runs on the fixture.

  test_light_threads.py UTILS_BIN [--runs N] [--threads N] [--retail DIR]
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_jsh2colour_threads as fixture  # noqa: E402


def lit_bsp(light, unlit, threads):
    work = unlit.with_name("work.bsp")
    shutil.copyfile(unlit, work)
    out = fixture.run([light, "-threads", str(threads), work.name], work.parent)
    # a light that quietly ran fewer threads would make the check a no-op
    if "Setup for %d threads" % threads not in out:
        sys.exit("FAIL: light did not run %d threads:\n%s" % (threads, out[-2000:]))
    lit = work.read_bytes()
    if lit == unlit.read_bytes():
        sys.exit("FAIL: light left %s as it was" % unlit.name)
    return hashlib.sha256(lit).hexdigest()


def check(label, light, unlit, threads, runs):
    want = lit_bsp(light, unlit, 1)
    bad = 0
    for i in range(runs):
        got = lit_bsp(light, unlit, threads)
        if got != want:
            bad += 1
            print("  %s run %d -threads %d: %s != -threads 1 %s"
                  % (label, i + 1, threads, got[:16], want[:16]))
    if bad:
        print("FAIL: %s: %d of %d -threads %d runs differ from -threads 1"
              % (label, bad, runs, threads))
        return False
    print("ok: %s: %d -threads %d runs match -threads 1 (%s)"
          % (label, runs, threads, want[:16]))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bindir", type=Path)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--retail", type=Path, default=Path.home() / "hexen2")
    ap.add_argument("--retail-runs", type=int, default=3)
    a = ap.parse_args()

    tools = {t: a.bindir / t for t in ("qbsp", "light")}
    for t, p in tools.items():
        if not p.is_file():
            sys.exit("FAIL: no %s at %s" % (t, p))

    ok = True
    with tempfile.TemporaryDirectory(prefix="light-threads-") as tmp:
        tmp = Path(tmp)
        fixture.write_wad(tmp / "fixture.wad",
                          sorted(fixture.COLOURS) + [fixture.WALL])
        fixture.write_map(tmp / "fixture.map", "fixture.wad")
        fixture.run([tools["qbsp"], "fixture.map"], tmp)
        ok &= check("fixture", tools["light"], tmp / "fixture.bsp",
                    a.threads, a.runs)
        # BSP2 has its own face struct and its own path through PlaceFaceSpace
        shutil.copyfile(tmp / "fixture.map", tmp / "fixture2.map")
        fixture.run([tools["qbsp"], "-bsp2", "fixture2.map"], tmp)
        if (tmp / "fixture2.bsp").read_bytes()[:4] != b"BSP2":
            sys.exit("FAIL: qbsp -bsp2 did not write a BSP2 file")
        ok &= check("fixture-bsp2", tools["light"], tmp / "fixture2.bsp",
                    a.threads, a.runs)

        retail = (("keep2", a.retail / "portals" / "pak3.pak"),
                  ("demo1", a.retail / "data1" / "pak0.pak"))
        for name, pak in retail:
            data = (fixture.pak_member([pak], "maps/%s.bsp" % name)
                    if pak.is_file() else None)
            if data is None:
                print("SKIP: %s: no maps/%s.bsp in %s (retail Hexen II)"
                      % (name, name, pak))
                continue
            (tmp / ("%s.bsp" % name)).write_bytes(data)
            ok &= check(name, tools["light"], tmp / ("%s.bsp" % name),
                        a.threads, a.retail_runs)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
