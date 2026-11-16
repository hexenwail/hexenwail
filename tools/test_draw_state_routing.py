#!/usr/bin/env python3
"""Every draw-state change goes through gl_pipeline.c (GitHub #337).

gl_pipeline.c keeps a shadow of the GL draw state and drops calls that
would not change it.  That is only correct while nothing writes the state
behind its back: one raw glDepthMask in another file and the next
R_SetDepthMask can fold itself away against a value GL no longer has.
It is also the one place a second backend (SDL_GPU, #447) reads the
whole draw state from.

tools/test_gl_call_sites.py (#453) only checks that GL calls sit in the
renderer's gl_* files.  This checks the stronger thing: no draw-state
call (DRAW_STATE below, which includes glUseProgram -- R_UseProgram
shadows the bound program too) outside gl_pipeline.c, whatever the file.
Every spelling counts: raw (glEnable), loader table (glEnable_fp),
extension-suffixed (glMinSampleShadingARB, glFooARB_fp), and called
through parentheses or a dereference ((glEnable_fp)(x), (*glEnable_fp)(x)).
So does taking the function as a value -- a #define that aliases it, or
"= glEnable_fp" / "&glEnable_fp" handing it to a pointer -- which is how
a call would hide behind a macro or a wrapper.  An availability test in
a #define ("glFoo_fp != NULL") is not an alias; other spellings of one
("!glFoo_fp", "NULL != glFoo_fp") are flagged, which fails loudly and
can be rewritten.  Token pasting (gl##n##_fp) is not caught.

Not draw state, so not checked: glViewport and the glClear* values (the
viewport is dynamic command-buffer state in SDL_GPU; clears read the
shadowed masks and scissor but write none of them), glDrawBuffer(s)
(framebuffer state), glHint, glPixelStore, and
glEnable/DisableVertexAttribArray (vertex array object state).
Deleting a program without R_PipelineForgetProgram also stales the
shadow; that is a different rule and not checked here.

Allowed outside gl_pipeline.c:
  * CONTEXT_CAPS: switches set at context or allocation time, never per
    draw.  Matched by file, GL function and the WHOLE argument list, and
    each must appear exactly as many times as its entry says -- so the
    same call in another file, a second copy in its own file, another
    capability, or "GL_DEBUG_OUTPUT | GL_BLEND" all still fail.  An entry
    matching nothing fails too, so the list cannot outlive the code.
  * engine/h2shared/gl_func.h, the loader table: it defines every _fp
    name, which is the contract, not a violation.
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

OWNER = "engine/h2shared/gl_pipeline.c"
EXEMPT = {
    "engine/h2shared/gl_func.h",
    "engine/h2shared/web_canvas_gl2.c",
}
SUFFIXES = (".c", ".h", ".inc")

DRAW_STATE = (
    "Enable", "Disable", "Enablei", "Disablei",
    "BlendFunc", "BlendFunci", "BlendFuncSeparate", "BlendFuncSeparatei",
    "BlendEquation", "BlendEquationi",
    "BlendEquationSeparate", "BlendEquationSeparatei", "BlendColor",
    "DepthMask", "DepthFunc", "DepthRange", "DepthRangef",
    "DepthRangeArrayv", "DepthRangeIndexed",
    "CullFace", "FrontFace", "PolygonMode", "PolygonOffset",
    "PolygonOffsetClamp", "LineWidth", "PointSize", "ClipControl",
    "ColorMask", "ColorMaski", "LogicOp",
    "StencilFunc", "StencilFuncSeparate", "StencilOp", "StencilOpSeparate",
    "StencilMask", "StencilMaskSeparate",
    "Scissor", "ScissorIndexed", "ScissorArrayv",
    "SampleCoverage", "SampleMaski", "MinSampleShading",
    "ProvokingVertex", "PrimitiveRestartIndex",
    "UseProgram",
)

# (file, function without gl/_fp, whole argument list with whitespace
# collapsed) -> (times it appears, why it may stay there).  The function is
# part of the key: these are only ever switched on, so a glDisable of one is
# a per-draw toggle and fails like any other.
CONTEXT_CAPS = {
    ("engine/h2shared/gl_vidsdl.c", "Enable", "GL_DEBUG_OUTPUT"):
        (1, "GL_Init: KHR_debug output for the context's lifetime"),
    ("engine/h2shared/gl_vidsdl.c", "Enable", "GL_DEBUG_OUTPUT_SYNCHRONOUS"):
        (1, "GL_Init: debug callback fires before the bad call returns"),
    ("engine/h2shared/gl_vidsdl.c", "Enable", "GL_MULTISAMPLE_ARB"):
        (1, "GL_Init: multisampled default framebuffer, never toggled per draw"),
    ("engine/h2shared/gl_vidsdl.c", "ClipControl", "GL_LOWER_LEFT, GL_ZERO_TO_ONE"):
        (1, "GL_Init: reversed-Z clip convention, fixed for the context"),
    ("engine/h2shared/gl_shadow.c", "Enable", "GL_TEXTURE_CUBE_MAP_SEAMLESS"):
        (1, "shadow map (re)allocation: context-wide cube sampling, only improves it"),
}

NAMES = "|".join(sorted(DRAW_STATE, key=len, reverse=True))
NAME = r"\b(gl(" + NAMES + r")(?:ARB|EXT|OES|KHR|NV|AMD)?(?:_fp)?)\b"
# A call, also through "(name)(...)" or "(*name)(...)".  The argument list
# is captured when it holds no nested parentheses (all the allowlisted ones).
CALL_RE = re.compile(NAME + r"\s*\)?\s*\(([^()]*\))?")
# A bare name in a #define is an alias unless it is an availability test
# (HW_OIT_HAS_BLEND_FUNCI is "glBlendFunci_fp != NULL").
BARE_RE = re.compile(NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
# Outside a #define: the function handed to a pointer.
VALUE_RE = re.compile(
    r"(?:(?<![=!<>])=|&)\s*\(?\s*" + NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
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
        if key in CONTEXT_CAPS:
            used[key] = used.get(key, 0) + 1
            continue
        shown = args if args is not None else "..."
        errors.append(f"{rel}:{line(m.start())}: {full}({shown}) "
                      "-- draw state outside gl_pipeline.c")
    for d in DEFINE_RE.finditer(text):
        for m in BARE_RE.finditer(d.group(0)):
            errors.append(f"{rel}:{line(d.start() + m.start())}: #define aliases "
                          f"{m.group(1)} -- draw state outside gl_pipeline.c")
    for m in VALUE_RE.finditer(text):
        errors.append(f"{rel}:{line(m.start(1))}: {m.group(1)} taken as a value "
                      "-- draw state outside gl_pipeline.c")
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

    for key, (want, _why) in sorted(CONTEXT_CAPS.items()):
        got = used.get(key, 0)
        if got == 0:
            errors.append(f"{key[0]}: allowlist entry gl{key[1]}({key[2]}) matches "
                          "nothing -- delete it from CONTEXT_CAPS")
        elif got != want:
            errors.append(f"{key[0]}: gl{key[1]}({key[2]}) appears {got} times, "
                          f"the allowlist expects {want} -- a context switch is set once")

    if errors:
        sys.exit("FAIL:\n  " + "\n  ".join(errors))
    print(f"PASS no draw-state GL call outside gl_pipeline.c in {files} files, "
          f"{len(CONTEXT_CAPS)} context caps allowlisted (#337)")


if __name__ == "__main__":
    main()
