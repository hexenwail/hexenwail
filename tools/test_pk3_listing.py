#!/usr/bin/env python3
"""Name listings must see inside mounted .pk3 archives (GitHub #129).

FS_ScanFilesEx -- behind maplist, the map menu, randmap and the demo, cfg and
sky listings -- had a branch for paks and a fallback for loose directories,
and no branch for zips.  A pk3 searchpath has an empty filename, so it fell
into the loose-file scan, listed nothing from the archive, and globbed
"/maps" at the filesystem root.  A retail install repacked as pk3s printed
"No maps found."; the same install as paks found 42.
FS_ListSearchSubdirs had the same fall-through, and the path command printed
a pk3 as an empty line.

The shipped quakefs is the Rust port (engine/rust/src/quakefs.rs); the C
original (engine/h2shared/quakefs.c) is the reference the differential gate
compares it against.  Both are checked here.  A runtime test is not possible
in CI: any mounted pk3 marks the game modified, which only a registered
retail install may start, and no retail data ships here.

Fails if either implementation:
  - lists pak members but has no zip branch in the scan, or the zip branch
    does not walk the zip's own member table;
  - lets a zip searchpath reach FS_ListSearchSubdirs' directory listing;
  - prints a zip searchpath without naming the archive in the path command.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
C = ROOT / "engine/h2shared/quakefs.c"
RS = ROOT / "engine/rust/src/quakefs.rs"


def strip(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)


def body(text, signature):
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.index("{", start)
    depth, i = 1, brace + 1
    while depth:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[brace:i]


def main():
    # Optional paths check other copies, e.g. master's, to show the gate fails
    # without the fix.
    c_path = Path(sys.argv[1]) if len(sys.argv) > 2 else C
    rs_path = Path(sys.argv[2]) if len(sys.argv) > 2 else RS
    c, rs = strip(c_path.read_text()), strip(rs_path.read_text())
    errors = []

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            errors.append(what)

    scan_c = body(c, "static void FS_ScanFilesEx (")
    scan_rs = body(rs, "unsafe fn fs_scan_files_ex(")
    check(scan_c != "" and scan_rs != "", "found the scan in both implementations")
    check(re.search(r"else if \(search->zip\)\s*\{[^}]*search->zip->numfiles", scan_c, re.S)
          is not None and "search->zip->files[i].name" in scan_c,
          "C FS_ScanFilesEx walks a zip's members")
    check(re.search(r"else if !\(\*search\)\.zip\.is_null\(\)\s*\{.*?\(\*zip\)\.numfiles", scan_rs, re.S)
          is not None and "(*zip).files.add(" in scan_rs,
          "Rust fs_scan_files_ex walks a zip's members")
    # The zip branch has to come before the loose-directory fallback.
    for name, s, zip_pat in (("C", scan_c, "search->zip"), ("Rust", scan_rs, "(*search).zip")):
        z, f = s.find(zip_pat), s.find("Sys_FindFirstFile")
        check(0 <= z < f, "%s: the zip branch precedes the loose-file scan" % name)

    sub_c = body(c, "int FS_ListSearchSubdirs (")
    sub_rs = body(rs, "pub unsafe extern \"C\" fn FS_ListSearchSubdirs(")
    check("search->pack || search->zip" in sub_c, "C FS_ListSearchSubdirs skips zips")
    check("!(*search).pack.is_null() || !(*search).zip.is_null()" in sub_rs,
          "Rust FS_ListSearchSubdirs skips zips")

    path_c = body(c, "static void FS_Path_f (")
    path_rs = body(rs, "unsafe extern \"C\" fn fs_path_f(")
    check("s->zip->filename" in path_c, "C path command names a pk3")
    check("(*(*s).zip).filename" in path_rs, "Rust path command names a pk3")

    if errors:
        sys.exit("FAIL: %d check(s)" % len(errors))
    print("PASS")


if __name__ == "__main__":
    main()
