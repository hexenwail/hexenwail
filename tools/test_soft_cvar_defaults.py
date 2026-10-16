#!/usr/bin/env python3
"""The two web builds must agree on every cvar they both register (GitHub #104).

Both web configurations (WEB_RENDERER=webgl2 and software) can read and write
one config.cfg: same origin, same /persistent tree.  Host_WriteConfiguration
rebuilds that file from Cvar_WriteVariables (engine/rust/src/cvar.rs, the
port of engine/h2shared/cvar.c), which
writes every archived cvar the running build has registered, and nothing else.
Two consequences:

  * a cvar both builds register with a different default: whichever build
    writes config.cfg first decides the value for both.  Found writing
    docs/web/SOFTWARE_RENDERER.md: r_wateralpha "1" and r_dither "0" in
    r_soft_web.c, scr_*scale "1" and scr_conalpha "0.8" in draw_soft_web.c --
    a config first written by the software build carried those to the GL one.
  * a cvar one build registers as archived and the other does not register as
    archived: every session of the other build drops it from config.cfg, so
    the first build loses the player's value.  r_dynamic, r_clearcolor and
    showfps were archived only by the GL build.

This FAILS on the first kind and on a cvar registered by both whose archive
flag differs (except EXCEPTIONS, each with its reason), and REPORTS the second
kind for cvars only one build registers: that half is open as GitHub #362.

How it knows what each build registers: every client source under
engine/hexen2 and engine/h2shared is read through a small preprocessor model of
that build -- the defines engine/CMakeLists.txt gives it, with any condition on
a macro the model does not know keeping both branches -- and a cvar counts as
registered when its Cvar_RegisterVariable (&var) survives.  The file sets come
from CMakeLists.txt too: each renderer's source list plus the PLATFORM_SOURCES
its WEB_RENDERER branch appends, and every other .c file in engine/hexen2 and
engine/h2shared except NOT_WEB (files neither web build compiles).  One build
registering a name from two files that disagree fails as ambiguous.

  test_soft_cvar_defaults.py [--engine DIR] [-v]

--engine reads another engine tree (to show the test failing on an older one);
-v lists the cvars behind the reported counts.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SERVER_ONLY = {"sv_model.c"}
# Client-tree files neither web build compiles: the C originals of the Rust
# ports, other platforms' backends, codecs and MIDI drivers the web build turns
# off, the null backends, and the native-only HexenWorld client.  Taken from the
# compile lines of a WEB_RENDERER=software wasm build (2026-10-16), which differs
# from the webgl2 one only in renderer files.  A file missing from this list is
# harmless unless it registers a cvar name differently from a compiled file, and
# that fails below as ambiguous.
NOT_WEB = {
    "h2shared/" + f for f in (
        "cmd.c", "cvar.c", "hashindex.c", "link_ops.c", "mathlib.c", "msg_io.c",
        "quakefs.c", "sizebuf.c", "wad.c", "zone.c",
        "bgmnull.c", "bgmnull_midi.c", "bgmnull_none.c", "cd_bsd.c", "cd_linux.c",
        "cd_null.c", "in_svgalib.c", "mid2strm.c", "midi_fluid.c", "midi_win.c",
        "no_dxe.c", "snd_alsa.c", "snd_flac.c", "snd_null.c", "snd_opus.c",
        "snd_vorbis.c", "snd_xmp.c")
} | {
    "hexen2/" + f for f in (
        "cl_hw.c", "net_bsd.c", "net_loop.c", "net_udp.c", "net_win.c", "net_wins.c",
        "sys_win.c")
}
ADAPTERS = {"r_soft_web.c", "draw_soft_web.c", "web_canvas_gl2.c", "vid_soft_web.c"}

# What each web build defines (engine/CMakeLists.txt CLIENT_DEFINITIONS, plus
# USE_GLES forced on for webgl2 and __EMSCRIPTEN__ for both), and what it
# certainly does not.  A macro in neither set is unknown: both branches kept.
COMMON_UNDEF = {"H2W", "SERVERONLY", "GL_DLSYM", "_WIN32", "_WIN64", "WIN32", "_MSC_VER",
                "__APPLE__", "__MACOSX__", "PLATFORM_OSX", "PLATFORM_WINDOWS", "PLATFORM_DOS",
                "PLATFORM_AMIGA", "PLATFORM_OS2", "__DJGPP__", "__amigaos4__", "__MORPHOS__"}
BUILDS = {
    "webgl2": ({"__EMSCRIPTEN__", "GLQUAKE", "SDLQUAKE", "NO_SPLASHES", "USE_GLES"},
               COMMON_UNDEF | {"WEBSOFT", "WEBQUAKE"}),
    "software": ({"__EMSCRIPTEN__", "WEBQUAKE", "WEBSOFT", "SDLQUAKE", "NO_SPLASHES"},
                 COMMON_UNDEF | {"GLQUAKE", "USE_GLES"}),
}

# name -> why the two builds keep different defaults.
EXCEPTIONS = {
    "r_waterwarp": "one Water Warp row implemented twice; the restored r_main.c "
                   "defaults it on as shipped, the GL build off.  Whichever build "
                   "writes config.cfg first decides for both -- a policy question "
                   "for GitHub #362.",
}

DECL = re.compile(r'\bcvar_t\s+(\w+)\s*=\s*\{')
CVAR = re.compile(r'\bcvar_t\s+(\w+)\s*=\s*\{\s*"(\w+)"\s*,\s*"([^"]*)"\s*,\s*([^}]*)\}')
REG = re.compile(r'Cvar_RegisterVariable\s*\(\s*&\s*(\w+)\s*\)')


def strip_comments(text):
    """Blank // and /* */ comments, keeping string and char literals and every
    newline, so a commented-out definition or registration is not read as code
    (gl_screen.c's gl_triplebuffer is both)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def cond_value(expr, defined, undefined):
    """True/False for a #if expression over defined(), or None when unknown."""
    unknown = False

    def repl(m):
        nonlocal unknown
        name = m.group(1) or m.group(2)
        if name in defined:
            return " True "
        if name in undefined:
            return " False "
        unknown = True
        return " True "
    e = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)', repl, expr)
    e = e.replace("&&", " and ").replace("||", " or ")
    e = re.sub(r'!(?!=)', " not ", e)
    e = re.sub(r'/\*.*?\*/|//.*', "", e)
    if unknown or re.search(r'[A-Za-z_]\w*', re.sub(r'\b(True|False|and|or|not)\b', "", e)):
        return None
    try:
        return bool(eval(e, {"__builtins__": {}}))
    except Exception:
        return None


