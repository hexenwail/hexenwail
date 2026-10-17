#!/usr/bin/env python3
"""A mod's gfx.wad, conchars, backtile and loading disc load from the C heap (#126).

They were loaded into the 256 KB secondary zone at the file's own size, so a
mod shipping a bigger gfx.wad aborted the engine at startup ("Z_Malloc: failed
on allocation of 410765 bytes" for a retail gfx.wad with a 400 KB lump added;
it starts, and survives vid_restart's reload, now).  Checked at source level:
  - W_LoadWadFile, in wad.c and in its Rust port, loads with FS_LoadMallocFile
    and releases the previous wad with free -- never the zone;
  - draw.c (the software renderer) does the same for conchars, backtile and
    the loading disc;
  - the wad differential harness wraps free, so it still checks the release.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


wad = strip(read("engine/h2shared/wad.c"))
if "free (wad_base);" not in wad or "wad_base = FS_LoadMallocFile (filename, NULL);" not in wad:
    errors.append("wad.c: W_LoadWadFile must load with FS_LoadMallocFile and free the old wad")
rs = strip(read("engine/rust/src/wad.rs"))
if "free(wad_base.cast::<c_void>());" not in rs or "wad_base = FS_LoadMallocFile(filename, core::ptr::null_mut());" not in rs:
    errors.append("wad.rs: the Rust W_LoadWadFile must do what wad.c does")
for name, text in (("wad.c", wad), ("wad.rs", rs)):
    if "FS_LoadZoneFile" in text or "Z_Free" in text:
        errors.append(f"{name}: still touches the zone")
draw = strip(read("engine/h2shared/draw.c"))
for lump in ("conchars.lmp", "backtile.lmp"):
    if not re.search(r'FS_LoadMallocFile \("gfx/menu/' + re.escape(lump) + r'", NULL\)', draw):
        errors.append(f"draw.c: {lump} must load with FS_LoadMallocFile")
if "FS_LoadZoneFile" in draw:
    errors.append("draw.c: still loads into the zone")
if "-Wl,--wrap=free" not in read("engine/rust/wad/tests/run_diff_harness.sh"):
    errors.append("the wad differential harness must wrap free to keep checking the release")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS gfx.wad and the 2D lumps load from the C heap, not the secondary zone (#126)")
