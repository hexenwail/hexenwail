#!/usr/bin/env python3
"""Every texture creation, upload and sampler-state call goes through
gl_texcreate.c (GitHub #340, phase P1a).

GL edits a texture through whatever is bound to the active unit, and the
call names no texture.  The SDL_GPU backend (#340) has no such binding: it
creates a texture from a description, fills it through a copy pass and
samples it through a separate sampler object.  gl_texcreate.h is that
second shape implemented on the first -- every entry point names the
texture it acts on -- and it is the one place the SDL_GPU arm will branch.
A raw call anywhere else is a texture that backend never hears about.

So, outside gl_texcreate.c, none of these, whatever the file:
  * names: glGenTextures, glCreateTextures, glGenSamplers,
    glCreateSamplers, glDeleteSamplers.
  * storage and contents: glTexImage1D/2D/3D, glTexImage2D/3DMultisample,
    glTexStorage*, glCompressedTexImage*, glTexBuffer(Range),
    glTextureView, and the DSA glTexture* spellings of all of them.
  * uploads: glTexSubImage*, glCompressedTexSubImage*, glCopyTexImage*,
    glCopyTexSubImage*, glCopyImageSubData, glClearTex(Sub)Image,
    glGenerateMipmap, glGenerateTextureMipmap.
  * sampling state: glTexParameter{f,i,fv,iv,Iiv,Iuiv},
    glTextureParameter*, glSamplerParameter*.
  * the EXT_direct_state_access and EXT_memory_object spellings of the
    above: glTextureImage*EXT, glMultiTex{Image,SubImage,Parameter}*EXT,
    glCopyMultiTex*EXT, glMultiTexGenerateMipmapEXT, glTex/TextureStorageMem*.
  * glPixelStorei / glPixelStoref, unless the first argument is a
    GL_PACK_* name: unpack state belongs to uploads (R_TexSubImage2DRows
    owns GL_UNPACK_ROW_LENGTH), pack state to readbacks, which are a later
    phase's seam.  A first argument that is not a literal GL_PACK_* name
    counts as unpack.

glDeleteTextures is not here: it goes through gl_texbind.c's
R_DeleteTextures, which has to update the binding shadow, and
tools/test_texture_bind_routing.py owns it.  The glGetTex* reads are
readbacks and are not checked here either.

Every spelling counts: raw (glTexImage2D), loader table (glTexImage2D_fp),
extension-suffixed (glTexStorage2DEXT), called through parentheses or a
dereference ((glTexImage2D_fp)(...), (*glTexImage2D_fp)(...)), and taken as
a value -- a #define that aliases it, "= glTexParameteri_fp" /
"&glTexParameteri_fp" handing it to a pointer, or the name passed as an
argument (install (glTexImage2D_fp, 3)), returned, put in an initializer
list or a ternary arm, cast or not.  An availability test ("glTexStorage2D_fp != NULL",
"!glTexImage3D_fp", "&& glTexStorage2D_fp", "if (glTexImage3D_fp)") is not
a call and not a value hand-off.  Not caught: token pasting (gl##n##_fp),
and a prefixed wrapper name (qglTexImage2D) -- the engine has neither.

Allowed outside gl_texcreate.c:
  * ALLOWED: (file, GL function without gl/_fp/suffix) -> (exact count,
    why).  P1a routed the call sites one family at a time and this was the
    ledger of what was left; empty since the last family went, and the
    mechanism stays so the first exception is spelled out and counted
    rather than slipped in.  A count that no longer matches fails -- a new
    raw call in an allowlisted file is not absorbed by the entry -- and so
    does an entry matching nothing, so the list cannot outlive the code.
    Keyed coarser than test_texture_bind_routing.py's (file, function,
    arguments) because it was a migration ledger: a routed family took its
    rows with it, not its argument lists.
  * engine/h2shared/gl_func.h, the loader table: it declares every _fp
    name (and the GLES no-op macros), which is the contract.
  * engine/h2shared/rg_null.c, the null-GL table SDL_GPU mode installs,
    generated from gl_func.h by tools/gen_rg_null.py: it names every entry
    point to stub and verify it (RGN_VERIFY (glTexImage2D)), which is the
    table, not a caller.
  * engine/h2shared/web_canvas_gl2.c, the software renderer's WebGL2
    presentation canvas: raw GLES, no loader table, a separate renderer
    path (same exemption as test_gl_call_sites.py).

Scope: engine/**/*.c, *.h and *.inc.  Comments and string/char literals
are blanked first (line numbers kept).  #if 0 blocks are NOT skipped: a
dead raw call is still flagged, so delete it rather than fence it.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

OWNER = "engine/h2shared/gl_texcreate.c"
EXEMPT = {
    "engine/h2shared/gl_func.h",
    "engine/h2shared/web_canvas_gl2.c",
    "engine/h2shared/rg_null.c",
}
SUFFIXES = (".c", ".h", ".inc")


def _dims(stem, dims=("1D", "2D", "3D")):
    return tuple(stem + d for d in dims)


TEXTURE_CREATION = (
    ("GenTextures", "CreateTextures",
     "GenSamplers", "CreateSamplers", "DeleteSamplers",
     "TexImage2DMultisample", "TexImage3DMultisample",
     "TexStorage2DMultisample", "TexStorage3DMultisample",
     "TextureStorage2DMultisample", "TextureStorage3DMultisample",
     "TexBuffer", "TexBufferRange", "TextureBuffer", "TextureBufferRange",
     "TextureView", "CopyImageSubData", "ClearTexImage", "ClearTexSubImage",
     "GenerateMipmap", "GenerateTextureMipmap")
    + _dims("TexImage") + _dims("TexSubImage") + _dims("TexStorage")
    + _dims("TextureStorage") + _dims("TextureSubImage")
    + _dims("CompressedTexImage") + _dims("CompressedTexSubImage")
    + _dims("CompressedTextureSubImage")
    + _dims("CopyTexImage", ("1D", "2D")) + _dims("CopyTexSubImage")
    + _dims("CopyTextureSubImage")
    + tuple(stem + v for stem in ("TexParameter", "TextureParameter", "SamplerParameter",
                                  "MultiTexParameter")
            for v in ("f", "i", "fv", "iv", "Iiv", "Iuiv"))
    # EXT_direct_state_access and EXT_memory_object.
    + _dims("TextureImage") + _dims("MultiTexImage") + _dims("MultiTexSubImage")
    + _dims("CopyTextureImage", ("1D", "2D")) + _dims("CopyMultiTexImage", ("1D", "2D"))
    + _dims("CopyMultiTexSubImage") + _dims("CompressedTextureImage")
    + _dims("CompressedMultiTexImage") + _dims("CompressedMultiTexSubImage")
    + ("MultiTexGenerateMipmap", "MultiTexBuffer")
    + _dims("TexStorageMem") + _dims("TextureStorageMem")
    + ("TexStorageMem2DMultisample", "TexStorageMem3DMultisample",
       "TextureStorageMem2DMultisample", "TextureStorageMem3DMultisample")
)
PIXEL_STORE = ("PixelStorei", "PixelStoref")

# (file, function without gl/_fp/suffix) -> (times it appears, why it may
# stay there).  Empty since P1a routed the last family.
ALLOWED = {
}

NAMES = "|".join(sorted(TEXTURE_CREATION + PIXEL_STORE, key=len, reverse=True))
NAME = r"\b(gl(" + NAMES + r")(?:ARB|EXT|OES|KHR|NV|AMD)?(?:_fp)?)\b"
# A call, also through "(name)(...)" or "(*name)(...)".  The argument list
# is captured when it holds no nested parentheses.
CALL_RE = re.compile(NAME + r"\s*\)?\s*\(([^()]*\))?")
# A bare name in a #define is an alias unless it is an availability test.
BARE_RE = re.compile(NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
# Outside a #define: the function handed to a pointer.  "&&" is a
# capability test, not an address-of.
# An optional cast in front of a handed-on name: "(void *)glTexImage2D_fp".
_CAST = r"(?:\(\s*[A-Za-z_][\w\s]*\**\s*\)\s*)?"
VALUE_RE = re.compile(
    r"(?:(?<![=!<>])=|(?<!&)&(?!&))\s*" + _CAST + r"\(?\s*" + NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
# The name handed on rather than called: an argument, a return value, an
# initializer-list entry, a ternary arm.  After "(" only when the "(" is a
# call's (an identifier that is not a keyword precedes it), so
# "if (glTexImage3D_fp)" stays an availability test.
_NOT_CALLED = r"(?!\s*\)?\s*\()(?!\s*(?:[!=]=|&&|\|\|))"
HANDOFF_RE = re.compile(r"(?:[,?:{]|\breturn\b)\s*" + _CAST + NAME + _NOT_CALLED)
ARG_RE = re.compile(r"\b(?!(?:if|while|for|switch|return|sizeof)\b)[A-Za-z_]\w*\s*\(\s*"
                    + _CAST + NAME + _NOT_CALLED)
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", re.MULTILINE)
BLANK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)
# glPixelStore's first argument is a literal GL_PACK_* name, whatever the
# rest of the argument list looks like.
PACK_RE = re.compile(r"\s*\)?\s*\(\s*GL_PACK_\w+\s*,")


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
        if name in PIXEL_STORE and PACK_RE.match(text, m.end(1)):
            continue	# pack state: a readback's, not an upload's
        key = (rel, name)
        if key in ALLOWED:
            used[key] = used.get(key, 0) + 1
            continue
        shown = args if args is not None else "..."
        errors.append(f"{rel}:{line(m.start())}: {full}({shown}) "
                      "-- texture creation outside gl_texcreate.c")
    for d in DEFINE_RE.finditer(text):
        for m in BARE_RE.finditer(d.group(0)):
            errors.append(f"{rel}:{line(d.start() + m.start())}: #define aliases "
                          f"{m.group(1)} -- texture creation outside gl_texcreate.c")
    seen = set()
    for rx in (VALUE_RE, HANDOFF_RE, ARG_RE):
        for m in rx.finditer(text):
            if m.start(1) in seen:
                continue
            seen.add(m.start(1))
            errors.append(f"{rel}:{line(m.start(1))}: {m.group(1)} taken as a value "
                          "-- texture creation outside gl_texcreate.c")
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
            errors.append(f"{key[0]}: allowlist entry gl{key[1]} matches nothing "
                          "-- delete it from ALLOWED")
        elif got != want:
            errors.append(f"{key[0]}: gl{key[1]} appears {got} times, "
                          f"the allowlist expects {want}")

    if errors:
        sys.exit("FAIL:\n  " + "\n  ".join(errors))
    left = sum(want for want, _why in ALLOWED.values())
    print(f"PASS no texture-creation GL call outside gl_texcreate.c in {files} files, "
          f"{left} allowlisted in {len(ALLOWED)} entries (#340 P1a)")


if __name__ == "__main__":
    main()
