#!/usr/bin/env python3
"""The WebGL2 fixes from verifying the ES tier in a browser (#112).

Run in headless Chromium, the webgl2 build logged 254 "texParameter: invalid
parameter name", 2 "bindTexture: invalid target" and 2 failed framebuffer
attachments per demo1 load, and every `screenshot` came back black.  Checked:
  - texture swizzle (GL_TEXTURE_SWIZZLE_A), which WebGL2 dropped, is a renderer
    cap false under Emscripten; every swizzle call is gated on it, and where it
    is false opaque textures get their alpha written as 1 instead (they are
    RGBA8 on the ES tier, and index 255 is alpha 0 in d_8to24table);
  - OIT does not create GL_TEXTURE_2D_MULTISAMPLE targets on the ES tier,
    which has none; under MSAA there it reports itself unavailable;
  - on the web, `screenshot` captures the finished frame in SCR_UpdateScreen
    before it is presented, not between frames after the browser cleared it.
scripts/wasm-boot-smoke.sh now fails on any WebGL/GL error line.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []

vid = read("engine/h2shared/gl_vidsdl.c")
if not re.search(r"#ifdef __EMSCRIPTEN__\s*gl_renderer_caps\.texture_swizzle = false;\s*#else\s*gl_renderer_caps\.texture_swizzle = true;", vid):
    errors.append("gl_vidsdl.c: texture_swizzle must be false under Emscripten")
draw = read("engine/h2shared/gl_draw.c")
calls = [m.start() for m in re.finditer(r"glTexParameteri_fp\(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A", draw)]
if len(calls) < 2:
    errors.append("gl_draw.c: expected the swizzle calls in GL_Upload32 and the embedded-mips path")
for c in calls:
    if "if (gl_renderer_caps.texture_swizzle)" not in draw[max(0, c - 80):c]:
        errors.append(f"gl_draw.c: a GL_TEXTURE_SWIZZLE_A call (offset {c}) is not gated on texture_swizzle")
if not re.search(r"if \(!\(glt->flags & TEX_ALPHA\) && !gl_renderer_caps\.texture_swizzle\)\s*\{[^}]*data\[i\] \|= MASK_a;", draw):
    errors.append("gl_draw.c: GL_Upload32 must write opaque alpha where there is no swizzle")
if "!gl_renderer_caps.texture_swizzle) ? MASK_a : 0" not in draw:
    errors.append("gl_draw.c: the embedded-mips path must write opaque alpha where there is no swizzle")

pp = read("engine/h2shared/gl_postprocess.c")
fbo = pp[pp.index("static qboolean OIT_CreateFBO (int width, int height, GLuint depth_stencil_rb, GLuint depth_stencil_tex, int samples)\n{"):]
fbo = fbo[:fbo.index("glGenTextures_fp")]
if not re.search(r"#ifdef USE_GLES\s*/\*.*?\*/\s*if \(samples > 1\)\s*\{[^}]*return false;", fbo, re.S):
    errors.append("OIT_CreateFBO: the ES tier must refuse multisampled OIT targets")

scr = read("engine/h2shared/gl_screen.c")
f = scr[scr.index("static void SCR_ScreenShot_f (void)\n{"):]
f = f[:f.index("\n}\n")]
if not re.search(r"#ifdef __EMSCRIPTEN__\s*q_strlcpy \(scr_shot_ext, ext, sizeof\(scr_shot_ext\)\);\s*scr_shot_quality = quality;\s*scr_shot_pending = true;\s*#else\s*SCR_TakeScreenShot \(ext, quality\);", f):
    errors.append("SCR_ScreenShot_f: the web build must defer the capture, the desktop take it at once")
if not re.search(r"GL_PostProcess_EndFrame \(\);\s*#ifdef __EMSCRIPTEN__\s*/\*.*?\*/\s*if \(scr_shot_pending\)", scr, re.S):
    errors.append("SCR_UpdateScreen: the deferred capture must run after the frame is composed, before it is presented")

smoke = read("scripts/wasm-boot-smoke.sh")
if "WebGL: (INVALID|OUT_OF_MEMORY|CONTEXT_LOST)|GL error GL_" not in smoke:
    errors.append("wasm-boot-smoke.sh must fail on WebGL/GL error lines")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS WebGL2: no swizzle, no multisample OIT on ES, screenshots before present (#112)")
