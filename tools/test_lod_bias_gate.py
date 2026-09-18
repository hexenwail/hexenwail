#!/usr/bin/env python3
"""Keep GL_TEXTURE_LOD_BIAS behind the renderer capability gate.

GL_TEXTURE_LOD_BIAS (0x8501) is a desktop-GL texture parameter.  OpenGL ES 3.0
and WebGL2 reject it with GL_INVALID_ENUM, so an unguarded glTexParameter call
turns every mipmapped upload into an end-of-frame GL error on the GLES/WebGL2
tier (GitHub #149).  The engine routes the parameter through one helper,
GL_ApplyLodBias in gl_draw.c, which returns early unless
gl_renderer_caps.texture_lod_bias is set.  This source check fails if:

  - GL_TEXTURE_LOD_BIAS is passed to any GL call outside that helper,
  - the helper stops checking the capability before its GL call, or
  - the capability is no longer false on the ES tier and true on desktop.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "engine"
DRAW = ENGINE / "h2shared/gl_draw.c"
VID = ENGINE / "h2shared/gl_vidsdl.c"
HELPER_SIGNATURE = "static void GL_ApplyLodBias (void)\n{"
CAP = "gl_renderer_caps.texture_lod_bias"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return start, end


def strip_comments(text):
    # Replace comments with same-length whitespace so offsets stay valid.
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    failures = []

    draw = strip_comments(DRAW.read_text())
    try:
        helper_start, helper_end = function(draw, HELPER_SIGNATURE)
    except ValueError:
        raise AssertionError(f"missing {HELPER_SIGNATURE.splitlines()[0]} in {DRAW}")
    helper = draw[helper_start:helper_end]

    # Any use of the enum as an argument, i.e. not the #ifndef/#define lines.
    use = re.compile(r"\bGL_TEXTURE_LOD_BIAS\b")
    for path in sorted(ENGINE.rglob("*.[ch]")):
        text = strip_comments(path.read_text(errors="replace"))
        for match in use.finditer(text):
            line_start = text.rfind("\n", 0, match.start()) + 1
            line_end = text.find("\n", match.end())
            line = text[line_start:line_end].strip()
            if re.match(r"#\s*(ifndef|ifdef|define|endif)\b", line):
                continue
            if path == DRAW and helper_start <= match.start() < helper_end:
                continue
            lineno = text.count("\n", 0, match.start()) + 1
            failures.append(f"{path.relative_to(ROOT)}:{lineno}: {line}")

    require(not failures,
            "GL_TEXTURE_LOD_BIAS used outside GL_ApplyLodBias "
            "(ES 3.0/WebGL2 raise GL_INVALID_ENUM, route it through the helper):\n  "
            + "\n  ".join(failures))

    gate = re.search(r"if\s*\(\s*!\s*" + re.escape(CAP) + r"\s*\)\s*return\s*;", helper)
    call = use.search(helper)
    require(gate is not None, f"GL_ApplyLodBias must return early when !{CAP}")
    require(call is not None and gate.start() < call.start(),
            "GL_ApplyLodBias must check the capability before setting the parameter")

    vid = strip_comments(VID.read_text())
    caps_start, caps_end = function(vid, "static void GL_InitRendererCaps (void)\n{")
    caps = vid[caps_start:caps_end]
    # The ES arm (with its own nested __EMSCRIPTEN__ #else) runs from the GLES3
    # profile assignment to the desktop one; the desktop arm follows it.
    es_at = caps.find("profile = GL_RENDERER_GLES3")
    desktop_at = caps.find("profile = GL_RENDERER_DESKTOP_43")
    require(0 <= es_at < desktop_at, "GL_InitRendererCaps lost its ES/desktop split")
    enabled = re.compile(re.escape(CAP) + r"\s*=\s*true\s*;")
    require(enabled.search(caps[es_at:desktop_at]) is None,
            f"{CAP} must not be enabled on the ES/WebGL2 tier")
    require(enabled.search(caps[desktop_at:]) is not None,
            f"{CAP} must be enabled on the desktop GL tier")

    print("PASS: GL_TEXTURE_LOD_BIAS only set through the capability-gated helper")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        sys.exit(1)