def active_text(text, defined, undefined):
    """The file with every branch this build excludes blanked out."""
    out, stack = [], []          # stack of (parent_active, this_taken, this_active)
    active = True
    for line in text.split("\n"):
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', line)
        if not m:
            out.append(line if active else "")
            continue
        kind, rest = m.group(1), m.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            if kind == "ifdef":
                v = cond_value("defined(%s)" % rest.split()[0], defined, undefined)
            elif kind == "ifndef":
                v = cond_value("!defined(%s)" % rest.split()[0], defined, undefined)
            else:
                v = cond_value(rest, defined, undefined)
            stack.append([active, v is not False, v])
            active = active and v is not False
        elif kind == "elif":
            parent, taken, prev = stack[-1]
            v = cond_value(rest, defined, undefined)
            # an earlier branch definitely taken excludes this one
            this = (prev is not True) and v is not False
            stack[-1] = [parent, taken or this, True if prev is True else v]
            active = parent and this
        elif kind == "else":
            parent, taken, prev = stack[-1]
            this = prev is not True
            stack[-1] = [parent, True, True]
            active = parent and this
        elif kind == "endif":
            if stack:
                active = stack.pop()[0]
        out.append("")
    return "\n".join(out)


def cmake_files(engine, block):
    files = set()
    for var, rel in re.findall(r"\$\{(ENGINE_TOP|COMMONDIR)\}/(\S+\.c)", block):
        files.add(engine / rel if var == "ENGINE_TOP" else engine / "h2shared" / rel)
    return files


def build_files(engine):
    cmake = (engine / "CMakeLists.txt").read_text()
    lists = {}
    for name in ("SOFT_RENDERER_SOURCES", "GL_RENDERER_SOURCES"):
        block = re.search(r"set\(%s(.*?)\n\)" % name, cmake, re.S)
        if not block:
            sys.exit("FAIL: %s not found in engine/CMakeLists.txt" % name)
        lists[name] = cmake_files(engine, block.group(1))
    plat = re.search(r'if\(WEB_RENDERER STREQUAL "software"\)\s*\n(\s*list\(APPEND PLATFORM_SOURCES[^)]*\))'
                     r'\s*else\(\)\s*\n(\s*list\(APPEND PLATFORM_SOURCES[^)]*\))', cmake)
    if not plat:
        sys.exit("FAIL: the WEB_RENDERER PLATFORM_SOURCES branches not found in engine/CMakeLists.txt")
    soft_r = lists["SOFT_RENDERER_SOURCES"] | cmake_files(engine, plat.group(1))
    gl_r = lists["GL_RENDERER_SOURCES"] | cmake_files(engine, plat.group(2))
    missing = ADAPTERS - {p.name for p in soft_r}
    if missing:
        sys.exit("FAIL: the software build's source lists no longer name %s" % ", ".join(sorted(missing)))
    every = {p for p in set((engine / "hexen2").glob("*.c")) | set((engine / "h2shared").glob("*.c"))
             if p.name not in SERVER_ONLY and str(p.relative_to(engine)) not in NOT_WEB}
    renderer_owned = (soft_r | gl_r) - (soft_r & gl_r)
    shared = every - renderer_owned
    return {"software": shared | soft_r, "webgl2": shared | gl_r}, soft_r - gl_r


