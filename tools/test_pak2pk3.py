#!/usr/bin/env python3
"""Gate test for tools/pak2pk3.py (GitHub #129), with no game data.

Builds synthetic paks -- carrying the exact member sizes the engine's content
identification looks for, so they read as v1.11 data1 -- and checks the
converter keeps what the engine relies on:
  - duplicate member names survive, in pak order, each with its own bytes
    (retail data1/pak1.pak has three; the engine resolves them by order);
  - every member's size and bytes are exact, including an empty one;
  - output names keep pak1 over pak0 and sort before a user's own pk3s,
    under the engine's case-insensitive name order;
  - --loose packs exactly the loose game files, from the top level only;
  - it refuses: content the engine would not recognise as a pk3 (demo, OEM,
    hw), non-ASCII member names, misnamed paks; and leaves nothing behind;
  - its copy of the content marks matches quakefs.c's contentdata[].
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/pak2pk3.py"
sys.path.insert(0, str(ROOT / "tools"))
import pak2pk3  # noqa: E402


def make_pak(path, members):
    body, entries, ofs = b"", [], 12
    for name, blob in members:
        raw = name if isinstance(name, bytes) else name.encode()
        entries.append((raw, ofs, len(blob)))
        body += blob
        ofs += len(blob)
    directory = b"".join(raw.ljust(56, b"\0") + struct.pack("<ii", o, n)
                         for raw, o, n in entries)
    path.write_bytes(b"PACK" + struct.pack("<ii", 12 + len(body), len(directory))
                     + body + directory)


def marks(key):
    return [(n, b"\x5a" * s) for n, s in pak2pk3.CONTENT_MARKS[key][1].items()]


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def engine_order(names):
    # quakefs.c sorts pk3 names with q_strcasecmp (common.c): ASCII case
    # folded, then compared as SIGNED chars, so a byte >= 0x80 sorts first.
    # The last one mounted wins.
    def key(n):
        return [b - 256 if b >= 128 else b for b in n.encode("latin-1").lower()]
    return sorted(names, key=key)


def main():
    errors = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            errors.append(what)

    # The tool's limits must be the engine's.
    qfs_text = (ROOT / "engine/h2shared/quakefs.c").read_text()
    m = re.search(r"#define\s+MAX_ZIP_INFLATE\s+\((\d+)\s*\*\s*(\d+)\s*\*\s*(\d+)\)", qfs_text)
    check(m is not None and int(m[1]) * int(m[2]) * int(m[3]) == pak2pk3.MAX_ZIP_INFLATE,
          "MAX_ZIP_INFLATE matches quakefs.c")
    m = re.search(r"#define\s+MAX_PK3_PER_DIR\s+(\d+)", qfs_text)
    check(m is not None and int(m[1]) == pak2pk3.MAX_PK3_PER_DIR,
          "MAX_PK3_PER_DIR matches quakefs.c")
    m = re.search(r"#define\s+MAX_FILES_IN_PACK\s+(\d+)", (ROOT / "common/pakfile.h").read_text())
    check(m is not None and int(m[1]) == pak2pk3.MAX_FILES_IN_PACK,
          "MAX_FILES_IN_PACK matches common/pakfile.h")

    # The tool's marks must be the engine's.
    qfs = (ROOT / "engine/h2shared/quakefs.c").read_text()
    for key, cname in (("data1 pak0", "mark_data1_pak0"),
                       ("data1 pak1", "mark_data1_pak1"),
                       ("portals", "mark_portals")):
        block = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % cname, qfs, re.S)
        engine = dict((n, int(s)) for n, s in
                      re.findall(r'\{\s*"([^"]+)",\s*(\d+)\s*\}', block.group(1))) if block else {}
        check(engine == pak2pk3.CONTENT_MARKS[key][1],
              "the tool's %s marks match quakefs.c %s" % (key, cname))
        rblock = re.search(r"static %s: \[ContentMarkC; \d+\] = \[(.*?)\];" % cname.upper(),
                           (ROOT / "engine/rust/src/quakefs.rs").read_text(), re.S)
        rust = dict((n, int(s)) for n, s in
                    re.findall(r'name: c"([^"]+)", filelen: (\d+)', rblock.group(1))) if rblock else {}
        check(rust == pak2pk3.CONTENT_MARKS[key][1],
              "the tool's %s marks match the shipped Rust %s" % (key, cname.upper()))

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        game = work / "data1"
        game.mkdir()
        pak0 = marks("data1 pak0") + [
            ("models/mumshot.mdl", b"first copy" * 100),
            ("sound/a.wav", os.urandom(5000)),
            ("models/mumshot.mdl", b"second copy, different" * 50),
            ("empty.txt", b""),
            ("gfx/x.lmp", b"\0" * 70000),
        ]
        pak1 = marks("data1 pak1") + [("maps/extra.bsp", os.urandom(20000))]
        make_pak(game / "pak0.pak", pak0)
        make_pak(game / "pak1.pak", pak1)
        make_pak(game / "pak0_old.pak", pak0)     # the engine never opens this
        (game / "PROGS.DAT").write_bytes(b"gamecode")
        (game / "Hexen.rc").write_bytes(b"exec default.cfg\n")
        (game / "config.cfg").write_bytes(b"user settings")
        (game / "demo1.dem").write_bytes(b"a recorded demo")
        (game / "s0").mkdir()
        (game / "s0" / "info.dat").write_bytes(b"a savegame")
        (game / "maps").mkdir()
        (game / "maps" / "loose.bsp").write_bytes(b"loose map")

        out = work / "out"
        r = run(game, out, "--loose")
        print(r.stdout, end="")
        check(r.returncode == 0, "converter exits 0: " + r.stderr.strip())
        if r.returncode != 0:
            sys.exit("FAIL: the main conversion failed; nothing else is meaningful")

        with zipfile.ZipFile(out / "000-pak0.pk3") as z:
            infos = z.infolist()
            check([i.filename for i in infos] == [n for n, _ in pak0],
                  "pak0 keeps every member in pak order, duplicates included")
            check(all(z.read(i) == blob for i, (_, blob) in zip(infos, pak0)),
                  "every member's bytes are exact (both duplicate copies distinct)")
            check([i.file_size for i in infos] == [len(b) for _, b in pak0],
                  "every member's size is exact, the empty one included")
            big = [i for i in infos if i.filename == "gfx/x.lmp"][0]
            check(big.compress_type == zipfile.ZIP_DEFLATED and big.compress_size < 1000,
                  "members are DEFLATE-compressed")
        check("1 duplicate name kept" in r.stdout, "the duplicate is reported, not hidden")
        check("skipped pak0_old.pak" in r.stdout and not (out / "000-pak0_old.pk3").exists(),
              "a pak the engine never opens is skipped, not converted")

        names = sorted(p.name for p in out.iterdir())
        check(names == ["000-pak0.pk3", "000-pak1.pk3", "zz-loose.pk3"],
              "outputs: %s" % names)
        order = engine_order(names + ["hd.pk3", "a_textures.pk3", "Z_mod.pk3",
                                      "_x.pk3", "00a.pk3", "1.pk3"])
        check(order.index("000-pak0.pk3") < order.index("000-pak1.pk3")
              < min(order.index(n) for n in ("hd.pk3", "a_textures.pk3", "Z_mod.pk3",
                                             "_x.pk3", "00a.pk3", "1.pk3")),
              "pak1 still beats pak0, and a user's pk3s still beat both")
        below = engine_order(["000-pak1.pk3", "!x.pk3", "00.pk3", "000-hd.pk3", "\xe9x.pk3"])
        check(below[-1] == "000-pak1.pk3",
              "the docstring's examples of names that sort below 000-pak1 really do: %s" % below)
        check([pak2pk3.engine_key(n) for n in engine_order(["b", "A", "\xe9", "000-pak1.pk3"])]
              == sorted(pak2pk3.engine_key(n) for n in ["b", "A", "\xe9", "000-pak1.pk3"]),
              "the tool's engine_key orders names the way the test's model of q_strcasecmp does")
        check(order[-1] == "zz-loose.pk3", "the loose archive beats the paks and common pk3 names")

        with zipfile.ZipFile(out / "zz-loose.pk3") as z:
            loose = sorted(z.namelist())
        check(loose == ["Hexen.rc", "PROGS.DAT"],
              "--loose packs only the loose game files, top level only: %s" % loose)
        check(not list(out.glob(".pak2pk3-*")), "no temporary files left behind")

        r2 = run(game, work / "out2")
        check(r2.returncode == 0 and "PROGS.DAT stay loose" in r2.stdout,
              "without --loose the loose gamecode is named")

        # Demo-like content: a pak0 without the v1.11 marks.  Refused, and
        # nothing is written.
        demo = work / "demo" / "data1"
        demo.mkdir(parents=True)
        make_pak(demo / "pak0.pak", [("maps/demo1.bsp", b"x" * 100)])
        r3 = run(demo, work / "out3")
        check(r3.returncode != 0 and "pak0.pak and pak1.pak pair" in r3.stderr
              and not (work / "out3").exists(),
              "unrecognised content is refused before anything is written")
        r3f = run(demo, work / "out3f", "--force")
        check(r3f.returncode != 0 and not (work / "out3f").exists(),
              "--force does not override data1's pak0+pak1 requirement")

        # The real demo's shape: a pak0 WITH the v1.11 pak0 marks and no pak1.
        # The engine cannot identify that as pk3s (measured on the real demo:
        # "Unable to find a proper Hexen II installation"), so it is refused.
        demo2 = work / "demo2" / "data1"
        demo2.mkdir(parents=True)
        make_pak(demo2 / "pak0.pak", marks("data1 pak0"))
        r3b = run(demo2, work / "out3b")
        check(r3b.returncode != 0 and "pak0.pak and pak1.pak pair" in r3b.stderr
              and not (work / "out3b").exists(),
              "a data1 with pak0 content but no pak1 (the demo) is refused")

        # HexenWorld's hw: refused outright, --force included (a converted
        # pak4 stops hwsv and hwcl).
        hw = work / "hw"
        hw.mkdir()
        make_pak(hw / "pak4.pak", [("maps/hwmap.bsp", b"h" * 10)])
        r4 = run(hw, work / "out4", "--force")
        check(r4.returncode != 0 and "HexenWorld" in r4.stderr and "--force" not in r4.stderr
              and not (work / "out4").exists(),
              "hw is refused even with --force, and the message offers no override")

        # A pre-1.11/OEM-shaped data1 (pak0 and pak1, neither with the v1.11
        # marks): the data1 rule answers first, without suggesting --force or
        # moving pak0 away.
        old = work / "old" / "data1"
        old.mkdir(parents=True)
        make_pak(old / "pak0.pak", [("maps/demo1.bsp", b"o" * 10)])
        make_pak(old / "pak1.pak", [("maps/cath.bsp", b"o" * 10)])
        r4b = run(old, work / "out4b")
        check(r4b.returncode != 0 and "pair" in r4b.stderr and "--force" not in r4b.stderr
              and "move" not in r4b.stderr.lower(),
              "a non-v1.11 data1 is refused by the data1 rule, offering nothing that cannot work")
        r4c = run(old, work / "out4c", "--force")
        check(r4c.returncode != 0 and "warning" not in r4c.stdout and not (work / "out4c").exists(),
              "--force does not pretend to convert it")

        # data1 content in a directory that is not data1 (a mod): needs --force.
        wrong = work / "mymod"
        wrong.mkdir()
        make_pak(wrong / "pak0.pak", [("maps/mod.bsp", b"m" * 10)])
        r4d = run(wrong, work / "out4d")
        check(r4d.returncode != 0 and "mod's directory" in r4d.stderr,
              "a mod's gamedir needs --force")
        r4e = run(wrong, work / "out4e", "--force")
        check(r4e.returncode == 0 and (work / "out4e" / "000-pak0.pk3").exists(),
              "--force converts a mod's gamedir")

        # A non-ASCII member name: refused, nothing written.
        latin = work / "latin" / "data1"
        latin.mkdir(parents=True)
        make_pak(latin / "pak0.pak", marks("data1 pak0") + [(b"sound/caf\xe9.wav", b"x")])
        r5 = run(latin, work / "out5")
        check(r5.returncode != 0 and "not ASCII" in r5.stderr and not (work / "out5").exists(),
              "a non-ASCII member name is refused")

        # Corrupt pak: refused, nothing written.
        bad = work / "bad" / "data1"
        bad.mkdir(parents=True)
        (bad / "pak0.pak").write_bytes(b"PACK" + struct.pack("<ii", 999999, 64))
        r6 = run(bad, work / "out6")
        check(r6.returncode != 0 and "corrupt" in r6.stderr and not (work / "out6").exists(),
              "a corrupt pak is refused and nothing is written")

        check((game / "pak0.pak").read_bytes()[:4] == b"PACK", "source files are not modified")

        # Permissions: what open() would give under the umask, not mkstemp's 0600.
        old = os.umask(0o022)
        try:
            r7 = run(game, work / "out7")
        finally:
            os.umask(old)
        modes = ({oct(os.stat(f).st_mode & 0o777) for f in (work / "out7").iterdir()}
                 if (work / "out7").is_dir() else {"no output"})
        check(r7.returncode == 0 and modes == {"0o644"},
              "outputs are 0644 under umask 022, readable by the account that runs the game: %s" % modes)

        # "." names the directory.
        r8 = subprocess.run([sys.executable, str(TOOL), ".", str(work / "out8")],
                            cwd=game, capture_output=True, text=True)
        check(r8.returncode == 0 and (work / "out8" / "000-pak0.pk3").exists(),
              "the gamedir can be given as '.': " + r8.stderr.strip()[:120])

        # A registered data1 plus a mod's pak5: refused for the real reason.
        mod = work / "mod" / "data1"
        mod.mkdir(parents=True)
        make_pak(mod / "pak0.pak", marks("data1 pak0"))
        make_pak(mod / "pak1.pak", marks("data1 pak1"))
        make_pak(mod / "pak5.pak", [("maps/modmap.bsp", b"m" * 50)])
        r9 = run(mod, work / "out9")
        check(r9.returncode != 0 and "outranks every pak" in r9.stderr
              and not (work / "out9").exists(),
              "an extra pak stops the run, naming the precedence problem")
        r9f = run(mod, work / "out9f", "--force")
        check(r9f.returncode == 0 and (work / "out9f" / "000-pak5.pk3").exists(),
              "--force converts the extra pak too")

        # --loose alone on a data1 already converted (no paks left).
        done = work / "done" / "data1"
        done.mkdir(parents=True)
        (done / "PROGS.DAT").write_bytes(b"gamecode")
        r10 = run(done, work / "out10", "--loose")
        check(r10.returncode == 0 and (work / "out10" / "zz-loose.pk3").exists(),
              "--loose works on a data1 whose paks are already converted")

        # Portals: infolist.txt is a loose file the mission pack cannot run
        # without, and it must be packed; anything else left is named.
        portals = work / "portals"
        portals.mkdir()
        make_pak(portals / "pak3.pak", marks("portals"))
        for f in ("progs.dat", "strings.txt", "puzzles.txt", "infolist.txt",
                  "default.cfg", "hexen.rc", "notes.txt"):
            (portals / f).write_bytes(f.encode())
        r11 = run(portals, work / "out11", "--loose")
        packed = []
        if (work / "out11" / "zz-loose.pk3").exists():
            with zipfile.ZipFile(work / "out11" / "zz-loose.pk3") as z:
                packed = sorted(z.namelist())
        check("infolist.txt" in packed and len(packed) == 6,
              "portals --loose packs infolist.txt and the other five: %s" % packed)
        check("not packed, keep it if the game needs it: notes.txt" in r11.stdout,
              "a top-level file left behind is named")
        r11b = run(portals, work / "out11b")
        check("infolist.txt" in r11b.stdout and "stay loose" in r11b.stdout,
              "without --loose, portals' infolist.txt is named among the files to keep")

        # Existing pk3s whose rank changes are named, before anything is written.
        inplace = work / "inplace" / "data1"
        inplace.mkdir(parents=True)
        make_pak(inplace / "pak0.pak", marks("data1 pak0"))
        make_pak(inplace / "pak1.pak", marks("data1 pak1"))
        (inplace / "PROGS.DAT").write_bytes(b"g")
        (inplace / "autoexec.cfg").write_bytes(b"bind x jump")
        (inplace / "textures").mkdir()
        for n in ("!fix.pk3", "000-hd.pk3", "hd.pk3", "zzz-progs.pk3"):
            (inplace / n).write_bytes(b"PK")
        r16 = run(inplace, inplace, "--loose")
        warned = [l for l in r16.stdout.splitlines() if l.startswith("warning:")]
        check(r16.returncode == 0
              and any("!fix.pk3 sorts below" in w for w in warned)
              and any("000-hd.pk3 sorts below" in w for w in warned)
              and any("zzz-progs.pk3 sorts after zz-loose.pk3" in w for w in warned)
              and not any("hd.pk3 sorts" in w and "000-hd" not in w for w in warned),
              "existing pk3s whose rank changes are named, and only those: %s" % warned)
        check("autoexec.cfg" in r16.stdout and "subdirectories are never packed: textures" in r16.stdout,
              "autoexec.cfg and subdirectories are named as left behind")
        r16b = run(inplace, inplace, "--loose")
        check("note: replacing the existing 000-pak0.pk3" in r16b.stdout,
              "a rerun says which outputs it replaces")

        # The engine's 64-pk3 limit: a run that would cross it is refused
        # before anything is written; one that lands exactly on it is fine.
        full = work / "full" / "data1"
        full.mkdir(parents=True)
        make_pak(full / "pak0.pak", marks("data1 pak0"))
        make_pak(full / "pak1.pak", marks("data1 pak1"))
        for i in range(63):
            (full / ("hd%02d.pk3" % i)).write_bytes(b"PK")
        r19 = run(full, full)
        check(r19.returncode != 0 and "over the engine's 64" in r19.stderr
              and not (full / "000-pak0.pk3").exists(),
              "a run that would put 65 pk3s in data1 is refused, nothing written")
        (full / "hd62.pk3").unlink()
        r19b = run(full, full)
        check(r19b.returncode == 0 and len(list(full.glob("*.pk3"))) == 64,
              "62 existing + 2 outputs = 64 is allowed")

        # Staging into another directory still compares against the
        # gamedir's own pk3s, and says so.
        stage_src = work / "stage" / "data1"
        stage_src.mkdir(parents=True)
        make_pak(stage_src / "pak0.pak", marks("data1 pak0"))
        make_pak(stage_src / "pak1.pak", marks("data1 pak1"))
        (stage_src / "000-hd.pk3").write_bytes(b"PK")
        r20 = run(stage_src, work / "staged")
        check(r20.returncode == 0 and "not the gamedir" in r20.stdout
              and "000-hd.pk3 sorts below" in r20.stdout,
              "a staged run warns about the gamedir's pk3s too")

        # Nothing to convert is an error, not "total 0.0 MB".
        empty = work / "emptymod"
        empty.mkdir()
        (empty / "game.exe").write_bytes(b"MZ")
        r21 = run(empty, work / "out21", "--force", "--loose")
        check(r21.returncode != 0 and "nothing to convert" in r21.stderr,
              "a run with nothing to convert fails")

        # --loose cannot sidestep the mod-directory rule, and the install
        # root gets pointed at its data1.
        r17 = run(work / "mymod", work / "out17", "--loose")
        check(r17.returncode != 0 and "mod's directory" in r17.stderr,
              "--loose on a mod's directory still needs --force")
        root = work / "install"
        (root / "data1").mkdir(parents=True)
        r18 = run(root, work / "out18", "--loose")
        check(r18.returncode != 0 and "install root" in r18.stderr
              and not (work / "out18").exists(),
              "the install root is refused, pointing at its data1")

        # An output path that is not valid UTF-8 still converts everything.
        odd = Path(os.fsdecode(bytes(work / "out14") + b"\xff"))
        r14 = run(game, odd)
        check(r14.returncode == 0 and sorted(p.name for p in odd.iterdir()) == ["000-pak0.pk3", "000-pak1.pk3"],
              "an output path that cannot print as UTF-8 still gets every archive: " + r14.stderr.strip()[:100])

        # Case-insensitive filesystems (Windows, macOS): PAK0.PAK is pak0.pak.
        upper = work / "upper" / "data1"
        upper.mkdir(parents=True)
        make_pak(upper / "PAK0.PAK", marks("data1 pak0"))
        make_pak(upper / "PAK1.PAK", marks("data1 pak1"))
        saved_ci = pak2pk3.CASE_INSENSITIVE_FS
        pak2pk3.CASE_INSENSITIVE_FS = True
        try:
            rc = pak2pk3.main([str(upper), str(work / "out15")])
        finally:
            pak2pk3.CASE_INSENSITIVE_FS = saved_ci
        check(rc == 0 and sorted(p.name for p in (work / "out15").iterdir())
              == ["000-pak0.pk3", "000-pak1.pk3"],
              "on a case-insensitive filesystem PAK0.PAK is converted, to a lowercase name")

        # Engine limits mirrored by read_pak.
        lim = work / "lim" / "data1"
        lim.mkdir(parents=True)
        make_pak(lim / "pak0.pak", [("f%d" % i, b"") for i in range(pak2pk3.MAX_FILES_IN_PACK + 1)])
        r12 = run(lim, work / "out12", "--force")
        check(r12.returncode != 0 and "never mounts" in r12.stderr,
              "a pak over MAX_FILES_IN_PACK is refused, as the engine refuses it")
        slash = work / "slash" / "data1"
        slash.mkdir(parents=True)
        make_pak(slash / "pak0.pak", marks("data1 pak0") + [("maps/", b"")])
        r13 = run(slash, work / "out13")
        check(r13.returncode != 0 and "not a file name" in r13.stderr,
              "a member name ending in '/' is refused")
        ragged = work / "ragged" / "data1"
        ragged.mkdir(parents=True)
        make_pak(ragged / "pak0.pak", marks("data1 pak0"))
        raw = bytearray((ragged / "pak0.pak").read_bytes())
        dirofs, dirlen = struct.unpack("<ii", raw[4:12])
        raw[8:12] = struct.pack("<i", dirlen - 10)    # the last entry is cut short
        (ragged / "pak0.pak").write_bytes(bytes(raw))
        members = pak2pk3.read_pak(str(ragged / "pak0.pak"))
        check(len(members) == len(marks("data1 pak0")) - 1,
              "a ragged directory is rounded down, as the engine does")

        # A failure mid-write leaves neither the output nor a temporary file.
        real_verify = pak2pk3.verify_pk3
        def broken(members, out):
            raise pak2pk3.PakError("simulated failure")
        pak2pk3.verify_pk3 = broken
        mid = work / "mid"
        mid.mkdir()
        try:
            pak2pk3.write_pk3([("a", b"x")], str(mid / "x.pk3"))
            raised = False
        except pak2pk3.PakError:
            raised = True
        finally:
            pak2pk3.verify_pk3 = real_verify
        check(raised and list(mid.iterdir()) == [],
              "a failure mid-write leaves nothing behind")

    # The size limit: over MAX_ZIP_INFLATE a member is STORED, at it DEFLATEd.
    # Checked through write_pk3 with the limit lowered, not with 64 MB of data.
    saved = pak2pk3.MAX_ZIP_INFLATE
    pak2pk3.MAX_ZIP_INFLATE = 1000
    try:
        with tempfile.TemporaryDirectory() as work:
            out = os.path.join(work, "x.pk3")
            pak2pk3.write_pk3([("at.bin", b"\0" * 1000), ("over.bin", b"\0" * 1001)], out)
            with zipfile.ZipFile(out) as z:
                methods = [i.compress_type for i in z.infolist()]
            check(methods == [zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED],
                  "a member over MAX_ZIP_INFLATE is STORED, one at it is DEFLATEd")
    finally:
        pak2pk3.MAX_ZIP_INFLATE = saved

    if errors:
        sys.exit("FAIL: %d check(s)" % len(errors))
    print("PASS")


if __name__ == "__main__":
    main()
