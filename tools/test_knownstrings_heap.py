#!/usr/bin/env python3
"""The progs known-string tables live on the C heap, never the zone (#488).

PR_AllocStringSlots grew pr_knownstrings with Z_Realloc, which frees the old
block before it allocates the new one.  When a free block sat right before the
table the two merged, Z_TagMalloc handed back the merged block at the earlier
address, and wrote its split header and ZMAGIC trash marker into old slots that
the memmove had not copied yet -- slots 132-136 of a 256-slot table came back
as garbage pointers.  QuakeSpasm e1c94a0 fixed the same code with libc realloc.
Checked at source level:
  - no statement anywhere in the engine hands a known-string table to
    Z_Malloc, Z_Realloc or Z_Free (Z_Free on a malloc pointer is fatal, and a
    zone pointer passed to free is heap corruption);
  - PR_AllocStringSlots grows with realloc, fails loudly on NULL, and zeroes the
    PR_STRING_ALLOCSLOTS new slots (Z_Realloc zero-filled; PR_AllocString and
    PR_SetEngineString scan for NULL slots, so stale heap bytes there would be
    handed out as live strings);
  - both releases -- PR_LoadProgs' and CL_ShutdownCSProgs' for the CSQC table
    it swaps in through PR_RestoreVMState -- are free;
  - Z_Realloc stays removed from the C zone and its Rust port, so nothing can
    pick the free-then-allocate realloc back up.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def function_body(text, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


sources = sorted(p for d in ("engine/h2shared", "engine/hexen2", "engine/hexenworld")
                 for p in (ROOT / d).rglob("*.[ch]") if p.is_file())
zone_call = re.compile(r"\bZ_(Malloc|Realloc|Free)\s*\(")
for path in sources:
    text = strip(path.read_text(errors="replace"))
    if "knownstrings" not in text:
        continue
    for stmt in re.split(r"[;{}]", text):
        if "knownstrings" in stmt and zone_call.search(stmt):
            errors.append(f"{path.relative_to(ROOT)}: a known-string table goes through the zone: "
                          + " ".join(stmt.split()))
    # every realloc of a known-string table zeroes what it added
    for m in re.finditer(r"\b(\w*knownstrings)\s*=\s*\([^)]*\)\s*realloc\s*\(", text):
        start = text.rfind("\n{", 0, m.start())
        end = text.find("\n}", m.end())
        body = text[start:end]
        if not re.search(r"\bmemset\s*\(\s*\(void\s*\*\)\s*\(\s*" + re.escape(m.group(1)) + r"\s*\+", body):
            errors.append(f"{path.relative_to(ROOT)}: a realloc of {m.group(1)} does not memset the new slots")

edict = strip((ROOT / "engine/h2shared/pr_edict.c").read_text())
grow = function_body(edict, "PR_AllocStringSlots")
if grow is None:
    errors.append("pr_edict.c: PR_AllocStringSlots not found")
else:
    g = " ".join(grow.split())
    old = re.search(r"const int (\w+) = pr_maxknownstrings;", g)
    if not re.search(r"pr_knownstrings = \(const char \*\*\) realloc \(\(void \*\)pr_knownstrings, "
                     r"pr_maxknownstrings \* sizeof\(char \*\)\);", g):
        errors.append("pr_edict.c: PR_AllocStringSlots must grow pr_knownstrings with realloc")
    if not re.search(r"if \(!pr_knownstrings\) Sys_Error \(", g):
        errors.append("pr_edict.c: PR_AllocStringSlots must Sys_Error when realloc returns NULL")
    if not old or not re.search(r"memset \(\(void \*\)\(pr_knownstrings \+ " + old.group(1)
                                + r"\), 0, PR_STRING_ALLOCSLOTS \* sizeof\(char \*\)\);", g):
        errors.append("pr_edict.c: PR_AllocStringSlots must zero the PR_STRING_ALLOCSLOTS new slots, "
                      "from the old maximum")
    elif g.find(old.group(0)) > g.find("pr_maxknownstrings +="):
        errors.append("pr_edict.c: the old maximum must be read before pr_maxknownstrings grows")
if "free ((void *)pr_knownstrings);" not in edict:
    errors.append("pr_edict.c: PR_LoadProgs must release the old table with free")
for m in re.finditer(r"\bpr_knownstrings\s*=\s*([^;]+);", edict):
    rhs = " ".join(m.group(1).split())
    if rhs not in ("NULL", "state->knownstrings") and "realloc (" not in rhs:
        errors.append(f"pr_edict.c: pr_knownstrings assigned from something other than realloc: {rhs}")

csqc = strip((ROOT / "engine/hexen2/cl_csqc.c").read_text())
shutdown = function_body(csqc, "CL_ShutdownCSProgs") or ""
if "free ((void *)csqc_state.knownstrings);" not in shutdown:
    errors.append("cl_csqc.c: CL_ShutdownCSProgs must release the CSQC table with free")

for rel in ("engine/h2shared/zone.h", "engine/h2shared/zone.c", "engine/rust/src/zone.rs",
            "engine/rust/zone/tests/diff_harness.c", "engine/rust/zone/tests/run_diff_harness.sh",
            "scripts/check-rust-zone.sh"):
    if re.search(r"\bZ_Realloc\b", strip((ROOT / rel).read_text())):
        errors.append(f"{rel}: Z_Realloc is back")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS the progs known-string tables grow with realloc, zero their new slots and "
      "never touch the zone (#488)")
