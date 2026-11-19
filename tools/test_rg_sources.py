#!/usr/bin/env python3
"""The SDL_GPU backend stays inside the desktop GL client (GitHub #340 P0).

engine/h2shared/rg_*.c is the SDL_GPU backend.  It must never reach the
GLES tier, the browser builds (WebGL2 and the WEBSOFT rasterizer) or the
dedicated servers: on WEBSOFT a stray reference is a link error, on GLES it
would pull SDL_GPU into a build that has no GL_DLSYM loader table for
rg_null.c to fill, and on h2ded/hwsv it would drag SDL into a server.

Checked, from the sources (stock python3, lint.yml):

  engine/CMakeLists.txt
    * rg_*.c is named only inside `if(USE_SDL_GPU)`, collected into
      RG_SOURCES, and RG_SOURCES feeds only the client's ALL_SOURCES;
    * USE_SDL_GPU is forced OFF for USE_GLES, EMSCRIPTEN, any build
      without GL_DLSYM and SDL older than 3.4, BEFORE anything tests it;
    * USE_SDL_GPU reaches the compiler only through CLIENT_DEFINITIONS --
      never add_definitions() / a -D flag, which would leak it into h2ded
      and hwsv.
  every other engine C source and header
    * each RG_* / rg_* identifier, R_GPU() and #include "rg_device.h" sits
      inside an `#ifdef USE_SDL_GPU` (or `#if defined(USE_SDL_GPU)`) region,
      comments excluded -- so with the option off, no translation unit names
      a symbol only rg_*.c defines.
  every engine C source
    * no SDL_GL_GetProcAddress load can run under -gpu: outside gl_vidsdl.c's
      own loaders each sits in an `if (!R_GPU())` block, GL_Init returns into
      GL_Init_NullGL before its first load, and the loaders are called from
      GL_Init alone.  -gpu loads no GL library, so a load writes NULL over a
      null-GL stub.
  engine/h2shared/rg_*.c / rg_*.h
    * each refuses to compile without USE_SDL_GPU (#error), so a CMake slip
      that compiled one anyway fails loudly instead of linking half a
      backend.
    * rg_internal.h, the backend's private header, is included by rg_*.c
      alone; every seam sees rg_device.h only (#340 P1c).
  the SDL_GPU seams (#340 P1c) -- gl_texcreate.c, gl_texbind.c,
  gl_pipeline.c, gl_uniforms.c, gl_vbo.c and gl_shader.c's R_Program_Create
  and R_ProgramSamplerLoc / R_ProgramSamplerUnit
    * every GL call in them is one -gpu cannot reach: in the body of an
      `if (!R_GPU ())`, in the else of an `if (R_GPU ())`, after an
      `if (R_GPU ())` that returns (in a block still open at the call), or
      wrapped in gl_pipeline.c's RP_GL(), whose USE_SDL_GPU arm must skip the
      call under R_GPU() and whose other arm must be the bare call.  An
      arm compiled out by `#if 0` does not count, and "returns" means the
      arm's last statement is an unconditional return.  Calls made through
      helpers in other files (GL_Upload, GL_BindBuffer, GL_ForgetBuffer)
      are not followed: an arm has to sit in the seam itself.  Under -gpu an unarmed call lands in the
      null table: the frame draws nothing and gpu_unrouted counts it.  The
      two shutdown functions that make GL calls only on names -gpu never
      creates are listed, with that reason, in UNARMED_OK.
  engine/CMakeLists.txt, decision 6 on #340 (the client embeds the SPIR-V)
    * glslangValidator is required where USE_SDL_GPU is on and nowhere
      else: find_program is never REQUIRED, and the configure error for a
      missing glslang sits inside `if(USE_SDL_GPU)` and names
      -DUSE_SDL_GPU=OFF as the way out;
    * under USE_SDL_GPU only: CompileSpirv.cmake writes ${SPIRV_STAMP},
      depending on ${SHADERS_GEN_H} and ${SHADER_SOURCES} (shaders_gen.h is
      rewritten only when its text changes); EmbedSpirv.cmake alone writes
      ${SPIRV_GEN_C}, depending on the stamp, so `--target shaders_spirv`
      still only compiles; ${SPIRV_GEN_C} reaches the client only through
      that arm's RG_SOURCES;
    * shaders_spirv / shaders_cross are never in ALL, nothing but
      shaders_cross DEPENDS on shaders_spirv, and the targets sharing those
      rules are ordered shaders_gen <- shaders_spirv <- spirv_embed <- the
      client (unordered, make -j runs a shared rule twice at once).
  flake.nix
    * .#nixos and .#win64 (host pkgs, not pkgsCross64) carry glslang in
      nativeBuildInputs; linuxBuildAttrs, gles, h2ded, hwsv, h2ded-win64 and
      wasm do not, and the three server builds configure -DUSE_SDL_GPU=OFF.
  engine/h2shared/rg_shader.c
    * blobs come from -gpushaders <dir> first, then the embedded table, and
      no implicit directory (SDL_GetBasePath, FS_GetBasedir, RG_SPIRV_DIR)
      is searched.
  tools/test_embed_spirv.py checks what EmbedSpirv.cmake writes.
  the 3D world under -gpu (#340 P2b), which tools/sweep-world-gpu.steps
  checks frame by frame; these pin the source it rests on:
    * depth: GL_Init_NullGL turns gl_clipcontrol_able on (SDL_GPU clips to
      [0,1], GL_ZERO_TO_ONE's space) with the GL_GEQUAL test and a 0.0
      clear depth GL_Init gives reversed-Z, and never sets it false;
      rg_pipeline.c clips depth exactly when the draw tests it, and keeps the
      clockwise front face (GL_CCW seen through VK_FLIP_Y and SDL's
      negative-height viewport); rg_frame.c clears to the depth RG_Clear was
      handed, never a literal;
    * the clear seam: glClear, glClearColor, glClearDepth and glClearStencil
      appear only in gl_pipeline.c, apart from gl_vidsdl.c's FBO self-test
      (GL only, it reads pixels back) and the loader table; there each GL
      arm is the bare call -- glClear_fp (mask) after R_ClearBuffers'
      -gpu arm returns, the value setters' RP_GL (glClear*_fp (...)) with
      their own arguments -- and R_ClearBuffers' -gpu arm hands RG_Clear
      the mask and the three values;
    * the immediate-mode fallbacks: R_BuildWorldVBO and R_BuildSkyStencilVBO
      return under R_GPU () before they create a GL object (so world_vao,
      world_ibo and sky_stencil_vao stay 0), and so does R_BuildWorldCull
      before its programs; alias instancing and GL_DrawAliasSkeletal need
      gl_renderer_caps.shader_storage, which GL_Init_NullGL leaves false.

Usage: test_rg_sources.py [--root DIR]
"""
from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
errors = []


def fail(msg):
    errors.append(msg)


