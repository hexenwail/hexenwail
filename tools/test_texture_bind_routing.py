#!/usr/bin/env python3
"""Every texture-unit selection and bind goes through gl_texbind.c (GitHub #338).

gl_texbind.c keeps a shadow of what each texture unit holds, per target,
and of the active unit, and drops binds that would not change them.  That
is only correct while nothing binds behind its back: one raw glBindTexture
on unit 0 and the next R_BindTextureSlot (0, ...) can fold itself away
against a texture GL no longer has there.  One raw glActiveTexture and the
module's cursor disagrees with GL's, so its next slot-0 bind lands on the
wrong unit.  The module is also the one place a second backend (SDL_GPU,
#447) reads "which textures does this draw sample" from.

So, outside gl_texbind.c, none of these, whatever the file:
  * glActiveTexture -- the cursor is the module's.
  * glBindTexture / glBindTextures / glBindTextureUnit /
    glBindMultiTexture -- sampling binds AND bind-to-edit (before a
    glTexImage / glTexParameter / glTexStorage / glCopyTexSubImage);
    the second goes through R_BindTextureForEdit, which keeps the shadow
    coherent because it is the same tracked bind on unit 0.
  * glBindSampler(s) -- sampler objects are per-unit state the shadow
    would have to model; nothing uses them yet, and the first user should
    add the entry point rather than call GL raw.
  * glBindImageTexture(s) -- image units are not texture units, so they
    cannot stale the shadow, but they are how a compute pass is handed its
    storage textures (R_BindImageTexture), which is the same question a
    second backend asks.
  * glDeleteTextures -- deleting a bound texture unbinds it inside GL and
    frees the name for glGenTextures to hand straight back; the shadow
    would still believe the old binding, and the bind that should make
    the new texture current would fold away (R_DeleteTextures).

Every spelling counts: raw (glBindTexture), loader table
(glBindTexture_fp), extension-suffixed (glActiveTextureARB,
glBindTextureEXT_fp), and called through parentheses or a dereference
((glBindTexture_fp)(a, b), (*glBindTexture_fp)(a, b)).  So does taking the
function as a value -- a #define that aliases it, or "= glBindTexture_fp"
/ "&glBindTexture_fp" handing it to a pointer.  An availability test in a
#define ("glFoo_fp != NULL") is not an alias; outside a #define a bare
"!glBindImageTexture_fp" or "&& glBindImageTexture_fp" capability test is
not a call and not a value hand-off, so it is not flagged.  Token pasting
(gl##n##_fp) is not caught.

Not checked: glGenTextures (a fresh name is bound nowhere) and
glGetIntegerv (GL_TEXTURE_BINDING_*) reads.

Allowed outside gl_texbind.c:
  * ALLOWED: (file, GL function without gl/_fp, WHOLE argument list with
    whitespace collapsed) -> (exact count, why).  Same rules as
    tools/test_draw_state_routing.py: the same call in another file, a
    second copy, or other arguments all still fail, and an entry matching
    nothing fails too, so the list cannot outlive the code.  Empty since
    #338 converted the last site -- the mechanism stays so the first
    exception is spelled out and counted rather than slipped in.
  * engine/h2shared/gl_func.h, the loader table: it defines every _fp
    name (and the GLES no-op glBindImageTexture_fp), which is the
    contract, not a violation.
  * engine/h2shared/web_canvas_gl2.c, the software renderer's WebGL2
    presentation canvas: raw GLES, no loader table, a separate renderer
    path (same exemption as test_gl_call_sites.py).

Scope: engine/**/*.c, *.h and *.inc (cl_hw_*.inc are C #included into
cl_hw.c).  Comments and string/char literals are blanked first (line
numbers kept).  #if 0 blocks are NOT skipped: a dead raw call is still
flagged, so delete it rather than fence it.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

OWNER = "engine/h2shared/gl_texbind.c"
EXEMPT = {
    "engine/h2shared/gl_func.h",
    "engine/h2shared/web_canvas_gl2.c",
}
SUFFIXES = (".c", ".h", ".inc")

TEXTURE_BINDING = (
    "ActiveTexture",
    "BindTexture", "BindTextures", "BindTextureUnit", "BindMultiTexture",
    "BindSampler", "BindSamplers",
    "BindImageTexture", "BindImageTextures",
    "DeleteTextures",
)

# (file, function without gl/_fp, whole argument list with whitespace
# collapsed) -> (times it appears, why it may stay there).
ALLOWED = {
}

NAMES = "|".join(sorted(TEXTURE_BINDING, key=len, reverse=True))
NAME = r"\b(gl(" + NAMES + r")(?:ARB|EXT|OES|KHR|NV|AMD)?(?:_fp)?)\b"
# A call, also through "(name)(...)" or "(*name)(...)".  The argument list
# is captured when it holds no nested parentheses.
CALL_RE = re.compile(NAME + r"\s*\)?\s*\(([^()]*\))?")
# A bare name in a #define is an alias unless it is an availability test.
BARE_RE = re.compile(NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
# Outside a #define: the function handed to a pointer.  "&&" is a
# capability test (SS_Active: "... && glBindImageTexture_fp && ..."), not
# an address-of.
VALUE_RE = re.compile(
    r"(?:(?<![=!<>])=|(?<!&)&(?!&))\s*\(?\s*" + NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", re.MULTILINE)
BLANK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)


def blank(text):
    """Blank comments and literals, keeping newlines so line numbers hold."""
    return BLANK_RE.sub(
        lambda m: "".join("\n" if ch == "\n" else " " for ch in m.group(0)),
        text)


def scan(rel, text, used):
    errors = []

    def line(pos):
        return text.count("\n", 0, pos) + 1

    for m in CALL_RE.finditer(text):
        full, name, args = m.group(1), m.group(2), m.group(3)
        args = " ".join(args[:-1].split()) if args is not None else None
        key = (rel, name, args)
        if key in ALLOWED:
            used[key] = used.get(key, 0) + 1
            continue
        shown = args if args is not None else "..."
        errors.append(f"{rel}:{line(m.start())}: {full}({shown}) "
                      "-- texture binding outside gl_texbind.c")
    for d in DEFINE_RE.finditer(text):
        for m in BARE_RE.finditer(d.group(0)):
            errors.append(f"{rel}:{line(d.start() + m.start())}: #define aliases "
                          f"{m.group(1)} -- texture binding outside gl_texbind.c")
    for m in VALUE_RE.finditer(text):
        errors.append(f"{rel}:{line(m.start(1))}: {m.group(1)} taken as a value "
                      "-- texture binding outside gl_texbind.c")
    return errors


def main():
    errors, used, files = [], {}, 0
    paths = sorted(p for p in (ROOT / "engine").rglob("*") if p.suffix in SUFFIXES)
    for path in paths:
        rel = path.relative_to(ROOT).as_posix()
        if rel == OWNER or rel in EXEMPT:
            continue
        files += 1
        errors += scan(rel, blank(path.read_text(errors="replace")), used)

    for key, (want, _why) in sorted(ALLOWED.items()):
        got = used.get(key, 0)
        if got == 0:
            errors.append(f"{key[0]}: allowlist entry gl{key[1]}({key[2]}) matches "
                          "nothing -- delete it from ALLOWED")
        elif got != want:
            errors.append(f"{key[0]}: gl{key[1]}({key[2]}) appears {got} times, "
                          f"the allowlist expects {want}")

    if errors:
        sys.exit("FAIL:\n  " + "\n  ".join(errors))
    print(f"PASS no texture-binding GL call outside gl_texbind.c in {files} files, "
          f"{len(ALLOWED)} allowlisted (#338)")


if __name__ == "__main__":
    main()
