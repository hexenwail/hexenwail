#!/usr/bin/env python3
"""Gate for tools/softemu-palette.py and the sweep it serves (#340 P4d).

softemu-diff.sh's palette-membership check is only as good as the palette it
is handed and the counter that reads the frames, and a check that silently
passes is worse than none.  So, on synthetic data (no game files):

  - dump finds gfx/palette.lmp in a pak, the highest-numbered pak first (pak0-pak9 only), a
    pk3 over the paks and a loose file over both -- FS_AddGameDirectory's
    order -- and prints 256 RRGGBB lines;
  - outside counts exactly the pixels whose colour is not among entries
    0-254: entry 255 (the transparency key no LUT emits) counts as outside,
    and the region argument limits the count;
  - every frame tools/softemu-diff.sh checks is shot by
    tools/sweep-softemu-gpu.steps, and every REPORT section it reads is
    echoed there, so a renamed frame cannot turn a check into a MISSING it
    never reaches or a section into one it never finds.
"""
import importlib.util
import io
import re
import struct
import sys
import tempfile
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True	# the module is loaded by path; a cached copy can outlive an edit
TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("softemu_palette", TOOLS / "softemu-palette.py")
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)

errors = []


def check(cond, msg):
    if not cond:
        errors.append(msg)


def lmp(seed):
    return bytes((i * 3 + c + seed) & 0xff for i in range(256) for c in range(3))


def pak(path, files):
    body, ents = b"", []
    for name, data in files.items():
        ents.append((name, 12 + len(body), len(data)))
        body += data
    d = b"".join(struct.pack("<56sii", n.encode(), o, l) for n, o, l in ents)
    path.write_bytes(b"PACK" + struct.pack("<ii", 12 + len(body), len(d)) + body + d)


def dump(game):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = sp.main(["dump", str(game)])
    return rc, out.getvalue().split()


def first(seed):
    return "%02x%02x%02x" % tuple(lmp(seed)[:3])


with tempfile.TemporaryDirectory() as t:
    g = Path(t) / "data1"
    g.mkdir()
    pak(g / "pak0.pak", {"gfx/palette.lmp": lmp(0), "gfx/other.lmp": b"x" * 9})
    rc, lines = dump(g)
    check(rc == 0 and len(lines) == 256, "dump: want 256 lines, got %d (rc %s)" % (len(lines), rc))
    check(lines and lines[0] == first(0), "dump: pak0's palette not read")
    check(all(re.fullmatch(r"[0-9a-f]{6}", x) for x in lines), "dump: a line is not RRGGBB")
    pak(g / "pak2.pak", {"GFX/PALETTE.LMP": lmp(7)})	# names are matched case-blind
    pak(g / "pak10.pak", {"gfx/palette.lmp": lmp(9)})	# FS_AddGameDirectory loads pak0-pak9 only
    check(dump(g)[1][0] == first(7), "dump: pak2 must win over pak0, and pak10 is never read")
    with zipfile.ZipFile(g / "a.pk3", "w") as z:
        z.writestr("gfx/palette.lmp", lmp(11))
    with zipfile.ZipFile(g / "b.pk3", "w") as z:
        z.writestr("gfx/palette.lmp", lmp(13))
    check(dump(g)[1][0] == first(13), "dump: the alphabetically last pk3 must win over the paks")
    (g / "gfx").mkdir()
    (g / "gfx/palette.lmp").write_bytes(lmp(17))
    check(dump(g)[1][0] == first(17), "dump: a loose gfx/palette.lmp must win over pk3s and paks")

# dump warns when entry 255's colour is an emitted entry's too (retail's is),
# since `outside` cannot see an emitted 255 then.
with tempfile.TemporaryDirectory() as t:
    g = Path(t)
    dup = bytearray(lmp(0))
    dup[765:768] = dup[93:96]	# 255 = entry 31, as in retail
    for data, want in [(bytes(dup), True), (lmp(0), False)]:
        pak(g / "pak0.pak", {"gfx/palette.lmp": data})
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            sp.main(["dump", str(g)])
        check(("entry 255" in err.getvalue()) == want,
              "dump: entry-255 warning %s" % ("missing on a duplicate" if want else "on a unique 255"))

pal = [bytes(((i * 5) & 0xff, (i * 7) & 0xff, i)) for i in range(256)]
# A 4x3 frame: entries 0, 1, 254 are in; entry 255 and a stranger are out.
px = [pal[0], pal[1], pal[254], pal[255],
      pal[2], b"\x01\x02\x03", pal[3], pal[4],
      pal[5], pal[6], pal[7], pal[255]]