def registered(engine, files, build, strict, errors):
    """name -> [(default, archived, file)] for every cvar this build registers.

    Definitions and registrations are joined across the whole build by
    variable name, because a cvar is often defined in one file and registered
    from another (gl_rmain.c's, from gl_rmisc.c's R_Init).  A static of the same
    name in two files is resolved to the definition in the registering file."""
    defined, undefined = BUILDS[build]
    defs, regs_at = {}, []
    for path in sorted(files):
        if not path.exists():
            continue
        text = active_text(strip_comments(path.read_text(errors="replace")), defined, undefined)
        decls = {m.group(1) for m in DECL.finditer(text)}
        found = {}
        for var, name, default, flags in CVAR.findall(text):
            d = (name, default, "CVAR_ARCHIVE" in flags)
            # Two surviving definitions of one variable in one file (both arms
            # of an #if the model cannot decide): disagreeing ones are ambiguous.
            if var in found and found[var] != d:
                errors.append("%s: cvar_t %s is defined twice with different contents in the %s "
                              "build model (an #if it cannot decide?); cannot tell which is compiled"
                              % (path.relative_to(engine.parent), var, build))
            found[var] = d
        if path in strict and decls - set(found):
            errors.append("%s: cvar_t %s has an initializer this test cannot read; spell its name "
                          "and default out literally" % (path.relative_to(engine.parent),
                                                         ", ".join(sorted(decls - set(found)))))
        for var, d in found.items():
            defs.setdefault(var, {})[path] = d
        regs_at.extend((var, path) for var in REG.findall(text))
    regs = {}
    for var, path in regs_at:
        where = defs.get(var)
        if not where:
            continue
        src = path if path in where else sorted(where)[0]
        name, default, archive = where[src]
        regs.setdefault(name, []).append((default, archive, src.relative_to(engine.parent)))
    return regs


def same(a, b):
    try:
        return float(a) == float(b)
    except ValueError:
        return a == b


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--engine", type=Path, default=ROOT / "engine")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    engine = args.engine.resolve()

    files, soft_only = build_files(engine)
    errors = []
    regs = {b: registered(engine, files[b], b, soft_only, errors) for b in BUILDS}
    soft, gl = regs["software"], regs["webgl2"]

    # One build registering one name from several files that disagree: the model
    # cannot tell which the build compiles, so it cannot compare honestly.
    for build, regs_b in regs.items():
        for name, rs in sorted(regs_b.items()):
            if len({(d, a) for d, a, _ in rs}) > 1:
                errors.append("%s: the %s build model registers it from %s with different "
                              "definitions; add the uncompiled file to NOT_WEB, or fix the "
                              "conflict" % (name, build, ", ".join(str(w) for _, _, w in rs)))

    excused, compared = [], 0
    for name in sorted(set(soft) & set(gl)):
        compared += 1
        (sd, sa, sw), (gd, ga, gw) = soft[name][0], gl[name][0]
        problems = []
        if not same(sd, gd):
            problems.append('default "%s", GL "%s"' % (sd, gd))
        if sa != ga:
            problems.append("%sarchived, GL %sarchived" % ("" if sa else "not ", "" if ga else "not "))
        if problems:
            line = "%s (%s vs %s): %s" % (name, sw, gw, "; ".join(problems))
            (excused if name in EXCEPTIONS else errors).append(line)

    gl_only = sorted(n for n in set(gl) - set(soft) if gl[n][0][1])
    soft_only_names = sorted(n for n in set(soft) - set(gl) if soft[n][0][1])

    print("compared %d cvars both web builds register" % compared)
    for line in excused:
        print("excepted: " + line)
    print("open (#362), not failed: %d archived cvars only the webgl2 build registers; "
          "a software session drops them from config.cfg" % len(gl_only))
    if args.v:
        print("  " + " ".join(gl_only))
    print("open (#362), not failed: %d archived cvars only the software build registers; "
          "a webgl2 session drops them" % len(soft_only_names))
    if args.v:
        print("  " + " ".join(soft_only_names))
    stale = sorted(set(EXCEPTIONS) - {l.split(" ")[0] for l in excused})
    if stale:
        errors.append("EXCEPTIONS lists %s, which no longer differ: drop them" % ", ".join(stale))
    if compared < 100:
        errors.append("compared only %d cvars; the model is broken" % compared)
    if errors:
        print("FAIL:")
        for line in errors:
            print("  " + line)
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
