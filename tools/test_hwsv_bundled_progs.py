#!/usr/bin/env python3
"""hwsv finds the hwprogs.dat shipped beside it (GitHub #37).

.#hwsv-bundled installed share/hexenwail/hw/hwprogs.dat, but hwsv's
PR_BundleDir was a stub returning NULL, so the bundled server still stopped
with "PR_LoadProgs: couldn't load hwprogs.dat" right after the data checks
passed -- which reads as the wrong pak4.

Held here:
  - PR_FindBundleDir / PR_BundleDir are compiled for H2W too (no NULL stub),
    and hwsv links sys_exedir.c for Sys_GetExeDir;
  - the bundle is a FALLBACK in hwsv: PR_LoadBundledHWProgs runs only after
    the normal gamedir lookup failed, so an installed copy always wins;
  - it looks in the bundle for the gamedir the server is in
    (fs_gamedir_nopath), never hw's bytecode under siege;
  - -vanillaprogs turns it off, and a bundled file failing the extents check
    is refused out loud and falls through to the unchanged error.

Measured with retail data1 + hw/pak4.pak: the bundled server spawns demo1
from the bundle (file crc 37917); with a hw/hwprogs.dat installed it loads
that one (crc 9155); -vanillaprogs, a bare .#hwsv and -game siege all stop
with the unchanged error; a truncated bundled file is reported as
"statements go past end of file" and refused.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
pr = (ROOT / "engine/h2shared/pr_edict.c").read_text()
cmake = (ROOT / "engine/CMakeLists.txt").read_text()
errors = []


def fenced_h2w_only(src, needle):
    """True if `needle` sits inside an `#if !defined(H2W)` block."""
    depth_stack = []
    pos = src.index(needle)
    for m in re.finditer(r"^#\s*(if|ifdef|ifndef|else|elif|endif)\b(.*)$", src[:pos], re.M):
        kind, rest = m.group(1), m.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            depth_stack.append(kind == "if" and "!defined(H2W)" in rest.replace(" ", "")
                               or kind == "ifndef" and rest.strip() == "H2W")
        elif kind == "endif":
            depth_stack.pop()
        elif kind == "else" and depth_stack:
            depth_stack[-1] = False
    return any(depth_stack)


for name in ("static const char *PR_FindBundleDir (void)\n{", "const char *PR_BundleDir (void)\n{"):
    if pr.count(name) != 1:
        errors.append(f"expected exactly one definition of {name.split('(')[0].strip()}")
    elif fenced_h2w_only(pr, name):
        errors.append(f"{name.split('(')[0].strip()} must be compiled for hwsv too")
if "return NULL;\n}\n#endif" in pr[pr.index("const char *PR_BundleDir (void)\n{"):][:80]:
    errors.append("PR_BundleDir must not be a NULL stub")

fn = re.search(r"static dprograms_t \*PR_LoadBundledHWProgs \(const char \*progname\)\n\{(.*?)\n\}", pr, re.S)
if not fn:
    errors.append("PR_LoadBundledHWProgs is missing")
else:
    body = fn.group(1)
    for need in ('COM_CheckParm("-vanillaprogs")', "PR_FindBundleDir()",
                 '"%s/%s/%s", bundle, fs_gamedir_nopath, progname',
                 "PR_CheckProgsExtents (p, fs_filesize)", 'Con_Printf ("Ignoring bundled %s: %s\\n", path, bad);',
                 "Hunk_FreeToLowMark (hunkmark);"):
        if need not in body:
            errors.append(f"PR_LoadBundledHWProgs lacks {need}")
    if '"hw"' in body or "/hw/" in body:
        errors.append("PR_LoadBundledHWProgs must not hard-code hw's directory")

order = re.search(r"\tif \(!progs\)\n\t\tprogs = \(dprograms_t \*\)FS_LoadHunkFile \(progname, NULL\);\n"
                  r"#if defined\(H2W\)\n\tif \(!progs\)\n\t\tprogs = PR_LoadBundledHWProgs \(progname\);\n#endif\n"
                  r"\tif \(!progs\)\n\t\tHost_Error", pr)
if not order:
    errors.append("hwsv must try the bundle only after the gamedir lookup failed, and before Host_Error")

hwsv = cmake[cmake.index("set(HWSV_SOURCES"):cmake.index("add_executable(hwsv")]
if "${COMMONDIR}/sys_exedir.c" not in hwsv:
    errors.append("hwsv must link sys_exedir.c (Sys_GetExeDir)")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS hwsv falls back to the bundled hwprogs.dat, gamedir copy first (#37)")