frame = b"".join(px)
n, ex = sp.outside(pal, 4, frame)
check(n == 3, "outside: want 3 (two entry-255 pixels and one stranger), got %d" % n)
check(ex and ex[0] == "3,0 %s" % pal[255].hex(), "outside: first example should be 3,0, got %r" % ex[:1])
n, _ = sp.outside(pal, 4, frame, (0, 0, 3, 3))
check(n == 1, "outside: region 0,0,3,3 holds one stranger, got %d" % n)
n, _ = sp.outside(pal, 4, frame, (0, 2, 3, 1))
check(n == 0, "outside: region 0,2,3,1 is all in, got %d" % n)
for bad in [(0, 0, 5, 1), (0, 2, 1, 2)]:
    try:
        sp.outside(pal, 4, frame, bad)
        errors.append("outside: region %r past the frame accepted" % (bad,))
    except SystemExit:
        pass
try:
    sp.outside(pal, 4, frame[:-1])
    errors.append("outside: a frame that is not whole rows accepted")
except SystemExit:
    pass

steps = (TOOLS / "sweep-softemu-gpu.steps").read_text()
diff = (TOOLS / "softemu-diff.sh").read_text()
shots = set(re.findall(r"^shot\s+(\S+)", steps, re.M))
m = re.search(r'^FRAMES=\$\{FRAMES:-"([^"]*)"\}', diff, re.M)
check(m is not None, "softemu-diff.sh: no FRAMES default found")
named = set(m.group(1).split()) if m else set()
check(named and named <= shots, "softemu-diff.sh names frames the steps never shoot: %s"
      % " ".join(sorted(named - shots)))
check(shots <= named, "the steps shoot frames softemu-diff.sh never checks: %s"
      % " ".join(sorted(shots - named)))
m = re.search(r'^SECTIONS="([A-Z ]+)"', diff, re.M)
check(m is not None, "softemu-diff.sh: no SECTIONS list found")
for s in (m.group(1).split() if m else []):
    body = re.search(r"^cmd gpu_unrouted reset\n(?:(?!^cmd gpu_unrouted reset).*\n)*?"
                     r"cmd echo REPORT-%s-BEGIN\ncmd gpu_unrouted\n(?:cmd .*\n)*?cmd echo REPORT-%s-END$"
                     % (s, s), steps, re.M)
    check(body is not None, "steps: REPORT-%s must be `cmd echo REPORT-%s-BEGIN`, `cmd gpu_unrouted`, ..., "
          "`cmd echo REPORT-%s-END`, after a `cmd gpu_unrouted reset`" % (s, s, s))
    if body:
        check(re.search(r"^shot ", body.group(0), re.M) is not None,
              "steps: REPORT-%s counts no shot (none between its reset and its BEGIN)" % s)

# The REPORT parser softemu-diff.sh's check 6 runs.
OK = ("REPORT-GY-BEGIN \ngpu_unrouted: 0 calls into 0 of 155 GL entry points\n"
      "gpu_unrouted: 0 routed calls the SDL_GPU backend did not handle, 0 kinds\nREPORT-GY-END\n")
check(sp.reports(OK, ["GY"]) == [], "reports: a clean section flagged: %r" % sp.reports(OK, ["GY"]))
for log, why in [
        (OK.replace(": 0 calls", ": 3 calls"), "nonzero calls"),
        (OK.replace(": 0 routed", ": 2 routed"), "nonzero routed"),
        (OK.replace("REPORT-GY-BEGIN \n", ""), "BEGIN missing"),
        (OK.replace("REPORT-GY-END\n", ""), "END missing"),
        (OK.replace("gpu_unrouted: 0 routed calls the SDL_GPU backend did not handle, 0 kinds\n", ""), "a count missing"),
        ("gpu_unrouted: not in SDL_GPU mode (start with -gpu)\n", "a GL log"),
        (OK.replace("GY", "POOLS"), "the wrong section"),
        ("]echo " + OK, "the echoed command line, not its output"),
        ("REPORT-GY-BEGIN \nREPORT-GY-END\n" + OK.split("REPORT-GY-BEGIN \n")[1].replace("REPORT-GY-END\n", ""),
         "counts from outside the section")]:
    check(sp.reports(log, ["GY"]) != [], "reports: %s passed" % why)

if errors:
    for e in errors:
        print("FAIL:", e)
    sys.exit(1)
print("test_softemu_palette: OK (%d frames, dumper search order, membership counter)" % len(named))