def strip_c_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] == '"':
            # keep string literals (an #include "rg_device.h" is one) but do
            # not let a quote inside a comment desync anything
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


GUARD = re.compile(r"#\s*(?:ifdef\s+USE_SDL_GPU\b|if\s+defined\s*\(\s*USE_SDL_GPU\s*\)\s*$)")
SYMBOL = re.compile(r"\b(?:RG_\w+|rg_\w+|R_GPU)\b|\"rg_device\.h\"")


def check_c(path, rel):
    """Every backend reference is under an #ifdef USE_SDL_GPU arm."""
    stack = []  # per open conditional: True if this arm is a USE_SDL_GPU arm
    for ln, line in enumerate(strip_c_comments(path.read_text(errors="replace")).splitlines(), 1):
        s = line.strip()
        d = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b", s)
        if d:
            kw = d.group(1)
            if kw in ("if", "ifdef", "ifndef"):
                stack.append(bool(GUARD.match(s)))
            elif kw in ("elif", "else"):
                if stack:
                    stack[-1] = False   # the other arm of `#ifdef USE_SDL_GPU`
            elif stack:
                stack.pop()
        for m in SYMBOL.finditer(line):
            tok = m.group(0)
            if tok.startswith("R_GPU") and re.match(r"R_GPU\w", line[m.start():m.start() + 6]):
                continue
            if not any(stack):
                fail("%s:%d: `%s` outside #ifdef USE_SDL_GPU -- builds without the SDL_GPU "
                     "backend (GLES, WEBSOFT, dedicated) would not compile or link" % (rel, ln, tok))


