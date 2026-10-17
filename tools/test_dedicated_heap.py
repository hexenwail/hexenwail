#!/usr/bin/env python3
"""Dedicated servers start with the client's 512 MB heap (#79).

64 MB (h2ded) and 16 MB (hwsv) could not hold a large BSP2 map the GL client
loads with its 512 ("Hunk_AllocName: failed on 1656928 bytes for vertexes").
The hunk is malloc'd and only touched an allocation at a time, so the bigger
default costs address space, not memory (measured: h2ded VmRSS 44000 vs
44008 kB, hwsv 11152 vs 11156 kB).  Checked at source level:
  - every dedicated entry point (h2ded and hwsv, Unix and Windows, and the
    client's -dedicated path) defaults to 0x20000000 and still honours
    -heapsize, parsed as KB;
  - the usage text says KB, as the parse does;
  - running out names the way out, identically in zone.c and the Rust port
    that replaces it (the zone differential harness pins the text).
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
read = lambda p: (ROOT / p).read_text()

SERVERS = ["engine/hexen2/server/sys_unix.c", "engine/hexen2/server/sys_win.c",
           "engine/hexenworld/server/sys_unix.c", "engine/hexenworld/server/sys_win.c"]
CLIENTS = ["engine/hexen2/sys_unix.c", "engine/hexen2/sys_win.c"]

for path in SERVERS + CLIENTS:
    src = read(path)
    if not re.search(r"#define\s+STD_MEM_ALLOC\s+0x20000000\b", src):
        errors.append(f"{path}: STD_MEM_ALLOC must be 0x20000000 (512 MB)")
    if not re.search(r"\n\s*parms\.memsize = STD_MEM_ALLOC;", src):
        errors.append(f"{path}: the default heap must be STD_MEM_ALLOC, dedicated or not")
    if not re.search(r'COM_CheckParm \("-heapsize"\);\s*if \(i && i < com_argc-1\)\s*parms\.memsize = atoi \(com_argv\[i\+1\]\) \* 1024;', src):
        errors.append(f"{path}: -heapsize must still override the default, in KB")
    if "heapsize Bytes" in src:
        errors.append(f"{path}: the usage text says Bytes; -heapsize is parsed as KB")

hint = "The heap is %i KB; start with -heapsize <KB> for more (e.g. -heapsize %i)"
if hint not in read("engine/h2shared/zone.c"):
    errors.append("zone.c: Hunk_AllocName's failure must name -heapsize")
if hint not in read("engine/rust/src/zone.rs"):
    errors.append("zone.rs: the Rust Hunk_AllocName must say what zone.c says")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS dedicated servers default to 512 MB, -heapsize is KB and named on failure (#79)")
