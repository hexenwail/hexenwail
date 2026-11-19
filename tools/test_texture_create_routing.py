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
a value -- a #define that aliases it, or "= glTexParameteri_fp" /
"&glTexParameteri_fp" handing it to a pointer.  An availability test
("glTexStorage2D_fp != NULL", "!glTexImage3D_fp", "&& glTexStorage2D_fp")
is not a call and not a value hand-off.  Token pasting (gl##n##_fp) is not
caught.

Allowed outside gl_texcreate.c:
  * ALLOWED: (file, GL function without gl/_fp/suffix) -> (exact count,
    why).  P1a routes the call sites one family at a time, and this is the
    list of what is left; it only shrinks.  A count that no longer matches
    fails -- a new raw call in an allowlisted file is not absorbed by the
    entry -- and so does an entry matching nothing, so the list cannot
    outlive the code.  Coarser than test_texture_bind_routing.py's
    (file, function, arguments) key on purpose: it is a migration ledger,
    and once a family is routed its rows go, not its argument lists.
  * engine/h2shared/gl_func.h, the loader table: it declares every _fp
    name (and the GLES no-op macros), which is the contract.
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
    + tuple(stem + v for stem in ("TexParameter", "TextureParameter", "SamplerParameter")
            for v in ("f", "i", "fv", "iv", "Iiv", "Iuiv"))
)
PIXEL_STORE = ("PixelStorei", "PixelStoref")

# (file, function without gl/_fp/suffix) -> (times it appears, why it may
# stay there).  The families P1a has not routed yet; shrinks to empty.
ALLOWED = {
    ("engine/h2shared/gl_lightcluster.c", "GenTextures"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_lightcluster.c", "TexImage3D"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_lightcluster.c", "TexParameterf"): (5, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_mesh.c", "GenTextures"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_mesh.c", "TexImage2D"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_mesh.c", "TexParameterf"): (4, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_postprocess.c", "CopyTexSubImage2D"): (2, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_postprocess.c", "GenTextures"): (12, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_postprocess.c", "TexImage2D"): (11, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_postprocess.c", "TexImage2DMultisample"): (3, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_postprocess.c", "TexImage3D"): (2, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_postprocess.c", "TexParameterf"): (51, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_shader.c", "GenTextures"): (4, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_shader.c", "TexImage2D"): (4, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_shader.c", "TexParameterf"): (16, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_shadow.c", "GenTextures"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_shadow.c", "TexImage3D"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_shadow.c", "TexParameteri"): (11, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_sky.c", "GenTextures"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_sky.c", "TexImage2D"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_sky.c", "TexParameterf"): (4, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_ssao.c", "GenTextures"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_ssao.c", "TexParameteri"): (5, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_ssao.c", "TexStorage2D"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_ssao.c", "TexSubImage2D"): (2, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_vidsdl.c", "GenTextures"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_vidsdl.c", "TexImage2D"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_vidsdl.c", "TexParameterf"): (6, "not routed yet: the rest, P1a step 4"),
    ("engine/h2shared/gl_worldcull.c", "GenTextures"): (2, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_worldcull.c", "TexImage2D"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_worldcull.c", "TexParameterf"): (8, "not routed yet: render targets, P1a step 3"),
    ("engine/h2shared/gl_worldcull.c", "TexStorage2D"): (1, "not routed yet: render targets, P1a step 3"),
    ("engine/hexen2/gl_rmain.c", "TexParameterf"): (4, "not routed yet: the rest, P1a step 4"),
    ("engine/hexen2/gl_rmisc.c", "GenTextures"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/hexen2/gl_rmisc.c", "TexImage2D"): (1, "not routed yet: the rest, P1a step 4"),
    ("engine/hexen2/gl_rmisc.c", "TexParameterf"): (2, "not routed yet: the rest, P1a step 4"),
    ("engine/hexen2/gl_rsurf.c", "GenTextures"): (4, "not routed yet: lightmaps, P1a step 2"),
    ("engine/hexen2/gl_rsurf.c", "PixelStorei"): (4, "not routed yet: lightmaps, P1a step 2"),
    ("engine/hexen2/gl_rsurf.c", "TexImage2D"): (3, "not routed yet: lightmaps, P1a step 2"),
    ("engine/hexen2/gl_rsurf.c", "TexParameterf"): (10, "not routed yet: lightmaps, P1a step 2"),
    ("engine/hexen2/gl_rsurf.c", "TexSubImage2D"): (3, "not routed yet: lightmaps, P1a step 2"),
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
VALUE_RE = re.compile(
    r"(?:(?<![=!<>])=|(?<!&)&(?!&))\s*\(?\s*" + NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", re.MULTILINE)
BLANK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)
PACK_RE = re.compile(r"\s*GL_PACK_\w+\s*$")


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
        if name in PIXEL_STORE and args is not None and PACK_RE.match(args.split(",")[0]):
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
    for m in VALUE_RE.finditer(text):
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