def functions(code):
    """[(name, open, close)] for each top-level function body in comment-
    stripped C: the name is the last identifier before the header's '('."""
    out, depth, i, n = [], 0, 0, len(code)
    start = None
    while i < n:
        c = code[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and code[j] != c and code[j] != "\n":
                j += 2 if code[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            if depth == 0:
                head = code[max(0, i - 400):i]
                m = re.search(r"(\w+)\s*\([^;{}]*\)\s*$", head)
                start = (m.group(1) if m else None, i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start:
                out.append((start[0], start[1], i))
                start = None
        i += 1
    return out


# The loaders SDL_GPU mode never reaches: GL_Init returns into GL_Init_NullGL
# before calling either of the first two, and before its own loads.
VIDSDL_LOADERS = ("GL_Init_Functions", "GL_LoadFunctionPointers", "GL_Init")
GPU_SKIP = re.compile(r"if\s*\(\s*!\s*R_GPU\s*\(\s*\)\s*\)\s*(?:#\s*endif\s*)?$")


def check_proc_loads(root):
    """No SDL_GL_GetProcAddress load runs under -gpu.  rg_null.c's stubs fill
    every gl_func.h pointer; a later load finds no GL library and writes NULL
    over one, which crashes on the first call far from the load (a stray
    reload in GL_PostProcess_Init did exactly that, #340 P0)."""
    vid = root / "engine/h2shared/gl_vidsdl.c"
    for p in sorted((root / "engine").rglob("*.c")):
        rel = p.relative_to(root)
        if re.match(r"rg_\w+\.c$", p.name) or ("rust" in rel.parts and "target" in rel.parts):
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        if "SDL_GL_GetProcAddress" not in code:
            continue
        funcs = functions(code)
        for m in re.finditer(r"\bSDL_GL_GetProcAddress\b", code):
            ln = code.count("\n", 0, m.start()) + 1
            fn = next((f for f in funcs if f[1] < m.start() < f[2]), None)
            if p == vid and fn and fn[0] in VIDSDL_LOADERS:
                continue
            # walk outwards through the enclosing blocks for an `if (!R_GPU())`
            ok, pos, depth = False, m.start(), 0
            while pos > (fn[1] if fn else 0):
                pos -= 1
                if code[pos] == "}":
                    depth += 1
                elif code[pos] == "{":
                    if depth:
                        depth -= 1
                    elif GPU_SKIP.search(code[max(0, pos - 200):pos].rstrip()):
                        ok = True
                        break
            if not ok:
                fail("%s:%d: SDL_GL_GetProcAddress outside an `if (!R_GPU())` block -- under -gpu "
                     "there is no GL library, and the NULL it returns overwrites rg_null.c's stub"
                     % (rel, ln))
    # GL_Init has to leave for GL_Init_NullGL before any load, and the two
    # loader functions have to be called from GL_Init alone.
    code = strip_c_comments(vid.read_text())
    funcs = {f[0]: f for f in functions(code)}
    gi = funcs.get("GL_Init")
    if not gi:
        fail("gl_vidsdl.c: no GL_Init")
        return
    body = code[gi[1]:gi[2]]
    arm = re.search(r"if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*\{\s*GL_Init_NullGL\s*\(\s*\)\s*;\s*return\s*;", body)
    first = min([body.find(s) for s in ("SDL_GL_GetProcAddress", "GL_Init_Functions",
                                          "GL_LoadFunctionPointers") if s in body] or [len(body)])
    if not arm or arm.start() > first:
        fail("gl_vidsdl.c: GL_Init does not return into GL_Init_NullGL under R_GPU() before its "
             "first GL load")
    for name in ("GL_Init_Functions", "GL_LoadFunctionPointers"):
        for m in re.finditer(r"\b%s\s*\(" % name, code):
            fn = next((f for f in funcs.values() if f[1] < m.start() < f[2]), None)
            if fn and fn[0] != "GL_Init":
                fail("gl_vidsdl.c:%d: %s called from %s, not GL_Init -- a path SDL_GPU mode "
                     "may reach" % (code.count("\n", 0, m.start()) + 1, name, fn[0]))


def cmake_blocks(text):
    """(line, kind, condition, depth-stack-of-conditions) for each line."""
    stack, out = [], []
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0]
        m = re.match(r"\s*(if|elseif|else|endif)\s*\((.*)\)\s*$", line, re.I)
        if m:
            kw, cond = m.group(1).lower(), " ".join(m.group(2).split())
            if kw == "if":
                stack.append(cond)
            elif kw in ("elseif", "else"):
                if stack:
                    stack[-1] = "NOT(%s)" % stack[-1] if kw == "else" else cond
            elif stack:
                stack.pop()
            out.append((ln, line, list(stack)))
            continue
        out.append((ln, line, list(stack)))
    return out


def message_lines(text):
    """Line numbers spanned by message(...) calls (string arguments only)."""
    out = set()
    for m in re.finditer(r'\bmessage\s*\((?:[^()"]|"(?:[^"\\]|\\.)*")*\)', text):
        first = text.count("\n", 0, m.start()) + 1
        out.update(range(first, first + m.group(0).count("\n") + 1))
    return out


def check_cmake(root):
    path = root / "engine/CMakeLists.txt"
    text = path.read_text()
    rows = cmake_blocks(text)
    first_test = None
    force_line = None
    rg_named = 0
    in_message = message_lines(text)
    for ln, line, stack in rows:
        if re.search(r"\brg_\w+\.c\b", line):
            rg_named += 1
            if not stack or stack[-1] != "USE_SDL_GPU":
                fail("CMakeLists.txt:%d: rg_*.c named outside `if(USE_SDL_GPU)`" % ln)
        if re.search(r"\bRG_SOURCES\b", line) and not re.search(r"set\s*\(\s*RG_SOURCES\b", line):
            if not re.match(r"\s*\$\{RG_SOURCES\}\s*$", line):
                fail("CMakeLists.txt:%d: RG_SOURCES used other than as a line of ALL_SOURCES" % ln)
        # A message() may tell the user to pass -DUSE_SDL_GPU=OFF; that
        # defines nothing.
        if ln not in in_message and re.search(
                r"add_definitions\s*\([^)]*USE_SDL_GPU|-DUSE_SDL_GPU\b|"
                r"add_compile_definitions\s*\([^)]*USE_SDL_GPU", line):
            fail("CMakeLists.txt:%d: USE_SDL_GPU as a directory-wide definition; it must reach "
                 "only the client, through CLIENT_DEFINITIONS" % ln)
        if re.match(r"\s*if\s*\(\s*USE_SDL_GPU\s+AND\s*\(.*USE_GLES.*EMSCRIPTEN.*GL_DLSYM", line):
            force_line = ln
            # rg_device.c calls SDL_GetGPUDeviceProperties, new in SDL 3.4
            if not re.search(r"SDL3_VERSION\s+VERSION_LESS\s+3\.4", line):
                fail("CMakeLists.txt:%d: USE_SDL_GPU is not forced off below SDL 3.4" % ln)
        elif re.search(r"\bif\s*\(\s*USE_SDL_GPU\b", line) and first_test is None:
            first_test = ln
    if not rg_named:
        fail("CMakeLists.txt: no rg_*.c source listed -- the SDL_GPU backend is never built")
    if force_line is None:
        fail("CMakeLists.txt: no `if(USE_SDL_GPU AND (USE_GLES OR EMSCRIPTEN OR NOT \"GL_DLSYM\" "
             "IN_LIST CLIENT_DEFINITIONS))` forcing the option off")
    elif first_test is not None and first_test < force_line:
        fail("CMakeLists.txt:%d: USE_SDL_GPU tested before it is forced off for GLES/web (line %d)"
             % (first_test, force_line))
    else:
        nxt = [l for n, l, _ in rows if n == force_line + 1]
        if not nxt or not re.match(r"\s*set\s*\(\s*USE_SDL_GPU\s+OFF\s*\)", nxt[0]):
            fail("CMakeLists.txt:%d: the forcing `if` must be followed by set(USE_SDL_GPU OFF)"
                 % force_line)
    if not re.search(r"list\s*\(\s*APPEND\s+CLIENT_DEFINITIONS\s+USE_SDL_GPU\s*\)", text):
        fail("CMakeLists.txt: USE_SDL_GPU never appended to CLIENT_DEFINITIONS")
    # RG_SOURCES must be in the client's ALL_SOURCES and no server list
    m = re.search(r"set\s*\(\s*ALL_SOURCES\b(.*?)\)", text, re.S)
    if not m or "${RG_SOURCES}" not in m.group(1):
        fail("CMakeLists.txt: ${RG_SOURCES} missing from ALL_SOURCES")
    for lst in ("SERVER_SOURCES", "HWSV_SOURCES"):
        for mm in re.finditer(r"(?:set|list)\s*\(\s*(?:APPEND\s+)?%s\b(.*?)\)" % lst, text, re.S):
            if "RG_SOURCES" in mm.group(1) or re.search(r"\brg_\w+\.c", mm.group(1)):
                fail("CMakeLists.txt: %s carries the SDL_GPU backend" % lst)


# The seams that carry SDL_GPU arms, and the functions in each the arm rule
# covers (None: every function in the file).
SEAMS = {
    "engine/h2shared/gl_texcreate.c": None,
    "engine/h2shared/gl_texbind.c": None,
    "engine/h2shared/gl_pipeline.c": None,
    "engine/h2shared/gl_uniforms.c": None,
    "engine/h2shared/gl_vbo.c": None,
    # The rest of gl_shader.c is GL program set-up the seam calls into;
    # only the creation entry point and the sampler-unit pair (#340 P2a)
    # are the seam.
    "engine/h2shared/gl_shader.c": {"R_Program_Create", "R_ProgramSamplerLoc",
                                    "R_ProgramSamplerUnit"},
}
# Functions that make GL calls with no arm, and why that is safe under -gpu.
UNARMED_OK = {
    ("engine/h2shared/gl_uniforms.c", "R_Uniforms_Shutdown"):
        (r"ubo_\w+", "every call is under `if (ubo_*)`; R_Uniforms_Init's arm creates no buffer, "
                     "so each is 0"),
    ("engine/h2shared/gl_vbo.c", "GL_VBO_Shutdown"):
        (r"imm_\w+", "every call is under `if (imm_*)`; GL_VBO_Init's arm returns before creating any"),
}
GL_CALL = re.compile(r"\bgl[A-Z]\w*_fp\s*\(")


def skip_ws_pp(s, i):
    """Past whitespace and whole preprocessor lines (#endif between an arm's
    statement and its else, say)."""
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s[i] == "#" and (i == 0 or s.rfind("\n", 0, i) == i - 1 or not s[s.rfind("\n", 0, i) + 1:i].strip()):
            j = s.find("\n", i)
            i = len(s) if j < 0 else j + 1
        else:
            break
    return i


def stmt_end(s, i):
    """End of the statement starting at s[i]: a balanced block, or up to the
    first `;` outside parentheses."""
    i = skip_ws_pp(s, i)
    if i < len(s) and s[i] == "{":
        depth = 0
        for j in range(i, len(s)):
            depth += {"{": 1, "}": -1}.get(s[j], 0)
            if depth == 0:
                return j + 1
        return len(s)
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
        elif s[j] == ";" and depth == 0:
            return j + 1
    return len(s)


def dead_spans(s):
    """[(begin, end)] of text inside `#if 0` (until its #else / #endif)."""
    spans, stack = [], []
    for m in re.finditer(r"^[ \t]*#\s*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)", s, re.M):
        kw, rest = m.group(1), m.group(2).strip()
        if kw in ("if", "ifdef", "ifndef"):
            stack.append([kw == "if" and rest == "0", m.end()])
        elif not stack:
            continue
        elif kw in ("else", "elif"):
            if stack[-1][0]:
                spans.append((stack[-1][1], m.start()))
                stack[-1][0] = False
        else:
            top = stack.pop()
            if top[0]:
                spans.append((top[1], m.start()))
    return spans


def returns_always(s, b, e):
    """The statement s[b:e] ends in an unconditional return."""
    body = s[b:e].strip()
    if body.startswith("{"):
        body = body[1:-1]
        depth, last = 0, 0
        for i, ch in enumerate(body):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    last = i + 1
            elif ch == ";" and depth == 0:
                nxt = body[i + 1:].strip()
                if nxt:
                    last = i + 1
        body = body[last:]
    return re.fullmatch(r"\s*return\b[^;{}]*;\s*", body) is not None


def gpu_arms(s):
    """Each `if (R_GPU ())` / `if (!R_GPU ())`: (start, negated, then, else),
    then/else as (begin, end) spans, else None when there is no else."""
    out = []
    dead = dead_spans(s)
    for m in ARM.finditer(s):
        if any(a <= m.start() < z for a, z in dead):
            continue
        neg = "!" in m.group(0)
        b = skip_ws_pp(s, m.end())
        e = stmt_end(s, b)
        k = skip_ws_pp(s, e)
        els = None
        if re.match(r"else\b", s[k:]):
            eb = skip_ws_pp(s, k + 4)
            els = (eb, stmt_end(s, eb))
        out.append((m.start(), neg, (b, e), els))
    return out


def excluded(s, arms, c):
    """A GL call at s[c] that -gpu cannot reach: in an `if (!R_GPU ())` body,
    in the else of an `if (R_GPU ())`, or after an `if (R_GPU ())` whose body
    returns, with the block that arm sits in still open at the call."""
    for start, neg, (tb, te), els in arms:
        if neg and tb <= c < te:
            return True
        if not neg and els and els[0] <= c < els[1]:
            return True
        if not neg and te <= c and returns_always(s, tb, te):
            depth = 0
            for ch in s[te:c]:
                depth += {"{": 1, "}": -1}.get(ch, 0)
                if depth < 0:
                    break
            else:
                return True
    return False


def in_name_guard(code, pos, name_rx):
    """True if the innermost block around `pos` is the body of `if (name)`,
    name matching name_rx."""
    depth = 0
    while pos > 0:
        pos -= 1
        if code[pos] == "}":
            depth += 1
        elif code[pos] == "{":
            if depth:
                depth -= 1
            else:
                return re.search(r"\bif\s*\(\s*(?:%s)\s*\)\s*$" % name_rx,
                                 code[max(0, pos - 120):pos]) is not None
    return False
ARM = re.compile(r"\bif\s*\(\s*!?\s*R_GPU\s*\(\s*\)\s*\)")


def check_seams(root):
    """Every GL call in a seam has an SDL_GPU arm ahead of it."""
    seen_ok = set()
    nfuncs = 0
    for rel, only in SEAMS.items():
        path = root / rel
        if not path.exists():
            fail("%s: seam file missing" % rel)
            continue
        code = strip_c_comments(path.read_text(errors="replace"))
        if rel != "engine/h2shared/gl_pipeline.c" and re.search(r"#\s*define\s+RP_GL\b", code):
            fail("%s: defines RP_GL -- the skip-under-R_GPU wrapper is gl_pipeline.c's, and only "
                 "its definition is checked" % rel)
        funcs = functions(code)
        names = {f[0] for f in funcs}
        for want in (only or ()):
            if want not in names:
                fail("%s: no %s -- the seam this check covers is gone" % (rel, want))
        for name, start, end in funcs:
            if only is not None and name not in only:
                continue
            body = code[start:end]
            # RP_GL(...) wraps a call in its own arm: blank those calls out,
            # keeping offsets so a line number still lands on the source.
            # Only gl_pipeline.c's RP_GL, whose definition is checked below.
            bare = body
            if rel == "engine/h2shared/gl_pipeline.c":
                bare = re.sub(r"\bRP_GL\s*\((?:[^()]|\([^()]*(?:\([^()]*\))*[^()]*\))*\)",
                              lambda m: re.sub(r"[^\n]", " ", m.group(0)), body)
            calls = [m.start() for m in GL_CALL.finditer(bare)]
            if not calls:
                continue
            nfuncs += 1
            if (rel, name) in UNARMED_OK:
                seen_ok.add((rel, name))
                name_rx, why = UNARMED_OK[(rel, name)]
                guarded = all(in_name_guard (bare, c, name_rx) for c in calls)
                if not guarded:
                    fail("%s: %s makes a GL call outside an `if (%s)` guard -- UNARMED_OK no longer "
                         "holds (%s)" % (rel, name, name_rx, why))
                continue
            arms = gpu_arms(bare)
            for c in calls:
                if not excluded(bare, arms, c):
                    ln = code.count("\n", 0, start + c) + 1
                    fail("%s:%d: %s makes a GL call no SDL_GPU arm excludes -- under -gpu it lands "
                         "in the null table (#340 P1c)" % (rel, ln, name))
                    break
    for key in UNARMED_OK:
        if key not in seen_ok:
            fail("%s: UNARMED_OK names %s, which makes no GL call any more -- drop the entry"
                 % key)
    # RP_GL's SDL_GPU arm must skip the call under R_GPU().
    pipe = strip_c_comments((root / "engine/h2shared/gl_pipeline.c").read_text())
    m = re.search(r"#\s*ifdef\s+USE_SDL_GPU\s*\n\s*#\s*define\s+RP_GL\s*\(\s*call\s*\)([^\n]*)\n"
                  r"\s*#\s*else\s*\n\s*#\s*define\s+RP_GL\s*\(\s*call\s*\)([^\n]*)\n", pipe)
    if not m:
        fail("gl_pipeline.c: no `#ifdef USE_SDL_GPU / #define RP_GL(call) ... / #else / #define "
             "RP_GL(call) ...` pair")
    else:
        if not re.search(r"if\s*\(\s*!\s*R_GPU\s*\(\s*\)\s*\)\s*\{?\s*call\s*;", m.group(1)):
            fail("gl_pipeline.c: RP_GL's USE_SDL_GPU arm does not skip `call` under R_GPU ()")
        if not re.fullmatch(r"\s*(?:do\s*\{\s*call\s*;\s*\}\s*while\s*\(\s*0\s*\)|call)\s*", m.group(2)):
            fail("gl_pipeline.c: RP_GL's other arm must be the bare call, `do { call; } while (0)`")
    return nfuncs


def check_internal_header(root):
    """rg_internal.h is the backend's own; only rg_*.c include it."""
    n = 0
    for p in sorted((root / "engine").rglob("*")):
        if p.suffix not in (".c", ".h") or not p.is_file():
            continue
        rel = p.relative_to(root)
        if "rust" in rel.parts and "target" in rel.parts:
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        if re.search(r'#\s*include\s*"rg_internal\.h"', code):
            if re.match(r"rg_\w+\.c$", p.name):
                n += 1
            else:
                fail("%s: includes rg_internal.h, the SDL_GPU backend's private header -- a seam "
                     "includes rg_device.h" % rel)
    if not n:
        fail("no rg_*.c includes rg_internal.h -- the backend split is gone")


def strip_cmake_comments(text):
    """text with every `# ...` comment blanked, a # inside "..." kept."""
    out, quoted = [], False
    for line in text.splitlines():
        res, i = [], 0
        while i < len(line):
            c = line[i]
            if quoted:
                if c == "\\":
                    res.append(line[i:i + 2])
                    i += 2
                    continue
                if c == '"':
                    quoted = False
            elif c == '"':
                quoted = True
            elif c == "#":
                break
            res.append(c)
            i += 1
        out.append("".join(res))
    return "\n".join(out)


def cmake_statements(text):
    """(first line, command name, argument text, stack at that line) for each
    top-level CMake command call, comments stripped."""
    code = strip_cmake_comments(text)
    stacks = {ln: stack for ln, _, stack in cmake_blocks(text)}
    out = []
    for m in re.finditer(r"\b(\w+)\s*\(((?:[^()\"]|\"[^\"]*\"|\([^()]*\))*)\)", code):
        ln = code.count("\n", 0, m.start()) + 1
        out.append((ln, m.group(1).lower(), m.group(2), stacks.get(ln, [])))
    return out


def check_spirv_build(root):
    """Decision 6: the client embeds the SPIR-V, so glslangValidator is
    required with USE_SDL_GPU and with it alone."""
    text = (root / "engine/CMakeLists.txt").read_text()
    stmts = cmake_statements(text)
    on = lambda stack: "USE_SDL_GPU" in stack
    rule = "#340 decision 6: glslangValidator is required only where USE_SDL_GPU is on"

    fps = [s for s in stmts if s[1] == "find_program" and re.match(r"\s*GLSLANG_VALIDATOR\b", s[2])]
    if not fps:
        fail("CMakeLists.txt: no find_program(GLSLANG_VALIDATOR ...)")
    for ln, _, args, _ in fps:
        if re.search(r"\bREQUIRED\b", args):
            fail("CMakeLists.txt:%d: find_program(GLSLANG_VALIDATOR ... REQUIRED) -- GLES, web and "
                 "server builds would need glslang (%s)" % (ln, rule))

    # Every configure error about a missing glslang sits in the USE_SDL_GPU arm,
    # and there is one there that tells the user the way out.
    code = strip_cmake_comments(text)
    required = False
    for m in re.finditer(r"\bif\s*\(\s*NOT\s+GLSLANG_VALIDATOR\s*\)(.*?)\bendif\s*\(", code, re.S):
        ln = code.count("\n", 0, m.start()) + 1
        stack = [s for l, _, s in cmake_blocks(text) if l == ln][0][:-1]
        if re.search(r"FATAL_ERROR|SEND_ERROR", m.group(1)):
            if not on(stack):
                fail("CMakeLists.txt:%d: a missing glslangValidator fails configure outside "
                     "`if(USE_SDL_GPU)` (%s)" % (ln, rule))
            elif re.search(r"USE_SDL_GPU=OFF", m.group(1)):
                required = True
    if not required:
        fail("CMakeLists.txt: no `if(NOT GLSLANG_VALIDATOR)` FATAL_ERROR under `if(USE_SDL_GPU)` "
             "naming -DUSE_SDL_GPU=OFF -- an SDL_GPU build without glslang would fail late, or "
             "link no shaders")

    # Two custom commands, both under USE_SDL_GPU: CompileSpirv writes the
    # stamp, depending on the shader sources; EmbedSpirv writes spirv_gen.c,
    # depending on the stamp.  Only RG_SOURCES carries spirv_gen.c.
    def p_script(name, args):
        return re.search(r"-P\s+\S*/%s\.cmake" % name, args)
    embeds = [s for s in stmts if p_script("EmbedSpirv", s[2])]
    if not embeds:
        fail("CMakeLists.txt: EmbedSpirv.cmake is never run -- the client would embed no SPIR-V")
    for ln, cmd, args, stack in embeds:
        if not on(stack):
            fail("CMakeLists.txt:%d: EmbedSpirv.cmake runs outside `if(USE_SDL_GPU)` (%s)" % (ln, rule))
        if cmd != "add_custom_command" or not re.search(r"\bOUTPUT\s+\$\{SPIRV_GEN_C\}", args):
            fail("CMakeLists.txt:%d: EmbedSpirv.cmake must run from the add_custom_command whose "
                 "OUTPUT is ${SPIRV_GEN_C}" % ln)
        elif p_script("CompileSpirv", args):
            fail("CMakeLists.txt:%d: the embed command also compiles; then `--target shaders_spirv` "
                 "cannot produce the .spv a stale gl_progid_refl.h is regenerated from" % ln)
        elif not re.search(r"\bDEPENDS\b[^)]*\$\{SPIRV_STAMP\}", args):
            fail("CMakeLists.txt:%d: the embed command does not DEPEND on ${SPIRV_STAMP}, so it "
                 "can embed before (or without) the compile" % ln)
    stamps = [s for s in stmts if s[1] == "add_custom_command" and
              re.search(r"\bOUTPUT\s+\$\{SPIRV_STAMP\}", s[2])]
    if not stamps:
        fail("CMakeLists.txt: no add_custom_command(OUTPUT ${SPIRV_STAMP} ...) compiling the SPIR-V")
    for ln, cmd, args, stack in stamps:
        if not on(stack):
            fail("CMakeLists.txt:%d: the SPIR-V stamp command is outside `if(USE_SDL_GPU)` (%s)"
                 % (ln, rule))
        if not p_script("CompileSpirv", args):
            fail("CMakeLists.txt:%d: the stamp command does not run CompileSpirv.cmake" % ln)
        for dep in ("SHADERS_GEN_H", "SHADER_SOURCES"):
            if not re.search(r"\bDEPENDS\b[^)]*\$\{%s\}" % dep, args):
                fail("CMakeLists.txt:%d: the SPIR-V compile does not DEPEND on ${%s}; EmbedShaders "
                     "rewrites shaders_gen.h only when its text changes, so a variants.txt edit "
                     "would leave stale blobs embedded" % (ln, dep))
    for ln, cmd, args, stack in stmts:
        if "SPIRV_STAMP" in args and not (cmd == "set" and re.match(r"\s*SPIRV_STAMP\b", args)) \
                and not on(stack):
            fail("CMakeLists.txt:%d: ${SPIRV_STAMP} used outside `if(USE_SDL_GPU)` (%s)" % (ln, rule))
    for ln, cmd, args, stack in stmts:
        if "SPIRV_GEN_C" not in args:
            continue
        if cmd == "set" and re.match(r"\s*SPIRV_GEN_C\b", args):
            continue
        if not on(stack):
            fail("CMakeLists.txt:%d: ${SPIRV_GEN_C} used outside `if(USE_SDL_GPU)` (%s)" % (ln, rule))
        if cmd == "set" and re.match(r"\s*RG_SOURCES\b", args):
            continue
        if cmd in ("add_custom_command", "add_custom_target"):
            continue
        fail("CMakeLists.txt:%d: ${SPIRV_GEN_C} reaches %s(); the generated file is compiled "
             "through RG_SOURCES alone" % (ln, cmd))
    rgset = [s for s in stmts if s[1] == "set" and re.match(r"\s*RG_SOURCES\b", s[2]) and on(s[3])]
    if not any("${SPIRV_GEN_C}" in s[2] for s in rgset):
        fail("CMakeLists.txt: ${SPIRV_GEN_C} is not in the USE_SDL_GPU arm's RG_SOURCES -- the "
             "embedded table would not be linked")

    # shaders_spirv: never ALL; with USE_SDL_GPU the client depends on it,
    # without it nothing does.
    for ln, cmd, args, stack in stmts:
        if cmd == "add_custom_target" and re.match(r"\s*shaders_(?:spirv|cross)\b", args):
            if re.search(r"\bALL\b", args):
                fail("CMakeLists.txt:%d: %s is part of ALL -- a build without SDL_GPU would need "
                     "glslang" % (ln, args.split()[0]))
        if cmd in ("add_custom_target", "add_custom_command"):
            name = args.split()[0] if args.split() else ""
            if name != "shaders_cross" and re.search(r"\bDEPENDS\b[^)]*\bshaders_spirv\b", args):
                fail("CMakeLists.txt:%d: `%s(%s ... DEPENDS shaders_spirv)` makes a build step need "
                     "glslangValidator" % (ln, cmd, name))
    # The dependency edges.  A custom command's output listed by several
    # targets is a rule copied into each, and unordered copies run at once
    # under make -j, so every target that shares one is ordered:
    # shaders_gen <- shaders_spirv <- spirv_embed <- the client.
    need = {
        ("shaders_spirv", "shaders_gen"): False,
        ("spirv_embed", "shaders_spirv"): False,
        ("${EXECUTABLE_NAME}", "spirv_embed"): False,
    }
    for ln, cmd, args, stack in stmts:
        if cmd != "add_dependencies":
            continue
        words = tuple(args.split())
        shown = " ".join(words)
        if "shaders_cross" in words:
            fail("CMakeLists.txt:%d: `add_dependencies(%s)` makes a build need shadercross" % (ln, shown))
            continue
        if not ({"shaders_spirv", "spirv_embed"} & set(words)):
            continue
        if words == ("shaders_spirv", "shaders_gen"):
            if on(stack):
                need[words] = True
            continue
        if words not in need:
            fail("CMakeLists.txt:%d: `add_dependencies(%s)`: the only edges on the SPIR-V targets "
                 "are shaders_spirv <- spirv_embed <- the client" % (ln, shown))
        elif not on(stack):
            fail("CMakeLists.txt:%d: `add_dependencies(%s)` outside `if(USE_SDL_GPU)` (%s)"
                 % (ln, shown, rule))
        else:
            need[words] = True
    for (a, b), seen in sorted(need.items()):
        if not seen:
            fail("CMakeLists.txt: no `add_dependencies(%s %s)` under `if(USE_SDL_GPU)` -- unordered, "
                 "make -j can run a shared custom-command rule twice at once" % (a, b))


def nix_attr_block(text, name):
    """The `name = ... ;` binding's text in flake.nix, brace-matched, or None."""
    m = re.search(r"^\s*%s\s*=\s*" % re.escape(name), text, re.M)
    if not m:
        return None
    i, depth, start = m.end(), 0, m.end()
    while i < len(text):
        c = text[i]
        if c == "#":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if text.startswith("''", i):
            j = i + 2
            while True:
                j = text.find("''", j)
                if j < 0 or text[j + 2:j + 3] not in ("$", "'", "\\"):
                    break
                j += 3	# ''$ ''' ''\ are escapes, not the end
            i = len(text) if j < 0 else j + 2
            continue
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        w = re.match(r"[A-Za-z_][\w'-]*", text[i:i + 64]) if (c.isalpha() or c == "_") and \
            (i == 0 or not (text[i - 1].isalnum() or text[i - 1] in "_'-.")) else None
        if w and w.group(0) == "with":
            # `with pkgs;` scopes an expression; its `;` ends nothing.
            j = text.find(";", i)
            i = len(text) if j < 0 else j + 1
            continue
        if w:
            # `let ... in` brackets bindings whose `;` must not end this one.
            depth += {"let": 1, "in": -1}.get(w.group(0), 0)
            i += len(w.group(0))
            continue
        if c in "{[(":
            depth += 1
        elif c in "}])":
            depth -= 1
        elif c == ";" and depth == 0:
            return text[start:i]
        i += 1
    return None


def check_flake_glslang(root):
    """glslang is a native input of the SDL_GPU client builds only; the server
    builds turn the client-only option off so their configure never needs it."""
    text = (root / "flake.nix").read_text()
    # The package bindings; the devShells and apps reuse some of the names.
    pk = text.find("packages = let")
    text = text[pk:] if pk >= 0 else text
    nocomment = lambda s: re.sub(r"(?m)(^|\s)#.*$", r"\1", s or "")
    for name in ("nixos", "win64"):
        b = nix_attr_block(text, name)
        nbi = nocomment(nix_attr_block(b or "", "nativeBuildInputs"))
        if b is None:
            fail("flake.nix: no packages.%s" % name)
        elif not re.search(r"\bglslang\b", nbi):
            fail("flake.nix: .#%s builds with USE_SDL_GPU but has no glslang in nativeBuildInputs "
                 "(#340 decision 6)" % name)
        elif re.search(r"pkgsCross64\.glslang|with\s+pkgsCross64\s*;[^]]*\bglslang\b", nbi):
            fail("flake.nix: .#%s takes glslang from pkgsCross64; the compiler runs on the "
                 "build machine (pkgs.glslang)" % name)
    for name in ("linuxBuildAttrs", "gles", "h2ded", "hwsv", "h2ded-win64", "wasm"):
        b = nix_attr_block(text, name)
        if b is None:
            fail("flake.nix: no %s" % name)
        elif re.search(r"\bglslang\b", nocomment(b)):
            fail("flake.nix: %s pulls in glslang; only the SDL_GPU client builds need it "
                 "(#340 decision 6)" % name)
    for name in ("h2ded", "hwsv", "h2ded-win64"):
        b = nix_attr_block(text, name)
        if b is not None and "-DUSE_SDL_GPU=OFF" not in nocomment(nix_attr_block(b, "cmakeFlags")):
            fail("flake.nix: .#%s does not configure with -DUSE_SDL_GPU=OFF, so its configure "
                 "needs glslang for a client it never builds" % name)


def check_blob_source(root):
    """rg_shader.c: -gpushaders first, then the embedded table, nothing else."""
    path = root / "engine/h2shared/rg_shader.c"
    code = strip_c_comments(path.read_text())
    m = re.search(r"\bstatic\s+void\s+RG_FindShaders\s*\(\s*void\s*\)\s*\{", code)
    if not m:
        fail("rg_shader.c: no RG_FindShaders()")
        return
    body = code[m.end():code.find("\n}", m.end())]
    flag = body.find('COM_CheckParm ("-gpushaders")')
    table = body.find("RG_EmbeddedBlob")
    if flag < 0 or table < 0:
        fail("rg_shader.c: RG_FindShaders must consult -gpushaders and the embedded table")
    elif table < flag:
        fail("rg_shader.c: RG_FindShaders looks at the embedded table before -gpushaders -- the "
             "developer override could never win")
    for bad in ("SDL_GetBasePath", "FS_GetBasedir", "RG_SPIRV_DIR"):
        if re.search(r"\b%s\b" % bad, code):
            fail("rg_shader.c: %s -- an implicit on-disk SPIR-V search could put a stale "
                 "directory ahead of the blobs this binary was built with" % bad)
    m = re.search(r"\bRG_LoadBlob\s*\([^)]*\)\s*\{", code)
    if not m:
        fail("rg_shader.c: no RG_LoadBlob()")
        return
    body = code[m.end():code.find("\n}", m.end())]
    d = re.search(r"\bif\s*\(\s*rg_spv_fromdir\s*\)", body)
    e = body.find("RG_EmbeddedBlob")
    if not d or e < 0 or e < d.start():
        fail("rg_shader.c: RG_LoadBlob must read the -gpushaders directory when one was picked, "
             "and the embedded table otherwise")


# GL clear entry points outside gl_pipeline.c: (file, function name) -> count.
# gl_vidsdl.c's FBO self-test runs on a real context only and puts the clear
# colour back with the same call.  Exempt: gl_func.h and rg_null.c, the loader
# table and its null stubs, and web_canvas_gl2.c, the software renderer's raw
# WebGL2 canvas (test_draw_state_routing.py exempts it the same way).
CLEAR_RAW_OK = {
    ("engine/h2shared/gl_vidsdl.c", "glClearColor_fp"): 2,
    ("engine/h2shared/gl_vidsdl.c", "glClear_fp"): 1,
}
CLEAR_RAW = re.compile(r"\b(glClear(?:Color|Depth|Stencil)?(?:f)?(?:_fp)?)\s*\(")
CLEAR_EXEMPT = {"engine/h2shared/gl_func.h", "engine/h2shared/rg_null.c",
                "engine/h2shared/gl_pipeline.c", "engine/h2shared/web_canvas_gl2.c"}


def body_named(root, rel, name, nth=0):
    code = strip_c_comments((root / rel).read_text(errors="replace"))
    found = [code[b:e] for n, b, e in functions(code) if n == name]
    if len(found) <= nth:
        fail("%s: no %s -- the #340 P2b check that pins it has nothing to read" % (rel, name))
        return None
    return found[nth]


def gpu_return_before(body, first_rx):
    """`#ifdef USE_SDL_GPU / if (R_GPU ()) return;` ahead of the first match of
    first_rx (and that match exists)."""
    g = re.search(r"#\s*ifdef\s+USE_SDL_GPU\s*\n\s*if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*return\s*;", body)
    f = re.search(first_rx, body)
    if g is None or f is None or g.start() > f.start():
        return False
    # at the body's own level, not inside a loop or a branch
    head = body[:g.start()]
    return head.count("{") - head.count("}") == 1


def check_world_imm(root):
    """#340 P2b: reversed-Z under -gpu, the clear seam, and the 3D world on
    the GL_ImmEnd fallbacks."""
    vid = "engine/h2shared/gl_vidsdl.c"
    b = body_named(root, vid, "GL_Init_NullGL")
    if b is not None:
        if [v.strip() for v in re.findall(r"\bgl_clipcontrol_able\s*=\s*([^;=][^;]*);", b)] != ["true"]:
            fail("%s: GL_Init_NullGL must set gl_clipcontrol_able = true and nothing else -- SDL_GPU "
                 "clips to [0,1], so -gpu runs the reversed-Z path (#340 P2b)" % vid)
        if not re.search(r"\bR_SetDepthFunc\s*\(\s*GL_GEQUAL\s*\)\s*;", b):
            fail("%s: GL_Init_NullGL must set the reversed-Z test, R_SetDepthFunc (GL_GEQUAL)" % vid)
        if not re.search(r"\bR_SetClearDepth\s*\(\s*0(?:\.0*)?f?\s*\)\s*;", b):
            fail("%s: GL_Init_NullGL must clear depth to 0, R_SetClearDepth (0.0): reversed-Z's far "
                 "plane (#340 P2b)" % vid)

    pipe = strip_c_comments((root / "engine/h2shared/rg_pipeline.c").read_text())
    if not re.search(r"\benable_depth_clip\s*=\s*k->depth_test\s*\?\s*true\s*:\s*false\s*;", pipe):
        fail("rg_pipeline.c: enable_depth_clip must follow the depth test, `k->depth_test ? true : "
             "false` -- 3D clips at the near and far planes as GL does, 2D clamps (#340 P2b)")
    if len(re.findall(r"\bfront_face\s*=", pipe)) != 1 or \
       not re.search(r"\bfront_face\s*=\s*SDL_GPU_FRONTFACE_CLOCKWISE\s*;", pipe):
        fail("rg_pipeline.c: the front face is SDL_GPU_FRONTFACE_CLOCKWISE, set once -- GL's GL_CCW "
             "through VK_FLIP_Y and SDL's negative-height viewport (#340 P2b)")
    frame = strip_c_comments((root / "engine/h2shared/rg_frame.c").read_text())
    if not re.search(r"\bds\.load_op\s*=\s*\(\s*\(\s*rgf_clear_mask\s*&\s*GL_DEPTH_BUFFER_BIT\s*\)\s*"
                     r"\|\|\s*!\s*rgf_cleared\s*\)\s*\?\s*SDL_GPU_LOADOP_CLEAR\s*:\s*SDL_GPU_LOADOP_LOAD\s*;", frame):
        fail("rg_frame.c: the depth load op must clear when a clear asked for depth or the frame has "
             "not started, `ds.load_op = ((rgf_clear_mask & GL_DEPTH_BUFFER_BIT) || !rgf_cleared) ? "
             "SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;` (#340 P2b)")
    cd = re.findall(r"\bclear_depth\s*=\s*([^;]+);", frame)
    if cd != ["rgf_clear_depth"]:
        fail("rg_frame.c: the render pass must clear depth to what RG_Clear was handed, "
             "`ds.clear_depth = rgf_clear_depth;` alone (found %s) -- a literal 1.0 under "
             "reversed-Z's GL_GEQUAL draws no 3D at all (#340 P2b)" % (cd or "none"))

    # The clear seam's GL arms, call for call what R_Clear and its neighbours made.
    gp = "engine/h2shared/gl_pipeline.c"
    arms = {
        "R_SetClearColor": r"RP_GL\s*\(\s*glClearColor_fp\s*\(\s*r\s*,\s*g\s*,\s*b\s*,\s*a\s*\)\s*\)\s*;",
        "R_SetClearDepth": r"RP_GL\s*\(\s*glClearDepth_fp\s*\(\s*depth\s*\)\s*\)\s*;",
        "R_SetClearStencil": r"RP_GL\s*\(\s*glClearStencil_fp\s*\(\s*s\s*\)\s*\)\s*;",
    }
    for fn, rx in arms.items():
        b = body_named(root, gp, fn)
        if b is not None and (len(re.findall(r"\bgl\w+_fp\s*\(", b)) != 1 or not re.search(rx, b)):
            fail("%s: %s's GL arm must be the one bare call, %s" % (gp, fn, rx.replace("\\s*", "")))
    b = body_named(root, gp, "R_ClearBuffers")
    if b is not None:
        if len(re.findall(r"\bgl\w+_fp\s*\(", b)) != 1 or \
           not re.search(r"\}\s*#\s*endif\s*glClear_fp\s*\(\s*mask\s*\)\s*;\s*$", b):
            fail("%s: R_ClearBuffers' GL arm must be the bare `glClear_fp (mask);` after its -gpu arm" % gp)
        if not re.search(r"RG_Clear\s*\(\s*mask\s*,\s*rp_clear_color\s*,\s*\(float\)\s*rp_clear_depth\s*,"
                         r"\s*rp_clear_stencil\s*\)\s*;\s*return\s*;", b):
            fail("%s: R_ClearBuffers' -gpu arm must hand RG_Clear the mask and the three clear values, "
                 "then return" % gp)
    seen = {}
    for p in sorted((root / "engine").rglob("*")):
        if p.suffix not in (".c", ".h", ".inc") or not p.is_file():
            continue
        rel = str(p.relative_to(root))
        if rel in CLEAR_EXEMPT or ("rust" in p.parts and "target" in p.parts):
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        for m in CLEAR_RAW.finditer(code):
            key = (rel, m.group(1))
            seen[key] = seen.get(key, 0) + 1
            if key not in CLEAR_RAW_OK:
                fail("%s:%d: %s outside gl_pipeline.c -- clear through R_ClearBuffers / "
                     "R_SetClear*, or -gpu never sees it (#340 P2b)"
                     % (rel, code.count("\n", 0, m.start()) + 1, m.group(1)))
    for key, n in CLEAR_RAW_OK.items():
        if seen.get(key, 0) != n:
            fail("%s: CLEAR_RAW_OK expects %d %s, found %d -- update the entry with the code"
                 % (key[0], n, key[1], seen.get(key, 0)))

    # The immediate-mode fallbacks.
    rs = "engine/hexen2/gl_rsurf.c"
    for fn, first in (("R_BuildWorldVBO", r"\bglGen\w*_fp\s*\(|\bmalloc\s*\("),
                      ("R_BuildSkyStencilVBO", r"\bglGen\w*_fp\s*\(|\bmalloc\s*\(")):
        b = body_named(root, rs, fn)
        if b is not None and not gpu_return_before(b, first):
            fail("%s: %s must return under R_GPU () before it creates anything -- under -gpu the "
                 "null table's names would turn on the VBO draw paths, whose glDrawElements nothing "
                 "routes until P2d (#340 P2b)" % (rs, fn))
    b = body_named(root, "engine/h2shared/gl_worldcull.c", "R_BuildWorldCull")
    if b is not None and not gpu_return_before(b, r"\bR_Program_Create\s*\("):
        fail("gl_worldcull.c: R_BuildWorldCull must return under R_GPU () before its programs -- the "
             "null glDispatchCompute is not NULL, and culling is compute (P5) drawn from world_vao")
    rm = "engine/hexen2/gl_rmain.c"
    code = strip_c_comments((root / rm).read_text(errors="replace"))
    ui = re.findall(r"\buse_instancing\s*=\s*([^;]*);", code)
    terms = []
    if len(ui) == 1:
        e = re.sub(r"\s+", " ", ui[0]).strip()
        while e.startswith("(") and e.endswith(")"):
            e = e[1:-1].strip()
        terms = [t.strip() for t in e.split("&&")] if "||" not in e else []
    if "gl_renderer_caps.shader_storage" not in terms:
        fail("%s: use_instancing must require gl_renderer_caps.shader_storage -- instanced alias "
             "models read storage buffers, which -gpu does not have until P2e (#340 P2b)" % rm)
    b = body_named(root, rm, "GL_DrawAliasSkeletal")
    if b is not None:
        g = re.search(r"if\s*\(\s*!\s*gl_renderer_caps\.shader_storage\s*\)\s*\{[^{}]*\breturn\s*;\s*\}", b)
        f = re.search(r"\bGL_GetAliasGPUMesh\s*\(", b)
        if not g or not f or g.start() > f.start():
            fail("%s: GL_DrawAliasSkeletal must return without shader storage before it touches the "
                 "mesh -- its bones are a storage buffer (#340 P2b)" % rm)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    a = ap.parse_args(argv)
    root = a.root.resolve()
    eng = root / "engine"
    check_cmake(root)
    check_proc_loads(root)
    nseam = check_seams(root)
    check_internal_header(root)
    check_spirv_build(root)
    check_flake_glslang(root)
    check_blob_source(root)
    check_world_imm(root)
    nfiles = 0
    rg_files = []
    for p in sorted(eng.rglob("*")):
        if p.suffix not in (".c", ".h") or not p.is_file():
            continue
        rel = p.relative_to(root)
        if "rust" in rel.parts and "target" in rel.parts:
            continue
        if re.match(r"rg_\w+\.[ch]$", p.name):
            rg_files.append(p)
            continue
        nfiles += 1
        check_c(p, rel)
    if not rg_files:
        fail("no engine/**/rg_*.c or rg_*.h found")
    for p in rg_files:
        code = strip_c_comments(p.read_text())
        if not re.search(r"#\s*ifndef\s+USE_SDL_GPU\s*\n\s*#\s*error\b", code):
            fail("%s: no `#ifndef USE_SDL_GPU / #error` guard" % p.relative_to(root))
    for e in errors:
        print("FAIL: " + e)
    if errors:
        print("test_rg_sources: %d failure(s)" % len(errors))
        return 1
    print("test_rg_sources: OK, %d backend files guarded, %d other engine sources reference it only "
          "under USE_SDL_GPU, CMake builds it into the desktop GL client alone; %d seam functions "
          "making GL calls all armed for SDL_GPU" % (len(rg_files), nfiles, nseam))
    return 0


if __name__ == "__main__":
    sys.exit(main())
